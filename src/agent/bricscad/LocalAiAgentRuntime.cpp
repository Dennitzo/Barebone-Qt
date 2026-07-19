#include "LocalAiAgentRuntime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace {

QString normalizedBaseUrl(QString value, const QString& provider)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return provider.compare(QStringLiteral("official"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("https://api.openai.com/v1")
            : QStringLiteral("http://127.0.0.1:1234/v1");
    }
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString normalizedReasoningEffort(QString effort)
{
    effort = effort.trimmed().toLower();
    if (effort == QStringLiteral("none") || effort == QStringLiteral("low")
        || effort == QStringLiteral("medium") || effort == QStringLiteral("high")) {
        return effort;
    }
    return QStringLiteral("high");
}

enum class ModelFamily {
    GptOss,
    Gemma4,
    Generic,
};

ModelFamily modelFamily(const QString& model)
{
    if (model.contains(QStringLiteral("gpt-oss"), Qt::CaseInsensitive)) {
        return ModelFamily::GptOss;
    }
    if (model.contains(QStringLiteral("gemma-4"), Qt::CaseInsensitive)) {
        return ModelFamily::Gemma4;
    }
    return ModelFamily::Generic;
}

bool useResponsesApiForModel(const QString& model)
{
    return modelFamily(model) == ModelFamily::GptOss;
}

} // namespace

LocalAiAgentRuntime::LocalAiAgentRuntime(ConfigManager& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_scheduler(new LocalAiJobScheduler(this))
{
    QObject::connect(m_scheduler, &LocalAiJobScheduler::statusChanged,
        this, &LocalAiAgentRuntime::statusChanged);
    QObject::connect(m_scheduler, &LocalAiJobScheduler::jobStarted,
        this, &LocalAiAgentRuntime::jobStarted);
    QObject::connect(m_scheduler, &LocalAiJobScheduler::jobFinished,
        this, &LocalAiAgentRuntime::jobFinished);
}

QString LocalAiAgentRuntime::provider() const
{
    return m_config.aiProvider();
}

QString LocalAiAgentRuntime::model() const
{
    const QString configured = m_config.aiModel().trimmed();
    if (!configured.isEmpty()) {
        return configured;
    }
    return officialProvider()
        ? QStringLiteral("gpt-5.5")
        : QStringLiteral("openai/gpt-oss-20b");
}

QString LocalAiAgentRuntime::baseUrl() const
{
    return normalizedBaseUrl(m_config.aiBaseUrl(), provider());
}

bool LocalAiAgentRuntime::officialProvider() const
{
    return provider().compare(QStringLiteral("official"), Qt::CaseInsensitive) == 0;
}

bool LocalAiAgentRuntime::useResponsesApi(ResponseKind responseKind, const QString& modelName) const
{
    return officialProvider()
        || (responseKind == ResponseKind::VisibleMarkdown && useResponsesApiForModel(modelName));
}

QString LocalAiAgentRuntime::reasoningEffortForModel(const QString& modelName, const QString& configuredEffort) const
{
    const QString normalized = normalizedReasoningEffort(configuredEffort);
    if (modelFamily(modelName) == ModelFamily::GptOss && normalized == QStringLiteral("none")) {
        return QStringLiteral("low");
    }
    return normalized;
}

QJsonObject LocalAiAgentRuntime::structuredResponseFormat(const QString& name, const QStringList& requiredFields) const
{
    QJsonObject properties;
    auto propertySchema = [](const QString& field) {
        static const QSet<QString> arrayFields{
            QStringLiteral("relevantMessageIndexes"),
            QStringLiteral("omittedTopics"),
            QStringLiteral("toolNames"),
            QStringLiteral("workflowIds"),
            QStringLiteral("relevantObjects"),
            QStringLiteral("relevantLayers"),
            QStringLiteral("measurements"),
            QStringLiteral("uncertainties"),
            QStringLiteral("values"),
            QStringLiteral("steps"),
            QStringLiteral("missing"),
        };
        static const QSet<QString> booleanFields{
            QStringLiteral("ready"),
            QStringLiteral("readyForExecution"),
        };
        static const QSet<QString> numberFields{
            QStringLiteral("confidence"),
        };
        if (arrayFields.contains(field)) {
            return QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), QJsonObject{}},
            };
        }
        if (booleanFields.contains(field)) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
        }
        if (numberFields.contains(field)) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
        }
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    };

    QJsonArray required;
    QStringList fields = requiredFields;
    fields.removeDuplicates();
    for (const QString& field : std::as_const(fields)) {
        properties.insert(field, propertySchema(field));
        required.append(field);
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("json_schema")},
        {QStringLiteral("json_schema"), QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("strict"), false},
            {QStringLiteral("schema"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), properties},
                {QStringLiteral("required"), required},
                {QStringLiteral("additionalProperties"), true},
            }},
        }},
    };
}

QString LocalAiAgentRuntime::chatCompletionContent(const QJsonObject& response) const
{
    const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
        const QString content = message.value(QStringLiteral("content")).toString().trimmed();
        if (!content.isEmpty()) {
            return content;
        }
    }
    const QString outputText = response.value(QStringLiteral("output_text")).toString().trimmed();
    if (!outputText.isEmpty()) {
        return outputText;
    }
    QStringList parts;
    for (const QJsonValue& outputValue : response.value(QStringLiteral("output")).toArray()) {
        const QJsonObject output = outputValue.toObject();
        for (const QJsonValue& contentValue : output.value(QStringLiteral("content")).toArray()) {
            const QJsonObject content = contentValue.toObject();
            const QString text = content.value(QStringLiteral("text")).toString(
                content.value(QStringLiteral("output_text")).toString());
            if (!text.trimmed().isEmpty()) {
                parts << text.trimmed();
            }
        }
    }
    return parts.join(QStringLiteral("\n\n")).trimmed();
}

QJsonObject LocalAiAgentRuntime::objectFromModelContent(const QString& content, bool* ok) const
{
    QString text = content.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        text.remove(QRegularExpression(QStringLiteral("^```[a-zA-Z0-9_-]*\\s*")));
        text.remove(QRegularExpression(QStringLiteral("\\s*```$")));
        text = text.trimmed();
    }
    const int first = text.indexOf(QLatin1Char('{'));
    const int last = text.lastIndexOf(QLatin1Char('}'));
    if (first >= 0 && last > first) {
        text = text.mid(first, last - first + 1);
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    const bool parsed = parseError.error == QJsonParseError::NoError && document.isObject();
    if (ok) {
        *ok = parsed;
    }
    return parsed ? document.object() : QJsonObject{};
}

QString LocalAiAgentRuntime::enqueuePost(
    QNetworkRequest request,
    const QJsonObject& payload,
    const QString& kind,
    int priority,
    bool background,
    bool cancellable,
    const QString& dedupeKey,
    int operationGeneration,
    bool bricsCadMode,
    std::function<void(const LocalAiJobScheduler::Result&)> callback)
{
    if (!m_scheduler) {
        return {};
    }
    m_scheduler->setMaxConcurrentJobs(
        provider() == QStringLiteral("local") && bricsCadMode ? 4 : 1);

    LocalAiJobScheduler::Job job;
    job.kind = kind;
    job.priority = priority;
    job.background = background;
    job.cancellable = cancellable;
    job.dedupeKey = dedupeKey;
    job.request = request;
    job.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    job.operationGeneration = operationGeneration;
    job.callback = std::move(callback);
    return m_scheduler->enqueue(std::move(job));
}

void LocalAiAgentRuntime::submitJson(const JsonRequest& jsonRequest, std::function<void(const JsonResult&)> callback)
{
    const QString modelName = model();
    const bool responsesApi = useResponsesApi(ResponseKind::StructuredJson, modelName);
    const QUrl url(baseUrl() + (responsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));

    const auto completion = QSharedPointer<std::function<void(const JsonResult&)>>::create(std::move(callback));
    auto deliver = [completion](JsonResult result) {
        if (completion && *completion) {
            (*completion)(result);
        }
    };
    auto fail = [deliver](const QString& error) mutable {
        JsonResult result;
        result.ok = false;
        result.error = error;
        deliver(result);
    };

    if (!url.isValid() || url.scheme().isEmpty()) {
        fail(QStringLiteral("AI Server URL ist ungueltig."));
        return;
    }
    if (officialProvider() && m_config.aiApiKey().trimmed().isEmpty()) {
        fail(QStringLiteral("API Key fehlt."));
        return;
    }

    QJsonArray messages{
        QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("content"), jsonRequest.systemInstruction},
        },
        QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(jsonRequest.input).toJson(QJsonDocument::Compact))},
        },
    };

    QJsonObject payload{{QStringLiteral("model"), modelName}};
    if (responsesApi) {
        payload.insert(QStringLiteral("input"), messages);
        payload.insert(QStringLiteral("max_output_tokens"), jsonRequest.maxOutputTokens);
        const QString effort = reasoningEffortForModel(modelName, QStringLiteral("low"));
        if (effort != QStringLiteral("none")) {
            payload.insert(QStringLiteral("reasoning"), QJsonObject{{QStringLiteral("effort"), effort}});
        }
        if (!officialProvider()) {
            payload.insert(QStringLiteral("temperature"), 0.0);
        }
    } else {
        payload.insert(QStringLiteral("messages"), messages);
        payload.insert(QStringLiteral("temperature"), 0.0);
        payload.insert(QStringLiteral("max_tokens"), jsonRequest.maxOutputTokens);
        const ModelFamily family = modelFamily(modelName);
        if (family == ModelFamily::Gemma4 || family == ModelFamily::GptOss) {
            payload.insert(QStringLiteral("reasoning_effort"), reasoningEffortForModel(modelName, QStringLiteral("low")));
        }
        if (!officialProvider()) {
            payload.insert(QStringLiteral("response_format"),
                structuredResponseFormat(QStringLiteral("barebone_parallel_%1").arg(jsonRequest.slot),
                    jsonRequest.requiredFields));
        }
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (officialProvider()) {
        request.setRawHeader("Authorization",
            QStringLiteral("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(jsonRequest.timeoutMs);

    const QString jobId = enqueuePost(
        request,
        payload,
        jsonRequest.kind.isEmpty() ? QStringLiteral("BricsCadPreparation-%1").arg(jsonRequest.slot) : jsonRequest.kind,
        jsonRequest.priority,
        jsonRequest.background,
        jsonRequest.cancellable,
        jsonRequest.dedupeKey,
        jsonRequest.operationGeneration,
        jsonRequest.bricsCadMode,
        [this, deliver](const LocalAiJobScheduler::Result& result) mutable {
            JsonResult out;
            out.rawBody = result.body;
            if (result.networkError != QNetworkReply::NoError) {
                out.ok = false;
                out.error = result.errorString;
                deliver(out);
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument responseDocument = QJsonDocument::fromJson(result.body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
                out.ok = false;
                out.error = QStringLiteral("OpenAI-Antwort ist kein gueltiges JSON.");
                deliver(out);
                return;
            }
            out.rawContent = chatCompletionContent(responseDocument.object()).trimmed();
            bool parsed = false;
            out.object = objectFromModelContent(out.rawContent, &parsed);
            out.ok = parsed;
            if (!parsed) {
                out.error = QStringLiteral("Slot-Antwort konnte nicht als JSON gelesen werden.");
            }
            deliver(out);
        });
    if (jobId.trimmed().isEmpty()) {
        fail(QStringLiteral("AI Job konnte nicht gestartet werden."));
    }
}

void LocalAiAgentRuntime::abortAll()
{
    if (m_scheduler) {
        m_scheduler->abortAll();
    }
}

QJsonObject LocalAiAgentRuntime::status() const
{
    return m_scheduler ? m_scheduler->status() : QJsonObject{};
}
