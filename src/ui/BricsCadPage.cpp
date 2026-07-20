#include "BricsCadPage.h"

#include "../agent/BrxAgent.h"
#include "../agent/bricscad/BricsCadAgentUtils.h"
#include "../agent/bricscad/ToolWorkflowAgent.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QUuid>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QPalette>
#include <QPageLayout>
#include <QPageSize>
#include <QSizePolicy>
#include <QSet>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QUrl>
#include <QVector>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace {

bool isSystemRouteWorkflowId(const QString& id)
{
    return id == QStringLiteral("workflow_04_drinking_water_route")
        || id == QStringLiteral("workflow_05_heating_route")
        || id == QStringLiteral("workflow_06_ventilation_route")
        || id == QStringLiteral("workflow_05_system_pipe_contours");
}

template <typename Callback>
void emitToWebAsync(AiWebBridge* bridge, Callback callback)
{
    if (!bridge) {
        return;
    }

    QPointer<AiWebBridge> guard(bridge);
    QTimer::singleShot(0, bridge, [guard, callback = std::move(callback)]() mutable {
        if (guard) {
            callback(guard);
        }
    });
}

void emitWebMessage(AiWebBridge* bridge, QVariantMap message)
{
    emitToWebAsync(bridge, [message = std::move(message)](AiWebBridge* target) {
        Q_EMIT target->messageAdded(message);
    });
}

void emitWebStatus(AiWebBridge* bridge, QString status)
{
    emitToWebAsync(bridge, [status = std::move(status)](AiWebBridge* target) {
        Q_EMIT target->statusChanged(status);
    });
}

QString effectiveWebTheme(QString theme)
{
    const QString normalized = theme.trimmed().toLower();
    if (normalized == QStringLiteral("light") || normalized == QStringLiteral("dark")) {
        return normalized;
    }
    return QApplication::palette().color(QPalette::Window).lightness() < 128
        ? QStringLiteral("dark")
        : QStringLiteral("light");
}

QString effectiveUiLanguage(QString language)
{
    const QString normalized = language.trimmed().toLower();
    return normalized == QStringLiteral("en") ? QStringLiteral("en") : QStringLiteral("de");
}

bool englishUiLanguage(const ConfigManager& config)
{
    return effectiveUiLanguage(config.language()) == QStringLiteral("en");
}

QString aiLanguageInstruction(const ConfigManager& config)
{
    return englishUiLanguage(config)
        ? QStringLiteral("Answer in English unless the user explicitly asks for another language.")
        : QStringLiteral("Antworte auf Deutsch, sofern der Nutzer keine andere Sprache wuenscht.");
}

bool isGenericPreparedActionText(QString text)
{
    text = text.trimmed().toLower();
    return text.isEmpty()
        || text == QStringLiteral("der agent hat eine bricscad-aktion vorbereitet.")
        || text == QStringLiteral("der agent hat eine bricscad-aktion vorbereitet")
        || text == QStringLiteral("der agent hat eine cad-aktion vorbereitet.")
        || text == QStringLiteral("der agent hat eine cad-aktion vorbereitet")
        || text == QStringLiteral("bricscad-aktion vorbereitet.")
        || text == QStringLiteral("bricscad-aktion vorbereitet")
        || text == QStringLiteral("ai vorschlag bereit")
        || text == QStringLiteral("ai batch-vorschlag bereit");
}

QString jsStringLiteral(const QString& value)
{
    QJsonArray array;
    array.append(value);
    QString json = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    if (json.size() >= 2 && json.startsWith(QLatin1Char('[')) && json.endsWith(QLatin1Char(']'))) {
        json = json.mid(1, json.size() - 2);
    }
    return json;
}

void emitWebProposal(AiWebBridge* bridge, QVariantMap proposal)
{
    emitToWebAsync(bridge, [proposal = std::move(proposal)](AiWebBridge* target) {
        Q_EMIT target->proposalChanged(proposal);
    });
}

void emitWebProposalCleared(AiWebBridge* bridge)
{
    emitToWebAsync(bridge, [](AiWebBridge* target) {
        Q_EMIT target->proposalCleared();
    });
}

void emitWebBridgeLog(AiWebBridge* bridge, QString line)
{
    emitToWebAsync(bridge, [line = std::move(line)](AiWebBridge* target) {
        Q_EMIT target->bridgeLogAdded(line);
    });
}

void emitWebReasoningProgress(AiWebBridge* bridge, QVariantMap progress)
{
    emitToWebAsync(bridge, [progress = std::move(progress)](AiWebBridge* target) {
        Q_EMIT target->agentReasoningProgressChanged(progress);
    });
}




constexpr const char* kBrxSdkRoot = "C:/Program Files/Bricsys/BRXSDK/BRX26.1.05.0";
constexpr const char* kBrxPluginName = "BareboneBrx.brx";
constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47626;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";
constexpr bool kAgentActionToolsEnabled = true;
constexpr int kMaxAgentValidationRetries = 20;
constexpr int kMaxAgentRepairRetries = 3;
constexpr int kMaxAgentBatchActions = 50;
constexpr int kAgentBatchActionDelayMs = 1000;
constexpr int kAiModelResponseTimeoutMs = 10 * 60 * 1000;
constexpr int kFocusedContextAiTimeoutMs = 90 * 1000;
constexpr int kLocalAiUnreachablePollIntervalMs = 15 * 1000;
constexpr int kLocalAiReachablePollIntervalMs = 60 * 1000;
constexpr int kWorkflowTrainingAiTimeoutMs = kAiModelResponseTimeoutMs;
constexpr int kWorkflowTrainingOutputTokens = 16384;
constexpr int kWorkflowTrainingCompactOutputTokens = 8192;
constexpr qsizetype kMaxDocumentContextChars = 90000;

QString brxValidationToolName(const QString& tool)
{
    if (tool == QStringLiteral("brx.sdk.entity.transformBy")
        || tool == QStringLiteral("brx.sdk.blockReference.setPosition")) {
        return QStringLiteral("geometry.move");
    }
    if (tool == QStringLiteral("brx.sdk.entity.copy")) {
        return QStringLiteral("geometry.copy");
    }
    if (tool == QStringLiteral("brx.sdk.entity.rotateBy")) {
        return QStringLiteral("geometry.rotate");
    }
    if (tool == QStringLiteral("brx.sdk.entity.scaleBy")) {
        return QStringLiteral("geometry.scale");
    }
    if (tool == QStringLiteral("brx.sdk.entity.erase")) {
        return QStringLiteral("geometry.delete");
    }
    if (tool == QStringLiteral("brx.sdk.entity.setLayer")) {
        return QStringLiteral("entity.setLayer");
    }
    if (tool == QStringLiteral("brx.sdk.entity.setName")) {
        return QStringLiteral("entity.setName");
    }
    if (tool == QStringLiteral("brx.sdk.selection.setPickfirst")) {
        return QStringLiteral("selection.set");
    }
    if (tool == QStringLiteral("brx.sdk.bim.classification.set")) {
        return QStringLiteral("bim.classify");
    }
    return tool;
}

QJsonObject normalizedWorkflowStepForExecution(QJsonObject step)
{
    return step;
}

enum class LocalModelFamily {
    GptOss,
    Gemma4,
    Generic,
};

enum class AgentResponseKind {
    VisibleMarkdown,
    StructuredJson,
};

struct ParsedModelOutput {
    QString finalText;
    QString reasoning;
    QJsonObject structuredValue;
    bool truncated = false;
};

LocalModelFamily localModelFamily(const QString& model)
{
    if (model.contains(QStringLiteral("gpt-oss"), Qt::CaseInsensitive)) {
        return LocalModelFamily::GptOss;
    }
    if (model.contains(QStringLiteral("gemma-4"), Qt::CaseInsensitive)) {
        return LocalModelFamily::Gemma4;
    }
    return LocalModelFamily::Generic;
}

bool useResponsesApiForModel(const QString& model)
{
    return localModelFamily(model) == LocalModelFamily::GptOss;
}

QString normalizedReasoningEffort(QString effort);

bool isGemma4ReasoningModel(const QString& model)
{
    return localModelFamily(model) == LocalModelFamily::Gemma4;
}

QString reasoningEffortForModel(const QString& model, const QString& configuredEffort)
{
    const QString normalized = normalizedReasoningEffort(configuredEffort);
    if (localModelFamily(model) == LocalModelFamily::GptOss
        && normalized == QStringLiteral("none")) {
        // gpt-oss exposes low as its minimum reasoning level in LM Studio.
        return QStringLiteral("low");
    }
    return normalized;
}

void insertChatReasoningForModel(QJsonObject& payload, const QString& model, const QString& configuredEffort)
{
    const LocalModelFamily family = localModelFamily(model);
    if (family == LocalModelFamily::Gemma4 || family == LocalModelFamily::GptOss) {
        payload.insert(QStringLiteral("reasoning_effort"),
            reasoningEffortForModel(model, configuredEffort));
    }
}

bool useResponsesApiForProvider(
    const QString& provider,
    const QString& model,
    AgentResponseKind responseKind = AgentResponseKind::VisibleMarkdown)
{
    return provider.compare(QStringLiteral("official"), Qt::CaseInsensitive) == 0
        || (responseKind == AgentResponseKind::VisibleMarkdown && useResponsesApiForModel(model));
}

QJsonObject localStructuredResponseFormat(
    const QString& name,
    const QStringList& requiredFields = {})
{
    QJsonObject properties;
    auto propertySchema = [](const QString& field) {
        static const QSet<QString> arrayFields{
            QStringLiteral("tools"),
            QStringLiteral("lessons"),
            QStringLiteral("relevantMessageIndexes"),
            QStringLiteral("omittedTopics"),
            QStringLiteral("blocks"),
            QStringLiteral("tables"),
            QStringLiteral("inputs"),
            QStringLiteral("formulas"),
            QStringLiteral("examples"),
            QStringLiteral("assumptions"),
            QStringLiteral("warnings"),
            QStringLiteral("sourceRefs"),
            QStringLiteral("tags"),
            QStringLiteral("toolNames"),
            QStringLiteral("workflowIds"),
            QStringLiteral("relevantObjects"),
            QStringLiteral("relevantLayers"),
            QStringLiteral("measurements"),
            QStringLiteral("uncertainties"),
        };
        static const QSet<QString> booleanFields{
            QStringLiteral("needsWorkflow"),
            QStringLiteral("needsCadContext"),
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
    QStringList fields = requiredFields;
    if (name == QStringLiteral("barebone_router")) {
        fields << QStringLiteral("schema") << QStringLiteral("route")
               << QStringLiteral("capabilityProfile") << QStringLiteral("reason");
    } else if (name == QStringLiteral("barebone_tool_selection")) {
        fields << QStringLiteral("schema") << QStringLiteral("tools")
               << QStringLiteral("lessons") << QStringLiteral("needsWorkflow")
               << QStringLiteral("needsCadContext") << QStringLiteral("confidence")
               << QStringLiteral("reason");
    } else if (name == QStringLiteral("barebone_context_focus")) {
        fields << QStringLiteral("schema") << QStringLiteral("topic")
               << QStringLiteral("relevantSummary") << QStringLiteral("relevantMessageIndexes")
               << QStringLiteral("omittedTopics") << QStringLiteral("confidence");
    } else if (name == QStringLiteral("barebone_agent_response")
        || name == QStringLiteral("barebone_workflow_training")) {
        fields << QStringLiteral("schema") << QStringLiteral("type");
    } else if (name == QStringLiteral("barebone_general_workflow")) {
        fields << QStringLiteral("schema") << QStringLiteral("id")
               << QStringLiteral("title") << QStringLiteral("description")
               << QStringLiteral("blocks") << QStringLiteral("tables")
               << QStringLiteral("inputs") << QStringLiteral("formulas")
               << QStringLiteral("examples") << QStringLiteral("assumptions")
               << QStringLiteral("warnings") << QStringLiteral("sourceRefs")
               << QStringLiteral("verificationStatus") << QStringLiteral("tags");
    }
    fields.removeDuplicates();

    QJsonArray required;
    for (const QString& field : std::as_const(fields)) {
        properties.insert(field, propertySchema(field));
    }
    for (const QString& field : requiredFields) {
        required.append(field);
    }
    return QJsonObject{
        {"type", "json_schema"},
        {"json_schema", QJsonObject{
            {"name", name},
            {"strict", false},
            {"schema", QJsonObject{
                {"type", "object"},
                {"properties", properties},
                {"required", required},
                {"additionalProperties", true},
            }},
        }},
    };
}

QString indentPlainContext(QString text, int spaces)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QString prefix(std::max(0, spaces), QLatin1Char(' '));
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString& line : lines) {
        line.prepend(prefix);
    }
    return lines.join(QLatin1Char('\n'));
}

QString semanticJsonContextText(const QJsonValue& value, int depth = 0)
{
    if (depth > 12 || value.isUndefined() || value.isNull()) {
        return {};
    }
    if (value.isString()) {
        return value.toString();
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 15);
    }
    if (value.isArray()) {
        QStringList lines;
        for (const QJsonValue& item : value.toArray()) {
            const QString itemText = semanticJsonContextText(item, depth + 1);
            if (itemText.trimmed().isEmpty()) {
                continue;
            }
            if (item.isObject() || item.isArray() || itemText.contains(QLatin1Char('\n'))) {
                lines.append(QStringLiteral("-\n%1").arg(indentPlainContext(itemText, 2)));
            } else {
                lines.append(QStringLiteral("- %1").arg(itemText));
            }
        }
        return lines.join(QLatin1Char('\n'));
    }

    QStringList lines;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString itemText = semanticJsonContextText(it.value(), depth + 1);
        if (itemText.trimmed().isEmpty()) {
            continue;
        }
        if (it.value().isObject() || it.value().isArray() || itemText.contains(QLatin1Char('\n'))) {
            lines.append(QStringLiteral("%1:\n%2").arg(it.key(), indentPlainContext(itemText, 2)));
        } else {
            lines.append(QStringLiteral("%1: %2").arg(it.key(), itemText));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString normalizedReasoningEffort(QString effort)
{
    effort = effort.trimmed().toLower();
    if (effort == "none" || effort == "low" || effort == "medium" || effort == "high") {
        return effort;
    }
    return QStringLiteral("high");
}

QString repairMojibakeText(QString text);

QString normalizedBricsCadLayerName(QString name)
{
    name = repairMojibakeText(name).trimmed();

    QString normalized;
    normalized.reserve(name.size());
    for (const QChar ch : name) {
        if (ch == QChar(0x00DF)) {
            normalized += QStringLiteral("ss");
            continue;
        }

        const bool germanUmlaut = ch == QChar(0x00E4)
            || ch == QChar(0x00F6)
            || ch == QChar(0x00FC)
            || ch == QChar(0x00C4)
            || ch == QChar(0x00D6)
            || ch == QChar(0x00DC);
        const bool asciiLetterOrDigit = ch.isLetterOrNumber() && ch.unicode() < 128;
        if (asciiLetterOrDigit || germanUmlaut) {
            normalized += ch;
            continue;
        }

        if (ch.isSpace()) {
            normalized += QLatin1Char(' ');
            continue;
        }

        normalized += QLatin1Char(' ');
    }

    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    normalized = normalized.trimmed();
    return normalized;
}

bool isValidBricsCadLayerName(const QString& name)
{
    const QString trimmed = name.trimmed();
    return !trimmed.isEmpty()
        && normalizedBricsCadLayerName(trimmed) == trimmed
        && trimmed.compare(QStringLiteral("0"), Qt::CaseInsensitive) != 0;
}

bool promptExplicitlyNamesLayer(const QString& prompt, const QString& layerName)
{
    const QString expected = layerName.trimmed();
    if (expected.isEmpty()) {
        return false;
    }

    const QRegularExpression quotedLayer(
        QStringLiteral(R"((?:layer|ebene)\s*(?:name)?\s*(?:[:=]\s*)?[\"']([^\"'\r\n]+)[\"'])"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator matches = quotedLayer.globalMatch(prompt);
    while (matches.hasNext()) {
        if (matches.next().captured(1).trimmed().compare(expected, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    const QRegularExpression assignedLayer(
        QStringLiteral(R"((?:layer|ebene)\s*(?:name)?\s*[:=]\s*([^,;.\r\n]{1,64}))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch assignedMatch = assignedLayer.match(prompt);
    return assignedMatch.hasMatch()
        && assignedMatch.captured(1).trimmed().compare(expected, Qt::CaseInsensitive) == 0;
}

QJsonObject sanitizedDocumentContext(QJsonObject context)
{
    QString selectedText = context.value("selectedText").toString();
    if (selectedText.trimmed().isEmpty()) {
        return {};
    }

    bool truncated = context.value("truncated").toBool(false);
    if (selectedText.size() > kMaxDocumentContextChars) {
        selectedText = selectedText.left(kMaxDocumentContextChars);
        truncated = true;
    }

    context.insert("selectedText", selectedText);
    context.insert("hasDocuments", true);
    context.insert("truncated", truncated);
    context.insert("contextPolicy", QJsonObject{
        {"scope", "Only the extracted document excerpts in selectedText are available to the model."},
        {"largeDocumentStrategy", "The WebView selects requested PDF pages first; otherwise it sends a capped excerpt. Ask for a narrower page range if required information is missing."},
        {"citationPolicy", "When answering document questions, refer to page numbers or document names when available."},
    });
    return context;
}

QString promptWithDocumentContext(const QString& prompt, const QJsonObject& context)
{
    if (context.isEmpty()) {
        return prompt;
    }

    const QJsonArray documents = context.value("documents").toArray();
    const QString selectedText = context.value("selectedText").toString().trimmed();
    if (selectedText.isEmpty()) {
        return prompt;
    }

    QStringList metadataLines;
    for (const QJsonValue& value : documents) {
        const QJsonObject document = value.toObject();
        const QString name = document.value("name").toString();
        const QString type = document.value("type").toString();
        const QString included = document.value("included").toString();
        metadataLines << QString("- %1 (%2)%3")
            .arg(name.isEmpty() ? QStringLiteral("Dokument") : name,
                type.isEmpty() ? QStringLiteral("Datei") : type,
                included.isEmpty() ? QString() : QStringLiteral(", Kontext: %1").arg(included));
    }

    return QString(
        "%1\n\n"
        "[Dokumentkontext]\n"
        "Nutze ausschliesslich die folgenden extrahierten Auszuege fuer dokumentbezogene Fragen. "
        "Wenn der gewÃƒÂ¼nschte Seitenbereich oder Inhalt nicht enthalten ist, sage das klar und fordere einen engeren/anderen Bereich an.\n"
        "Dokumente:\n%2\n\n"
        "%3")
        .arg(prompt,
            metadataLines.isEmpty() ? QStringLiteral("- AngehÃƒÂ¤ngtes Dokument") : metadataLines.join('\n'),
            selectedText);
}

bool bridgeCapabilityMethodAvailable(const QJsonObject& method)
{
    if (method.contains(QStringLiteral("available"))
        && !method.value(QStringLiteral("available")).toBool(false)) {
        return false;
    }
    const QJsonObject availability = method.value(QStringLiteral("availability")).toObject();
    return !availability.contains(QStringLiteral("available"))
        || availability.value(QStringLiteral("available")).toBool(false);
}

bool bridgeCapabilitiesContainMethod(const QJsonObject& capabilities, const QString& methodName)
{
    const QJsonArray methods = capabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value("name").toString() == methodName
            && bridgeCapabilityMethodAvailable(method)) {
            return true;
        }
    }
    return false;
}

QJsonObject validationRepairGuidance(const QString& errorMessage)
{
    const QRegularExpression missingLayerPattern(
        QStringLiteral(R"((?:Aktion\s+(\d+)\s+\(([^)]+)\).*?)?(?:params\.)?layer[^\r\n]*?(?:nicht\s+vorhandenen\s+Layer|Layer[^\r\n]*?nicht\s+vorhanden)\s+['\"]([^'\"]+)['\"])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = missingLayerPattern.match(errorMessage);
    if (!match.hasMatch()) {
        return {};
    }

    QJsonObject guidance{
        {QStringLiteral("category"), QStringLiteral("missing_layer")},
        {QStringLiteral("layerName"), match.captured(3).trimmed()},
        {QStringLiteral("failedTool"), match.captured(2).trimmed()},
        {QStringLiteral("requiredTools"), QJsonArray{
            QStringLiteral("layers.create")}},
        {QStringLiteral("requiredSequence"), QJsonArray{
            QStringLiteral("1. Den exakt genannten Layer mit einem aktuell gemeldeten Layer-Tool anlegen."),
            QStringLiteral("2. Danach die urspruengliche Aktion mit unveraendertem Ziel-Layer erneut ausfuehren.")}},
        {QStringLiteral("forbiddenWorkarounds"), QJsonArray{
            QStringLiteral("Nicht ersatzweise auf Layer 0 oder einen anderen vorhandenen Layer zeichnen."),
            QStringLiteral("Den Layernamen nicht umbenennen, entfernen oder aus den Geometrieparametern weglassen."),
            QStringLiteral("Keine nativen Befehle oder Pseudo-Tools verwenden, wenn layers.create verfuegbar ist.")}},
        {QStringLiteral("batchPolicy"), QStringLiteral("Layer-Erzeugung muss im selben action_proposal vor jeder Aktion stehen, die diesen Layer verwendet.")},
    };
    bool actionOk = false;
    const int actionIndex = match.captured(1).toInt(&actionOk);
    if (actionOk) {
        guidance.insert(QStringLiteral("failedActionIndex"), actionIndex);
    }
    return guidance;
}

bool proposalCreatesMissingLayerBeforeUse(const QJsonArray& actions, const QJsonObject& guidance)
{
    if (guidance.value(QStringLiteral("category")).toString() != QStringLiteral("missing_layer")) {
        return false;
    }
    const QString missingLayer = normalizedBricsCadLayerName(guidance.value(QStringLiteral("layerName")).toString());
    const int failedActionIndex = guidance.value(QStringLiteral("failedActionIndex")).toInt(0);
    if (missingLayer.isEmpty() || failedActionIndex < 2 || failedActionIndex > actions.size()) {
        return false;
    }

    for (int i = 0; i < failedActionIndex - 1; ++i) {
        const QJsonObject action = actions.at(i).toObject();
        if (action.value(QStringLiteral("tool")).toString() != QStringLiteral("layers.create")) {
            continue;
        }
        const QString plannedLayer = normalizedBricsCadLayerName(
            action.value(QStringLiteral("params")).toObject().value(QStringLiteral("name")).toString());
        if (plannedLayer.compare(missingLayer, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString bridgeTokenFilePath()
{
    return QDir::temp().filePath(kBridgeTokenFileName);
}

bool shouldPrefetchSelectionDescription(const QString& prompt)
{
    const QString normalized = prompt.toLower();
    return normalized.contains("auswahl")
        && (normalized.contains("beschreib")
            || normalized.contains("was ist")
            || normalized.contains("was wurde")
            || normalized.contains("aktuell"));
}

bool extractPromptNumberMm(QString text, double* value)
{
    if (!value) {
        return false;
    }

    text.replace(',', '.');
    const QRegularExpression keyedPattern(
        QStringLiteral(R"((?:height|heightMm|hoehe|hohe|h|z)\s*[:=]?\s*(-?\d+(?:\.\d+)?)\s*(?:mm)?)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = keyedPattern.match(text);
    if (!match.hasMatch()) {
        const QRegularExpression fallbackPattern(
            QStringLiteral(R"((-?\d+(?:\.\d+)?)\s*(?:mm)?)"),
            QRegularExpression::CaseInsensitiveOption);
        match = fallbackPattern.match(text);
    }

    if (!match.hasMatch()) {
        return false;
    }

    bool ok = false;
    const double parsed = match.captured(1).toDouble(&ok);
    if (!ok || parsed <= 0.0) {
        return false;
    }

    *value = parsed;
    return true;
}

bool textHasKeyedNumber(QString text, const QStringList& keys)
{
    text.replace(',', '.');
    for (const QString& key : keys) {
        const QRegularExpression pattern(
            QStringLiteral(R"(\b%1\s*[:=]?\s*-?\d+(?:\.\d+)?\s*(?:mm)?)").arg(QRegularExpression::escape(key)),
            QRegularExpression::CaseInsensitiveOption);
        if (pattern.match(text).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool textMentionsAny(const QString& normalized, const QStringList& needles)
{
    for (const QString& needle : needles) {
        if (normalized.contains(needle)) {
            return true;
        }
    }
    return false;
}

bool promptRequestsCadDataQuery(const QString& text)
{
    const QString normalized = text.toLower();
    const bool dataSubject = textMentionsAny(normalized, {
        QStringLiteral("tabelle"),
        QStringLiteral("tabellar"),
        QStringLiteral("daten"),
        QStringLiteral("messwert"),
        QStringLiteral("metriken"),
        QStringLiteral("abmess"),
        QStringLiteral("masse"),
        QStringLiteral("maÃƒÅ¸e"),
        QStringLiteral("hoehe"),
        QStringLiteral("hÃƒÆ’Ã‚Â¶he"),
        QStringLiteral("breite"),
        QStringLiteral("tiefe"),
        QStringLiteral("laenge"),
        QStringLiteral("lÃƒÆ’Ã‚Â¤nge"),
        QStringLiteral("flaeche"),
        QStringLiteral("flÃƒÆ’Ã‚Â¤che"),
        QStringLiteral("volumen"),
        QStringLiteral("bounding"),
        QStringLiteral("bbox"),
        QStringLiteral("objekt"),
        QStringLiteral("object"),
        QStringLiteral("entity"),
        QStringLiteral("entities"),
        QStringLiteral("geometry"),
        QStringLiteral("geometrie"),
        QStringLiteral("layer"),
        QStringLiteral("bim"),
        QStringLiteral("klassifikation"),
        QStringLiteral("eigenschaft"),
        QStringLiteral("property"),
        QStringLiteral("component type"),
        QStringLiteral("guid"),
        QStringLiteral("handle"),
        QStringLiteral("wand"),
        QStringLiteral("wall"),
        QStringLiteral("fenster"),
        QStringLiteral("window"),
        QStringLiteral("tuer"),
        QStringLiteral("tÃƒÂ¼r"),
        QStringLiteral("door"),
    });
    const bool dataVerb = textMentionsAny(normalized, {
        QStringLiteral("zeige"),
        QStringLiteral("anzeigen"),
        QStringLiteral("liste"),
        QStringLiteral("auflisten"),
        QStringLiteral("ausgeben"),
        QStringLiteral("auslesen"),
        QStringLiteral("abfragen"),
        QStringLiteral("fetch"),
        QStringLiteral("fetche"),
        QStringLiteral("holen"),
        QStringLiteral("hole"),
        QStringLiteral("ermittle"),
        QStringLiteral("ermitteln"),
        QStringLiteral("messen"),
        QStringLiteral("messe"),
        QStringLiteral("berechne"),
        QStringLiteral("ergaenz"),
        QStringLiteral("ergÃƒÆ’Ã‚Â¤nz"),
        QStringLiteral("einbezieh"),
        QStringLiteral("versuche"),
        QStringLiteral("brauch"),
    });
    const bool allObjects = normalized.contains(QStringLiteral("alle"))
        && textMentionsAny(normalized, {
            QStringLiteral("objekt"),
            QStringLiteral("object"),
            QStringLiteral("entity"),
             QStringLiteral("entities"),
             QStringLiteral("layer"),
             QStringLiteral("bim"),
             QStringLiteral("waende"),
             QStringLiteral("wÃƒÂ¤nde"),
             QStringLiteral("walls"),
         });
    return (dataSubject && dataVerb) || allObjects;
}

bool promptNeedsCalculationAgent(const QString& text)
{
    const QString normalized = text.toLower();
    return textMentionsAny(normalized, {
        QStringLiteral("berechn"),
        QStringLiteral("rechnung"),
        QStringLiteral("calculate"),
        QStringLiteral("calculation"),
        QStringLiteral("koordinate"),
        QStringLiteral("coordinate"),
        QStringLiteral("vektor"),
        QStringLiteral("vector"),
        QStringLiteral("abstand"),
        QStringLiteral("distance"),
        QStringLiteral("laenge"),
        QStringLiteral("lÃƒÂ¤nge"),
        QStringLiteral("flaeche"),
        QStringLiteral("flÃƒÂ¤che"),
        QStringLiteral("area"),
        QStringLiteral("heizlast"),
        QStringLiteral("waermelast"),
        QStringLiteral("wÃƒÂ¤rmelast"),
        QStringLiteral("volumen"),
        QStringLiteral("winkel"),
        QStringLiteral("angle"),
    });
}

bool promptRequestsEntityRename(const QString& text)
{
    const QString normalized = text.toLower();
    const bool renameIntent = textMentionsAny(normalized, {
        QStringLiteral("umbenenn"),
        QStringLiteral("rename"),
        QStringLiteral("benenn"),
        QStringLiteral("namen"),
        QStringLiteral("name"),
    });
    if (!renameIntent) {
        return false;
    }

    const bool entityIntent = textMentionsAny(normalized, {
        QStringLiteral("bim-wand"),
        QStringLiteral("bim wand"),
        QStringLiteral("bim-waende"),
        QStringLiteral("bim wÃƒÂ¤nde"),
        QStringLiteral("bim-wÃƒÂ¤nde"),
        QStringLiteral("wand"),
        QStringLiteral("wÃƒÂ¤nde"),
        QStringLiteral("waende"),
        QStringLiteral("solid"),
        QStringLiteral("objekt"),
        QStringLiteral("object"),
        QStringLiteral("entity"),
        QStringLiteral("entities"),
    });
    const bool explicitLayerRename = textMentionsAny(normalized, {
        QStringLiteral("layer umbenenn"),
        QStringLiteral("layer rename"),
        QStringLiteral("ebene umbenenn"),
    });
    return entityIntent && !explicitLayerRename;
}

bool promptMentionsRotationAngle(const QString& text)
{
    QString normalized = text.toLower();
    normalized.replace(',', '.');

    const QRegularExpression explicitUnitPattern(
        QStringLiteral(R"((?:^|[^\p{L}\d])[-+]?\d+(?:\.\d+)?\s*(?:Ã‚Â°|grad|degree|degrees|deg)\b)"),
        QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::CaseInsensitiveOption);
    if (explicitUnitPattern.match(normalized).hasMatch()) {
        return true;
    }

    const QRegularExpression umAnglePattern(
        QStringLiteral(R"(\bum\s*[-+]?\d+(?:\.\d+)?\b)"),
        QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::CaseInsensitiveOption);
    if (umAnglePattern.match(normalized).hasMatch()) {
        return true;
    }

    return textMentionsAny(normalized, {
        QStringLiteral("viertelumdrehung"),
        QStringLiteral("halbe umdrehung"),
        QStringLiteral("halbumdrehung"),
        QStringLiteral("angledeg"),
        QStringLiteral("anglerad"),
    });
}

QString agentResourceText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

QJsonObject agentResourceJsonObject(const QString& path)
{
    const QString text = agentResourceText(path);
    if (text.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

struct AgentWorkflowFile {
    QString directoryPath;
    QString fileName;
    QString path;
    bool bundled = false;
};

QVector<AgentWorkflowFile> agentWorkflowFiles(const QStringList& directoryPaths)
{
    QVector<AgentWorkflowFile> files;
    for (const QString& directoryPath : directoryPaths) {
        QDir dir(directoryPath);
        if (!dir.exists()) {
            continue;
        }

        const QStringList entries = dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& fileName : entries) {
            files.append(AgentWorkflowFile{
                directoryPath,
                fileName,
                dir.filePath(fileName),
                directoryPath.startsWith(QStringLiteral(":/")),
            });
        }
    }
    return files;
}

QString workflowTimestampIso(const AgentWorkflowFile& source, const QJsonObject& workflow, const QString& fieldName)
{
    const QString direct = workflow.value(fieldName).toString().trimmed();
    if (!direct.isEmpty()) {
        return direct;
    }

    const QString alternate = fieldName == QStringLiteral("modifiedAt")
        ? workflow.value(QStringLiteral("updatedAt")).toString().trimmed()
        : workflow.value(QStringLiteral("createdAt")).toString().trimmed();
    if (!alternate.isEmpty()) {
        return alternate;
    }

    if (source.bundled) {
        return {};
    }

    const QFileInfo info(source.path);
    const QDateTime timestamp = fieldName == QStringLiteral("createdAt")
        ? info.birthTime()
        : info.lastModified();
    return timestamp.isValid() ? timestamp.toUTC().toString(Qt::ISODateWithMs) : QString{};
}

bool readAgentWorkflowJson(const QString& path, QJsonObject* workflow, QString* errorMessage = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }

    if (workflow) {
        *workflow = document.object();
    }
    return true;
}

QJsonArray stringsToJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

void appendJsonStringUnique(QJsonArray& array, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    for (const QJsonValue& existing : array) {
        if (existing.toString() == trimmed) {
            return;
        }
    }
    array.append(trimmed);
}

void mergeJsonStringArray(QJsonObject& object, const QString& key, const QJsonArray& values)
{
    QJsonArray merged = object.value(key).toArray();
    for (const QJsonValue& value : values) {
        appendJsonStringUnique(merged, value.toString());
    }
    object.insert(key, merged);
}

QStringList allowedRoutesForModeName(const QString& mode)
{
    if (mode == QStringLiteral("general")) {
        return {
            QStringLiteral("general_chat"),
            QStringLiteral("document_qa"),
        };
    }
    return {
        QStringLiteral("bricscad_question"),
        QStringLiteral("bricscad_action"),
        QStringLiteral("document_qa_with_cad_context"),
        QStringLiteral("execution_summary"),
        QStringLiteral("validation_retry"),
    };
}

bool documentContextHasText(const QJsonObject& context);

QString fallbackRouteForMode(const QString& mode, const QJsonObject& documentContext)
{
    if (mode == QStringLiteral("general")) {
        return documentContextHasText(documentContext)
            ? QStringLiteral("document_qa")
            : QStringLiteral("general_chat");
    }
    return documentContextHasText(documentContext)
        ? QStringLiteral("document_qa_with_cad_context")
        : QStringLiteral("bricscad_question");
}

QString routerContractResourceForMode(const QString& mode)
{
    return mode == QStringLiteral("general")
        ? QStringLiteral(":/agent/contracts/router-general-v1.json")
        : QStringLiteral(":/agent/contracts/router-bricscad-v1.json");
}

bool routeAllowsCadContext(const QJsonObject& route)
{
    const QString name = route.value("route").toString();
    return name == QStringLiteral("bricscad_question")
        || name == QStringLiteral("bricscad_action")
        || name == QStringLiteral("document_qa_with_cad_context")
        || name == QStringLiteral("validation_retry")
        || name == QStringLiteral("execution_summary");
}

bool routeAllowsCadActions(const QJsonObject& route)
{
    const QString name = route.value("route").toString();
    return name == QStringLiteral("bricscad_action")
        || name == QStringLiteral("validation_retry");
}

bool routeAllowsDocumentContext(const QJsonObject& route)
{
    const QString name = route.value("route").toString();
    return name == QStringLiteral("document_qa")
        || name == QStringLiteral("document_qa_with_cad_context")
        || name == QStringLiteral("general_chat")
        || name == QStringLiteral("bricscad_question")
        || name == QStringLiteral("bricscad_action")
        || name == QStringLiteral("validation_retry");
}

bool promptRequiresCompleteConversationContext(const QString& prompt)
{
    QString normalized = repairMojibakeText(prompt).toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    const bool reviewIntent = textMentionsAny(normalized, {
        QStringLiteral("ÃƒÂ¼berprÃƒÂ¼f"), QStringLiteral("ueberpruef"), QStringLiteral("prÃƒÂ¼f"),
        QStringLiteral("pruef"), QStringLiteral("kontrollier"), QStringLiteral("rechenfehler"),
        QStringLiteral("formelfehler"), QStringLiteral("vollstÃƒÂ¤ndig"), QStringLiteral("vollstaendig"),
    });
    const bool wholeContextTarget = textMentionsAny(normalized, {
        QStringLiteral("workflow"), QStringLiteral("sitzung"), QStringLiteral("verlauf"),
        QStringLiteral("gesamten"), QStringLiteral("kompletten"), QStringLiteral("aktuellen"),
        QStringLiteral("formeln"), QStringLiteral("gleichungen"), QStringLiteral("rechnungen"),
    });
    return reviewIntent && wholeContextTarget;
}

QString capabilityProfileForRoute(const QString& route)
{
    if (route == QStringLiteral("document_qa")) {
        return QStringLiteral("document");
    }
    if (route == QStringLiteral("document_qa_with_cad_context")) {
        return QStringLiteral("document_bricscad");
    }
    if (route == QStringLiteral("bricscad_question")) {
        return QStringLiteral("bricscad_readonly");
    }
    if (route == QStringLiteral("bricscad_action")
        || route == QStringLiteral("validation_retry")) {
        return QStringLiteral("bricscad_confirmed_actions");
    }
    if (route == QStringLiteral("execution_summary")) {
        return QStringLiteral("bricscad_execution_summary");
    }
    return QStringLiteral("general");
}

bool documentContextHasText(const QJsonObject& context)
{
    return !context.value("selectedText").toString().trimmed().isEmpty();
}

QString workflowTrainingSearchText(QString text);

bool promptRequestsBimClassification(const QString& prompt)
{
    const QString normalized = prompt.toLower();
    return normalized.contains(QStringLiteral("bimwand"))
        || normalized.contains(QStringLiteral("bim wall"))
        || normalized.contains(QStringLiteral("bim-wall"))
        || normalized.contains(QStringLiteral("als bim"))
        || normalized.contains(QStringLiteral("bim klassifiz"))
        || normalized.contains(QStringLiteral("klassifiz"))
        || normalized.contains(QStringLiteral("klassifizi"))
        || normalized.contains(QStringLiteral("classification"));
}

bool promptDescribesArchitecturalWalls(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    return textMentionsAny(normalized, {
               QStringLiteral("wand"),
               QStringLiteral("waende"),
               QStringLiteral("aussenwand"),
               QStringLiteral("aussenwaende"),
               QStringLiteral("wÃƒÂ¤nde"),
               QStringLiteral("wall"),
               QStringLiteral("walls"),
           })
        && textMentionsAny(normalized, {
               QStringLiteral("wandstaerke"),
               QStringLiteral("wandstÃƒÂ¤rke"),
               QStringLiteral("wanddicke"),
               QStringLiteral("wandhoehe"),
               QStringLiteral("wandhÃƒÂ¶he"),
               QStringLiteral("hoehe der waende"),
               QStringLiteral("hÃƒÂ¶he der wÃƒÂ¤nde"),
               QStringLiteral("raum"),
               QStringLiteral("grundriss"),
           });
}

bool promptAllowsBimWallClassification(const QString& prompt)
{
    return promptRequestsBimClassification(prompt)
        || promptDescribesArchitecturalWalls(prompt);
}

bool promptTargetsBimObjects(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    return textMentionsAny(normalized, {
        QStringLiteral("bim"),
        QStringLiteral("bimwall"),
        QStringLiteral("bim wall"),
        QStringLiteral("klassifiziert"),
        QStringLiteral("classification"),
        QStringLiteral("component type"),
        QStringLiteral("guid"),
        QStringLiteral("wand"),
        QStringLiteral("wall"),
        QStringLiteral("fenster"),
        QStringLiteral("window"),
        QStringLiteral("tuer"),
        QStringLiteral("tÃƒÂ¼r"),
        QStringLiteral("door"),
        QStringLiteral("slab"),
        QStringLiteral("column"),
        QStringLiteral("beam"),
    });
}

QString fallbackRouteNameForPrompt(const QString& prompt, const QJsonObject& documentContext)
{
    const QString normalized = prompt.toLower();
    const bool hasDocumentContext = documentContextHasText(documentContext);
    const bool mentionsDocument = textMentionsAny(normalized, {
        QStringLiteral("pdf"),
        QStringLiteral("dokument"),
        QStringLiteral("datei"),
        QStringLiteral("seite"),
        QStringLiteral("seiten"),
        QStringLiteral("word"),
        QStringLiteral("text"),
        QStringLiteral("zusammenfassung"),
        QStringLiteral("fasse"),
        QStringLiteral("inhalt"),
        QStringLiteral("rechtschreib"),
        QStringLiteral("anhang"),
    });
    const bool mentionsCad = textMentionsAny(normalized, {
        QStringLiteral("bricscad"),
        QStringLiteral("cad"),
        QStringLiteral("dwg"),
        QStringLiteral("zeichnung"),
        QStringLiteral("grundriss"),
        QStringLiteral("floor plan"),
        QStringLiteral("layer"),
        QStringLiteral("ebene"),
        QStringLiteral("wand"),
        QStringLiteral("solid"),
        QStringLiteral("kreis"),
        QStringLiteral("rechteck"),
        QStringLiteral("linie"),
        QStringLiteral("polyline"),
        QStringLiteral("geometrie"),
        QStringLiteral("extrusion"),
        QStringLiteral("extrudi"),
        QStringLiteral("bim"),
        QStringLiteral("tga"),
        QStringLiteral("layout"),
        QStringLiteral("block"),
        QStringLiteral("auswahl"),
        QStringLiteral("selekt"),
        QStringLiteral("objekt"),
        QStringLiteral("entity"),
        QStringLiteral("handle"),
        QStringLiteral("modell"),
        QStringLiteral("fenster"),
        QStringLiteral("window"),
        QStringLiteral("tuer"),
        QStringLiteral("tÃƒÂ¼r"),
        QStringLiteral("door"),
    });
    const bool explanatoryQuestion =
        normalized.startsWith(QStringLiteral("was ist"))
        || normalized.startsWith(QStringLiteral("was bedeutet"))
        || normalized.startsWith(QStringLiteral("erklaere"))
        || normalized.startsWith(QStringLiteral("erklÃƒÂ¤re"))
        || normalized.contains(QStringLiteral("definition"));
    const bool mentionsBimWallAction = mentionsCad
        && !explanatoryQuestion
        && promptAllowsBimWallClassification(normalized);
    const bool mentionsCadDataQuery = !explanatoryQuestion
        && promptRequestsCadDataQuery(normalized);
    const bool mentionsCadAction = mentionsCad && !explanatoryQuestion && textMentionsAny(normalized, {
        QStringLiteral("erstelle"),
        QStringLiteral("erstellen"),
        QStringLiteral("zeichne"),
        QStringLiteral("anlegen"),
        QStringLiteral("lege"),
        QStringLiteral("loesche"),
        QStringLiteral("lÃƒÂ¶sche"),
        QStringLiteral("aendere"),
        QStringLiteral("ÃƒÂ¤ndere"),
        QStringLiteral("setze"),
        QStringLiteral("verschiebe"),
        QStringLiteral("verschieben"),
        QStringLiteral("kopiere"),
        QStringLiteral("extrudi"),
        QStringLiteral("klassifiz"),
        QStringLiteral("klassifizi"),
        QStringLiteral("zuweis"),
        QStringLiteral("umwandel"),
        QStringLiteral("wandle"),
        QStringLiteral("faerbe"),
        QStringLiteral("fÃƒÂ¤rbe"),
        QStringLiteral("skaliere"),
        QStringLiteral("rotiere"),
        QStringLiteral("drehe"),
        QStringLiteral("rotate"),
        QStringLiteral("selektiere"),
        QStringLiteral("waehle"),
        QStringLiteral("wÃƒÂ¤hle"),
        QStringLiteral("select"),
        QStringLiteral("speichere"),
        QStringLiteral("fuehre"),
        QStringLiteral("fÃƒÂ¼hre"),
        QStringLiteral("ausfÃƒÂ¼hren"),
        QStringLiteral("ausfuehren"),
    }) || mentionsBimWallAction || mentionsCadDataQuery;

    if (hasDocumentContext && mentionsDocument && !mentionsCadAction) {
        return QStringLiteral("document_qa");
    }
    if (mentionsCadAction) {
        return QStringLiteral("bricscad_action");
    }
    if (mentionsCad) {
        return QStringLiteral("bricscad_question");
    }
    if (hasDocumentContext && mentionsDocument) {
        return QStringLiteral("document_qa");
    }
    return QStringLiteral("general_chat");
}

bool promptDelegatesValueChoice(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    return textMentionsAny(normalized, {
        QStringLiteral("suche dir"), QStringLiteral("such dir"),
        QStringLiteral("waehle selbst"), QStringLiteral("waehle selbst"),
        QStringLiteral("waehle eigene"), QStringLiteral("waehle eigene"),
        QStringLiteral("setz eigene"), QStringLiteral("setze eigene"),
        QStringLiteral("nimm eigene"), QStringLiteral("bestimme selbst"),
        QStringLiteral("entscheide selbst"),
        QStringLiteral("ansonsten alle werte selbst"),
        QStringLiteral("alle anderen werte selbst"),
        QStringLiteral("beispielwert"), QStringLiteral("beispielwerte"),
        QStringLiteral("plausible werte"), QStringLiteral("sinnvolle werte"),
        QStringLiteral("eigene werte")});
}

QJsonObject makeAgentRoute(const QString& route, const QString& reason)
{
    return QJsonObject{
        {"schema", "barebone.agent.router.v1"},
        {"route", route},
        {"capabilityProfile", capabilityProfileForRoute(route)},
        {"reason", reason},
    };
}

QJsonObject normalizedAgentRoute(
    const QJsonObject& routeObject,
    const QString& prompt,
    const QJsonObject& documentContext,
    const QString& fallbackReason = QStringLiteral("local fallback"))
{
    static const QStringList allowedRoutes{
        QStringLiteral("general_chat"),
        QStringLiteral("document_qa"),
        QStringLiteral("document_qa_with_cad_context"),
        QStringLiteral("bricscad_question"),
        QStringLiteral("bricscad_action"),
        QStringLiteral("execution_summary"),
        QStringLiteral("validation_retry"),
    };
    QString route = routeObject.value("route").toString().trimmed();
    if (!allowedRoutes.contains(route)) {
        route = fallbackRouteNameForPrompt(prompt, documentContext);
    }

    const QString profile = capabilityProfileForRoute(route);

    QString reason = routeObject.value("reason").toString().trimmed();
    if (reason.isEmpty()) {
        reason = fallbackReason;
    }

    QJsonObject normalized = routeObject;
    normalized.insert("schema", "barebone.agent.router.v1");
    normalized.insert("route", route);
    normalized.insert("capabilityProfile", profile);
    normalized.insert("reason", reason);
    return normalized;
}

QJsonObject normalizedAgentRouteForMode(
    const QJsonObject& routeObject,
    const QString& prompt,
    const QJsonObject& documentContext,
    const QString& mode,
    const QString& fallbackReason = QStringLiteral("local fallback"))
{
    QJsonObject normalized = normalizedAgentRoute(routeObject, prompt, documentContext, fallbackReason);
    const QStringList allowedRoutes = allowedRoutesForModeName(mode);
    QString route = normalized.value("route").toString();
    if (!allowedRoutes.contains(route)) {
        QString fallbackRoute = mode == QStringLiteral("bricscad")
            ? QStringLiteral("bricscad_action")
            : fallbackRouteNameForPrompt(prompt, documentContext);
        if (mode == QStringLiteral("general")) {
            fallbackRoute = documentContextHasText(documentContext)
                ? QStringLiteral("document_qa")
                : QStringLiteral("general_chat");
        } else if (fallbackRoute == QStringLiteral("document_qa")) {
            fallbackRoute = QStringLiteral("document_qa_with_cad_context");
        }
        if (!allowedRoutes.contains(fallbackRoute)) {
            fallbackRoute = fallbackRouteForMode(mode, documentContext);
        }
        const QString originalRoute = route;
        route = fallbackRoute;
        normalized.insert("route", route);
        normalized.insert("capabilityProfile", capabilityProfileForRoute(route));
        normalized.insert("reason", QString("%1; modePolicy=%2; route %3 -> %4")
            .arg(normalized.value("reason").toString(fallbackReason),
                mode,
                originalRoute.isEmpty() ? QStringLiteral("<leer>") : originalRoute,
                route));
    }
    normalized.insert("mode", mode);
    return normalized;
}

QJsonObject modePolicyForMode(const QString& mode, const QJsonObject& route)
{
    const bool bricscadMode = mode == QStringLiteral("bricscad");
    const bool cadActionRoute = bricscadMode && routeAllowsCadActions(route);
    const bool cadContextRoute = bricscadMode && routeAllowsCadContext(route);

    return QJsonObject{
        {"mode", mode},
        {"allowedRoutes", stringsToJsonArray(allowedRoutesForModeName(mode))},
        {"cadContextAllowed", cadContextRoute},
        {"cadToolsAllowed", cadActionRoute},
        {"cadActionsRequireBrx", cadActionRoute},
        {"toolProposalAllowed", cadActionRoute},
        {"generalChatAllowed", mode == QStringLiteral("general")},
        {"documentContextAllowed", routeAllowsDocumentContext(route)},
        {"route", route.value("route").toString()},
        {"capabilityProfile", route.value("capabilityProfile").toString()},
    };
}

QJsonArray routeAllowedResponseTypes(const QString& route, bool toolsAvailable)
{
    Q_UNUSED(toolsAvailable);
    if (route == QStringLiteral("general_chat")
        || route == QStringLiteral("document_qa")) {
        return QJsonArray{"message"};
    }
    if (route == QStringLiteral("document_qa_with_cad_context")
        || route == QStringLiteral("bricscad_question")) {
        return route == QStringLiteral("bricscad_question")
            ? QJsonArray{"message", "ask_user", "context_request", "action_proposal", "plan"}
            : QJsonArray{"message", "ask_user", "context_request"};
    }
    if (route == QStringLiteral("execution_summary")) {
        return QJsonArray{"message"};
    }
    if (route == QStringLiteral("bricscad_action")
        || route == QStringLiteral("validation_retry")) {
        return QJsonArray{"message", "ask_user", "context_request", "action_proposal", "plan"};
    }
    return QJsonArray{"message"};
}

bool routeAllowsResponseType(const QJsonObject& route, const QString& responseType, bool toolsAvailable)
{
    const QJsonArray allowed = routeAllowedResponseTypes(route.value("route").toString(), toolsAvailable);
    for (const QJsonValue& value : allowed) {
        if (value.toString() == responseType) {
            return true;
        }
    }
    return false;
}

QStringList jsonStringArrayToStringList(const QJsonArray& values, int maxCount = 24)
{
    QStringList result;
    for (const QJsonValue& value : values) {
        const QString text = value.toString().trimmed();
        if (text.isEmpty()) {
            continue;
        }
        result << text;
        if (result.size() >= maxCount) {
            break;
        }
    }
    return result;
}

QStringList toolNamesForLog(const QJsonArray& tools, int maxCount = 24)
{
    QStringList names;
    for (const QJsonValue& value : tools) {
        const QString name = value.toObject().value("name").toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        names << name;
        if (names.size() >= maxCount) {
            if (tools.size() > maxCount) {
                names << QStringLiteral("+%1").arg(tools.size() - maxCount);
            }
            break;
        }
    }
    return names;
}

QStringList policyRefsForRoute(const QJsonObject& route, const QString& prompt, const QJsonObject& documentContext)
{
    const QString routeName = route.value("route").toString();
    Q_UNUSED(prompt);
    QStringList refs{QStringLiteral("core")};

    if (routeName == QStringLiteral("general_chat")) {
        refs << QStringLiteral("general");
    }
    if (routeName == QStringLiteral("document_qa")
        || routeName == QStringLiteral("document_qa_with_cad_context")
        || documentContextHasText(documentContext)) {
        refs << QStringLiteral("documents");
    }
    if (routeAllowsCadContext(route)) {
        refs << QStringLiteral("bricscad-safety");
    }

    refs.removeDuplicates();
    return refs;
}

QString policyResourcePath(const QString& ref)
{
    return QStringLiteral(":/agent/policies/%1.md").arg(ref);
}

QJsonArray policyTextForRefs(const QStringList& refs)
{
    QJsonArray policies;
    for (const QString& ref : refs) {
        const QString text = agentResourceText(policyResourcePath(ref));
        if (!text.isEmpty()) {
            policies.append(QJsonObject{
                {"ref", ref},
                {"text", text},
            });
        }
    }
    return policies;
}

QString repairMojibakeText(QString text);
QString workflowSlotNameFromValue(const QJsonValue& value);
QString canonicalWorkflowSlot(QString slot);
QString workflowTrainingSearchText(QString text);

QString workflowSlug(QString text)
{
    text = repairMojibakeText(text).trimmed().toLower();
    text.replace(QChar(0x00E4), QStringLiteral("ae"));
    text.replace(QChar(0x00F6), QStringLiteral("oe"));
    text.replace(QChar(0x00FC), QStringLiteral("ue"));
    text.replace(QChar(0x00DF), QStringLiteral("ss"));
    text.replace(QStringLiteral("Ã¤"), QStringLiteral("ae"));
    text.replace(QStringLiteral("Ã¶"), QStringLiteral("oe"));
    text.replace(QStringLiteral("Ã¼"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ÃŸ"), QStringLiteral("ss"));
    text.replace(QStringLiteral("ã¤"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ã¶"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ã¼"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ÃƒÂ¤"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ÃƒÂ¶"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ÃƒÂ¼"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ÃƒÅ¸"), QStringLiteral("ss"));
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    text.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (text.startsWith(QLatin1Char('_'))) {
        text.remove(0, 1);
    }
    while (text.endsWith(QLatin1Char('_'))) {
        text.chop(1);
    }
    return text.isEmpty() ? QStringLiteral("workflow") : text.left(80);
}

QString jsonStringFieldFromPrefix(const QByteArray& data, const QString& key)
{
    if (data.isEmpty() || key.trimmed().isEmpty()) {
        return {};
    }
    const QString text = QString::fromUtf8(data);
    const QRegularExpression expression(
        QStringLiteral(R"json("%1"\s*:\s*"((?:\\.|[^"\\])*)")json").arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch match = expression.match(text);
    if (!match.hasMatch()) {
        return {};
    }
    const QByteArray objectJson = QByteArrayLiteral("{\"value\":\"")
        + match.captured(1).toUtf8()
        + QByteArrayLiteral("\"}");
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(objectJson, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        return document.object().value(QStringLiteral("value")).toString().trimmed();
    }
    return match.captured(1).trimmed();
}

QString normalizedGeneralWorkflowLatex(QString text)
{
    return repairMojibakeText(text).trimmed();
}

QString latexFromGeneralWorkflowFormula(const QJsonObject& formula)
{
    QString latex = formula.value(QStringLiteral("latex")).toString().trimmed();
    if (latex.isEmpty()) {
        latex = formula.value(QStringLiteral("latexExpression")).toString().trimmed();
    }
    if (latex.isEmpty()) {
        latex = formula.value(QStringLiteral("expression")).toString().trimmed();
    }
    if (latex.isEmpty()) {
        latex = formula.value(QStringLiteral("formula")).toString().trimmed();
    }
    return normalizedGeneralWorkflowLatex(latex);
}

QString normalizedGeneralWorkflowBlockText(QString text)
{
    text = repairMojibakeText(text).trimmed();
    if (text.isEmpty()) {
        return {};
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QRegularExpression(QStringLiteral("\\n{3,}")), QStringLiteral("\n\n"));
    return text.trimmed();
}

QJsonObject normalizedGeneralWorkflowForSave(QJsonObject workflow)
{
    return workflow;
}

QJsonArray generalWorkflowBlocks(const QJsonObject& workflow)
{
    return workflow.value(QStringLiteral("display")).toObject().value(QStringLiteral("blocks")).toArray();
}

QJsonObject generalWorkflowWithBlocks(QJsonObject workflow, const QJsonArray& blocks)
{
    QJsonObject display = workflow.value(QStringLiteral("display")).toObject();
    display.insert(QStringLiteral("blocks"), blocks);
    workflow.insert(QStringLiteral("display"), display);
    return workflow;
}

QJsonValue repairedMojibakeJsonValue(const QJsonValue& value)
{
    if (value.isString()) {
        return repairMojibakeText(value.toString());
    }
    if (value.isArray()) {
        QJsonArray repaired;
        for (const QJsonValue& item : value.toArray()) {
            repaired.append(repairedMojibakeJsonValue(item));
        }
        return repaired;
    }
    if (value.isObject()) {
        QJsonObject repaired;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            repaired.insert(it.key(), repairedMojibakeJsonValue(it.value()));
        }
        return repaired;
    }
    return value;
}

QJsonObject normalizedGeneralWorkflowDraft(QJsonObject workflow)
{
    workflow = repairedMojibakeJsonValue(workflow).toObject();
    if (workflow.contains(QStringLiteral("workflow")) && workflow.value(QStringLiteral("workflow")).isObject()) {
        workflow = workflow.value(QStringLiteral("workflow")).toObject();
    }
    if (workflow.value(QStringLiteral("schema")).toString().trimmed().isEmpty()) {
        workflow.insert(QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.v1"));
    }
    workflow.insert(QStringLiteral("kind"), QStringLiteral("general"));

    QString id = workflowSlug(workflow.value(QStringLiteral("id")).toString());
    if (id.isEmpty() || id == QStringLiteral("workflow")) {
        id = workflowSlug(workflow.value(QStringLiteral("title")).toString(QStringLiteral("planungsthema")));
    }
    workflow.insert(QStringLiteral("id"), id.isEmpty() ? QStringLiteral("planungsthema") : id);

    if (workflow.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        workflow.insert(QStringLiteral("title"), workflow.value(QStringLiteral("id")).toString(QStringLiteral("Workflow")));
    }
    if (!workflow.contains(QStringLiteral("description"))) {
        workflow.insert(QStringLiteral("description"), QString());
    }

    QJsonArray normalizedBlocks;
    QSet<QString> usedIds;
    const QJsonArray blocks = generalWorkflowBlocks(workflow);
    for (int i = 0; i < blocks.size(); ++i) {
        if (!blocks.at(i).isObject()) {
            continue;
        }
        QJsonObject block = blocks.at(i).toObject();
        QString title = repairMojibakeText(block.value(QStringLiteral("title")).toString()).trimmed();
        QString text = repairMojibakeText(block.value(QStringLiteral("text")).toString()).trimmed();
        if (text.isEmpty()) {
            text = repairMojibakeText(block.value(QStringLiteral("content")).toString()).trimmed();
        }
        if (text.isEmpty()) {
            text = repairMojibakeText(block.value(QStringLiteral("description")).toString()).trimmed();
        }
        text = normalizedGeneralWorkflowBlockText(text);
        QString id = workflowSlug(block.value(QStringLiteral("id")).toString());
        if (id.isEmpty() || id == QStringLiteral("workflow")) {
            id = workflowSlug(title);
        }
        if (id.isEmpty() || id == QStringLiteral("workflow")) {
            id = QStringLiteral("absatz_%1").arg(i + 1);
        }
        const QString baseId = id;
        int suffix = 2;
        while (usedIds.contains(id)) {
            id = QStringLiteral("%1_%2").arg(baseId).arg(suffix++);
        }
        usedIds.insert(id);
        if (title.isEmpty()) {
            title = QStringLiteral("Absatz %1").arg(i + 1);
        }
        block.insert(QStringLiteral("id"), id);
        block.insert(QStringLiteral("title"), title);
        block.insert(QStringLiteral("text"), text);
        normalizedBlocks.append(block);
    }

    workflow = generalWorkflowWithBlocks(workflow, normalizedBlocks);
    workflow = normalizedGeneralWorkflowForSave(workflow);
    return workflow;
}

bool isGenericGeneralWorkflowName(const QString& name)
{
    const QString slug = workflowSlug(name);
    static const QSet<QString> genericNames{
        QStringLiteral("workflow"),
        QStringLiteral("neuer_workflow"),
        QStringLiteral("neue_workflow"),
        QStringLiteral("chat_workflow"),
        QStringLiteral("general_workflow"),
        QStringLiteral("allgemeiner_workflow"),
        QStringLiteral("allgemeine_workflow"),
        QStringLiteral("planungsthema"),
        QStringLiteral("neues_planungsthema"),
        QStringLiteral("berechnung"),
        QStringLiteral("workflow_speichern")
    };
    return genericNames.contains(slug)
        || slug.startsWith(QStringLiteral("workflow_"))
        || slug.startsWith(QStringLiteral("neuer_workflow_"))
        || slug.startsWith(QStringLiteral("chat_workflow_"));
}

QString generalWorkflowDraftValidationError(const QJsonObject& workflow)
{
    const QString title = repairMojibakeText(workflow.value(QStringLiteral("title")).toString()).trimmed();
    if (title.isEmpty()) {
        return QStringLiteral("title fehlt");
    }
    if (isGenericGeneralWorkflowName(title)) {
        return QStringLiteral("title ist zu generisch; benenne direkt das Thema des Workflows");
    }
    const QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty() || isGenericGeneralWorkflowName(id)) {
        return QStringLiteral("id ist zu generisch; nutze einen thematischen Dateinamen");
    }

    const QJsonArray blocks = generalWorkflowBlocks(workflow);
    if (blocks.isEmpty()) {
        return QStringLiteral("display.blocks fehlt oder enthaelt keine Absaetze");
    }

    QSet<QString> ids;
    for (int i = 0; i < blocks.size(); ++i) {
        if (!blocks.at(i).isObject()) {
            return QStringLiteral("display.blocks[%1] ist kein Objekt").arg(i);
        }
        const QJsonObject block = blocks.at(i).toObject();
        const QString id = workflowSlug(block.value(QStringLiteral("id")).toString());
        const QString blockTitle = repairMojibakeText(block.value(QStringLiteral("title")).toString()).trimmed();
        const QString text = repairMojibakeText(block.value(QStringLiteral("text")).toString()).trimmed();
        if (id.isEmpty()) {
            return QStringLiteral("display.blocks[%1].id fehlt").arg(i);
        }
        if (ids.contains(id)) {
            return QStringLiteral("display.blocks[%1].id ist doppelt: %2").arg(i).arg(id);
        }
        ids.insert(id);
        if (blockTitle.isEmpty()) {
            return QStringLiteral("display.blocks[%1].title fehlt").arg(i);
        }
        if (text.isEmpty()) {
            return QStringLiteral("display.blocks[%1].text fehlt").arg(i);
        }
    }

    return {};
}

int substringCount(const QString& text, const QString& needle)
{
    if (needle.isEmpty()) {
        return 0;
    }
    int count = 0;
    qsizetype offset = 0;
    while ((offset = text.indexOf(needle, offset)) >= 0) {
        ++count;
        offset += needle.size();
    }
    return count;
}

QStringList markdownTableCells(const QString& line)
{
    QString value = line.trimmed();
    if (!value.contains(QLatin1Char('|'))) {
        return {};
    }
    if (value.startsWith(QLatin1Char('|'))) {
        value.remove(0, 1);
    }
    if (value.endsWith(QLatin1Char('|'))) {
        value.chop(1);
    }
    QStringList cells;
    for (const QString& cell : value.split(QLatin1Char('|'))) {
        cells << cell.trimmed();
    }
    return cells.isEmpty() ? QStringList{} : cells;
}

bool isMarkdownTableSeparatorLine(const QString& line)
{
    const QStringList cells = markdownTableCells(line);
    if (cells.isEmpty()) {
        return false;
    }
    const QRegularExpression separator(QStringLiteral(R"(^:?-{3,}:?$)"));
    for (const QString& cell : cells) {
        if (!separator.match(cell).hasMatch()) {
            return false;
        }
    }
    return true;
}

bool textContainsValidMarkdownTable(const QString& text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (!markdownTableCells(lines.at(i)).isEmpty()
            && isMarkdownTableSeparatorLine(lines.at(i + 1))) {
            return true;
        }
    }
    return false;
}

QStringList looseTableCells(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()
        || trimmed.startsWith(QStringLiteral("- "))
        || trimmed.startsWith(QStringLiteral("* "))
        || trimmed.startsWith(QStringLiteral("Ã¢â‚¬Â¢ "))
        || QRegularExpression(QStringLiteral(R"(^\d+[.)]\s+)")).match(trimmed).hasMatch()
        || isMarkdownTableSeparatorLine(trimmed)
        || !markdownTableCells(trimmed).isEmpty()) {
        return {};
    }

    QStringList cells;
    if (trimmed.contains(QLatin1Char('\t'))) {
        cells = trimmed.split(QRegularExpression(QStringLiteral("\\t+")), Qt::SkipEmptyParts);
    } else {
        cells = trimmed.split(QRegularExpression(QStringLiteral("\\s{2,}")), Qt::SkipEmptyParts);
    }
    for (QString& cell : cells) {
        cell = cell.trimmed();
    }
    cells.removeAll(QString());
    return cells.size() >= 2 ? cells : QStringList{};
}

bool textContainsLooseTable(const QString& text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    int consecutiveRows = 0;
    int expectedColumns = 0;
    for (const QString& line : lines) {
        const QStringList cells = looseTableCells(line);
        if (cells.size() >= 2 && (expectedColumns == 0 || cells.size() == expectedColumns)) {
            expectedColumns = cells.size();
            ++consecutiveRows;
            if (consecutiveRows >= 2) {
                return true;
            }
            continue;
        }
        consecutiveRows = 0;
        expectedColumns = 0;
    }
    return false;
}

QString structuredTablesValidationError(const QJsonArray& tables)
{
    for (int i = 0; i < tables.size(); ++i) {
        if (!tables.at(i).isObject()) {
            return QStringLiteral("Formatierungsfehler: tables[%1] ist kein Objekt").arg(i);
        }
        const QJsonObject table = tables.at(i).toObject();
        const QJsonArray columns = table.value(QStringLiteral("columns")).toArray();
        const QJsonArray rows = table.value(QStringLiteral("rows")).toArray();
        if (columns.isEmpty()) {
            return QStringLiteral("Formatierungsfehler: tables[%1].columns fehlt oder ist leer").arg(i);
        }
        if (rows.isEmpty()) {
            return QStringLiteral("Formatierungsfehler: tables[%1].rows fehlt oder ist leer").arg(i);
        }
        QStringList columnKeys;
        columnKeys.reserve(columns.size());
        for (int columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
            const QJsonValue column = columns.at(columnIndex);
            QString label;
            QString key;
            if (column.isString()) {
                label = column.toString().trimmed();
                key = label;
            } else if (column.isObject()) {
                const QJsonObject object = column.toObject();
                key = object.value(QStringLiteral("key")).toString().trimmed();
                if (key.isEmpty()) {
                    key = object.value(QStringLiteral("name")).toString().trimmed();
                }
                if (key.isEmpty()) {
                    key = object.value(QStringLiteral("label")).toString().trimmed();
                }
                label = object.value(QStringLiteral("label")).toString().trimmed();
                if (label.isEmpty()) {
                    label = object.value(QStringLiteral("name")).toString().trimmed();
                }
                if (label.isEmpty()) {
                    label = object.value(QStringLiteral("key")).toString().trimmed();
                }
            }
            if (label.isEmpty()) {
                return QStringLiteral("Formatierungsfehler: tables[%1].columns[%2] braucht label, name oder key")
                    .arg(i)
                    .arg(columnIndex);
            }
            columnKeys.append(key.isEmpty() ? label : key);
        }
        auto cellIsExplicitlyEmpty = [](const QJsonValue& value) {
            if (value.isNull() || value.isUndefined()) {
                return true;
            }
            QString text;
            if (value.isString()) {
                text = repairMojibakeText(value.toString()).trimmed();
            } else if (value.isDouble()) {
                text = QString::number(value.toDouble(), 'g', 12).trimmed();
            } else if (value.isBool()) {
                text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
            } else if (value.isArray()) {
                text = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)).trimmed();
            } else {
                text = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)).trimmed();
            }
            return text.isEmpty();
        };
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const QJsonValue row = rows.at(rowIndex);
            if (!row.isArray() && !row.isObject()) {
                return QStringLiteral("Formatierungsfehler: tables[%1].rows[%2] muss Objekt oder Array sein")
                    .arg(i)
                    .arg(rowIndex);
            }
            for (int columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
                const QJsonValue cell = row.isArray()
                    ? (columnIndex < row.toArray().size() ? row.toArray().at(columnIndex) : QJsonValue{})
                    : row.toObject().value(columnKeys.value(columnIndex));
                if (cellIsExplicitlyEmpty(cell)) {
                    return QStringLiteral("Formatierungsfehler: tables[%1].rows[%2].cells[%3] ist leer; nutze einen Textwert oder bewusst 'Ã¢â‚¬â€'")
                        .arg(i)
                        .arg(rowIndex)
                        .arg(columnIndex);
                }
            }
        }
    }
    return {};
}

QString blockFormattingValidationError(const QString& text, int blockIndex)
{
    const QString location = QStringLiteral("display.blocks[%1].text").arg(blockIndex);
    if (text.contains(QRegularExpression(QStringLiteral(R"(<\s*br\s*/?\s*>)"), QRegularExpression::CaseInsensitiveOption))) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt HTML-<br>; nutze echte Zeilenumbrueche und Markdown-Listen").arg(location);
    }

    if (text.contains(QLatin1Char('\t'))) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt tabulatorgetrennte Tabellen; nutze tables[] oder eine Markdown-Pipe-Tabelle").arg(location);
    }
    if (textContainsLooseTable(text)) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt eine spaltenartige Klartext-Tabelle; nutze tables[] oder eine Markdown-Pipe-Tabelle").arg(location);
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    int tableLikeRows = 0;
    bool hasPipeRowWithoutSeparator = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        if (!markdownTableCells(line).isEmpty()) {
            ++tableLikeRows;
            if (i + 1 >= lines.size() || !isMarkdownTableSeparatorLine(lines.at(i + 1))) {
                hasPipeRowWithoutSeparator = true;
            }
        }

        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("- ")) && QRegularExpression(QStringLiteral(R"(\s+-\s+\S)")).match(trimmed.mid(2)).hasMatch()) {
            return QStringLiteral("Formatierungsfehler: %1 enthaelt mehrere Aufzaehlungspunkte in einer Zeile").arg(location);
        }
        if (QRegularExpression(QStringLiteral(R"(^\d+[.)]\s+.+\s+\d+[.)]\s+\S)")).match(trimmed).hasMatch()) {
            return QStringLiteral("Formatierungsfehler: %1 enthaelt mehrere nummerierte Punkte in einer Zeile").arg(location);
        }
    }
    if (tableLikeRows >= 2 && hasPipeRowWithoutSeparator && !textContainsValidMarkdownTable(text)) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt eine Pipe-Tabelle ohne Markdown-Trennzeile |---|---|").arg(location);
    }

    return {};
}

QString generalWorkflowFormattingValidationError(const QJsonObject& workflow)
{
    const QString tablesError = structuredTablesValidationError(workflow.value(QStringLiteral("tables")).toArray());
    if (!tablesError.isEmpty()) {
        return tablesError;
    }

    const QJsonArray blocks = generalWorkflowBlocks(workflow);
    for (int i = 0; i < blocks.size(); ++i) {
        const QString error = blockFormattingValidationError(blocks.at(i).toObject().value(QStringLiteral("text")).toString(), i);
        if (!error.isEmpty()) {
            return error;
        }
    }

    return {};
}

QString publicWorkflowSummaryText(QString text);

QJsonObject generalWorkflowFromSaveResponse(QJsonObject response)
{
    if (response.contains(QStringLiteral("workflow")) && response.value(QStringLiteral("workflow")).isObject()) {
        response = response.value(QStringLiteral("workflow")).toObject();
    } else if (response.contains(QStringLiteral("workflowDraft")) && response.value(QStringLiteral("workflowDraft")).isObject()) {
        response = response.value(QStringLiteral("workflowDraft")).toObject();
    }

    QJsonObject workflow = response;
    const QString schema = workflow.value(QStringLiteral("schema")).toString().trimmed();
    if (schema == QStringLiteral("barebone.general.workflow.save.response.v1") || schema.isEmpty()) {
        workflow.insert(QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.v1"));
    }

    QJsonArray blocks = workflow.value(QStringLiteral("blocks")).toArray();
    if (blocks.isEmpty()) {
        blocks = workflow.value(QStringLiteral("display")).toObject().value(QStringLiteral("blocks")).toArray();
    }
    if (!blocks.isEmpty()) {
        QJsonObject display = workflow.value(QStringLiteral("display")).toObject();
        display.insert(QStringLiteral("blocks"), blocks);
        workflow.insert(QStringLiteral("display"), display);
    }
    workflow.remove(QStringLiteral("blocks"));

    if (!workflow.contains(QStringLiteral("domain"))) {
        workflow.insert(QStringLiteral("domain"), QStringLiteral("Allgemein"));
    }
    QString description = repairMojibakeText(workflow.value(QStringLiteral("description")).toString()).trimmed();
    if (description.isEmpty()) {
        description = repairMojibakeText(workflow.value(QStringLiteral("contextSummary")).toString()).trimmed();
    }
    if (description.isEmpty()) {
        for (const QJsonValue& value : generalWorkflowBlocks(workflow)) {
            const QString text = repairMojibakeText(value.toObject().value(QStringLiteral("text")).toString()).trimmed();
            if (!text.isEmpty()) {
                description = publicWorkflowSummaryText(text);
                if (description.isEmpty()) {
                    description = text;
                }
                break;
            }
        }
    }
    if (!description.isEmpty()) {
        description = publicWorkflowSummaryText(description);
        description.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        workflow.insert(QStringLiteral("description"), description.left(420).trimmed());
    }
    if (!workflow.contains(QStringLiteral("tags"))) {
        workflow.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("Chat"), QStringLiteral("AI-Entwurf")});
    }
    if (!workflow.contains(QStringLiteral("verificationStatus"))) {
        workflow.insert(QStringLiteral("verificationStatus"), QStringLiteral("AI-Entwurf"));
    }
    if (!workflow.contains(QStringLiteral("contextSummary"))
        || workflow.value(QStringLiteral("contextSummary")).toString().trimmed().isEmpty()) {
        workflow.insert(QStringLiteral("contextSummary"), workflow.value(QStringLiteral("description")).toString());
    }
    if (!workflow.contains(QStringLiteral("triggerExamples"))
        && !workflow.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        workflow.insert(QStringLiteral("triggerExamples"), QJsonArray{workflow.value(QStringLiteral("title")).toString()});
    }

    return normalizedGeneralWorkflowDraft(workflow);
}

QString cleanedGeneralWorkflowHeading(QString text)
{
    text = repairMojibakeText(text).trimmed();
    text.remove(QRegularExpression(QStringLiteral("^#{1,6}\\s*")));
    text.remove(QRegularExpression(QStringLiteral("^[-*]+\\s*")));
    text.remove(QRegularExpression(QStringLiteral("^\\d+[.)]\\s*")));
    text.replace(QRegularExpression(QStringLiteral("^\\*\\*(.+?)\\*\\*$")), QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral("^__(.+?)__$")), QStringLiteral("\\1"));
    text.remove(QRegularExpression(QStringLiteral("^\\*+\\s*")));
    text.remove(QRegularExpression(QStringLiteral("\\*+\\s*$")));
    text.replace(QRegularExpression(QStringLiteral("^(workflow|workflow-entwurf|entwurf|planungsthema|thema|titel|name)\\s*[:\\-*]+\\s*"),
        QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.trimmed();
}

QString stripMarkdownCodeFence(QString text)
{
    text = repairMojibakeText(text).trimmed();
    if (!text.startsWith(QStringLiteral("```"))) {
        return text;
    }
    const int firstNewline = text.indexOf(QLatin1Char('\n'));
    const int lastFence = text.lastIndexOf(QStringLiteral("```"));
    if (firstNewline >= 0 && lastFence > firstNewline) {
        return text.mid(firstNewline + 1, lastFence - firstNewline - 1).trimmed();
    }
    return text;
}

bool isGeneralWorkflowSaveMetaInstruction(const QString& text)
{
    const QString slug = workflowSlug(text);
    return slug == QStringLiteral("workflow_speichern")
        || slug == QStringLiteral("save_workflow")
        || slug == QStringLiteral("speichern")
        || slug == QStringLiteral("so_speichern")
        || slug.startsWith(QStringLiteral("speichere_ausschliesslich_diese_ai_nachricht"))
        || slug.startsWith(QStringLiteral("speichere_diese_ai_nachricht"))
        || slug.startsWith(QStringLiteral("speichere_ausgewaehlte_ai_nachricht"));
}

bool isGeneralWorkflowBoilerplateLine(const QString& line)
{
    QString value = repairMojibakeText(line).trimmed();
    if (value.isEmpty()) {
        return false;
    }

    QString plain = value;
    plain.replace(QRegularExpression(QStringLiteral(R"(\*\*([^*]+)\*\*)")), QStringLiteral("\\1"));
    plain.replace(QRegularExpression(QStringLiteral(R"(__([^_]+)__)")), QStringLiteral("\\1"));
    plain = cleanedGeneralWorkflowHeading(plain);

    const QString plainSlug = workflowSlug(plain);
    const QString fullSlug = workflowSlug(value);
    return isGeneralWorkflowSaveMetaInstruction(plain)
        || fullSlug.startsWith(QStringLiteral("workflow_entwurf_speichere_ausschliesslich_diese_ai_nachricht"))
        || fullSlug.startsWith(QStringLiteral("workflow_entwurf_speichere_diese_ai_nachricht"))
        || fullSlug.startsWith(QStringLiteral("workflow_speichere_ausschliesslich_diese_ai_nachricht"))
        || plainSlug.startsWith(QStringLiteral("speichere_ausschliesslich_diese_ai_nachricht"));
}

bool isOnlyMarkdownRuleText(const QString& text)
{
    const QStringList lines = repairMojibakeText(text).split(QLatin1Char('\n'));
    bool sawContent = false;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        sawContent = true;
        if (!QRegularExpression(QStringLiteral(R"(^(-{3,}|_{3,}|\*{3,})$)")).match(line).hasMatch()) {
            return false;
        }
    }
    return sawContent;
}

QString sanitizedGeneralWorkflowSourceText(QString text)
{
    text = stripMarkdownCodeFence(text);
    QStringList keptLines;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (isGeneralWorkflowBoilerplateLine(line)) {
            continue;
        }
        keptLines << line;
    }
    return keptLines.join(QLatin1Char('\n')).trimmed();
}

QString titleCandidateFromText(QString text)
{
    text = sanitizedGeneralWorkflowSourceText(text);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& rawLine : lines) {
        QString line = cleanedGeneralWorkflowHeading(rawLine);
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1Char('|')) || line.startsWith(QStringLiteral("\\["))) {
            continue;
        }
        if (line.size() > 100) {
            line = line.left(100).trimmed();
            const int lastSpace = line.lastIndexOf(QLatin1Char(' '));
            if (lastSpace > 40) {
                line = line.left(lastSpace).trimmed();
            }
        }
        if (!isGenericGeneralWorkflowName(line)) {
            return line;
        }
    }
    return {};
}

QString titleCandidateFromSaveContext(const QJsonObject& saveContext)
{
    const QString instruction = cleanedGeneralWorkflowHeading(saveContext.value(QStringLiteral("userInstruction")).toString());
    if (!instruction.isEmpty()
        && !isGenericGeneralWorkflowName(instruction)
        && !isGeneralWorkflowSaveMetaInstruction(instruction)) {
        return instruction.left(90).trimmed();
    }

    const QString sessionTitle = cleanedGeneralWorkflowHeading(saveContext.value(QStringLiteral("sessionTitle")).toString());
    if (!sessionTitle.isEmpty() && !isGenericGeneralWorkflowName(sessionTitle)) {
        return sessionTitle.left(90).trimmed();
    }

    const QJsonObject selectedWorkflow = saveContext.value(QStringLiteral("selectedWorkflow")).toObject();
    const QString selectedTitle = repairMojibakeText(selectedWorkflow.value(QStringLiteral("title")).toString()).trimmed();
    if (!selectedTitle.isEmpty() && !isGenericGeneralWorkflowName(selectedTitle)) {
        return selectedTitle;
    }

    const QJsonArray conversation = saveContext.value(QStringLiteral("conversation")).toArray();
    for (int i = conversation.size() - 1; i >= 0; --i) {
        const QJsonObject message = conversation.at(i).toObject();
        if (message.value(QStringLiteral("speaker")).toString() != QStringLiteral("Du")) {
            continue;
        }
        QString candidate = cleanedGeneralWorkflowHeading(message.value(QStringLiteral("message")).toString());
        if (candidate.isEmpty() || isGenericGeneralWorkflowName(candidate)) {
            continue;
        }
        if (candidate.size() > 90) {
            candidate = candidate.left(90).trimmed();
            const int lastSpace = candidate.lastIndexOf(QLatin1Char(' '));
            if (lastSpace > 35) {
                candidate = candidate.left(lastSpace).trimmed();
            }
        }
        return candidate;
    }
    return {};
}

QString sessionTitleSuggestionFromAgentReply(const QJsonObject& reply)
{
    QString title = repairMojibakeText(reply.value(QStringLiteral("sessionTitle")).toString(
        reply.value(QStringLiteral("conversationTitle")).toString(
            reply.value(QStringLiteral("chatTitle")).toString()))).trimmed();
    if (title.isEmpty()) {
        const QJsonObject meta = reply.value(QStringLiteral("meta")).toObject();
        title = repairMojibakeText(meta.value(QStringLiteral("sessionTitle")).toString(
            meta.value(QStringLiteral("conversationTitle")).toString())).trimmed();
    }
    title = cleanedGeneralWorkflowHeading(title);
    title.remove(QRegularExpression(QStringLiteral(R"([.!?;:,]+$)")));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (title.size() > 64) {
        title = title.left(64).trimmed();
        const int lastSpace = title.lastIndexOf(QLatin1Char(' '));
        if (lastSpace > 24) {
            title = title.left(lastSpace).trimmed();
        }
    }
    if (title.isEmpty() || isGenericGeneralWorkflowName(title)) {
        return {};
    }
    return title;
}

QString descriptionFromGeneralWorkflowText(const QString& text, const QString& title)
{
    const QString normalizedTitle = cleanedGeneralWorkflowHeading(title);
    const QStringList paragraphs = sanitizedGeneralWorkflowSourceText(text).split(QRegularExpression(QStringLiteral("\\n\\s*\\n")));
    for (QString paragraph : paragraphs) {
        paragraph = repairMojibakeText(paragraph).trimmed();
        if (paragraph.isEmpty()) {
            continue;
        }
        const QString heading = cleanedGeneralWorkflowHeading(paragraph);
        if (!normalizedTitle.isEmpty() && heading == normalizedTitle) {
            continue;
        }
        paragraph.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        return paragraph.left(420).trimmed();
    }
    return normalizedTitle;
}

QJsonArray generalWorkflowBlocksFromMarkdown(QString text, const QString& title)
{
    text = sanitizedGeneralWorkflowSourceText(text);
    QJsonArray blocks;
    QString currentTitle = QStringLiteral("Inhalt");
    QStringList currentLines;
    QSet<QString> usedIds;
    bool sawHeading = false;
    const QString titleSlug = workflowSlug(title);
    const QStringList lines = text.split(QLatin1Char('\n'));

    auto flushBlock = [&]() {
        QString body = currentLines.join(QLatin1Char('\n')).trimmed();
        if (body.isEmpty() || isOnlyMarkdownRuleText(body)) {
            currentLines.clear();
            return;
        }
        QString blockTitle = cleanedGeneralWorkflowHeading(currentTitle);
        if (blockTitle.isEmpty()) {
            blockTitle = QStringLiteral("Inhalt");
        }
        QString id = workflowSlug(blockTitle);
        if (id.isEmpty() || id == QStringLiteral("workflow")) {
            id = QStringLiteral("abschnitt_%1").arg(blocks.size() + 1);
        }
        const QString baseId = id;
        int suffix = 2;
        while (usedIds.contains(id)) {
            id = QStringLiteral("%1_%2").arg(baseId).arg(suffix++);
        }
        usedIds.insert(id);
        blocks.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), blockTitle},
            {QStringLiteral("text"), normalizedGeneralWorkflowBlockText(body)},
        });
        currentLines.clear();
    };

    const QRegularExpression headingPattern(QStringLiteral("^\\s{0,3}#{1,6}\\s+(.+?)\\s*$"));
    const QRegularExpression boldHeadingPattern(QStringLiteral("^\\s*\\*\\*(.{3,90})\\*\\*\\s*$"));
    for (const QString& line : lines) {
        QRegularExpressionMatch headingMatch = headingPattern.match(line);
        if (!headingMatch.hasMatch()) {
            headingMatch = boldHeadingPattern.match(line);
        }
        if (headingMatch.hasMatch()) {
            const QString nextTitle = cleanedGeneralWorkflowHeading(headingMatch.captured(1));
            if (workflowSlug(nextTitle) == titleSlug && blocks.isEmpty() && currentLines.isEmpty()) {
                sawHeading = true;
                currentTitle = QStringLiteral("Inhalt");
                continue;
            }
            flushBlock();
            currentTitle = nextTitle.isEmpty() ? QStringLiteral("Abschnitt %1").arg(blocks.size() + 1) : nextTitle;
            sawHeading = true;
            continue;
        }
        currentLines << line;
    }
    flushBlock();

    if (blocks.isEmpty()) {
        blocks.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("inhalt")},
            {QStringLiteral("title"), sawHeading ? QStringLiteral("Inhalt") : QStringLiteral("Zusammenfassung")},
            {QStringLiteral("text"), normalizedGeneralWorkflowBlockText(text)},
        });
    }
    return blocks;
}

QJsonObject generalWorkflowFromAiText(const QString& content, const QJsonObject& saveContext)
{
    QString text = sanitizedGeneralWorkflowSourceText(content);
    if (text.trimmed().isEmpty()) {
        return {};
    }
    QString title = titleCandidateFromSaveContext(saveContext);
    const QString contentTitle = titleCandidateFromText(text);
    if (title.isEmpty() || isGenericGeneralWorkflowName(title)) {
        title = contentTitle;
    }
    if (title.isEmpty() || isGenericGeneralWorkflowName(title)) {
        title = QStringLiteral("Allgemeiner Chatkontext %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmm")));
    }

    QJsonObject workflow{
        {QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.v1")},
        {QStringLiteral("id"), workflowSlug(title)},
        {QStringLiteral("title"), title},
        {QStringLiteral("description"), descriptionFromGeneralWorkflowText(text, title)},
        {QStringLiteral("domain"), QStringLiteral("Allgemein")},
        {QStringLiteral("tags"), QJsonArray{QStringLiteral("Chat"), QStringLiteral("AI-Entwurf")}},
        {QStringLiteral("display"), QJsonObject{{QStringLiteral("blocks"), generalWorkflowBlocksFromMarkdown(text, title)}}},
        {QStringLiteral("verificationStatus"), QStringLiteral("AI-Entwurf")},
        {QStringLiteral("contextSummary"), descriptionFromGeneralWorkflowText(text, title)},
        {QStringLiteral("triggerExamples"), QJsonArray{title}},
    };

    const QJsonObject selectedWorkflow = saveContext.value(QStringLiteral("selectedWorkflow")).toObject();
    const QString selectedId = selectedWorkflow.value(QStringLiteral("id")).toString().trimmed();
    if (!selectedId.isEmpty() && !isGenericGeneralWorkflowName(selectedId)) {
        workflow.insert(QStringLiteral("id"), selectedId);
    }
    return normalizedGeneralWorkflowDraft(workflow);
}

QString generalWorkflowMarkdownCell(const QJsonValue& value)
{
    QString text;
    if (value.isString()) {
        text = repairMojibakeText(value.toString());
    } else if (value.isDouble()) {
        text = QString::number(value.toDouble(), 'g', 12);
    } else if (value.isBool()) {
        text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else if (value.isNull() || value.isUndefined()) {
        text = {};
    } else {
        text = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
        if (text == QStringLiteral("{}") && value.isArray()) {
            text = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
        }
    }
    text.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    text = text.trimmed();
    if (text.isEmpty()) {
        return QStringLiteral("-");
    }
    return text.isEmpty() ? QStringLiteral("Ã¢â‚¬â€") : text;
}

QString generalWorkflowTableMarkdown(const QJsonObject& table)
{
    const QJsonArray columns = table.value(QStringLiteral("columns")).toArray();
    const QJsonArray rows = table.value(QStringLiteral("rows")).toArray();
    if (columns.isEmpty() || rows.isEmpty()) {
        return {};
    }

    QStringList keys;
    QStringList headers;
    for (const QJsonValue& value : columns) {
        if (value.isString()) {
            const QString key = value.toString().trimmed();
            keys << key;
            headers << repairMojibakeText(key);
            continue;
        }
        const QJsonObject column = value.toObject();
        const QString key = column.value(QStringLiteral("key")).toString(
            column.value(QStringLiteral("name")).toString(column.value(QStringLiteral("label")).toString())).trimmed();
        keys << key;
        headers << repairMojibakeText(column.value(QStringLiteral("label")).toString(
            column.value(QStringLiteral("name")).toString(key))).trimmed();
    }
    if (headers.isEmpty()) {
        return {};
    }

    QStringList lines;
    const QString title = repairMojibakeText(table.value(QStringLiteral("title")).toString(table.value(QStringLiteral("name")).toString())).trimmed();
    if (!title.isEmpty()) {
        lines << QStringLiteral("**%1**").arg(title);
    }
    lines << QStringLiteral("| %1 |").arg(headers.join(QStringLiteral(" | ")));
    lines << QStringLiteral("| %1 |").arg(QStringList(headers.size(), QStringLiteral("---")).join(QStringLiteral(" | ")));
    for (const QJsonValue& rowValue : rows) {
        QStringList cells;
        if (rowValue.isArray()) {
            const QJsonArray row = rowValue.toArray();
            for (int i = 0; i < headers.size(); ++i) {
                cells << generalWorkflowMarkdownCell(i < row.size() ? row.at(i) : QJsonValue{});
            }
        } else {
            const QJsonObject row = rowValue.toObject();
            for (const QString& key : keys) {
                cells << generalWorkflowMarkdownCell(row.value(key));
            }
        }
        lines << QStringLiteral("| %1 |").arg(cells.join(QStringLiteral(" | ")));
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QString generalWorkflowTablesMarkdown(const QJsonArray& tables)
{
    QStringList blocks;
    for (const QJsonValue& value : tables) {
        const QString table = generalWorkflowTableMarkdown(value.toObject());
        if (!table.isEmpty()) {
            blocks << table;
        }
    }
    return blocks.join(QStringLiteral("\n\n")).trimmed();
}

QJsonValue objectValueByPath(const QJsonObject& object, const QStringList& path)
{
    QJsonValue current(object);
    for (const QString& segment : path) {
        if (!current.isObject()) {
            return {};
        }
        current = current.toObject().value(segment);
    }
    return current;
}

QString brxTableNumber(double value)
{
    QString text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

QString brxGeometryTablePointCell(const QJsonValue& value)
{
    if (value.isArray()) {
        const QJsonArray point = value.toArray();
        if (point.size() < 2) {
            return QStringLiteral("-");
        }
        return QStringLiteral("%1 / %2 / %3")
            .arg(brxTableNumber(point.at(0).toDouble()))
            .arg(brxTableNumber(point.at(1).toDouble()))
            .arg(brxTableNumber(point.size() >= 3 ? point.at(2).toDouble() : 0.0));
    }

    const QJsonObject point = value.toObject();
    if (point.isEmpty()) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1 / %2 / %3")
        .arg(brxTableNumber(point.value(QStringLiteral("x")).toDouble()))
        .arg(brxTableNumber(point.value(QStringLiteral("y")).toDouble()))
        .arg(brxTableNumber(point.value(QStringLiteral("z")).toDouble()));
}

QString brxGeometryTableMetricCell(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined()) {
        return QStringLiteral("-");
    }
    if (value.isDouble()) {
        return brxTableNumber(value.toDouble());
    }
    return generalWorkflowMarkdownCell(value);
}

QString brxGeometryTableScalarCell(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined()) {
        return QStringLiteral("-");
    }
    if (value.isDouble()) {
        return brxTableNumber(value.toDouble());
    }
    return generalWorkflowMarkdownCell(value);
}

QString brxGeometryObjectsMarkdownTable(const QString& title, const QJsonArray& objects)
{
    if (objects.isEmpty()) {
        return {};
    }

    struct Column {
        QString header;
        QStringList path;
    };
    QVector<Column> columns{
        {QStringLiteral("Handle"), {QStringLiteral("handle")}},
        {QStringLiteral("Typ"), {QStringLiteral("type")}},
        {QStringLiteral("Layer"), {QStringLiteral("layer")}},
        {QStringLiteral("Breite X"), {QStringLiteral("dimensions"), QStringLiteral("widthX")}},
        {QStringLiteral("Tiefe Y"), {QStringLiteral("dimensions"), QStringLiteral("depthY")}},
        {QStringLiteral("Hoehe Z"), {QStringLiteral("dimensions"), QStringLiteral("heightZ")}},
        {QStringLiteral("Laenge"), {QStringLiteral("metrics"), QStringLiteral("length")}},
        {QStringLiteral("Flaeche"), {QStringLiteral("metrics"), QStringLiteral("area")}},
        {QStringLiteral("Volumen"), {QStringLiteral("metrics"), QStringLiteral("volume")}},
        {QStringLiteral("Hinweise"), {QStringLiteral("error")}},
    };
    const bool hasBimData = std::any_of(objects.begin(), objects.end(), [](const QJsonValue& value) {
        return !value.toObject().value(QStringLiteral("bim")).toObject().isEmpty();
    });
    if (hasBimData) {
        columns.insert(3, {QStringLiteral("BIM Name"), {QStringLiteral("bim"), QStringLiteral("name")}});
        columns.insert(4, {QStringLiteral("BIM Typ"), {QStringLiteral("bim"), QStringLiteral("type")}});
        columns.insert(5, {QStringLiteral("Component Type"), {QStringLiteral("bim"), QStringLiteral("componentType")}});
        columns.insert(6, {QStringLiteral("Component Quelle"), {QStringLiteral("bim"), QStringLiteral("componentTypeSource")}});
        columns.insert(7, {QStringLiteral("Blockname"), {QStringLiteral("bim"), QStringLiteral("blockName")}});
        columns.insert(8, {QStringLiteral("GUID"), {QStringLiteral("bim"), QStringLiteral("guid")}});
    }

    QStringList headers;
    for (const Column& column : columns) {
        headers << column.header;
    }

    QStringList lines;
    const QString cleanTitle = repairMojibakeText(title).trimmed();
    if (!cleanTitle.isEmpty()) {
        lines << QStringLiteral("**%1**").arg(cleanTitle);
    }
    lines << QStringLiteral("| %1 |").arg(headers.join(QStringLiteral(" | ")));
    lines << QStringLiteral("| %1 |").arg(QStringList(headers.size(), QStringLiteral("---")).join(QStringLiteral(" | ")));

    const int maxRows = std::min(static_cast<int>(objects.size()), 100);
    for (int i = 0; i < maxRows; ++i) {
        const QJsonObject object = objects.at(i).toObject();
        QStringList cells;
        for (const Column& column : columns) {
            const QJsonValue value = objectValueByPath(object, column.path);
            if (column.path.size() == 2 && column.path.first() == QStringLiteral("bounds")) {
                cells << brxGeometryTablePointCell(value);
            } else if (column.path.size() == 2 && column.path.first() == QStringLiteral("metrics")) {
                cells << brxGeometryTableMetricCell(value);
            } else if (column.path.size() == 2 && column.path.first() == QStringLiteral("dimensions")) {
                cells << brxGeometryTableScalarCell(value);
            } else {
                cells << generalWorkflowMarkdownCell(value);
            }
        }
        lines << QStringLiteral("| %1 |").arg(cells.join(QStringLiteral(" | ")));
    }
    if (objects.size() > maxRows) {
        QStringList overflowCells(columns.size(), QStringLiteral(""));
        overflowCells[0] = QStringLiteral("...");
        if (overflowCells.size() > 1) overflowCells[1] = QStringLiteral("%1 weitere Objekte").arg(objects.size() - maxRows);
        lines << QStringLiteral("| %1 |").arg(overflowCells.join(QStringLiteral(" | ")));
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QJsonObject fallbackBimObjectTable(const QJsonArray& objects)
{
    QJsonArray rows;
    for (const QJsonValue& value : objects) {
        const QJsonObject object = value.toObject();
        const QJsonObject dimensions = object.value(QStringLiteral("dimensions")).toObject();
        rows.append(QJsonArray{
            object.value(QStringLiteral("handle")),
            object.value(QStringLiteral("name")),
            object.value(QStringLiteral("bimType")),
            object.value(QStringLiteral("componentType")),
            object.value(QStringLiteral("layer")),
            dimensions.value(QStringLiteral("widthX")),
            dimensions.value(QStringLiteral("depthY")),
            dimensions.value(QStringLiteral("heightZ")),
            object.value(QStringLiteral("guid")),
        });
    }
    return QJsonObject{
        {QStringLiteral("columns"), QJsonArray{
            QStringLiteral("Handle"),
            QStringLiteral("Name"),
            QStringLiteral("BIM-Typ"),
            QStringLiteral("Component Type"),
            QStringLiteral("Layer"),
            QStringLiteral("Breite X"),
            QStringLiteral("Tiefe Y"),
            QStringLiteral("Hoehe Z"),
            QStringLiteral("GUID"),
        }},
        {QStringLiteral("rows"), rows},
    };
}

QJsonObject fallbackBimPropertyTable(const QJsonArray& objects)
{
    QJsonArray rows;
    for (const QJsonValue& value : objects) {
        const QJsonObject object = value.toObject();
        for (const QJsonValue& propertyValue : object.value(QStringLiteral("properties")).toArray()) {
            const QJsonObject property = propertyValue.toObject();
            QJsonValue displayValue = property.value(QStringLiteral("formattedValue"));
            if (displayValue.toString().trimmed().isEmpty()) {
                displayValue = property.value(QStringLiteral("value"));
            }
            rows.append(QJsonArray{
                object.value(QStringLiteral("handle")),
                object.value(QStringLiteral("name")),
                property.value(QStringLiteral("qualifiedName")),
                property.value(QStringLiteral("dataType")),
                displayValue,
                property.value(QStringLiteral("unitType")),
                property.value(QStringLiteral("readOnly")),
            });
        }
    }
    return QJsonObject{
        {QStringLiteral("columns"), QJsonArray{
            QStringLiteral("Handle"),
            QStringLiteral("Name"),
            QStringLiteral("Eigenschaft"),
            QStringLiteral("Datentyp"),
            QStringLiteral("Wert"),
            QStringLiteral("Einheit"),
            QStringLiteral("Read-only"),
        }},
        {QStringLiteral("rows"), rows},
    };
}

QString brxBimQueryTablesMarkdown(const QString& title, const QJsonObject& result)
{
    const QJsonArray objects = result.value(QStringLiteral("objects")).toArray();
    QStringList blocks;

    QJsonObject objectTable = result.value(QStringLiteral("objectTable")).toObject();
    if (objectTable.value(QStringLiteral("rows")).toArray().isEmpty()) {
        objectTable = fallbackBimObjectTable(objects);
    }
    objectTable.insert(QStringLiteral("title"), title);
    const QString objectMarkdown = generalWorkflowTableMarkdown(objectTable);
    if (!objectMarkdown.isEmpty()) {
        blocks.append(objectMarkdown);
    }

    QJsonObject propertyTable = result.value(QStringLiteral("propertyTable")).toObject();
    if (propertyTable.value(QStringLiteral("rows")).toArray().isEmpty()) {
        propertyTable = fallbackBimPropertyTable(objects);
    }
    propertyTable.insert(QStringLiteral("title"), QStringLiteral("BIM-Eigenschaften"));
    const QString propertyMarkdown = generalWorkflowTableMarkdown(propertyTable);
    if (!propertyMarkdown.isEmpty()) {
        blocks.append(propertyMarkdown);
    }
    return blocks.join(QStringLiteral("\n\n")).trimmed();
}

QString brxGeometryResultTablesMarkdown(const QJsonArray& results)
{
    QStringList blocks;
    bool hasCreatedGeometryInspection = false;
    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.query")
            && item.value(QStringLiteral("params")).toObject().value(QStringLiteral("inspection")).toString()
                == QStringLiteral("createdGeometryBatch")
            && !item.value(QStringLiteral("result")).toObject().value(QStringLiteral("objects")).toArray().isEmpty()) {
            hasCreatedGeometryInspection = true;
            break;
        }
    }

    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        const QString tool = item.value(QStringLiteral("tool")).toString();
        if (tool != QStringLiteral("geometry.query")
            && tool != QStringLiteral("bim.objects.query")
            && tool != QStringLiteral("selection.describe")
            && tool != QStringLiteral("entity.describe")
            && tool != QStringLiteral("measurement.bbox")) {
            continue;
        }

        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        const bool createdGeometryInspection =
            tool == QStringLiteral("geometry.query")
            && item.value(QStringLiteral("params")).toObject().value(QStringLiteral("inspection")).toString()
                == QStringLiteral("createdGeometryBatch");
        if (hasCreatedGeometryInspection
            && !createdGeometryInspection
            && (tool == QStringLiteral("geometry.query")
                || tool == QStringLiteral("selection.describe")
                || tool == QStringLiteral("entity.describe")
                || tool == QStringLiteral("measurement.bbox"))) {
            continue;
        }

        const QJsonArray objects = result.value(QStringLiteral("objects")).toArray();
        if (objects.isEmpty()
            && result.value(QStringLiteral("objectTable")).toObject().value(QStringLiteral("rows")).toArray().isEmpty()) {
            continue;
        }

        const int index = item.value(QStringLiteral("index")).toInt(blocks.size() + 1);
        const QString title = createdGeometryInspection
            ? QStringLiteral("Erzeugte Geometrien geprueft (%1 Objekte)")
                .arg(result.value(QStringLiteral("count")).toInt(objects.size()))
            : QStringLiteral("%1 Ergebnis %2 (%3 Objekte)")
                .arg(tool)
                .arg(index)
                .arg(result.value(QStringLiteral("count")).toInt(objects.size()));
        if (tool == QStringLiteral("bim.objects.query")) {
            blocks << brxBimQueryTablesMarkdown(title, result);
        } else {
            blocks << brxGeometryObjectsMarkdownTable(title, objects);
        }
    }
    return blocks.join(QStringLiteral("\n\n")).trimmed();
}

QJsonObject compactGeometryObjectForAgent(const QJsonObject& object)
{
    QJsonObject compact;
    const QStringList keys{
        QStringLiteral("handle"),
        QStringLiteral("type"),
        QStringLiteral("kind"),
        QStringLiteral("shape"),
        QStringLiteral("layer"),
        QStringLiteral("name"),
        QStringLiteral("description"),
        QStringLiteral("guid"),
        QStringLiteral("bimType"),
        QStringLiteral("entityType"),
        QStringLiteral("componentType"),
        QStringLiteral("componentTypeSource"),
        QStringLiteral("blockName"),
        QStringLiteral("bim"),
        QStringLiteral("classification"),
        QStringLiteral("bounds"),
        QStringLiteral("dimensions"),
        QStringLiteral("metrics"),
        QStringLiteral("geometry"),
        QStringLiteral("ok"),
        QStringLiteral("success"),
        QStringLiteral("error"),
    };
    for (const QString& key : keys) {
        if (object.contains(key)) {
            compact.insert(key, object.value(key));
        }
    }
    const QJsonArray properties = object.value(QStringLiteral("properties")).toArray();
    if (!properties.isEmpty()) {
        QJsonArray limitedProperties;
        for (int i = 0; i < properties.size() && i < 200; ++i) {
            limitedProperties.append(properties.at(i));
        }
        compact.insert(QStringLiteral("properties"), limitedProperties);
        compact.insert(QStringLiteral("propertiesTruncated"), properties.size() > limitedProperties.size());
    }
    return compact;
}

QJsonArray compactGeometryObjectsForAgent(const QJsonArray& objects, int maxCount = 100)
{
    QJsonArray compact;
    const int count = std::min(static_cast<int>(objects.size()), maxCount);
    for (int i = 0; i < count; ++i) {
        compact.append(compactGeometryObjectForAgent(objects.at(i).toObject()));
    }
    if (objects.size() > maxCount) {
        compact.append(QJsonObject{
            {QStringLiteral("truncated"), true},
            {QStringLiteral("remaining"), objects.size() - maxCount},
        });
    }
    return compact;
}

QJsonObject compactBrxResponseForAgent(const QJsonObject& response)
{
    QJsonObject compact;
    const QStringList scalarKeys{
        QStringLiteral("schema"),
        QStringLiteral("ok"),
        QStringLiteral("message"),
        QStringLiteral("summary"),
        QStringLiteral("count"),
        QStringLiteral("total"),
        QStringLiteral("offset"),
        QStringLiteral("limit"),
        QStringLiteral("truncated"),
        QStringLiteral("selected"),
        QStringLiteral("created"),
        QStringLiteral("classified"),
        QStringLiteral("deleted"),
        QStringLiteral("moved"),
        QStringLiteral("copied"),
        QStringLiteral("rotated"),
        QStringLiteral("scaled"),
        QStringLiteral("saved"),
        QStringLiteral("measured"),
        QStringLiteral("failed"),
        QStringLiteral("units"),
        QStringLiteral("coordinateSystem"),
    };
    for (const QString& key : scalarKeys) {
        if (response.contains(key)) {
            compact.insert(key, response.value(key));
        }
    }

    const QStringList arrayKeys{
        QStringLiteral("handles"),
        QStringLiteral("createdHandles"),
        QStringLiteral("selectedHandles"),
        QStringLiteral("objectHandles"),
        QStringLiteral("affectedHandles"),
        QStringLiteral("resolvedHandles"),
        QStringLiteral("targetFingerprints"),
        QStringLiteral("failedTargets"),
        QStringLiteral("errors"),
        QStringLiteral("before"),
        QStringLiteral("after"),
    };
    for (const QString& key : arrayKeys) {
        const QJsonArray values = response.value(key).toArray();
        if (values.isEmpty()) {
            continue;
        }
        QJsonArray limited;
        for (int i = 0; i < values.size() && i < 120; ++i) {
            limited.append(values.at(i));
        }
        if (values.size() > 120) {
            limited.append(QStringLiteral("... %1 weitere").arg(values.size() - 120));
        }
        compact.insert(key, limited);
    }

    const QJsonArray layers = response.value(QStringLiteral("layers")).toArray();
    if (!layers.isEmpty()) {
        QJsonArray limitedLayers;
        for (int i = 0; i < layers.size() && i < 200; ++i) {
            limitedLayers.append(layers.at(i));
        }
        if (layers.size() > 200) {
            limitedLayers.append(QStringLiteral("... %1 weitere").arg(layers.size() - 200));
        }
        compact.insert(QStringLiteral("layers"), limitedLayers);
        compact.insert(QStringLiteral("layerCount"), layers.size());
    }

    const QJsonArray objects = response.value(QStringLiteral("objects")).toArray();
    if (!objects.isEmpty()) {
        compact.insert(QStringLiteral("objects"), compactGeometryObjectsForAgent(objects));
        compact.insert(QStringLiteral("objectCount"), response.value(QStringLiteral("count")).toInt(objects.size()));
    }
    if (response.contains(QStringLiteral("availability"))) {
        compact.insert(QStringLiteral("availability"), response.value(QStringLiteral("availability")));
    }
    if (response.contains(QStringLiteral("classifications"))) {
        compact.insert(QStringLiteral("classifications"), response.value(QStringLiteral("classifications")));
    }
    if (response.contains(QStringLiteral("bounds"))) {
        compact.insert(QStringLiteral("bounds"), response.value(QStringLiteral("bounds")));
    }
    if (response.contains(QStringLiteral("dimensions"))) {
        compact.insert(QStringLiteral("dimensions"), response.value(QStringLiteral("dimensions")));
    }
    return compact;
}

QJsonObject compactToolStepResultForAgent(const QJsonObject& item)
{
    QJsonObject compact;
    const QStringList keys{
        QStringLiteral("index"),
        QStringLiteral("tool"),
        QStringLiteral("bridgeMethod"),
        QStringLiteral("error"),
    };
    for (const QString& key : keys) {
        if (item.contains(key)) {
            compact.insert(key, item.value(key));
        }
    }
    if (item.contains(QStringLiteral("params"))) {
        compact.insert(QStringLiteral("params"), item.value(QStringLiteral("params")));
    }
    if (item.contains(QStringLiteral("result"))) {
        compact.insert(QStringLiteral("result"), compactBrxResponseForAgent(item.value(QStringLiteral("result")).toObject()));
    }
    if (item.contains(QStringLiteral("response"))) {
        const QJsonObject response = item.value(QStringLiteral("response")).toObject();
        QJsonObject compactResponse = compactBrxResponseForAgent(response);
        if (response.contains(QStringLiteral("result"))) {
            compactResponse.insert(QStringLiteral("result"), compactBrxResponseForAgent(response.value(QStringLiteral("result")).toObject()));
        }
        compact.insert(QStringLiteral("response"), compactResponse);
    }
    return compact;
}

QJsonArray compactToolStepResultsForAgent(const QJsonArray& results, int maxCount = 30)
{
    QJsonArray compact;
    const int count = std::min(static_cast<int>(results.size()), maxCount);
    for (int i = 0; i < count; ++i) {
        compact.append(compactToolStepResultForAgent(results.at(i).toObject()));
    }
    if (results.size() > maxCount) {
        compact.append(QJsonObject{
            {QStringLiteral("truncated"), true},
            {QStringLiteral("remaining"), results.size() - maxCount},
        });
    }
    return compact;
}

QJsonObject compactBatchResultForAgent(const QJsonObject& batchResult)
{
    QJsonObject compact;
    const QStringList keys{
        QStringLiteral("schema"),
        QStringLiteral("summary"),
        QStringLiteral("actionsRequested"),
        QStringLiteral("actionsCompleted"),
        QStringLiteral("failed"),
        QStringLiteral("executionStats"),
    };
    for (const QString& key : keys) {
        if (batchResult.contains(key)) {
            compact.insert(key, batchResult.value(key));
        }
    }
    if (batchResult.contains(QStringLiteral("results"))) {
        compact.insert(QStringLiteral("results"), compactToolStepResultsForAgent(batchResult.value(QStringLiteral("results")).toArray()));
    }
    const QString table = batchResult.value(QStringLiteral("geometryTablesMarkdown")).toString();
    if (!table.isEmpty()) {
        compact.insert(QStringLiteral("geometryTablesMarkdown"), table.left(12000));
        compact.insert(QStringLiteral("geometryTablesTruncated"), table.size() > 12000);
    }
    return compact;
}

QString generalWorkflowFormulasMarkdown(const QJsonArray& formulas)
{
    QStringList blocks;
    for (const QJsonValue& value : formulas) {
        const QJsonObject formula = value.toObject();
        QString name = formula.value(QStringLiteral("title")).toString().trimmed();
        if (name.isEmpty()) {
            name = formula.value(QStringLiteral("name")).toString().trimmed();
        }
        if (name.isEmpty()) {
            name = formula.value(QStringLiteral("label")).toString().trimmed();
        }
        if (name.isEmpty()) {
            name = formula.value(QStringLiteral("id")).toString().trimmed();
        }
        name = repairMojibakeText(name).trimmed();
        const QString description = repairMojibakeText(formula.value(QStringLiteral("description")).toString()).trimmed();
        const QString latex = latexFromGeneralWorkflowFormula(formula);
        if (!latex.isEmpty()) {
            QStringList lines;
            if (!name.isEmpty()) {
                lines << QStringLiteral("**%1**").arg(name);
            }
            lines << QStringLiteral("\\[%1\\]").arg(latex);
            if (!description.isEmpty()) {
                lines << description;
            }
            blocks << lines.join(QLatin1Char('\n')).trimmed();
        }
    }
    return blocks.join(QStringLiteral("\n\n")).trimmed();
}

QString generalWorkflowDraftMessageForChat(const QJsonObject& workflow)
{
    QStringList lines;
    const QString title = repairMojibakeText(workflow.value(QStringLiteral("title")).toString()).trimmed();
    const QString description = repairMojibakeText(workflow.value(QStringLiteral("description")).toString()).trimmed();
    lines << QStringLiteral("**Workflow-Entwurf:** %1").arg(title.isEmpty() ? QStringLiteral("Workflow") : title);
    if (!description.isEmpty()) {
        lines << QString() << description;
    }
    lines << QString() << QStringLiteral("**AbsÃƒÂ¤tze**");
    const QJsonArray blocks = generalWorkflowBlocks(workflow);
    for (int i = 0; i < blocks.size(); ++i) {
        const QJsonObject block = blocks.at(i).toObject();
        lines << QString() << QStringLiteral("**%1. %2**").arg(i + 1).arg(repairMojibakeText(block.value(QStringLiteral("title")).toString()).trimmed());
        lines << repairMojibakeText(block.value(QStringLiteral("text")).toString()).trimmed();
    }
    const QJsonArray formulas = workflow.value(QStringLiteral("formulas")).toArray();
    const QString formulasMarkdown = generalWorkflowFormulasMarkdown(formulas);
    if (!formulasMarkdown.isEmpty()) {
        lines << QString() << QStringLiteral("**Formeln**");
        lines << formulasMarkdown;
    }
    const QString tablesMarkdown = generalWorkflowTablesMarkdown(workflow.value(QStringLiteral("tables")).toArray());
    if (!tablesMarkdown.isEmpty()) {
        lines << QString() << QStringLiteral("**Tabellen**");
        lines << tablesMarkdown;
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QString bareboneProjectRootPath()
{
    const QString appDir = QApplication::applicationDirPath();
    const QStringList candidates{
        QDir::currentPath(),
        appDir,
        QDir(appDir).absoluteFilePath(QStringLiteral("..")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../..")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../../..")),
    };

    for (const QString& candidate : candidates) {
        QDir dir(candidate);
        if (dir.exists(QStringLiteral("agent"))
            && dir.exists(QStringLiteral("resources/resources.qrc"))) {
            const QString canonical = dir.canonicalPath();
            return canonical.isEmpty() ? dir.absolutePath() : canonical;
        }
    }

    return QDir::currentPath();
}

bool jsonContainsTemplatePlaceholder(const QJsonValue& value)
{
    if (value.isString()) {
        return value.toString().contains(QRegularExpression(QStringLiteral(R"(\{\{\s*[A-Za-z_][A-Za-z0-9_]*\s*\}\})")));
    }
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (jsonContainsTemplatePlaceholder(item)) {
                return true;
            }
        }
        return false;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (jsonContainsTemplatePlaceholder(it.value())) {
                return true;
            }
        }
    }
    return false;
}

void collectTemplatePlaceholders(const QJsonValue& value, QStringList& placeholders)
{
    if (value.isString()) {
        const QRegularExpression pattern(QStringLiteral(R"(\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\})"));
        QRegularExpressionMatchIterator it = pattern.globalMatch(value.toString());
        while (it.hasNext()) {
            const QString name = it.next().captured(1).trimmed();
            if (!name.isEmpty() && !placeholders.contains(name)) {
                placeholders << name;
            }
        }
        return;
    }
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            collectTemplatePlaceholders(item, placeholders);
        }
        return;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            collectTemplatePlaceholders(it.value(), placeholders);
        }
    }
}

QJsonValue workflowSlotValueByName(const QJsonObject& slotValues, const QString& key)
{
    if (slotValues.contains(key)) {
        return slotValues.value(key);
    }
    const QString canonical = canonicalWorkflowSlot(key);
    if (!canonical.isEmpty() && slotValues.contains(canonical)) {
        return slotValues.value(canonical);
    }
    for (auto it = slotValues.constBegin(); it != slotValues.constEnd(); ++it) {
        if (canonicalWorkflowSlot(it.key()) == canonical) {
            return it.value();
        }
    }
    return {};
}

QJsonValue workflowTemplateValue(const QJsonValue& value, const QJsonObject& slotValues)
{
    if (value.isString()) {
        QString text = value.toString();
        const QRegularExpression exact(QStringLiteral(R"(^\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}$)"));
        const QRegularExpressionMatch exactMatch = exact.match(text);
        if (exactMatch.hasMatch()) {
            const QString key = exactMatch.captured(1).trimmed();
            return workflowSlotValueByName(slotValues, key);
        }

        const QRegularExpression pattern(QStringLiteral(R"(\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\})"));
        QRegularExpressionMatchIterator it = pattern.globalMatch(text);
        while (it.hasNext()) {
            const QString key = it.next().captured(1).trimmed();
            text.replace(QRegularExpression(QStringLiteral(R"(\{\{\s*%1\s*\}\})").arg(QRegularExpression::escape(key))),
                workflowSlotValueByName(slotValues, key).toVariant().toString());
        }
        return text;
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue& item : value.toArray()) {
            array.append(workflowTemplateValue(item, slotValues));
        }
        return array;
    }
    if (value.isObject()) {
        QJsonObject object;
        const QJsonObject source = value.toObject();
        for (auto it = source.begin(); it != source.end(); ++it) {
            object.insert(it.key(), workflowTemplateValue(it.value(), slotValues));
        }
        return object;
    }
    return value;
}

bool workflowConditionAllowsStep(const QJsonValue& conditionValue, const QJsonObject& slotValues)
{
    if (conditionValue.isUndefined() || conditionValue.isNull()) {
        return true;
    }
    if (conditionValue.isBool()) {
        return conditionValue.toBool();
    }
    const QString condition = conditionValue.toString().trimmed();
    if (condition.isEmpty()) {
        return true;
    }

    const QRegularExpression boolComparison(
        QStringLiteral(R"(^\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}\s*(==|!=)\s*(true|false)$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = boolComparison.match(condition);
    if (match.hasMatch()) {
        const QString key = match.captured(1).trimmed();
        const bool expected = match.captured(3).compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
        const bool actual = slotValues.value(key).toBool(false);
        return match.captured(2) == QStringLiteral("==") ? actual == expected : actual != expected;
    }

    const QRegularExpression slotOnly(QStringLiteral(R"(^\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}$)"));
    const QRegularExpressionMatch slotMatch = slotOnly.match(condition);
    if (slotMatch.hasMatch()) {
        return slotValues.value(slotMatch.captured(1).trimmed()).toBool(false);
    }

    return true;
}

bool jsonObjectHasAnyKey(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        if (object.contains(key) && !object.value(key).isUndefined() && !object.value(key).isNull()) {
            return true;
        }
    }
    return false;
}

bool workflowTrainingToolRelevantForPrompt(const QString& toolName, const QString& normalizedPrompt, bool compactContext)
{
    if (!compactContext) {
        return true;
    }
    if (toolName == QStringLiteral("document.save")) {
        return false;
    }

    const bool mentionsLayer = textMentionsAny(normalizedPrompt, {
        QStringLiteral("layer"),
        QStringLiteral("ebene"),
        QStringLiteral("tga"),
    });
    const bool mentionsGeometry = textMentionsAny(normalizedPrompt, {
        QStringLiteral("grundriss"),
        QStringLiteral("raum"),
        QStringLiteral("wand"),
        QStringLiteral("rechteck"),
        QStringLiteral("solid"),
        QStringLiteral("solids"),
        QStringLiteral("geometrie"),
        QStringLiteral("extrud"),
        QStringLiteral("bim"),
        QStringLiteral("klassifiz"),
        QStringLiteral("selekt"),
        QStringLiteral("auswahl"),
        QStringLiteral("verschieb"),
        QStringLiteral("move"),
        QStringLiteral("rotier"),
        QStringLiteral("rotation"),
        QStringLiteral("rotate"),
        QStringLiteral("skalier"),
        QStringLiteral("scale"),
        QStringLiteral("query"),
        QStringLiteral("fetch"),
        QStringLiteral("daten"),
        QStringLiteral("abfrag"),
        QStringLiteral("ausles"),
        QStringLiteral("tabelle"),
        QStringLiteral("metriken"),
    });
    const bool mentionsGeometryQuery = textMentionsAny(normalizedPrompt, {
        QStringLiteral("geometry.query"),
        QStringLiteral("query"),
        QStringLiteral("fetch"),
        QStringLiteral("daten"),
        QStringLiteral("abfrag"),
        QStringLiteral("ausles"),
        QStringLiteral("tabelle"),
        QStringLiteral("metriken"),
        QStringLiteral("objekte"),
    });
    const bool mentionsMove = textMentionsAny(normalizedPrompt, {
        QStringLiteral("verschieb"),
        QStringLiteral("verschieben"),
        QStringLiteral("verschiebungsvektor"),
        QStringLiteral("vektor"),
        QStringLiteral("move"),
    });
    const bool mentionsBim = textMentionsAny(normalizedPrompt, {
        QStringLiteral("bim"),
        QStringLiteral("tuer"),
        QStringLiteral("tuer"),
        QStringLiteral("fenster"),
        QStringLiteral("wand"),
        QStringLiteral("waende"),
        QStringLiteral("waende"),
        QStringLiteral("stuetze"),
        QStringLiteral("stuetze"),
        QStringLiteral("traeger"),
        QStringLiteral("traeger"),
        QStringLiteral("decke"),
        QStringLiteral("door"),
        QStringLiteral("window"),
        QStringLiteral("wall"),
        QStringLiteral("column"),
        QStringLiteral("beam"),
        QStringLiteral("slab"),
    });
    const bool mentionsRotate = textMentionsAny(normalizedPrompt, {
        QStringLiteral("rotier"),
        QStringLiteral("rotation"),
        QStringLiteral("drehen"),
        QStringLiteral("rotate"),
    });
    const bool mentionsScale = textMentionsAny(normalizedPrompt, {
        QStringLiteral("skalier"),
        QStringLiteral("skalieren"),
        QStringLiteral("scale"),
    });

    if (mentionsLayer && (toolName.startsWith(QStringLiteral("layers."))
        || toolName == QStringLiteral("layers.ensureMany"))) {
        return true;
    }
    if (mentionsMove && ((mentionsBim && toolName == QStringLiteral("bim.move"))
            || (!mentionsBim && toolName == QStringLiteral("geometry.move")))) {
        return true;
    }
    if (mentionsRotate && toolName == QStringLiteral("geometry.rotate")) {
        return true;
    }
    if (mentionsScale && toolName == QStringLiteral("geometry.scale")) {
        return true;
    }
    if (mentionsGeometry) {
        return toolName == QStringLiteral("geometry.create")
            || toolName == QStringLiteral("geometry.query")
            || toolName == QStringLiteral("selection.describe")
            || toolName == QStringLiteral("entity.describe")
            || toolName == QStringLiteral("measurement.bbox")
            || toolName == QStringLiteral("measurement.length")
            || toolName == QStringLiteral("measurement.area")
            || toolName == QStringLiteral("geometry.move")
            || toolName == QStringLiteral("geometry.copy")
            || toolName == QStringLiteral("geometry.rotate")
            || toolName == QStringLiteral("geometry.scale")
            || toolName == QStringLiteral("rectangles.extrude")
            || toolName == QStringLiteral("circle.extrude")
            || toolName == QStringLiteral("profile.extrude")
            || toolName == QStringLiteral("bim.create")
            || toolName == QStringLiteral("bim.move")
            || toolName == QStringLiteral("bim.classify")
            || toolName == QStringLiteral("bim.objects.query")
            || toolName == QStringLiteral("bim.selection.set")
            || toolName == QStringLiteral("selection.set")
            || toolName == QStringLiteral("layers.ensureMany");
    }
    if (mentionsGeometryQuery) {
        return toolName == QStringLiteral("geometry.query")
            || toolName == QStringLiteral("bim.objects.query")
            || toolName == QStringLiteral("selection.describe")
            || toolName == QStringLiteral("entity.describe")
            || toolName == QStringLiteral("measurement.bbox")
            || toolName == QStringLiteral("bim.selection.set")
            || toolName == QStringLiteral("selection.set");
    }

    return toolName == QStringLiteral("geometry.create")
        || toolName == QStringLiteral("geometry.query")
        || toolName == QStringLiteral("rectangles.extrude")
        || toolName == QStringLiteral("circle.extrude")
        || toolName == QStringLiteral("profile.extrude")
        || toolName == QStringLiteral("bim.classify")
        || toolName == QStringLiteral("bim.objects.query")
        || toolName == QStringLiteral("bim.selection.set")
        || toolName == QStringLiteral("selection.set");
}

bool textProvidesMissingField(const QString& prompt, const QString& missingField, const QJsonObject& draft)
{
    const QString normalized = prompt.toLower();
    const QString field = missingField.toLower();
    const QJsonObject draftParams = draft.value("params").toObject();
    const QString draftGeometry = draftParams.value("geometry").toString().toLower();

    if (field == "heightmm") {
        double value = 0.0;
        return extractPromptNumberMm(prompt, &value);
    }
    if (field == "height" || field == "z") {
        if (textHasKeyedNumber(prompt, {"height", "hoehe", "hohe", "h", "z"})) {
            return true;
        }
        if (draftGeometry == "rectangle" && textHasKeyedNumber(prompt, {"y", "b", "depth", "tiefe", "length", "laenge", "lange"})) {
            return true;
        }
    }
    if (field == "width" || field == "x") {
        return textHasKeyedNumber(prompt, {"width", "breite", "x", "a"});
    }
    if (field == "depth" || field == "length" || field == "y") {
        return textHasKeyedNumber(prompt, {"depth", "tiefe", "length", "laenge", "lange", "y", "b"});
    }
    if (field == "radius") {
        return textHasKeyedNumber(prompt, {"radius", "r"});
    }
    if (field == "distance"
        || field == "distancemm"
        || field == "amount"
        || field == "amountmm"
        || field == "offset"
        || field == "offsetmm"
        || field == "delta"
        || field == "deltamm"
        || field == "lengthdelta"
        || field == "extendby"
        || field == "extension"
        || field == "extensionmm"
        || field == "movedistance"
        || field == "movedistancemm"
        || field == "vector"
        || field == "translation") {
        double value = 0.0;
        return extractPromptNumberMm(prompt, &value)
            || textHasKeyedNumber(prompt, {
                "distance", "distanz", "abstand", "amount", "offset", "versatz",
                "delta", "verschiebung", "verschieben", "move", "extend", "verlaengern", "verlÃƒÂ¤ngern"});
    }
    if (field == "direction" || field == "axis") {
        return textMentionsAny(normalized, {
            "x-richtung", "x richtung", "+x", "-x", "in x", "nach x",
            "y-richtung", "y richtung", "+y", "-y", "in y", "nach y",
            "z-richtung", "z richtung", "+z", "-z", "in z", "nach z",
            "rechts", "links", "oben", "unten"});
    }
    if (field == "face" || field == "subentity" || field == "subent" || field == "surface") {
        return textMentionsAny(normalized, {"face", "flaeche", "flÃƒÂ¤che", "seite", "stirnseite"});
    }
    if (field == "origin" || field == "center" || field == "centerpoint") {
        return normalized.contains("ursprung")
            || normalized.contains("origin")
            || normalized.contains("mittelpunkt")
            || normalized.contains("center")
            || QRegularExpression(QStringLiteral(R"(-?\d+(?:[,.]\d+)?\s*[,/]\s*-?\d+(?:[,.]\d+)?)")).match(prompt).hasMatch();
    }
    if (field == "selector") {
        return normalized.contains("auswahl")
            || normalized.contains("ausgewaehlt")
            || normalized.contains("ausgewahlt")
            || normalized.contains("selection")
            || normalized.contains("selected")
            || normalized.contains("handle");
    }
    if (field == "layer" || field == "layername" || field == "layer_name") {
        const QRegularExpression explicitLayer(
            QStringLiteral(R"((?:layer|ebene)\s*(?:name)?\s*(?:(?:[:=]\s*)?[\"'][^\"'\r\n]+[\"']|[:=]\s*[^,;.\r\n]+))"),
            QRegularExpression::CaseInsensitiveOption);
        return explicitLayer.match(prompt).hasMatch();
    }

    return false;
}

QStringList providedMissingFields(const QString& prompt, const QJsonArray& missing, const QJsonObject& draft)
{
    QStringList fields;
    for (const QJsonValue& value : missing) {
        const QString field = value.toString().trimmed();
        if (!field.isEmpty() && textProvidesMissingField(prompt, field, draft)) {
            fields << field;
        }
    }
    return fields;
}

QString canonicalMissingField(QString field)
{
    field = repairMojibakeText(field).trimmed().toLower();
    field.remove(QRegularExpression(QStringLiteral("[^a-z0-9ÃƒÂ¤ÃƒÂ¶ÃƒÂ¼ÃƒÅ¸]")));
    if (field == QStringLiteral("height")
        || field == QStringLiteral("heightmm")
        || field == QStringLiteral("hoehe")
        || field == QStringLiteral("hÃƒÂ¶he")
        || field == QStringLiteral("h")
        || field == QStringLiteral("z")) {
        return QStringLiteral("height");
    }
    if (field == QStringLiteral("width")
        || field == QStringLiteral("breite")
        || field == QStringLiteral("w")
        || field == QStringLiteral("x")) {
        return QStringLiteral("width");
    }
    if (field == QStringLiteral("depth")
        || field == QStringLiteral("length")
        || field == QStringLiteral("tiefe")
        || field == QStringLiteral("laenge")
        || field == QStringLiteral("lÃƒÂ¤nge")
        || field == QStringLiteral("y")) {
        return QStringLiteral("depth");
    }
    if (field == QStringLiteral("distance")
        || field == QStringLiteral("distancemm")
        || field == QStringLiteral("offset")
        || field == QStringLiteral("offsetmm")
        || field == QStringLiteral("delta")
        || field == QStringLiteral("deltamm")
        || field == QStringLiteral("amount")
        || field == QStringLiteral("amountmm")) {
        return QStringLiteral("distance");
    }
    if (field == QStringLiteral("direction") || field == QStringLiteral("axis")) {
        return QStringLiteral("direction");
    }
    if (field == QStringLiteral("face")
        || field == QStringLiteral("surface")
        || field == QStringLiteral("subentity")
        || field == QStringLiteral("subent")) {
        return QStringLiteral("face");
    }
    return field;
}

bool missingContainsEquivalentField(const QJsonArray& missing, const QString& inferredField)
{
    const QString inferred = canonicalMissingField(inferredField);
    if (inferred.isEmpty()) {
        return false;
    }
    for (const QJsonValue& value : missing) {
        if (canonicalMissingField(value.toString()) == inferred) {
            return true;
        }
    }
    return false;
}

QStringList inferredProvidedFieldsFromAskMessage(const QString& prompt, const QString& askMessage, const QJsonObject& draft)
{
    const QString normalized = repairMojibakeText(askMessage).toLower();
    QStringList fields;

    if (textMentionsAny(normalized, {"wie viel", "wieviel", "distanz", "abstand", "offset", "versatz", "verschieb", "verlaeng", "verlÃƒÂ¤ng", "mm"})
        && textProvidesMissingField(prompt, "distance", draft)) {
        fields << "distance";
    }
    if (textMentionsAny(normalized, {"hoehe", "hÃƒÂ¶he", "height", "hoch", "z"})
        && textProvidesMissingField(prompt, "heightMm", draft)) {
        fields << "heightMm";
    }
    if (textMentionsAny(normalized, {"richtung", "achse", "axis", "direction"})
        && textProvidesMissingField(prompt, "direction", draft)) {
        fields << "direction";
    }
    if (textMentionsAny(normalized, {"face", "flaeche", "flÃƒÂ¤che", "seite", "stirnseite"})
        && textProvidesMissingField(prompt, "face", draft)) {
        fields << "face";
    }

    fields.removeDuplicates();
    return fields;
}

QString generateBridgeToken()
{
    return QString("%1%2")
        .arg(static_cast<qulonglong>(QRandomGenerator::global()->generate64()), 16, 16, QLatin1Char('0'))
        .arg(static_cast<qulonglong>(QRandomGenerator::global()->generate64()), 16, 16, QLatin1Char('0'));
}

QByteArray toJsonLine(const QJsonObject& message)
{
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

QString bridgeErrorMessage(const QJsonObject& response, const QString& fallback)
{
    const QString error = response.value("error").toString();
    if (!error.isEmpty()) return error;
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    if (result.contains(QStringLiteral("found")) || result.contains(QStringLiteral("errors"))) {
        QString message = QStringLiteral("%1: found=%2, classified=%3, errors=%4")
            .arg(fallback)
            .arg(result.value(QStringLiteral("found")).toInt())
            .arg(result.value(QStringLiteral("classified")).toInt())
            .arg(result.value(QStringLiteral("errors")).toInt());
        const QJsonArray debug = response.value(QStringLiteral("debug")).toArray();
        if (!debug.isEmpty()) message += QStringLiteral("; %1").arg(debug.last().toString());
        return message;
    }
    const QJsonArray debug = response.value(QStringLiteral("debug")).toArray();
    if (!debug.isEmpty()) return QStringLiteral("%1: %2").arg(fallback, debug.last().toString());
    return fallback;
}

QStringList stringsFromJsonArray(const QJsonArray& values)
{
    QStringList result;
    for (const QJsonValue& value : values) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty() && !result.contains(text)) {
            result << text;
        }
    }
    return result;
}

QJsonObject firstRepairActionFromObject(const QJsonObject& object)
{
    QJsonObject proposal = object.value(QStringLiteral("proposal")).toObject();
    if (proposal.isEmpty()) {
        proposal = object;
    }

    const QJsonObject failedAction = proposal.value(QStringLiteral("failedAction")).toObject();
    if (!failedAction.value(QStringLiteral("tool")).toString().trimmed().isEmpty()) {
        return failedAction;
    }

    const QJsonArray actions = proposal.value(QStringLiteral("actions")).toArray();
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        if (!action.value(QStringLiteral("tool")).toString().trimmed().isEmpty()) {
            return action;
        }
    }

    const QString tool = proposal.value(QStringLiteral("tool")).toString().trimmed();
    if (!tool.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("tool"), tool},
            {QStringLiteral("params"), proposal.value(QStringLiteral("params")).toObject()},
        };
    }
    return {};
}

QJsonArray mergeToolArraysByName(QJsonArray base, const QJsonArray& extra)
{
    QSet<QString> known;
    for (const QJsonValue& value : base) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            known.insert(name);
        }
    }

    for (const QJsonValue& value : extra) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || known.contains(name)) {
            continue;
        }
        known.insert(name);
        base.append(tool);
    }
    return base;
}

bool isWorkflowTrainingReviewConfirmationText(const QString& prompt)
{
    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    return normalized == QStringLiteral("ja")
        || normalized == QStringLiteral("ok")
        || normalized == QStringLiteral("passt")
        || normalized == QStringLiteral("passt so")
        || normalized == QStringLiteral("korrekt")
        || normalized == QStringLiteral("ist korrekt")
        || normalized == QStringLiteral("so speichern")
        || normalized == QStringLiteral("speichern")
        || normalized == QStringLiteral("workflow speichern")
        || normalized == QStringLiteral("bestaetigen")
        || normalized == QStringLiteral("bestÃƒÂ¤tigen")
        || normalized == QStringLiteral("freigeben")
        || normalized == QStringLiteral("review bestaetigt")
        || normalized == QStringLiteral("review bestÃƒÂ¤tigt");
}

bool isWorkflowTrainingExplicitSaveText(const QString& prompt)
{
    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    return normalized == QStringLiteral("speichern")
        || normalized == QStringLiteral("so speichern")
        || normalized == QStringLiteral("workflow speichern")
        || normalized == QStringLiteral("entwurf speichern")
        || normalized == QStringLiteral("workflow jetzt speichern");
}

bool isGeneralWorkflowDeleteText(const QString& prompt)
{
    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    return normalized == QStringLiteral("workflow loeschen")
        || normalized == QStringLiteral("workflow lÃƒÂ¶schen")
        || normalized == QStringLiteral("workflow entfernen")
        || normalized == QStringLiteral("planungsthema loeschen")
        || normalized == QStringLiteral("planungsthema lÃƒÂ¶schen")
        || normalized == QStringLiteral("planungsthema entfernen")
        || normalized == QStringLiteral("berechnung loeschen")
        || normalized == QStringLiteral("berechnung lÃƒÂ¶schen")
        || normalized == QStringLiteral("berechnung entfernen");
}

bool workflowHasSaveableTrainingContent(const QJsonObject& workflow)
{
    return workflow.value("steps").isArray()
        && !workflow.value("steps").toArray().isEmpty()
        && workflow.value("validationExamples").isArray()
        && !workflow.value("validationExamples").toArray().isEmpty();
}

bool workflowHasExecutableTrainingContent(const QJsonObject& workflow)
{
    if (workflow.value("steps").isArray() && !workflow.value("steps").toArray().isEmpty()) {
        return true;
    }
    const QJsonArray batches = workflow.value("executionBatches").toArray();
    for (const QJsonValue& value : batches) {
        if (!value.toObject().value("steps").toArray().isEmpty()) {
            return true;
        }
    }
    return false;
}

bool workflowMatchesSelectedWorkflow(const QJsonObject& workflow, const QJsonObject& selectedWorkflow, const QString& selectedWorkflowId)
{
    if (selectedWorkflow.isEmpty()) {
        return false;
    }
    const QString workflowId = workflowSlug(workflow.value("id").toString());
    const QString selectedId = workflowSlug(selectedWorkflow.value("id").toString(selectedWorkflowId));
    return !workflowId.isEmpty()
        && workflowId != QStringLiteral("workflow")
        && workflowId == selectedId;
}

bool isWorkflowTrainingRunText(const QString& prompt)
{
    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    return normalized == QStringLiteral("workflow ausfuehren")
        || normalized == QStringLiteral("workflow ausfÃƒÂ¼hren")
        || normalized == QStringLiteral("workflow starten")
        || normalized == QStringLiteral("workflow testen")
        || normalized == QStringLiteral("ausfuehren")
        || normalized == QStringLiteral("ausfÃƒÂ¼hren")
        || normalized == QStringLiteral("starten")
        || normalized == QStringLiteral("testen");
}

QString workflowJsonValueSummary(const QJsonValue& value)
{
    if (value.isString()) {
        return repairMojibakeText(value.toString()).trimmed();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 12);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return {};
}

QStringList workflowObjectArraySummaries(const QJsonArray& values, int limit)
{
    QStringList result;
    for (const QJsonValue& value : values) {
        if (result.size() >= limit) {
            break;
        }
        if (value.isString()) {
            QString normalized = repairMojibakeText(value.toString());
            normalized.replace(QStringLiteral("\\r\\n"), QStringLiteral("\n"));
            normalized.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            normalized.replace(QStringLiteral("\\t"), QStringLiteral(" "));
            normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            normalized.replace(QRegularExpression(QStringLiteral(R"(\s+(\d+[\.\)]\s+))")), QStringLiteral("\n\\1"));
            normalized.replace(QRegularExpression(QStringLiteral(R"(\s+([-*]\s+))")), QStringLiteral("\n\\1"));
            const QStringList parts = normalized.split('\n');
            for (QString text : parts) {
                text = text.trimmed();
                text.remove(QRegularExpression(QStringLiteral(R"(^\d+[\.\)]\s*)")));
                while (text.startsWith('-') || text.startsWith('*')) {
                    text = text.mid(1).trimmed();
                }
                if (!text.isEmpty()
                    && text != QStringLiteral("-")
                    && text != QStringLiteral("Ã¢â‚¬â€œ")) {
                    result << text;
                    if (result.size() >= limit) {
                        break;
                    }
                }
            }
            continue;
        }

        const QJsonObject object = value.toObject();
        QString text = repairMojibakeText(object.value("title").toString()).trimmed();
        if (text.isEmpty()) {
            text = repairMojibakeText(object.value("name").toString()).trimmed();
        }
        if (text.isEmpty()) {
            text = repairMojibakeText(object.value("id").toString()).trimmed();
        }
        if (!text.isEmpty()) {
            result << text;
        }
    }
    return result;
}

QString compactWorkflowJsonForChat(const QJsonObject& object, int maxChars = 520)
{
    if (object.isEmpty()) {
        return {};
    }
    QString json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    json = repairMojibakeText(json).trimmed();
    if (json.size() > maxChars) {
        json = json.left(maxChars - 3) + QStringLiteral("...");
    }
    return json;
}

QJsonArray workflowDisplaySteps(const QJsonObject& draft)
{
    QJsonArray steps;
    const QJsonArray batches = draft.value("executionBatches").toArray();
    for (const QJsonValue& batchValue : batches) {
        const QJsonObject batch = batchValue.toObject();
        const QString batchTitle = repairMojibakeText(batch.value("title").toString(batch.value("id").toString())).trimmed();
        const QJsonArray batchSteps = batch.value("steps").toArray();
        for (const QJsonValue& stepValue : batchSteps) {
            QJsonObject step = stepValue.toObject();
            if (!batchTitle.isEmpty() && !step.contains(QStringLiteral("batchTitle"))) {
                step.insert(QStringLiteral("batchTitle"), batchTitle);
            }
            steps.append(step);
        }
    }
    if (steps.isEmpty()) {
        steps = draft.value("steps").toArray();
    }
    return steps;
}

QString workflowStepDisplayLine(const QJsonObject& step, int number)
{
    const QString title = repairMojibakeText(step.value("title").toString(step.value("id").toString())).trimmed();
    const QString tool = repairMojibakeText(step.value("tool").toString()).trimmed();
    QJsonObject params = step.value("paramsTemplate").toObject();
    QString paramsLabel = QStringLiteral("paramsTemplate");
    if (params.isEmpty()) {
        params = step.value("params").toObject();
        paramsLabel = QStringLiteral("params");
    }

    QString line = QStringLiteral("%1. ").arg(number);
    if (!title.isEmpty()) {
        line += title + QStringLiteral(": ");
    }
    line += tool.isEmpty() ? QStringLiteral("<tool fehlt>") : tool;
    const QString paramsJson = compactWorkflowJsonForChat(params);
    if (!paramsJson.isEmpty()) {
        line += QStringLiteral(" mit `%1=%2`").arg(paramsLabel, paramsJson);
    }
    const QString layer = repairMojibakeText(params.value("layer").toString()).trimmed();
    if (!layer.isEmpty()) {
        line += QStringLiteral(" (Layer wird direkt als `layer=\"%1\"` an %2 gesendet)")
            .arg(layer, tool.isEmpty() ? QStringLiteral("das Tool") : tool);
    }
    return line;
}

QString workflowValidationContextForStep(const QJsonObject& workflow, const QJsonObject& step)
{
    QStringList lines;
    const QString title = repairMojibakeText(workflow.value("title").toString()).trimmed();
    const QString description = repairMojibakeText(workflow.value("description").toString()).trimmed();
    const QString stepTitle = repairMojibakeText(step.value("title").toString(step.value("id").toString())).trimmed();
    const QString tool = repairMojibakeText(step.value("tool").toString()).trimmed();
    if (!title.isEmpty()) {
        lines << title;
    }
    if (!description.isEmpty()) {
        lines << description;
    }
    if (!stepTitle.isEmpty()) {
        lines << stepTitle;
    }
    if (!tool.isEmpty()) {
        lines << QStringLiteral("Tool: %1").arg(tool);
    }
    const QJsonObject params = step.value("paramsTemplate").toObject();
    const QString classification = params.value("classification").toString().trimmed();
    const QString target = params.value("target").toString().trimmed();
    if (!classification.isEmpty()) {
        lines << QStringLiteral("classification=%1").arg(classification);
    }
    if (!target.isEmpty()) {
        lines << QStringLiteral("target=%1").arg(target);
    }
    const QStringList strategy = workflowObjectArraySummaries(workflow.value("constructionStrategy").toArray(), 6);
    for (const QString& item : strategy) {
        lines << item;
    }
    return lines.join(QLatin1Char('\n')).left(2200);
}

QString publicWorkflowSummaryText(QString text)
{
    text = repairMojibakeText(text).trimmed();
    if (text.isEmpty()) {
        return {};
    }
    text.replace(QRegularExpression(QStringLiteral(R"(^\s*\*{0,2}Workflow-Entwurf:\*{0,2}[^\n]*(?:\n{1,2})?)"),
        QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral(R"(^\s*Workflow-Entwurf:[^\n]*(?:\n{1,2})?)"),
        QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral(R"(^\s*(Workflow speichern|Titelvorschlag|Speichere|Wandle|Kombiniere|Nutze selectedMessageText|Erstelle title selbst)\b[^\n]*(?:\n{1,2})?)"),
        QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.trimmed();
}

QString workflowCompactSummaryForSelector(const QJsonObject& workflow)
{
    QString summary = repairMojibakeText(workflow.value("compactSummary").toString()).trimmed();
    if (summary.isEmpty()) {
        summary = repairMojibakeText(workflow.value("description").toString()).trimmed();
    }
    if (summary.isEmpty()) {
        summary = repairMojibakeText(workflow.value("contextSummary").toString()).trimmed();
    }
    if (summary.isEmpty()) {
        const QJsonArray blocks = generalWorkflowBlocks(workflow);
        for (const QJsonValue& value : blocks) {
            const QString text = repairMojibakeText(value.toObject().value(QStringLiteral("text")).toString()).trimmed();
            if (!text.isEmpty()) {
                summary = text;
                break;
            }
        }
    }
    if (summary.isEmpty()) {
        const QStringList strategy = workflowObjectArraySummaries(workflow.value("constructionStrategy").toArray(), 2);
        summary = strategy.join(QStringLiteral(" "));
    }
    if (summary.isEmpty()) {
        const QStringList triggers = workflowObjectArraySummaries(workflow.value("triggerExamples").toArray(), 2);
        summary = triggers.join(QStringLiteral(" "));
    }
    if (summary.isEmpty()) {
        summary = repairMojibakeText(workflow.value("title").toString()).trimmed();
    }
    const QString publicSummary = publicWorkflowSummaryText(summary);
    return (publicSummary.isEmpty() ? summary : publicSummary).left(420);
}

QStringList workflowToolNamesForSelector(const QJsonObject& workflow, int maxCount = 12)
{
    QStringList tools;
    for (const QJsonValue& value : workflow.value("recommendedTools").toArray()) {
        const QString tool = value.toString().trimmed();
        if (!tool.isEmpty() && !tools.contains(tool)) {
            tools << tool;
        }
        if (tools.size() >= maxCount) {
            return tools;
        }
    }
    for (const QJsonValue& value : workflow.value("preferredTools").toArray()) {
        const QString tool = value.toString().trimmed();
        if (!tool.isEmpty() && !tools.contains(tool)) {
            tools << tool;
        }
        if (tools.size() >= maxCount) {
            return tools;
        }
    }

    const QJsonArray steps = workflowDisplaySteps(workflow);
    for (const QJsonValue& value : steps) {
        const QString tool = value.toObject().value("tool").toString().trimmed();
        if (!tool.isEmpty() && !tools.contains(tool)) {
            tools << tool;
        }
        if (tools.size() >= maxCount) {
            break;
        }
    }
    return tools;
}

QStringList workflowSlotNamesForSelector(const QJsonObject& workflow, int maxCount = 12)
{
    QStringList slots;
    auto appendSlot = [&](const QString& slot) {
        const QString trimmed = slot.trimmed();
        if (!trimmed.isEmpty() && !slots.contains(trimmed) && slots.size() < maxCount) {
            slots << trimmed;
        }
    };

    for (const QJsonValue& value : workflow.value("requiredSlots").toArray()) {
        appendSlot(workflowSlotNameFromValue(value));
    }
    const QJsonObject optionalSlots = workflow.value("optionalSlots").toObject();
    for (auto it = optionalSlots.constBegin(); it != optionalSlots.constEnd() && slots.size() < maxCount; ++it) {
        appendSlot(it.key());
    }
    for (const QJsonValue& value : workflow.value("derivedValues").toArray()) {
        appendSlot(value.toObject().value("name").toString());
    }
    return slots;
}

QStringList workflowKeywordsForSelector(const QJsonObject& workflow, int maxCount = 24)
{
    QStringList keywords;
    auto appendKeyword = [&](QString text) {
        text = workflowTrainingSearchText(repairMojibakeText(text)).trimmed();
        text.replace(QRegularExpression(QStringLiteral(R"([^a-z0-9_\.]+)")), QStringLiteral(" "));
        const QStringList parts = text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            if (part.size() < 3) {
                continue;
            }
            if (part == QStringLiteral("und")
                || part == QStringLiteral("oder")
                || part == QStringLiteral("der")
                || part == QStringLiteral("die")
                || part == QStringLiteral("das")
                || part == QStringLiteral("mit")
                || part == QStringLiteral("fuer")
                || part == QStringLiteral("fÃƒÂ¼r")
                || part == QStringLiteral("eine")
                || part == QStringLiteral("einen")) {
                continue;
            }
            if (!keywords.contains(part)) {
                keywords << part;
                if (keywords.size() >= maxCount) {
                    return;
                }
            }
        }
    };

    appendKeyword(workflow.value(QStringLiteral("id")).toString());
    appendKeyword(workflow.value(QStringLiteral("title")).toString());
    appendKeyword(workflow.value(QStringLiteral("description")).toString());
    appendKeyword(workflowCompactSummaryForSelector(workflow));
    for (const QJsonValue& value : workflow.value(QStringLiteral("triggerExamples")).toArray()) {
        appendKeyword(value.toString());
        if (keywords.size() >= maxCount) {
            return keywords;
        }
    }
    for (const QString& tool : workflowToolNamesForSelector(workflow, 16)) {
        appendKeyword(tool);
        if (keywords.size() >= maxCount) {
            return keywords;
        }
    }
    for (const QString& slot : workflowSlotNamesForSelector(workflow, 16)) {
        appendKeyword(slot);
        if (keywords.size() >= maxCount) {
            return keywords;
        }
    }
    const QJsonArray steps = workflowDisplaySteps(workflow);
    for (const QJsonValue& value : steps) {
        const QJsonObject step = value.toObject();
        appendKeyword(step.value(QStringLiteral("title")).toString(step.value(QStringLiteral("id")).toString()));
        appendKeyword(step.value(QStringLiteral("tool")).toString());
        if (keywords.size() >= maxCount) {
            return keywords;
        }
    }
    return keywords;
}

QJsonArray workflowStepSummaryForSelector(const QJsonObject& workflow, int maxCount = 8)
{
    QJsonArray summaries;
    const QJsonArray steps = workflowDisplaySteps(workflow);
    for (int i = 0; i < steps.size() && i < maxCount; ++i) {
        const QJsonObject step = steps.at(i).toObject();
        const QString tool = step.value(QStringLiteral("tool")).toString().trimmed();
        const QString title = repairMojibakeText(step.value(QStringLiteral("title")).toString(step.value(QStringLiteral("id")).toString())).trimmed();
        summaries.append(QStringLiteral("%1. %2%3%4")
            .arg(i + 1)
            .arg(tool.isEmpty() ? QStringLiteral("<tool fehlt>") : tool)
            .arg(title.isEmpty() ? QString() : QStringLiteral(" - "))
            .arg(title.left(120)));
    }
    return summaries;
}

QJsonObject workflowStepCapsule(const QJsonObject& step, bool detailed)
{
    QJsonObject capsule{
        {"id", step.value(QStringLiteral("id")).toString()},
        {"title", repairMojibakeText(step.value(QStringLiteral("title")).toString(step.value(QStringLiteral("id")).toString())).left(160)},
        {"tool", step.value(QStringLiteral("tool")).toString()},
    };
    if (step.contains(QStringLiteral("condition"))) {
        capsule.insert(QStringLiteral("condition"), step.value(QStringLiteral("condition")));
    }
    if (step.contains(QStringLiteral("requiresSlots"))) {
        capsule.insert(QStringLiteral("requiresSlots"), step.value(QStringLiteral("requiresSlots")).toArray());
    }
    if (detailed) {
        QJsonObject params = step.value(QStringLiteral("paramsTemplate")).toObject();
        if (params.isEmpty()) {
            params = step.value(QStringLiteral("params")).toObject();
        }
        capsule.insert(QStringLiteral("paramsTemplate"), params);
    }
    return capsule;
}

QJsonObject workflowCapsuleForAgent(const QJsonObject& workflow, bool detailed)
{
    QJsonObject capsule{
        {"id", workflow.value(QStringLiteral("id")).toString()},
        {"title", repairMojibakeText(workflow.value(QStringLiteral("title")).toString()).left(180)},
        {"description", repairMojibakeText(workflow.value(QStringLiteral("description")).toString(workflowCompactSummaryForSelector(workflow))).left(detailed ? 900 : 260)},
        {"triggerExamples", workflow.value(QStringLiteral("triggerExamples")).toArray()},
        {"preferredTools", stringsToJsonArray(workflowToolNamesForSelector(workflow, detailed ? 18 : 8))},
        {"slots", stringsToJsonArray(workflowSlotNamesForSelector(workflow, detailed ? 24 : 10))},
        {"derivedValues", workflow.value(QStringLiteral("derivedValues")).toArray()},
        {"validationWarnings", workflow.value(QStringLiteral("validationWarnings")).toArray()},
    };

    if (detailed) {
        QJsonArray batches;
        const QJsonArray rawBatches = workflow.value(QStringLiteral("executionBatches")).toArray();
        for (const QJsonValue& batchValue : rawBatches) {
            const QJsonObject batch = batchValue.toObject();
            QJsonArray steps;
            const QJsonArray rawSteps = batch.value(QStringLiteral("steps")).toArray();
            for (const QJsonValue& stepValue : rawSteps) {
                steps.append(workflowStepCapsule(stepValue.toObject(), true));
            }
            batches.append(QJsonObject{
                {"id", batch.value(QStringLiteral("id")).toString()},
                {"title", repairMojibakeText(batch.value(QStringLiteral("title")).toString(batch.value(QStringLiteral("id")).toString())).left(160)},
                {"mode", batch.value(QStringLiteral("mode")).toString(QStringLiteral("sequential"))},
                {"stopOnFailure", batch.value(QStringLiteral("stopOnFailure")).toBool(true)},
                {"dependsOnBatch", batch.value(QStringLiteral("dependsOnBatch")).toString()},
                {"requiresVerificationPassed", batch.value(QStringLiteral("requiresVerificationPassed")).toBool(false)},
                {"continueOnlyWhenVerificationPasses", batch.value(QStringLiteral("continueOnlyWhenVerificationPasses")).toBool(false)},
                {"verificationContract", batch.value(QStringLiteral("verificationContract")).toObject()},
                {"steps", steps},
            });
        }
        if (!batches.isEmpty()) {
            capsule.insert(QStringLiteral("executionBatches"), batches);
        } else {
            QJsonArray flatSteps;
            for (const QJsonValue& stepValue : workflow.value(QStringLiteral("steps")).toArray()) {
                flatSteps.append(workflowStepCapsule(stepValue.toObject(), true));
            }
            if (!flatSteps.isEmpty()) {
                capsule.insert(QStringLiteral("steps"), flatSteps);
            }
        }
        capsule.insert(QStringLiteral("knownSlotValues"), workflow.value(QStringLiteral("knownSlotValues")).toObject());
    } else {
        capsule.insert(QStringLiteral("stepSummary"), workflowStepSummaryForSelector(workflow, 6));
    }
    return capsule;
}

QString workflowSelectorSearchText(const QJsonValue& value, int depth = 0)
{
    if (depth > 4 || value.isNull() || value.isUndefined()) {
        return {};
    }
    if (value.isString()) {
        return repairMojibakeText(value.toString());
    }
    if (value.isDouble() || value.isBool()) {
        return value.toVariant().toString();
    }
    QStringList parts;
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            const QString text = workflowSelectorSearchText(item, depth + 1).trimmed();
            if (!text.isEmpty()) {
                parts << text;
            }
        }
        return parts.join(QLatin1Char(' '));
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            parts << it.key();
            const QString text = workflowSelectorSearchText(it.value(), depth + 1).trimmed();
            if (!text.isEmpty()) {
                parts << text;
            }
        }
    }
    return parts.join(QLatin1Char(' '));
}

QStringList localWorkflowSelectionForPrompt(const QJsonArray& compactWorkflows, const QString& prompt, int maxCount = 3)
{
    const QString normalizedPrompt = workflowTrainingSearchText(prompt);
    QStringList terms = normalizedPrompt.split(QRegularExpression(QStringLiteral(R"([^a-z0-9_\.]+)")), Qt::SkipEmptyParts);
    terms.erase(std::remove_if(terms.begin(), terms.end(), [](const QString& term) {
        return term.size() < 3
            || term == QStringLiteral("und")
            || term == QStringLiteral("oder")
            || term == QStringLiteral("der")
            || term == QStringLiteral("die")
            || term == QStringLiteral("das")
            || term == QStringLiteral("mit")
            || term == QStringLiteral("zeichne")
            || term == QStringLiteral("zeichnen")
            || term == QStringLiteral("erstelle")
            || term == QStringLiteral("erstellen")
            || term == QStringLiteral("anlegen")
            || term == QStringLiteral("lege")
            || term == QStringLiteral("mach")
            || term == QStringLiteral("mache")
            || term == QStringLiteral("nutze")
            || term == QStringLiteral("verwende")
            || term == QStringLiteral("nun")
            || term == QStringLiteral("workflow")
            || term == QStringLiteral("ausfuehren")
            || term == QStringLiteral("ausfuehrung")
            || term == QStringLiteral("anwenden")
            || term == QStringLiteral("starten")
            || term == QStringLiteral("starte")
            || term == QStringLiteral("alle")
            || term == QStringLiteral("neue")
            || term == QStringLiteral("neuen")
            || term == QStringLiteral("neuer")
            || term == QStringLiteral("neu")
            || term == QStringLiteral("namens")
            || term == QStringLiteral("name")
            || term == QStringLiteral("nur")
            || term == QStringLiteral("layer")
            || term == QStringLiteral("layers")
            || term == QStringLiteral("ebene")
            || term == QStringLiteral("create")
            || term == QStringLiteral("query")
            || term == QStringLiteral("geometry")
            || term == QStringLiteral("measurement")
            || term == QStringLiteral("einen")
            || term == QStringLiteral("eine");
    }), terms.end());
    terms.removeDuplicates();

    QVector<QPair<int, QString>> scored;
    for (const QJsonValue& value : compactWorkflows) {
        const QJsonObject workflow = value.toObject();
        const QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        const QString haystack = workflowTrainingSearchText(workflowSelectorSearchText(workflow));
        int score = 0;
        for (const QString& term : terms) {
            if (haystack.contains(term)) {
                score += 2;
            }
        }
        const QString title = workflowTrainingSearchText(workflow.value(QStringLiteral("title")).toString());
        if (!title.isEmpty() && normalizedPrompt.contains(title.left(40))) {
            score += 12;
        }
        for (const QJsonValue& triggerValue : workflow.value(QStringLiteral("triggerExamples")).toArray()) {
            const QString trigger = workflowTrainingSearchText(triggerValue.toString());
            if (!trigger.isEmpty() && (normalizedPrompt.contains(trigger.left(40)) || trigger.contains(normalizedPrompt.left(40)))) {
                score += 10;
            }
        }
        if (score > 0) {
            scored.append(qMakePair(score, id));
        }
    }
    std::sort(scored.begin(), scored.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
        if (a.first == b.first) {
            return a.second < b.second;
        }
        return a.first > b.first;
    });

    QStringList ids;
    for (const auto& item : scored) {
        if (item.first < 2) {
            continue;
        }
        if (!ids.contains(item.second)) {
            ids << item.second;
        }
        if (ids.size() >= maxCount) {
            break;
        }
    }
    return ids;
}

QStringList routeWorkflowIds(const QJsonObject& route, int maxCount = 3)
{
    QStringList ids;
    auto appendId = [&](const QString& value) {
        const QString id = value.trimmed();
        if (!id.isEmpty() && !ids.contains(id) && ids.size() < maxCount) {
            ids << id;
        }
    };

    for (const QString& key : {QStringLiteral("selectedWorkflows"), QStringLiteral("workflows"), QStringLiteral("workflowIds")}) {
        const QJsonArray array = route.value(key).toArray();
        for (const QJsonValue& value : array) {
            if (value.isString()) {
                appendId(value.toString());
            } else if (value.isObject()) {
                const QJsonObject object = value.toObject();
                QString id = object.value("id").toString(object.value("workflowId").toString());
                if (id.isEmpty()) {
                    id = object.value("title").toString();
                }
                appendId(id);
            }
        }
    }
    return ids;
}

QString workflowDraftMessageForChat(const QJsonObject& reply, const QJsonObject& draft, const QJsonArray& unresolvedMissing)
{
    QStringList lines;
    const QString message = repairMojibakeText(reply.value("message").toString()).trimmed();
    if (!message.isEmpty()) {
        lines << message;
    }

    const QString title = repairMojibakeText(draft.value("title").toString()).trimmed();
    if (!title.isEmpty()) {
        lines << QStringLiteral("**Workflow-Entwurf:** %1").arg(title);
    }

    const QString description = repairMojibakeText(draft.value("description").toString()).trimmed();
    if (!description.isEmpty()) {
        lines << description;
    }

    const QStringList strategy = workflowObjectArraySummaries(draft.value("constructionStrategy").toArray(), 4);
    if (!strategy.isEmpty()) {
        lines << QStringLiteral("**Strategie**");
        for (const QString& item : strategy) {
            lines << QStringLiteral("- %1").arg(item);
        }
    }

    const QJsonArray derivedValues = draft.value("derivedValues").toArray();
    if (!derivedValues.isEmpty()) {
        lines << QStringLiteral("**Formeln**");
        int count = 0;
        for (const QJsonValue& value : derivedValues) {
            if (count++ >= 6) {
                break;
            }
            const QJsonObject derived = value.toObject();
            const QString name = repairMojibakeText(derived.value("name").toString()).trimmed();
            const QString expression = repairMojibakeText(derived.value("expression").toString()).trimmed();
            const QString example = workflowJsonValueSummary(derived.value("example"));
            QString line = name.isEmpty() ? QStringLiteral("Formel") : name;
            if (!expression.isEmpty()) {
                line += QStringLiteral(" = %1").arg(expression);
            }
            if (!example.isEmpty()) {
                line += QStringLiteral(" (Beispiel: %1)").arg(example);
            }
            lines << QStringLiteral("- %1").arg(line);
        }
    }

    const QJsonArray batches = draft.value("executionBatches").toArray();
    if (!batches.isEmpty()) {
        lines << QStringLiteral("**Batch-Ausfuehrungen**");
        int count = 0;
        for (const QJsonValue& value : batches) {
            if (count++ >= 6) {
                break;
            }
            const QJsonObject batch = value.toObject();
            QString label = repairMojibakeText(batch.value("title").toString()).trimmed();
            if (label.isEmpty()) {
                label = repairMojibakeText(batch.value("id").toString()).trimmed();
            }
            const int stepCount = batch.value("steps").toArray().size();
            if (label.isEmpty()) {
                label = QStringLiteral("Batch %1").arg(count);
            }
            lines << QStringLiteral("- %1 (%2 Schritt%3)")
                .arg(label)
                .arg(stepCount)
                .arg(stepCount == 1 ? QString() : QStringLiteral("e"));
        }
    }

    const QJsonArray displaySteps = workflowDisplaySteps(draft);
    if (!displaySteps.isEmpty()) {
        lines << QStringLiteral("**Werkzeugschritte**");
        int count = 0;
        for (const QJsonValue& value : displaySteps) {
            if (count >= 12) {
                break;
            }
            const QJsonObject step = value.toObject();
            lines << QStringLiteral("- %1").arg(workflowStepDisplayLine(step, count + 1));
            ++count;
        }
        if (displaySteps.size() > count) {
            lines << QStringLiteral("- ... %1 weitere Schritte").arg(displaySteps.size() - count);
        }
    }

    const QJsonArray questions = reply.value("questions").toArray();
    QStringList openQuestions;
    for (const QJsonValue& value : questions) {
        const QString question = repairMojibakeText(value.toString()).trimmed();
        if (!question.isEmpty()) {
            openQuestions << question;
        }
    }
    if (!openQuestions.isEmpty()) {
        lines << QStringLiteral("**Offene Fragen**");
        for (const QString& question : openQuestions.mid(0, 8)) {
            lines << QStringLiteral("- %1").arg(question);
        }
    } else if (!unresolvedMissing.isEmpty()) {
        lines << QStringLiteral("**Offene Angaben**");
        int count = 0;
        for (const QJsonValue& value : unresolvedMissing) {
            if (count++ >= 8) {
                break;
            }
            const QString item = repairMojibakeText(workflowSlotNameFromValue(value)).trimmed();
            if (!item.isEmpty()) {
                lines << QStringLiteral("- %1").arg(item);
            }
        }
    }

    if (lines.isEmpty()) {
        lines << QStringLiteral("Workflow-Entwurf wurde aktualisiert. Du kannst ihn jetzt weiter beschreiben oder mit \"passt, speichern\" finalisieren.");
    }
    return lines.join('\n');
}

QString validationFailureMessage(const QJsonObject& response)
{
    const QJsonObject result = response.value("result").toObject();
    QStringList lines;

    const QString transportError = response.value("error").toString().trimmed();
    if (!transportError.isEmpty()) {
        lines << QString("BRX Fehler: %1").arg(transportError);
    }

    const QStringList errors = stringsFromJsonArray(result.value("errors").toArray());
    if (!errors.isEmpty()) {
        lines << QString("Fehler: %1").arg(errors.join("; "));
    }

    const QStringList missing = stringsFromJsonArray(result.value("missing").toArray());
    if (!missing.isEmpty()) {
        lines << QString("Fehlende Daten: %1").arg(missing.join("; "));
    }

    const QStringList hints = stringsFromJsonArray(result.value("hints").toArray());
    if (!hints.isEmpty()) {
        lines << QString("Hinweise: %1").arg(hints.join("; "));
    }

    if (lines.isEmpty()) {
        lines << QStringLiteral("BRX Preflight konnte den Vorschlag nicht validieren.");
    }
    return lines.join("\n").left(4000);
}

QJsonObject agentResponseContractObject()
{
    return QJsonObject{
        {"schema", "barebone.agent.response.v2"},
        {"strictJsonObject", true},
        {"preferred", true},
        {"topLevel", QJsonObject{
            {"schema", "barebone.agent.response.v2"},
            {"allowedTypes", QJsonArray{"message", "ask_user", "context_request", "action_proposal", "plan"}},
            {"required", QJsonArray{"schema", "type"}},
            {"sessionTitlePolicy", "Set sessionTitle as a top-level field on every response; max 6 words."},
        }},
        {"sessionTitle", QJsonObject{
            {"recommended", true},
            {"maxWords", 6},
            {"policy", "Kurzer Sitzungsname aus komprimiertem Kontext; keine generischen Titel wie Neuer Chat, Allgemeiner Chat, Workflow oder Frage."},
        }},
        {"actionProposal", QJsonObject{
            {"required", QJsonArray{"type", "message", "proposal"}},
            {"proposalRequired", QJsonArray{"requiresConfirmation", "actions"}},
            {"actionShape", QJsonObject{
                {"required", QJsonArray{"tool", "params"}},
                {"toolSource", "effectiveTools[].name"},
                {"paramsSource", "effectiveTools[].inputSchema"},
            }},
            {"forbiddenTopLevelFields", QJsonArray{"tool", "params", "actions"}},
        }},
        {"askUser", QJsonObject{
            {"required", QJsonArray{"type", "message", "missing"}},
            {"draftPolicy", "If a draft is included, use draft.proposal with the same proposal shape as action_proposal."},
        }},
        {"defaultsPolicy", "Ask only for mandatory unknown data. For domain-standard requests, propose sensible defaults and list them in assumptions."},
    };
}

QString normalizedAiBaseUrl(QString value, const QString& provider)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        value = provider.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("http://192.168.0.67:1234/v1")
            : QStringLiteral("https://api.openai.com/v1");
    }
    if (!value.startsWith("http://", Qt::CaseInsensitive)
        && !value.startsWith("https://", Qt::CaseInsensitive)) {
        value.prepend(provider.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("http://")
            : QStringLiteral("https://"));
    }
    while (value.endsWith('/')) {
        value.chop(1);
    }
    return value;
}

QUrl lmStudioModelsUrlFromOpenAiBaseUrl(const QString& baseUrl)
{
    QString root = baseUrl.trimmed();
    while (root.endsWith('/')) {
        root.chop(1);
    }
    if (root.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        root.chop(3);
    } else if (root.endsWith(QStringLiteral("/api/v1"), Qt::CaseInsensitive)) {
        root.chop(7);
    }
    while (root.endsWith('/')) {
        root.chop(1);
    }
    return QUrl(root + QStringLiteral("/api/v1/models"));
}

int positiveIntValue(const QJsonObject& object, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isDouble()) {
            const int parsed = value.toInt();
            if (parsed > 0) {
                return parsed;
            }
        }
        if (value.isString()) {
            bool ok = false;
            const int parsed = value.toString().toInt(&ok);
            if (ok && parsed > 0) {
                return parsed;
            }
        }
    }
    return 0;
}

QString finalAiMessageSegment(QString content)
{
    return content.trimmed();
}

QString aiChatCompletionFinishReason(const QJsonObject& response)
{
    const QJsonArray choices = response.value("choices").toArray();
    if (!choices.isEmpty()) {
        return choices.first().toObject().value("finish_reason").toString().trimmed();
    }

    const QString status = response.value("status").toString().trimmed();
    if (!status.isEmpty()) {
        return status;
    }
    return {};
}

bool aiResponseWasTruncated(const QJsonObject& response)
{
    const QString finishReason = aiChatCompletionFinishReason(response).trimmed().toLower();
    if (finishReason == QStringLiteral("length")
        || finishReason == QStringLiteral("max_tokens")
        || finishReason == QStringLiteral("max_output_tokens")
        || finishReason == QStringLiteral("incomplete")) {
        return true;
    }

    const QJsonObject incompleteDetails = response.value(QStringLiteral("incomplete_details")).toObject();
    const QString reason = incompleteDetails.value(QStringLiteral("reason")).toString().trimmed().toLower();
    return reason.contains(QStringLiteral("max"))
        && reason.contains(QStringLiteral("token"));
}

QString aiResponseTruncationReason(const QJsonObject& response)
{
    const QString finishReason = aiChatCompletionFinishReason(response).trimmed();
    const QString incompleteReason = response.value(QStringLiteral("incomplete_details")).toObject()
        .value(QStringLiteral("reason")).toString().trimmed();
    if (!incompleteReason.isEmpty()) {
        return incompleteReason;
    }
    return finishReason.isEmpty() ? QStringLiteral("token_limit") : finishReason;
}

QString decodeJsonStringLiteral(const QString& literal)
{
    if (!literal.trimmed().startsWith('"')) {
        return {};
    }

    QJsonParseError parseError;
    const QByteArray wrapper = QByteArrayLiteral("{\"value\":") + literal.toUtf8() + QByteArrayLiteral("}");
    const QJsonDocument document = QJsonDocument::fromJson(wrapper, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object().value("value").toString().trimmed();
}

QJsonObject workflowTrainingAskUserFromPartialContent(const QString& content)
{
    const QString source = finalAiMessageSegment(content);
    if (!source.contains(QStringLiteral("\"ask_user\""))
        && !source.contains(QStringLiteral("ask_user"))) {
        return {};
    }

    QString message;
    const QRegularExpression messagePattern(
        QStringLiteral(R"("message"\s*:\s*("(?:\\.|[^"\\])*"))"));
    const QRegularExpressionMatch messageMatch = messagePattern.match(source);
    if (messageMatch.hasMatch()) {
        message = decodeJsonStringLiteral(messageMatch.captured(1));
    }
    if (message.isEmpty()) {
        message = QStringLiteral("Die AI hat eine Rueckfrage begonnen, die Antwort wurde aber abgeschnitten. Bitte gib die fehlenden Angaben fuer den Workflow an oder formuliere den letzten Trainingsschritt knapper.");
    }

    QJsonArray missing;
    const QRegularExpression missingPattern(
        QStringLiteral(R"("missing"\s*:\s*\[([^\]]*)\])"));
    const QRegularExpressionMatch missingMatch = missingPattern.match(source);
    if (missingMatch.hasMatch()) {
        const QRegularExpression itemPattern(QStringLiteral(R"("(?:\\.|[^"\\])*")"));
        QRegularExpressionMatchIterator it = itemPattern.globalMatch(missingMatch.captured(1));
        while (it.hasNext()) {
            const QString item = decodeJsonStringLiteral(it.next().captured(0));
            if (!item.isEmpty()) {
                missing.append(item);
            }
        }
    }

    QJsonArray questions;
    const QRegularExpression questionsPattern(
        QStringLiteral(R"("questions"\s*:\s*\[([^\]]*)\])"));
    const QRegularExpressionMatch questionsMatch = questionsPattern.match(source);
    if (questionsMatch.hasMatch()) {
        const QRegularExpression itemPattern(QStringLiteral(R"("(?:\\.|[^"\\])*")"));
        QRegularExpressionMatchIterator it = itemPattern.globalMatch(questionsMatch.captured(1));
        while (it.hasNext()) {
            const QString item = decodeJsonStringLiteral(it.next().captured(0));
            if (!item.isEmpty()) {
                questions.append(item);
            }
        }
    }

    QJsonObject reply{
        {"schema", "barebone.workflow.training.response.v1"},
        {"type", "ask_user"},
        {"message", message},
    };
    if (!missing.isEmpty()) {
        reply.insert("missing", missing);
    }
    if (!questions.isEmpty()) {
        reply.insert("questions", questions);
    }
    return reply;
}

QString repairMojibakeText(QString text)
{
    const auto chars = [](std::initializer_list<ushort> codePoints) {
        QString value;
        for (const ushort codePoint : codePoints) {
            value.append(QChar(codePoint));
        }
        return value;
    };
    const QList<QPair<QString, QString>> simpleMojibakeReplacements = {
        {chars({0x00C3, 0x00A4}), chars({0x00E4})},
        {chars({0x00C3, 0x00B6}), chars({0x00F6})},
        {chars({0x00C3, 0x00BC}), chars({0x00FC})},
        {chars({0x00C3, 0x0084}), chars({0x00C4})},
        {chars({0x00C3, 0x0096}), chars({0x00D6})},
        {chars({0x00C3, 0x009C}), chars({0x00DC})},
        {chars({0x00C3, 0x009F}), chars({0x00DF})},
        {chars({0x00C2, 0x00B0}), chars({0x00B0})},
        {chars({0x00C2, 0x00B2}), chars({0x00B2})},
        {chars({0x00C2, 0x00B3}), chars({0x00B3})},
        {chars({0x00C2, 0x00B5}), chars({0x00B5})},
        {chars({0x00E2, 0x0082, 0x00AC}), chars({0x20AC})},
        {chars({0x00E2, 0x0080, 0x0093}), chars({0x2013})},
        {chars({0x00E2, 0x0080, 0x0094}), chars({0x2014})},
        {chars({0x00E2, 0x0080, 0x009E}), chars({0x201E})},
        {chars({0x00E2, 0x0080, 0x009C}), chars({0x201C})},
        {chars({0x00E2, 0x0080, 0x009D}), chars({0x201D})},
        {chars({0x00E2, 0x0080, 0x0098}), chars({0x2018})},
        {chars({0x00E2, 0x0080, 0x0099}), chars({0x2019})},
        {chars({0x00E2, 0x0080, 0x00A6}), chars({0x2026})},
    };
#if 0
    const QList<QPair<QString, QString>> simpleReplacements = {
        {QStringLiteral("Ã¤"), QStringLiteral("ä")},
        {QStringLiteral("Ã¶"), QStringLiteral("ö")},
        {QStringLiteral("Ã¼"), QStringLiteral("ü")},
        {QStringLiteral("Ã„"), QStringLiteral("Ä")},
        {QStringLiteral("Ã–"), QStringLiteral("Ö")},
        {QStringLiteral("Ãœ"), QStringLiteral("Ü")},
        {QStringLiteral("ÃŸ"), QStringLiteral("ß")},
        {QStringLiteral("Â°"), QStringLiteral("°")},
        {QStringLiteral("Â²"), QStringLiteral("²")},
        {QStringLiteral("Â³"), QStringLiteral("³")},
        {QStringLiteral("Âµ"), QStringLiteral("µ")},
        {QStringLiteral("â‚¬"), QStringLiteral("€")},
        {QStringLiteral("â€“"), QStringLiteral("–")},
        {QStringLiteral("â€”"), QStringLiteral("—")},
        {QStringLiteral("â€ž"), QStringLiteral("„")},
        {QStringLiteral("â€œ"), QStringLiteral("“")},
        {QStringLiteral("â€"), QStringLiteral("”")},
        {QStringLiteral("â€˜"), QStringLiteral("‘")},
        {QStringLiteral("â€™"), QStringLiteral("’")},
        {QStringLiteral("â€¦"), QStringLiteral("…")},
    };
#endif
    for (const auto& replacement : simpleMojibakeReplacements) {
        text.replace(replacement.first, replacement.second);
    }

    if (!text.contains(QStringLiteral("ÃƒÆ’"))
        && !text.contains(QStringLiteral("Ãƒâ€š"))
        && !text.contains(QStringLiteral("ÃƒÂ¢"))
        && !text.contains(QStringLiteral("ÃƒÅ½"))
        && !text.contains(QStringLiteral("ÃƒÂ"))) {
        return text;
    }

    const QList<QPair<QString, QString>> replacements = {
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾"), QStringLiteral("Ã¢â‚¬Å¾")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã…â€œ"), QStringLiteral("Ã¢â‚¬Å“")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â"), QStringLiteral("Ã¢â‚¬Â")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢"), QStringLiteral("Ã¢â‚¬â„¢")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã‹Å“"), QStringLiteral("Ã¢â‚¬Ëœ")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“"), QStringLiteral("Ã¢â‚¬â€œ")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â"), QStringLiteral("Ã¢â‚¬â€")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦"), QStringLiteral("Ã¢â‚¬Â¦")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¯"), QStringLiteral(" ")},
        {QStringLiteral("ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Ëœ"), QStringLiteral("-")},
        {QStringLiteral("ÃƒÂ¢Ã‚ÂÃ‚Â»"), QStringLiteral("Ã¢ÂÂ»")},
        {QStringLiteral("ÃƒÂ¢Ã‚ÂÃ‚Âº"), QStringLiteral("Ã¢ÂÂº")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Å¡Ã¢â‚¬Å¡"), QStringLiteral("Ã¢â€šâ€š")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Å¡Ã†â€™"), QStringLiteral("Ã¢â€šÆ’")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€ "), QStringLiteral("Ã¢â€°Ë†")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¤"), QStringLiteral("Ã¢â€°Â¤")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥"), QStringLiteral("Ã¢â€°Â¥")},
        {QStringLiteral("ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢"), QStringLiteral("Ã¢â€ â€™")},
        {QStringLiteral("ÃƒÆ’Ã‚Â¤"), QStringLiteral("ÃƒÂ¤")},
        {QStringLiteral("ÃƒÆ’Ã‚Â¶"), QStringLiteral("ÃƒÂ¶")},
        {QStringLiteral("ÃƒÆ’Ã‚Â¼"), QStringLiteral("ÃƒÂ¼")},
        {QStringLiteral("ÃƒÆ’Ã¢â‚¬Å¾"), QStringLiteral("Ãƒâ€ž")},
        {QStringLiteral("ÃƒÆ’Ã¢â‚¬â€œ"), QStringLiteral("Ãƒâ€“")},
        {QStringLiteral("ÃƒÆ’Ã…â€œ"), QStringLiteral("ÃƒÅ“")},
        {QStringLiteral("ÃƒÆ’Ã…Â¸"), QStringLiteral("ÃƒÅ¸")},
        {QStringLiteral("ÃƒÆ’Ã‚Â©"), QStringLiteral("ÃƒÂ©")},
        {QStringLiteral("ÃƒÆ’Ã‚Â¨"), QStringLiteral("ÃƒÂ¨")},
        {QStringLiteral("ÃƒÆ’Ã‚Â¡"), QStringLiteral("ÃƒÂ¡")},
        {QStringLiteral("ÃƒÆ’Ã‚Â³"), QStringLiteral("ÃƒÂ³")},
        {QStringLiteral("ÃƒÆ’Ã‚Â±"), QStringLiteral("ÃƒÂ±")},
        {QStringLiteral("ÃƒÆ’Ã‚Â§"), QStringLiteral("ÃƒÂ§")},
        {QStringLiteral("ÃƒÆ’Ã¢â‚¬â€"), QStringLiteral("Ãƒâ€”")},
        {QStringLiteral("Ãƒâ€šÃ‚Â«"), QStringLiteral("Ã‚Â«")},
        {QStringLiteral("Ãƒâ€šÃ‚Â»"), QStringLiteral("Ã‚Â»")},
        {QStringLiteral("Ãƒâ€šÃ‚Â°"), QStringLiteral("Ã‚Â°")},
        {QStringLiteral("ÃƒÅ½Ã¢â‚¬Â"), QStringLiteral("ÃŽâ€")},
        {QStringLiteral("ÃƒÅ½Ã‚Â´"), QStringLiteral("ÃŽÂ´")},
        {QStringLiteral("ÃƒÅ½Ã‚Âµ"), QStringLiteral("ÃŽÂµ")},
        {QStringLiteral("ÃƒÂÃ‚Â"), QStringLiteral("ÃÂ")},
        {QStringLiteral("ÃƒÂÃ‚â€ "), QStringLiteral("Ãâ€ ")},
        {QStringLiteral("ÃƒÂÃ¢â€šÂ¬"), QStringLiteral("Ãâ‚¬")},
        {QStringLiteral("Ãƒâ€š"), QStringLiteral("")},
    };

    for (const auto& replacement : replacements) {
        text.replace(replacement.first, replacement.second);
    }
    return text;
}

bool looksLikeReasoningLeak(const QString& content)
{
    const QString lower = content.left(6000).toLower();
    if (lower.startsWith(QStringLiteral("the user asked"))
        || lower.startsWith(QStringLiteral("we need to"))
        || lower.contains(QStringLiteral("developer message"))
        || lower.contains(QStringLiteral("system message says"))
        || lower.contains(QStringLiteral("we need to ensure"))) {
        return true;
    }

    int hits = 0;
    for (const QString& marker : {
             QStringLiteral("the user asked"),
             QStringLiteral("the user wants"),
             QStringLiteral("we need to"),
             QStringLiteral("we should"),
             QStringLiteral("we can"),
             QStringLiteral("we must"),
             QStringLiteral("let's produce"),
             QStringLiteral("final answer"),
             QStringLiteral("developer message"),
             QStringLiteral("instructions")}) {
        if (lower.contains(marker)) {
            ++hits;
        }
    }
    return hits >= 3;
}

QString removeReasoningLeak(QString content)
{
    content = repairMojibakeText(content).trimmed();
    if (!looksLikeReasoningLeak(content)) {
        return content;
    }

    QString candidate = content;
    const QString lower = candidate.toLower();
    const QStringList finalMarkers = {
        QStringLiteral("ok let's produce final answer:"),
        QStringLiteral("let's produce final answer:"),
        QStringLiteral("final answer:"),
        QStringLiteral("final response:")
    };
    int markerIndex = -1;
    int markerLength = 0;
    for (const QString& marker : finalMarkers) {
        const int index = lower.lastIndexOf(marker);
        if (index >= 0 && index >= markerIndex) {
            markerIndex = index;
            markerLength = marker.size();
        }
    }
    if (markerIndex >= 0) {
        candidate = candidate.mid(markerIndex + markerLength).trimmed();
    }

    const int markdownHeading = candidate.indexOf(QStringLiteral("**"));
    if (markdownHeading > 0 && markdownHeading < 160) {
        candidate = candidate.mid(markdownHeading).trimmed();
    }

    const QString candidateLower = candidate.left(1200).toLower();
    if (candidate.isEmpty() || looksLikeReasoningLeak(candidate) || candidateLower.contains(QStringLiteral("developer message"))) {
        return QStringLiteral("Die AI-Antwort enthielt interne Analyse und wurde ausgeblendet. Bitte sende die Anfrage erneut.");
    }
    return candidate;
}

QString firstBalancedJsonObject(const QString& content)
{
    const int start = content.indexOf('{');
    if (start < 0) {
        return {};
    }

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (int i = start; i < content.size(); ++i) {
        const QChar ch = content.at(i);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('\\') && inString) {
            escaped = true;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = !inString;
            continue;
        }
        if (inString) {
            continue;
        }
        if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                return content.mid(start, i - start + 1);
            }
        }
    }

    return {};
}

QJsonObject jsonObjectFromAiContent(QString content, bool* ok)
{
    content = finalAiMessageSegment(content);
    content = content.trimmed();

    if (content.startsWith("```")) {
        const int firstNewline = content.indexOf('\n');
        const int lastFence = content.lastIndexOf("```");
        if (firstNewline >= 0 && lastFence > firstNewline) {
            content = content.mid(firstNewline + 1, lastFence - firstNewline - 1).trimmed();
        }
    }

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject object = document.object();
        const QString nestedMessage = object.value("message").toString().trimmed();
        if (!nestedMessage.isEmpty()
            && nestedMessage.startsWith(QLatin1Char('{'))) {
            bool nestedOk = false;
            const QJsonObject nestedObject = jsonObjectFromAiContent(nestedMessage, &nestedOk);
            if (nestedOk && nestedObject.contains("type")) {
                if (ok) {
                    *ok = true;
                }
                return nestedObject;
            }
        }
        if (ok) {
            *ok = true;
        }
        return object;
    }
    if (parseError.error == QJsonParseError::NoError && document.isArray()) {
        const QJsonArray array = document.array();
        if (!array.isEmpty() && array.first().isObject()) {
            if (ok) {
                *ok = true;
            }
            return array.first().toObject();
        }
    }
    if (content.startsWith('"') && content.endsWith('"')) {
        QJsonParseError wrappedParseError;
        const QJsonDocument wrappedDocument = QJsonDocument::fromJson(
            QByteArray("[") + content.toUtf8() + QByteArray("]"),
            &wrappedParseError);
        if (wrappedParseError.error == QJsonParseError::NoError
            && wrappedDocument.isArray()
            && !wrappedDocument.array().isEmpty()
            && wrappedDocument.array().first().isString()) {
            bool nestedOk = false;
            const QJsonObject nestedObject = jsonObjectFromAiContent(wrappedDocument.array().first().toString(), &nestedOk);
            if (nestedOk) {
                if (ok) {
                    *ok = true;
                }
                return nestedObject;
            }
        }
    }

    const QString candidate = firstBalancedJsonObject(content);
    if (!candidate.isEmpty()) {
        document = QJsonDocument::fromJson(candidate.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            if (ok) {
                *ok = true;
            }
            return document.object();
        }
    }

    if (ok) {
        *ok = false;
    }
    return {};
}

QStringList workflowTrainingSlotLabels(const QJsonObject& workflow, const QString& key)
{
    QStringList labels;
    const QJsonArray slots = workflow.value(key).toArray();
    for (const QJsonValue& value : slots) {
        QString name;
        QString description;
        if (value.isObject()) {
            const QJsonObject slot = value.toObject();
            name = slot.value("name").toString().trimmed();
            description = slot.value("description").toString().trimmed();
        } else {
            name = value.toString().trimmed();
        }
        if (name.isEmpty()) {
            continue;
        }
        labels << (description.isEmpty()
            ? name
            : QStringLiteral("%1: %2").arg(name, description));
    }
    return labels;
}

QStringList workflowTrainingMissingLabels(const QJsonObject& reply, const QJsonObject& workflow)
{
    QStringList labels;
    for (const QJsonValue& value : reply.value("missing").toArray()) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            labels << text;
        }
    }
    if (labels.isEmpty()) {
        labels = workflowTrainingSlotLabels(workflow, QStringLiteral("requiredSlots"));
    }
    labels.removeDuplicates();
    return labels;
}

QString workflowTrainingSearchText(QString text)
{
    text = text.toLower();
    text.replace(QChar(0x00e4), QStringLiteral("ae"));
    text.replace(QChar(0x00f6), QStringLiteral("oe"));
    text.replace(QChar(0x00fc), QStringLiteral("ue"));
    text.replace(QChar(0x00df), QStringLiteral("ss"));
    text.replace(QChar(0x00b2), QStringLiteral("2"));
    text.replace(QStringLiteral("ÃƒÂ¤"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ÃƒÂ¶"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ÃƒÂ¼"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ÃƒÅ¸"), QStringLiteral("ss"));
    text.replace(QStringLiteral("Ã‚Â²"), QStringLiteral("2"));
    text.replace(',', '.');
    return text;
}

QString workflowSlotNameFromValue(const QJsonValue& value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QString name = object.value("name").toString().trimmed();
        if (name.isEmpty()) {
            name = object.value("id").toString().trimmed();
        }
        if (name.isEmpty()) {
            name = object.value("slot").toString().trimmed();
        }
        if (name.isEmpty()) {
            name = object.value("key").toString().trimmed();
        }
        return name;
    }
    return value.toString().trimmed();
}

QJsonObject normalizedWorkflowOptionalSlots(const QJsonValue& value, bool* changed = nullptr)
{
    bool localChanged = false;
    QJsonObject optionalSlots;
    if (value.isObject()) {
        optionalSlots = value.toObject();
        for (auto it = optionalSlots.begin(); it != optionalSlots.end(); ++it) {
            if (it.value().isString()) {
                it.value() = QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("string")},
                    {QStringLiteral("description"), it.value().toString()},
                };
                localChanged = true;
            } else if (it.value().isNull() || it.value().isUndefined()) {
                it.value() = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
                localChanged = true;
            }
        }
    } else if (value.isArray()) {
        localChanged = true;
        for (const QJsonValue& itemValue : value.toArray()) {
            QString name = workflowSlotNameFromValue(itemValue).trimmed();
            if (name.isEmpty()) {
                continue;
            }
            QJsonObject slot;
            if (itemValue.isObject()) {
                slot = itemValue.toObject();
                slot.remove(QStringLiteral("name"));
                slot.remove(QStringLiteral("id"));
                slot.remove(QStringLiteral("slot"));
                slot.remove(QStringLiteral("key"));
            } else {
                slot.insert(QStringLiteral("type"), QStringLiteral("string"));
            }
            if (!slot.contains(QStringLiteral("type"))) {
                slot.insert(QStringLiteral("type"), QStringLiteral("string"));
            }
            optionalSlots.insert(name, slot);
        }
    } else if (!value.isUndefined() && !value.isNull()) {
        localChanged = true;
    }

    if (changed) {
        *changed = *changed || localChanged;
    }
    return optionalSlots;
}

QJsonObject workflowObjectFromAiSaveResponse(QJsonObject object)
{
    for (const QString& key : {
             QStringLiteral("workflow"),
             QStringLiteral("workflowUpdate"),
             QStringLiteral("workflow_update"),
             QStringLiteral("workflowDraft"),
             QStringLiteral("workflow_draft"),
         }) {
        if (object.value(key).isObject()) {
            return object.value(key).toObject();
        }
    }
    return object;
}

QString workflowTitleSeed(QString text)
{
    text = repairMojibakeText(text)
        .replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
        .replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "))
        .trimmed();
    return text.left(90).trimmed();
}

bool isGenericWorkflowSessionTitle(const QString& title)
{
    const QString slug = workflowSlug(title);
    return slug.isEmpty()
        || slug == QStringLiteral("workflow")
        || slug == QStringLiteral("neuer_chat")
        || slug.startsWith(QStringLiteral("neuer_chat_"))
        || slug == QStringLiteral("bricscad_ai_assistent")
        || slug == QStringLiteral("ai_assistent")
        || slug == QStringLiteral("chat");
}

void appendWorkflowLayerNamesFromAction(QStringList& names, const QJsonObject& action, const QString& paramsKey)
{
    const QString tool = action.value(QStringLiteral("tool")).toString(
        action.value(QStringLiteral("method")).toString(
            action.value(QStringLiteral("operation")).toString())).trimmed();
    const QJsonObject params = action.value(paramsKey).toObject();
    auto appendName = [&names](const QString& value) {
        const QString name = repairMojibakeText(value).trimmed();
        if (!name.isEmpty() && !names.contains(name)) {
            names << name;
        }
    };

    if (tool == QStringLiteral("layers.create")) {
        appendName(params.value(QStringLiteral("name")).toString());
        return;
    }
    if (tool == QStringLiteral("layers.ensureMany")) {
        for (const QJsonValue& layerValue : params.value(QStringLiteral("layers")).toArray()) {
            if (layerValue.isObject()) {
                appendName(layerValue.toObject().value(QStringLiteral("name")).toString());
            } else {
                appendName(layerValue.toString());
            }
        }
    }
}

QStringList workflowLayerNames(const QJsonObject& workflow)
{
    QStringList names;
    for (const QJsonValue& exampleValue : workflow.value(QStringLiteral("validationExamples")).toArray()) {
        for (const QJsonValue& actionValue : exampleValue.toObject().value(QStringLiteral("actions")).toArray()) {
            appendWorkflowLayerNamesFromAction(names, actionValue.toObject(), QStringLiteral("params"));
        }
    }
    for (const QJsonValue& stepValue : workflow.value(QStringLiteral("steps")).toArray()) {
        appendWorkflowLayerNamesFromAction(names, stepValue.toObject(), QStringLiteral("paramsTemplate"));
    }
    for (const QJsonValue& batchValue : workflow.value(QStringLiteral("executionBatches")).toArray()) {
        for (const QJsonValue& stepValue : batchValue.toObject().value(QStringLiteral("steps")).toArray()) {
            appendWorkflowLayerNamesFromAction(names, stepValue.toObject(), QStringLiteral("paramsTemplate"));
        }
    }
    return names;
}

void ensureWorkflowIdentityForSessionSave(QJsonObject& workflow, const QString& sessionTitle, const QString& lastUserPrompt)
{
    QString title = workflowTitleSeed(workflow.value(QStringLiteral("title")).toString());
    const QStringList layerNames = workflowLayerNames(workflow);
    if (title.isEmpty()) {
        if (layerNames.size() == 1) {
            title = QStringLiteral("Layer \"%1\" anlegen").arg(layerNames.first());
        } else if (layerNames.size() > 1) {
            title = QStringLiteral("%1 Layer anlegen").arg(layerNames.size());
        }
    }
    if (title.isEmpty()) {
        const QJsonArray triggers = workflow.value(QStringLiteral("triggerExamples")).toArray();
        for (const QJsonValue& value : triggers) {
            title = workflowTitleSeed(value.toString());
            if (!title.isEmpty()) {
                break;
            }
        }
    }
    if (title.isEmpty() && !isGenericWorkflowSessionTitle(sessionTitle)) {
        title = workflowTitleSeed(sessionTitle);
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(lastUserPrompt);
    }
    if (title.isEmpty()) {
        title = QStringLiteral("BricsCAD Workflow");
    }
    workflow.insert(QStringLiteral("title"), title);

    QString id = workflowSlug(workflow.value(QStringLiteral("id")).toString());
    if (id == QStringLiteral("workflow")) {
        id = workflowSlug(title);
    }
    workflow.insert(QStringLiteral("id"), id);

    if (workflow.value(QStringLiteral("description")).toString().trimmed().isEmpty()) {
        workflow.insert(QStringLiteral("description"), title);
    }
    QJsonArray triggerExamples = workflow.value(QStringLiteral("triggerExamples")).toArray();
    if (triggerExamples.isEmpty() && !lastUserPrompt.trimmed().isEmpty()) {
        triggerExamples.append(lastUserPrompt.trimmed());
        workflow.insert(QStringLiteral("triggerExamples"), triggerExamples);
    }
}

QString canonicalWorkflowSlot(QString slot)
{
    slot = workflowTrainingSearchText(slot.trimmed());
    if (slot.isEmpty()) {
        return {};
    }
    if (slot.contains(QStringLiteral("wandstaerke"))
        || slot.contains(QStringLiteral("wanddicke"))
        || slot.contains(QStringLiteral("wallthickness"))
        || slot == QStringLiteral("thickness")
        || slot.contains(QStringLiteral("dicke"))) {
        return QStringLiteral("wallThicknessMm");
    }
    if (slot.contains(QStringLiteral("wandhoehe"))
        || slot.contains(QStringLiteral("wallheight"))
        || slot == QStringLiteral("height")
        || slot == QStringLiteral("hoehe")
        || slot == QStringLiteral("hohe")) {
        return QStringLiteral("wallHeightMm");
    }
    if (slot.contains(QStringLiteral("raumbreite"))
        || slot.contains(QStringLiteral("roomwidth"))
        || slot == QStringLiteral("width")
        || slot == QStringLiteral("breite")) {
        return QStringLiteral("roomWidthMm");
    }
    if (slot.contains(QStringLiteral("raumlaenge"))
        || slot.contains(QStringLiteral("roomlength"))
        || slot == QStringLiteral("length")
        || slot == QStringLiteral("laenge")
        || slot == QStringLiteral("lange")
        || slot == QStringLiteral("tiefe")) {
        return QStringLiteral("roomLengthMm");
    }
    if (slot.contains(QStringLiteral("raumflaeche"))
        || slot.contains(QStringLiteral("roomarea"))
        || slot == QStringLiteral("area")
        || slot == QStringLiteral("flaeche")
        || slot == QStringLiteral("flache")) {
        return QStringLiteral("roomAreaM2");
    }
    if (slot.contains(QStringLiteral("layer"))) {
        return QStringLiteral("layerName");
    }
    if (slot.contains(QStringLiteral("bewegungsmodus"))
        || slot.contains(QStringLiteral("verschiebungsmodus"))
        || slot.contains(QStringLiteral("movemode"))
        || slot.contains(QStringLiteral("movementmode"))
        || slot.contains(QStringLiteral("relativoderabsolut"))
        || slot.contains(QStringLiteral("absoluteorrelative"))) {
        return QStringLiteral("moveMode");
    }
    if (slot.contains(QStringLiteral("verschiebungsvektor"))
        || slot.contains(QStringLiteral("bewegungsvektor"))
        || slot.contains(QStringLiteral("movevector"))
        || slot.contains(QStringLiteral("movementvector"))
        || slot == QStringLiteral("vector")
        || slot == QStringLiteral("vektor")
        || slot == QStringLiteral("offset")
        || slot == QStringLiteral("translation")) {
        return QStringLiteral("moveVector");
    }
    if (slot.contains(QStringLiteral("bim"))
        || slot.contains(QStringLiteral("klassifiz"))
        || slot.contains(QStringLiteral("classification"))) {
        return QStringLiteral("classification");
    }
    return slot;
}

QStringList workflowSlotAliases(const QString& canonicalSlot)
{
    if (canonicalSlot == QStringLiteral("wallThicknessMm")) {
        return {"wandstaerke", "wanddicke", "wallthickness", "thickness", "dicke"};
    }
    if (canonicalSlot == QStringLiteral("wallHeightMm")) {
        return {"wandhoehe", "wallheight", "hoehe", "hohe", "height"};
    }
    if (canonicalSlot == QStringLiteral("roomWidthMm")) {
        return {"raumbreite", "roomwidth", "breite", "width"};
    }
    if (canonicalSlot == QStringLiteral("roomLengthMm")) {
        return {"raumlaenge", "roomlength", "laenge", "lange", "tiefe", "length"};
    }
    if (canonicalSlot == QStringLiteral("roomAreaM2")) {
        return {"raumflaeche", "roomarea", "flaeche", "flache", "area", "m2", "qm", "quadratmeter"};
    }
    if (canonicalSlot == QStringLiteral("layerName")) {
        return {"layer", "layername", "ebene"};
    }
    if (canonicalSlot == QStringLiteral("moveMode")) {
        return {"verschiebungsmodus", "bewegungsmodus", "movemode", "movementmode", "relativ", "relative", "absolut", "absolute"};
    }
    if (canonicalSlot == QStringLiteral("moveVector")) {
        return {"verschiebungsvektor", "bewegungsvektor", "vektor", "vector", "offset", "translation", "x", "y", "z"};
    }
    if (canonicalSlot == QStringLiteral("classification")) {
        return {"bim", "bimwall", "klassifizierung", "classification"};
    }
    return {workflowTrainingSearchText(canonicalSlot)};
}

void appendCanonicalWorkflowSlotUnique(QStringList& slots, const QString& slot)
{
    const QString canonical = canonicalWorkflowSlot(slot);
    if (!canonical.isEmpty() && !slots.contains(canonical)) {
        slots << canonical;
    }
}

void collectWorkflowSlotRefsFromValue(const QJsonValue& value, QStringList& slots)
{
    QStringList placeholders;
    collectTemplatePlaceholders(value, placeholders);
    for (const QString& placeholder : placeholders) {
        appendCanonicalWorkflowSlotUnique(slots, placeholder);
    }
}

QStringList workflowTemplateReferencedSlots(const QJsonObject& workflow)
{
    QStringList slots;

    const QJsonArray batches = workflow.value("executionBatches").toArray();
    for (const QJsonValue& batchValue : batches) {
        const QJsonObject batch = batchValue.toObject();
        collectWorkflowSlotRefsFromValue(batch.value("condition"), slots);
        for (const QJsonValue& stepValue : batch.value("steps").toArray()) {
            const QJsonObject step = stepValue.toObject();
            collectWorkflowSlotRefsFromValue(step.value("condition"), slots);
            collectWorkflowSlotRefsFromValue(step.value("paramsTemplate"), slots);
        }
    }

    for (const QJsonValue& stepValue : workflow.value("steps").toArray()) {
        const QJsonObject step = stepValue.toObject();
        collectWorkflowSlotRefsFromValue(step.value("condition"), slots);
        collectWorkflowSlotRefsFromValue(step.value("paramsTemplate"), slots);
    }

    QHash<QString, QStringList> derivedDependencies;
    for (const QJsonValue& value : workflow.value("derivedValues").toArray()) {
        const QJsonObject derived = value.toObject();
        const QString name = canonicalWorkflowSlot(derived.value("name").toString());
        if (name.isEmpty()) {
            continue;
        }
        QStringList dependencies;
        for (const QJsonValue& dependencyValue : derived.value("dependsOn").toArray()) {
            appendCanonicalWorkflowSlotUnique(dependencies, workflowSlotNameFromValue(dependencyValue));
        }
        derivedDependencies.insert(name, dependencies);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = derivedDependencies.constBegin(); it != derivedDependencies.constEnd(); ++it) {
            if (!slots.contains(it.key())) {
                continue;
            }
            for (const QString& dependency : it.value()) {
                if (!dependency.isEmpty() && !slots.contains(dependency)) {
                    slots << dependency;
                    changed = true;
                }
            }
        }
    }

    slots.removeDuplicates();
    return slots;
}

QStringList workflowExecutionReferencedSlots(const QJsonObject& workflow)
{
    QStringList slots = workflowTemplateReferencedSlots(workflow);

    for (const QJsonValue& value : workflow.value("requiredSlots").toArray()) {
        const QJsonObject slot = value.toObject();
        if (slot.value("requiredForExecution").toBool(false)) {
            appendCanonicalWorkflowSlotUnique(slots, workflowSlotNameFromValue(value));
        }
    }

    slots.removeDuplicates();
    return slots;
}

QJsonObject workflowWithPrunedRequiredSlots(QJsonObject workflow)
{
    if (!workflowHasExecutableTrainingContent(workflow)) {
        return workflow;
    }

    const QStringList referencedSlots = workflowExecutionReferencedSlots(workflow);
    QJsonArray pruned;
    for (const QJsonValue& value : workflow.value("requiredSlots").toArray()) {
        const QString canonical = canonicalWorkflowSlot(workflowSlotNameFromValue(value));
        if (!canonical.isEmpty() && referencedSlots.contains(canonical)) {
            pruned.append(value);
        }
    }
    workflow.insert(QStringLiteral("requiredSlots"), pruned);
    return workflow;
}

QStringList canonicalWorkflowSlotsFromArray(const QJsonArray& values)
{
    QStringList slots;
    for (const QJsonValue& value : values) {
        const QString canonical = canonicalWorkflowSlot(workflowSlotNameFromValue(value));
        if (!canonical.isEmpty() && !slots.contains(canonical)) {
            slots << canonical;
        }
    }
    return slots;
}

QStringList expectedWorkflowSlots(const QJsonArray& missing, const QJsonObject& workflow)
{
    QStringList slots = canonicalWorkflowSlotsFromArray(missing);
    if (slots.isEmpty()) {
        slots = canonicalWorkflowSlotsFromArray(workflow.value("requiredSlots").toArray());
    }
    return slots;
}

bool extractLooseWorkflowNumber(QString text, double* value, QString* unit)
{
    if (!value) {
        return false;
    }
    text = workflowTrainingSearchText(text);
    const QRegularExpression pattern(
        QStringLiteral(R"((-?\d+(?:\.\d+)?)\s*(mm|cm|m2|m|qm|quadratmeter)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const double parsed = match.captured(1).toDouble(&ok);
    if (!ok || parsed <= 0.0) {
        return false;
    }
    *value = parsed;
    if (unit) {
        *unit = match.captured(2).trimmed();
    }
    return true;
}

bool extractWorkflowNumberForAliases(const QString& text, const QStringList& aliases, double* value, QString* unit)
{
    if (!value) {
        return false;
    }
    const QString normalized = workflowTrainingSearchText(text);
    for (const QString& alias : aliases) {
        const QString key = QRegularExpression::escape(workflowTrainingSearchText(alias));
        const QRegularExpression keyedAfterPattern(
            QStringLiteral(R"((?:^|\b)%1\b[^\d-]{0,48}(-?\d+(?:\.\d+)?)\s*(mm|cm|m2|m|qm|quadratmeter)?)").arg(key),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch match = keyedAfterPattern.match(normalized);
        if (!match.hasMatch()) {
            const QRegularExpression keyedBeforePattern(
                QStringLiteral(R"((-?\d+(?:\.\d+)?)\s*(mm|cm|m2|m|qm|quadratmeter)?[^\n.;:]{0,48}(?:^|\b)%1\b)").arg(key),
                QRegularExpression::CaseInsensitiveOption);
            match = keyedBeforePattern.match(normalized);
        }
        if (!match.hasMatch()) {
            continue;
        }
        bool ok = false;
        const double parsed = match.captured(1).toDouble(&ok);
        if (!ok || parsed <= 0.0) {
            continue;
        }
        *value = parsed;
        if (unit) {
            *unit = match.captured(2).trimmed();
        }
        return true;
    }
    return false;
}

QJsonValue workflowTrainingNumberValue(const QString& canonicalSlot, double value, const QString& unit)
{
    const QString normalizedUnit = workflowTrainingSearchText(unit);
    if (canonicalSlot.endsWith(QStringLiteral("Mm"))) {
        if (normalizedUnit == QStringLiteral("m")) {
            value *= 1000.0;
        } else if (normalizedUnit == QStringLiteral("cm")) {
            value *= 10.0;
        } else if (normalizedUnit.isEmpty()) {
            if (canonicalSlot == QStringLiteral("wallThicknessMm")) {
                if (value <= 2.0) {
                    value *= 1000.0;
                } else if (value <= 100.0) {
                    value *= 10.0;
                }
            } else if (value <= 50.0) {
                value *= 1000.0;
            }
        }
    } else if (canonicalSlot == QStringLiteral("roomAreaM2")) {
        if (normalizedUnit == QStringLiteral("mm2")) {
            value /= 1000000.0;
        } else if (normalizedUnit == QStringLiteral("cm2")) {
            value /= 10000.0;
        }
    }
    return QJsonValue(value);
}

double workflowVectorNumberToMm(double value, const QString& unit)
{
    const QString normalizedUnit = workflowTrainingSearchText(unit);
    if (normalizedUnit == QStringLiteral("m")) {
        return value * 1000.0;
    }
    if (normalizedUnit == QStringLiteral("cm")) {
        return value * 10.0;
    }
    return value;
}

QJsonObject workflowMoveVectorFromPrompt(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    if (!textMentionsAny(normalized, {
            QStringLiteral("vektor"),
            QStringLiteral("vector"),
            QStringLiteral("verschieb"),
            QStringLiteral("bewegen"),
            QStringLiteral("move"),
            QStringLiteral("offset"),
            QStringLiteral("relativ"),
        })) {
        return {};
    }

    QJsonObject vector;
    auto extractAxis = [&](const QString& axis) {
        const QRegularExpression pattern(
            QStringLiteral(R"(\b%1\s*[:=]\s*(-?\d+(?:\.\d+)?)\s*(mm|cm|m)?)").arg(axis),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = pattern.match(normalized);
        if (!match.hasMatch()) {
            return;
        }
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        if (ok) {
            vector.insert(axis, workflowVectorNumberToMm(value, match.captured(2)));
        }
    };

    extractAxis(QStringLiteral("x"));
    extractAxis(QStringLiteral("y"));
    extractAxis(QStringLiteral("z"));
    if (vector.isEmpty()) {
        return {};
    }
    if (!vector.contains(QStringLiteral("x"))) {
        vector.insert(QStringLiteral("x"), 0);
    }
    if (!vector.contains(QStringLiteral("y"))) {
        vector.insert(QStringLiteral("y"), 0);
    }
    if (!vector.contains(QStringLiteral("z"))) {
        vector.insert(QStringLiteral("z"), 0);
    }
    if (qFuzzyIsNull(vector.value(QStringLiteral("x")).toDouble())
        && qFuzzyIsNull(vector.value(QStringLiteral("y")).toDouble())
        && qFuzzyIsNull(vector.value(QStringLiteral("z")).toDouble())) {
        return {};
    }
    return vector;
}

QJsonObject workflowTrainingSlotValuesFromPrompt(
    const QString& prompt,
    const QJsonArray& missing,
    const QJsonObject& workflow)
{
    QJsonObject slots;
    QStringList targets = expectedWorkflowSlots(missing, workflow);
    const QStringList commonTargets{
        QStringLiteral("wallThicknessMm"),
        QStringLiteral("wallHeightMm"),
        QStringLiteral("roomWidthMm"),
        QStringLiteral("roomLengthMm"),
        QStringLiteral("roomAreaM2"),
    };
    for (const QString& target : commonTargets) {
        if (!targets.contains(target)) {
            targets << target;
        }
    }

    for (const QString& target : targets) {
        double number = 0.0;
        QString unit;
        if (extractWorkflowNumberForAliases(prompt, workflowSlotAliases(target), &number, &unit)) {
            slots.insert(target, workflowTrainingNumberValue(target, number, unit));
        }
    }

    const QString normalized = workflowTrainingSearchText(prompt);
    if (!slots.contains(QStringLiteral("layerName"))) {
        QRegularExpression layerPattern(
            QStringLiteral(R"(layer\s*(?:name)?\s*[:=]\s*[\"']?([^\"'\n\r.;,]{1,48})[\"']?)"),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch match = layerPattern.match(prompt);
        if (!match.hasMatch()) {
            layerPattern = QRegularExpression(
                QStringLiteral(R"(layer\s*(?:name)?\s*[\"']([^\"'\n\r]{1,48})[\"'])"),
                QRegularExpression::CaseInsensitiveOption);
            match = layerPattern.match(prompt);
        }
        if (match.hasMatch()) {
            const QString layerName = match.captured(1).trimmed();
            if (!layerName.isEmpty()) {
                slots.insert(QStringLiteral("layerName"), layerName);
            }
        }
    }
    if (!slots.contains(QStringLiteral("classification"))
        && (normalized.contains(QStringLiteral("bimwand"))
            || normalized.contains(QStringLiteral("bim wall"))
            || normalized.contains(QStringLiteral("bim-wall"))
            || normalized.contains(QStringLiteral("als bim"))
            || normalized.contains(QStringLiteral("klassifiz")))) {
        slots.insert(QStringLiteral("classification"), QStringLiteral("BIMWall"));
    }
    const QJsonObject moveVector = workflowMoveVectorFromPrompt(prompt);
    if (!moveVector.isEmpty()) {
        slots.insert(QStringLiteral("moveVector"), moveVector);
        slots.insert(QStringLiteral("vector"), moveVector);
        slots.insert(QStringLiteral("offset"), moveVector);
    }
    if (textMentionsAny(normalized, {
            QStringLiteral("relativ"),
            QStringLiteral("relative"),
            QStringLiteral("vektor"),
            QStringLiteral("vector"),
            QStringLiteral("offset"),
            QStringLiteral("verschiebungsvektor"),
        })) {
        slots.insert(QStringLiteral("moveMode"), QStringLiteral("relativeVector"));
        slots.insert(QStringLiteral("movementMode"), QStringLiteral("relativeVector"));
    }

    const QStringList unresolvedTargets = expectedWorkflowSlots(missing, workflow);
    if (unresolvedTargets.size() == 1 && !slots.contains(unresolvedTargets.first())) {
        double number = 0.0;
        QString unit;
        if (extractLooseWorkflowNumber(prompt, &number, &unit)) {
            slots.insert(unresolvedTargets.first(), workflowTrainingNumberValue(unresolvedTargets.first(), number, unit));
        }
    }

    return slots;
}

bool workflowTrainingQuestionNeedsAnswer(const QString& question, const QJsonObject& knownSlots)
{
    const QString normalized = workflowTrainingSearchText(question);
    for (auto it = knownSlots.begin(); it != knownSlots.end(); ++it) {
        const QString canonical = canonicalWorkflowSlot(it.key());
        for (const QString& alias : workflowSlotAliases(canonical)) {
            if (normalized.contains(alias)) {
                return false;
            }
        }
    }
    return true;
}

bool isGenericWorkflowAskUserText(const QString& text)
{
    const QString normalized = workflowTrainingSearchText(repairMojibakeText(text)).trimmed();
    return normalized.isEmpty()
        || normalized == QStringLiteral("kurze rueckfrage")
        || normalized == QStringLiteral("kurze ruckfrage")
        || normalized == QStringLiteral("rueckfrage")
        || normalized == QStringLiteral("ruckfrage")
        || normalized == QStringLiteral("frage")
        || normalized == QStringLiteral("konkrete fachliche frage")
        || normalized == QStringLiteral("gezielte rueckfrage ohne platzhaltertext");
}

bool workflowTrainingPromptDelegatesChoices(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    return normalized.contains(QStringLiteral("selber"))
        || normalized.contains(QStringLiteral("selbst"))
        || normalized.contains(QStringLiteral("automatisch"))
        || normalized.contains(QStringLiteral("sinnvoll"))
        || normalized.contains(QStringLiteral("geeignet"))
        || normalized.contains(QStringLiteral("schlage"))
        || normalized.contains(QStringLiteral("vorschlag"))
        || normalized.contains(QStringLiteral("waehle"))
        || normalized.contains(QStringLiteral("wahle"))
        || normalized.contains(QStringLiteral("entscheide"))
        || normalized.contains(QStringLiteral("nutze dafuer"))
        || normalized.contains(QStringLiteral("nutze dafÃƒÂ¼r"))
        || normalized.contains(QStringLiteral("nach tga"))
        || normalized.contains(QStringLiteral("tga themen"))
        || normalized.contains(QStringLiteral("tga gruppen"));
}

bool workflowTrainingAskUserRequestsDelegatedValues(const QJsonObject& reply, const QString& prompt)
{
    if (!workflowTrainingPromptDelegatesChoices(prompt)) {
        return false;
    }

    QStringList parts;
    parts << reply.value(QStringLiteral("message")).toString();
    for (const QJsonValue& value : reply.value(QStringLiteral("missing")).toArray()) {
        parts << workflowSlotNameFromValue(value);
    }
    for (const QJsonValue& value : reply.value(QStringLiteral("questions")).toArray()) {
        parts << value.toString();
    }
    const QString combined = workflowTrainingSearchText(parts.join(QLatin1Char('\n')));
    const bool asksForNamesOrColors = textMentionsAny(combined, {
        QStringLiteral("layername"),
        QStringLiteral("layernamen"),
        QStringLiteral("layer name"),
        QStringLiteral("layer names"),
        QStringLiteral("name"),
        QStringLiteral("namen"),
        QStringLiteral("gruppe"),
        QStringLiteral("gruppen"),
        QStringLiteral("farbe"),
        QStringLiteral("farben"),
        QStringLiteral("color"),
        QStringLiteral("aci"),
        QStringLiteral("farbwert"),
        QStringLiteral("liste"),
    });
    const bool promptAllowsDomainDefaults = textMentionsAny(workflowTrainingSearchText(prompt), {
        QStringLiteral("tga"),
        QStringLiteral("themen"),
        QStringLiteral("gruppen"),
        QStringLiteral("sinnvoll"),
        QStringLiteral("geeignet"),
    });
    return asksForNamesOrColors && promptAllowsDomainDefaults;
}

QString workflowRepairDialogText(const QJsonArray& dialog)
{
    QStringList lines;
    for (const QJsonValue& value : dialog) {
        const QJsonObject item = value.toObject();
        const QString speaker = repairMojibakeText(item.value(QStringLiteral("speaker")).toString()).trimmed();
        const QString message = repairMojibakeText(item.value(QStringLiteral("message")).toString()).trimmed();
        if (!message.isEmpty()) {
            lines << QStringLiteral("%1: %2").arg(speaker.isEmpty() ? QStringLiteral("dialog") : speaker, message);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

bool workflowStepRepairQuestionAlreadyAnswered(
    const QString& questionText,
    const QString& latestUserFeedback,
    const QJsonArray& repairDialog)
{
    const QString ask = workflowTrainingSearchText(questionText);
    const QString latest = workflowTrainingSearchText(latestUserFeedback);
    const QString dialog = workflowTrainingSearchText(workflowRepairDialogText(repairDialog));
    const QString combined = latest + QLatin1Char('\n') + dialog;

    if (latest.contains(QStringLiteral("du hast die antwort"))
        || latest.contains(QStringLiteral("antwort bereits"))
        || latest.contains(QStringLiteral("bereits bekommen"))) {
        return true;
    }

    if (ask.contains(QStringLiteral("mittelpunkt"))
        && (combined.contains(QStringLiteral("eigenen mittelpunkt"))
            || combined.contains(QStringLiteral("jeweiligen mittelpunkt"))
            || combined.contains(QStringLiteral("seinen eigenen mittelpunkt"))
            || combined.contains(QStringLiteral("eigenen ursprung"))
            || combined.contains(QStringLiteral("jeweiligen ursprung")))) {
        return true;
    }

    if ((ask.contains(QStringLiteral("separat"))
            || ask.contains(QStringLiteral("einzeln"))
            || ask.contains(QStringLiteral("jeder solid"))
            || ask.contains(QStringLiteral("jedes solid"))
            || ask.contains(QStringLiteral("separaten rotate")))
        && (combined.contains(QStringLiteral("separater rotate"))
            || combined.contains(QStringLiteral("separate rotate"))
            || combined.contains(QStringLiteral("separat ausfuehren"))
            || combined.contains(QStringLiteral("separat ausfÃƒÂ¼hren"))
            || combined.contains(QStringLiteral("einzeln ausfuehren"))
            || combined.contains(QStringLiteral("einzeln ausfÃƒÂ¼hren")))) {
        return true;
    }

    if ((ask.contains(QStringLiteral("gemeinsamen punkt"))
            || ask.contains(QStringLiteral("gemeinsamer punkt"))
            || ask.contains(QStringLiteral("mittelpunkt der gesamten auswahl"))
            || ask.contains(QStringLiteral("gesamten auswahlsatz")))
        && (combined.contains(QStringLiteral("eigenen mittelpunkt"))
            || combined.contains(QStringLiteral("jeweiligen mittelpunkt"))
            || combined.contains(QStringLiteral("eigenen ursprung"))
            || combined.contains(QStringLiteral("jeweiligen ursprung"))
            || combined.contains(QStringLiteral("separater rotate")))) {
        return true;
    }

    return false;
}

bool workflowRunStepRemovalRequested(const QString& prompt, int currentIndex, int actionCount, int* targetIndex)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    const bool removeIntent = textMentionsAny(normalized, {
        QStringLiteral("entfern"),
        QStringLiteral("loesch"),
        QStringLiteral("loesche"),
        QStringLiteral("loeschen"),
        QStringLiteral("loschen"),
        QStringLiteral("lÃƒÂ¶schen"),
        QStringLiteral("ueberspring"),
        QStringLiteral("ÃƒÂ¼berspring"),
        QStringLiteral("streichen"),
        QStringLiteral("weg lassen"),
        QStringLiteral("weglassen"),
        QStringLiteral("ueberfluessig"),
        QStringLiteral("ÃƒÂ¼berflÃƒÂ¼ssig"),
        QStringLiteral("nicht benoetigt"),
        QStringLiteral("nicht benÃƒÂ¶tigt"),
    });
    if (!removeIntent) {
        return false;
    }

    int requestedIndex = currentIndex;
    const QRegularExpression explicitStepPattern(
        QStringLiteral(R"((?:workflow[\s_-]*schritt|schritt|aktion)\s*(\d+))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch explicitMatch = explicitStepPattern.match(normalized);
    if (explicitMatch.hasMatch()) {
        requestedIndex = explicitMatch.captured(1).toInt() - 1;
    } else {
        const QRegularExpression anyNumberPattern(QStringLiteral(R"(\b(\d+)\b)"));
        const QRegularExpressionMatch numberMatch = anyNumberPattern.match(normalized);
        if (numberMatch.hasMatch()) {
            requestedIndex = numberMatch.captured(1).toInt() - 1;
        }
    }

    if (requestedIndex < 0 || requestedIndex >= actionCount) {
        return false;
    }
    if (targetIndex) {
        *targetIndex = requestedIndex;
    }
    return true;
}

bool workflowRepairFeedbackRequestsStepInsertion(const QString& text)
{
    const QString normalized = workflowTrainingSearchText(text);
    if (normalized.contains(QStringLiteral("statt"))
        || normalized.contains(QStringLiteral("ersetze"))
        || normalized.contains(QStringLiteral("ersetzen"))
        || normalized.contains(QStringLiteral("austauschen"))) {
        return false;
    }
    if (normalized.contains(QStringLiteral("im naechsten schritt"))
        || normalized.contains(QStringLiteral("im nÃƒÂ¤chsten schritt"))
        || normalized.contains(QStringLiteral("naechster schritt"))
        || normalized.contains(QStringLiteral("nÃƒÂ¤chster schritt"))
        || normalized.contains(QStringLiteral("zusaetzlich"))
        || normalized.contains(QStringLiteral("zusÃƒÂ¤tzlich"))
        || normalized.contains(QStringLiteral("hinzufueg"))
        || normalized.contains(QStringLiteral("hinzufÃƒÂ¼g"))
        || normalized.contains(QStringLiteral("einfueg"))
        || normalized.contains(QStringLiteral("einfÃƒÂ¼g"))) {
        return true;
    }
    return (normalized.contains(QStringLiteral("erst "))
            || normalized.contains(QStringLiteral("zuerst"))
            || normalized.contains(QStringLiteral("vorher"))
            || normalized.contains(QStringLiteral("davor")))
        && (normalized.contains(QStringLiteral("danach"))
            || normalized.contains(QStringLiteral("anschliessend"))
            || normalized.contains(QStringLiteral("anschlieÃƒÅ¸end"))
            || normalized.contains(QStringLiteral("naechsten schritt"))
            || normalized.contains(QStringLiteral("nÃƒÂ¤chsten schritt")));
}

bool workflowRepairInsertionBeforeCurrent(const QString& userFeedback, const QJsonObject& reply)
{
    const QString position = workflowTrainingSearchText(reply.value(QStringLiteral("position")).toString(
        reply.value(QStringLiteral("insertPosition")).toString()));
    if (position.contains(QStringLiteral("after"))
        || position.contains(QStringLiteral("nach"))
        || position.contains(QStringLiteral("danach"))
        || position.contains(QStringLiteral("after_current"))) {
        return false;
    }
    if (position.contains(QStringLiteral("before"))
        || position.contains(QStringLiteral("vor"))
        || position.contains(QStringLiteral("davor"))
        || position.contains(QStringLiteral("before_current"))) {
        return true;
    }
    const QString normalized = workflowTrainingSearchText(userFeedback);
    if (normalized.contains(QStringLiteral("danach"))
        && !normalized.contains(QStringLiteral("erst"))
        && !normalized.contains(QStringLiteral("zuerst"))) {
        return false;
    }
    return true;
}

bool workflowTrainingBrxFailureIsRuntimeDependent(const QString& message)
{
    const QString normalized = workflowTrainingSearchText(message);
    return (normalized.contains(QStringLiteral("selector"))
            || normalized.contains(QStringLiteral("selection"))
            || normalized.contains(QStringLiteral("auswahl")))
        && (normalized.contains(QStringLiteral("keine objekte"))
            || normalized.contains(QStringLiteral("findet keine"))
            || normalized.contains(QStringLiteral("nicht gefunden"))
            || normalized.contains(QStringLiteral("no objects"))
            || normalized.contains(QStringLiteral("not found")));
}

bool actionCanCreateRuntimeSelectionTarget(const QJsonObject& action)
{
    const QString tool = action.value("tool").toString().trimmed();
    if (tool == QStringLiteral("geometry.create")
        || tool == QStringLiteral("rectangles.extrude")
        || tool == QStringLiteral("circle.extrude")
        || tool == QStringLiteral("profile.extrude")
        || tool == QStringLiteral("bim.create")
        || tool == QStringLiteral("bim.objects.query")) {
        return true;
    }
    if (tool == QStringLiteral("selection.set")
        || tool == QStringLiteral("bim.selection.set")) {
        return true;
    }
    return false;
}

bool actionDependsOnRuntimeSelectionTarget(const QJsonObject& action)
{
    const QString tool = action.value("tool").toString().trimmed();
    const QJsonObject params = action.value("params").toObject();
    const QString target = params.value("target").toString().trimmed().toLower();
    const QJsonObject selector = params.value("selector").toObject();
    const QString scope = selector.value("scope").toString().trimmed().toLower();

    if (params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPointsFromLastQuery")).toBool(false)
        || params.value(QStringLiteral("autoCreatedGeometryHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPolylineHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPolylineHandlesFromLastQuery")).toBool(false)
        || params.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
        || params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
        || params.contains(QStringLiteral("createdGeometryHandleIndex"))
        || !params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray().isEmpty()) {
        return true;
    }

    if (target == QStringLiteral("lastresult")
        || target == QStringLiteral("last_result")
        || target == QStringLiteral("lastextruded")
        || target == QStringLiteral("last_extruded")
        || target == QStringLiteral("selection")) {
        return true;
    }
    if (scope == QStringLiteral("lastresult")
        || scope == QStringLiteral("last_result")
        || scope == QStringLiteral("lastextruded")
        || scope == QStringLiteral("last_extruded")
        || scope == QStringLiteral("selection")
        || scope == QStringLiteral("currentspace")
        || scope == QStringLiteral("current_space")) {
        return tool == QStringLiteral("bim.classify")
            || tool == QStringLiteral("brx.sdk.bim.classification.set")
            || tool == QStringLiteral("bim.selection.set")
            || tool == QStringLiteral("rectangles.extrude")
            || tool == QStringLiteral("circle.extrude")
            || tool == QStringLiteral("profile.extrude")
            || tool.startsWith(QStringLiteral("geometry."))
            || tool.startsWith(QStringLiteral("brx.sdk.entity."))
            || tool == QStringLiteral("brx.sdk.blockReference.setPosition");
    }
    return false;
}

bool proposalPreflightFailureCanBeDeferred(const QJsonArray& actions, const QString& message)
{
    if (actions.size() < 2 || !workflowTrainingBrxFailureIsRuntimeDependent(message)) {
        return false;
    }

    bool priorCreatesRuntimeTarget = false;
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        if (priorCreatesRuntimeTarget && actionDependsOnRuntimeSelectionTarget(action)) {
            return true;
        }
        if (actionCanCreateRuntimeSelectionTarget(action)) {
            priorCreatesRuntimeTarget = true;
        }
    }
    return false;
}

QString layerNameFromProposalActions(const QJsonObject& proposal)
{
    const QJsonArray actions = proposal.value(QStringLiteral("actions")).toArray();
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString();
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (tool == QStringLiteral("layers.create")) {
            const QString name = params.value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) {
                return name;
            }
        }
    }
    for (const QJsonValue& value : actions) {
        const QJsonObject params = value.toObject().value(QStringLiteral("params")).toObject();
        const QString layer = params.value(QStringLiteral("layer")).toString().trimmed();
        if (!layer.isEmpty()) {
            return layer;
        }
    }
    return {};
}

bool promptLooksLikeRectangularRoomWallRun(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    return promptDescribesArchitecturalWalls(normalized)
        && textMentionsAny(normalized, {
               QStringLiteral("4 wand"),
               QStringLiteral("4 waende"),
               QStringLiteral("4 aussenwand"),
               QStringLiteral("4 aussenwaende"),
               QStringLiteral("4 wÃƒÂ¤nde"),
               QStringLiteral("vier wand"),
               QStringLiteral("vier waende"),
               QStringLiteral("vier aussenwand"),
               QStringLiteral("vier aussenwaende"),
               QStringLiteral("vier wÃƒÂ¤nde"),
               QStringLiteral("rechteckiger raum"),
               QStringLiteral("quadratmeter"),
               QStringLiteral("m2"),
               QStringLiteral("qm"),
               QStringLiteral("raum bilden"),
           });
}

bool promptHasExplicitRoomDimensionPair(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    const QRegularExpression pairPattern(
        QStringLiteral(R"(\b\d+(?:\.\d+)?\s*(?:mm|cm|m)?\s*(?:x|\*)\s*\d+(?:\.\d+)?\s*(?:mm|cm|m)?\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return pairPattern.match(normalized).hasMatch()
        || (textMentionsAny(normalized, {
                QStringLiteral("raumlaenge"),
                QStringLiteral("laenge"),
                QStringLiteral("lange"),
                QStringLiteral("length"),
            })
            && textMentionsAny(normalized, {
                QStringLiteral("raumbreite"),
                QStringLiteral("breite"),
                QStringLiteral("width"),
            }));
}

QJsonObject pointObject(double x, double y, double z = 0.0)
{
    return QJsonObject{{"x", x}, {"y", y}, {"z", z}};
}

QJsonObject wallBoxAction(
    const QString& title,
    const QString& layerName,
    double originX,
    double originY,
    double width,
    double depth,
    double height)
{
    QJsonObject params{
        {"geometry", "box"},
        {"origin", pointObject(originX, originY)},
        {"width", width},
        {"depth", depth},
        {"height", height},
        {"saveBefore", false},
    };
    if (!layerName.trimmed().isEmpty()) {
        params.insert(QStringLiteral("layer"), layerName.trimmed());
    }
    return QJsonObject{
        {"tool", "geometry.create"},
        {"params", params},
        {"reason", title},
    };
}

QJsonObject normalizedRectangularRoomWallProposal(QJsonObject proposal, const QString& prompt)
{
    if (!promptLooksLikeRectangularRoomWallRun(prompt)) {
        return proposal;
    }

    const QJsonObject slots = workflowTrainingSlotValuesFromPrompt(prompt, {}, {});
    double roomLength = slots.value(QStringLiteral("roomLengthMm")).toDouble(0.0);
    double roomWidth = slots.value(QStringLiteral("roomWidthMm")).toDouble(0.0);
    const double roomAreaM2 = slots.value(QStringLiteral("roomAreaM2")).toDouble(0.0);
    double wallThickness = slots.value(QStringLiteral("wallThicknessMm")).toDouble(0.0);
    double wallHeight = slots.value(QStringLiteral("wallHeightMm")).toDouble(0.0);

    const bool hasExplicitDimensionPair = promptHasExplicitRoomDimensionPair(prompt);
    if (roomAreaM2 > 0.0 && !hasExplicitDimensionPair) {
        const double sideMm = std::sqrt(roomAreaM2) * 1000.0;
        if (sideMm > 0.0) {
            roomLength = sideMm;
            roomWidth = sideMm;
        }
    } else if (roomAreaM2 > 0.0 && roomLength > 0.0 && roomWidth <= 0.0) {
        roomWidth = (roomAreaM2 * 1000000.0) / roomLength;
    } else if (roomAreaM2 > 0.0 && roomWidth > 0.0 && roomLength <= 0.0) {
        roomLength = (roomAreaM2 * 1000000.0) / roomWidth;
    }

    if (wallThickness <= 0.0 && roomAreaM2 > 0.0) {
        wallThickness = 240.0;
    }
    if (wallHeight <= 0.0 && roomAreaM2 > 0.0) {
        wallHeight = 3000.0;
    }
    if (roomLength <= 0.0 || roomWidth <= 0.0 || wallThickness <= 0.0 || wallHeight <= 0.0) {
        return proposal;
    }

    QString layerName = slots.value(QStringLiteral("layerName")).toString().trimmed();
    if (layerName.isEmpty()) {
        layerName = layerNameFromProposalActions(proposal);
    }
    if (layerName.isEmpty()) {
        layerName = QStringLiteral("Raum - Waende");
    }

    const double outerLength = roomLength + (2.0 * wallThickness);
    QJsonArray actions;
    if (!layerName.isEmpty()) {
        actions.append(QJsonObject{
            {"tool", "layers.create"},
            {"params", QJsonObject{{"name", layerName}, {"saveBefore", false}}},
            {"reason", QString("Layer \"%1\" vorbereiten").arg(layerName)},
        });
    }
    actions.append(wallBoxAction(QStringLiteral("Suedwand erstellen"), layerName, -wallThickness, -wallThickness, outerLength, wallThickness, wallHeight));
    actions.append(wallBoxAction(QStringLiteral("Nordwand erstellen"), layerName, -wallThickness, roomWidth, outerLength, wallThickness, wallHeight));
    actions.append(wallBoxAction(QStringLiteral("Westwand erstellen"), layerName, -wallThickness, 0.0, wallThickness, roomWidth, wallHeight));
    actions.append(wallBoxAction(QStringLiteral("Ostwand erstellen"), layerName, roomLength, 0.0, wallThickness, roomWidth, wallHeight));

    if (promptAllowsBimWallClassification(prompt)) {
        actions.append(QJsonObject{
            {"tool", "bim.classify"},
            {"params", QJsonObject{
                {"classification", "BIMWall"},
                {"target", "lastExtruded"},
                {"autoHandlesFromBatch", true},
                {"saveBefore", false},
            }},
            {"reason", "neu erzeugte Wand-Solids als BIMWall klassifizieren"},
            {"_validationContext", "BIM-Wand-Klassifizierung fuer neu erzeugte architektonische Aussenwaende"},
        });
    }

    proposal.insert(QStringLiteral("actions"), actions);
    proposal.insert(QStringLiteral("requiresConfirmation"), true);
    proposal.insert(QStringLiteral("continueAfterSuccess"), false);
    proposal.remove(QStringLiteral("nextIntent"));
    proposal.insert(QStringLiteral("summary"),
        QString("Ich lege %1vier korrekt anschliessende Wandkoerper fuer einen Innenraum %2 x %3 mm mit %4 mm Wandstaerke und %5 mm Hoehe an%6.")
            .arg(layerName.isEmpty() ? QString() : QString("den Layer \"%1\" an und ").arg(layerName))
            .arg(roomLength)
            .arg(roomWidth)
            .arg(wallThickness)
            .arg(wallHeight)
            .arg(promptAllowsBimWallClassification(prompt) ? QStringLiteral(" und klassifiziere sie als BIM-Waende") : QString()));
    proposal.insert(QStringLiteral("normalizedByQt"), QStringLiteral("rectangular_room_walls_v1"));
    proposal.insert(QStringLiteral("calculation"), QJsonObject{
        {"roomLengthMm", roomLength},
        {"roomWidthMm", roomWidth},
        {"roomAreaM2", roomAreaM2},
        {"interiorAreaCheckM2", (roomLength * roomWidth) / 1000000.0},
        {"areaAssumption", roomAreaM2 > 0.0 && !hasExplicitDimensionPair
                ? QStringLiteral("Quadratischer Innenraum aus angegebener Flaeche abgeleitet.")
                : QStringLiteral("Innenmasse aus Prompt oder Slots uebernommen.")},
        {"wallThicknessMm", wallThickness},
        {"wallHeightMm", wallHeight},
        {"outerLengthMm", outerLength},
        {"southWall", QJsonObject{{"origin", pointObject(-wallThickness, -wallThickness)}, {"width", outerLength}, {"depth", wallThickness}}},
        {"northWall", QJsonObject{{"origin", pointObject(-wallThickness, roomWidth)}, {"width", outerLength}, {"depth", wallThickness}}},
        {"westWall", QJsonObject{{"origin", pointObject(-wallThickness, 0.0)}, {"width", wallThickness}, {"depth", roomWidth}}},
        {"eastWall", QJsonObject{{"origin", pointObject(roomLength, 0.0)}, {"width", wallThickness}, {"depth", roomWidth}}},
    });
    return proposal;
}

QJsonArray createdGeometryHandlesFromBatchResults(const QJsonArray& results)
{
    QJsonArray handles;
    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.create")) {
            continue;
        }
        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        const QString handle = result.value(QStringLiteral("handle")).toString().trimmed();
        if (!handle.isEmpty()) {
            handles.append(handle);
        }
        for (const QJsonValue& handleValue : result.value(QStringLiteral("affectedHandles")).toArray()) {
            const QString affectedHandle = handleValue.toString().trimmed();
            if (!affectedHandle.isEmpty()) {
                handles.append(affectedHandle);
            }
        }
    }
    return handles;
}

QJsonArray selectedArrayValues(const QJsonArray& values, const QJsonArray& indexes)
{
    if (indexes.isEmpty()) {
        return values;
    }

    QJsonArray selected;
    for (const QJsonValue& indexValue : indexes) {
        const int index = indexValue.toInt(-1);
        if (index >= 0 && index < values.size()) {
            selected.append(values.at(index));
        }
    }
    return selected;
}

QJsonArray createdGeometryHandlesOfTypeFromBatchResults(const QJsonArray& results, const QString& geometryType)
{
    QJsonArray handles;
    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.create")) {
            continue;
        }

        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        const QString createdType = result.value(QStringLiteral("geometry")).toString(
            item.value(QStringLiteral("params")).toObject().value(QStringLiteral("geometry")).toString());
        if (createdType.compare(geometryType, Qt::CaseInsensitive) != 0) {
            continue;
        }

        const QString handle = result.value(QStringLiteral("handle")).toString().trimmed();
        if (!handle.isEmpty()) {
            handles.append(handle);
        }
        for (const QJsonValue& handleValue : result.value(QStringLiteral("affectedHandles")).toArray()) {
            const QString affectedHandle = handleValue.toString().trimmed();
            if (!affectedHandle.isEmpty()) {
                handles.append(affectedHandle);
            }
        }
    }
    return handles;
}

QJsonArray latestContiguousCreatedGeometryHandlesOfType(const QJsonArray& results, const QString& geometryType)
{
    QJsonArray handles;
    for (int i = results.size() - 1; i >= 0; --i) {
        const QJsonObject item = results.at(i).toObject();
        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        const QString createdType = result.value(QStringLiteral("geometry")).toString(
            item.value(QStringLiteral("params")).toObject().value(QStringLiteral("geometry")).toString());
        const bool matches = item.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.create")
            && createdType.compare(geometryType, Qt::CaseInsensitive) == 0;
        if (!matches) {
            if (!handles.isEmpty()) {
                break;
            }
            continue;
        }

        QJsonArray itemHandles;
        const QString handle = result.value(QStringLiteral("handle")).toString().trimmed();
        if (!handle.isEmpty()) {
            itemHandles.append(handle);
        }
        for (const QJsonValue& handleValue : result.value(QStringLiteral("affectedHandles")).toArray()) {
            const QString affectedHandle = handleValue.toString().trimmed();
            if (!affectedHandle.isEmpty()) {
                itemHandles.append(affectedHandle);
            }
        }
        for (int handleIndex = itemHandles.size() - 1; handleIndex >= 0; --handleIndex) {
            handles.prepend(itemHandles.at(handleIndex));
        }
    }
    return handles;
}

QJsonArray selectedQueryPoints(const QJsonArray& points, const QJsonArray& indexes)
{
    QJsonArray selected = selectedArrayValues(points, indexes);
    if (selected.size() >= 2 || indexes.isEmpty() || points.size() < 2) {
        return selected;
    }

    // Local models occasionally keep the global point offset when referring to
    // the latest per-system query. Normalize those indexes to the query-local
    // point array instead of silently producing an empty POLYLINE.
    QJsonArray normalizedIndexes;
    for (const QJsonValue& indexValue : indexes) {
        const int index = indexValue.toInt(-1);
        if (index < 0) {
            continue;
        }
        normalizedIndexes.append(index % points.size());
    }
    selected = selectedArrayValues(points, normalizedIndexes);
    return selected;
}

QJsonArray pointsFromLatestGeometryQuery(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.query")) {
            continue;
        }

        QJsonArray points;
        const QJsonArray objects = item.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("objects")).toArray();
        for (const QJsonValue& objectValue : objects) {
            const QJsonObject object = objectValue.toObject();
            QJsonObject point = object.value(QStringLiteral("geometry")).toObject()
                                    .value(QStringLiteral("position")).toObject();
            if (point.isEmpty()) {
                point = object.value(QStringLiteral("bounds")).toObject()
                            .value(QStringLiteral("min")).toObject();
            }
            if (point.isEmpty()) {
                const QString queriedHandle = object.value(QStringLiteral("handle")).toString().trimmed();
                for (int resultIndex = previousResults.size() - 1; resultIndex >= 0; --resultIndex) {
                    const QJsonObject createdItem = previousResults.at(resultIndex).toObject();
                    if (createdItem.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.create")) {
                        continue;
                    }
                    const QJsonObject createdResult = createdItem.value(QStringLiteral("result")).toObject();
                    if (createdResult.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("point"), Qt::CaseInsensitive) != 0
                        || createdResult.value(QStringLiteral("handle")).toString().trimmed() != queriedHandle) {
                        continue;
                    }
                    point = createdItem.value(QStringLiteral("params")).toObject()
                                .value(QStringLiteral("position")).toObject();
                    break;
                }
            }
            if (point.value(QStringLiteral("x")).isDouble()
                && point.value(QStringLiteral("y")).isDouble()) {
                if (!point.value(QStringLiteral("z")).isDouble()) {
                    point.insert(QStringLiteral("z"), 0.0);
                }
                points.append(point);
            }
        }
        if (!points.isEmpty()) {
            return points;
        }
    }
    return {};
}

bool toolCanUseRuntimeSelectionHandles(const QString& tool)
{
    return tool == QStringLiteral("geometry.move")
        || tool == QStringLiteral("bim.move")
        || tool == QStringLiteral("brx.sdk.entity.transformBy")
        || tool == QStringLiteral("brx.sdk.blockReference.setPosition")
        || tool == QStringLiteral("geometry.copy")
        || tool == QStringLiteral("brx.sdk.entity.copy")
        || tool == QStringLiteral("geometry.rotate")
        || tool == QStringLiteral("brx.sdk.entity.rotateBy")
        || tool == QStringLiteral("geometry.scale")
        || tool == QStringLiteral("brx.sdk.entity.scaleBy")
        || tool == QStringLiteral("geometry.delete")
        || tool == QStringLiteral("brx.sdk.entity.erase")
        || tool == QStringLiteral("entity.setLayer")
        || tool == QStringLiteral("brx.sdk.entity.setLayer")
        || tool == QStringLiteral("entity.setName")
        || tool == QStringLiteral("brx.sdk.entity.setName")
        || tool == QStringLiteral("rectangles.extrude")
        || tool == QStringLiteral("circle.extrude")
        || tool == QStringLiteral("profile.extrude")
        || tool == QStringLiteral("bim.classify")
        || tool == QStringLiteral("brx.sdk.bim.classification.set");
}

QJsonArray uniqueStringArray(const QJsonArray& values)
{
    QJsonArray unique;
    QStringList seen;
    for (const QJsonValue& value : values) {
        const QString text = value.toString().trimmed();
        if (text.isEmpty() || seen.contains(text)) {
            continue;
        }
        seen << text;
        unique.append(text);
    }
    return unique;
}

bool toolAcceptsHandleSelectorForNormalization(const QString& tool)
{
    return toolCanUseRuntimeSelectionHandles(tool)
        || tool == QStringLiteral("selection.set")
        || tool == QStringLiteral("bim.selection.set")
        || tool == QStringLiteral("geometry.query")
        || tool == QStringLiteral("measurement.bbox")
        || tool == QStringLiteral("measurement.length")
        || tool == QStringLiteral("measurement.area")
        || tool == QStringLiteral("entity.describe");
}

QJsonArray handlesFromSelectionSnapshot(const QJsonArray& selection)
{
    QJsonArray handles;
    for (const QJsonValue& value : selection) {
        const QJsonObject item = value.toObject();
        const QString handle = item.value(QStringLiteral("handle")).toString().trimmed().toUpper();
        if (!handle.isEmpty()) {
            handles.append(handle);
        }
    }
    return uniqueStringArray(handles);
}

bool looksLikeCadHandleToken(const QString& token, bool allowNumericOnly)
{
    const QString normalized = token.trimmed().toUpper();
    if (normalized.isEmpty() || normalized.size() > 16) {
        return false;
    }
    bool hasHexLetter = false;
    bool hasDigit = false;
    for (const QChar ch : normalized) {
        const ushort code = ch.unicode();
        if (code >= '0' && code <= '9') {
            hasDigit = true;
            continue;
        }
        if (code >= 'A' && code <= 'F') {
            hasHexLetter = true;
            continue;
        }
        return false;
    }
    return allowNumericOnly ? hasDigit : (hasDigit && hasHexLetter);
}

QJsonArray explicitCadHandlesFromText(const QString& text)
{
    QJsonArray handles;
    QStringList seen;
    auto appendHandle = [&](const QString& token, bool allowNumericOnly) {
        QString handle = token.trimmed().toUpper();
        handle.remove(QRegularExpression(QStringLiteral(R"(^[#=:\s]+|[,;.\s]+$)")));
        if (!looksLikeCadHandleToken(handle, allowNumericOnly) || seen.contains(handle)) {
            return;
        }
        seen << handle;
        handles.append(handle);
    };

    const QRegularExpression keywordPattern(
        QStringLiteral(R"((?:\bhandle\b|\bhandles\b|\bobjekt(?:e)?\b|\bentity\b|\bentities\b|\belement(?:e)?\b)\s*(?:id)?\s*[:=#]?\s*((?:[#]?[0-9a-fA-F]{1,16})(?:\s*(?:,|;|/|\+|\bund\b|\band\b)\s*[#]?[0-9a-fA-F]{1,16})*))"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    QRegularExpressionMatchIterator keywordMatches = keywordPattern.globalMatch(text);
    while (keywordMatches.hasNext()) {
        const QRegularExpressionMatch match = keywordMatches.next();
        const QString group = match.captured(1);
        QRegularExpressionMatchIterator tokenMatches =
            QRegularExpression(QStringLiteral(R"([0-9a-fA-F]{1,16})")).globalMatch(group);
        while (tokenMatches.hasNext()) {
            appendHandle(tokenMatches.next().captured(0), true);
        }
    }

    if (!handles.isEmpty()) {
        return handles;
    }

    const QString normalized = workflowTrainingSearchText(text);
    if (!textMentionsAny(normalized, {
            QStringLiteral("handle"),
            QStringLiteral("objekt"),
            QStringLiteral("object"),
            QStringLiteral("entity"),
            QStringLiteral("element"),
            QStringLiteral("id"),
        })) {
        return handles;
    }

    const QRegularExpression standalonePattern(
        QStringLiteral(R"(\b(?=[0-9a-fA-F]*[a-fA-F])(?=[0-9a-fA-F]*\d)[0-9a-fA-F]{2,16}\b)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator standaloneMatches = standalonePattern.globalMatch(text);
    while (standaloneMatches.hasNext()) {
        appendHandle(standaloneMatches.next().captured(0), false);
    }
    return handles;
}

QJsonObject paramsWithConcreteHandleSelector(
    const QString& tool,
    QJsonObject params,
    const QString& promptText,
    const QJsonArray& currentSelection)
{
    if (!toolAcceptsHandleSelectorForNormalization(tool)) {
        return params;
    }

    QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
    if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("handles"), Qt::CaseInsensitive) == 0
        && !selector.value(QStringLiteral("handles")).toArray().isEmpty()) {
        return params;
    }

    QJsonArray explicitHandles = explicitCadHandlesFromText(promptText);
    if (!explicitHandles.isEmpty()) {
        selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
        selector.insert(QStringLiteral("handles"), explicitHandles);
        params.insert(QStringLiteral("selector"), selector);
        params.remove(QStringLiteral("handle"));
        params.remove(QStringLiteral("handles"));
        return params;
    }

    if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("selection"), Qt::CaseInsensitive) != 0
        || selector.value(QStringLiteral("handles")).isArray()) {
        return params;
    }

    const QJsonArray selectedHandles = handlesFromSelectionSnapshot(currentSelection);
    if (selectedHandles.isEmpty()) {
        return params;
    }
    selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
    selector.insert(QStringLiteral("handles"), selectedHandles);
    params.insert(QStringLiteral("selector"), selector);
    return params;
}

QJsonArray handlesFromLatestGeometryQuery(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.query")) {
            continue;
        }
        QJsonArray handles;
        const QJsonArray objects = item.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("objects")).toArray();
        for (const QJsonValue& objectValue : objects) {
            const QString handle = objectValue.toObject().value(QStringLiteral("handle")).toString().trimmed();
            if (!handle.isEmpty()) {
                handles.append(handle);
            }
        }
        handles = uniqueStringArray(handles);
        if (!handles.isEmpty()) {
            return handles;
        }
    }
    return {};
}

QJsonArray handlesFromLatestBimQuery(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("bim.objects.query")) {
            continue;
        }
        QJsonArray handles;
        const QJsonArray objects = item.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("objects")).toArray();
        for (const QJsonValue& objectValue : objects) {
            const QString handle = objectValue.toObject().value(QStringLiteral("handle")).toString().trimmed();
            if (!handle.isEmpty()) {
                handles.append(handle);
            }
        }
        handles = uniqueStringArray(handles);
        // The newest BIM query is authoritative even when it found no targets.
        // Falling through would bind an unrelated, older query to a mutation.
        return handles;
    }
    return {};
}

QJsonArray affectedHandlesFromBatchResultItem(const QJsonObject& item);
QJsonObject paramsWithBimValidationTargets(QJsonObject params, const QJsonObject& validationAction)
{
    const QJsonArray resolvedHandles = validationAction.value(QStringLiteral("resolvedHandles")).toArray();
    const QJsonArray targetFingerprints = validationAction.value(QStringLiteral("targetFingerprints")).toArray();
    if (!resolvedHandles.isEmpty()) {
        params.insert(QStringLiteral("resolvedHandles"), resolvedHandles);
    }
    if (!targetFingerprints.isEmpty()) {
        params.insert(QStringLiteral("targetFingerprints"), targetFingerprints);
    }
    return params;
}

QJsonArray orderedRoomHandlesFromLatestGeometryQuery(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.query")) continue;
        QVector<QJsonObject> rooms;
        for (const QJsonValue& value : item.value(QStringLiteral("result")).toObject().value(QStringLiteral("objects")).toArray()) {
            const QJsonObject object = value.toObject();
            if (!object.value(QStringLiteral("handle")).toString().trimmed().isEmpty()) rooms.append(object);
        }
        std::sort(rooms.begin(), rooms.end(), [](const QJsonObject& a, const QJsonObject& b) {
            const QJsonObject amin = a.value(QStringLiteral("bounds")).toObject().value(QStringLiteral("min")).toObject();
            const QJsonObject bmin = b.value(QStringLiteral("bounds")).toObject().value(QStringLiteral("min")).toObject();
            if (std::abs(amin.value(QStringLiteral("y")).toDouble() - bmin.value(QStringLiteral("y")).toDouble()) > 1.0) return amin.value(QStringLiteral("y")).toDouble() < bmin.value(QStringLiteral("y")).toDouble();
            return amin.value(QStringLiteral("x")).toDouble() < bmin.value(QStringLiteral("x")).toDouble();
        });
        QJsonArray handles;
        for (const QJsonObject& room : rooms) handles.append(room.value(QStringLiteral("handle")));
        if (!handles.isEmpty()) return uniqueStringArray(handles);
    }
    return {};
}

QJsonArray affectedHandlesFromBatchResultItem(const QJsonObject& item)
{
    QJsonArray handles = item.value(QStringLiteral("result")).toObject().value(QStringLiteral("affectedHandles")).toArray();
    if (handles.isEmpty()) {
        handles = item.value(QStringLiteral("response")).toObject()
            .value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("affectedHandles")).toArray();
    }
    return uniqueStringArray(handles);
}

QJsonArray latestSelectionSetHandlesFromBatchResults(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        const QString tool = item.value(QStringLiteral("tool")).toString();
        if (tool != QStringLiteral("selection.set")
            && tool != QStringLiteral("bim.selection.set")) {
            continue;
        }
        const QJsonArray handles = affectedHandlesFromBatchResultItem(item);
        if (!handles.isEmpty()) {
            return handles;
        }
    }
    return {};
}

QJsonObject paramsWithRuntimeBatchHandles(const QString& tool, QJsonObject params, const QJsonArray& previousResults)
{
    if ((tool == QStringLiteral("bim.selection.set")
            || tool == QStringLiteral("bim.move"))
        && params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)) {
        const QJsonArray handles = handlesFromLatestBimQuery(previousResults);
        params.remove(QStringLiteral("autoBimHandlesFromLastQuery"));
        if (!handles.isEmpty()) {
            params.insert(QStringLiteral("selector"), QJsonObject{
                {QStringLiteral("scope"), QStringLiteral("handles")},
                {QStringLiteral("handles"), handles},
            });
        }
    }

    if (tool == QStringLiteral("geometry.query")
        && params.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)) {
        const QJsonArray allPointHandles = createdGeometryHandlesOfTypeFromBatchResults(previousResults, QStringLiteral("point"));
        QJsonArray handles = selectedArrayValues(allPointHandles, params.value(QStringLiteral("createdPointHandleIndexes")).toArray());
        if (handles.size() < 2) {
            handles = latestContiguousCreatedGeometryHandlesOfType(previousResults, QStringLiteral("point"));
        }
        params.remove(QStringLiteral("autoPointHandlesFromBatch"));
        params.remove(QStringLiteral("createdPointHandleIndexes"));
        if (!handles.isEmpty()) {
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            selector.insert(QStringLiteral("kind"), QStringLiteral("entity"));
            params.insert(QStringLiteral("selector"), selector);
        }
    }

    if (tool == QStringLiteral("geometry.query")
        && (params.value(QStringLiteral("autoCreatedGeometryHandlesFromBatch")).toBool(false)
            || params.contains(QStringLiteral("createdGeometryHandleIndex"))
            || !params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray().isEmpty())) {
        QJsonArray handles = uniqueStringArray(createdGeometryHandlesFromBatchResults(previousResults));
        const QJsonArray createdHandleIndexes = params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray();
        if (!createdHandleIndexes.isEmpty()) {
            handles = selectedArrayValues(handles, createdHandleIndexes);
        }
        const int createdHandleIndex = params.value(QStringLiteral("createdGeometryHandleIndex")).toInt(-1);
        if (createdHandleIndex >= 0) {
            QJsonArray selectedHandle;
            if (createdHandleIndex < handles.size()) {
                selectedHandle.append(handles.at(createdHandleIndex));
            }
            handles = selectedHandle;
        }
        params.remove(QStringLiteral("autoCreatedGeometryHandlesFromBatch"));
        params.remove(QStringLiteral("createdGeometryHandleIndex"));
        params.remove(QStringLiteral("createdGeometryHandleIndexes"));
        if (!handles.isEmpty()) {
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            if (selector.value(QStringLiteral("kind")).toString().trimmed().isEmpty()) {
                selector.insert(QStringLiteral("kind"), QStringLiteral("entity"));
            }
            params.insert(QStringLiteral("selector"), selector);
        }
    }

    if (tool == QStringLiteral("geometry.create")
        && params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("polyline"), Qt::CaseInsensitive) == 0
        && params.value(QStringLiteral("autoPointsFromLastQuery")).toBool(false)) {
        QJsonArray points = pointsFromLatestGeometryQuery(previousResults);
        points = selectedQueryPoints(points, params.value(QStringLiteral("queriedPointIndexes")).toArray());
        params.remove(QStringLiteral("autoPointsFromLastQuery"));
        params.remove(QStringLiteral("queriedPointIndexes"));
        if (!points.isEmpty()) {
            params.insert(QStringLiteral("points"), points);
        }
    }

    if ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
        && params.value(QStringLiteral("autoPolylineHandlesFromBatch")).toBool(false)) {
        QJsonArray handles = createdGeometryHandlesOfTypeFromBatchResults(previousResults, QStringLiteral("polyline"));
        handles = selectedArrayValues(handles, params.value(QStringLiteral("createdPolylineHandleIndexes")).toArray());
        params.remove(QStringLiteral("autoPolylineHandlesFromBatch"));
        params.remove(QStringLiteral("createdPolylineHandleIndexes"));
        if (!handles.isEmpty()) {
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            selector.insert(QStringLiteral("kind"), QStringLiteral("polyline"));
            params.insert(QStringLiteral("selector"), selector);
        }
    }

    if (params.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)) {
        const QJsonArray handles = orderedRoomHandlesFromLatestGeometryQuery(previousResults);
        params.remove(QStringLiteral("autoRoomHandlesFromLastQuery"));
        if (!handles.isEmpty()) {
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            selector.insert(QStringLiteral("kind"), QStringLiteral("polyline"));
            selector.insert(QStringLiteral("shape"), QStringLiteral("rectangle"));
            params.insert(QStringLiteral("selector"), selector);
        }
    }

    if ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
        && params.value(QStringLiteral("autoPolylineHandlesFromLastQuery")).toBool(false)) {
        const QJsonArray handles = handlesFromLatestGeometryQuery(previousResults);
        params.remove(QStringLiteral("autoPolylineHandlesFromLastQuery"));
        if (!handles.isEmpty()) {
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            selector.insert(QStringLiteral("kind"), QStringLiteral("polyline"));
            params.insert(QStringLiteral("selector"), selector);
        }
    }

    if (toolCanUseRuntimeSelectionHandles(tool)) {
        QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
        if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("selection"), Qt::CaseInsensitive) == 0
            && !selector.value(QStringLiteral("handles")).isArray()) {
            const QJsonArray handles = latestSelectionSetHandlesFromBatchResults(previousResults);
            if (!handles.isEmpty()) {
                selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
                selector.insert(QStringLiteral("handles"), handles);
                params.insert(QStringLiteral("selector"), selector);
            }
        }
    }

    if ((tool == QStringLiteral("rectangles.extrude") || tool == QStringLiteral("circle.extrude") || tool == QStringLiteral("profile.extrude"))
        && params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)) {
        QJsonArray handles = createdGeometryHandlesFromBatchResults(previousResults);
        const QJsonArray createdHandleIndexes = params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray();
        if (!createdHandleIndexes.isEmpty()) {
            QJsonArray selectedHandles;
            for (const QJsonValue& indexValue : createdHandleIndexes) {
                const int index = indexValue.toInt(-1);
                if (index >= 0 && index < handles.size()) {
                    selectedHandles.append(handles.at(index));
                }
            }
            handles = uniqueStringArray(selectedHandles);
        }
        const int createdHandleIndex = params.value(QStringLiteral("createdGeometryHandleIndex")).toInt(-1);
        if (createdHandleIndex >= 0) {
            QJsonArray selectedHandle;
            if (createdHandleIndex < handles.size()) {
                selectedHandle.append(handles.at(createdHandleIndex));
            }
            handles = selectedHandle;
        }
        const int skipCreatedHandles = std::clamp(
            params.value(QStringLiteral("skipCreatedGeometryHandles")).toInt(0),
            0,
            static_cast<int>(handles.size()));
        for (int i = 0; i < skipCreatedHandles; ++i) {
            handles.removeFirst();
        }
        if (!handles.isEmpty()) {
            params.remove(QStringLiteral("autoHandlesFromBatch"));
            params.remove(QStringLiteral("skipCreatedGeometryHandles"));
            params.remove(QStringLiteral("createdGeometryHandleIndex"));
            params.remove(QStringLiteral("createdGeometryHandleIndexes"));
            QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
            selector.insert(QStringLiteral("scope"), QStringLiteral("handles"));
            selector.insert(QStringLiteral("handles"), handles);
            if (tool == QStringLiteral("rectangles.extrude")) {
                selector.insert(QStringLiteral("kind"), QStringLiteral("rectangle"));
                selector.insert(QStringLiteral("shape"), QStringLiteral("rectangle"));
            } else if (tool == QStringLiteral("circle.extrude")) {
                selector.insert(QStringLiteral("kind"), QStringLiteral("circle"));
                selector.insert(QStringLiteral("shape"), QStringLiteral("circle"));
            }
            params.insert(QStringLiteral("selector"), selector);
        }
        params.remove(QStringLiteral("skipCreatedGeometryHandles"));
        params.remove(QStringLiteral("createdGeometryHandleIndex"));
        params.remove(QStringLiteral("createdGeometryHandleIndexes"));
    }

    if (tool != QStringLiteral("bim.classify")
        || !params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)) {
        return params;
    }

    QJsonArray handles = createdGeometryHandlesFromBatchResults(previousResults);
    if (handles.isEmpty()) {
        handles = createdGeometryHandlesFromBatchResults(previousResults);
    }
    if (handles.isEmpty()) {
        return params;
    }

    params.remove(QStringLiteral("target"));
    params.remove(QStringLiteral("autoHandlesFromBatch"));
    params.insert(QStringLiteral("selector"), QJsonObject{
        {"scope", "handles"},
        {"handles", handles},
        {"kind", "solid"},
    });
    return params;
}

bool workflowActionCreatesRectangle(const QJsonObject& action, const QString& paramsKey)
{
    const QString tool = action.value("tool").toString().trimmed();
    const QJsonObject params = action.value(paramsKey).toObject();
    return tool == QStringLiteral("geometry.create")
        && params.value("geometry").toString().compare(QStringLiteral("rectangle"), Qt::CaseInsensitive) == 0;
}

bool workflowActionSetsSelection(const QJsonObject& action, const QString& paramsKey)
{
    const QString tool = action.value("tool").toString().trimmed();
    return (tool == QStringLiteral("selection.set") || tool == QStringLiteral("bim.selection.set"))
        && action.value(paramsKey).toObject().value("selector").isObject();
}

bool workflowToolCanUsePreviousSelection(const QString& tool)
{
    return toolCanUseRuntimeSelectionHandles(tool);
}

QJsonObject workflowSelectionSelectorFromPreviousStep(const QJsonObject& previousAction, const QString& paramsKey)
{
    QJsonObject selector = previousAction.value(paramsKey).toObject().value("selector").toObject();
    selector.insert(QStringLiteral("scope"), QStringLiteral("selection"));
    return selector;
}

QJsonObject normalizedWorkflowRuntimeSelectorAction(
    QJsonObject action,
    const QJsonObject& previousAction,
    const QString& paramsKey,
    bool* changed)
{
    QJsonObject params = action.value(paramsKey).toObject();
    const QString tool = action.value("tool").toString().trimmed();
    if (tool == QStringLiteral("rectangles.extrude") && workflowActionCreatesRectangle(previousAction, paramsKey)) {
        QJsonObject selector = params.value("selector").toObject();
        if (selector.value("scope").toString().compare(QStringLiteral("selection"), Qt::CaseInsensitive) == 0) {
            selector.insert(QStringLiteral("scope"), QStringLiteral("lastResult"));
            selector.insert(QStringLiteral("kind"), QStringLiteral("rectangle"));
            params.insert(QStringLiteral("selector"), selector);
            action.insert(paramsKey, params);
            if (changed) {
                *changed = true;
            }
        }
    }

    if (workflowActionSetsSelection(previousAction, paramsKey) && workflowToolCanUsePreviousSelection(tool)) {
        const bool hasSelector = params.value("selector").isObject();
        const bool hasTarget = !params.value("target").toString().trimmed().isEmpty();
        const bool hasHandles = params.value("handles").isArray() || !params.value("handle").toString().trimmed().isEmpty();
        if (!hasSelector && !hasHandles) {
            if ((tool == QStringLiteral("bim.classify") || tool == QStringLiteral("brx.sdk.bim.classification.set")) && !hasTarget) {
                params.insert(QStringLiteral("target"), QStringLiteral("selection"));
                action.insert(paramsKey, params);
                if (changed) {
                    *changed = true;
                }
            } else if (tool != QStringLiteral("bim.classify") && tool != QStringLiteral("brx.sdk.bim.classification.set")) {
                params.insert(QStringLiteral("selector"), workflowSelectionSelectorFromPreviousStep(previousAction, paramsKey));
                action.insert(paramsKey, params);
                if (changed) {
                    *changed = true;
                }
            }
        }
    }

    return action;
}

QJsonArray normalizedWorkflowRuntimeSelectorSequence(const QJsonArray& actions, const QString& paramsKey, bool* changed)
{
    QJsonArray normalized;
    QJsonObject previous;
    for (const QJsonValue& value : actions) {
        QJsonObject action = value.toObject();
        action = normalizedWorkflowRuntimeSelectorAction(action, previous, paramsKey, changed);
        normalized.append(action);
        previous = action;
    }
    return normalized;
}

QJsonObject normalizedWorkflowRuntimeSelectors(QJsonObject workflow, bool* changed = nullptr)
{
    bool localChanged = false;
    workflow.insert(QStringLiteral("optionalSlots"),
        normalizedWorkflowOptionalSlots(workflow.value(QStringLiteral("optionalSlots")), &localChanged));

    QJsonArray steps = workflow.value("steps").toArray();
    if (!steps.isEmpty()) {
        steps = normalizedWorkflowRuntimeSelectorSequence(steps, QStringLiteral("paramsTemplate"), &localChanged);
        workflow.insert(QStringLiteral("steps"), steps);
    }

    QJsonArray executionBatches = workflow.value("executionBatches").toArray();
    for (int i = 0; i < executionBatches.size(); ++i) {
        QJsonObject batch = executionBatches.at(i).toObject();
        const QJsonArray batchSteps = normalizedWorkflowRuntimeSelectorSequence(
            batch.value("steps").toArray(),
            QStringLiteral("paramsTemplate"),
            &localChanged);
        batch.insert(QStringLiteral("steps"), batchSteps);
        executionBatches.replace(i, batch);
    }
    if (!executionBatches.isEmpty()) {
        workflow.insert(QStringLiteral("executionBatches"), executionBatches);
    }

    QJsonArray validationExamples = workflow.value("validationExamples").toArray();
    for (int i = 0; i < validationExamples.size(); ++i) {
        QJsonObject example = validationExamples.at(i).toObject();
        const QJsonArray actions = normalizedWorkflowRuntimeSelectorSequence(
            example.value("actions").toArray(),
            QStringLiteral("params"),
            &localChanged);
        example.insert(QStringLiteral("actions"), actions);
        validationExamples.replace(i, example);
    }
    if (!validationExamples.isEmpty()) {
        workflow.insert(QStringLiteral("validationExamples"), validationExamples);
    }
    if (localChanged) {
        workflow.remove(QStringLiteral("validationWarnings"));
    }
    if (changed) {
        *changed = localChanged;
    }
    return workflow;
}

void appendJsonDebugLines(QPlainTextEdit* log, const QJsonArray& debugLines)
{
    if (!log) {
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    for (const QJsonValue& value : debugLines) {
        const QString line = value.toString();
        if (!line.isEmpty()) {
            log->appendPlainText(QString("[%1] BRX Debug: %2").arg(stamp, line));
        }
    }
}

QWidget* wrapCard(QWidget* content, QWidget* parent, int height = 154)
{
    auto* card = new QWidget(parent);
    card->setObjectName("metricCard");
    card->setFixedHeight(height);

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setColor(QColor(16, 24, 40, 24));
    shadow->setOffset(0, 10);
    card->setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(content);
    return card;
}

QLabel* cardHeader(const QString& title, const QString& value, QWidget* parent)
{
    auto* label = new QLabel(
        QString("<span style='color:#8a93a3;font-size:12px;font-weight:700;letter-spacing:0'>%1</span><br><b style='font-size:24px'>%2</b>")
            .arg(title.toHtmlEscaped(), value.toHtmlEscaped()),
        parent);
    label->setMinimumHeight(66);
    return label;
}

QWidget* createBrxLoadCard(QWidget* parent)
{
    const QString loadCommand = QString("APPLOAD %1").arg(kBrxPluginName);

    auto* wrapper = new QWidget(parent);
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* headline = cardHeader("BricsCAD", "BRX laden", wrapper);
    auto* command = new QLineEdit(loadCommand, wrapper);
    command->setReadOnly(true);

    auto* row = new QHBoxLayout();
    auto* copy = new QPushButton("Befehl kopieren", wrapper);
    copy->setObjectName("primaryButton");
    auto* docs = new QPushButton("Doku oeffnen", wrapper);
    row->addWidget(copy);
    row->addWidget(docs);

    layout->addWidget(headline);
    layout->addWidget(command);
    layout->addLayout(row);

    QObject::connect(copy, &QPushButton::clicked, wrapper, [command]() {
        QApplication::clipboard()->setText(command->text());
    });
    QObject::connect(docs, &QPushButton::clicked, wrapper, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(kBrxSdkRoot).filePath("docs/BrxDevRef.chm")));
    });

    return wrapCard(wrapper, parent, 166);
}

} // namespace

QJsonObject capabilitySummary(const QJsonObject& capabilities);
bool promptPrimarilyRequestsLayerMutation(const QString& prompt);
QString promptProposalConflictMessage(const QString& prompt, const QJsonArray& actions);

BricsCadPage::BricsCadPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_aiNetwork(new QNetworkAccessManager(this))
    , m_aiRuntime(new LocalAiAgentRuntime(m_config, this))
    , m_bridgeToken(generateBridgeToken())
{
    m_reasoningEffort = normalizedReasoningEffort(m_config.aiReasoningEffort());
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_agentWidget = new QWidget(this);
    m_agentWidget->setObjectName("agentPanel");
    m_agentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* agentLayout = new QVBoxLayout(m_agentWidget);
    agentLayout->setContentsMargins(0, 0, 0, 0);
    agentLayout->setSpacing(0);

    m_agentWebView = new QWebEngineView(m_agentWidget);
    m_agentWebView->setObjectName("agentWebView");
    m_agentWebView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_agentWebView->setMinimumSize(720, 520);
    m_agentWebView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    m_agentWebView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

    m_agentBridge = new AiWebBridge(this);
    m_agentChannel = new QWebChannel(m_agentWebView);
    m_agentChannel->registerObject(QStringLiteral("bareboneBridge"), m_agentBridge);
    m_agentWebView->page()->setWebChannel(m_agentChannel);
    m_agentWebView->setUrl(QUrl(QStringLiteral("qrc:/web/ai-assistant.html?profile=unified&theme=%1&lang=%2")
        .arg(effectiveWebTheme(m_config.theme()), effectiveUiLanguage(m_config.language()))));

    QObject::connect(m_aiRuntime, &LocalAiAgentRuntime::statusChanged, this, [this](const QJsonObject& status) {
        emitAgentRuntimeStatus(status);
        const bool foregroundActive = status.value(QStringLiteral("activeForeground")).toInt() > 0
            || m_parallelPreparationActive;
        if (m_agentBusy != foregroundActive) {
            setAgentBusy(foregroundActive);
        }
    });
    QObject::connect(m_aiRuntime, &LocalAiAgentRuntime::jobStarted, this, [this](const QString& id, const QString& kind, bool background) {
        appendBridgeLog(QString("AI Runtime: Job gestartet kind=%1 id=%2%3")
            .arg(kind, id.left(8), background ? QStringLiteral(" background") : QString()));
    });
    QObject::connect(m_aiRuntime, &LocalAiAgentRuntime::jobFinished, this, [this](const QString& id, const QString& kind, bool background, bool aborted) {
        appendBridgeLog(QString("AI Runtime: Job beendet kind=%1 id=%2%3%4")
            .arg(kind,
                id.left(8),
                background ? QStringLiteral(" background") : QString(),
                aborted ? QStringLiteral(" aborted") : QString()));
    });

    m_localAiPollTimer = new QTimer(this);
    m_localAiPollTimer->setSingleShot(true);
    QObject::connect(m_localAiPollTimer, &QTimer::timeout, this, [this]() {
        if (m_config.aiProvider() == QStringLiteral("local")) {
            refreshLocalContextWindow(true);
        }
        scheduleLocalAiStatusPoll();
    });

    agentLayout->addWidget(m_agentWebView, 1);

    m_bridgeStatus = new QLabel(QStringLiteral("Server startet..."), this);
    m_bridgeStatus->hide();

    m_bridgeLog = new QPlainTextEdit(this);
    m_bridgeLog->setObjectName("logView");
    m_bridgeLog->setReadOnly(true);
    root->addWidget(m_bridgeLog, 1);

    m_bricsCadCoordinator = new BricsCadAgentCoordinator(*m_aiRuntime, this);
    m_bricsCadCoordinator->setCurrentGenerationProvider([this]() {
        return m_operationGeneration;
    });
    m_bricsCadCoordinator->setBridgeRequestHandler([this](
        const QString& method,
        const QJsonObject& params,
        int timeoutMs,
        DrawingContextAgent::BridgeCallback callback) {
        return sendBridgeRequest(method, params, timeoutMs, std::move(callback));
    });
    m_bricsCadCoordinator->setWorkflowLoader([this](const QString& workflowId) {
        return loadWorkflowById(workflowId);
    });
    m_bricsCadCoordinator->setLogHandler([this](const QString& message) {
        appendBridgeLog(message);
    });
    m_bricsCadCoordinator->setBusyHandler([this](bool busy) {
        setAgentBusy(busy);
    });
    m_bricsCadCoordinator->setParallelActiveHandler([this](bool active) {
        m_parallelPreparationActive = active;
    });
    m_bricsCadCoordinator->setActiveReasoningRunHandler([this](const QString& runId) {
        m_activeReasoningRunId = runId;
    });
    m_bricsCadCoordinator->setReasoningProgressHandler([this](const QVariantMap& progress) {
        emitWebReasoningProgress(m_agentBridge, progress);
    });
    m_bricsCadCoordinator->setFocusedConversationHandler([this](const QJsonObject& focusedContext) {
        m_lastFocusedConversationContext = focusedContext;
    });
    m_bricsCadCoordinator->setSelectionHandler([this](const QJsonArray& selection) {
        m_currentSelection = selection;
    });
    m_bricsCadCoordinator->setFinalRouteHandler([this](
        const QString& prompt,
        const QJsonObject& documentContext,
        const QJsonObject& finalRoute) {
        m_lastAgentRoute = finalRoute;
        sendUnifiedAgentRequest(prompt, documentContext, finalRoute);
    });
    startBridgeServer();

    QObject::connect(m_agentBridge, &AiWebBridge::promptSubmitted, this, [this](const QString& prompt) {
        sendAgentPrompt(prompt);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::promptSubmittedWithContext, this, [this](const QString& prompt, const QVariantMap& context) {
        const QJsonObject jsonContext = QJsonObject::fromVariantMap(context);
        const QJsonObject contextWorkflow = jsonContext.value(QStringLiteral("selectedWorkflow")).toObject();
        const QString contextWorkflowId = contextWorkflow.value(QStringLiteral("id")).toString().trimmed();
        if (!contextWorkflowId.isEmpty()
            && (m_selectedWorkflow.isEmpty()
                || workflowSlug(m_selectedWorkflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId)) != workflowSlug(contextWorkflowId))) {
            selectWorkflowForChat(contextWorkflowId);
        }
        sendAgentPrompt(prompt, jsonContext);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::localAiStatusCheckRequested, this, [this]() {
        refreshLocalContextWindow(true);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::proposalConfirmed, this, [this]() {
        executeAgentProposal();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::proposalClearedByUser, this, [this]() {
        clearAgentProposal();
        m_pendingAgentDraft = {};
    });
    QObject::connect(m_agentBridge, &AiWebBridge::operationCancelledByUser, this, [this]() {
        cancelCurrentOperation();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowTrainingSaveConfirmed, this, [this]() {
        confirmWorkflowTrainingSaveReview();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowTrainingFinalSaveConfirmed, this, [this]() {
        confirmWorkflowTrainingFinalSave();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::newChatRequested, this, [this]() {
        resetAgentConversation();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::sessionOpened, this, [this](const QString& sessionId, const QVariantList& history) {
        openAgentSession(sessionId, history);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::reasoningEffortChanged, this, [this](const QString& effort) {
        setReasoningEffort(effort);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::chatModeChanged, this, [this](const QString& mode) {
        setChatMode(mode);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::trainingModeChanged, this, [this](bool enabled) {
        setTrainingMode(enabled);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::assistantWorkspaceChanged, this, [this](const QString& workspace) {
        setAssistantWorkspace(workspace);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::clientStateSaved, this, [this](const QString& stateJson) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(stateJson.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            setUnifiedAssistantState(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
        }
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowListRequested, this, [this]() {
        emitWorkflowListToWeb();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowSelected, this, [this](const QString& workflowId) {
        selectWorkflowForChat(workflowId);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowTestRequested, this, [this](const QString&) {
        if (!isChatWorkspace()) {
            return;
        }
        appendAgentChat("Barebone-Qt", "Workflow-Tests stehen nur im Chat-Arbeitsbereich zur Verfuegung.");
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowDeleteRequested, this, [this](const QString& workflowId) {
        QString deletedPath;
        QString errorMessage;
        const bool chatWorkspace = isChatWorkspace();
        if (!deleteWorkflowById(workflowId, &deletedPath, &errorMessage)) {
            const QString message = errorMessage.isEmpty()
                ? QStringLiteral("Workflow konnte nicht geloescht werden.")
                : errorMessage;
            if (chatWorkspace) {
                appendAgentChat("Barebone-Qt", message);
            } else {
                appendAgentChat("Barebone-Qt", message);
                appendBridgeLog(QString("BricsCAD Workflow Delete: %1").arg(message));
            }
            return;
        }
        if (chatWorkspace) {
            appendAgentChat("Barebone-Qt", QString("Workflow geloescht: %1").arg(deletedPath));
        } else {
            appendAgentChat("Barebone-Qt", QString("Workflow geloescht: %1").arg(deletedPath));
            appendBridgeLog(QString("BricsCAD Workflow geloescht: %1").arg(deletedPath));
        }
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowSelectionCleared, this, [this]() {
        clearSelectedWorkflowForChat();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::messagePdfExportRequested, this, [this](const QString& messageId, const QString& suggestedTitle) {
        exportAgentMessageToPdf(messageId, suggestedTitle);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::messageWorkflowSaveRequested, this, [this](const QString& messageId, const QString& messageText, const QString& sessionTitle) {
        if (isChatWorkspace()) {
            saveGeneralWorkflowFromMessage(messageId, messageText, sessionTitle);
        } else {
            Q_UNUSED(messageText);
            saveBricsCadWorkflowFromExecution(messageId, sessionTitle);
        }
    });
    QObject::connect(m_agentBridge, &AiWebBridge::uiReady, this, [this]() {
        emitToWebAsync(m_agentBridge, [state = unifiedAssistantState()](AiWebBridge* target) {
            Q_EMIT target->clientStateLoaded(state);
        });
        emitToWebAsync(m_agentBridge, [workspace = m_assistantWorkspace](AiWebBridge* target) {
            Q_EMIT target->assistantWorkspaceApplied(workspace);
        });
        emitUiThemeToWeb();
        emitUiLanguageToWeb();
        emitToWebAsync(m_agentBridge, [message = m_localAiStatusMessage, reachable = m_localAiReachable](AiWebBridge* target) {
            Q_EMIT target->localAiStatusChanged(message, reachable);
        });
        emitWorkflowListToWeb();
        if (!m_selectedWorkflow.isEmpty()) {
            emitToWebAsync(m_agentBridge, [workflow = m_selectedWorkflow.toVariantMap()](AiWebBridge* target) {
                Q_EMIT target->selectedWorkflowChanged(workflow);
            });
        }
        refreshLocalContextWindow(false);
        emitContextBudget();
        emitWebStatus(m_agentBridge, m_agentBusy ? QStringLiteral("thinking") : QStringLiteral("idle"));
        if (!m_pendingAgentProposal.isEmpty()) {
            setAgentProposal(m_pendingAgentProposal);
        } else {
            clearAgentProposal();
        }
        setPluginStatus(m_brxAuthenticated ? QStringLiteral("BRX Plugin verbunden") : QStringLiteral("BRX Plugin nicht verbunden"), m_brxAuthenticated);
        emitCapabilitiesStatusToWeb();
    });
    QObject::connect(&m_config, &ConfigManager::changed, this, [this]() {
        emitUiThemeToWeb();
        emitUiLanguageToWeb();
        refreshLocalContextWindow(true);
        scheduleLocalAiStatusPoll();
    });
    refreshLocalContextWindow(true);
    scheduleLocalAiStatusPoll();
}

QWidget* BricsCadPage::agentWidget() const
{
    return m_agentWidget;
}

bool BricsCadPage::isChatWorkspace() const
{
    return m_assistantWorkspace == QStringLiteral("chat");
}

QString BricsCadPage::unifiedAssistantState() const
{
    return m_config.unifiedAiAssistantState();
}

void BricsCadPage::setUnifiedAssistantState(const QString& stateJson)
{
    m_config.setUnifiedAiAssistantState(stateJson);
}

void BricsCadPage::emitUiThemeToWeb() const
{
    if (!m_agentBridge) {
        return;
    }
    emitToWebAsync(m_agentBridge, [theme = effectiveWebTheme(m_config.theme())](AiWebBridge* target) {
        Q_EMIT target->uiThemeChanged(theme);
    });
}

void BricsCadPage::emitUiLanguageToWeb() const
{
    if (!m_agentBridge) {
        return;
    }
    emitToWebAsync(m_agentBridge, [language = effectiveUiLanguage(m_config.language())](AiWebBridge* target) {
        Q_EMIT target->uiLanguageChanged(language);
    });
}

void BricsCadPage::scheduleLocalAiStatusPoll()
{
    if (!m_localAiPollTimer) {
        return;
    }

    if (m_config.aiProvider() != QStringLiteral("local")) {
        m_localAiPollTimer->stop();
        return;
    }

    const int intervalMs = m_localAiReachable
        ? kLocalAiReachablePollIntervalMs
        : kLocalAiUnreachablePollIntervalMs;
    m_localAiPollTimer->start(intervalMs);
}

void BricsCadPage::refreshLocalContextWindow(bool force)
{
    if (m_config.aiProvider() != QStringLiteral("local")) {
        m_contextWindowAvailable = false;
        m_contextWindowTokens = 0;
        m_contextWindowMaxTokens = 0;
        m_contextWindowModel.clear();
        setLocalAiStatus(QStringLiteral("Lokale AI nicht aktiv"), false);
        emitContextBudget();
        scheduleLocalAiStatusPoll();
        return;
    }
    if (m_contextWindowRequestInFlight) {
        return;
    }
    if (!force && m_contextWindowAvailable && m_contextWindowTokens > 0) {
        emitContextBudget();
        return;
    }

    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), QStringLiteral("local"));
    const QUrl url = lmStudioModelsUrlFromOpenAiBaseUrl(baseUrl);
    if (!url.isValid()) {
        setLocalAiStatus(QStringLiteral("Lokale AI URL ungÃƒÂ¼ltig"), false);
        emitContextBudget(-1, false, QStringLiteral("LM Studio Kontext: ungueltige URL"));
        scheduleLocalAiStatusPoll();
        return;
    }

    m_contextWindowRequestInFlight = true;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(5000);
    appendBridgeLog(QString("Qt -> LM Studio: models %1").arg(url.toString()));

    QNetworkReply* reply = m_aiNetwork->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_contextWindowRequestInFlight = false;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendBridgeLog(QString("LM Studio Kontext: Modelle konnten nicht gelesen werden: %1").arg(reply->errorString()));
            setLocalAiStatus(QStringLiteral("Lokale AI nicht erreichbar"), false);
            emitContextBudget(-1, false, QStringLiteral("Kontextlaenge nicht abrufbar"));
            scheduleLocalAiStatusPoll();
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            appendBridgeLog(QString("LM Studio Kontext: ungueltige JSON Antwort: %1").arg(parseError.errorString()));
            setLocalAiStatus(QStringLiteral("Lokale AI Antwort ungÃƒÂ¼ltig"), false);
            emitContextBudget(-1, false, QStringLiteral("Kontextlaenge nicht abrufbar"));
            scheduleLocalAiStatusPoll();
            reply->deleteLater();
            return;
        }

        handleLocalContextWindowResponse(document.object());
        scheduleLocalAiStatusPoll();
        reply->deleteLater();
    });
}

void BricsCadPage::handleLocalContextWindowResponse(const QJsonObject& response)
{
    setLocalAiStatus(QStringLiteral("Lokale AI erreichbar"), true);

    QJsonArray models = response.value("models").toArray();
    if (models.isEmpty()) {
        models = response.value("data").toArray();
    }

    const QString requestedModel = m_config.aiModel().trimmed();
    QJsonObject selectedModel;
    QJsonObject selectedLoadedInstance;
    bool selectedHasLoadedInstance = false;

    for (const QJsonValue& value : models) {
        const QJsonObject model = value.toObject();
        const QString type = model.value("type").toString();
        if (!type.isEmpty() && type != QStringLiteral("llm")) {
            continue;
        }
        const QString key = model.value("key").toString(model.value("id").toString());
        const bool keyMatches = !requestedModel.isEmpty()
            && key.compare(requestedModel, Qt::CaseInsensitive) == 0;
        const QJsonArray loadedInstances = model.value("loaded_instances").toArray();

        QJsonObject matchingInstance;
        for (const QJsonValue& instanceValue : loadedInstances) {
            const QJsonObject instance = instanceValue.toObject();
            const QString instanceId = instance.value("id").toString();
            if (keyMatches || instanceId.compare(requestedModel, Qt::CaseInsensitive) == 0) {
                matchingInstance = instance;
                break;
            }
        }

        if (keyMatches || !matchingInstance.isEmpty()) {
            selectedModel = model;
            selectedLoadedInstance = matchingInstance.isEmpty() && !loadedInstances.isEmpty()
                ? loadedInstances.first().toObject()
                : matchingInstance;
            selectedHasLoadedInstance = !selectedLoadedInstance.isEmpty();
            break;
        }

        if (selectedModel.isEmpty() && !loadedInstances.isEmpty()) {
            selectedModel = model;
            selectedLoadedInstance = loadedInstances.first().toObject();
            selectedHasLoadedInstance = true;
        }
        if (selectedModel.isEmpty()) {
            selectedModel = model;
        }
    }

    const QJsonObject instanceConfig = selectedLoadedInstance.value("config").toObject();
    const QJsonObject instanceRuntime = selectedLoadedInstance.value("runtime").toObject();
    const QJsonObject modelConfig = selectedModel.value("config").toObject();
    const QJsonObject modelInfo = selectedModel.value("model_info").toObject();
    const int loadedContext = positiveIntValue(instanceConfig, {"context_length", "loaded_context_length", "n_ctx", "num_ctx", "contextLength"});
    const int runtimeContext = positiveIntValue(instanceRuntime, {"context_length", "loaded_context_length", "n_ctx", "num_ctx", "contextLength"});
    const int legacyLoadedContext = positiveIntValue(selectedLoadedInstance, {"context_length", "loaded_context_length", "n_ctx", "num_ctx", "contextLength"});
    const int modelConfigContext = positiveIntValue(modelConfig, {"context_length", "loaded_context_length", "max_context_length", "n_ctx", "num_ctx", "contextLength", "max_position_embeddings"});
    const int modelInfoContext = positiveIntValue(modelInfo, {"context_length", "loaded_context_length", "max_context_length", "n_ctx", "num_ctx", "contextLength", "max_position_embeddings"});
    const int modelDirectContext = positiveIntValue(selectedModel, {"context_length", "loaded_context_length", "max_context_length", "n_ctx", "num_ctx", "contextLength", "max_position_embeddings"});
    const int modelContext = modelConfigContext > 0
        ? modelConfigContext
        : (modelInfoContext > 0 ? modelInfoContext : modelDirectContext);
    const int maxContextDirect = positiveIntValue(selectedModel, {"max_context_length", "context_length", "n_ctx", "num_ctx", "contextLength", "max_position_embeddings"});
    const int maxContext = maxContextDirect > 0
        ? maxContextDirect
        : modelContext;
    const int contextLength = loadedContext > 0
        ? loadedContext
        : (runtimeContext > 0 ? runtimeContext : (legacyLoadedContext > 0 ? legacyLoadedContext : modelContext));

    if (contextLength <= 0) {
        m_contextWindowAvailable = false;
        appendBridgeLog("LM Studio Kontext: keine Kontextlaenge in /api/v1/models gefunden");
        emitContextBudget(-1, false, QStringLiteral("Kontextlaenge nicht gemeldet"));
        return;
    }

    m_contextWindowAvailable = true;
    m_contextWindowTokens = contextLength;
    m_contextWindowMaxTokens = maxContext > 0 ? maxContext : contextLength;
    m_contextWindowModel = selectedModel.value("key").toString(selectedModel.value("id").toString(requestedModel));
    appendBridgeLog(QString("LM Studio Kontext: model=%1 loaded=%2 max=%3 loadedInstance=%4")
        .arg(m_contextWindowModel.isEmpty() ? QStringLiteral("<unbekannt>") : m_contextWindowModel)
        .arg(m_contextWindowTokens)
        .arg(m_contextWindowMaxTokens)
        .arg(selectedHasLoadedInstance ? QStringLiteral("yes") : QStringLiteral("no")));
    emitContextBudget();
}

int BricsCadPage::effectiveContextWindowTokens() const
{
    if (m_contextWindowAvailable && m_contextWindowTokens > 0) {
        return m_contextWindowTokens;
    }
    if (m_config.aiProvider() == QStringLiteral("local")) {
        return 8192;
    }
    return 128000;
}

int BricsCadPage::adjustedOutputTokenLimit(int requestedOutputTokens) const
{
    if (requestedOutputTokens <= 0) {
        return 512;
    }

    const int contextTokens = effectiveContextWindowTokens();
    const int dynamicLimit = std::max(384, contextTokens / 4);
    return std::clamp(requestedOutputTokens, 256, dynamicLimit);
}

int BricsCadPage::dynamicOutputTokenTarget(int minimumTokens, int maximumTokens, int contextDivisor) const
{
    const int contextTokens = effectiveContextWindowTokens();
    const int lower = std::max(128, minimumTokens);
    const int upper = std::max(lower, maximumTokens);
    const int divisor = std::max(1, contextDivisor);
    const int target = contextTokens > 0
        ? contextTokens / divisor
        : lower;
    return std::clamp(target, lower, upper);
}

int BricsCadPage::adjustedOutputTokenLimitForMessages(const QJsonArray& messages, int requestedOutputTokens) const
{
    const int baseLimit = adjustedOutputTokenLimit(requestedOutputTokens);
    const int contextTokens = effectiveContextWindowTokens();
    const int estimatedInputTokens = estimateTokensForMessages(messages);
    const int safetyTokens = std::clamp(contextTokens / 20, 512, 4096);
    const int availableOutputTokens = contextTokens - estimatedInputTokens - safetyTokens;
    if (availableOutputTokens <= 0) {
        return std::clamp(baseLimit, 128, 512);
    }
    return std::clamp(std::min(baseLimit, availableOutputTokens), 128, baseLimit);
}

int BricsCadPage::inputBudgetTokens(int requestedOutputTokens) const
{
    const int contextTokens = effectiveContextWindowTokens();
    const int outputTokens = adjustedOutputTokenLimit(requestedOutputTokens);
    const int safetyTokens = std::clamp(contextTokens / 10, 256, 2048);
    return std::max(512, contextTokens - outputTokens - safetyTokens);
}

int BricsCadPage::estimateTokensForText(const QString& text) const
{
    if (text.isEmpty()) {
        return 0;
    }
    return std::max<qsizetype>(1, (text.size() + 3) / 4);
}

int BricsCadPage::estimateTokensForMessages(const QJsonArray& messages) const
{
    return estimateTokensForText(QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact)));
}

QJsonObject BricsCadPage::documentContextWithTokenBudget(const QJsonObject& context, int tokenBudget, bool* minimized) const
{
    if (minimized) {
        *minimized = false;
    }

    QJsonObject sanitized = sanitizedDocumentContext(context);
    if (sanitized.isEmpty()) {
        return {};
    }

    const QString selectedText = sanitized.value("selectedText").toString();
    const int charBudget = std::max(1200, tokenBudget * 4);
    if (selectedText.size() <= charBudget) {
        return sanitized;
    }

    if (minimized) {
        *minimized = true;
    }
    sanitized.insert("selectedText", selectedText.left(charBudget)
        + QStringLiteral("\n[Dokumentkontext automatisch gekuerzt, damit die Anfrage in das Kontextfenster passt.]"));
    sanitized.insert("truncated", true);
    sanitized.insert("autoMinimized", true);
    return sanitized;
}

BricsCadPage::ContextBuildResult BricsCadPage::buildGeneralMessagesForBudget(
    const QJsonObject& envelope,
    const QJsonObject& instructionMessage,
    int requestedOutputTokens) const
{
    ContextBuildResult result;
    result.envelope = envelope;

    QStringList userSections;
    const QJsonObject documentContext = envelope.value(QStringLiteral("documentContext")).toObject();
    if (!documentContext.isEmpty()) {
        const QString contextText = semanticJsonContextText(documentContext).trimmed();
        if (!contextText.isEmpty()) {
            userSections.append(QStringLiteral("## Dokumentkontext\n%1").arg(contextText));
        }
    }

    const QJsonObject selectedWorkflow = envelope.value(QStringLiteral("selectedWorkflow")).toObject();
    if (!selectedWorkflow.isEmpty()) {
        const QString workflowText = semanticJsonContextText(selectedWorkflow).trimmed();
        if (!workflowText.isEmpty()) {
            userSections.append(QStringLiteral("## AusgewÃƒÂ¤hlter Workflow\n%1").arg(workflowText));
        }
    }

    const QJsonArray workflowCapsules = envelope.value(QStringLiteral("workflowCapsules")).toArray();
    const int firstAdditionalWorkflow = selectedWorkflow.isEmpty() ? 0 : 1;
    if (workflowCapsules.size() > firstAdditionalWorkflow) {
        QJsonArray additionalWorkflows;
        for (int i = firstAdditionalWorkflow; i < workflowCapsules.size(); ++i) {
            additionalWorkflows.append(workflowCapsules.at(i));
        }
        const QString workflowText = semanticJsonContextText(additionalWorkflows).trimmed();
        if (!workflowText.isEmpty()) {
            userSections.append(QStringLiteral("## Weitere relevante Workflows\n%1").arg(workflowText));
        }
    }

    const QString runtimeInstruction = envelope.value(QStringLiteral("instruction")).toString().trimmed();
    if (!runtimeInstruction.isEmpty()) {
        userSections.append(QStringLiteral("## Laufzeithinweis\n%1").arg(runtimeInstruction));
    }

    const QString prompt = envelope.value(QStringLiteral("userPrompt")).toString().trimmed();
    userSections.append(userSections.isEmpty()
        ? prompt
        : QStringLiteral("## Nutzeranfrage\n%1").arg(prompt));
    const QString userContent = userSections.join(QStringLiteral("\n\n")).trimmed();

    QVector<QJsonObject> history;
    history.reserve(m_agentConversation.size());
    bool userSeen = false;
    for (const QJsonValue& value : m_agentConversation) {
        const QJsonObject stored = value.toObject();
        if (stored.value(QStringLiteral("kind")).toString()
            == QStringLiteral("thinking")) {
            continue;
        }
        const QString role = stored.value(QStringLiteral("role")).toString().trimmed().toLower();
        if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) {
            continue;
        }
        if (role == QStringLiteral("assistant") && !userSeen) {
            continue;
        }

        QString content = stored.value(QStringLiteral("content")).toString().trimmed();
        if (content.isEmpty()) {
            continue;
        }
        if (role == QStringLiteral("assistant")) {
            bool parsed = false;
            const QJsonObject legacy = jsonObjectFromAiContent(content, &parsed);
            if (parsed
                && legacy.value(QStringLiteral("schema")).toString() == QStringLiteral("barebone.agent.response.v2")
                && legacy.value(QStringLiteral("type")).toString() == QStringLiteral("message")
                && !legacy.value(QStringLiteral("message")).toString().trimmed().isEmpty()) {
                content = legacy.value(QStringLiteral("message")).toString().trimmed();
            }
        } else {
            userSeen = true;
        }

        history.append(QJsonObject{
            {QStringLiteral("role"), role},
            {QStringLiteral("content"), content},
        });
    }

    const int budget = inputBudgetTokens(requestedOutputTokens);
    auto messagesForIndexes = [&](const QSet<int>& selectedIndexes) {
        QJsonArray messages;
        messages.append(instructionMessage);
        for (int i = 0; i < history.size(); ++i) {
            if (selectedIndexes.contains(i)) {
                messages.append(history.at(i));
            }
        }
        messages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), userContent},
        });
        return messages;
    };

    const QSet<int> noHistory;
    result.messages = messagesForIndexes(noHistory);
    result.estimatedTokens = estimateTokensForMessages(result.messages);
    if (result.estimatedTokens > budget || history.isEmpty()) {
        return result;
    }

    QSet<int> allIndexes;
    for (int i = 0; i < history.size(); ++i) {
        allIndexes.insert(i);
    }
    const QJsonArray fullMessages = messagesForIndexes(allIndexes);
    const int fullEstimate = estimateTokensForMessages(fullMessages);
    if (fullEstimate <= budget) {
        result.messages = fullMessages;
        result.estimatedTokens = fullEstimate;
        result.historyMessages = history.size();
        return result;
    }

    if (envelope.value(QStringLiteral("completeConversationContext")).toObject()
            .value(QStringLiteral("required")).toBool(false)) {
        result.messages = fullMessages;
        result.estimatedTokens = fullEstimate;
        result.historyMessages = history.size();
        return result;
    }

    struct HistoryTurn {
        QVector<int> indexes;
        QString searchText;
        int lastIndex = -1;
    };
    QVector<HistoryTurn> turns;
    for (int i = 0; i < history.size(); ++i) {
        const QJsonObject message = history.at(i);
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("user") || turns.isEmpty()) {
            turns.append(HistoryTurn{});
        }
        HistoryTurn& turn = turns.last();
        turn.indexes.append(i);
        turn.searchText.append(QLatin1Char(' '));
        turn.searchText.append(message.value(QStringLiteral("content")).toString());
        turn.lastIndex = i;
    }

    auto wordsForText = [](const QString& text) {
        QSet<QString> words;
        QRegularExpressionMatchIterator matches =
            QRegularExpression(QStringLiteral(R"([\p{L}\p{N}_]{3,})")).globalMatch(text.toLower());
        while (matches.hasNext()) {
            words.insert(matches.next().captured(0));
        }
        return words;
    };
    const QSet<QString> promptWords = wordsForText(prompt);
    auto turnScore = [&](const HistoryTurn& turn) {
        int overlap = 0;
        const QSet<QString> turnWords = wordsForText(turn.searchText);
        for (const QString& word : promptWords) {
            if (turnWords.contains(word)) {
                ++overlap;
            }
        }
        return overlap * 100000 + turn.lastIndex;
    };

    QSet<int> selectedIndexes;
    auto tryAddTurn = [&](const HistoryTurn& turn) {
        QSet<int> candidate = selectedIndexes;
        for (int index : turn.indexes) {
            candidate.insert(index);
        }
        const QJsonArray messages = messagesForIndexes(candidate);
        const int estimate = estimateTokensForMessages(messages);
        if (estimate > budget) {
            return false;
        }
        selectedIndexes = candidate;
        result.messages = messages;
        result.estimatedTokens = estimate;
        return true;
    };

    const int recentTurnStart = std::max(0, static_cast<int>(turns.size()) - 3);
    for (int i = turns.size() - 1; i >= recentTurnStart; --i) {
        tryAddTurn(turns.at(i));
    }

    QVector<int> olderTurnIndexes;
    for (int i = 0; i < recentTurnStart; ++i) {
        olderTurnIndexes.append(i);
    }
    std::stable_sort(
        olderTurnIndexes.begin(),
        olderTurnIndexes.end(),
        [&](int left, int right) {
            return turnScore(turns.at(left)) > turnScore(turns.at(right));
        });
    for (int turnIndex : olderTurnIndexes) {
        tryAddTurn(turns.at(turnIndex));
    }

    result.historyMessages = selectedIndexes.size();
    result.compressedHistoryMessages = history.size() - result.historyMessages;
    result.minimized = result.compressedHistoryMessages > 0;
    return result;
}

QJsonObject BricsCadPage::fallbackFocusedConversationContext(const QString& prompt) const
{
    return QJsonObject{
        {"schema", "barebone.agent.focused-conversation-context.v1"},
        {"source", "local-empty-or-fallback"},
        {"topic", repairMojibakeText(prompt).left(140).trimmed()},
        {"relevantSummary", m_agentConversation.isEmpty()
            ? QStringLiteral("Keine bisherigen Sitzungsnachrichten vorhanden.")
            : QStringLiteral("Fokus-Vorlauf nicht verfuegbar; nutze den vorhandenen kompakten Sitzungsverlauf.")},
        {"relevantMessageIndexes", QJsonArray{}},
        {"omittedTopics", QJsonArray{}},
        {"confidence", m_agentConversation.isEmpty() ? 1.0 : 0.0},
    };
}

QJsonObject BricsCadPage::normalizedFocusedConversationContext(const QJsonObject& object, const QString& prompt) const
{
    QJsonObject source = object.value(QStringLiteral("focusedConversationContext")).toObject();
    if (source.isEmpty()) {
        source = object;
    }

    QString topic = repairMojibakeText(source.value(QStringLiteral("topic")).toString()).trimmed();
    if (topic.isEmpty()) {
        topic = repairMojibakeText(prompt).left(140).trimmed();
    }
    if (topic.size() > 180) {
        topic = topic.left(177).trimmed() + QStringLiteral("...");
    }

    QString summary = repairMojibakeText(source.value(QStringLiteral("relevantSummary")).toString(
        source.value(QStringLiteral("summary")).toString())).trimmed();
    summary.replace("\r\n", "\n");
    if (summary.size() > 7000) {
        summary = summary.left(6997).trimmed() + QStringLiteral("...");
    }

    QJsonArray relevantIndexes;
    QSet<int> seenIndexes;
    const QJsonArray rawIndexes = source.value(QStringLiteral("relevantMessageIndexes")).toArray(
        source.value(QStringLiteral("messageIndexes")).toArray());
    for (const QJsonValue& value : rawIndexes) {
        bool numericString = false;
        const int index = value.isString()
            ? value.toString().toInt(&numericString)
            : value.toInt(-1);
        if (value.isString() && !numericString) {
            continue;
        }
        if (index < 0 || index >= m_agentConversation.size() || seenIndexes.contains(index)) {
            continue;
        }
        seenIndexes.insert(index);
        relevantIndexes.append(index);
        if (relevantIndexes.size() >= 32) {
            break;
        }
    }

    // Some local structured-output models return a useful summary and high
    // confidence but omit the indexes. Attach the most recent actual turns
    // that the summary describes so the reported count and used context agree.
    if (relevantIndexes.isEmpty() && !summary.isEmpty() && !m_agentConversation.isEmpty()) {
        const int first = std::max(0, static_cast<int>(m_agentConversation.size()) - 2);
        for (int index = first; index < m_agentConversation.size(); ++index) {
            const QJsonObject message = m_agentConversation.at(index).toObject();
            if (!message.value(QStringLiteral("content")).toString().trimmed().isEmpty()) {
                relevantIndexes.append(index);
            }
        }
    }

    QJsonArray omittedTopics;
    const QJsonArray rawTopics = source.value(QStringLiteral("omittedTopics")).toArray();
    for (const QJsonValue& value : rawTopics) {
        QString topicText = repairMojibakeText(value.toString()).trimmed();
        if (topicText.isEmpty()) {
            continue;
        }
        if (topicText.size() > 120) {
            topicText = topicText.left(117).trimmed() + QStringLiteral("...");
        }
        omittedTopics.append(topicText);
        if (omittedTopics.size() >= 16) {
            break;
        }
    }

    double confidence = source.value(QStringLiteral("confidence")).toDouble(-1.0);
    if (confidence < 0.0 || confidence > 1.0) {
        confidence = summary.isEmpty() && relevantIndexes.isEmpty() ? 0.35 : 0.65;
    }
    confidence = std::clamp(confidence, 0.0, 1.0);

    if (summary.isEmpty() && relevantIndexes.isEmpty() && !m_agentConversation.isEmpty()) {
        return {};
    }

    return QJsonObject{
        {"schema", "barebone.agent.focused-conversation-context.v1"},
        {"source", "ai-focus-run"},
        {"topic", topic},
        {"relevantSummary", summary},
        {"relevantMessageIndexes", relevantIndexes},
        {"omittedTopics", omittedTopics},
        {"confidence", confidence},
    };
}

QJsonObject BricsCadPage::compactConversationHistoryMessage(int index, bool fullText, int maxChars) const
{
    if (index < 0 || index >= m_agentConversation.size()) {
        return {};
    }

    const QJsonObject message = m_agentConversation.at(index).toObject();
    const QString role = message.value(QStringLiteral("role")).toString();
    QString content = repairMojibakeText(message.value(QStringLiteral("content")).toString())
        .replace("\r\n", "\n")
        .trimmed();
    bool parsed = false;
    const QJsonObject parsedObject = jsonObjectFromAiContent(content, &parsed);
    if (parsed) {
        const QString visibleMessage = repairMojibakeText(parsedObject.value(QStringLiteral("message")).toString()).trimmed();
        if (!visibleMessage.isEmpty()) {
            content = visibleMessage;
        } else {
            const QString type = parsedObject.value(QStringLiteral("type")).toString().trimmed();
            content = type.isEmpty()
                ? QStringLiteral("Interne Agent-Antwort ohne sichtbaren Nachrichtentext.")
                : QStringLiteral("Interne Agent-Antwort vom Typ %1.").arg(type);
        }
    }
    content = removeReasoningLeak(content).trimmed();

    const int boundedMaxChars = std::clamp(maxChars, 120, 200000);
    const bool truncated = content.size() > boundedMaxChars;
    if (truncated) {
        content = content.left(boundedMaxChars - 3).trimmed() + QStringLiteral("...");
    }

    QJsonObject item{
        {"index", index},
        {"role", role},
        {"truncated", truncated},
    };
    if (fullText) {
        item.insert(QStringLiteral("content"), content);
    } else {
        item.insert(QStringLiteral("preview"), content.left(std::min<qsizetype>(content.size(), 280)));
    }
    return item;
}

QJsonArray BricsCadPage::conversationHistoryMessagesRange(int start, int count, bool fullText) const
{
    QJsonArray messages;
    if (m_agentConversation.isEmpty() || count <= 0) {
        return messages;
    }
    const int boundedStart = std::clamp(start, 0, static_cast<int>(m_agentConversation.size()) - 1);
    const int boundedCount = std::clamp(count, 1, 12);
    const int end = std::min(static_cast<int>(m_agentConversation.size()), boundedStart + boundedCount);
    for (int i = boundedStart; i < end; ++i) {
        const QJsonObject item = compactConversationHistoryMessage(i, fullText);
        if (!item.isEmpty()) {
            messages.append(item);
        }
    }
    return messages;
}

QJsonObject BricsCadPage::conversationAccessForFocusedContext(const QJsonObject& focusedContext) const
{
    const int recentCount = std::min(2, static_cast<int>(m_agentConversation.size()));
    const int recentStart = std::max(0, static_cast<int>(m_agentConversation.size()) - recentCount);
    QJsonArray topicIndex;
    const QString topic = focusedContext.value(QStringLiteral("topic")).toString().trimmed();
    if (!topic.isEmpty()) {
        topicIndex.append(QJsonObject{
            {"kind", "focused"},
            {"topic", topic},
            {"messageIndexes", focusedContext.value(QStringLiteral("relevantMessageIndexes")).toArray()},
            {"confidence", focusedContext.value(QStringLiteral("confidence")).toDouble()},
        });
    }
    for (const QJsonValue& value : focusedContext.value(QStringLiteral("omittedTopics")).toArray()) {
        const QString omitted = value.toString().trimmed();
        if (!omitted.isEmpty()) {
            topicIndex.append(QJsonObject{{"kind", "omitted"}, {"topic", omitted}});
        }
    }

    return QJsonObject{
        {"schema", "barebone.agent.conversation-access.v1"},
        {"messageCount", m_agentConversation.size()},
        {"currentPromptPersisted", false},
        {"indexesAreZeroBased", true},
        {"recentMessages", recentCount > 0 ? conversationHistoryMessagesRange(recentStart, recentCount, true) : QJsonArray{}},
        {"topicIndex", topicIndex},
        {"policy", "Nutze diesen kompakten Sitzungsverlauf nur als Orientierung. Stelle eine gezielte Rueckfrage, wenn der Verlauf nicht reicht."},
    };
}

void BricsCadPage::emitContextBudget(int estimatedTokens, bool minimized, const QString& detail)
{
    if (!m_agentBridge) {
        return;
    }

    const int maxTokens = effectiveContextWindowTokens();
    int usedTokens = estimatedTokens >= 0
        ? estimatedTokens
        : estimateTokensForMessages(m_agentConversation);
    const bool preserveLastFetchedUsage = m_agentBusy
        && estimatedTokens < 0
        && m_lastContextBudgetUsedTokens > 0;
    if (preserveLastFetchedUsage) {
        usedTokens = m_lastContextBudgetUsedTokens;
    } else if (usedTokens > 0 || !m_agentBusy) {
        m_lastContextBudgetUsedTokens = usedTokens;
    }
    const int rawPercent = maxTokens > 0
        ? std::clamp(static_cast<int>((static_cast<double>(usedTokens) / static_cast<double>(maxTokens)) * 100.0), 0, 100)
        : 0;
    // The meter uses whole percentages. With large local context windows a
    // real, non-empty request can otherwise be truncated to 0 %, which looks
    // like the context disappeared while the model is thinking. Keep 0 % for
    // an actually empty context and show the smallest representable non-zero
    // value for every positive usage.
    const int percent = usedTokens > 0 && maxTokens > 0
        ? std::max(1, rawPercent)
        : rawPercent;

    QString state = QStringLiteral("ok");
    if (!m_contextWindowAvailable && m_config.aiProvider() == QStringLiteral("local")) {
        state = QStringLiteral("unknown");
    } else if (percent >= 92) {
        state = QStringLiteral("full");
    } else if (percent >= 75) {
        state = QStringLiteral("warn");
    }

    emitToWebAsync(m_agentBridge, [payload = QVariantMap{
        {"available", m_contextWindowAvailable},
        {"provider", m_config.aiProvider()},
        {"model", m_contextWindowModel.isEmpty() ? m_config.aiModel() : m_contextWindowModel},
        {"maxTokens", maxTokens},
        {"maxSupportedTokens", m_contextWindowMaxTokens},
        {"usedTokens", usedTokens},
        {"percent", percent},
        {"state", state},
        {"minimized", minimized},
        {"preservedWhileThinking", preserveLastFetchedUsage},
        {"detail", detail},
    }](AiWebBridge* target) {
        Q_EMIT target->contextBudgetChanged(payload);
    });
}

void BricsCadPage::emitAgentRuntimeStatus(const QJsonObject& status) const
{
    if (!m_agentBridge) {
        return;
    }
    emitToWebAsync(m_agentBridge, [payload = status.toVariantMap()](AiWebBridge* target) {
        Q_EMIT target->agentRuntimeStatusChanged(payload);
    });
}





QJsonObject BricsCadPage::sanitizedPromptContext(const QJsonObject& context) const
{
    return sanitizedDocumentContext(context);
}


void BricsCadPage::setReasoningEffort(const QString& effort)
{
    const QString normalized = normalizedReasoningEffort(effort);
    if (m_reasoningEffort == normalized) {
        m_config.setAiReasoningEffort(normalized);
        emitToWebAsync(m_agentBridge, [effort = m_reasoningEffort](AiWebBridge* target) {
            Q_EMIT target->reasoningEffortApplied(effort);
        });
        return;
    }
    m_reasoningEffort = normalized;
    m_config.setAiReasoningEffort(m_reasoningEffort);
    emitToWebAsync(m_agentBridge, [effort = m_reasoningEffort](AiWebBridge* target) {
        Q_EMIT target->reasoningEffortApplied(effort);
    });
    appendBridgeLog(QString("AI Agent: Reasoning %1").arg(normalized));
}

void BricsCadPage::setAssistantWorkspace(const QString& workspace)
{
    QString normalized = workspace.trimmed().toLower();
    if (normalized != QStringLiteral("chat")) {
        normalized = QStringLiteral("bricscad");
    }
    if (m_assistantWorkspace == normalized) {
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [workspace = m_assistantWorkspace](AiWebBridge* target) {
                Q_EMIT target->assistantWorkspaceApplied(workspace);
            });
        }
        return;
    }

    m_assistantWorkspace = normalized;
    clearAgentProposal();
    m_pendingAgentDraft = {};
    m_pendingAgentProposal = {};
    m_agentValidationRetries = 0;
    m_repeatedAgentContextRequestCount = 0;
    m_lastAgentContextRequestSignature.clear();
    m_agentContextLoopBlocked = false;
    clearWorkflowTrainingPrompts();
    if (m_assistantWorkspace == QStringLiteral("bricscad")) {
        m_chatMode = QStringLiteral("bricscad");
    } else {
        m_chatMode = QStringLiteral("general");
        if (m_assistantWorkspace == QStringLiteral("chat") || isChatWorkspace()) {
            m_trainingMode = false;
            clearSelectedWorkflowForChat();
        }
    }
    resetAgentConversation();
    appendBridgeLog(QString("AI Assistent: Arbeitsbereich %1").arg(m_assistantWorkspace));
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [workspace = m_assistantWorkspace](AiWebBridge* target) {
            Q_EMIT target->assistantWorkspaceApplied(workspace);
        });
    }
    emitWorkflowListToWeb();
    if (isBricsCadMode() && m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
        requestBridgeCapabilities();
    }
}

void BricsCadPage::setChatMode(const QString& mode)
{
    const QString normalized = m_assistantWorkspace == QStringLiteral("bricscad")
        ? QStringLiteral("bricscad")
        : QStringLiteral("general");
    if (m_chatMode == normalized) {
        return;
    }

    m_chatMode = normalized;
    resetAgentConversation();
    appendBridgeLog(QString("AI Agent: Modus %1").arg(m_chatMode));
    if (isBricsCadMode() && m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
        requestBridgeCapabilities();
    }
}

void BricsCadPage::setTrainingMode(bool enabled)
{
    Q_UNUSED(enabled);
    m_trainingMode = false;
    clearWorkflowTrainingPrompts();
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
            Q_EMIT target->trainingModeApplied(false);
        });
    }
}

bool BricsCadPage::isBricsCadMode() const
{
    return m_assistantWorkspace == QStringLiteral("bricscad")
        && m_chatMode == QStringLiteral("bricscad");
}

void BricsCadPage::sendAgentPrompt(const QString& promptText, const QJsonObject& documentContext)
{
    if (m_agentBusy) {
        m_deferredAgentPrompts.append(DeferredAgentPrompt{promptText, documentContext});
        appendBridgeLog(QString("AI Runtime: Foreground-Prompt eingereiht queue=%1").arg(m_deferredAgentPrompts.size()));
        return;
    }

    const QString prompt = promptText.trimmed();
    if (prompt.isEmpty()) {
        return;
    }

    if (isBricsCadMode()) {
        // Drawing context is deliberately turn-local. Manual BricsCAD activity
        // is not observed in the background; every prompt starts from a clean
        // cache and fills it only through explicit read-only BRX requests.
        drawingContextStore().clear();
        m_currentSelection = {};
    }

    const bool confirmsPendingProposal = isAgentConfirmation(prompt) && !m_pendingAgentProposal.isEmpty();

    const QJsonObject sanitizedContext = sanitizedPromptContext(documentContext);
    appendAgentChat("Du", prompt);
    m_agentRejectedResponseSignatures.clear();

    if (confirmsPendingProposal) {
        executeAgentProposal();
        return;
    }


    // One user request (including internal continueAfterSuccess stages) gets
    // exactly one automatic pre-action save. A new user request starts a new
    // safety boundary; confirmation of an existing proposal does not.
    m_agentPreActionSaveCompleted = false;

    if (isChatWorkspace()) {
        if (isGeneralWorkflowDeleteText(prompt)) {
            if (m_selectedWorkflow.isEmpty()) {
                appendAgentChat("Barebone-Qt", "Es ist kein gespeicherter Workflow ausgewÃƒÂ¤hlt, der geloescht werden kann.");
                return;
            }
            QString deletedPath;
            QString errorMessage;
            const QString workflowId = m_selectedWorkflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId);
            if (!deleteGeneralWorkflowById(workflowId, &deletedPath, &errorMessage)) {
                appendAgentChat("Barebone-Qt", errorMessage);
                return;
            }
            appendAgentChat("Barebone-Qt", QString("Workflow geloescht: %1").arg(deletedPath));
            return;
        }

        if (m_trainingFinalSavePending) {
            if (isWorkflowTrainingExplicitSaveText(prompt)) {
                confirmWorkflowTrainingFinalSave();
                return;
            }
            clearWorkflowTrainingPrompts();
            appendBridgeLog("General Workflow Save: Nutzer bearbeitet vorbereiteten Speichervorschlag weiter");
            saveGeneralWorkflowFromTraining(prompt);
            return;
        }

        if (isWorkflowTrainingExplicitSaveText(prompt)) {
            saveGeneralWorkflowFromTraining();
            return;
        }
    }

    if (!isBricsCadMode()) {
        m_agentValidationRetries = 0;
        m_lastAgentUserPrompt = prompt;
        m_lastDocumentContext = sanitizedContext;
        m_lastAgentRoute = normalizedAgentRouteForMode(
            makeAgentRoute(
                documentContextHasText(sanitizedContext) ? QStringLiteral("document_qa") : QStringLiteral("general_chat"),
                QStringLiteral("Allgemeiner Modus")),
            prompt,
            sanitizedContext,
            m_chatMode);
        clearAgentProposal();
        m_pendingAgentDraft = {};
        appendBridgeLog(QStringLiteral(
            "AI Agent: Allgemeiner Modus nutzt lokale Route ohne Router- oder CAD-Anfrage"));
        continueUnifiedAgentRequest(prompt, sanitizedContext, m_lastAgentRoute);
        return;
    }

    const QJsonObject previousRoute = m_lastAgentRoute;
    if (!m_selectedWorkflow.isEmpty()) {
        QJsonObject explicitValues = workflowTrainingSlotValuesFromPrompt(prompt, {}, m_selectedWorkflow);
        const QJsonObject workflowDefaults = m_selectedWorkflow.value(QStringLiteral("knownSlotValues")).toObject();
        if (workflowDefaults.contains(QStringLiteral("houseWidthMm"))
            && explicitValues.contains(QStringLiteral("roomWidthMm"))) {
            explicitValues.insert(QStringLiteral("houseWidthMm"), explicitValues.value(QStringLiteral("roomWidthMm")));
        }
        if (workflowDefaults.contains(QStringLiteral("houseLengthMm"))
            && explicitValues.contains(QStringLiteral("roomLengthMm"))) {
            explicitValues.insert(QStringLiteral("houseLengthMm"), explicitValues.value(QStringLiteral("roomLengthMm")));
        }
        if (workflowDefaults.contains(QStringLiteral("grossFootprintM2"))
            && explicitValues.contains(QStringLiteral("roomAreaM2"))) {
            explicitValues.insert(QStringLiteral("grossFootprintM2"), explicitValues.value(QStringLiteral("roomAreaM2")));
        }
        for (auto it = explicitValues.begin(); it != explicitValues.end(); ++it) {
            m_selectedWorkflowSlotValues.insert(it.key(), it.value());
        }
        if (!explicitValues.isEmpty()) {
            appendBridgeLog(QString("Workflow-Nutzerwerte: %1")
                .arg(explicitValues.keys().join(QStringLiteral(","))));
        }
    }
    bool previousReplyAskedUser = false;
    if (!m_agentConversation.isEmpty()) {
        const QJsonObject lastMessage = m_agentConversation.last().toObject();
        bool parsedLastReply = false;
        const QJsonObject lastReply = jsonObjectFromAiContent(
            lastMessage.value(QStringLiteral("content")).toString(), &parsedLastReply);
        previousReplyAskedUser = lastMessage.value(QStringLiteral("role")).toString() == QStringLiteral("assistant")
            && parsedLastReply
            && lastReply.value(QStringLiteral("type")).toString() == QStringLiteral("ask_user");
    }
    const bool previousPromptWasCadAction = fallbackRouteNameForPrompt(
        m_lastAgentUserPrompt, sanitizedContext) == QStringLiteral("bricscad_action");
    const bool continuesCadTask = (!m_pendingAgentDraft.isEmpty() || previousReplyAskedUser)
        && (routeAllowsCadActions(previousRoute) || previousPromptWasCadAction);
    const QString normalizedCurrentPrompt = workflowTrainingSearchText(prompt);
    const bool explicitlyRequestsWorkflowExecution = textMentionsAny(normalizedCurrentPrompt, {
        QStringLiteral("workflow ausfuehr"),
        QStringLiteral("workflow anwenden"),
        QStringLiteral("workflow starten"),
        QStringLiteral("workflow nutzen"),
        QStringLiteral("ausgewaehlten workflow"),
        QStringLiteral("ausgewaehlter workflow"),
        QStringLiteral("fuehre den workflow"),
        QStringLiteral("fuehre den ausgewaehlten"),
        QStringLiteral("starte den workflow"),
        QStringLiteral("wende den workflow"),
    });
    const bool selectedWorkflowExecutionRequested = !m_selectedWorkflow.isEmpty()
        && !workflowDisplaySteps(m_selectedWorkflow).isEmpty()
        && explicitlyRequestsWorkflowExecution
        && !(normalizedCurrentPrompt.startsWith(QStringLiteral("was "))
            || normalizedCurrentPrompt.startsWith(QStringLiteral("wie "))
            || normalizedCurrentPrompt.startsWith(QStringLiteral("warum "))
            || normalizedCurrentPrompt.startsWith(QStringLiteral("welche "))
            || normalizedCurrentPrompt.startsWith(QStringLiteral("erklaer"))
            || normalizedCurrentPrompt.startsWith(QStringLiteral("beschreib")));
    QJsonObject continuationRoute = previousRoute;
    if (continuesCadTask && !routeAllowsCadActions(continuationRoute)) {
        continuationRoute = normalizedAgentRouteForMode(
            makeAgentRoute(QStringLiteral("bricscad_action"),
                QStringLiteral("Qt Verlaufskontinuitaet: Antwort auf offene CAD-Rueckfrage")),
            prompt, sanitizedContext, m_chatMode);
    }
    m_agentValidationRetries = 0;
    m_repeatedAgentContextRequestCount = 0;
    m_lastAgentContextRequestSignature.clear();
    m_agentContextLoopBlocked = false;
    m_lastAgentUserPrompt = prompt;
    m_lastDocumentContext = sanitizedContext;
    m_lastAgentRoute = continuesCadTask ? continuationRoute : QJsonObject{};

    if (!m_pendingAgentProposal.isEmpty()) {
        if (m_agentBridge) {
            emitWebProposalCleared(m_agentBridge);
        }
        appendBridgeLog("AI Agent: offener Vorschlag wird als Kontext an die AI weitergegeben");
    }

    if (!m_pendingAgentDraft.isEmpty()) {
        appendBridgeLog("AI Agent: offener Plan/Draft wird als Kontext an die AI weitergegeben");
    }

    if (continuesCadTask) {
        appendBridgeLog("AI Agent: Nutzerantwort ergaenzt offene CAD-Rueckfrage; Router wird uebersprungen");
        continueUnifiedAgentRequest(prompt, sanitizedContext, continuationRoute);
        return;
    }

    if (selectedWorkflowExecutionRequested) {
        appendBridgeLog("AI Agent: ausgewaehlter ausfuehrbarer Workflow; Router wird uebersprungen");
        continueUnifiedAgentRequest(
            prompt,
            sanitizedContext,
            normalizedAgentRouteForMode(
                makeAgentRoute(QStringLiteral("bricscad_action"), QStringLiteral("Ausgewaehlter Workflow ist ausfuehrbar")),
                prompt,
                sanitizedContext,
                m_chatMode));
        return;
    }

    routeUnifiedAgentPrompt(prompt, sanitizedContext);
}

void BricsCadPage::routeUnifiedAgentPrompt(const QString& prompt, const QJsonObject& documentContext)
{
    const QString mode = isBricsCadMode() ? QStringLiteral("bricscad") : QStringLiteral("general");
    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi =
        useResponsesApiForProvider(provider, model, AgentResponseKind::StructuredJson);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    const QJsonObject fallbackRoute = normalizedAgentRouteForMode(
        makeAgentRoute(
            mode == QStringLiteral("bricscad")
                ? QStringLiteral("bricscad_action")
                : fallbackRouteNameForPrompt(prompt, documentContext),
            mode == QStringLiteral("bricscad")
                ? QStringLiteral("Neutraler BricsCAD-Agentenpfad")
                : QStringLiteral("Lokale Router-Heuristik")),
        prompt,
        documentContext,
        mode);

    if (!url.isValid()) {
        appendBridgeLog(QString("AI Router: ungueltige AI Server URL, Fallback mode=%1 route=%2")
            .arg(mode, fallbackRoute.value("route").toString()));
        continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    const QJsonObject routerContract = agentResourceJsonObject(routerContractResourceForMode(mode));
    if (routerContract.isEmpty()) {
        appendBridgeLog(QString("AI Router: Contract fehlt, Fallback mode=%1 route=%2")
            .arg(mode, fallbackRoute.value("route").toString()));
        continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
        return;
    }

    QJsonObject routerInput{
        {"schema", "barebone.agent.router.input.v1"},
        {"mode", mode},
        {"modePolicy", modePolicyForMode(mode, fallbackRoute)},
        {"contract", routerContract},
        {"userPrompt", prompt},
        {"hasDocumentContext", documentContextHasText(documentContext)},
        {"documentContextPreview", documentContext.value("selectedText").toString().left(1200)},
        {"brxConnected", m_brxAuthenticated},
        {"capabilitiesLoaded", !m_brxCapabilities.isEmpty()},
        {"actionToolsAvailable", !availableAgentTools().isEmpty()},
        {"pendingDraftAvailable", !m_pendingAgentDraft.isEmpty()},
        {"pendingProposalAvailable", !m_pendingAgentProposal.isEmpty()},
        {"pendingProposal", m_pendingAgentProposal},
        {"instruction", "Klassifiziere nur die Route innerhalb des aktiven modePolicy.allowedRoutes. Keine Nutzerantwort und keine Aktion erzeugen."},
    };

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content",
            "Du bist der Barebone-Qt AI-Router. "
            "Klassifiziere den Nutzerprompt in genau eine Route gemaess dem beiliegenden mode-spezifischen Router-Contract. "
            "Du darfst keine Frage beantworten, keine Aktion vorschlagen und keine Tools nutzen. "
            "Waehle nur eine Route aus modePolicy.allowedRoutes. "
            "Antworte ausschliesslich mit einem JSON-Objekt: "
            "{\"schema\":\"barebone.agent.router.v1\",\"route\":\"...\",\"capabilityProfile\":\"...\",\"reason\":\"...\"}."},
    });
    messages.append(QJsonObject{
        {"role", "user"},
        {"content", QString::fromUtf8(QJsonDocument(routerInput).toJson(QJsonDocument::Compact))},
    });

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", 512);
        const QString reasoningEffort = reasoningEffortForModel(model,
            normalizedReasoningEffort(m_reasoningEffort) == QStringLiteral("none") ? QStringLiteral("none") : QStringLiteral("low"));
        if (reasoningEffort != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.0);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.0);
        payload.insert("max_tokens", 512);
        insertChatReasoningForModel(payload, model, m_reasoningEffort);
        if (!officialProvider) {
            payload.insert(
                QStringLiteral("response_format"),
                localStructuredResponseFormat(
                    QStringLiteral("barebone_router"),
                    {QStringLiteral("schema"), QStringLiteral("route"),
                        QStringLiteral("capabilityProfile"), QStringLiteral("reason")}));
        }
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    appendBridgeLog(QString("Qt -> AI Router: mode=%1 provider=%2 endpoint=%3 model=%4 timeoutMs=%5")
        .arg(mode, provider, url.toString(), model)
        .arg(kAiModelResponseTimeoutMs));

    const int operationGeneration = m_operationGeneration;
    enqueueAiPost(
        request,
        payload,
        QStringLiteral("Router"),
        95,
        false,
        false,
        {},
        [this, prompt, documentContext, fallbackRoute, mode, operationGeneration](const LocalAiJobScheduler::Result& result) {
        if (operationGeneration != m_operationGeneration) {
            return;
        }

        const int httpStatus = result.httpStatus;
        const QByteArray body = result.body;
        if (result.networkError != QNetworkReply::NoError) {
            appendBridgeLog(QString("AI Router: Fehler http=%1 %2, Fallback route=%3")
                .arg(httpStatus)
                .arg(result.errorString)
                .arg(fallbackRoute.value("route").toString()));
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Router Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendBridgeLog(QString("AI Router: OpenAI JSON ungueltig (%1), Fallback route=%2")
                .arg(parseError.errorString(), fallbackRoute.value("route").toString()));
            appendBridgeLog(QString("AI Router Body: %1").arg(QString::fromUtf8(body).left(800)));
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        QString reasoningText;
        const QString content = repairMojibakeText(
            aiChatCompletionContent(responseDocument.object(), &reasoningText)).trimmed();
        if (content.isEmpty()) {
            appendBridgeLog(QString("AI Router: leere Antwort, Fallback route=%1")
                .arg(fallbackRoute.value("route").toString()));
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        bool parsed = false;
        const QJsonObject parsedRoute = jsonObjectFromAiContent(content, &parsed);
        QJsonObject route = parsed
            ? normalizedAgentRouteForMode(parsedRoute, prompt, documentContext, mode, QStringLiteral("AI Router"))
            : fallbackRoute;
        appendBridgeLog(QString("AI Router: mode=%1 route=%2 profile=%3 reason=%4")
            .arg(mode,
                route.value("route").toString(),
                route.value("capabilityProfile").toString(),
                route.value("reason").toString().left(240)));
        continueUnifiedAgentRequest(prompt, documentContext, route);
    });
}

void BricsCadPage::startBricsCadParallelPreparation(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    if (!m_bricsCadCoordinator) {
        sendUnifiedAgentRequest(prompt, documentContext, route);
        return;
    }

    BricsCadAgentCoordinator::RunRequest request;
    request.prompt = prompt;
    request.documentContext = documentContext;
    request.route = route;
    request.conversation = m_agentConversation;
    request.capabilities = m_brxCapabilities;
    request.catalog = availableAgentTools();
    request.workflowIndex = bricsCadWorkflowIndex();
    request.sessionId = m_agentSessionId;
    request.manualWorkflowId = m_selectedWorkflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId);
    request.selectedWorkflow = m_selectedWorkflow;
    request.selectedWorkflowSlotValues = m_selectedWorkflowSlotValues;
    request.generation = m_operationGeneration;
    request.brxAuthenticated = m_brxAuthenticated;
    m_bricsCadCoordinator->start(request);
}

void BricsCadPage::continueUnifiedAgentRequest(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, documentContext, m_chatMode);
    appendBridgeLog(QStringLiteral("AI Prompt: %1").arg(repairMojibakeText(prompt).left(240)));
    if (isBricsCadMode() && routeAllowsCadContext(normalizedRoute)) {
        const bool manualWorkflowActive = !m_selectedWorkflow.isEmpty();
        if (manualWorkflowActive) {
            const QString activeWorkflowId = m_selectedWorkflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId).trimmed();
            if (!activeWorkflowId.isEmpty()) {
                normalizedRoute.insert(QStringLiteral("selectedWorkflows"), QJsonArray::fromStringList(QStringList{activeWorkflowId}));
                normalizedRoute.insert(QStringLiteral("activeWorkflowId"), activeWorkflowId);
                normalizedRoute.insert(QStringLiteral("workflowSource"), QStringLiteral("manual"));
                normalizedRoute.insert(QStringLiteral("workflowAuthority"), QStringLiteral("manualRequired"));
            }
        } else {
            normalizedRoute.remove(QStringLiteral("selectedWorkflows"));
            normalizedRoute.remove(QStringLiteral("activeWorkflowId"));
            normalizedRoute.insert(QStringLiteral("workflowSource"), QStringLiteral("none"));
            normalizedRoute.insert(QStringLiteral("workflowAuthority"), QStringLiteral("none"));
        }
    }
    if (isChatWorkspace()) {
        if (promptRequiresCompleteConversationContext(prompt) && !m_agentConversation.isEmpty()) {
            normalizedRoute.insert(QStringLiteral("completeConversationContextRequired"), true);
        }
        normalizedRoute.insert(QStringLiteral("conversationFocusAttempted"), true);
        m_lastFocusedConversationContext = {};
    }
    m_lastAgentRoute = normalizedRoute;

    if (!routeAllowsCadActions(normalizedRoute) && !m_pendingAgentDraft.isEmpty()) {
        appendBridgeLog("AI Agent: offener CAD-Draft verworfen, Route ist nicht ausfuehrend");
        m_pendingAgentDraft = {};
    }

    if (isBricsCadMode()
        && routeAllowsCadContext(normalizedRoute)
        && !normalizedRoute.value(QStringLiteral("parallelPreparationComplete")).toBool(false)) {
        m_queuedAgentRoute = normalizedRoute;
        if (routeAllowsCadActions(normalizedRoute)) {
            if (!ensureBridgeCapabilitiesForPrompt(prompt)) {
                return;
            }
        } else if (m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
            m_queuedAgentPrompt = prompt;
            appendBridgeLog("AI Parallel Pipeline: wartet auf BRX Capabilities");
            setAgentBusy(true);
            requestBridgeCapabilities();
            return;
        }
        startBricsCadParallelPreparation(prompt, documentContext, normalizedRoute);
        return;
    }

    if (routeAllowsCadActions(normalizedRoute)) {
        m_queuedAgentRoute = normalizedRoute;
        if (!ensureBridgeCapabilitiesForPrompt(prompt)) {
            return;
        }
    } else if (routeAllowsCadContext(normalizedRoute) && m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
        m_queuedAgentPrompt = prompt;
        m_queuedAgentRoute = normalizedRoute;
        appendBridgeLog("AI Agent: CAD-Frage wartet auf BRX Capabilities");
        setAgentBusy(true);
        requestBridgeCapabilities();
        return;
    }

    sendUnifiedAgentRequest(prompt, documentContext, normalizedRoute);
}


void BricsCadPage::sendUnifiedAgentRequest(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route)
{
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, documentContext, m_chatMode);
    m_lastAgentRoute = normalizedRoute;
    sendAgentEnvelope(
        agentRequestEnvelope(prompt, documentContext, normalizedRoute),
        prompt,
        true,
        QString("prompt/%1").arg(normalizedRoute.value("route").toString()));
}

static ParsedModelOutput parseModelOutput(
    const QJsonObject& response,
    AgentResponseKind responseKind);

QString BricsCadPage::enqueueAiPost(
    QNetworkRequest request,
    const QJsonObject& payload,
    const QString& kind,
    int priority,
    bool background,
    bool cancellable,
    const QString& dedupeKey,
    std::function<void(const LocalAiJobScheduler::Result&)> callback)
{
    if (!m_aiRuntime) {
        return {};
    }
    return m_aiRuntime->enqueuePost(
        request,
        payload,
        kind,
        priority,
        background,
        cancellable,
        dedupeKey,
        m_operationGeneration,
        isBricsCadMode(),
        std::move(callback));
}

void BricsCadPage::sendAgentEnvelope(const QJsonObject& envelope, const QString& userHistoryContent, bool storeUserMessage, const QString& logLabel)
{
    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const QJsonObject requestRoute = envelope.value(QStringLiteral("route")).toObject();
    const QString requestRouteName = requestRoute.value(QStringLiteral("route")).toString();
    const bool plainGeneralResponse =
        m_chatMode == QStringLiteral("general")
        && (requestRouteName == QStringLiteral("general_chat")
            || requestRouteName == QStringLiteral("document_qa"));
    const bool bricsCadStructuredResponse = isBricsCadMode() && !plainGeneralResponse;
    const QJsonObject slottedEnvelope = envelope;
    const AgentResponseKind responseKind = plainGeneralResponse
        ? AgentResponseKind::VisibleMarkdown
        : AgentResponseKind::StructuredJson;
    const bool useResponsesApi = useResponsesApiForProvider(provider, model, responseKind);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) {
        appendAgentChat("Barebone-Qt", QString("Ungueltige AI Server URL: %1").arg(baseUrl));
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    const QString corePrompt = bricsCadStructuredResponse
        ? QString()
        : agentResourceText(QStringLiteral(":/agent/policies/core.md"));
    const QString generalPlainPrompt = QStringLiteral(
        "Du bist der allgemeine AI-Assistent fÃƒÂ¼r Barebone-Qt. %1 "
        "Antworte direkt. Verwende Markdown-ÃƒÅ“berschriften, Tabellen und echte AufzÃƒÂ¤hlungen, wenn sie die Lesbarkeit verbessern. "
        "FÃƒÂ¼hre bei Berechnungen die Einheit an jedem eingesetzten Wert und jedem Zwischenergebnis mit und zeige notwendige SI-Umrechnungen. "
        "Rufe im Allgemeinen Modus keine CAD-/BRX-Funktionen auf und schlage keine Zeichnungsaktionen vor.")
        .arg(aiLanguageInstruction(m_config));
    const QString bricsCadMinimalPrompt = QStringLiteral(
        "Du bist der finale BricsCAD-Agent in Barebone-Qt. %1 "
        "Antworte ausschliesslich mit genau einem gueltigen JSON-Objekt nach schema=\"barebone.agent.response.v2\". "
        "Nutze nur Tools aus effectiveTools[].name und Parameter aus deren kompakten Schemas. "
        "Der unveraenderte originalUserPrompt und der relevante Verlauf bestimmen die Aufgabe. Waehle Tools und passende Workflowteile selbst und uebernimm keinen Workflow blind. "
        "Ein manuell ausgewaehlter Workflow ist verbindlich; bei einem Konflikt frage den Nutzer statt fachliche Schritte still zu ersetzen. "
        "Pruefe den kompakten drawingContext auf erkennbare Konflikte, Ueberlappungen und vorhandene Geometrie. "
        "Wenn die Aktion ausfuehrbar ist, liefere type=\"action_proposal\" mit proposal.actions[] als vollstaendigen Batch. "
        "Liefere bei action_proposal zusaetzlich proposalId, intentSummary, contextEvidence, workflowUsage und assumptions als knappe pruefbare Angaben ohne Gedankenkette. "
        "Bei action_proposal muss message sowie proposal.summary eine konkrete nutzerlesbare Beschreibung der geplanten Ausfuehrung enthalten; "
        "nenne Zielnamen, Toolabsicht und wichtige Parameter. Nutze proposal.details fuer die Schrittfolge. "
        "Keine generischen Beschreibungen wie 'Der Agent hat eine BricsCAD-Aktion vorbereitet'. "
        "Wenn echte Pflichtdaten fehlen oder ein Zeichnungskonflikt nicht sicher geloest werden kann, liefere type=\"ask_user\" mit missing und einer kurzen natuerlichen deutschen message. "
        "Die message muss alle benoetigten Angaben und ihr erwartetes Format direkt beschreiben, weil sie ohne 'Details anzeigen' im Chat steht. Verwende keinen allgemeinen Wartetext. "
        "Wenn nur zusaetzlicher read-only Zeichnungskontext fehlt, liefere type=\"context_request\". "
        "Wenn keine Ausfuehrung sinnvoll ist, darfst du message oder plan liefern. Keine nativen Commands, kein Markdown, keine lange Begruendung, keine Wiederholung von Workflow- oder Toolkontext.")
        .arg(aiLanguageInstruction(m_config));
    const QJsonObject systemMessage{
        {"role", plainGeneralResponse && localModelFamily(model) == LocalModelFamily::GptOss
                ? QStringLiteral("developer")
                : QStringLiteral("system")},
        {"content", plainGeneralResponse
            ? generalPlainPrompt
            : (bricsCadStructuredResponse
                ? bricsCadMinimalPrompt
            : (corePrompt.isEmpty()
            ? QStringLiteral("Du bist der AI Assistent fuer Barebone-Qt. %1 Antworte ausschliesslich mit einem gueltigen JSON-Objekt gemaess responseContract.")
                .arg(aiLanguageInstruction(m_config))
            : corePrompt))},
    };
    const bool includeConversationHistory = slottedEnvelope.value("includeConversationHistory").toBool(true);
    const int localResponsesOutputMaximum = std::clamp(effectiveContextWindowTokens() / 4, 8192, 32768);
    const int requestedOutputTokens = bricsCadStructuredResponse
        ? 2048
        : dynamicOutputTokenTarget(
            useResponsesApi ? 2048 : 1024,
            useResponsesApi
                ? (provider == QStringLiteral("local") ? localResponsesOutputMaximum : 8192)
                : 8192,
            useResponsesApi && provider == QStringLiteral("local") ? 4 : 16);
    const int initialOutputTokens = adjustedOutputTokenLimit(requestedOutputTokens);
    const int budget = inputBudgetTokens(initialOutputTokens);
    const int totalHistoryMessages = includeConversationHistory
        ? static_cast<int>(m_agentConversation.size())
        : 0;
    const QJsonObject focusedConversationContext = slottedEnvelope.value(QStringLiteral("focusedConversationContext")).toObject();
    const bool hasFocusedConversationContext = !focusedConversationContext.isEmpty();

    auto compactHistoryLine = [&](const QJsonObject& item, int maxChars) {
        const QString role = item.value("role").toString() == QStringLiteral("user")
            ? QStringLiteral("Nutzer")
            : QStringLiteral("AI");
        QString content = item.value("content").toString().trimmed();
        bool parsed = false;
        const QJsonObject parsedObject = jsonObjectFromAiContent(content, &parsed);
        if (parsed) {
            const QString message = parsedObject.value("message").toString().trimmed();
            if (!message.isEmpty()) {
                content = message;
            } else {
                const QString type = parsedObject.value("type").toString().trimmed();
                content = type.isEmpty()
                    ? QString::fromUtf8(QJsonDocument(parsedObject).toJson(QJsonDocument::Compact))
                    : QStringLiteral("Agent-Antwort vom Typ %1").arg(type);
            }
        }
        content = removeReasoningLeak(content)
            .replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "))
            .trimmed();
        if (content.size() > maxChars) {
            content = content.left(std::max(12, maxChars - 3)).trimmed() + QStringLiteral("...");
        }
        return QStringLiteral("- %1: %2").arg(role, content);
    };

    auto compressedHistorySummary = [&](int olderMessages, int charBudget) {
        if (!includeConversationHistory || olderMessages <= 0) {
            return QString();
        }
        const int boundedBudget = std::max(800, charBudget);
        const int perMessageChars = std::clamp((boundedBudget - 240) / std::max(1, olderMessages), 40, 700);
        QStringList lines{
            QStringLiteral("Komprimierter bisheriger Sitzungsverlauf. Diese Zusammenfassung ersetzt %1 aeltere Nachrichten; die juengsten Nachrichten folgen vollstaendig.")
                .arg(olderMessages)
        };
        int usedChars = lines.first().size() + 1;
        for (int i = 0; i < olderMessages && i < m_agentConversation.size(); ++i) {
            const QString line = compactHistoryLine(m_agentConversation.at(i).toObject(), perMessageChars);
            if (usedChars + line.size() + 1 > boundedBudget) {
                lines << QStringLiteral("- Weitere %1 aeltere Nachrichten wurden aus Platzgruenden nur als Teil dieser kompakten Verlaufslage beruecksichtigt.")
                    .arg(olderMessages - i);
                break;
            }
            lines << line;
            usedChars += line.size() + 1;
        }
        return lines.join('\n');
    };

    auto buildAgentMessages = [&](int recentHistoryMessages, const QJsonObject& sourceEnvelope, const QString& historySummary) {
        ContextBuildResult result;
        result.envelope = sourceEnvelope;
        result.historyMessages = recentHistoryMessages;
        result.compressedHistoryMessages = historySummary.trimmed().isEmpty()
            ? 0
            : std::max(0, totalHistoryMessages - recentHistoryMessages);

        QJsonArray builtMessages;
        builtMessages.append(systemMessage);
        if (!historySummary.trimmed().isEmpty()) {
            builtMessages.append(QJsonObject{
                {"role", "system"},
                {"content", historySummary.trimmed()},
            });
        }

        QJsonArray recentConversation;
        const qsizetype historyStart = std::max<qsizetype>(0, m_agentConversation.size() - recentHistoryMessages);
        for (qsizetype i = historyStart; i < m_agentConversation.size(); ++i) {
            const QJsonObject item = m_agentConversation.at(i).toObject();
            const QJsonObject apiMessage{
                {QStringLiteral("role"), item.value(QStringLiteral("role")).toString()},
                {QStringLiteral("content"), item.value(QStringLiteral("content")).toString()},
            };
            builtMessages.append(apiMessage);
            recentConversation.append(apiMessage);
        }

        QJsonObject outboundEnvelope = sourceEnvelope;
        outboundEnvelope.insert("conversation", recentConversation);
        if (hasFocusedConversationContext) {
            result.compressedHistoryMessages = std::max(0, totalHistoryMessages - recentHistoryMessages);
            outboundEnvelope.insert("conversationCompression", QJsonObject{
                {"mode", "prompt-focused-ai-summary"},
                {"compressedMessages", result.compressedHistoryMessages},
                {"fullRecentMessages", recentHistoryMessages},
                {"focusedSummaryAvailable", true},
            });
            outboundEnvelope.insert("conversationAccess", conversationAccessForFocusedContext(focusedConversationContext));
        }
        if (!historySummary.trimmed().isEmpty()) {
            outboundEnvelope.insert("conversationSummary", historySummary.trimmed());
            outboundEnvelope.insert("conversationCompression", QJsonObject{
                {"mode", "older-history-summary"},
                {"compressedMessages", result.compressedHistoryMessages},
                {"fullRecentMessages", recentHistoryMessages},
            });
        }
        result.envelope = outboundEnvelope;
        builtMessages.append(QJsonObject{
            {"role", "user"},
            {"content", QString::fromUtf8(QJsonDocument(outboundEnvelope).toJson(QJsonDocument::Compact))},
        });
        result.messages = builtMessages;
        result.estimatedTokens = estimateTokensForMessages(builtMessages);
        return result;
    };

    auto buildBestContext = [&](const QJsonObject& sourceEnvelope, bool forceMinimized) {
        if (hasFocusedConversationContext) {
            ContextBuildResult smallest;
            smallest.estimatedTokens = std::numeric_limits<int>::max();
            // The prompt-focused summary already represents relevant history. Re-sending
            // raw recent turns can make local models answer the preceding CAD operation.
            const int maxFocusedRecentMessages = 0;
            for (int recentHistoryMessages = maxFocusedRecentMessages; recentHistoryMessages >= 0; --recentHistoryMessages) {
                ContextBuildResult candidate = buildAgentMessages(recentHistoryMessages, sourceEnvelope, {});
                candidate.minimized = forceMinimized;
                if (candidate.estimatedTokens < smallest.estimatedTokens) {
                    smallest = candidate;
                }
                if (candidate.estimatedTokens <= budget) {
                    return candidate;
                }
            }
            return smallest;
        }

        const int rawHistoryMessages = isBricsCadMode() ? 0 : totalHistoryMessages;
        ContextBuildResult best = buildAgentMessages(rawHistoryMessages, sourceEnvelope, {});
        best.minimized = forceMinimized;
        if (best.estimatedTokens <= budget) {
            return best;
        }

        ContextBuildResult smallest = best;
        const int summaryCharBudget = std::clamp(budget, 1200, 16384);
        for (int recentHistoryMessages = rawHistoryMessages - 1; recentHistoryMessages >= 0; --recentHistoryMessages) {
            const int olderMessages = totalHistoryMessages - recentHistoryMessages;
            const QString summary = compressedHistorySummary(olderMessages, summaryCharBudget);
            ContextBuildResult candidate = buildAgentMessages(recentHistoryMessages, sourceEnvelope, summary);
            candidate.minimized = true;
            if (candidate.estimatedTokens < smallest.estimatedTokens) {
                smallest = candidate;
            }
            if (candidate.estimatedTokens <= budget) {
                return candidate;
            }
        }

        return smallest;
    };

    ContextBuildResult contextBuild = plainGeneralResponse
        ? buildGeneralMessagesForBudget(slottedEnvelope, systemMessage, initialOutputTokens)
        : buildBestContext(slottedEnvelope, false);

    if (!plainGeneralResponse && contextBuild.estimatedTokens > budget) {
        QJsonObject minimizedEnvelope = slottedEnvelope;
        bool docMinimized = false;
        if (minimizedEnvelope.contains("documentContext")) {
            minimizedEnvelope.insert(
                "documentContext",
                documentContextWithTokenBudget(
                    minimizedEnvelope.value("documentContext").toObject(),
                    std::max(300, budget / 3),
                    &docMinimized));
        }
        contextBuild = buildBestContext(minimizedEnvelope, true);
        contextBuild.minimized = true;
    }

    if (plainGeneralResponse && contextBuild.estimatedTokens > budget) {
        emitContextBudget(
            contextBuild.estimatedTokens,
            true,
            QStringLiteral("VollstÃƒÂ¤ndiger Pflichtkontext ÃƒÂ¼berschreitet das Eingabebudget"));
        appendAgentChat(
            QStringLiteral("Barebone-Qt"),
            QStringLiteral(
                "Der vollstÃƒÂ¤ndige verpflichtende Workflow-, Dokument- oder Sitzungsverlauf passt nicht in das aktuelle Kontextfenster. "
                "Die Anfrage wurde nicht gekÃƒÂ¼rzt oder an die AI gesendet. Bitte vergrÃƒÂ¶ÃƒÅ¸ere das Kontextfenster oder reduziere den ausgewÃƒÂ¤hlten Kontext."));
        return;
    }

    const QJsonArray outboundMessages = contextBuild.messages;
    const int outputTokens = adjustedOutputTokenLimitForMessages(outboundMessages, requestedOutputTokens);
    QString contextDetail;
    if (contextBuild.minimized) {
        QStringList details;
        if (contextBuild.compressedHistoryMessages > 0) {
            details << (plainGeneralResponse
                ? QStringLiteral("%1 ÃƒÂ¤ltere Nachrichten vollstÃƒÂ¤ndig ausgelassen").arg(contextBuild.compressedHistoryMessages)
                : (hasFocusedConversationContext
                    ? QStringLiteral("%1 aeltere Nachrichten ueber Fokus-Zusammenfassung ersetzt").arg(contextBuild.compressedHistoryMessages)
                    : QStringLiteral("%1 aeltere Nachrichten komprimiert").arg(contextBuild.compressedHistoryMessages)));
        }
        if (contextBuild.historyMessages < totalHistoryMessages) {
            details << QStringLiteral("%1 juengste Nachrichten vollstaendig").arg(contextBuild.historyMessages);
        }
        contextDetail = details.isEmpty()
            ? QStringLiteral("Kontext automatisch minimiert")
            : (hasFocusedConversationContext
                ? QStringLiteral("Kontext budgetbedingt minimiert: %1").arg(details.join(QStringLiteral(", ")))
                : QStringLiteral("Kontext automatisch minimiert: %1").arg(details.join(QStringLiteral(", "))));
    }
    emitContextBudget(
        contextBuild.estimatedTokens,
        contextBuild.minimized,
        contextDetail);

    QString requestReasoningEffort = m_forceAgentReasoningOffNextRequest
        ? QStringLiteral("none")
        : m_reasoningEffort;
    if (bricsCadStructuredResponse) {
        requestReasoningEffort = slottedEnvelope.value(QStringLiteral("type")).toString() == QStringLiteral("finalization_retry")
            ? QStringLiteral("none")
            : QStringLiteral("low");
    }
    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", outboundMessages);
        payload.insert("max_output_tokens", outputTokens);
        const QString reasoningEffort = reasoningEffortForModel(model, requestReasoningEffort);
        if (reasoningEffort != "none") {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("temperature", 0.1);
        payload.insert("messages", outboundMessages);
        payload.insert("max_tokens", outputTokens);
        insertChatReasoningForModel(payload, model, requestReasoningEffort);
        if (!officialProvider && responseKind == AgentResponseKind::StructuredJson) {
            payload.insert(
                QStringLiteral("response_format"),
                localStructuredResponseFormat(
                    QStringLiteral("barebone_agent_response"),
                    {QStringLiteral("schema"), QStringLiteral("type")}));
        }
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    const QString reasoningLog = (useResponsesApi || isGemma4ReasoningModel(model))
        ? QString(" reasoning=%1").arg(reasoningEffortForModel(model, requestReasoningEffort))
        : QString();
    m_forceAgentReasoningOffNextRequest = false;
    const QJsonObject route = requestRoute;
    const QJsonObject modePolicy = slottedEnvelope.value("modePolicy").toObject();
    const QStringList toolNames = toolNamesForLog(slottedEnvelope.value("effectiveTools").toArray());
    const QStringList policyNames = jsonStringArrayToStringList(slottedEnvelope.value("policyRefs").toArray());
    const auto objectTokens = [](const QJsonObject& value) {
        return (QJsonDocument(value).toJson(QJsonDocument::Compact).size() + 3) / 4;
    };
    const auto arrayTokens = [](const QJsonArray& value) {
        return (QJsonDocument(value).toJson(QJsonDocument::Compact).size() + 3) / 4;
    };
    appendBridgeLog(QString("AI Kontextanteile: workflow=%1 tools=%2 cad=%3 policies=%4 responseContract=%5 pending=%6 historyMessages=%7")
        .arg(arrayTokens(contextBuild.envelope.value(QStringLiteral("workflowCapsules")).toArray()))
        .arg(arrayTokens(contextBuild.envelope.value(QStringLiteral("effectiveTools")).toArray()))
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("drawingContext")).toObject()))
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("policyText")).toObject()))
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("responseContract")).toObject()))
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("pendingProposal")).toObject())
            + objectTokens(contextBuild.envelope.value(QStringLiteral("pendingDraft")).toObject()))
        .arg(contextBuild.historyMessages));
    appendBridgeLog(QString("AI Envelope: mode=%1 route=%2 profile=%3 toolsSent=%4 policyRefs=%5 deduplicated=%6 estimatedTokens=%7")
        .arg(modePolicy.value("mode").toString(m_chatMode),
            route.value("route").toString("<leer>"),
            route.value("capabilityProfile").toString("<leer>"),
            toolNames.isEmpty() ? QStringLiteral("-") : toolNames.join(","),
            policyNames.isEmpty() ? QStringLiteral("-") : policyNames.join(","),
            isBricsCadMode() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(contextBuild.estimatedTokens));
    if (isBricsCadMode()) {
        appendBridgeLog("AI Pipeline: AI Reasoning startet");
    }
    appendBridgeLog(QString("Qt -> AI Agent: %1 provider=%2 endpoint=%3 model=%4%5 timeoutMs=%6 maxTokens=%7 context=%8")
        .arg(logLabel,
            provider,
            url.toString(),
            model,
            reasoningLog)
        .arg(kAiModelResponseTimeoutMs)
        .arg(outputTokens)
        .arg(effectiveContextWindowTokens()));
    if (contextBuild.minimized) {
        appendBridgeLog(QString("%1 used=%2/%3 tokens recentHistory=%4 compressedHistory=%5")
            .arg(hasFocusedConversationContext
                ? QStringLiteral("AI Kontext: budgetbedingt minimiert")
                : QStringLiteral("AI Kontext: automatisch minimiert"))
            .arg(contextBuild.estimatedTokens)
            .arg(effectiveContextWindowTokens())
            .arg(contextBuild.historyMessages)
            .arg(contextBuild.compressedHistoryMessages));
    }

    const int operationGeneration = m_operationGeneration;
    const QString jobKind = plainGeneralResponse
        ? QStringLiteral("ForegroundChat")
        : (routeAllowsCadActions(route) ? QStringLiteral("CadAction") : QStringLiteral("ContextContinuation"));
    enqueueAiPost(
        request,
        payload,
        jobKind,
        routeAllowsCadActions(route) ? 100 : 80,
        false,
        false,
        {},
        [this, userHistoryContent, storeUserMessage, operationGeneration, plainGeneralResponse, bricsCadStructuredResponse, slottedEnvelope](const LocalAiJobScheduler::Result& result) {
        if (operationGeneration != m_operationGeneration) {
            return;
        }

        const int httpStatus = result.httpStatus;
        const QByteArray body = result.body;
        if (result.networkError != QNetworkReply::NoError) {
            appendAgentChat("Barebone-Qt", QString("Fehler bei der AI Anfrage: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(result.errorString));
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("AI", QString("Antwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            appendBridgeLog(QString("AI Body: %1").arg(QString::fromUtf8(body).left(800)));
            return;
        }

        const QJsonObject responseObject = responseDocument.object();
        const ParsedModelOutput parsedOutput = parseModelOutput(
            responseObject,
            plainGeneralResponse ? AgentResponseKind::VisibleMarkdown : AgentResponseKind::StructuredJson);
        const QString reasoningText = parsedOutput.reasoning;
        const QString content = repairMojibakeText(parsedOutput.finalText).trimmed();
        m_nextAgentMessageContinuationAvailable = plainGeneralResponse && parsedOutput.truncated;
        m_nextAgentMessageContinuationReason = m_nextAgentMessageContinuationAvailable
            ? aiResponseTruncationReason(responseObject)
            : QString();
        if (content.trimmed().isEmpty()) {
            m_nextAgentMessageContinuationAvailable = false;
            m_nextAgentMessageContinuationReason.clear();
            if (plainGeneralResponse) {
                appendBridgeLog(QString("AI Allgemeiner Modus: leere Antwort reasoningChars=%1").arg(reasoningText.size()));
                if (!reasoningText.trimmed().isEmpty() && m_agentValidationRetries == 0) {
                    ++m_agentValidationRetries;
                    m_forceAgentReasoningOffNextRequest = true;
                    QJsonObject retryEnvelope = agentRequestEnvelope(
                        m_lastAgentUserPrompt,
                        m_lastDocumentContext,
                        normalizedAgentRouteForMode(
                            m_lastAgentRoute,
                            m_lastAgentUserPrompt,
                            m_lastDocumentContext,
                            m_chatMode));
                    retryEnvelope.insert(QStringLiteral("type"), QStringLiteral("finalization_retry"));
                    retryEnvelope.insert(QStringLiteral("instruction"), QStringLiteral(
                        "Die vorherige Ausgabe enthielt nur Reasoning und keinen finalen Inhalt. "
                        "Antworte jetzt ohne weiteres Reasoning direkt als sichtbare Markdown-Antwort und erfÃƒÂ¼lle die ursprÃƒÂ¼ngliche Nutzeranfrage vollstÃƒÂ¤ndig."));
                    appendBridgeLog(QStringLiteral(
                        "AI Allgemeiner Modus: kompakter Finalisierungsversuch mit minimalem Reasoning"));
                    sendAgentEnvelope(
                        retryEnvelope,
                        userHistoryContent,
                        storeUserMessage,
                        QStringLiteral("general_finalization_retry"));
                    return;
                }
                appendAgentChat("AI", "Leere Antwort erhalten.");
                return;
            }
            if (!reasoningText.trimmed().isEmpty() && m_agentValidationRetries == 0) {
                if (bricsCadStructuredResponse) {
                    ++m_agentValidationRetries;
                    m_forceAgentReasoningOffNextRequest = true;
                    QJsonObject retryEnvelope = slottedEnvelope;
                    retryEnvelope.insert(QStringLiteral("type"), QStringLiteral("finalization_retry"));
                    retryEnvelope.insert(QStringLiteral("instruction"), QStringLiteral(
                        "Die vorherige BricsCAD-Finalantwort enthielt nur Reasoning. "
                        "Antworte jetzt ohne Analyse mit genau einem kurzen gueltigen barebone.agent.response.v2 JSON-Objekt. "
                        "Nutze den vorhandenen Minimal-Kontext und deterministicBatchDraft; wiederhole keinen Kontext."));
                    retryEnvelope.remove(QStringLiteral("repairMode"));
                    retryEnvelope.remove(QStringLiteral("repairCandidateTools"));
                    appendBridgeLog(QStringLiteral("AI Agent: kompakter BricsCAD-Finalisierungsversuch ohne Repair-Kontext"));
                    sendAgentEnvelope(
                        retryEnvelope,
                        userHistoryContent,
                        storeUserMessage,
                        QStringLiteral("bricscad_finalization_retry"));
                    return;
                }
                m_forceAgentReasoningOffNextRequest = true;
                appendBridgeLog(QStringLiteral("AI Agent: Reasoning erschoepfte Ausgabebudget; ein kompakter Finalisierungsversuch mit minimalem Reasoning"));
            }
            if (m_agentValidationRetries == 0
                && retryAgentAfterValidationFailure(
                    reasoningText,
                    {},
                    "Die vorherige Ausgabe endete nach Reasoning ohne finale Antwort. Wiederhole das Reasoning nicht. Antworte jetzt sofort und ausschliesslich mit genau einem kurzen gueltigen Barebone-JSON-Objekt.")) {
                return;
            }
            appendAgentChat("Barebone-Qt", "Die lokale AI hat ihr Ausgabebudget im Reasoning verbraucht und auch im kompakten Finalisierungsversuch keine finale JSON-Antwort geliefert.");
            return;
        }
        appendBridgeLog(QString(plainGeneralResponse ? "AI Antwort: %1" : "AI JSON: %1").arg(content.left(1600)));
        if (m_nextAgentMessageContinuationAvailable) {
            appendBridgeLog(QString("AI Allgemeiner Modus: Antwort vermutlich abgeschnitten reason=%1")
                .arg(m_nextAgentMessageContinuationReason));
        }

        if (storeUserMessage) {
            m_agentConversation.append(QJsonObject{{"role", "user"}, {"content", userHistoryContent}});
        }
        m_agentConversation.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        emitContextBudget();
        if (plainGeneralResponse) {
            m_agentValidationRetries = 0;
            appendAgentChat(QStringLiteral("AI"), content);
        } else {
            bool parsedAgentObject = false;
            const QJsonObject agentObject = jsonObjectFromAiContent(content, &parsedAgentObject);
            const QString agentType = agentObject.value(QStringLiteral("type")).toString();
            const bool proposalNeedsValidation = parsedAgentObject
                && (agentType == QStringLiteral("action_proposal")
                    || agentType == QStringLiteral("tool_proposal")
                    || agentType == QStringLiteral("workflow_proposal")
                    || agentType == QStringLiteral("workflow_run")
                    || agentType == QStringLiteral("workflow_run_proposal"));
            if (!m_activeReasoningRunId.isEmpty()) {
                emitWebReasoningProgress(m_agentBridge, QVariantMap{
                    {QStringLiteral("schema"), QStringLiteral("barebone.agent.reasoning-progress.v1")},
                    {QStringLiteral("runId"), m_activeReasoningRunId},
                    {QStringLiteral("stageId"), QStringLiteral("final-run")},
                    {QStringLiteral("state"), proposalNeedsValidation ? QStringLiteral("running") : QStringLiteral("succeeded")},
                    {QStringLiteral("revision"), 3},
                    {QStringLiteral("label"), QStringLiteral("Finale Auswertung")},
                    {QStringLiteral("message"), proposalNeedsValidation
                        ? QStringLiteral("Vorschlag wird technisch geprueft")
                        : QStringLiteral("Ausfuehrung oder Auswertung vorbereitet")},
                });
                if (!proposalNeedsValidation) {
                    m_activeReasoningRunId.clear();
                }
            }
            handleAgentReply(content);
        }
        m_nextAgentMessageContinuationAvailable = false;
        m_nextAgentMessageContinuationReason.clear();
    });
}

QString BricsCadPage::generalWorkflowsDirectoryPath() const
{
    QDir root(bareboneProjectRootPath());
    return root.filePath(QStringLiteral("agent/general-workflows"));
}

QString BricsCadPage::bricsCadWorkflowsDirectoryPath() const
{
    QDir root(bareboneProjectRootPath());
    return root.filePath(QStringLiteral("agent/bricscad-workflows"));
}

QJsonArray BricsCadPage::generalWorkflowIndex() const
{
    QJsonArray workflows;
    QSet<QString> seen;

    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        generalWorkflowsDirectoryPath(),
        QStringLiteral(":/agent/general-workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        if (!readAgentWorkflowJson(source.path, &workflow)) {
            continue;
        }

        const QFileInfo info(source.fileName);
        const QString id = info.baseName();
        const QString slug = workflowSlug(id);
        if (seen.contains(slug)) {
            continue;
        }
        seen.insert(slug);

        QString title = repairMojibakeText(workflow.value(QStringLiteral("title")).toString()).trimmed();
        if (title.trimmed().isEmpty()) {
            title = repairMojibakeText(id).replace(QLatin1Char('_'), QLatin1Char(' '));
        }
        QString description = repairMojibakeText(workflow.value(QStringLiteral("description")).toString()).trimmed();
        if (description.isEmpty()) {
            description = workflowCompactSummaryForSelector(workflow);
        }

        workflows.append(QJsonObject{
            {"fileName", source.fileName},
            {"id", id},
            {"title", title},
            {"description", description},
            {"kind", "general"},
            {"readOnly", source.bundled},
            {"createdAt", workflowTimestampIso(source, workflow, QStringLiteral("createdAt"))},
            {"modifiedAt", workflowTimestampIso(source, workflow, QStringLiteral("modifiedAt"))},
            {"verificationStatus", repairMojibakeText(workflow.value(QStringLiteral("verificationStatus")).toString()).trimmed()},
        });
    }
    return workflows;
}

QJsonArray BricsCadPage::bricsCadWorkflowIndex() const
{
    QJsonArray workflows;
    QSet<QString> seen;
    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        bricsCadWorkflowsDirectoryPath(),
        QStringLiteral(":/agent/bricscad-workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        if (!readAgentWorkflowJson(source.path, &workflow)) {
            continue;
        }
        const QString fileBaseName = QFileInfo(source.fileName).baseName();
        const QString id = workflow.value(QStringLiteral("id")).toString(fileBaseName);
        const QString slug = workflowSlug(id);
        if (slug.isEmpty() || seen.contains(slug)) {
            continue;
        }
        seen.insert(slug);
        workflow.insert(QStringLiteral("id"), id);
        workflow.insert(QStringLiteral("fileName"), source.fileName);
        workflow.insert(QStringLiteral("kind"), QStringLiteral("bricscad"));
        workflow.insert(QStringLiteral("readOnly"), source.bundled);
        workflow.insert(QStringLiteral("createdAt"), workflowTimestampIso(source, workflow, QStringLiteral("createdAt")));
        workflow.insert(QStringLiteral("modifiedAt"), workflowTimestampIso(source, workflow, QStringLiteral("modifiedAt")));
        workflows.append(workflow);
    }
    return workflows;
}

QJsonArray BricsCadPage::workflowTrainingIndex() const
{
    return isChatWorkspace() ? generalWorkflowIndex() : bricsCadWorkflowIndex();
}

QJsonArray BricsCadPage::compactWorkflowSelectorList() const
{
    QJsonArray compactWorkflows;
    const QJsonArray workflows = workflowTrainingIndex();
    for (const QJsonValue& value : workflows) {
        const QJsonObject indexed = value.toObject();
        const QJsonObject workflow = indexed.value("workflow").toObject(indexed);
        const QString id = indexed.value("id").toString(workflow.value("id").toString()).trimmed();
        const QString title = repairMojibakeText(indexed.value("title").toString(workflow.value("title").toString())).trimmed();
        if (id.isEmpty() && title.isEmpty()) {
            continue;
        }

        QJsonArray triggerExamples;
        const QJsonArray rawTriggers = workflow.value("triggerExamples").toArray();
        for (int i = 0; i < rawTriggers.size() && i < 4; ++i) {
            const QString trigger = repairMojibakeText(rawTriggers.at(i).toString()).trimmed();
            if (!trigger.isEmpty()) {
                triggerExamples.append(trigger.left(180));
            }
        }

        QJsonArray stepPreview;
        const QJsonArray displaySteps = workflowDisplaySteps(workflow);
        for (int i = 0; i < displaySteps.size() && i < 8; ++i) {
            const QJsonObject step = displaySteps.at(i).toObject();
            stepPreview.append(QJsonObject{
                {"title", repairMojibakeText(step.value("title").toString(step.value("id").toString())).left(120)},
                {"tool", step.value("tool").toString()},
            });
        }

        compactWorkflows.append(QJsonObject{
            {"id", id},
            {"fileName", indexed.value("fileName").toString()},
            {"title", title.isEmpty() ? id : title},
            {"compactSummary", workflowCompactSummaryForSelector(workflow)},
            {"keywords", stringsToJsonArray(workflowKeywordsForSelector(workflow, 24))},
            {"triggerExamples", triggerExamples},
            {"preferredTools", stringsToJsonArray(workflowToolNamesForSelector(workflow, 12))},
            {"stepTools", stringsToJsonArray(workflowToolNamesForSelector(workflow, 12))},
            {"slots", stringsToJsonArray(workflowSlotNamesForSelector(workflow, 12))},
            {"slotIndex", stringsToJsonArray(workflowSlotNamesForSelector(workflow, 12))},
            {"stepSummary", workflowStepSummaryForSelector(workflow, 8)},
            {"stepPreview", stepPreview},
        });
    }
    return compactWorkflows;
}

void BricsCadPage::emitWorkflowListToWeb() const
{
    if (!m_agentBridge) {
        return;
    }
    emitToWebAsync(m_agentBridge, [workflows = workflowTrainingIndex().toVariantList()](AiWebBridge* target) {
        Q_EMIT target->workflowListChanged(workflows);
    });
}

QJsonObject BricsCadPage::loadWorkflowById(const QString& workflowId, QString* fileName, QString* errorMessage) const
{
    if (isChatWorkspace()) {
        return loadGeneralWorkflowById(workflowId, fileName, errorMessage);
    }
    return loadBricsCadWorkflowById(workflowId, fileName, errorMessage);
}

QJsonObject BricsCadPage::loadBricsCadWorkflowById(const QString& workflowId, QString* fileName, QString* errorMessage) const
{
    const QString normalizedId = workflowSlug(workflowId);
    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        bricsCadWorkflowsDirectoryPath(),
        QStringLiteral(":/agent/bricscad-workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        if (!readAgentWorkflowJson(source.path, &workflow)) {
            continue;
        }
        const QString fileBaseName = QFileInfo(source.fileName).baseName();
        const QString id = workflow.value(QStringLiteral("id")).toString(fileBaseName);
        if (workflowSlug(id) != normalizedId && workflowSlug(fileBaseName) != normalizedId) {
            continue;
        }
        if (fileName) {
            *fileName = source.fileName;
        }
        workflow.insert(QStringLiteral("id"), id);
        workflow.insert(QStringLiteral("fileName"), source.fileName);
        workflow.insert(QStringLiteral("kind"), QStringLiteral("bricscad"));
        workflow.insert(QStringLiteral("readOnly"), source.bundled);
        return workflow;
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("BricsCAD-Workflow \"%1\" wurde nicht gefunden.").arg(workflowId);
    }
    return {};
}

QJsonObject BricsCadPage::loadGeneralWorkflowById(const QString& workflowId, QString* fileName, QString* errorMessage) const
{
    const QString normalizedId = workflowSlug(workflowId);
    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        generalWorkflowsDirectoryPath(),
        QStringLiteral(":/agent/general-workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        if (!readAgentWorkflowJson(source.path, &workflow)) {
            continue;
        }

        const QString fileBaseName = QFileInfo(source.fileName).baseName();
        const QString id = workflow.value("id").toString(fileBaseName);
        if (workflowSlug(id) == normalizedId || workflowSlug(fileBaseName) == normalizedId) {
            if (fileName) {
                *fileName = source.fileName;
            }
            if (workflowSlug(id) != workflowSlug(fileBaseName)) {
                workflow.insert(QStringLiteral("sourceId"), id);
            }
            workflow.insert(QStringLiteral("id"), fileBaseName);
            workflow.insert(QStringLiteral("fileName"), source.fileName);
            workflow.insert(QStringLiteral("kind"), QStringLiteral("general"));
            workflow.insert(QStringLiteral("readOnly"), source.bundled);
            return workflow;
        }
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("Workflow \"%1\" wurde nicht gefunden.").arg(workflowId);
    }
    return {};
}

bool BricsCadPage::saveGeneralWorkflowFromObject(const QJsonObject& workflow, QString* savedPath, QString* errorMessage) const
{
    QJsonObject normalized = normalizedGeneralWorkflowDraft(workflow);
    QString id = workflowSlug(normalized.value(QStringLiteral("id")).toString());
    if (id.isEmpty()) {
        id = workflowSlug(normalized.value(QStringLiteral("title")).toString(QStringLiteral("workflow")));
    }
    if (id.isEmpty()) {
        id = QStringLiteral("workflow");
    }
    normalized.insert(QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.v1"));
    normalized.insert(QStringLiteral("id"), id);
    normalized.insert(QStringLiteral("kind"), QStringLiteral("general"));
    if (normalized.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        normalized.insert(QStringLiteral("title"), id);
    }

    QDir dir(generalWorkflowsDirectoryPath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Allgemeines Workflow-Verzeichnis konnte nicht erstellt werden: %1").arg(dir.absolutePath());
        }
        return false;
    }

    const QString fileName = id + QStringLiteral(".json");
    QFile file(dir.filePath(fileName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow konnte nicht gespeichert werden: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(normalized).toJson(QJsonDocument::Indented));
    file.write("\n");
    if (savedPath) {
        *savedPath = QFileInfo(file).absoluteFilePath();
    }
    return true;
}

QJsonObject generalWorkflowWithUniqueId(QJsonObject workflow, const QString& directoryPath)
{
    workflow = normalizedGeneralWorkflowDraft(workflow);
    QString baseId = workflowSlug(workflow.value(QStringLiteral("id")).toString());
    if (baseId.isEmpty()) {
        baseId = workflowSlug(workflow.value(QStringLiteral("title")).toString(QStringLiteral("workflow")));
    }
    if (baseId.isEmpty()) {
        baseId = QStringLiteral("workflow");
    }

    QDir dir(directoryPath);
    QString id = baseId;
    int suffix = 2;
    while (QFileInfo::exists(dir.filePath(id + QStringLiteral(".json")))) {
        id = QStringLiteral("%1-%2").arg(baseId).arg(suffix++);
    }
    workflow.insert(QStringLiteral("id"), id);
    return workflow;
}

bool BricsCadPage::deleteGeneralWorkflowById(const QString& workflowId, QString* deletedPath, QString* errorMessage)
{
    QString fileName;
    QJsonObject workflow = loadGeneralWorkflowById(workflowId, &fileName, errorMessage);
    if (workflow.isEmpty() || fileName.trimmed().isEmpty()) {
        if (errorMessage && errorMessage->trimmed().isEmpty()) {
            *errorMessage = QStringLiteral("Workflow wurde nicht gefunden.");
        }
        return false;
    }
    if (workflow.value(QStringLiteral("readOnly")).toBool(false)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Mitgelieferte Workflows aus dem App-Bundle koennen nicht geloescht werden.");
        }
        return false;
    }

    QDir dir(generalWorkflowsDirectoryPath());
    QFile file(dir.filePath(fileName));
    const QString absolutePath = QFileInfo(file).absoluteFilePath();
    if (!file.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow-Datei fehlt: %1").arg(absolutePath);
        }
        return false;
    }
    if (!file.remove()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow konnte nicht geloescht werden: %1").arg(file.errorString());
        }
        return false;
    }

    if (deletedPath) {
        *deletedPath = absolutePath;
    }
    if (workflowSlug(m_selectedWorkflowId) == workflowSlug(workflowId)
        || workflowSlug(m_selectedWorkflow.value(QStringLiteral("id")).toString()) == workflowSlug(workflowId)) {
        m_selectedWorkflowId.clear();
        m_selectedWorkflow = {};
        m_selectedWorkflowSlotValues = {};
    }
    emitWorkflowListToWeb();
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(QVariantMap{});
        });
    }
    return true;
}

bool BricsCadPage::deleteWorkflowById(const QString& workflowId, QString* deletedPath, QString* errorMessage)
{
    if (isChatWorkspace()) {
        return deleteGeneralWorkflowById(workflowId, deletedPath, errorMessage);
    }
    return deleteBricsCadWorkflowById(workflowId, deletedPath, errorMessage);
}

bool BricsCadPage::deleteBricsCadWorkflowById(const QString& workflowId, QString* deletedPath, QString* errorMessage)
{
    QString fileName;
    QJsonObject workflow = loadBricsCadWorkflowById(workflowId, &fileName, errorMessage);
    if (workflow.isEmpty() || fileName.trimmed().isEmpty()) {
        return false;
    }
    if (workflow.value(QStringLiteral("readOnly")).toBool(false)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Der gebuendelte BricsCAD-Workflow ist schreibgeschuetzt.");
        }
        return false;
    }
    const QDir directory(bricsCadWorkflowsDirectoryPath());
    const QString directoryPath = QFileInfo(directory.absolutePath()).canonicalFilePath();
    const QString targetPath = QFileInfo(directory.filePath(fileName)).canonicalFilePath();
    if (directoryPath.isEmpty() || targetPath.isEmpty()
        || !targetPath.startsWith(directoryPath + QDir::separator(), Qt::CaseInsensitive)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Ungueltiger BricsCAD-Workflow-Pfad.");
        }
        return false;
    }
    QFile file(targetPath);
    if (!file.remove()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("BricsCAD-Workflow konnte nicht geloescht werden: %1").arg(file.errorString());
        }
        return false;
    }
    if (deletedPath) {
        *deletedPath = targetPath;
    }
    if (workflowSlug(m_selectedWorkflowId) == workflowSlug(workflowId)) {
        m_selectedWorkflowId.clear();
        m_selectedWorkflow = {};
        m_selectedWorkflowSlotValues = {};
    }
    emitWorkflowListToWeb();
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(QVariantMap{});
        });
    }
    return true;
}

QJsonObject BricsCadPage::selectedWorkflowSummary() const
{
    if (m_selectedWorkflow.isEmpty()) {
        return {};
    }
    if (isChatWorkspace()) {
        QJsonObject summary = m_selectedWorkflow;
        summary.insert(QStringLiteral("selectedSlotValues"), m_selectedWorkflowSlotValues);
        return summary;
    }
    return QJsonObject{
        {"id", m_selectedWorkflow.value("id").toString(m_selectedWorkflowId)},
        {"title", m_selectedWorkflow.value("title").toString(m_selectedWorkflowId)},
        {"description", m_selectedWorkflow.value("description").toString()},
        {"triggerExamples", m_selectedWorkflow.value("triggerExamples").toArray()},
        {"requiredSlots", m_selectedWorkflow.value("requiredSlots").toArray()},
        {"optionalSlots", m_selectedWorkflow.value("optionalSlots").toObject()},
        {"knownSlotValues", m_selectedWorkflow.value("knownSlotValues").toObject()},
        {"recommendedTools", m_selectedWorkflow.value("recommendedTools").toArray()},
        {"executionBatches", m_selectedWorkflow.value("executionBatches").toArray()},
        {"validationExamples", m_selectedWorkflow.value("validationExamples").toArray()},
        {"observedCommands", m_selectedWorkflow.value("observedCommands").toArray()},
        {"observedTraces", m_selectedWorkflow.value("observedTraces").toArray()},
        {"semanticActions", m_selectedWorkflow.value("semanticActions").toArray()},
        {"requiredRuntimeContext", m_selectedWorkflow.value("requiredRuntimeContext").toObject()},
        {"missingBindings", m_selectedWorkflow.value("missingBindings").toArray()},
        {"commandTemplates", m_selectedWorkflow.value("commandTemplates").toArray()},
        {"postconditions", m_selectedWorkflow.value("postconditions").toArray()},
        {"readbacks", m_selectedWorkflow.value("readbacks").toArray()},
        {"updateProtected", m_selectedWorkflow.value("updateProtected").toBool(false)},
        {"selectedSlotValues", m_selectedWorkflowSlotValues},
    };
}

QJsonArray BricsCadPage::selectedWorkflowObjectsForRoute(const QJsonObject& route) const
{
    QJsonArray workflows;
    QSet<QString> seen;

    auto appendWorkflow = [&](QJsonObject workflow) {
        if (workflow.isEmpty()) {
            return;
        }
        const QString id = workflow.value("id").toString(workflow.value("fileName").toString());
        const QString slug = workflowSlug(id);
        if (slug.isEmpty() || seen.contains(slug)) {
            return;
        }
        seen.insert(slug);
        workflow.remove(QStringLiteral("validationWarnings"));
        workflows.append(workflow);
    };

    // An explicit overlay selection is authoritative and must be the full
    // first capsule. Route matches are only supplemental context.
    if (!m_selectedWorkflow.isEmpty()) {
        QJsonObject selected = m_selectedWorkflow;
        QJsonObject runtimeValues = selected.value(QStringLiteral("knownSlotValues")).toObject();
        for (auto it = m_selectedWorkflowSlotValues.begin(); it != m_selectedWorkflowSlotValues.end(); ++it) {
            runtimeValues.insert(it.key(), it.value());
        }
        selected.insert(QStringLiteral("knownSlotValues"), runtimeValues);
        appendWorkflow(selected);
    }

    for (const QString& id : routeWorkflowIds(route, 3)) {
        QString errorMessage;
        QJsonObject workflow = loadWorkflowById(id, nullptr, &errorMessage);
        if (!workflow.isEmpty()) {
            appendWorkflow(workflow);
        }
    }
    return workflows;
}

QJsonArray BricsCadPage::workflowHintObjectsForRoute(const QJsonObject& route) const
{
    QJsonArray workflows;
    QSet<QString> seen;
    for (const QString& activeId : routeWorkflowIds(route, 6)) {
        const QString activeSlug = workflowSlug(activeId);
        if (!activeSlug.isEmpty()) {
            seen.insert(activeSlug);
        }
    }

    const QJsonArray hints = route.value(QStringLiteral("workflowHints")).toArray();
    for (const QJsonValue& value : hints) {
        QString id;
        if (value.isString()) {
            id = value.toString().trimmed();
        } else if (value.isObject()) {
            const QJsonObject object = value.toObject();
            id = object.value(QStringLiteral("id")).toString(
                object.value(QStringLiteral("workflowId")).toString());
        }
        const QString slug = workflowSlug(id);
        if (slug.isEmpty() || seen.contains(slug)) {
            continue;
        }
        QString errorMessage;
        QJsonObject workflow = loadWorkflowById(id, nullptr, &errorMessage);
        if (workflow.isEmpty()) {
            continue;
        }
        workflow.remove(QStringLiteral("validationWarnings"));
        workflows.append(workflow);
        seen.insert(slug);
        if (workflows.size() >= 3) {
            break;
        }
    }
    return workflows;
}


void BricsCadPage::selectWorkflowForChat(const QString& workflowId)
{
    QString fileName;
    QString errorMessage;
    QJsonObject workflow = loadWorkflowById(workflowId, &fileName, &errorMessage);
    if (workflow.isEmpty()) {
        appendAgentChat("Barebone-Qt", errorMessage.isEmpty()
            ? QStringLiteral("Workflow konnte nicht geladen werden.")
            : errorMessage);
        return;
    }

    m_selectedWorkflowId = workflow.value("id").toString(QFileInfo(fileName).baseName());
    m_selectedWorkflow = workflow;
    m_selectedWorkflowSlotValues = workflow.value("knownSlotValues").toObject();
    if (m_trainingMode) {
        m_trainingWorkflowContext = workflow;
        m_trainingSlotValues = m_selectedWorkflowSlotValues;
        m_trainingMissing = {};
        m_trainingPhase = QStringLiteral("editing");
    }
    appendBridgeLog(QString("Workflow ausgewaehlt: %1 (%2)")
        .arg(m_selectedWorkflow.value("title").toString(m_selectedWorkflowId), fileName));
    if (m_agentBridge) {
        QVariantMap payload = workflow.toVariantMap();
        payload.insert(QStringLiteral("selected"), true);
        emitToWebAsync(m_agentBridge, [payload](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(payload);
        });
    }
}






void BricsCadPage::clearSelectedWorkflowForChat()
{
    const QString oldWorkflowId = m_selectedWorkflow.value("id").toString(m_selectedWorkflowId);
    m_selectedWorkflowId.clear();
    m_selectedWorkflow = {};
    m_selectedWorkflowSlotValues = {};
    if (workflowSlug(m_trainingWorkflowContext.value("id").toString()) == workflowSlug(oldWorkflowId)) {
        m_trainingWorkflowContext = {};
        m_trainingSlotValues = {};
        m_trainingMissing = {};
    }
    appendBridgeLog("Workflow Auswahl aufgehoben");
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(QVariantMap{});
        });
    }
}

void BricsCadPage::exportAgentMessageToPdf(const QString& messageId, const QString& suggestedTitle)
{
    if (!m_agentWebView || !m_agentWebView->page()) {
        return;
    }

    const QString trimmedMessageId = messageId.trimmed();
    if (trimmedMessageId.isEmpty()) {
        appendAgentChat("Barebone-Qt", QStringLiteral("PDF-Export nicht moeglich: Nachricht wurde nicht gefunden."));
        return;
    }

    QString title = repairMojibakeText(suggestedTitle).trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("AI Nachricht");
    }
    QString fileSlug = workflowSlug(title);
    if (fileSlug == QStringLiteral("workflow")) {
        fileSlug = QStringLiteral("ai_nachricht");
    }

    QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    const QString defaultPath = QDir(directory).filePath(fileSlug + QStringLiteral(".pdf"));
    QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Nachricht als PDF speichern"),
        defaultPath,
        QStringLiteral("PDF-Dateien (*.pdf)"));
    if (filePath.trimmed().isEmpty()) {
        return;
    }
    if (!filePath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        filePath += QStringLiteral(".pdf");
    }

    const QString prepareScript = QStringLiteral(R"JS(
(() => {
  const messageId = %1;
  document.documentElement.classList.remove("pdf-exporting-message");
  document.body.classList.remove("pdf-exporting-message");
  document.querySelectorAll(".pdf-export-target").forEach((node) => node.classList.remove("pdf-export-target"));
  const target = Array.from(document.querySelectorAll(".message")).find((node) => node.dataset.messageId === messageId);
  if (!target) {
    return false;
  }
  target.classList.add("pdf-export-target");
  document.documentElement.classList.add("pdf-exporting-message");
  document.body.classList.add("pdf-exporting-message");
  if (typeof window.__bareboneTypesetMessageMath === "function") {
    window.__bareboneTypesetMessageMath(messageId);
  }
  return true;
})()
)JS").arg(jsStringLiteral(trimmedMessageId));

    const QString cleanupScript = QStringLiteral(R"JS(
(() => {
  document.documentElement.classList.remove("pdf-exporting-message");
  document.body.classList.remove("pdf-exporting-message");
  document.querySelectorAll(".pdf-export-target").forEach((node) => node.classList.remove("pdf-export-target"));
})()
)JS");

    QPointer<BricsCadPage> guard(this);
    m_agentWebView->page()->runJavaScript(prepareScript, [guard, filePath, cleanupScript](const QVariant& result) {
        if (!guard || !guard->m_agentWebView || !guard->m_agentWebView->page()) {
            return;
        }
        if (!result.toBool()) {
            guard->appendAgentChat("Barebone-Qt", QStringLiteral("PDF-Export nicht moeglich: Nachricht wurde nicht gefunden."));
            return;
        }

        QTimer::singleShot(700, guard, [guard, filePath, cleanupScript]() {
            if (!guard || !guard->m_agentWebView || !guard->m_agentWebView->page()) {
                return;
            }

            QPageLayout layout(
                QPageSize(QPageSize::A4),
                QPageLayout::Portrait,
                QMarginsF(12.0, 12.0, 12.0, 12.0),
                QPageLayout::Millimeter);

            guard->m_agentWebView->page()->printToPdf(
                [guard, filePath, cleanupScript](const QByteArray& pdfData) {
                    if (!guard || !guard->m_agentWebView || !guard->m_agentWebView->page()) {
                        return;
                    }
                    guard->m_agentWebView->page()->runJavaScript(cleanupScript);

                    if (pdfData.isEmpty()) {
                        guard->appendAgentChat("Barebone-Qt", QStringLiteral("PDF-Export fehlgeschlagen."));
                        return;
                    }

                    QFile file(filePath);
                    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        guard->appendAgentChat("Barebone-Qt", QStringLiteral("PDF konnte nicht geschrieben werden: %1").arg(file.errorString()));
                        return;
                    }
                    file.write(pdfData);
                    file.close();
                },
                layout);
        });
    });
}

QJsonObject BricsCadPage::workflowTrainingState() const
{
    const QJsonObject activeWorkflow = m_trainingWorkflowContext;
    return QJsonObject{
        {"phase", m_trainingPhase},
        {"knownSlotValues", m_trainingSlotValues},
        {"lastMissing", m_trainingMissing},
        {"activeWorkflow", activeWorkflow},
        {"selectedWorkflow", selectedWorkflowSummary()},
        {"saveReviewPending", m_trainingSaveReviewPending},
        {"reviewConfirmed", m_trainingReviewConfirmed},
    };
}

void BricsCadPage::mergeWorkflowTrainingSlotValues(const QJsonObject& slots)
{
    if (slots.isEmpty()) {
        return;
    }

    for (auto it = slots.begin(); it != slots.end(); ++it) {
        const QString canonical = canonicalWorkflowSlot(it.key());
        if (!it.key().trimmed().isEmpty()) {
            m_trainingSlotValues.insert(it.key(), it.value());
        }
        if (!canonical.isEmpty()) {
            m_trainingSlotValues.insert(canonical, it.value());
        }
    }
    if (!m_trainingSlotValues.isEmpty()) {
        m_trainingWorkflowContext.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }
    m_trainingMissing = unresolvedWorkflowTrainingMissing(m_trainingMissing);
}

void BricsCadPage::mergeWorkflowTrainingContext(const QJsonObject& context)
{
    if (context.isEmpty()) {
        return;
    }

    QJsonObject merged = m_trainingWorkflowContext;
    for (auto it = context.begin(); it != context.end(); ++it) {
        if (it.key() == QStringLiteral("requiredSlots")) {
            QJsonArray slots = merged.value(it.key()).toArray();
            QStringList known = canonicalWorkflowSlotsFromArray(slots);
            for (const QJsonValue& value : it.value().toArray()) {
                const QString canonical = canonicalWorkflowSlot(workflowSlotNameFromValue(value));
                if (!canonical.isEmpty() && !known.contains(canonical)) {
                    slots.append(value);
                    known << canonical;
                }
            }
            merged.insert(it.key(), slots);
            continue;
        }

        if (it.key() == QStringLiteral("optionalSlots")) {
            QJsonObject optionalSlots = merged.value(it.key()).isObject()
                ? merged.value(it.key()).toObject()
                : QJsonObject{};
            const QJsonObject incoming = normalizedWorkflowOptionalSlots(it.value());
            for (auto slotIt = incoming.begin(); slotIt != incoming.end(); ++slotIt) {
                optionalSlots.insert(slotIt.key(), slotIt.value());
            }
            merged.insert(it.key(), optionalSlots);
            continue;
        }

        if ((it.key() == QStringLiteral("knownSlotValues") || it.key() == QStringLiteral("slotValues"))
            && it.value().isObject()) {
            mergeWorkflowTrainingSlotValues(it.value().toObject());
            merged.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
            continue;
        }

        if (it.value().isArray()
            && (it.key() == QStringLiteral("derivedValues")
                || it.key() == QStringLiteral("preferredTools")
                || it.key() == QStringLiteral("steps")
                || it.key() == QStringLiteral("executionBatches")
                || it.key() == QStringLiteral("constructionStrategy")
                || it.key() == QStringLiteral("validationExamples")
                || it.key() == QStringLiteral("forbidden"))) {
            merged.insert(it.key(), it.value());
            continue;
        }

        if (it.value().isArray() && merged.value(it.key()).isArray()) {
            QJsonArray values = merged.value(it.key()).toArray();
            const QJsonArray incoming = it.value().toArray();
            for (const QJsonValue& value : incoming) {
                if (!values.contains(value)) {
                    values.append(value);
                }
            }
            merged.insert(it.key(), values);
            continue;
        }

        if (!it.value().isNull() && !it.value().isUndefined()) {
            merged.insert(it.key(), it.value());
        }
    }

    if (!m_trainingSlotValues.isEmpty()) {
        merged.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }
    merged = workflowWithPrunedRequiredSlots(merged);
    m_trainingWorkflowContext = merged;
    m_trainingMissing = unresolvedWorkflowTrainingMissing(m_trainingMissing);
}

QJsonArray BricsCadPage::unresolvedWorkflowTrainingMissing(const QJsonArray& missing) const
{
    QJsonArray unresolved;
    const QStringList referencedSlots = workflowTemplateReferencedSlots(m_trainingWorkflowContext);
    for (const QJsonValue& value : missing) {
        const QString name = workflowSlotNameFromValue(value);
        const QString canonical = canonicalWorkflowSlot(name);
        if (!referencedSlots.isEmpty() && !referencedSlots.contains(canonical)) {
            continue;
        }
        if (!canonical.isEmpty()
            && (m_trainingSlotValues.contains(canonical) || m_trainingSlotValues.contains(name))) {
            continue;
        }
        unresolved.append(value);
    }
    return unresolved;
}

QJsonObject BricsCadPage::workflowTrainingEnvelope(const QString& prompt, bool compactContext) const
{
    QJsonArray toolNames;
    QJsonArray effectiveTools;
    const QJsonArray allTools = availableAgentTools();
    const QString normalizedPrompt = QString("%1\n%2")
        .arg(prompt, m_lastAgentUserPrompt)
        .toLower();
    for (const QJsonValue& value : allTools) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value("name").toString();
        if (!name.isEmpty()) {
            if (!workflowTrainingToolRelevantForPrompt(name, normalizedPrompt, compactContext)) {
                continue;
            }
            toolNames.append(name);
            QJsonObject compactTool{
                {"name", name},
                {"title", tool.value("title").toString(name)},
                {"description", tool.value("description").toString()},
                {"kind", tool.value("kind").toString("action")},
                {"risk", tool.value("risk").toString()},
                {"category", tool.value("category").toString()},
                {"bridgeMethod", tool.value("bridgeMethod").toString(name)},
                {"inputSchema", tool.value("inputSchema").toObject()},
                {"capabilities", tool.value("capabilities").toObject()},
                {"agentHints", tool.value("agentHints").toArray()},
                {"semanticConstraints", tool.value("semanticConstraints").toArray()},
                {"unsupportedOperations", tool.value("unsupportedOperations").toArray()},
            };
            const QJsonObject apiPost = tool.value("apiDoc").toObject().value("post").toObject();
            if (!apiPost.isEmpty()) {
                compactTool.insert("apiPost", apiPost);
            }
            effectiveTools.append(compactTool);
        }
    }
    if (toolNames.isEmpty()) {
        for (const QString& name : QStringList{
                 QStringLiteral("layers.ensureMany"),
                 QStringLiteral("geometry.create"),
                 QStringLiteral("geometry.move"),
                 QStringLiteral("geometry.copy"),
                 QStringLiteral("geometry.rotate"),
                 QStringLiteral("geometry.scale"),
                 QStringLiteral("geometry.delete"),
                 QStringLiteral("rectangles.extrude"),
                 QStringLiteral("circle.extrude"),
                 QStringLiteral("profile.extrude"),
                 QStringLiteral("bim.create"),
                 QStringLiteral("bim.move"),
                 QStringLiteral("bim.classify"),
                 QStringLiteral("bim.objects.query"),
                 QStringLiteral("bim.selection.set"),
                 QStringLiteral("selection.set"),
                 QStringLiteral("document.save")}) {
            toolNames.append(name);
        }
    }

    QJsonArray existingWorkflows = workflowTrainingIndex();
    if (compactContext) {
        QJsonArray compactWorkflows;
        for (const QJsonValue& value : existingWorkflows) {
            const QJsonObject item = value.toObject();
            compactWorkflows.append(QJsonObject{
                {"fileName", item.value("fileName").toString()},
                {"id", item.value("id").toString()},
                {"title", item.value("title").toString()},
            });
        }
        existingWorkflows = compactWorkflows;
    }

    const QJsonObject activeWorkflow = m_trainingWorkflowContext.isEmpty()
        ? m_selectedWorkflow
        : m_trainingWorkflowContext;

    return QJsonObject{
        {"schema", "barebone.workflow.training.request.v1"},
        {"mode", "workflow_training"},
        {"compactContext", compactContext},
        {"userPrompt", prompt},
        {"instruction",
            "Trainingsmodus ist im BricsCAD-Arbeitsbereich deaktiviert. "
            "Ohne trainingState.activeWorkflow ist jede freie Nutzeranfrage als neuer Workflow-Entwurf zu behandeln. Suche dann keine bestehenden Workflows zum Bearbeiten oder Erweitern. "
            "Bestehende Workflows bearbeitest oder erweiterst du nur, wenn trainingState.activeWorkflow durch das Workflow-Overlay explizit geladen wurde. "
            "Erzeuge keine CAD-Aktion und keine normale Barebone-Agent-action_proposal. "
            "Qt verwaltet trainingState, knownSlotValues und activeWorkflow verbindlich. Frage bekannte Slots niemals erneut ab. "
            "Wenn der Nutzer die Auswahl an dich delegiert (z.B. 'erstelle selber', 'waehle sinnvoll', 'nutze passende Themen', 'schlage vor'), dann musst du fachlich plausible Werte selbst erzeugen und als knownSlotValues, derivedValues oder direkte paramsTemplate-Konstanten speichern; frage diese Werte nicht erneut ab. "
            "Du darfst eigene Workflow-Variablen anlegen, wenn sie die Aufgabe kompakter machen, z.B. knownSlotValues.tgaLayers als Array fuer layers.ensureMany.paramsTemplate.layers='{{tgaLayers}}'. "
            "Bei 'TGA Gruppen' ohne konkrete Liste erstelle eigenstaendig plausible deutsche TGA-Layergruppen und ACI-Farben; fuer 10 Layer z.B. Sanitaer, Heizung, Lueftung, Elektro, Kaelte, Sprinkler, Gas, MSR, Brandschutz, Daemmung mit sinnvollen colorIndex-Werten. "
            "Lange Erstprompts mit Titel, Beschreibung, Batch-Ausfuehrungen, Formeln oder komplexer Logik strukturierst du zuerst als type=workflow_draft. "
            "workflow_draft ist ein sichtbarer Entwurf und darf workflowDraft mit description, derivedValues, executionBatches, constructionStrategy, authoringNotes und offenen questions enthalten, aber er wird nicht gespeichert. "
            "Sammle fehlende Angaben mit kurzen type=ask_user Antworten, bestaetige neu erkannte Werte mit type=slot_update oder aktualisiere den sichtbaren Entwurf mit type=workflow_draft. "
            "Nutze ask_user nur fuer echte fachliche Entscheidungen, die du nicht plausibel selbst treffen darfst. Wenn der Nutzer dir die Entscheidung ausdruecklich ueberlassen hat, ist ask_user fuer Namen/Farben/Defaultwerte ungueltig. "
            "Vor dem Speichern erzwingt Qt eine Review-Phase: Wenn trainingState.saveReviewPending=true ist, pruefe Titel, Beschreibung, Pflichtangaben, Beispielwerte, Formeln, Batch-Ausfuehrungen, Tool-Parameter, Validierungsbeispiel und Risiken punktweise und antworte mit type=workflow_draft plus questions. "
            "Waehrend saveReviewPending=true darfst du kein workflow_update ausgeben, ausser der Nutzer hat die Review ausdruecklich bestaetigt und Qt fordert final dazu auf. "
            "Wenn noch Angaben fehlen, antworte mit type=ask_user, missing und questions; gib dabei hoechstens workflowSeed mit id/title/requiredSlots aus, aber keinen vollstaendigen Workflow. "
            "Erzeuge type=workflow_update erst, wenn die benoetigten Pflichtslots fachlich geklaert oder als variable requiredSlots definiert sind. "
            "Wenn der Nutzer den Entwurf bestaetigt oder Speichern/Aktualisieren verlangt, wandle workflowDraft in ein vollstaendiges workflow_update mit steps und validationExamples um. "
            "Wenn der Nutzer einen bestehenden Workflow erweitern oder bearbeiten moechte, aber trainingState.activeWorkflow leer ist, erstelle stattdessen einen neuen Workflow-Entwurf oder weise kurz darauf hin, dass Bearbeiten ueber das Workflow-Overlay gestartet wird. "
            "Wenn genug Daten vorhanden sind oder der Nutzer Speichern/Aktualisieren verlangt, antworte mit type=workflow_update und einem vollstaendigen workflow Objekt. "
            "Setze requiredSlots fuer Werte, die in paramsTemplate, condition oder derivedValues per {{slot}} verwendet werden. Fachlich zwingender Prozesskontext darf zusaetzlich als Slotobjekt mit requiredForExecution=true erhalten bleiben, auch wenn er nicht direkt an ein Tool gesendet wird; dies gilt insbesondere fuer Technikraumname, Technikraumbounds und Technikraumstart in Gebaeude-Workflows ab Workflow 03. "
            "Wenn der Nutzer 'relativ', 'Verschiebungsvektor', 'Vektor' oder x/y/z-Werte fuer eine Verschiebung nennt, ist damit geometry.move.params.vector oder bim.move.params.vector gemeint; frage dann nicht nach absoluter Zielkoordinate. "
            "Wenn vorhandene Objekte selektiert und bearbeitet werden, frage nicht nach Erzeugungsdaten wie rectangleWidthMm, rectangleHeightMm oder extrudeHeightMm; diese sind nur fuer neue Geometrie noetig. "
            "Wenn der Nutzer sagt, dass eine Angabe nicht gebraucht wird, pruefe aktiv den aktuellen Workflow und entferne unreferenzierte requiredSlots/missing Felder statt dieselbe Rueckfrage zu wiederholen. "
            "Nutze fuer ausfuehrbare Werkzeugschritte workflow.steps[].tool und workflow.steps[].paramsTemplate nach effectiveTools[].inputSchema/apiPost. Fuer Batch-Datenfluesse sind zusaetzlich die Qt-internen Felder autoPointHandlesFromBatch, createdPointHandleIndexes, autoCreatedGeometryHandlesFromBatch, createdGeometryHandleIndexes, autoPointsFromLastQuery, queriedPointIndexes, autoPolylineHandlesFromBatch, createdPolylineHandleIndexes, autoPolylineHandlesFromLastQuery und autoBimHandlesFromLastQuery erlaubt; sie werden vor dem BRX-Aufruf aufgeloest. "
            "Wenn der Nutzer verlangt, Geometrie in einem bestimmten Layer zu zeichnen, muss der geometry.create Schritt den Parameter paramsTemplate.layer exakt mit diesem Layernamen enthalten; layers.create allein setzt den Ziellayer fuer folgende Geometrie nicht automatisch. "
            "Wenn ein Schritt selection.set verwendet und der naechste Schritt auf dieser Auswahl arbeiten soll, muss der naechste Schritt trotzdem explizit paramsTemplate.selector={\"scope\":\"selection\"} oder target=\"selection\" enthalten. "
            "Batch-Ausfuehrungen modellierst du in workflow.executionBatches mit mode=sequential, stopOnFailure=true und steps[].tool/paramsTemplate. "
            "Spiegele dieselben ausfuehrbaren Schritte zusaetzlich in der flachen workflow.steps Liste, sobald du workflow_update erzeugst. "
            "Schreibe constructionStrategy als JSON-Array mit einem kurzen String pro Strategiepunkt; keine eingebetteten '\\n', keine nummerierte Liste in einem einzelnen String. "
            "Formeln speicherst du als workflow.derivedValues mit name, expression, dependsOn, unit und example; Qt fuehrt keine Formeln aus, daher muessen validationExamples konkrete Beispielwerte enthalten. "
            "Wenn ein Schritt unmittelbar zuvor mit geometry.create ein Rechteck erzeugt, verwende im naechsten rectangles.extrude selector={\"scope\":\"lastResult\",\"kind\":\"rectangle\"}, nicht scope=selection. "
            "Bei normalen mehrschrittigen Vorschlaegen kann ein direkt nachfolgendes bim.classify target=lastExtruded nutzen. "
            "Bei gespeicherten Workflows, die Schritt fuer Schritt mit Nutzerfeedback ausgefuehrt werden, bevorzuge nach dem Extrudieren einen expliziten Selector oder selection.set und danach bim.classify target=selection, weil jeder Schritt einzeln per BRX validiert wird. "
            "Wenn ein Workflow BricsCAD-Geometrie auswerten oder tabellarisch dokumentieren soll, nutze geometry.query, selection.describe oder entity.describe mit include=[\"metrics\",\"geometry\"] und einem sinnvollen positiven limit >= 1; nutze niemals limit=0. Fuer BIM Door/Window/Wall Mutationen loese Zielobjekte erst read-only auf und vergleiche danach passende effectiveTools, bevor du einen validierbaren Toolpfad vorschlaegst. Fuer klassifizierte BIM-Objekte und Properties nutze bim.objects.query mit include=[\"core\",\"geometry\"] beziehungsweise gezielt zusaetzlich \"properties\". "
            "Beschreibe im Workflow, dass Qt die objects-Ergebnisse als Markdown-Tabelle mit Handle, Typ, Art, Form, Layer, Bounds und Metriken im Chatfenster anzeigen kann. "
            "Nutze keine Felder toolName/arguments in constructionStrategy fuer Werkzeugaufrufe."},
        {"responseContract", QJsonObject{
            {"schema", "barebone.workflow.training.response.v1"},
            {"allowedTypes", QJsonArray{"ask_user", "slot_update", "workflow_draft", "workflow_update", "message"}},
            {"askUserShape", QJsonObject{
                {"schema", "barebone.workflow.training.response.v1"},
                {"type", "ask_user"},
                {"message", "Eine konkrete fachliche Rueckfrage als vollstaendiger Satz; niemals nur 'kurze Rueckfrage' schreiben"},
                {"workflowSeed", QJsonObject{
                    {"id", "abgeleitete_snake_case_id"},
                    {"title", "abgeleiteter lesbarer Titel"},
                    {"requiredSlots", QJsonArray{"optional: slotnamen oder slotobjekte, keine steps"}},
                }},
                {"missing", QJsonArray{"konkreter_slot_oder_fachliche_angabe"}},
                {"questions", QJsonArray{"konkrete fachliche Frage 1", "konkrete fachliche Frage 2"}},
                {"forbiddenInAskUser", QJsonArray{"workflow.steps", "workflow.validationExamples", "lange constructionStrategy", "vollstaendige Workflow-Entwuerfe"}},
            }},
            {"slotUpdateShape", QJsonObject{
                {"schema", "barebone.workflow.training.response.v1"},
                {"type", "slot_update"},
                {"message", "Kurze Bestaetigung erkannter Angaben"},
                {"slots", QJsonObject{
                    {"wallThicknessMm", 240},
                    {"wallHeightMm", 3000},
                    {"roomAreaM2", 12},
                }},
                {"next", "ask_user oder workflow_update"},
            }},
            {"workflowDraftShape", QJsonObject{
                {"schema", "barebone.workflow.training.response.v1"},
                {"type", "workflow_draft"},
                {"message", "Kurze lesbare Zusammenfassung des Entwurfs"},
                {"workflowDraft", QJsonObject{
                    {"schema", "barebone.agent.workflow.v1"},
                    {"id", "abgeleitete_snake_case_id"},
                    {"title", "Lesbarer Workflow-Titel"},
                    {"description", "Kurze fachliche Beschreibung"},
                    {"triggerExamples", QJsonArray{"typischer Nutzerprompt"}},
                    {"requiredSlots", QJsonArray{"slotName oder Slotobjekt"}},
                    {"optionalSlots", QJsonObject{}},
                    {"knownSlotValues", QJsonObject{}},
                    {"derivedValues", QJsonArray{
                        QJsonObject{
                            {"name", "roomLengthMm"},
                            {"description", "Berechneter Beispielwert fuer die Raumlaenge"},
                            {"unit", "mm"},
                            {"expression", "sqrt(roomAreaM2 * aspectRatio) * 1000"},
                            {"dependsOn", QJsonArray{"roomAreaM2", "aspectRatio"}},
                            {"example", 5000},
                        },
                    }},
                    {"executionBatches", QJsonArray{
                        QJsonObject{
                            {"id", "create_wall_profiles"},
                            {"title", "Wandprofile erzeugen"},
                            {"description", "Sequenzielle Batch-Ausfuehrung; Qt/BRX fuehrt spaeter Schritt fuer Schritt aus."},
                            {"mode", "sequential"},
                            {"stopOnFailure", true},
                            {"steps", QJsonArray{
                                QJsonObject{
                                    {"id", "step_id"},
                                    {"title", "kurzer Schrittname"},
                                    {"tool", "toolname aus effectiveTools[].name"},
                                    {"paramsTemplate", QJsonObject{}},
                                    {"requiresSlots", QJsonArray{}},
                                },
                            }},
                        },
                    }},
                    {"constructionStrategy", QJsonArray{"fachliche Strategie und Planungsannahmen"}},
                    {"authoringNotes", QJsonObject{
                        {"sourcePromptSummary", "Kurzfassung des Nutzerprompts"},
                        {"assumptions", QJsonArray{"Annahme 1"}},
                        {"openQuestions", QJsonArray{"Noch zu klaerende Frage"}},
                    }},
                }},
                {"missing", QJsonArray{"optional: noch offene Pflichtangaben"}},
                {"questions", QJsonArray{"konkrete naechste Frage oder leer, wenn der Nutzer speichern kann"}},
                {"next", "ask_user, weiterer workflow_draft oder workflow_update nach Nutzerbestaetigung"},
            }},
            {"workflowUpdateShape", QJsonObject{
                {"schema", "barebone.workflow.training.response.v1"},
                {"type", "workflow_update"},
                {"operation", "create_or_update"},
                {"message", "Ich speichere/aktualisiere den Workflow ..."},
                {"workflow", QJsonObject{
                    {"schema", "barebone.agent.workflow.v1"},
                    {"id", "snake_case_id"},
                    {"title", "Lesbarer Titel"},
                    {"description", "Kurze fachliche Beschreibung"},
                    {"triggerExamples", QJsonArray{}},
                    {"requiredSlots", QJsonArray{}},
                    {"optionalSlots", QJsonObject{}},
                    {"derivedValues", QJsonArray{
                        QJsonObject{
                            {"name", "berechneterWert"},
                            {"expression", "Formel als Text"},
                            {"dependsOn", QJsonArray{"slotA", "slotB"}},
                            {"unit", "mm"},
                            {"example", 1000},
                        },
                    }},
                    {"preferredTools", QJsonArray{}},
                    {"executionBatches", QJsonArray{
                        QJsonObject{
                            {"id", "batch_id"},
                            {"title", "Batch Titel"},
                            {"mode", "sequential"},
                            {"stopOnFailure", true},
                            {"steps", QJsonArray{}},
                        },
                    }},
                    {"steps", QJsonArray{
                        QJsonObject{
                            {"id", "step_id"},
                            {"title", "kurzer Schrittname"},
                            {"tool", "toolname aus effectiveTools[].name"},
                            {"paramsTemplate", QJsonObject{}},
                            {"requiresSlots", QJsonArray{}},
                        },
                    }},
                    {"constructionStrategy", QJsonArray{"kurze fachliche Strategie in Klartext"}},
                    {"authoringNotes", QJsonObject{
                        {"sourcePromptSummary", "Kurzfassung des Ursprungsprompts"},
                        {"assumptions", QJsonArray{}},
                        {"openQuestions", QJsonArray{}},
                    }},
                    {"validationExamples", QJsonArray{
                        QJsonObject{
                            {"title", "BRX Preflight Beispiel mit konkreten Parametern"},
                            {"actions", "array of {tool, params} ohne {{slot}} Platzhalter; tool muss aus effectiveTools[].name kommen"},
                        },
                    }},
                    {"forbidden", QJsonArray{}},
                }},
                {"followUp", "optionale naechste Frage nach dem Speichern"},
            }},
        }},
        {"workflowSchemaGuidance", QJsonObject{
            {"idPolicy", "id muss kurz, stabil und snake_case sein; Dateiname wird aus id abgeleitet."},
            {"notHardcoded", "Speichere Strategien, Slots, Defaults, Constraints und Beispiele; keine starren Wenn-Dann Prompt-Matches."},
            {"toolPolicy", "preferredTools und steps[].tool duerfen nur bekannte toolNames/effectiveTools[].name verwenden. paramsTemplate verwendet Parameter aus inputSchema.properties/apiPost.bodySchema; zusaetzlich sind die dokumentierten Qt-Runtime-Bindings autoPointHandlesFromBatch/createdPointHandleIndexes, autoCreatedGeometryHandlesFromBatch/createdGeometryHandleIndexes, autoPointsFromLastQuery/queriedPointIndexes, autoPolylineHandlesFromBatch/createdPolylineHandleIndexes, autoPolylineHandlesFromLastQuery und autoBimHandlesFromLastQuery erlaubt. Beispiele muessen konkrete, platzhalterfreie BRX-Aktionen enthalten."},
            {"editingPolicy", "Bei Bearbeitung immer den bestehenden Workflow erhalten und nur gezielt erweitern/veraendern."},
            {"slotPolicy", "requiredSlots ist eine Liste aus strings oder Objekten mit name/type/description und darf leer sein. Referenzierte Werte gehoeren als Slots hinein; fachlich zwingender Prozesskontext darf mit requiredForExecution=true ohne direkte Toolreferenz erhalten bleiben. Technikraumname, Bounds und Startpunkt sind in den Gebaeude-Workflows 03 bis 07 Pflichtkontext. optionalSlots ist immer ein Objekt nach slotName, kein Array."},
            {"draftPolicy", "Bei langen Erstprompts erst workflow_draft mit workflowDraft erzeugen. workflow_update nur nach Nutzerbestaetigung oder wenn der Nutzer explizit speichern/aktualisieren will."},
            {"calculationPolicy", "Formeln gehoeren in derivedValues als name/expression/dependsOn/unit/example. Schreibe konkrete Beispielwerte in knownSlotValues und validationExamples; Qt wertet expression nicht aus."},
            {"batchPolicy", "Komplexe Ablaufe gehoeren in executionBatches[].steps mit mode=sequential und stopOnFailure=true. Fuer workflow_update muss zusaetzlich workflow.steps die gleiche ausfuehrbare Sequenz flach enthalten."},
            {"validationPolicy", "workflow_update muss validationExamples[].actions mit konkreten, platzhalterfreien Beispielaktionen enthalten. Verkettete Beispiele duerfen lastResult nutzen, aber nicht eine leere selection voraussetzen. Fuer step-by-step Workflows nach einer Extrusion zuerst selection.set verwenden und danach bim.classify target=selection."},
            {"tableOutputPolicy", "Fuer allgemeine Geometrie-Auswertung als Tabelle nutze read-only Tools geometry.query, selection.describe oder entity.describe. Fuer BIM-Objekt- und Propertytabellen nutze bim.objects.query. limit muss >= 1 sein; Qt erzeugt aus dem Query-Ergebnis die Objekt- und bei include=properties zusaetzlich die Property-Markdowntabelle."},
            {"bimDataPolicy", "Bei BIM-Objektnamen wie Window 11 ist der Text kein Datenbank-Handle. Nutze bim.objects.query mit selector.scope=names und selector.names. Folgeschritte verwenden autoBimHandlesFromLastQuery=true, wenn das gewaehlte Tool diese Runtime-Bindung unterstuetzt; andernfalls nutze explizite selector.scope=handles aus der Abfrage. Properties werden nur gezielt mit include=properties nachgeladen."},
        }},
        {"knownToolNames", toolNames},
        {"effectiveTools", effectiveTools},
        {"existingWorkflows", existingWorkflows},
        {"trainingState", workflowTrainingState()},
        {"activeWorkflow", activeWorkflow},
        {"selectedWorkflow", selectedWorkflowSummary()},
    };
}

bool BricsCadPage::validateWorkflowStepForTraining(const QJsonObject& step, int index, QString& errorMessage, const QString& pathPrefix) const
{
    const QString prefix = pathPrefix.isEmpty()
        ? QStringLiteral("workflow.steps[%1]").arg(index)
        : QStringLiteral("%1[%2]").arg(pathPrefix).arg(index);
    if (step.contains(QStringLiteral("toolName")) || step.contains(QStringLiteral("arguments"))) {
        errorMessage = QStringLiteral("%1 nutzt alte Felder toolName/arguments. Nutze stattdessen tool und paramsTemplate.").arg(prefix);
        return false;
    }

    const QString tool = step.value("tool").toString().trimmed();
    if (tool.isEmpty()) {
        errorMessage = QStringLiteral("%1.tool fehlt").arg(prefix);
        return false;
    }

    QJsonObject definition;
    for (const QJsonValue& value : availableAgentTools()) {
        const QJsonObject item = value.toObject();
        if (item.value("name").toString() == tool) {
            definition = item;
            break;
        }
    }
    if (definition.isEmpty()) {
        errorMessage = QStringLiteral("%1.tool \"%2\" ist nicht in den BRX/effectiveTools enthalten").arg(prefix, tool);
        return false;
    }

    if (!step.value("paramsTemplate").isObject()) {
        errorMessage = QStringLiteral("%1.paramsTemplate muss ein Objekt sein").arg(prefix);
        return false;
    }
    const QJsonObject paramsTemplate = step.value("paramsTemplate").toObject();
    const QJsonObject inputSchema = definition.value("inputSchema").toObject();
    const QJsonObject properties = inputSchema.value("properties").toObject();
    if (properties.isEmpty()) {
        errorMessage = QStringLiteral("%1.tool \"%2\" hat kein pruefbares inputSchema").arg(prefix, tool);
        return false;
    }

    const auto runtimeBindingSchema = [&tool](const QString& key) -> QJsonObject {
        const bool booleanBinding = (tool == QStringLiteral("geometry.query")
                && (key == QStringLiteral("autoPointHandlesFromBatch")
                    || key == QStringLiteral("autoCreatedGeometryHandlesFromBatch")))
            || (tool == QStringLiteral("geometry.create") && key == QStringLiteral("autoPointsFromLastQuery"))
            || (tool == QStringLiteral("bim.selection.set")
                && key == QStringLiteral("autoBimHandlesFromLastQuery"))
            || ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
                && (key == QStringLiteral("autoPolylineHandlesFromBatch")
                    || key == QStringLiteral("autoPolylineHandlesFromLastQuery")));
        const bool roomBinding = key == QStringLiteral("autoRoomHandlesFromLastQuery")
            && (tool == QStringLiteral("measurement.bbox") || tool == QStringLiteral("measurement.area")
                || tool == QStringLiteral("annotations.createRoomDimensions") || tool == QStringLiteral("geometry.query"));
        if (booleanBinding || roomBinding) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
        }

        const bool indexBinding = (tool == QStringLiteral("geometry.query")
                && (key == QStringLiteral("createdPointHandleIndexes")
                    || key == QStringLiteral("createdGeometryHandleIndexes")))
            || (tool == QStringLiteral("geometry.create") && key == QStringLiteral("queriedPointIndexes"))
            || ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
                && key == QStringLiteral("createdPolylineHandleIndexes"));
        if (indexBinding) {
            return QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("number")},
                    {QStringLiteral("minimum"), 0},
                }},
            };
        }
        return {};
    };

    for (auto it = paramsTemplate.begin(); it != paramsTemplate.end(); ++it) {
        QJsonObject propertySchema = properties.value(it.key()).toObject();
        if (propertySchema.isEmpty()) {
            propertySchema = runtimeBindingSchema(it.key());
        }
        if (propertySchema.isEmpty()) {
            errorMessage = QStringLiteral("%1.paramsTemplate.%2 ist kein Parameter von %3").arg(prefix, it.key(), tool);
            return false;
        }
        if (!jsonContainsTemplatePlaceholder(it.value())) {
            QString schemaError;
            if (!validateSchemaValue(it.value(), propertySchema, prefix + ".paramsTemplate." + it.key(), schemaError)) {
                errorMessage = schemaError;
                return false;
            }
        }
    }

    const QJsonArray required = inputSchema.value("required").toArray();
    for (const QJsonValue& value : required) {
        const QString key = value.toString();
        const bool suppliedByRuntimeBimSelector = key == QStringLiteral("selector")
            && paramsTemplate.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
            && tool == QStringLiteral("bim.selection.set");
        if (!key.isEmpty() && !paramsTemplate.contains(key) && !suppliedByRuntimeBimSelector) {
            errorMessage = QStringLiteral("%1.paramsTemplate.%2 fehlt laut %3.inputSchema.required").arg(prefix, key, tool);
            return false;
        }
    }

    const QJsonArray oneOfRequired = definition.value("apiDoc").toObject()
        .value("post").toObject()
        .value("oneOfRequired").toArray();
    if (!oneOfRequired.isEmpty()) {
        QStringList alternatives;
        bool matched = false;
        for (const QJsonValue& groupValue : oneOfRequired) {
            QStringList group;
            if (groupValue.isArray()) {
                for (const QJsonValue& keyValue : groupValue.toArray()) {
                    if (!keyValue.toString().trimmed().isEmpty()) {
                        group << keyValue.toString().trimmed();
                    }
                }
            } else if (!groupValue.toString().trimmed().isEmpty()) {
                group << groupValue.toString().trimmed();
            }
            if (group.isEmpty()) {
                continue;
            }
            alternatives << group.join(QStringLiteral("+"));
            if (jsonObjectHasAnyKey(paramsTemplate, group)) {
                matched = true;
            }
        }
        if (!matched) {
            errorMessage = QStringLiteral("%1 braucht eine dieser Parametergruppen fuer %2: %3")
                .arg(prefix, tool, alternatives.join(QStringLiteral(" oder ")));
            return false;
        }
    }

    return true;
}

bool BricsCadPage::validateWorkflowForTraining(QJsonObject& workflow, QString& errorMessage) const
{
    if (workflow.isEmpty()) {
        errorMessage = QStringLiteral("workflow Objekt fehlt");
        return false;
    }

    if (workflow.value("schema").toString().trimmed().isEmpty()) {
        workflow.insert("schema", QStringLiteral("barebone.agent.workflow.v1"));
    }
    if (workflow.value("schema").toString() != QStringLiteral("barebone.agent.workflow.v1")) {
        errorMessage = QStringLiteral("workflow.schema muss barebone.agent.workflow.v1 sein");
        return false;
    }

    QString id = workflowSlug(workflow.value("id").toString());
    if (id == QStringLiteral("workflow")) {
        id = workflowSlug(workflow.value("title").toString());
    }
    if (id == QStringLiteral("workflow")) {
        errorMessage = QStringLiteral("workflow.id oder workflow.title fehlt");
        return false;
    }
    workflow.insert("id", id);

    if (workflow.value("title").toString().trimmed().isEmpty()) {
        errorMessage = QStringLiteral("workflow.title fehlt");
        return false;
    }
    if (workflow.contains(QStringLiteral("description"))
        && !workflow.value("description").isString()) {
        errorMessage = QStringLiteral("workflow.description muss ein Text sein");
        return false;
    }
    if (workflow.contains(QStringLiteral("authoringNotes"))
        && !workflow.value("authoringNotes").isObject()) {
        errorMessage = QStringLiteral("workflow.authoringNotes muss ein Objekt sein");
        return false;
    }

    QJsonArray triggerExamples = workflow.value("triggerExamples").toArray();
    if (triggerExamples.isEmpty() && !m_lastAgentUserPrompt.trimmed().isEmpty()) {
        triggerExamples.append(m_lastAgentUserPrompt.trimmed());
        workflow.insert("triggerExamples", triggerExamples);
    }
    if (triggerExamples.isEmpty()) {
        errorMessage = QStringLiteral("workflow.triggerExamples braucht mindestens ein Beispiel");
        return false;
    }

    if (!workflow.value("requiredSlots").isArray()) {
        workflow.insert("requiredSlots", QJsonArray{});
    }
    workflow = workflowWithPrunedRequiredSlots(workflow);

    const QJsonArray requiredSlots = workflow.value("requiredSlots").toArray();
    QStringList knownSlots;
    for (int i = 0; i < requiredSlots.size(); ++i) {
        const QJsonValue value = requiredSlots.at(i);
        const QString name = value.isObject()
            ? value.toObject().value("name").toString().trimmed()
            : value.toString().trimmed();
        if (name.isEmpty()) {
            errorMessage = QStringLiteral("workflow.requiredSlots[%1] braucht einen Namen").arg(i);
            return false;
        }
        if (!knownSlots.contains(name)) {
            knownSlots << name;
        }
    }

    workflow.insert(QStringLiteral("optionalSlots"),
        normalizedWorkflowOptionalSlots(workflow.value(QStringLiteral("optionalSlots"))));
    if (!workflow.value("optionalSlots").isObject()) {
        errorMessage = QStringLiteral("workflow.optionalSlots muss ein Objekt nach Slotname sein, kein Array");
        return false;
    }
    const QJsonObject optionalSlots = workflow.value("optionalSlots").toObject();
    for (auto it = optionalSlots.begin(); it != optionalSlots.end(); ++it) {
        if (!knownSlots.contains(it.key())) {
            knownSlots << it.key();
        }
    }

    if (!workflow.contains(QStringLiteral("derivedValues"))) {
        workflow.insert(QStringLiteral("derivedValues"), QJsonArray{});
    } else if (!workflow.value("derivedValues").isArray()) {
        errorMessage = QStringLiteral("workflow.derivedValues muss ein Array sein");
        return false;
    }

    QJsonArray normalizedDerivedValues;
    const QJsonArray derivedValues = workflow.value("derivedValues").toArray();
    QStringList derivedNames;
    for (int i = 0; i < derivedValues.size(); ++i) {
        if (!derivedValues.at(i).isObject()) {
            errorMessage = QStringLiteral("workflow.derivedValues[%1] muss ein Objekt sein").arg(i);
            return false;
        }
        QJsonObject derived = derivedValues.at(i).toObject();
        const QString name = derived.value("name").toString().trimmed();
        if (name.isEmpty()) {
            errorMessage = QStringLiteral("workflow.derivedValues[%1].name fehlt").arg(i);
            return false;
        }
        if (derivedNames.contains(name)) {
            errorMessage = QStringLiteral("workflow.derivedValues enthaelt den Namen \"%1\" mehrfach").arg(name);
            return false;
        }
        derivedNames << name;
        if (!knownSlots.contains(name)) {
            knownSlots << name;
        }
        if (derived.contains(QStringLiteral("expression")) && !derived.value("expression").isString()) {
            errorMessage = QStringLiteral("workflow.derivedValues[%1].expression muss ein Text sein").arg(i);
            return false;
        }
        if (derived.contains(QStringLiteral("unit")) && !derived.value("unit").isString()) {
            errorMessage = QStringLiteral("workflow.derivedValues[%1].unit muss ein Text sein").arg(i);
            return false;
        }
        if (derived.contains(QStringLiteral("description")) && !derived.value("description").isString()) {
            errorMessage = QStringLiteral("workflow.derivedValues[%1].description muss ein Text sein").arg(i);
            return false;
        }
        if (!derived.value("dependsOn").isArray()) {
            derived.insert(QStringLiteral("dependsOn"), QJsonArray{});
        }
        normalizedDerivedValues.append(derived);
    }

    for (int i = 0; i < normalizedDerivedValues.size(); ++i) {
        const QJsonObject derived = normalizedDerivedValues.at(i).toObject();
        const QString name = derived.value("name").toString();
        const QJsonArray dependsOn = derived.value("dependsOn").toArray();
        for (const QJsonValue& dependencyValue : dependsOn) {
            const QString dependency = workflowSlotNameFromValue(dependencyValue);
            if (dependency.isEmpty()) {
                errorMessage = QStringLiteral("workflow.derivedValues[%1].dependsOn enthaelt einen leeren Wert").arg(i);
                return false;
            }
            if (dependency == name) {
                errorMessage = QStringLiteral("workflow.derivedValues[%1] darf nicht von sich selbst abhaengen").arg(i);
                return false;
            }
            if (!knownSlots.contains(dependency)) {
                errorMessage = QStringLiteral("workflow.derivedValues[%1].dependsOn nutzt unbekannten Slot \"%2\"").arg(i).arg(dependency);
                return false;
            }
        }
    }
    workflow.insert(QStringLiteral("derivedValues"), normalizedDerivedValues);

    const QJsonArray preferredTools = workflow.value("preferredTools").toArray();
    if (preferredTools.isEmpty()) {
        errorMessage = QStringLiteral("workflow.preferredTools braucht mindestens ein Tool");
        return false;
    }
    QStringList availableToolNames;
    for (const QJsonValue& value : availableAgentTools()) {
        const QString name = value.toObject().value("name").toString();
        if (!name.isEmpty()) {
            availableToolNames << name;
        }
    }
    if (availableToolNames.isEmpty()) {
        errorMessage = QStringLiteral("BRX Capabilities/effectiveTools fehlen; Trainingsworkflow kann nicht toolgenau validiert werden");
        return false;
    }
    for (const QJsonValue& value : preferredTools) {
        const QString tool = value.toString().trimmed();
        if (tool.isEmpty() || !availableToolNames.contains(tool)) {
            errorMessage = QStringLiteral("workflow.preferredTools enthaelt unbekanntes Tool \"%1\"").arg(tool.isEmpty() ? QStringLiteral("<leer>") : tool);
            return false;
        }
    }

    QJsonArray flattenedBatchSteps;
    if (workflow.contains(QStringLiteral("executionBatches"))) {
        if (!workflow.value("executionBatches").isArray()) {
            errorMessage = QStringLiteral("workflow.executionBatches muss ein Array sein");
            return false;
        }

        QJsonArray normalizedBatches;
        const QJsonArray batches = workflow.value("executionBatches").toArray();
        for (int batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
            if (!batches.at(batchIndex).isObject()) {
                errorMessage = QStringLiteral("workflow.executionBatches[%1] muss ein Objekt sein").arg(batchIndex);
                return false;
            }
            QJsonObject batch = batches.at(batchIndex).toObject();
            QString batchId = workflowSlug(batch.value("id").toString());
            if (batchId == QStringLiteral("workflow")) {
                batchId = workflowSlug(batch.value("title").toString());
            }
            if (batchId == QStringLiteral("workflow")) {
                batchId = QStringLiteral("batch_%1").arg(batchIndex + 1);
            }
            batch.insert(QStringLiteral("id"), batchId);
            if (batch.value("title").toString().trimmed().isEmpty()) {
                batch.insert(QStringLiteral("title"), batchId);
            }

            QString mode = batch.value("mode").toString().trimmed();
            if (mode.isEmpty()) {
                mode = QStringLiteral("sequential");
                batch.insert(QStringLiteral("mode"), mode);
            }
            if (mode != QStringLiteral("sequential")) {
                errorMessage = QStringLiteral("workflow.executionBatches[%1].mode unterstuetzt aktuell nur \"sequential\"").arg(batchIndex);
                return false;
            }
            if (!batch.contains(QStringLiteral("stopOnFailure"))) {
                batch.insert(QStringLiteral("stopOnFailure"), true);
            }
            if (!batch.value("stopOnFailure").isBool()) {
                errorMessage = QStringLiteral("workflow.executionBatches[%1].stopOnFailure muss true/false sein").arg(batchIndex);
                return false;
            }
            if (!batch.value("steps").isArray()) {
                errorMessage = QStringLiteral("workflow.executionBatches[%1].steps muss ein Array sein").arg(batchIndex);
                return false;
            }
            const QJsonArray batchSteps = batch.value("steps").toArray();
            if (batchSteps.isEmpty()) {
                errorMessage = QStringLiteral("workflow.executionBatches[%1].steps braucht mindestens einen Werkzeugschritt").arg(batchIndex);
                return false;
            }
            QJsonArray normalizedBatchSteps;
            for (int stepIndex = 0; stepIndex < batchSteps.size(); ++stepIndex) {
                const QJsonObject step = normalizedWorkflowStepForExecution(batchSteps.at(stepIndex).toObject());
                const QString stepPrefix = QStringLiteral("workflow.executionBatches[%1].steps").arg(batchIndex);
                if (!validateWorkflowStepForTraining(step, stepIndex, errorMessage, stepPrefix)) {
                    return false;
                }
                QStringList placeholders;
                collectTemplatePlaceholders(step.value("paramsTemplate"), placeholders);
                for (const QString& placeholder : placeholders) {
                    if (!knownSlots.contains(placeholder)) {
                        errorMessage = QStringLiteral("%1[%2] nutzt unbekannten Platzhalter {{%3}}. Fuege ihn zu requiredSlots, optionalSlots oder derivedValues hinzu.")
                            .arg(stepPrefix)
                            .arg(stepIndex)
                            .arg(placeholder);
                        return false;
                    }
                }
                flattenedBatchSteps.append(step);
                normalizedBatchSteps.append(step);
            }
            batch.insert(QStringLiteral("steps"), normalizedBatchSteps);
            normalizedBatches.append(batch);
        }
        workflow.insert(QStringLiteral("executionBatches"), normalizedBatches);
    }

    QJsonArray steps = workflow.value("steps").toArray();
    if (steps.isEmpty() && !flattenedBatchSteps.isEmpty()) {
        steps = flattenedBatchSteps;
        workflow.insert(QStringLiteral("steps"), steps);
    }
    if (steps.isEmpty()) {
        if (!workflow.value("constructionStrategy").toArray().isEmpty()) {
            errorMessage = QStringLiteral("workflow.steps fehlt. Werkzeugaufrufe gehoeren in steps[].tool und steps[].paramsTemplate; constructionStrategy ist nur Klartextstrategie.");
        } else {
            errorMessage = QStringLiteral("workflow.steps braucht mindestens einen Werkzeugschritt");
        }
        return false;
    }

    QJsonArray normalizedSteps;
    for (int i = 0; i < steps.size(); ++i) {
        const QJsonObject step = normalizedWorkflowStepForExecution(steps.at(i).toObject());
        if (!validateWorkflowStepForTraining(step, i, errorMessage)) {
            return false;
        }

        QStringList placeholders;
        collectTemplatePlaceholders(step.value("paramsTemplate"), placeholders);
        for (const QString& placeholder : placeholders) {
            if (!knownSlots.contains(placeholder)) {
                errorMessage = QStringLiteral("workflow.steps[%1] nutzt unbekannten Platzhalter {{%2}}. Fuege ihn zu requiredSlots, optionalSlots oder derivedValues hinzu.")
                    .arg(i)
                    .arg(placeholder);
                return false;
            }
        }
        normalizedSteps.append(step);
    }
    workflow.insert(QStringLiteral("steps"), normalizedSteps);

    if (!workflow.value("constructionStrategy").isArray()) {
        workflow.insert("constructionStrategy", QJsonArray{});
    }
    if (!workflow.value("forbidden").isArray()) {
        workflow.insert("forbidden", QJsonArray{});
    }
    if (!workflow.value("validationExamples").isArray()) {
        workflow.insert("validationExamples", QJsonArray{});
    }

    return true;
}

bool BricsCadPage::saveWorkflowFromTraining(const QJsonObject& workflow, QString* savedPath, QString* errorMessage) const
{
    QJsonObject normalized = repairedMojibakeJsonValue(workflow).toObject();
    normalized = generalWorkflowWithUniqueId(normalized, bricsCadWorkflowsDirectoryPath());
    normalized.insert(QStringLiteral("schema"), QStringLiteral("barebone.bricscad.workflow.v1"));
    normalized.insert(QStringLiteral("kind"), QStringLiteral("bricscad"));
    const QString id = workflowSlug(normalized.value(QStringLiteral("id")).toString());
    if (id.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("BricsCAD-Workflow braucht eine stabile id.");
        }
        return false;
    }
    normalized.insert(QStringLiteral("id"), id);
    QDir directory(bricsCadWorkflowsDirectoryPath());
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("BricsCAD-Workflow-Verzeichnis konnte nicht erstellt werden: %1").arg(directory.absolutePath());
        }
        return false;
    }
    QFile file(directory.filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("BricsCAD-Workflow konnte nicht gespeichert werden: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(normalized).toJson(QJsonDocument::Indented));
    file.write("\n");
    if (savedPath) {
        *savedPath = QFileInfo(file).absoluteFilePath();
    }
    return true;
}

void BricsCadPage::finishWorkflowTrainingSave(const QJsonObject& reply, const QJsonObject& workflow)
{
    clearWorkflowTrainingSavePrompt();
    QString savedPath;
    QString errorMessage;
    if (!saveWorkflowFromTraining(workflow, &savedPath, &errorMessage)) {
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht gespeichert werden: %1").arg(errorMessage));
        return;
    }

    m_trainingValidationRetries = 0;
    m_trainingWorkflowContext = workflow;
    const QString savedWorkflowId = workflow.value(QStringLiteral("id")).toString();
    if (!savedWorkflowId.isEmpty()
        && (m_selectedWorkflow.isEmpty()
            || workflowSlug(m_selectedWorkflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId)) == workflowSlug(savedWorkflowId))) {
        m_selectedWorkflowId = savedWorkflowId;
        m_selectedWorkflow = workflow;
        m_selectedWorkflowSlotValues = workflow.value(QStringLiteral("knownSlotValues")).toObject();
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [workflowMap = m_selectedWorkflow.toVariantMap()](AiWebBridge* target) {
                Q_EMIT target->selectedWorkflowChanged(workflowMap);
            });
        }
    }
    m_trainingMissing = {};
    m_trainingPhase = QStringLiteral("saved");
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    m_trainingFinalSavePending = false;
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    QStringList lines;
    const QString message = reply.value("message").toString().trimmed();
    lines << (message.isEmpty()
        ? QStringLiteral("Workflow wurde gespeichert.")
        : message);
    lines << QStringLiteral("Gespeichert: %1").arg(savedPath);
    const QString followUp = reply.value("followUp").toString().trimmed();
    if (!followUp.isEmpty()) {
        lines << followUp;
    }
    appendAgentChat("AI", lines.join('\n'));
    emitWorkflowListToWeb();
}

bool BricsCadPage::retryWorkflowTrainingAfterValidationFailure(
    const QString& rejectedContent,
    const QJsonObject& rejectedObject,
    const QString& errorMessage)
{
    if (m_trainingValidationRetries >= kMaxAgentValidationRetries) {
        appendBridgeLog(QString("Workflow Training Loop: abgebrochen nach %1 Versuchen: %2")
            .arg(kMaxAgentValidationRetries)
            .arg(errorMessage.left(500)));
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht automatisch korrigiert werden: %1").arg(errorMessage));
        return false;
    }

    ++m_trainingValidationRetries;
    appendBridgeLog(QString("Workflow Training Loop %1/%2: %3")
        .arg(m_trainingValidationRetries)
        .arg(kMaxAgentValidationRetries)
        .arg(errorMessage.left(500)));

    const QString rejectedJson = rejectedObject.isEmpty()
        ? rejectedContent.left(6000)
        : QString::fromUtf8(QJsonDocument(rejectedObject).toJson(QJsonDocument::Compact)).left(6000);
    const QString rejectedSignature = rejectedJson.simplified().left(1200);
    const bool repeatedResponse = m_trainingRejectedResponseSignatures.contains(rejectedSignature);
    if (!rejectedSignature.isEmpty()) {
        m_trainingRejectedResponseSignatures << rejectedSignature;
        while (m_trainingRejectedResponseSignatures.size() > 8) {
            m_trainingRejectedResponseSignatures.removeFirst();
        }
    }
    const QString trainingStateJson = QString::fromUtf8(QJsonDocument(workflowTrainingState()).toJson(QJsonDocument::Compact)).left(8000);
    sendWorkflowTrainingPrompt(QString(
        "Korrigiere deine letzte Workflow-Training-Antwort intern. "
        "Der Nutzer sieht diese Validierung nicht. Fehler: %1\n"
        "Letzte Nutzeranweisung: %2\n"
        "Aktueller Trainingszustand: %3\n"
        "Vorherige Antwort: %4\n"
        "%5"
        "Antworte ausschliesslich mit barebone.workflow.training.response.v1 JSON. "
        "Wiederhole nicht dieselbe Antwort, dieselben missing-Felder oder denselben unveraenderten Entwurf. Jeder Retry muss eine erkennbare Korrektur enthalten. "
        "Wenn der Nutzer sagt, dass keine Angaben noetig sind, pruefe anhand activeWorkflow.steps/executionBatches, ob diese requiredSlots wirklich in paramsTemplate/condition/derivedValues referenziert werden; wenn nicht, entferne sie. "
        "Wenn der Nutzer die Auswahl an die AI delegiert hat (z.B. 'erstelle selber', 'sinnvolle Farben', 'nutze TGA Themen'), frage nicht erneut nach Namen/Farben/Listen. Erzeuge plausible Defaultwerte selbst und speichere sie als knownSlotValues, derivedValues oder paramsTemplate-Konstanten. "
        "Bei TGA-Layern ohne konkrete Vorgabe erzeuge eine plausible Liste aus TGA-Gruppen und ACI-Farben; bei 10 Layern z.B. Sanitaer, Heizung, Lueftung, Elektro, Kaelte, Sprinkler, Gas, MSR, Brandschutz, Daemmung. "
        "Wenn Daten fehlen, nutze ask_user mit kurzer message, missing und questions, aber ohne workflow.steps und ohne validationExamples. "
        "Wenn erst ein strukturierter Entwurf sinnvoll ist, nutze workflow_draft mit workflowDraft. "
        "Wenn nur neue Angaben erkannt wurden, nutze slot_update mit slots. "
        "Wenn der Workflow speicherbar ist, nutze workflow_update mit steps[].tool, steps[].paramsTemplate und validationExamples[].actions.")
        .arg(errorMessage,
             m_lastAgentUserPrompt.left(1200),
             trainingStateJson,
             rejectedJson,
             repeatedResponse
                 ? QStringLiteral("Achtung: Diese Antwort wurde bereits inhaltlich wiederholt. Gib jetzt zwingend eine andere Korrektur aus und entferne unreferenzierte Pflichtslots statt sie erneut zu melden.\n")
                 : QString()),
        true);
    return true;
}

bool BricsCadPage::retryGeneralWorkflowSaveAfterValidationFailure(
    const QJsonObject& saveContext,
    const QString& rejectedContent,
    const QString& errorMessage)
{
    if (m_generalWorkflowSaveRetries >= kMaxAgentValidationRetries) {
        appendBridgeLog(QString("General Workflow Save Loop: abgebrochen nach %1 Versuchen: %2")
            .arg(kMaxAgentValidationRetries)
            .arg(errorMessage.left(500)));
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht vorbereitet werden: %1").arg(errorMessage));
        m_generalWorkflowSaveRetries = 0;
        m_generalWorkflowSaveRejectedSignatures.clear();
        return false;
    }

    const QString rejectedSample = rejectedContent.left(6000);
    const QString rejectedSignature = rejectedSample.simplified().left(1600);
    const bool repeatedResponse = !rejectedSignature.isEmpty()
        && m_generalWorkflowSaveRejectedSignatures.contains(rejectedSignature);
    if (!rejectedSignature.isEmpty()) {
        m_generalWorkflowSaveRejectedSignatures << rejectedSignature;
        while (m_generalWorkflowSaveRejectedSignatures.size() > 8) {
            m_generalWorkflowSaveRejectedSignatures.removeFirst();
        }
    }

    ++m_generalWorkflowSaveRetries;
    appendBridgeLog(QString("General Workflow Save Loop %1/%2: %3")
        .arg(m_generalWorkflowSaveRetries)
        .arg(kMaxAgentValidationRetries)
        .arg(errorMessage.left(500)));

    const QString repeatedInstruction = repeatedResponse
        ? QStringLiteral("Deine letzte Antwort war identisch oder zu aehnlich und wurde bereits abgelehnt. Aendere die Struktur sichtbar: konkreter thematischer Titel, gueltige blocks[], keine Markdown-Fences, keine generische id.\n")
        : QString();
    const QString retryInstruction = QStringLiteral(
        "Korrigiere deine letzte Antwort fuer Chat-Workflow-Speichern. "
        "Der Nutzer sieht diese Validierung nicht. Fehler: %1\n"
        "Vorherige abgelehnte Antwort gekuerzt: %2\n"
        "%3"
        "Antworte ausschliesslich mit genau einem JSON-Objekt nach schema barebone.general.workflow.save.response.v1. "
        "Keine Markdown-Antwort, kein Codeblock, keine Erklaerung ausserhalb von JSON. "
        "Pflichtfelder: schema, id, title, description, blocks[]. Jeder block braucht id, title und text. "
        "Nutze blocks[] als grobe Absatzstruktur und schreibe die fachliche Tiefe in blocks[].text. "
        "title muss ein kurzer, passender Anzeigename sein, den du aus compressedTitleContext.summary, selectedMessageText und dem fachlichen Schwerpunkt ableitest. "
        "Uebernimm sessionTitle nicht automatisch als title. "
        "id muss daraus als stabiler snake_case Dateiname abgeleitet werden. "
        "title und id duerfen nicht Workflow, Neuer Workflow, Chat Workflow, General Workflow, Workflow speichern oder aehnlich generisch heissen. "
        "Chat-Speichern legt immer einen neuen Workflow an; uebernimm keine vorhandene id und ueberschreibe keinen gespeicherten Workflow. "
        "Keine BricsCAD-Workflow-Felder wie steps, executionBatches, validationExamples oder tools. "
        "Korrigiere bei Formatierungsfehlern nur die Formatierung; erhalte alle fachlichen Details, Tabellenwerte, Beispiele und Nutzerkorrekturen vollstaendig. "
        "Tabellen muessen als tables[] mit columns/rows oder als gueltige Markdown-Pipe-Tabelle mit |---|---|-Trennzeile ausgegeben werden. "
        "Listen muessen echte Markdown-Listen mit je einem Punkt pro Zeile sein. "
        "Nutze keine HTML-Tags wie <br>, keine tabulatorgetrennten Klartexttabellen und kein loses Sprachlabel 'text' vor Formeln.")
        .arg(errorMessage, rejectedSample, repeatedInstruction);

    saveGeneralWorkflowFromTraining(
        saveContext.value(QStringLiteral("userInstruction")).toString(),
        m_generalWorkflowSaveRetries,
        retryInstruction,
        rejectedSample,
        saveContext.value(QStringLiteral("selectedMessageText")).toString(),
        saveContext.value(QStringLiteral("sessionTitle")).toString());
    return true;
}

void BricsCadPage::startWorkflowTrainingSaveReview(
    const QString& rejectedContent,
    const QJsonObject& reply,
    const QJsonObject& workflow)
{
    QJsonObject reviewWorkflow = workflow;
    if (!m_trainingSlotValues.isEmpty()) {
        reviewWorkflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }
    if (!reviewWorkflow.isEmpty()) {
        mergeWorkflowTrainingContext(reviewWorkflow);
    }

    m_trainingSaveReviewPending = true;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent = rejectedContent;
    m_trainingPendingReviewReply = reply;
    m_trainingPendingReviewWorkflow = reviewWorkflow;
    m_trainingPhase = QStringLiteral("reviewing");

    clearWorkflowTrainingPrompts();
    appendBridgeLog("Workflow Training Review: Vor dem Speichern wird der Entwurf punktweise geprueft");
    sendWorkflowTrainingPrompt(QStringLiteral(
        "Bevor dieser Workflow gespeichert wird, pruefe den aktiven Workflow noch einmal fachlich und technisch. "
        "Denke die Zusammenfassung punktweise durch und antworte ausschliesslich mit type=workflow_draft. "
        "Gehe einzeln auf diese Punkte ein: Titel, Beschreibung, Pflichtangaben, bekannte Beispielwerte, Formeln, Batch-Ausfuehrungen, Tool-Parameter, Validierungsbeispiel und Risiken. "
        "Formuliere danach konkrete questions, ob diese Punkte korrekt sind oder ob noch etwas beruecksichtigt werden muss. "
        "Nutze workflowDraft mit dem vollstaendigen aktuellen Workflow-Entwurf. "
        "Antworte nicht mit workflow_update, nicht speichern und keine BRX-Aktion vorschlagen, bis der Nutzer die Review ausdruecklich bestaetigt."),
        true);
}

void BricsCadPage::confirmWorkflowTrainingSaveReview()
{
    if (!m_trainingMode || !m_trainingSaveReviewPending) {
        clearWorkflowTrainingPrompts();
        appendBridgeLog("Workflow Training Review: Speichern ignoriert, weil keine offene Review vorhanden ist");
        return;
    }

    appendBridgeLog("Workflow Training Review: Nutzer hat die Vor-Speicher-Review bestaetigt");
    clearWorkflowTrainingPrompts();
    QJsonObject workflow = m_trainingPendingReviewWorkflow.isEmpty()
        ? m_trainingWorkflowContext
        : m_trainingPendingReviewWorkflow;
    if (!m_trainingSlotValues.isEmpty()) {
        workflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }

    QJsonObject reply = m_trainingPendingReviewReply;
    if (reply.isEmpty()) {
        reply = QJsonObject{
            {"schema", "barebone.workflow.training.response.v1"},
            {"type", "workflow_update"},
            {"message", "Workflow nach Review bestaetigt und gespeichert."},
        };
    }
    if (!reply.contains(QStringLiteral("workflow"))) {
        reply.insert(QStringLiteral("workflow"), workflow);
    }

    const QString content = m_trainingPendingReviewContent.isEmpty()
        ? QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))
        : m_trainingPendingReviewContent;
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = true;
    m_trainingPhase = QStringLiteral("validating");
    validateWorkflowWithBrxAndSave(content, reply, workflow);
}

void BricsCadPage::confirmWorkflowTrainingFinalSave()
{
    if (!isChatWorkspace() || !m_trainingFinalSavePending || m_trainingFinalSaveWorkflow.isEmpty()) {
        clearWorkflowTrainingPrompts();
        appendBridgeLog("General Workflow Final Save: Speichern ignoriert, weil kein Chat-Workflow-Entwurf bereitsteht");
        return;
    }

    clearWorkflowTrainingPrompts();
    QString savedPath;
    QString errorMessage;
    if (!saveGeneralWorkflowFinalDraft(&savedPath, &errorMessage)) {
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht gespeichert werden: %1").arg(errorMessage));
        emitWorkflowTrainingFinalSavePrompt();
        return;
    }

    m_trainingFinalSavePending = false;
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    m_trainingPhase = QStringLiteral("saved");
    emitWorkflowListToWeb();
    if (m_agentBridge && !m_selectedWorkflow.isEmpty()) {
        QVariantMap payload = m_selectedWorkflow.toVariantMap();
        payload.insert(QStringLiteral("selected"), true);
        emitToWebAsync(m_agentBridge, [payload](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(payload);
        });
    }
    appendBridgeLog(QString("General Workflow Final Save: gespeichert: %1").arg(savedPath));
    appendAgentChat("Barebone-Qt", QString("Workflow gespeichert: %1").arg(m_selectedWorkflow.value(QStringLiteral("title")).toString(m_selectedWorkflowId)));
}
void BricsCadPage::emitWorkflowTrainingFinalSavePrompt()
{
    if (!m_agentBridge || !m_trainingFinalSavePending) {
        return;
    }

    QString title = repairMojibakeText(m_trainingFinalSaveWorkflow.value("title").toString()).trimmed();
    if (isChatWorkspace()) {
        const QString sessionTitle = titleCandidateFromSaveContext(m_lastGeneralWorkflowSaveContext);
        if (!sessionTitle.isEmpty()) {
            title = sessionTitle;
        }
    }
    const QJsonObject signatureObject{
        {"kind", isChatWorkspace() ? QStringLiteral("general_workflow_final_save") : QStringLiteral("training_final_save")},
        {"id", workflowSlug(m_trainingFinalSaveWorkflow.value("id").toString())},
        {"title", title},
        {"actions", m_trainingFinalSaveActions},
        {"blocks", generalWorkflowBlocks(m_trainingFinalSaveWorkflow).size()},
    };
    const QString signature = QString::fromUtf8(QJsonDocument(signatureObject).toJson(QJsonDocument::Compact));
    if (m_trainingPromptSignature == signature) {
        return;
    }
    m_trainingPromptSignature = signature;

    QVariantMap payload;
    payload.insert(QStringLiteral("title"), QStringLiteral("Workflow speichern"));
    payload.insert(QStringLiteral("message"), isChatWorkspace()
        ? (title.isEmpty()
            ? QStringLiteral("Mit Speichern wird ein neuer Workflow angelegt. Wenn der Titel anders lauten soll, antworte vor dem Speichern mit dem gewÃƒÂ¼nschten Titel.")
            : QStringLiteral("Titelvorschlag: \"%1\". Mit Speichern wird immer ein neuer Workflow angelegt. Wenn der Titel anders lauten soll, antworte vor dem Speichern mit dem gewÃƒÂ¼nschten Titel.").arg(title))
        : (title.isEmpty()
            ? QStringLiteral("Der Workflow wurde erfolgreich getestet. Mit Speichern wird daraus eine Workflow-Datei.")
            : QStringLiteral("Der Workflow \"%1\" wurde erfolgreich getestet. Mit Speichern wird daraus eine Workflow-Datei.").arg(title)));
    payload.insert(QStringLiteral("buttonText"), QStringLiteral("Speichern"));
    payload.insert(QStringLiteral("action"), QStringLiteral("finalSave"));
    payload.insert(QStringLiteral("workflowTitle"), title);
    emitToWebAsync(m_agentBridge, [payload](AiWebBridge* target) {
        Q_EMIT target->workflowTrainingFinalSavePromptChanged(payload);
    });
}

void BricsCadPage::clearWorkflowTrainingPrompts()
{
    m_trainingPromptSignature.clear();
    if (!m_agentBridge) {
        return;
    }
    emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
        Q_EMIT target->workflowTrainingSavePromptCleared();
    });
    emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
        Q_EMIT target->workflowTrainingRunPromptCleared();
    });
    emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
        Q_EMIT target->workflowTrainingFinalSavePromptCleared();
    });
}

void BricsCadPage::clearWorkflowTrainingSavePrompt()
{
    clearWorkflowTrainingPrompts();
}

void BricsCadPage::validateWorkflowWithBrxAndSave(
    const QString& rejectedContent,
    const QJsonObject& reply,
    const QJsonObject& workflow)
{
    bool runtimeSelectorNormalized = false;
    QJsonObject normalizedWorkflow = normalizedWorkflowRuntimeSelectors(workflow, &runtimeSelectorNormalized);
    if (runtimeSelectorNormalized) {
        appendBridgeLog("Workflow Training: Runtime-Selektoren auf lastResult normalisiert");
    }
    if (!m_trainingSlotValues.isEmpty() && !normalizedWorkflow.contains(QStringLiteral("knownSlotValues"))) {
        normalizedWorkflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }
    QString errorMessage;
    if (!validateWorkflowForTraining(normalizedWorkflow, errorMessage)) {
        retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
        return;
    }

    if (!m_brxAuthenticated || !bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("actions.validate"))) {
        retryWorkflowTrainingAfterValidationFailure(
            rejectedContent,
            reply,
            QStringLiteral("BRX actions.validate ist fuer Workflow-Training erforderlich. Verbinde BRX und lade Capabilities, bevor ein Workflow gespeichert wird."));
        return;
    }

    QJsonArray validationActions;
    const QJsonArray validationExamples = normalizedWorkflow.value("validationExamples").toArray();
    if (validationExamples.isEmpty()) {
        retryWorkflowTrainingAfterValidationFailure(
            rejectedContent,
            reply,
            QStringLiteral("workflow.validationExamples[0].actions fehlt. Fuer die direkte BRX-Validierung braucht der Workflow konkrete, platzhalterfreie Beispielaktionen."));
        return;
    }

    for (int exampleIndex = 0; exampleIndex < validationExamples.size(); ++exampleIndex) {
        const QJsonObject example = validationExamples.at(exampleIndex).toObject();
        const QJsonArray actions = example.value("actions").toArray();
        if (actions.isEmpty()) {
            errorMessage = QStringLiteral("workflow.validationExamples[%1].actions ist leer").arg(exampleIndex);
            retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
            return;
        }
        for (int actionIndex = 0; actionIndex < actions.size(); ++actionIndex) {
            const QJsonObject action = actions.at(actionIndex).toObject();
            const QString tool = action.value("tool").toString().trimmed();
            const QJsonObject params = action.value("params").toObject();
            if (tool.isEmpty() || !action.value("params").isObject()) {
                errorMessage = QStringLiteral("workflow.validationExamples[%1].actions[%2] braucht tool und params")
                    .arg(exampleIndex)
                    .arg(actionIndex);
                retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
                return;
            }
            if (jsonContainsTemplatePlaceholder(params)) {
                errorMessage = QStringLiteral("workflow.validationExamples[%1].actions[%2].params darf keine {{slot}} Platzhalter enthalten")
                    .arg(exampleIndex)
                    .arg(actionIndex);
                retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
                return;
            }

            QJsonObject pseudoStep{
                {"tool", tool},
                {"paramsTemplate", params},
            };
            if (!validateWorkflowStepForTraining(pseudoStep, actionIndex, errorMessage)) {
                retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
                return;
            }
            if (!validateAgentAction(action, errorMessage)) {
                retryWorkflowTrainingAfterValidationFailure(rejectedContent, reply, errorMessage);
                return;
            }
            validationActions.append(QJsonObject{{"tool", tool}, {"params", params}});
            if (validationActions.size() > 50) {
                retryWorkflowTrainingAfterValidationFailure(
                    rejectedContent,
                    reply,
                    QStringLiteral("workflow.validationExamples enthaelt mehr als 50 Aktionen; BRX actions.validate erlaubt maximal 50."));
                return;
            }
        }
    }

    appendBridgeLog(QString("Qt -> BRX: actions.validate Workflow-Training %1 Aktion(en)").arg(validationActions.size()));
    setAgentBusy(true);
    const bool queued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        QJsonObject{
            {"source", "workflow_training"},
            {"actions", validationActions},
        },
        15000,
        [this, rejectedContent, reply, normalizedWorkflow](const QJsonObject& response) {
            setAgentBusy(false);
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            const bool valid = response.value("ok").toBool(false)
                && result.value("valid").toBool(false);
            if (!valid) {
                const QString message = validationFailureMessage(response);
                appendBridgeLog(QString("BRX Workflow-Preflight: abgelehnt: %1").arg(message.left(700).replace('\n', " | ")));
                if (workflowTrainingBrxFailureIsRuntimeDependent(message)) {
                    QJsonObject warnedWorkflow = normalizedWorkflow;
                    QJsonArray warnings = warnedWorkflow.value("validationWarnings").toArray();
                    warnings.append(QStringLiteral("BRX actions.validate konnte ein statisches Trainingsbeispiel nicht vollstaendig pruefen, weil es von vorher erzeugten/ausgewaehlten Objekten abhaengt: %1")
                        .arg(message.left(500)));
                    warnedWorkflow.insert(QStringLiteral("validationWarnings"), warnings);
                    appendBridgeLog("BRX Workflow-Preflight: runtime-abhaengige Beispielvalidierung mit Warnung akzeptiert");
                    finishWorkflowTrainingSave(reply, warnedWorkflow);
                    return;
                }
                retryWorkflowTrainingAfterValidationFailure(
                    rejectedContent,
                    reply,
                    QString("BRX actions.validate hat die validationExamples abgelehnt. Korrigiere validationExamples und ggf. steps/paramsTemplate.\n%1").arg(message));
                return;
            }

            const QStringList warnings = stringsFromJsonArray(result.value("warnings").toArray());
            if (warnings.isEmpty()) {
                appendBridgeLog("BRX Workflow-Preflight: gueltig");
            } else {
                appendBridgeLog(QString("BRX Workflow-Preflight: gueltig mit Warnungen: %1").arg(warnings.join("; ").left(500)));
            }
            finishWorkflowTrainingSave(reply, normalizedWorkflow);
        });

    if (!queued) {
        setAgentBusy(false);
        retryWorkflowTrainingAfterValidationFailure(
            rejectedContent,
            reply,
            QStringLiteral("BRX actions.validate konnte fuer Workflow-Training nicht gesendet werden."));
    }
}

void BricsCadPage::sendWorkflowTrainingPrompt(const QString& prompt, bool compactContext)
{
    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model, AgentResponseKind::StructuredJson);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) {
        appendAgentChat("Barebone-Qt", QString("Ungueltige AI Server URL: %1").arg(baseUrl));
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    const QJsonObject envelope = workflowTrainingEnvelope(prompt, compactContext);
    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content", QStringLiteral(
            "Du bist der Barebone-Qt Workflow-Autorenagent im Trainingsmodus. "
            "Trainingsmodus ist im BricsCAD-Arbeitsbereich deaktiviert. "
            "Bearbeite oder erweitere bestehende Workflows nur, wenn trainingState.activeWorkflow explizit gesetzt ist; ohne activeWorkflow ist jede Nutzeranfrage eine Neuerstellung. "
            "Antworte ausschliesslich mit einem JSON-Objekt gemaess barebone.workflow.training.response.v1. "
            "Beim ersten langen fachlichen Prompt leitest du automatisch id, Titel, Beschreibung, Eingaben, Formeln und Batch-Struktur ab und antwortest mit type=workflow_draft. "
            "Stelle Rueckfragen erst dann mit type=ask_user, wenn nach dem Draft noch konkrete Pflichtangaben fehlen. "
            "Suche bestehende Workflows nicht anhand freier Bearbeiten-/Erweitern-Texte; der Nutzer waehlt bestehende Workflows dafuer im Workflow-Overlay aus. "
            "Nutze trainingState.knownSlotValues als verbindlichen Speicher; frage diese Werte nicht erneut ab. "
            "Wenn der Nutzer sagt, dass du Werte selbst auswaehlen sollst, erstelle eigene plausible Variablen/Defaults in knownSlotValues oder paramsTemplate und frage nicht nach Namen, Farben oder Listen. "
            "Bei TGA-Layern darfst du passende Gruppen und ACI-Farben selbst vorschlagen und als Workflow-Kontext speichern. "
            "Bei neuen Einzelangaben darfst du type=slot_update mit slots verwenden, statt einen kompletten Workflow zu schreiben. "
            "Bei type=ask_user darfst du keine vollstaendigen steps, validationExamples oder langen Workflow-Entwuerfe ausgeben; frage nur fehlende Daten ab. "
            "Bei type=workflow_draft darfst du workflowDraft mit description, derivedValues, executionBatches, constructionStrategy, authoringNotes und questions liefern; dieser Draft wird noch nicht gespeichert. "
            "Bei type=workflow_update muss workflow.steps flach ausfuehrbar sein, auch wenn executionBatches zusaetzlich gespeichert werden. "
            "Speichere Formeln als derivedValues mit expression und example; schreibe konkrete Beispielwerte in validationExamples. "
            "Fuer tabellarische allgemeine BricsCAD-Auswertungen nutze geometry.query, selection.describe oder entity.describe mit limit >= 1. Fuer klassifizierte BIM-Objekte und Properties nutze bim.objects.query; Qt gibt Objekt- und Propertytabellen im Workflow-Lauf und Chatfenster aus. "
            "Wenn Geometrie in einem bestimmten Layer entstehen soll, setze in geometry.create.paramsTemplate.layer den exakten Layernamen; ein vorheriges layers.create reicht nicht aus, um den Zeichenlayer implizit umzuschalten. "
            "Speichere keine starren Prompt-zu-Command-Regeln, sondern Slots, Defaults, Strategien, Constraints, Beispiele und bevorzugte Tools. "
            "Falls compactContext=true ist, konzentriere dich auf eine kurze gueltige JSON-Antwort und vermeide lange Erklaerungen.")},
    });
    const int trainingHistoryLimit = compactContext ? 4 : 16;
    const int trainingConversationSize = static_cast<int>(m_trainingConversation.size());
    const int trainingHistoryStart = std::max(0, trainingConversationSize - trainingHistoryLimit);
    for (int i = trainingHistoryStart; i < trainingConversationSize; ++i) {
        messages.append(m_trainingConversation.at(i).toObject());
    }
    messages.append(QJsonObject{
        {"role", "user"},
        {"content", QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact))},
    });
    const int requestedOutputTokens = dynamicOutputTokenTarget(
        compactContext ? 2048 : 4096,
        compactContext ? kWorkflowTrainingCompactOutputTokens : kWorkflowTrainingOutputTokens,
        compactContext ? 12 : 8);
    const int estimatedInputTokens = estimateTokensForMessages(messages);
    const int contextTokens = effectiveContextWindowTokens();
    const int safetyTokens = std::clamp(contextTokens / 20, 512, 2048);
    if (!compactContext && contextTokens > 0 && estimatedInputTokens + 1024 + safetyTokens > contextTokens) {
        appendBridgeLog(QString("Workflow Training: voller Kontext zu gross (%1/%2 Tokens), wiederhole kompakt")
            .arg(estimatedInputTokens)
            .arg(contextTokens));
        sendWorkflowTrainingPrompt(prompt, true);
        return;
    }
    if (compactContext && contextTokens > 0 && estimatedInputTokens + 512 + safetyTokens > contextTokens && trainingHistoryStart < m_trainingConversation.size()) {
        appendBridgeLog(QString("Workflow Training: kompakter Kontext noch zu gross (%1/%2 Tokens), sende ohne Trainingshistorie")
            .arg(estimatedInputTokens)
            .arg(contextTokens));
        messages = QJsonArray{
            messages.first().toObject(),
            QJsonObject{
                {"role", "user"},
                {"content", QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact))},
            },
        };
    }
    const int outputTokens = adjustedOutputTokenLimitForMessages(messages, requestedOutputTokens);

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", outputTokens);
        QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort == QStringLiteral("medium") || reasoningEffort == QStringLiteral("high")) {
            reasoningEffort = QStringLiteral("low");
        }
        if (reasoningEffort != "none") {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffortForModel(model, reasoningEffort)}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", outputTokens);
        insertChatReasoningForModel(payload, model, m_reasoningEffort);
        if (!officialProvider) {
            payload.insert(
                QStringLiteral("response_format"),
                localStructuredResponseFormat(
                    QStringLiteral("barebone_workflow_training"),
                    {QStringLiteral("schema"), QStringLiteral("type")}));
        }
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kWorkflowTrainingAiTimeoutMs);

    appendBridgeLog(QString("Qt -> AI Workflow Training: provider=%1 endpoint=%2 model=%3 compact=%4 timeoutMs=%5 maxTokens=%6 context=%7")
        .arg(provider,
            url.toString(),
            model,
            compactContext ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(kWorkflowTrainingAiTimeoutMs)
        .arg(outputTokens)
        .arg(effectiveContextWindowTokens()));

    const int operationGeneration = m_operationGeneration;
    enqueueAiPost(
        request,
        payload,
        QStringLiteral("WorkflowLearning"),
        70,
        false,
        false,
        {},
        [this, prompt, compactContext, operationGeneration](const LocalAiJobScheduler::Result& result) {
        if (operationGeneration != m_operationGeneration) {
            return;
        }

        const int httpStatus = result.httpStatus;
        const QByteArray body = result.body;
        if (result.networkError != QNetworkReply::NoError) {
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Workflow Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            const QString bodyText = QString::fromUtf8(body).toLower();
            const bool contextOverflow = httpStatus == 400
                && (bodyText.contains(QStringLiteral("context"))
                    || bodyText.contains(QStringLiteral("tokens"))
                    || bodyText.contains(QStringLiteral("exceeds")));
            if (!compactContext && contextOverflow) {
                appendBridgeLog("Workflow Training: LMStudio meldet Kontextueberschreitung, wiederhole kompakt");
                sendWorkflowTrainingPrompt(prompt, true);
                return;
            }
            appendAgentChat("AI", QString("Fehler bei der Workflow-Training-Anfrage: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(result.errorString));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("AI", QString("Workflow-Training-Antwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            appendBridgeLog(QString("AI Workflow Body: %1").arg(QString::fromUtf8(body).left(800)));
            return;
        }

        const QJsonObject responseObject = responseDocument.object();
        QString reasoningText;
        const QString content = repairMojibakeText(
            aiChatCompletionContent(responseObject, &reasoningText)).trimmed();
        const QString finishReason = aiChatCompletionFinishReason(responseObject);
        const auto rememberExchange = [this, &prompt](const QString& assistantContent) {
            m_trainingConversation.append(QJsonObject{{"role", "user"}, {"content", prompt}});
            m_trainingConversation.append(QJsonObject{{"role", "assistant"}, {"content", assistantContent}});
            while (m_trainingConversation.size() > 16) {
                m_trainingConversation.removeFirst();
            }
        };

        if (finishReason.compare(QStringLiteral("length"), Qt::CaseInsensitive) == 0) {
            appendBridgeLog(QString("AI Workflow: Antwort wurde vom Modell wegen Tokenlimit abgeschnitten compact=%1 contentChars=%2")
                .arg(compactContext ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(content.size()));

            const QJsonObject partialAskUser = workflowTrainingAskUserFromPartialContent(content);
            if (!partialAskUser.isEmpty()) {
                const QString normalizedAskUser = QString::fromUtf8(QJsonDocument(partialAskUser).toJson(QJsonDocument::Compact));
                appendBridgeLog(QString("AI Workflow: abgeschnittene ask_user Rueckfrage rekonstruiert: %1")
                    .arg(normalizedAskUser.left(800)));
                rememberExchange(normalizedAskUser);
                handleWorkflowTrainingReply(normalizedAskUser);
                return;
            }

            if (!compactContext) {
                sendWorkflowTrainingPrompt(
                    QStringLiteral("Deine letzte Workflow-Training-Antwort wurde vom Modell wegen max_tokens abgeschnitten. Antworte jetzt kurz und ausschliesslich mit barebone.workflow.training.response.v1 JSON. Wenn es ein Entwurf ist, nutze type=workflow_draft mit kurzer workflowDraft-Zusammenfassung. Wenn noch Daten fehlen, nutze type=ask_user mit message, missing und questions, aber ohne workflow.steps und ohne validationExamples. Wenn nur neue Angaben erkannt wurden, nutze slot_update. Wenn genug Daten vorhanden sind oder der Nutzer speichern will, nutze workflow_update kompakt."),
                    true);
                return;
            }

            appendAgentChat("AI", "Die Workflow-Training-Antwort wurde vom Modell abgeschnitten. Bitte sende die Trainingsanweisung noch einmal knapper oder reduziere die geforderten Workflow-Details.");
            return;
        }

        if (content.trimmed().isEmpty()) {
            appendBridgeLog(QString("AI Workflow: leere finale Antwort reasoningChars=%1 compact=%2")
                .arg(reasoningText.size())
                .arg(compactContext ? QStringLiteral("true") : QStringLiteral("false")));
            if (!compactContext
                && retryWorkflowTrainingAfterValidationFailure(
                    reasoningText,
                    {},
                    QStringLiteral("Die AI hat im Trainingsmodus keine finale JSON-Antwort geliefert. Antworte jetzt kurz und ausschliesslich mit barebone.workflow.training.response.v1 JSON."))) {
                return;
            }
            appendAgentChat("AI", "Leere Antwort im Trainingsmodus erhalten. Bitte sende die Trainingsanweisung noch einmal knapper oder stelle Reasoning auf niedrig.");
            return;
        }

        appendBridgeLog(QString("AI Workflow JSON: %1").arg(content.left(1600)));
        bool parsedForHistory = false;
        const QJsonObject historyObject = jsonObjectFromAiContent(content, &parsedForHistory);
        const QString historyType = historyObject.value("type").toString();
        if (parsedForHistory
            && (historyType == QStringLiteral("ask_user")
                || historyType == QStringLiteral("slot_update")
                || historyType == QStringLiteral("workflow_draft")
                || historyType == QStringLiteral("workflow_update")
                || historyType == QStringLiteral("workflow_save")
                || historyType == QStringLiteral("workflow"))) {
            rememberExchange(content);
        }
        handleWorkflowTrainingReply(content);
    });
}

bool BricsCadPage::saveGeneralWorkflowFinalDraft(QString* savedPath, QString* errorMessage)
{
    QJsonObject workflow = m_trainingFinalSaveWorkflow.isEmpty()
        ? m_trainingWorkflowContext
        : m_trainingFinalSaveWorkflow;
    if (workflow.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Kein Workflow-Entwurf vorhanden.");
        }
        return false;
    }

    workflow = normalizedGeneralWorkflowDraft(workflow);
    const QString validationError = generalWorkflowDraftValidationError(workflow);
    if (!validationError.isEmpty()) {
        if (errorMessage) {
            *errorMessage = validationError;
        }
        return false;
    }
    const QString formattingError = generalWorkflowFormattingValidationError(workflow);
    if (!formattingError.isEmpty()) {
        if (errorMessage) {
            *errorMessage = formattingError;
        }
        return false;
    }
    workflow = generalWorkflowWithUniqueId(workflow, generalWorkflowsDirectoryPath());

    if (!saveGeneralWorkflowFromObject(workflow, savedPath, errorMessage)) {
        return false;
    }

    QString fileName;
    QJsonObject savedWorkflow = loadGeneralWorkflowById(workflow.value(QStringLiteral("id")).toString(), &fileName);
    if (!savedWorkflow.isEmpty()) {
        m_selectedWorkflowId = savedWorkflow.value(QStringLiteral("id")).toString(QFileInfo(fileName).baseName());
        m_selectedWorkflow = savedWorkflow;
        m_selectedWorkflowSlotValues = {};
        m_trainingWorkflowContext = savedWorkflow;
    } else {
        m_trainingWorkflowContext = workflow;
    }
    return true;
}

QJsonObject BricsCadPage::workflowSaveCompressedTitleContext(const QString& selectedMessageText, const QString& sessionTitle) const
{
    auto previewText = [](QString text, int maxChars) {
        text = repairMojibakeText(text).trimmed();
        bool parsed = false;
        const QJsonObject parsedObject = jsonObjectFromAiContent(text, &parsed);
        if (parsed) {
            QString message = repairMojibakeText(parsedObject.value(QStringLiteral("message")).toString()).trimmed();
            if (message.isEmpty()) {
                const QJsonObject workflow = parsedObject.value(QStringLiteral("workflow")).toObject();
                const QString title = repairMojibakeText(workflow.value(QStringLiteral("title")).toString(
                    parsedObject.value(QStringLiteral("title")).toString())).trimmed();
                const QString description = repairMojibakeText(workflow.value(QStringLiteral("description")).toString(
                    parsedObject.value(QStringLiteral("description")).toString())).trimmed();
                message = QStringList{title, description}.join(QStringLiteral(" ")).trimmed();
            }
            if (message.isEmpty()) {
                const QString type = repairMojibakeText(parsedObject.value(QStringLiteral("type")).toString()).trimmed();
                message = type.isEmpty()
                    ? QString::fromUtf8(QJsonDocument(parsedObject).toJson(QJsonDocument::Compact))
                    : QStringLiteral("AI-Antworttyp: %1").arg(type);
            }
            text = message;
        }
        text = removeReasoningLeak(text)
            .replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
            .replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "))
            .trimmed();
        if (maxChars > 0 && text.size() > maxChars) {
            text = text.left(std::max(12, maxChars - 3)).trimmed() + QStringLiteral("...");
        }
        return text;
    };

    QJsonArray recentMessages;
    const qsizetype totalMessages = m_agentConversation.size();
    const qsizetype start = std::max<qsizetype>(0, totalMessages - 10);
    for (qsizetype i = start; i < totalMessages; ++i) {
        const QJsonObject message = m_agentConversation.at(i).toObject();
        const QString preview = previewText(message.value(QStringLiteral("content")).toString(), 700);
        if (preview.isEmpty()) {
            continue;
        }
        recentMessages.append(QJsonObject{
            {QStringLiteral("role"), message.value(QStringLiteral("role")).toString()},
            {QStringLiteral("preview"), preview},
        });
    }

    const QString selectedPreview = previewText(selectedMessageText, 2000);
    const QString sessionPreview = previewText(sessionTitle, 160);
    QStringList summaryLines{
        QStringLiteral("Komprimierter Kontext fuer die automatische Workflow-Titelerzeugung."),
        QStringLiteral("Der Titel soll aus fachlichem Schwerpunkt, markierter AI-Nachricht und Verlaufsvorschau abgeleitet werden."),
    };
    if (!selectedPreview.isEmpty()) {
        summaryLines << QStringLiteral("Markierte AI-Nachricht: %1").arg(selectedPreview);
    }
    if (!sessionPreview.isEmpty()) {
        summaryLines << QStringLiteral("Sitzungsname nur als schwacher Kontext, nicht als Titelvorgabe: %1").arg(sessionPreview);
    }
    for (const QJsonValue& value : recentMessages) {
        const QJsonObject message = value.toObject();
        const QString role = message.value(QStringLiteral("role")).toString() == QStringLiteral("user")
            ? QStringLiteral("Nutzer")
            : QStringLiteral("AI");
        summaryLines << QStringLiteral("%1: %2").arg(role, message.value(QStringLiteral("preview")).toString());
    }

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.title-context.v1")},
        {QStringLiteral("mode"), QStringLiteral("compressed-recent-message-previews")},
        {QStringLiteral("messagesIncluded"), recentMessages.size()},
        {QStringLiteral("messagesTotal"), static_cast<int>(totalMessages)},
        {QStringLiteral("selectedMessagePreview"), selectedPreview},
        {QStringLiteral("sessionTitlePreview"), sessionPreview},
        {QStringLiteral("recentMessages"), recentMessages},
        {QStringLiteral("summary"), summaryLines.join(QLatin1Char('\n')).left(7000)},
        {QStringLiteral("titlePolicy"), QStringLiteral("AI erstellt title selbst aus dieser komprimierten Kontextlage; keinen Sitzungsnamen automatisch uebernehmen.")},
    };
}

void BricsCadPage::saveGeneralWorkflowFromMessage(const QString& messageId, const QString& messageText, const QString& sessionTitle)
{
    if (!isChatWorkspace()) {
        appendAgentChat("Barebone-Qt", "Allgemeine Workflows koennen nur im Chat gespeichert werden.");
        return;
    }

    const QString cleanText = repairMojibakeText(messageText).trimmed();
    if (cleanText.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Workflow konnte nicht vorbereitet werden: Die ausgewaehlte AI-Nachricht ist leer.");
        return;
    }

    appendBridgeLog(QString("General Workflow Save From Message: messageId=%1 chars=%2")
        .arg(messageId.trimmed())
        .arg(cleanText.size()));
    const QString instruction = QStringLiteral(
        "Speichere die geklickte AI-Nachricht als neuen allgemeinen Workflow. "
        "Nutze selectedMessageText als primaeren fachlichen Inhalt. "
        "Erstelle title selbst aus compressedTitleContext und dem fachlichen Schwerpunkt; uebernimm keinen Sitzungsnamen als Titelvorgabe.");
    saveGeneralWorkflowFromTraining(
        instruction,
        0,
        {},
        {},
        cleanText,
        repairMojibakeText(sessionTitle).trimmed());
}










void BricsCadPage::saveBricsCadWorkflowFromSession(const QString& sessionTitle, int retryCount, const QString& validationError, const QString& rejectedContent)
{
    if (isChatWorkspace() || m_agentConversation.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Der BricsCAD-Sitzungsverlauf ist leer oder nicht aktiv.");
        return;
    }
    if (retryCount > 2) {
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht validiert werden: %1").arg(validationError));
        return;
    }
    const QJsonArray tools = availableAgentTools();
    if (tools.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Aktuelle BRX-Capabilities fehlen; der Workflow wird nicht mit erfundenen Tools gespeichert.");
        return;
    }
    const QString provider = m_config.aiProvider();
    const bool official = provider == QStringLiteral("official");
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (official ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b")) : m_config.aiModel().trimmed();
    const bool responsesApi = useResponsesApiForProvider(provider, model, AgentResponseKind::StructuredJson);
    const QUrl url(baseUrl + (responsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid() || (official && m_config.aiApiKey().trimmed().isEmpty())) {
        appendAgentChat("Barebone-Qt", "Die AI-Verbindung fuer die Workflow-Aufarbeitung ist nicht verwendbar.");
        return;
    }
    QJsonArray conversation;
    for (const QJsonValue& message : m_agentConversation) conversation.append(message);
    const QJsonObject context{{"schema", "barebone.bricscad.workflow.save.request.v1"},
        {"sessionTitle", repairMojibakeText(sessionTitle).trimmed()}, {"conversation", conversation},
        {"drawingContext", currentAgentContext()}, {"lastToolResult", m_lastAgentToolResult},
        {"selectedWorkflow", m_selectedWorkflow}, {"availableTools", tools}};
    QJsonArray messages{
        QJsonObject{{"role", "system"}, {"content", QStringLiteral(
            "Erstelle aus dem vollstaendigen BricsCAD-Sitzungsverlauf genau einen bereinigten, direkt ausfuehrbaren Batch-Workflow. Antworte nur mit einem JSON-Objekt. "
            "schema muss barebone.agent.workflow.v1 sein. Pflichtfelder: schema, id, title, description, triggerExamples, requiredSlots, optionalSlots, derivedValues, preferredTools, steps, executionBatches, constructionStrategy, forbidden, validationExamples. "
            "id und title duerfen niemals leer sein. Benenne das wiederverwendbare Thema, nicht die konkrete Ausfuehrung: "
            "keine Masse, Koordinaten, Anzahlen oder konkreten Objekt-/Layernamen in id/title. "
            "Beispiele: title='Layer erstellen', id='layer_erstellen'; title='Kreis-Geometrie erstellen', id='kreis_geometrie_erstellen'. "
            "optionalSlots muss immer ein Objekt nach Slotname sein, z.B. {\"layerName\":{\"type\":\"string\",\"description\":\"Ziellayer\"}}, niemals ein Array. "
            "Kombiniere Nutzerabsicht, Reparaturen, Kontext und erfolgreiche Ergebnisse zur final korrigierten Loesung; entferne fehlgeschlagene, abgelehnte, doppelte und ueberholte Versuche. "
            "Nutze ausschliesslich exakte Tools und Parameterschemas aus availableTools, keine nativen Commands oder erfundenen Tools. preferredTools enthaelt nur verwendete Tools. "
            "executionBatches braucht mindestens einen Batch mit mode='sequential', stopOnFailure=true und den finalen Schritten in richtiger Reihenfolge; steps enthaelt dieselben Schritte. "
            "Jeder Schritt braucht id, title, tool, paramsTemplate. Jeder {{slot}} muss definiert sein. validationExamples[0].actions enthaelt fuer jeden finalen Schritt konkrete, platzhalterfreie tool/params-Aktionen. "
            "Uebernimm keine zufaelligen Zeichnungs-Handles als feste IDs. Der Workflow muss spaeter als geordneter Batch ausfuehrbar sein.")}},
        QJsonObject{{"role", "user"}, {"content", QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact))}}};
    if (!validationError.trimmed().isEmpty()) {
        messages.append(QJsonObject{{"role", "user"}, {"content", QStringLiteral("Lokale Validierung abgelehnt: %1\nKorrigiere vollstaendig: %2")
            .arg(validationError.left(3000), rejectedContent.left(6000))}});
    }
    QJsonObject payload{{"model", model}};
    const int tokens = adjustedOutputTokenLimitForMessages(messages, 16384);
    if (responsesApi) {
        payload.insert("input", messages); payload.insert("max_output_tokens", tokens);
        const QString effort = normalizedReasoningEffort(m_reasoningEffort);
        if (effort != QStringLiteral("none")) payload.insert("reasoning", QJsonObject{{"effort", reasoningEffortForModel(model, effort)}});
    } else {
        payload.insert("messages", messages); payload.insert("temperature", 0.1); payload.insert("max_tokens", tokens);
        insertChatReasoningForModel(payload, model, m_reasoningEffort);
        if (!official) payload.insert("response_format", localStructuredResponseFormat(QStringLiteral("barebone_bricscad_workflow"),
            {QStringLiteral("schema"), QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("preferredTools"), QStringLiteral("steps"), QStringLiteral("executionBatches")}));
    }
    QNetworkRequest request(url); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (official) request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    request.setTransferTimeout(kAiModelResponseTimeoutMs);
    setAgentBusy(true);
    appendBridgeLog(QString("Qt -> AI BricsCAD Workflow Session Save: messages=%1 tools=%2 retry=%3").arg(conversation.size()).arg(tools.size()).arg(retryCount));
    const int generation = m_operationGeneration;
    enqueueAiPost(request, payload, QStringLiteral("BricsCadWorkflowSave"), 70, false, false, {},
        [this, generation, sessionTitle, retryCount](const LocalAiJobScheduler::Result& result) {
        if (generation != m_operationGeneration) return;
        setAgentBusy(false);
        if (result.networkError != QNetworkReply::NoError) {
            appendAgentChat("Barebone-Qt", QString("Workflow-Aufarbeitung fehlgeschlagen: %1").arg(result.errorString)); return;
        }
        QJsonParseError parseError; const QJsonDocument response = QJsonDocument::fromJson(result.body, &parseError);
        QString reasoning; const QString content = response.isObject()
            ? repairMojibakeText(aiChatCompletionContent(response.object(), &reasoning)).trimmed() : QString{};
        bool parsed = false; QJsonObject workflow = jsonObjectFromAiContent(content, &parsed);
        if (parsed) {
            workflow = workflowObjectFromAiSaveResponse(workflow);
        }
        QString error;
        if (!parsed) error = QStringLiteral("Die AI-Antwort war kein gueltiges JSON-Objekt.");
        else {
            workflow = normalizedWorkflowRuntimeSelectors(workflow);
            ensureWorkflowIdentityForSessionSave(workflow, sessionTitle, m_lastAgentUserPrompt);
            validateWorkflowForTraining(workflow, error);
            if (error.isEmpty() && workflow.value("executionBatches").toArray().isEmpty()) error = QStringLiteral("executionBatches fehlt.");
            const QJsonArray examples = workflow.value("validationExamples").toArray();
            if (error.isEmpty() && examples.isEmpty()) error = QStringLiteral("validationExamples fehlt.");
            if (error.isEmpty()) {
                const QJsonArray actions = examples.first().toObject().value("actions").toArray();
                if (actions.isEmpty()) error = QStringLiteral("validationExamples[0].actions fehlt.");
                for (int i = 0; error.isEmpty() && i < actions.size(); ++i) {
                    const QJsonObject action = actions.at(i).toObject();
                    if (jsonContainsTemplatePlaceholder(action.value("params"))) {
                        error = QStringLiteral("validationExamples[0].actions[%1] enthaelt einen Platzhalter.").arg(i);
                    } else if (!validateAgentAction(action, error)) {
                        error = QStringLiteral("validationExamples[0].actions[%1]: %2").arg(i).arg(error);
                    }
                }
            }
        }
        if (!error.isEmpty()) { saveBricsCadWorkflowFromSession(sessionTitle, retryCount + 1, error, content); return; }
        QString savedPath;
        if (!saveWorkflowFromTraining(workflow, &savedPath, &error)) {
            appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht gespeichert werden: %1").arg(error)); return;
        }
        const QString savedId = QFileInfo(savedPath).completeBaseName();
        QJsonObject savedWorkflow = loadBricsCadWorkflowById(savedId);
        if (!savedWorkflow.isEmpty()) {
            m_selectedWorkflowId = savedId; m_selectedWorkflow = savedWorkflow;
            m_selectedWorkflowSlotValues = savedWorkflow.value("knownSlotValues").toObject();
            if (m_agentBridge) emitToWebAsync(m_agentBridge, [map = savedWorkflow.toVariantMap()](AiWebBridge* target) { Q_EMIT target->selectedWorkflowChanged(map); });
        }
        emitWorkflowListToWeb();
        appendAgentChat("Barebone-Qt", QString("Der gesamte Sitzungsverlauf wurde bereinigt und als Batch-Workflow gespeichert: %1").arg(savedPath));
    });
}

void BricsCadPage::saveGeneralWorkflowFromTraining(
    const QString& userInstruction,
    int retryCount,
    const QString& validationError,
    const QString& rejectedContent,
    const QString& selectedMessageText,
    const QString& sessionTitle)
{
    if (!isChatWorkspace()) {
        appendAgentChat("Barebone-Qt", "Allgemeine Workflows koennen nur im Chat gespeichert werden.");
        return;
    }

    const QString cleanSelectedMessageText = repairMojibakeText(selectedMessageText).trimmed();
    const QString cleanSessionTitle = repairMojibakeText(sessionTitle).trimmed();
    const bool saveFromMessageFooter = !cleanSelectedMessageText.isEmpty();

    if (m_agentConversation.isEmpty() && m_trainingFinalSaveWorkflow.isEmpty() && cleanSelectedMessageText.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Es gibt noch keinen Kontext, aus dem ein Workflow erstellt werden kann.");
        return;
    }

    if (retryCount == 0 && validationError.trimmed().isEmpty()) {
        m_generalWorkflowSaveRetries = 0;
        m_generalWorkflowSaveRejectedSignatures.clear();
        m_lastGeneralWorkflowSaveContext = {};
        if (saveFromMessageFooter) {
            m_trainingFinalSavePending = false;
            m_trainingFinalSaveWorkflow = {};
            m_trainingFinalSaveActions = {};
        }
    }

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi =
        useResponsesApiForProvider(provider, model, AgentResponseKind::StructuredJson);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) {
        appendAgentChat("Barebone-Qt", QString("Ungueltige AI Server URL: %1").arg(baseUrl));
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    QJsonArray compactConversation;
    for (int i = 0; i < m_agentConversation.size(); ++i) {
        compactConversation.append(m_agentConversation.at(i));
    }
    const QJsonObject compressedTitleContext = workflowSaveCompressedTitleContext(cleanSelectedMessageText, cleanSessionTitle);
    const QJsonObject pendingWorkflowForSave = saveFromMessageFooter ? QJsonObject{} : m_trainingFinalSaveWorkflow;

    QJsonObject saveContext{
        {"schema", "barebone.general.workflow.save.request.v1"},
        {"source", saveFromMessageFooter ? QStringLiteral("message_footer") : QStringLiteral("chat_prompt")},
        {"forceNewWorkflow", true},
        {"selectedWorkflow", QJsonObject{}},
        {"pendingWorkflow", pendingWorkflowForSave},
        {"userInstruction", repairMojibakeText(userInstruction).trimmed()},
        {"selectedMessageText", cleanSelectedMessageText},
        {"sessionTitle", cleanSessionTitle},
        {"compressedTitleContext", compressedTitleContext},
        {"conversation", compactConversation},
        {"conversationCompression", QJsonObject{
            {"mode", QStringLiteral("full")},
            {"messagesIncluded", compactConversation.size()},
            {"messagesTotal", m_agentConversation.size()},
        }},
        {"namingPolicy", QJsonObject{
            {"newWorkflowTitle", QStringLiteral("Erzeuge title selbst aus compressedTitleContext.summary, selectedMessageText und dem fachlichen Schwerpunkt des Workflows. Der title wird in der Sidebar angezeigt.")},
            {"sessionTitlePolicy", QStringLiteral("sessionTitle ist hoechstens schwacher Kontext und darf nicht automatisch als title uebernommen werden.")},
            {"idPolicy", QStringLiteral("Leite id als stabilen snake_case Dateinamen aus title ab. Keine generischen IDs wie workflow oder neuer_workflow.")},
            {"overwritePolicy", QStringLiteral("Chat-Speichern legt immer einen neuen Workflow an. Keine vorhandene id uebernehmen und keinen bestehenden Workflow ueberschreiben.")},
        }},
        {"requiredSchema", QJsonObject{
            {"schema", "barebone.general.workflow.save.response.v1"},
            {"fields", QJsonArray{"schema", "id", "title", "description", "blocks", "tables", "inputs", "formulas", "examples", "assumptions", "warnings", "sourceRefs", "verificationStatus", "tags"}},
        }},
    };
    m_lastGeneralWorkflowSaveContext = saveContext;

    QJsonArray messages{
        QJsonObject{
            {"role", "system"},
            {"content", QStringLiteral(
                "Du erstellst aus dem gesamten Chatkontext einen detaillierten allgemeinen Barebone-Qt Workflow. "
                "Antworte ausschliesslich mit genau einem JSON-Objekt. Keine Markdown-Antwort, kein Codeblock, keine Erklaerung ausserhalb von JSON. "
                "Das JSON muss schema='barebone.general.workflow.save.response.v1' verwenden. "
                "Pflichtfelder: schema, id, title, description, blocks. blocks ist ein Array aus Objekten mit id, title, text. "
                "Optionale Felder: tables, inputs, formulas, examples, assumptions, warnings, sourceRefs, verificationStatus, tags. "
                "Erstelle keine knappe Kurzfassung. Wenn selectedMessageText vorhanden ist, nutze diese markierte AI-Nachricht als primaeren fachlichen Workflow-Inhalt; conversation und compressedTitleContext dienen dann zur Einordnung und Titelwahl, nicht zum Einmischen fremder Themen. "
                "Wenn selectedMessageText leer ist, kombiniere detailreich alle fachlichen Informationen aus conversation, pendingWorkflow und userInstruction. "
                "Bewahre Tabellen, Formeln, Beispiele, Rechenschritte, Annahmen, Hinweise, Nutzerkorrekturen und offene Unsicherheiten. "
                "Unterteile nur grob nach AbsÃƒÂ¤tzen; fachliche Tiefe gehoert in blocks[].text, nicht in eine komplexe JSON-Verschachtelung. "
                "Bei Berechnungen muss nach jeder Grundgleichung eine kurze Symbolerklaerung der verwendeten Groessen folgen. "
                "Rechenschritte muessen Einheiten an jeder eingesetzten Zahl und jedem Summanden fuehren; keine reine Zahlenkette mit Einheit nur am Ende. "
                "Wenn Eingabewerte nicht in SI-Einheiten vorliegen, zeige zuerst die Umrechnung in SI-Einheiten. "
                "Tabellen muessen entweder als tables[] mit columns und rows oder als gueltige Markdown-Pipe-Tabelle mit Header, |---|---|-Trennzeile und Datenzeilen ausgegeben werden; keine tabulatorgetrennten Tabellen. "
                "Aufzaehlungen muessen echte Markdown-Listen sein, je ein Punkt pro Zeile mit '- ' oder '1. '; keine zusammengeklebten Listen in einer Zeile. "
                "Nutze keine HTML-Tags wie <br>; verwende echte Zeilenumbrueche. "
                "Bei Korrektur-Retries wegen Formatierung darfst du fachliche Inhalte nicht kuerzen oder neu zusammenfassen; korrigiere nur die betroffenen Formatierungen und erhalte alle Details. "
                "Du musst selbst einen kurzen, passenden title fuer das konkrete Thema erstellen; nutze dafuer compressedTitleContext.summary, selectedMessageText und den fachlichen Schwerpunkt. "
                "Uebernimm sessionTitle nicht automatisch als title. "
                "Leite id daraus als stabilen snake_case Dateinamen ab. title und id duerfen nicht Workflow, Neuer Workflow, Chat Workflow, General Workflow oder aehnlich generisch heissen. "
                "Chat-Speichern legt immer einen neuen Workflow an; uebernimm keine vorhandene id und ueberschreibe keinen gespeicherten Workflow. "
                "Nutze keine BricsCAD-Workflow-Felder wie steps, executionBatches, validationExamples oder tools. "
                "Wenn pendingWorkflow vorhanden ist, ueberarbeite diesen anhand userInstruction. "
                "Wenn Werte AI-Entwurf sind, setze verificationStatus='AI-Entwurf'.")},
        },
        QJsonObject{
            {"role", "user"},
            {"content", QString::fromUtf8(QJsonDocument(saveContext).toJson(QJsonDocument::Compact))},
        },
    };
    if (!validationError.trimmed().isEmpty()) {
        messages.append(QJsonObject{
            {"role", "user"},
            {"content", QStringLiteral(
                "Die vorherige Antwort wurde lokal abgelehnt und wird dem Nutzer nicht angezeigt.\n"
                "Korrekturauftrag: %1\n"
                "Vorherige Antwort gekuerzt: %2\n"
                "Erzeuge jetzt ein anderes, gueltiges JSON-Objekt nach barebone.general.workflow.save.response.v1.")
                .arg(validationError.left(2500), rejectedContent.left(2500))},
        });
    }

    QJsonObject payload;
    payload.insert("model", model);
    const int outputTokens = adjustedOutputTokenLimitForMessages(messages, 16384);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", outputTokens);
        QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort == QStringLiteral("high")) {
            reasoningEffort = QStringLiteral("low");
        }
        if (reasoningEffort != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffortForModel(model, reasoningEffort)}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", outputTokens);
        insertChatReasoningForModel(payload, model, m_reasoningEffort);
        if (!officialProvider) {
            payload.insert(
                QStringLiteral("response_format"),
                localStructuredResponseFormat(
                    QStringLiteral("barebone_general_workflow"),
                    {QStringLiteral("schema"), QStringLiteral("id"),
                        QStringLiteral("title"), QStringLiteral("description"),
                        QStringLiteral("blocks")}));
        }
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    clearWorkflowTrainingPrompts();
    appendBridgeLog(QString("Qt -> AI General Workflow Prepare Save: retry=%1 provider=%2 endpoint=%3 model=%4")
        .arg(retryCount)
        .arg(provider, url.toString(), model));

    const int operationGeneration = m_operationGeneration;
    enqueueAiPost(
        request,
        payload,
        QStringLiteral("WorkflowLearning"),
        70,
        false,
        false,
        {},
        [this, operationGeneration, saveContext](const LocalAiJobScheduler::Result& result) {
        if (operationGeneration != m_operationGeneration) {
            return;
        }
        const int httpStatus = result.httpStatus;
        const QByteArray body = result.body;
        if (result.networkError != QNetworkReply::NoError) {
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI General Workflow Prepare Save Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht vorbereitet werden: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(result.errorString));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("Barebone-Qt", QString("Speicherantwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            return;
        }

        QString reasoningText;
        const QString content = repairMojibakeText(
            aiChatCompletionContent(responseDocument.object(), &reasoningText)).trimmed();
        bool parsed = false;
        QJsonObject workflow = jsonObjectFromAiContent(content, &parsed);
        if (!parsed) {
            appendBridgeLog(QString("AI General Workflow Prepare Save Content: %1").arg(content.left(1000)));
            if (retryGeneralWorkflowSaveAfterValidationFailure(
                    saveContext,
                    content,
                    QStringLiteral("Die AI hat kein gueltiges Workflow-JSON geliefert. Antworte ausschliesslich mit genau einem JSON-Objekt nach barebone.general.workflow.save.response.v1."))) {
                return;
            }
            appendAgentChat("Barebone-Qt", "Workflow konnte nicht vorbereitet werden: Die AI hat kein gueltiges Workflow-JSON geliefert.");
            return;
        }

        workflow = generalWorkflowFromSaveResponse(workflow);
        const QString validationError = generalWorkflowDraftValidationError(workflow);
        if (!validationError.isEmpty()) {
            const QString rejectedJson = QString::fromUtf8(QJsonDocument(workflow).toJson(QJsonDocument::Compact));
            if (retryGeneralWorkflowSaveAfterValidationFailure(saveContext, rejectedJson, validationError)) {
                return;
            }
            appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(validationError));
            return;
        }

        const QString formattingError = generalWorkflowFormattingValidationError(workflow);
        if (!formattingError.isEmpty()) {
            const QString rejectedJson = QString::fromUtf8(QJsonDocument(workflow).toJson(QJsonDocument::Compact));
            if (retryGeneralWorkflowSaveAfterValidationFailure(saveContext, rejectedJson, formattingError)) {
                return;
            }
            appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(formattingError));
            return;
        }

        m_generalWorkflowSaveRetries = 0;
        m_generalWorkflowSaveRejectedSignatures.clear();
        m_lastGeneralWorkflowSaveContext = {};
        m_trainingFinalSavePending = true;
        m_trainingFinalSaveWorkflow = workflow;
        m_trainingFinalSaveActions = {};
        m_trainingWorkflowContext = workflow;
        m_trainingPhase = QStringLiteral("general_ready_to_save");
        appendAgentChat("AI", generalWorkflowDraftMessageForChat(workflow));
        emitWorkflowTrainingFinalSavePrompt();
    });
}
void BricsCadPage::handleWorkflowTrainingReply(const QString& content)
{
    bool parsed = false;
    const QJsonObject reply = jsonObjectFromAiContent(content, &parsed);
    if (!parsed) {
        appendBridgeLog("Workflow Training: Antwort konnte nicht als JSON interpretiert werden");
        const QJsonObject partialAskUser = workflowTrainingAskUserFromPartialContent(content);
        if (!partialAskUser.isEmpty()) {
            appendBridgeLog("Workflow Training: unvollstaendige ask_user Antwort rekonstruiert");
            const QString normalizedAskUser = QString::fromUtf8(QJsonDocument(partialAskUser).toJson(QJsonDocument::Compact));
            handleWorkflowTrainingReply(normalizedAskUser);
            return;
        }
        if (retryWorkflowTrainingAfterValidationFailure(
                content,
                {},
                QStringLiteral("Die Trainingsantwort konnte nicht als JSON gelesen werden. Korrigiere Syntaxfehler, schliesse Arrays/Objekte korrekt und antworte ausschliesslich mit barebone.workflow.training.response.v1 JSON."))) {
            return;
        }
        appendAgentChat("Barebone-Qt", "Die Trainingsantwort konnte nicht als Workflow-JSON gelesen werden. Bitte formuliere den letzten Schritt erneut.");
        return;
    }

    const QString type = reply.value("type").toString();
    const QString message = repairMojibakeText(reply.value("message").toString()).trimmed();

    if (type == QStringLiteral("workflow_draft")) {
        const bool repairPhase = m_trainingPhase == QStringLiteral("repairing");
        QJsonObject draft = reply.value("workflowDraft").toObject();
        if (draft.isEmpty()) {
            draft = reply.value("draft").toObject();
        }
        if (draft.isEmpty()) {
            draft = reply.value("workflow").toObject();
        }
        if (!draft.isEmpty()) {
            mergeWorkflowTrainingContext(draft);
        }
        const QJsonObject slots = reply.value("slots").toObject();
        if (!slots.isEmpty()) {
            mergeWorkflowTrainingSlotValues(slots);
        }
        m_trainingMissing = unresolvedWorkflowTrainingMissing(reply.value("missing").toArray());
        m_trainingPhase = QStringLiteral("drafting");
        m_trainingValidationRetries = 0;

        QJsonObject displayDraft = draft;
        if (displayDraft.isEmpty()) {
            displayDraft = m_trainingWorkflowContext;
        }
        displayDraft = workflowWithPrunedRequiredSlots(displayDraft);
        const QJsonArray questions = reply.value("questions").toArray();
        if (repairPhase && m_trainingReviewConfirmed && questions.isEmpty() && !displayDraft.isEmpty()) {
            appendBridgeLog("Workflow Training Repair: Draft statt workflow_update erhalten; fordere speicherbare Reparatur an");
            sendWorkflowTrainingPrompt(
                QStringLiteral("Die letzte Reparaturantwort war nur ein workflow_draft. Erzeuge jetzt daraus ein kompaktes type=workflow_update mit vollstaendigem workflow Objekt. Repariere nur den betroffenen Schritt, behalte alle anderen Schritte bei und nutze fuer step-by-step Workflows nach der Extrusion selection.set plus bim.classify target=selection statt target=lastExtruded."),
                true);
            return;
        }
        appendAgentChat("AI", workflowDraftMessageForChat(reply, displayDraft, m_trainingMissing));
        return;
    }

    if (type == QStringLiteral("ask_user")) {
        if (workflowTrainingAskUserRequestsDelegatedValues(reply, m_lastAgentUserPrompt)) {
            appendBridgeLog("Workflow Training: ask_user nach delegierten Werten abgelehnt; AI soll eigene Variablen/Defaults erzeugen");
            if (retryWorkflowTrainingAfterValidationFailure(
                    content,
                    reply,
                    QStringLiteral("type=ask_user ist unzureichend: Der Nutzer hat die Auswahl explizit an die AI delegiert. Frage nicht nach Layernamen, Layerfarben, ACI-Werten oder Listen. Erzeuge stattdessen plausible Defaultwerte selbst, speichere sie als knownSlotValues oder direkte paramsTemplate-Konstanten und aktualisiere den Entwurf mit workflow_draft oder workflow_update. Bei TGA-Layern mit 10 Gruppen erzeuge selbst eine sinnvolle Liste mit Namen und colorIndex-Werten, z.B. Sanitaer, Heizung, Lueftung, Elektro, Kaelte, Sprinkler, Gas, MSR, Brandschutz, Daemmung."))) {
                return;
            }
        }

        QJsonObject workflow = reply.value("workflow").toObject();
        if (workflow.isEmpty()) {
            workflow = reply.value("workflowSeed").toObject();
        }
        if (!workflow.isEmpty()) {
            mergeWorkflowTrainingContext(workflow);
        }
        const QJsonArray unresolvedMissing = unresolvedWorkflowTrainingMissing(reply.value("missing").toArray());
        m_trainingMissing = unresolvedMissing;
        m_trainingPhase = QStringLiteral("collecting");
        m_trainingValidationRetries = 0;

        if (unresolvedMissing.isEmpty()
            && !reply.value("missing").toArray().isEmpty()
            && workflowHasExecutableTrainingContent(m_trainingWorkflowContext)) {
            appendBridgeLog("Workflow Training: Rueckfrage uebersprungen, weil die gemeldeten missing Slots nicht fuer die Ausfuehrung referenziert sind");
            m_trainingPhase = QStringLiteral("drafting");
            sendWorkflowTrainingPrompt(
                QStringLiteral("Die zuletzt angefragten fehlenden Angaben sind fuer den aktiven Workflow nicht relevant, weil sie in keinem paramsTemplate, keiner condition und keiner derivedValues-Abhaengigkeit referenziert werden. Entferne diese requiredSlots/missing Angaben und aktualisiere den Entwurf mit type=workflow_draft. Frage nicht erneut nach diesen Angaben."),
                true);
            return;
        }

        QStringList lines;
        const bool genericMessage = isGenericWorkflowAskUserText(message);
        if (!message.isEmpty() && !genericMessage) {
            lines << message;
        }
        const QJsonArray questions = reply.value("questions").toArray();
        for (const QJsonValue& value : questions) {
            const QString question = repairMojibakeText(value.toString()).trimmed();
            if (!isGenericWorkflowAskUserText(question)
                && workflowTrainingQuestionNeedsAnswer(question, m_trainingSlotValues)) {
                lines << QStringLiteral("- %1").arg(question);
            }
        }
        QJsonObject filteredReply = reply;
        filteredReply.insert(QStringLiteral("missing"), unresolvedMissing);
        const QStringList missing = unresolvedMissing.isEmpty()
            ? QStringList{}
            : workflowTrainingMissingLabels(filteredReply, m_trainingWorkflowContext);
        if (lines.isEmpty() && !missing.isEmpty()) {
            lines << QStringLiteral("Bitte gib folgende Angaben fuer den Workflow an:");
        }
        if (!missing.isEmpty() && questions.isEmpty()) {
            for (const QString& item : missing.mid(0, 8)) {
                lines << QStringLiteral("- %1").arg(repairMojibakeText(item));
            }
        }
        if (lines.isEmpty()) {
            if (genericMessage || questions.isEmpty()) {
                appendBridgeLog("Workflow Training: generische ask_user Rueckfrage ohne verwertbaren Inhalt abgelehnt");
                if (retryWorkflowTrainingAfterValidationFailure(
                        content,
                        reply,
                        QStringLiteral("type=ask_user ist unzureichend: Die Rueckfrage enthaelt nur einen Platzhalter oder keinen konkreten Inhalt. Formuliere eine konkrete fachliche Frage mit Kontext oder erstelle einen workflow_draft/workflow_update. Wenn der Nutzer bereits relativ/Verschiebungsvektor gesagt hat, frage nicht erneut absolute vs relative ab."))) {
                    return;
                }
            }
            const QString title = repairMojibakeText(workflow.value("title").toString()).trimmed();
            lines << (title.isEmpty()
                ? QStringLiteral("Welche konkreten Pflichtangaben soll dieser Workflow vom Nutzer abfragen?")
                : QStringLiteral("Welche konkreten Pflichtangaben soll der Workflow \"%1\" vom Nutzer abfragen?").arg(title));
        }
        appendAgentChat("AI", lines.join('\n'));
        return;
    }

    if (type == QStringLiteral("slot_update")) {
        const QJsonObject slots = reply.value("slots").toObject();
        if (!slots.isEmpty()) {
            mergeWorkflowTrainingSlotValues(slots);
            appendBridgeLog(QString("Workflow Training Slots aktualisiert: %1")
                .arg(QString::fromUtf8(QJsonDocument(m_trainingSlotValues).toJson(QJsonDocument::Compact))));
        }
        m_trainingPhase = m_trainingMissing.isEmpty()
            ? QStringLiteral("drafting")
            : QStringLiteral("collecting");

        if (!message.isEmpty()) {
            appendAgentChat("AI", message);
        }

        sendWorkflowTrainingPrompt(
            m_trainingMissing.isEmpty()
                ? QStringLiteral("Die bekannten Slot-Werte sind im trainingState gespeichert. Aktualisiere den Entwurf mit workflow_draft, erzeuge bei Speicherwunsch einen kompakten workflow_update oder stelle genau eine weitere Rueckfrage, falls fachlich noch etwas fehlt.")
                : QStringLiteral("Die neuen Slot-Werte sind im trainingState gespeichert. Frage jetzt nur die verbleibenden unbekannten Angaben ab."),
            true);
        return;
    }

    if (type == QStringLiteral("workflow_update")
        || type == QStringLiteral("workflow_save")
        || type == QStringLiteral("workflow")) {
        QJsonObject workflow = reply.value("workflow").toObject();
        if (workflow.isEmpty()) {
            workflow = reply.value("workflowJson").toObject();
        }
        if (workflow.isEmpty()) {
            workflow = reply.value("workflowDraft").toObject();
        }
        if (workflow.isEmpty()) {
            workflow = m_trainingWorkflowContext;
        }
        if (!m_trainingSlotValues.isEmpty()) {
            workflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
        }
        workflow = workflowWithPrunedRequiredSlots(workflow);
        if (!m_trainingReviewConfirmed) {
            mergeWorkflowTrainingContext(workflow);
            appendAgentChat("AI", workflowDraftMessageForChat(reply, m_trainingWorkflowContext, m_trainingMissing));
            return;
        }
        m_trainingPhase = QStringLiteral("validating");
        validateWorkflowWithBrxAndSave(content, reply, workflow);
        return;
    }

    if (type == QStringLiteral("message")) {
        if (retryWorkflowTrainingAfterValidationFailure(
                content,
                reply,
                QStringLiteral("type=message ist im Trainingsmodus nicht ausreichend, solange ein Workflow erstellt oder bearbeitet wird. Nutze workflow_draft fuer strukturierte Entwuerfe, ask_user fuer Rueckfragen oder workflow_update mit vollstaendigem workflow."))) {
            return;
        }
        appendAgentChat("AI", message.isEmpty()
            ? QStringLiteral("Trainingsmodus ist aktiv. Beschreibe den Workflow, den wir erstellen oder bearbeiten sollen.")
            : message);
        return;
    }

    appendAgentChat("Barebone-Qt", QString("Unbekannter Workflow-Training-Antworttyp: %1").arg(type.isEmpty() ? QStringLiteral("<leer>") : type));
}

void BricsCadPage::handleAgentReply(const QString& content)
{
    bool parsed = false;
    const QJsonObject reply = jsonObjectFromAiContent(content, &parsed);
    if (!parsed) {
        if (m_chatMode == QStringLiteral("general")
            && !routeAllowsCadActions(m_lastAgentRoute)
            && !content.trimmed().startsWith(QLatin1Char('{'))) {
            const QString plainMessage =
                removeReasoningLeak(repairMojibakeText(content)).trimmed();
            if (!plainMessage.isEmpty()) {
                appendBridgeLog("AI Agent: Allgemeiner Modus akzeptiert Plain-Text-Antwort als message");
                appendAgentChat("AI", plainMessage);
                m_agentValidationRetries = 0;
                return;
            }
        }
        if (retryAgentAfterValidationFailure(
                content,
                {},
                "Antwort ist kein gueltiges Barebone-Agent-JSON. Antworte ausschliesslich mit einem JSON-Objekt gemaess erlaubten Antworttypen.")) {
            return;
        }
        appendBridgeLog("AI Agent: Antwort konnte nicht als Barebone JSON interpretiert werden");
        appendAgentChat("Barebone-Qt", "AI Antwort konnte nicht als gueltiges Agent-JSON gelesen werden. Bitte formuliere die Aktion erneut.");
        return;
    }

    const QString sessionTitleSuggestion = sessionTitleSuggestionFromAgentReply(reply);
    const QVariantMap sessionTitleExtra = sessionTitleSuggestion.isEmpty()
        ? QVariantMap{}
        : QVariantMap{{QStringLiteral("sessionTitleSuggestion"), sessionTitleSuggestion}};
    const QString rawType = reply.value("type").toString();
    QString type = rawType;
    if (type.trimmed().isEmpty()
        && m_chatMode == QStringLiteral("general")
        && !routeAllowsCadActions(m_lastAgentRoute)) {
        const QString fallbackMessage = repairMojibakeText(
            reply.value(QStringLiteral("message")).toString(
                reply.value(QStringLiteral("answer")).toString(
                    reply.value(QStringLiteral("content")).toString()))).trimmed();
        if (!fallbackMessage.isEmpty()) {
            appendBridgeLog("AI Agent: Allgemeiner Modus akzeptiert JSON ohne type als message");
            if (!sessionTitleSuggestion.isEmpty()) {
                emitSessionTitleSuggestion(sessionTitleSuggestion);
            }
            appendAgentChat("AI", fallbackMessage, sessionTitleExtra);
            m_agentValidationRetries = 0;
            return;
        }
    }
    if (type == "assistant_message") {
        type = "message";
    } else if (type == "tool_proposal") {
        type = "action_proposal";
    } else if (type == "workflow_proposal"
        || type == "workflow_run"
        || type == "workflow_run_proposal") {
        appendBridgeLog("AI Agent: legacy workflow_run_proposal zu action_proposal normalisiert");
        type = "action_proposal";
    } else if (type == "operation_plan") {
        type = "plan";
    }
    const QString message = repairMojibakeText(reply.value("message").toString());
    const QJsonArray effectiveToolsForRoute = availableAgentToolsForRoute(m_lastAgentRoute, m_lastAgentUserPrompt);
    const bool toolsAvailableForRoute = !effectiveToolsForRoute.isEmpty();

    if (!routeAllowsResponseType(m_lastAgentRoute, type, toolsAvailableForRoute)) {
        if (retryAgentAfterValidationFailure(
                content,
                reply,
                QString("Antworttyp '%1' ist fuer route=%2 nicht erlaubt. Erlaubt sind: %3.")
                    .arg(type.isEmpty() ? QStringLiteral("<leer>") : type,
                        m_lastAgentRoute.value("route").toString("<leer>"),
                        jsonStringArrayToStringList(routeAllowedResponseTypes(
                            m_lastAgentRoute.value("route").toString(),
                            toolsAvailableForRoute)).join(", ")))) {
            return;
        }
        appendAgentChat("Barebone-Qt", QString("AI Antworttyp '%1' passt nicht zur Route %2.")
            .arg(type.isEmpty() ? QStringLiteral("<leer>") : type,
                m_lastAgentRoute.value("route").toString("<leer>")));
        return;
    }

    if (type == "action_proposal") {
        if (!isBricsCadMode()) {
            clearAgentProposal();
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    "CAD-Aktionen sind nur im BricsCAD-Modus erlaubt.")) {
                return;
            }
            appendAgentChat("Barebone-Qt", "AI Vorschlag abgelehnt: Der BricsCAD-Modus ist nicht aktiv.");
            return;
        }
        if (!m_brxAuthenticated) {
            clearAgentProposal();
            appendAgentChat("Barebone-Qt", "BRX Plugin ist nicht verbunden. CAD-Aktionen koennen erst vorgeschlagen und validiert werden, wenn BricsCAD verbunden ist.");
            return;
        }
        QJsonObject proposal = normalizedAgentProposal(reply);
        if (!message.trimmed().isEmpty()) {
            const QString currentSummary = repairMojibakeText(proposal.value(QStringLiteral("summary")).toString(
                proposal.value(QStringLiteral("message")).toString())).trimmed();
            if (currentSummary.isEmpty()
                || isGenericPreparedActionText(currentSummary)
                || (!isGenericPreparedActionText(message) && message.size() > currentSummary.size())) {
                proposal.insert("summary", message.trimmed());
            }
            if (proposal.value(QStringLiteral("message")).toString().trimmed().isEmpty()) {
                proposal.insert(QStringLiteral("message"), message.trimmed());
            }
        }
        if (!sessionTitleSuggestion.isEmpty()) {
            proposal.insert(QStringLiteral("sessionTitleSuggestion"), sessionTitleSuggestion);
        }
        if (proposal.value(QStringLiteral("proposalId")).toString().trimmed().isEmpty()) {
            proposal.insert(QStringLiteral("proposalId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        }
        processAgentProposal(content, reply, proposal, sessionTitleSuggestion);
        return;

        // Unreachable legacy block retained temporarily for source compatibility
        // while old workflow formats are migrated. The AI-reviewed path above is
        // the only runtime path for new proposals.
        const bool workflowRunProposal = false;
        const QString proposalDisplaySummary = repairMojibakeText(proposal.value(QStringLiteral("summary")).toString(
            proposal.value(QStringLiteral("message")).toString())).trimmed();
        if (isGenericPreparedActionText(proposalDisplaySummary)
            && m_agentValidationRetries < 1
            && retryAgentAfterValidationFailure(
                content,
                reply,
                QStringLiteral(
                    "action_proposal braucht eine konkrete sichtbare Ausfuehrungsbeschreibung. "
                    "Setze top-level message und proposal.summary aus Nutzerprompt, Toolauswahl und Parametern; "
                    "bei mehreren Schritten setze proposal.details mit der Schrittfolge. "
                    "Verwende keine generische Meldung wie 'Der Agent hat eine BricsCAD-Aktion vorbereitet'."))) {
            return;
        }
        QString errorMessage;
        if (!validateAgentProposal(proposal, errorMessage)) {
            clearAgentProposal();
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    QString("%1 ist nicht gueltig: %2")
                        .arg(workflowRunProposal ? QStringLiteral("workflow_run_proposal") : QStringLiteral("action_proposal"),
                            errorMessage))) {
                return;
            }
            appendAgentChat("Barebone-Qt", QString("AI Vorschlag abgelehnt: %1").arg(errorMessage));
            return;
        }
        proposal.insert(QStringLiteral("requiresConfirmation"), proposalRequiresUserConfirmation(proposal));
        const QString normalizedOriginalPrompt = workflowTrainingSearchText(m_lastAgentUserPrompt);
        const bool originalPromptRequestsMutation = textMentionsAny(normalizedOriginalPrompt, {
            QStringLiteral("zeichne"), QStringLiteral("erstelle"), QStringLiteral("erzeuge"),
            QStringLiteral("einfueg"), QStringLiteral("einfÃƒÂ¼g"), QStringLiteral("verschieb"),
            QStringLiteral("kopier"), QStringLiteral("extrudi"), QStringLiteral("loesch"),
            QStringLiteral("lÃƒÂ¶sch"), QStringLiteral("setz"), QStringLiteral("zuweis")});
        if (!proposal.value(QStringLiteral("requiresConfirmation")).toBool(true)
            && originalPromptRequestsMutation
            && routeAllowsCadActions(m_lastAgentRoute)) {
            proposal.insert(QStringLiteral("continueAfterSuccess"), true);
            proposal.insert(QStringLiteral("nextIntent"), m_lastAgentUserPrompt);
            proposal.insert(QStringLiteral("continuationPolicy"), QStringLiteral(
                "Die Read-only-Aktion war nur Vorbereitung der urspruenglichen Mutation. Nutze ihr konkretes Ergebnis und liefere jetzt die mutierenden Aktionen; wiederhole dieselbe Abfrage nicht."));
        }
        const QJsonArray actions = agentProposalActions(proposal);
        if (actions.size() > 1) {
            appendBridgeLog(QString("AI Batch-Vorschlag: %1 Aktionen").arg(actions.size()));
        } else {
            const QJsonObject action = actions.isEmpty() ? QJsonObject{} : actions.first().toObject();
            appendBridgeLog(QString("AI Vorschlag: %1 params=%2")
                .arg(action.value("tool").toString(proposal.value("tool").toString()),
                    QString::fromUtf8(QJsonDocument(action.value("params").toObject(proposal.value("params").toObject())).toJson(QJsonDocument::Compact))));
        }
        if (!sessionTitleSuggestion.isEmpty()) {
            emitSessionTitleSuggestion(sessionTitleSuggestion);
        }
        if (!proposal.value(QStringLiteral("requiresConfirmation")).toBool(true)) {
            appendBridgeLog(QString("AI Read-only Vorschlag: fuehre %1 Tool(s) automatisch aus").arg(actions.size()));
            m_pendingAgentProposal = proposal;
            executeAgentProposal();
            return;
        }
        preflightAgentProposal(content, proposal, proposal);
        return;
    }

    if (type == "context_request") {
        if (!sessionTitleSuggestion.isEmpty()) {
            emitSessionTitleSuggestion(sessionTitleSuggestion);
        }
        discardLastAssistantConversation(content);
        handleAgentContextRequest(reply);
        return;
    }
    if (type == "ask_user") {
        const QJsonArray missing = reply.value("missing").toArray();
        if (message.trimmed().isEmpty()
            && retryAgentAfterValidationFailure(
                content,
                reply,
                QStringLiteral(
                    "ask_user.message fehlt. Formuliere eine kurze direkte deutsche Rueckfrage, die alle Werte aus missing konkret benennt und das erwartete Eingabeformat erklaert. "
                    "Uebernimm missing und draft inhaltlich, aber gib keinen generischen Wartetext aus."))) {
            return;
        }
        QString question = repairMojibakeText(message).trimmed();
        if (question.isEmpty()) {
            QStringList readableMissing;
            for (const QJsonValue& value : missing) {
                const QString field = value.toString().trimmed().toLower();
                if (field == QStringLiteral("center") || field == QStringLiteral("origin")) {
                    readableMissing << QStringLiteral("den Mittelpunkt bzw. Ursprung als x-, y- und z-Koordinate");
                } else if (field == QStringLiteral("point") || field == QStringLiteral("position")) {
                    readableMissing << QStringLiteral("die Position als x-, y- und z-Koordinate");
                } else if (!field.isEmpty()) {
                    readableMissing << repairMojibakeText(value.toString()).trimmed();
                }
            }
            question = readableMissing.isEmpty()
                ? QStringLiteral("Welche konkrete Angabe soll ich für die Ausführung verwenden?")
                : QStringLiteral("Bitte gib noch %1 an.").arg(readableMissing.join(QStringLiteral(", ")));
        }
        bool requestsToolAddition = message.contains("Tool", Qt::CaseInsensitive)
            && (message.contains("hinzuf", Qt::CaseInsensitive)
                || message.contains("add", Qt::CaseInsensitive));
        for (const QJsonValue& value : missing) {
            if (value.toString().compare(QStringLiteral("tool_addition"), Qt::CaseInsensitive) == 0) {
                requestsToolAddition = true;
                break;
            }
        }
        if (requestsToolAddition) {
            clearAgentProposal();
            m_pendingAgentDraft = {};
            appendAgentChat("Barebone-Qt", "Tool-Erweiterungen werden nicht per Chat angelegt. Die verbindliche Toolliste muss aus der BRX-Verbindung kommen; ich lade die Capabilities neu.");
            if (m_brxAuthenticated) {
                m_capabilitiesRequested = false;
                requestBridgeCapabilities();
            } else {
                appendAgentChat("Barebone-Qt", "BRX Plugin ist nicht verbunden. Bitte BareboneBrx.brx in BricsCAD laden, danach die Anfrage erneut senden.");
            }
            return;
        }
        m_pendingAgentDraft = reply.value("draft").toObject();
        if (m_pendingAgentDraft.contains("proposal")
            || m_pendingAgentDraft.contains("actions")
            || m_pendingAgentDraft.contains("tool")) {
            m_pendingAgentDraft = normalizedAgentProposal(m_pendingAgentDraft);
        }
        if (m_pendingAgentDraft.isEmpty()
            && routeAllowsCadActions(m_lastAgentRoute)) {
            m_pendingAgentDraft = QJsonObject{
                {"schema", "barebone.agent.pending_question.v1"},
                {"type", "ask_user"},
                {"message", message},
                {"missing", missing},
                {"route", m_lastAgentRoute},
            };
        }
        if (!m_pendingAgentDraft.isEmpty() && !m_lastAgentUserPrompt.isEmpty()) {
            m_pendingAgentDraft.insert("_sourcePrompt", m_lastAgentUserPrompt);
        }
        clearAgentProposal();
        if (!sessionTitleSuggestion.isEmpty()) {
            emitSessionTitleSuggestion(sessionTitleSuggestion);
        }
        // Rueckfragen sind normale, persistente AI-Nachrichten. Die rohe
        // ask_user-JSON-Antwort darf nicht zusaetzlich im Sitzungskontext bleiben.
        discardLastAssistantConversation(content);
        m_agentConversation.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("content"), question},
        });
        appendAgentChat(QStringLiteral("AI"), question, sessionTitleExtra);
        m_agentValidationRetries = 0;
        setAgentBusy(false);
        if (m_pendingAgentDraft.isEmpty()) {
            appendBridgeLog("AI Agent: Rueckfrage ohne Draft");
        } else {
            appendBridgeLog("AI Agent: Rueckfrage mit Draft gespeichert");
        }
        return;
    }

    if (type == "message") {
        if (!message.isEmpty()) {
            if (!sessionTitleSuggestion.isEmpty()) {
                emitSessionTitleSuggestion(sessionTitleSuggestion);
            }
            QVariantMap messageExtra = sessionTitleExtra;
            if (m_lastAgentRoute.value(QStringLiteral("route")).toString()
                == QStringLiteral("execution_summary")) {
                const QVariantMap executionExtra = bricsCadExecutionMessageExtra();
                for (auto it = executionExtra.cbegin(); it != executionExtra.cend(); ++it) {
                    messageExtra.insert(it.key(), it.value());
                }
            }
            appendAgentChat("AI", message, messageExtra);
        }
        if (!m_pendingAgentProposal.isEmpty()) {
            appendBridgeLog("AI Agent: offener Vorschlag verworfen, AI hat keinen aktualisierten Vorschlag geliefert");
            clearAgentProposal();
        }
        m_agentValidationRetries = 0;
        return;
    }

    if (type == "plan") {
        m_pendingAgentDraft = reply;
        clearAgentProposal();
        appendBridgeLog(QString("AI Agent: Plan missing=%1")
            .arg(QString::fromUtf8(QJsonDocument(reply.value("missingCapabilities").toArray()).toJson(QJsonDocument::Compact))));
        if (!message.isEmpty()) {
            if (!sessionTitleSuggestion.isEmpty()) {
                emitSessionTitleSuggestion(sessionTitleSuggestion);
            }
            appendAgentChat("AI", message, sessionTitleExtra);
        }
        return;
    }

    if (retryAgentAfterValidationFailure(
            content,
            reply,
            QString("Unbekannter oder nicht erlaubter Antworttyp '%1'. Nutze nur message, ask_user, context_request oder action_proposal.").arg(rawType.isEmpty() ? "<leer>" : rawType))) {
        return;
    }
    appendAgentChat("Barebone-Qt", QString("Unbekannter AI Antworttyp: %1").arg(rawType.isEmpty() ? "<leer>" : rawType));
}

void BricsCadPage::processAgentProposal(
    const QString& rejectedContent,
    const QJsonObject& rejectedReply,
    QJsonObject proposal,
    const QString& sessionTitleSuggestion)
{
    QString errorMessage;
    if (!validateAgentProposal(proposal, errorMessage)) {
        clearAgentProposal();
        if (retryAgentAfterValidationFailure(
                rejectedContent,
                rejectedReply,
                QStringLiteral("Qt Struktur-/Capability-Pruefung fehlgeschlagen: %1").arg(errorMessage))) {
            return;
        }
        appendAgentChat(QStringLiteral("Barebone-Qt"),
            QStringLiteral("AI Vorschlag abgelehnt: %1").arg(errorMessage));
        return;
    }

    proposal.insert(QStringLiteral("requiresConfirmation"), proposalRequiresUserConfirmation(proposal));
    const QJsonArray actions = agentProposalActions(proposal);
    QStringList toolNames;
    for (const QJsonValue& value : actions) {
        toolNames << value.toObject().value(QStringLiteral("tool")).toString();
    }
    appendBridgeLog(QStringLiteral("AI Vorschlag strukturell geprueft: actions=%1 tools=%2")
        .arg(actions.size())
        .arg(toolNames.join(QStringLiteral(","))));

    if (!sessionTitleSuggestion.isEmpty()) {
        emitSessionTitleSuggestion(sessionTitleSuggestion);
    }
    if (!m_activeReasoningRunId.isEmpty()) {
        emitWebReasoningProgress(m_agentBridge, QVariantMap{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.reasoning-progress.v1")},
            {QStringLiteral("runId"), m_activeReasoningRunId},
            {QStringLiteral("stageId"), QStringLiteral("final-run")},
            {QStringLiteral("state"), QStringLiteral("succeeded")},
            {QStringLiteral("revision"), 4},
            {QStringLiteral("label"), QStringLiteral("Finale Auswertung")},
            {QStringLiteral("message"), QStringLiteral("Vorschlag technisch vorbereitet")},
        });
        m_activeReasoningRunId.clear();
    }
    if (!proposal.value(QStringLiteral("requiresConfirmation")).toBool(true)) {
        appendBridgeLog(QStringLiteral("AI Read-only Vorschlag: fuehre %1 Tool(s) automatisch aus").arg(actions.size()));
        m_pendingAgentProposal = proposal;
        executeAgentProposal();
        return;
    }
    preflightAgentProposal(rejectedContent, proposal, proposal);
}

void BricsCadPage::discardLastAssistantConversation(const QString& content)
{
    if (m_agentConversation.isEmpty()) {
        return;
    }

    const QJsonObject last = m_agentConversation.last().toObject();
    if (last.value("role").toString() == "assistant"
        && last.value("content").toString() == content) {
        m_agentConversation.removeLast();
    }
}

bool BricsCadPage::retryAgentAfterValidationFailure(
    const QString& rejectedContent,
    const QJsonObject& rejectedObject,
    const QString& errorMessage)
{
    discardLastAssistantConversation(rejectedContent);

    const QJsonObject repairGuidance;
    const QString rejectedSignature = QString::fromUtf8(QJsonDocument(rejectedObject).toJson(QJsonDocument::Compact))
        .trimmed()
        .left(1800);
    const bool repeatedResponse = !rejectedSignature.isEmpty()
        && m_agentRejectedResponseSignatures.contains(rejectedSignature);
    if (!rejectedSignature.isEmpty()) {
        m_agentRejectedResponseSignatures << rejectedSignature;
        while (m_agentRejectedResponseSignatures.size() > 8) {
            m_agentRejectedResponseSignatures.removeFirst();
        }
    }

    if (repeatedResponse) {
        appendBridgeLog(QString("AI Agent Loop: identische abgelehnte Antwort gestoppt: %1")
            .arg(errorMessage.left(260)));
        return false;
    }

    if (m_agentValidationRetries >= kMaxAgentRepairRetries) {
        appendBridgeLog(QString("AI Agent Loop: abgebrochen nach %1 Versuchen: %2")
            .arg(kMaxAgentRepairRetries)
            .arg(errorMessage));
        return false;
    }

    ++m_agentValidationRetries;
    appendBridgeLog(QString("AI Agent Loop %1/%2: %3")
        .arg(m_agentValidationRetries)
        .arg(kMaxAgentRepairRetries)
        .arg(errorMessage.left(260)));

    QJsonObject envelope = agentRequestEnvelope(m_lastAgentUserPrompt.isEmpty()
        ? QStringLiteral("Korrigiere deine letzte Antwort.")
        : m_lastAgentUserPrompt,
        m_lastDocumentContext,
        normalizedAgentRouteForMode(m_lastAgentRoute, m_lastAgentUserPrompt, m_lastDocumentContext, m_chatMode));
    envelope.insert("type", "validation_error");
    envelope.insert("validationError", errorMessage);
    if (!repairGuidance.isEmpty()) {
        envelope.insert(QStringLiteral("repairGuidance"), repairGuidance);
    }
    const QJsonObject failedAction = firstRepairActionFromObject(rejectedObject);
    envelope.insert("rejectedContent", rejectedContent.left(800));
    if (!failedAction.isEmpty()) {
        envelope.insert(QStringLiteral("rejectedAction"), failedAction);
    } else if (!rejectedObject.isEmpty()) {
        envelope.insert(QStringLiteral("rejectedResponseBrief"), QJsonObject{
            {QStringLiteral("schema"), rejectedObject.value(QStringLiteral("schema")).toString()},
            {QStringLiteral("type"), rejectedObject.value(QStringLiteral("type")).toString()},
            {QStringLiteral("message"), rejectedObject.value(QStringLiteral("message")).toString().left(500)},
        });
    }
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentRepairRetries},
        {"repeatedRejectedResponse", repeatedResponse},
        {"policy", "Your previous response was not shown to the user. Correct it using the available tools and schemas."},
    });

    const QJsonArray allRepairTools = availableAgentTools();
    QJsonObject compactRepairMode{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.repair-context.compact.v1")},
        {QStringLiteral("retry"), m_agentValidationRetries},
        {QStringLiteral("maxRetries"), kMaxAgentRepairRetries},
        {QStringLiteral("validationError"), errorMessage.left(900)},
        {QStringLiteral("policy"), QStringLiteral(
            "Pruefe die Aufgabe aus originalUserPrompt und relevantem Verlauf neu. Der Fehler beschreibt nur den abgelehnten Pfad; "
            "Qt schreibt keine fachliche Ersatzloesung vor.")},
        {QStringLiteral("drawingContext"), drawingContextStore().inspect(QJsonObject{
            {QStringLiteral("limit"), 8},
        })},
    };
    envelope.insert(QStringLiteral("repairMode"), compactRepairMode);
    const QJsonArray augmentedTools = mergeToolArraysByName(
        envelope.value(QStringLiteral("effectiveTools")).toArray(),
        allRepairTools);
    QJsonArray boundedTools = augmentedTools;
    while (boundedTools.size() > 16) {
        boundedTools.removeAt(boundedTools.size() - 1);
    }
    envelope.insert(QStringLiteral("effectiveTools"), boundedTools);

    QString retryInstruction =
        QStringLiteral("Korrigiere deine letzte Antwort. Antworte ausschliesslich mit einem gueltigen JSON-Objekt. "
        "Nutze Barebone-Agent-JSON v2 mit schema=\"barebone.agent.response.v2\". "
        "Setze sessionTitle direkt auf Top-Level des Antwortobjekts. "
        "Nutze keinen freien Plan und keine Pseudo-Actions. Wenn die Aufgabe ausfuehrbar ist und die Parameter bekannt sind, liefere genau einen action_proposal mit einem direkt ausfuehrbaren proposal.actions[] Batch. "
        "Korrigiere intern und plane die Aufgabe aus originalUserPrompt, relevantem Verlauf, effectiveTools und aktuellem Zeichnungskontext neu. "
        "Der Validatorfehler ist Diagnose, keine fachliche Qt-Vorgabe. Wiederhole nicht denselben abgelehnten Tool-/Param-Pfad. "
        "Direkte BricsCAD-DB-Schreibvorgaenge, AcDb-/LayerTable-/EntityTable-Mutationen und Pseudo-Tools fuer DB-Writes sind verboten; nutze ausschliesslich effectiveTools[].name. "
        "Wenn mehrere Aktionen mit bekannten Parametern erforderlich sind, liefere ein action_proposal mit proposal.actions:[{\"tool\":\"...\",\"params\":{...}},...] und proposal.continueAfterSuccess=false. "
        "Nutze continueAfterSuccess nicht, um Batch-Aufgaben wie mehrere Layer oder mehrere gleichartige Objekte einzeln nachzufordern. "
        "tool muss exakt einem effectiveTools[].name entsprechen. params muessen inputSchema/apiDoc.post erfuellen. "
        "Interpretiere Massangaben x/y/z als Breite/Laenge/Hoehe und mappe sie auf die exakten Schemafelder width/depth/height. "
        "x/y/z innerhalb von origin, point, position, coordinates, center oder vector sind dagegen Koordinaten und duerfen nicht als Abmessungen umgedeutet werden. "
        "Bevor du ask_user verwendest, pruefe, ob ein angeblich fehlender Wert bereits im originalUserPrompt oder unter einem abgelehnten Alias vorhanden ist; mappe ihn dann auf das Schemafeld, statt erneut danach zu fragen. "
        "Wenn validationError mit BRX Preflight beginnt, wiederhole nicht denselben Vorschlag; nutze die dort genannten Fehler, fehlenden Daten und Hinweise verbindlich. ");
    if (repeatedResponse) {
        retryInstruction += QStringLiteral("Deine letzte Antwort war strukturell identisch zu einer bereits abgelehnten Antwort. Aendere Toolwahl, Params, stepPlan oder frage gezielt nach; dieselbe Antwort ist verboten. ");
    }
    retryInstruction += QStringLiteral(
        "Wenn du Sitzungsverlauf brauchst, nutze context_request mit einer Methode aus conversationAccess.allowedMethods. "
        "Wenn du Zeichnungskontext brauchst, nutze context_request mit exakt einer readOnlyMethods[].name Methode. ");
    if (routeAllowsCadActions(m_lastAgentRoute)) {
        retryInstruction += QStringLiteral(
            "Wenn nach Workflow- und Nutzerwerten echte ungebundene Pflichtinformationen fehlen, nutze ask_user mit missing und einem draft. "
            "Wenn nach erneuter Pruefung keine Aktion sinnvoll oder technisch moeglich ist, darfst du message oder plan liefern; behaupte keine Ausfuehrung.");
    } else {
        retryInstruction += QStringLiteral(
            "Wenn echte Informationen fehlen, nutze ask_user mit missing und einem draft. Wenn die Anfrage allgemein ist, nutze type=message.");
    }
    envelope.insert("instruction", retryInstruction);

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, QString("agent_loop_%1").arg(m_agentValidationRetries));
    return true;
}

QJsonObject BricsCadPage::normalizedAgentProposal(const QJsonObject& proposal) const
{
    QJsonObject normalized = proposal.value("proposal").toObject();
    if (normalized.isEmpty()) {
        normalized = proposal;
    } else {
        const QString message = repairMojibakeText(proposal.value("message").toString()).trimmed();
        if (!message.isEmpty()) {
            normalized.insert("message", message);
            const QString existingSummary = repairMojibakeText(normalized.value("summary").toString(
                normalized.value("message").toString())).trimmed();
            if (existingSummary.isEmpty()
                || isGenericPreparedActionText(existingSummary)
                || (!isGenericPreparedActionText(message) && message.size() > existingSummary.size())) {
                normalized.insert("summary", message);
            }
        }
        const QString details = repairMojibakeText(proposal.value(QStringLiteral("details")).toString()).trimmed();
        if (!details.isEmpty() && normalized.value(QStringLiteral("details")).toString().trimmed().isEmpty()) {
            normalized.insert(QStringLiteral("details"), details);
        }
        if (proposal.contains("assumptions")) {
            normalized.insert("assumptions", proposal.value("assumptions"));
        }
        if (proposal.contains("schema")) {
            normalized.insert("schema", proposal.value("schema"));
        }
    }

    const QJsonArray actions = normalized.value("actions").toArray();
    if (!actions.isEmpty()) {
        QJsonArray normalizedActions;
        for (const QJsonValue& value : actions) {
            if (value.isObject()) {
                normalizedActions.append(normalizedAgentAction(value.toObject()));
            }
        }
        normalized.insert(QStringLiteral("actions"), normalizedActions);
        normalized.remove(QStringLiteral("tool"));
        normalized.remove(QStringLiteral("params"));
        return normalized;

        // Legacy semantic materialization below is unreachable.
        const bool networkRouteWorkflow = isSystemRouteWorkflowId(m_selectedWorkflowId)
            && m_selectedWorkflowId != QStringLiteral("workflow_05_system_pipe_contours");
        if (networkRouteWorkflow) {
            int pointCount = 0;
            int polylineCount = 0;
            QJsonArray canonicalPolylineParams;
            QJsonArray canonicalPointQueryParams;
            QJsonArray canonicalValidationParams;
            for (const QJsonValue& batchValue : m_selectedWorkflow.value(QStringLiteral("executionBatches")).toArray()) {
                for (const QJsonValue& stepValue : batchValue.toObject().value(QStringLiteral("steps")).toArray()) {
                    const QJsonObject step = stepValue.toObject();
                    const QJsonObject params = step.value(QStringLiteral("paramsTemplate")).toObject();
                    if (step.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.create")
                        && params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("polyline"), Qt::CaseInsensitive) == 0) {
                        canonicalPolylineParams.append(params);
                    } else if (step.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.query")
                        && params.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)) {
                        canonicalPointQueryParams.append(params);
                    } else if (step.value(QStringLiteral("tool")).toString() == QStringLiteral("pipes.validateNetwork")) {
                        canonicalValidationParams.append(params);
                    }
                }
            }
            for (const QJsonValue& actionValue : normalizedActions) {
                const QJsonObject action = actionValue.toObject();
                const QJsonObject params = action.value(QStringLiteral("params")).toObject();
                if (action.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.create")) {
                    if (params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("point"), Qt::CaseInsensitive) == 0) {
                        ++pointCount;
                    } else if (params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("polyline"), Qt::CaseInsensitive) == 0) {
                        ++polylineCount;
                    }
                }
            }
            QJsonArray pointIndexes;
            for (int i = 0; i < pointCount; ++i) pointIndexes.append(i);
            QJsonArray polylineIndexes;
            for (int i = 0; i < polylineCount; ++i) polylineIndexes.append(i);
            int polylineOrdinal = 0;
            int pointQueryOrdinal = 0;
            int validationOrdinal = 0;
            for (int i = 0; i < normalizedActions.size(); ++i) {
                QJsonObject action = normalizedActions.at(i).toObject();
                QJsonObject params = action.value(QStringLiteral("params")).toObject();
                const QString tool = action.value(QStringLiteral("tool")).toString();
                if (tool == QStringLiteral("geometry.query") && pointCount > 0) {
                    params.insert(QStringLiteral("autoPointHandlesFromBatch"), true);
                    params.insert(QStringLiteral("createdPointHandleIndexes"), pointQueryOrdinal < canonicalPointQueryParams.size()
                        ? canonicalPointQueryParams.at(pointQueryOrdinal).toObject().value(QStringLiteral("createdPointHandleIndexes")).toArray()
                        : pointIndexes);
                    ++pointQueryOrdinal;
                } else if (tool == QStringLiteral("geometry.create")
                    && params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("polyline"), Qt::CaseInsensitive) == 0) {
                    params.insert(QStringLiteral("autoPointsFromLastQuery"), true);
                    if (polylineOrdinal < canonicalPolylineParams.size()) {
                        params.insert(QStringLiteral("queriedPointIndexes"), canonicalPolylineParams.at(polylineOrdinal).toObject()
                            .value(QStringLiteral("queriedPointIndexes")).toArray());
                    }
                    ++polylineOrdinal;
                } else if (tool == QStringLiteral("pipes.validateNetwork") && polylineCount > 0) {
                    params.insert(QStringLiteral("autoPolylineHandlesFromBatch"), true);
                    if (validationOrdinal < canonicalValidationParams.size()) {
                        const QJsonObject canonicalParams = canonicalValidationParams.at(validationOrdinal).toObject();
                        params.insert(QStringLiteral("createdPolylineHandleIndexes"), canonicalParams.value(QStringLiteral("createdPolylineHandleIndexes")).toArray());
                        params.insert(QStringLiteral("minimumClearanceMm"), canonicalParams.value(QStringLiteral("minimumClearanceMm")));
                        params.insert(QStringLiteral("avoidLayers"), canonicalParams.value(QStringLiteral("avoidLayers")));
                    } else {
                        params.insert(QStringLiteral("createdPolylineHandleIndexes"), polylineIndexes);
                        params.insert(QStringLiteral("minimumClearanceMm"),
                            m_selectedWorkflowId == QStringLiteral("workflow_06_ventilation_route") ? 400 : 100);
                    }
                    ++validationOrdinal;
                }
                action.insert(QStringLiteral("params"), params);
                normalizedActions.replace(i, action);
            }
        }

        const QString normalizedPrompt = workflowTrainingSearchText(m_lastAgentUserPrompt);
        const bool explicitSelectionRequested = textMentionsAny(normalizedPrompt, {
            QStringLiteral("selekt"), QStringLiteral("auswaehl"), QStringLiteral("auswahl"),
            QStringLiteral("select"), QStringLiteral("markier")});
        QJsonArray materializedActions;
        for (int i = 0; i < normalizedActions.size(); ++i) {
            QJsonObject action = normalizedActions.at(i).toObject();
            const QString tool = action.value(QStringLiteral("tool")).toString();
            const bool extrusion = tool == QStringLiteral("rectangles.extrude")
                || tool == QStringLiteral("circle.extrude")
                || tool == QStringLiteral("profile.extrude");

            if (extrusion && explicitSelectionRequested && !materializedActions.isEmpty()) {
                const QString previousTool = materializedActions.last().toObject()
                    .value(QStringLiteral("tool")).toString();
                if (previousTool == QStringLiteral("geometry.create")) {
                    materializedActions.append(QJsonObject{
                        {QStringLiteral("tool"), QStringLiteral("selection.set")},
                        {QStringLiteral("params"), QJsonObject{
                            {QStringLiteral("selector"), QJsonObject{
                                {QStringLiteral("scope"), QStringLiteral("lastResult")}}},
                            {QStringLiteral("saveBefore"), false}}},
                        {QStringLiteral("reason"), QStringLiteral("Neu erzeugte Geometrie wie vom Nutzer verlangt selektieren")},
                    });
                    QJsonObject params = action.value(QStringLiteral("params")).toObject();
                    params.insert(QStringLiteral("selector"), QJsonObject{
                        {QStringLiteral("scope"), QStringLiteral("selection")}});
                    action.insert(QStringLiteral("params"), params);
                }
            }

            materializedActions.append(action);
            if (!extrusion) {
                continue;
            }

            const QString targetLayer = action.value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("layer")).toString().trimmed();
            const QJsonObject nextAction = i + 1 < normalizedActions.size()
                ? normalizedActions.at(i + 1).toObject() : QJsonObject{};
            const QJsonObject nextParams = nextAction.value(QStringLiteral("params")).toObject();
            const bool matchingLayerFollowUp = nextAction.value(QStringLiteral("tool")).toString()
                    == QStringLiteral("entity.setLayer")
                && nextParams.value(QStringLiteral("layer")).toString().trimmed()
                    .compare(targetLayer, Qt::CaseInsensitive) == 0
                && nextParams.value(QStringLiteral("selector")).toObject()
                    .value(QStringLiteral("scope")).toString()
                    .compare(QStringLiteral("lastResult"), Qt::CaseInsensitive) == 0;
            if (!targetLayer.isEmpty() && !matchingLayerFollowUp) {
                materializedActions.append(QJsonObject{
                    {QStringLiteral("tool"), QStringLiteral("entity.setLayer")},
                    {QStringLiteral("params"), QJsonObject{
                        {QStringLiteral("selector"), QJsonObject{
                            {QStringLiteral("scope"), QStringLiteral("lastResult")}}},
                        {QStringLiteral("layer"), targetLayer},
                        {QStringLiteral("saveBefore"), false}}},
                    {QStringLiteral("reason"), QStringLiteral("Extrusion deterministisch dem Ziellayer zuweisen")},
                });
            }
        }
        normalizedActions = materializedActions;
        normalized.insert("actions", normalizedActions);
        normalized.insert("requiresConfirmation", true);
        normalized.insert("continueAfterSuccess", false);
        normalized.remove("nextIntent");
        return normalized;
    }

    const QJsonObject action = normalizedAgentAction(proposal);
    normalized.insert("tool", action.value("tool"));
    normalized.insert("params", action.value("params").toObject());
    normalized.insert("requiresConfirmation", true);
    return normalized;
}

QJsonObject BricsCadPage::floorPlanBatchProposalFromCalculationResult(const QJsonObject& baseProposal) const
{
    const QString workflowSource = m_lastAgentRoute.value(QStringLiteral("workflowSource")).toString();
    if (workflowSource != QStringLiteral("manual")
        && workflowSource != QStringLiteral("explicitPrompt")) {
        return {};
    }

    bool floorPlanWorkflowActive = false;
    for (const QJsonValue& value : selectedWorkflowObjectsForRoute(m_lastAgentRoute)) {
        if (value.toObject().value(QStringLiteral("id")).toString()
            == QStringLiteral("grundriss_und_aussenwaende_einzeichnen")) {
            floorPlanWorkflowActive = true;
            break;
        }
    }
    if (!floorPlanWorkflowActive) {
        return {};
    }

    const QJsonObject calculation = m_lastAgentRoute.value(QStringLiteral("calculationResult")).toObject();
    if (!calculation.value(QStringLiteral("readyForExecution")).toBool(false)
        || calculation.value(QStringLiteral("contour")).toObject().isEmpty()
        || calculation.value(QStringLiteral("wallBoxes")).toArray().size() != 4) {
        return {};
    }

    for (const QString& requiredTool : {
             QStringLiteral("geometry.create"),
             QStringLiteral("geometry.query"),
             QStringLiteral("measurement.area"),
             QStringLiteral("measurement.bbox"),
         }) {
        if (toolDefinition(requiredTool).isEmpty()) {
            return {};
        }
    }

    const QJsonObject contour = calculation.value(QStringLiteral("contour")).toObject();
    const QJsonObject origin = contour.value(QStringLiteral("origin")).toObject();
    const double width = contour.value(QStringLiteral("width")).toDouble(0.0);
    const double depth = contour.value(QStringLiteral("depth")).toDouble(0.0);
    const QString layer = contour.value(QStringLiteral("layer")).toString(QStringLiteral("0"));
    if (width <= 0.0 || depth <= 0.0) {
        return {};
    }

    auto action = [](const QString& tool, const QJsonObject& params, const QString& reason = QString()) {
        QJsonObject object{
            {QStringLiteral("tool"), tool},
            {QStringLiteral("params"), params},
        };
        if (!reason.trimmed().isEmpty()) {
            object.insert(QStringLiteral("reason"), reason.trimmed());
        }
        return object;
    };

    QJsonArray actions;
    actions.append(action(QStringLiteral("geometry.create"), QJsonObject{
        {QStringLiteral("geometry"), QStringLiteral("rectangle")},
        {QStringLiteral("origin"), origin},
        {QStringLiteral("width"), width},
        {QStringLiteral("depth"), depth},
        {QStringLiteral("layer"), layer},
        {QStringLiteral("saveBefore"), true},
    }, QStringLiteral("Innenkontur der freien Hausflaeche erstellen")));
    actions.append(action(QStringLiteral("geometry.query"), QJsonObject{
        {QStringLiteral("selector"), QJsonObject{
            {QStringLiteral("scope"), QStringLiteral("lastResult")},
            {QStringLiteral("kind"), QStringLiteral("polyline")},
            {QStringLiteral("shape"), QStringLiteral("rectangle")},
        }},
        {QStringLiteral("include"), QJsonArray{
            QStringLiteral("geometry"),
            QStringLiteral("metrics"),
            QStringLiteral("dimensions"),
        }},
        {QStringLiteral("limit"), 1},
    }, QStringLiteral("Neue Innenkontur eindeutig abfragen")));
    actions.append(action(QStringLiteral("measurement.area"), QJsonObject{
        {QStringLiteral("autoRoomHandlesFromLastQuery"), true},
    }, QStringLiteral("Innenflaeche messen")));
    actions.append(action(QStringLiteral("measurement.bbox"), QJsonObject{
        {QStringLiteral("autoRoomHandlesFromLastQuery"), true},
    }, QStringLiteral("Innenmasse pruefen")));

    for (const QJsonValue& value : calculation.value(QStringLiteral("wallBoxes")).toArray()) {
        const QJsonObject box = value.toObject();
        actions.append(action(QStringLiteral("geometry.create"), QJsonObject{
            {QStringLiteral("geometry"), QStringLiteral("box")},
            {QStringLiteral("origin"), box.value(QStringLiteral("origin")).toObject()},
            {QStringLiteral("width"), box.value(QStringLiteral("width")).toDouble()},
            {QStringLiteral("depth"), box.value(QStringLiteral("depth")).toDouble()},
            {QStringLiteral("height"), box.value(QStringLiteral("height")).toDouble()},
            {QStringLiteral("layer"), box.value(QStringLiteral("layer")).toString(layer)},
            {QStringLiteral("saveBefore"), false},
        }, QStringLiteral("Aussenwand ausserhalb der geprueften Innenkontur erzeugen")));
    }

    QJsonArray createdGeometryHandleIndexes;
    for (int i = 0; i < 5; ++i) {
        createdGeometryHandleIndexes.append(i);
    }
    actions.append(action(QStringLiteral("geometry.query"), QJsonObject{
        {QStringLiteral("autoCreatedGeometryHandlesFromBatch"), true},
        {QStringLiteral("createdGeometryHandleIndexes"), createdGeometryHandleIndexes},
        {QStringLiteral("inspection"), QStringLiteral("createdGeometryBatch")},
        {QStringLiteral("selector"), QJsonObject{
            {QStringLiteral("scope"), QStringLiteral("handles")},
            {QStringLiteral("kind"), QStringLiteral("entity")},
        }},
        {QStringLiteral("include"), QJsonArray{
            QStringLiteral("geometry"),
            QStringLiteral("metrics"),
            QStringLiteral("dimensions"),
        }},
        {QStringLiteral("limit"), 5},
    }, QStringLiteral("Alle erzeugten Geometrien nach der Erstellung pruefen")));

    QJsonObject proposal = baseProposal;
    proposal.insert(QStringLiteral("summary"), QStringLiteral(
        "Ich erstelle die Innenkontur, pruefe Flaeche und Innenmasse, erzeuge vier Aussenwaende und pruefe danach alle fuenf erzeugten Geometrien."));
    proposal.insert(QStringLiteral("requiresConfirmation"), true);
    proposal.insert(QStringLiteral("continueAfterSuccess"), false);
    proposal.insert(QStringLiteral("workflowBatchMode"),
        QStringLiteral("floor_plan_interior_verification_and_walls_v1"));
    proposal.insert(QStringLiteral("workflowVerificationGate"), QJsonObject{
        {QStringLiteral("afterAction"), 4},
        {QStringLiteral("blocksFollowingActionsOnFailure"), true},
        {QStringLiteral("tools"), QJsonArray{
            QStringLiteral("geometry.query"),
            QStringLiteral("measurement.area"),
            QStringLiteral("measurement.bbox"),
        }},
    });
    proposal.insert(QStringLiteral("calculation"), calculation);
    proposal.insert(QStringLiteral("actions"), actions);
    proposal.remove(QStringLiteral("tool"));
    proposal.remove(QStringLiteral("params"));
    proposal.remove(QStringLiteral("nextIntent"));
    proposal.remove(QStringLiteral("workflowStage"));
    proposal.remove(QStringLiteral("continuationPolicy"));
    return proposal;
}

bool promptPrimarilyRequestsLayerMutation(const QString& prompt)
{
    const QString normalized = workflowTrainingSearchText(prompt);
    const bool mentionsLayer = textMentionsAny(normalized, {
        QStringLiteral("layer"),
        QStringLiteral("layers"),
        QStringLiteral("ebene"),
        QStringLiteral("layers.create"),
    });
    const bool mutationVerb = textMentionsAny(normalized, {
        QStringLiteral("erstelle"),
        QStringLiteral("erstellen"),
        QStringLiteral("anlegen"),
        QStringLiteral("lege"),
        QStringLiteral("erzeuge"),
        QStringLiteral("neu"),
        QStringLiteral("create"),
    });
    const bool mentionsGeometryTarget = textMentionsAny(normalized, {
        QStringLiteral("rechteck"),
        QStringLiteral("linie"),
        QStringLiteral("kreis"),
        QStringLiteral("polyline"),
        QStringLiteral("geometrie"),
        QStringLiteral("wand"),
        QStringLiteral("waende"),
        QStringLiteral("grundriss"),
        QStringLiteral("solid"),
        QStringLiteral("box"),
        QStringLiteral("extrusion"),
    });
    return mentionsLayer && mutationVerb && !mentionsGeometryTarget;
}

QString promptProposalConflictMessage(const QString& prompt, const QJsonArray& actions)
{
    if (!promptPrimarilyRequestsLayerMutation(prompt)) {
        return {};
    }

    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString(
            action.value(QStringLiteral("name")).toString()).trimmed();
        if (tool == QStringLiteral("geometry.create")
            || tool == QStringLiteral("rectangles.extrude")
            || tool == QStringLiteral("circle.extrude")
            || tool == QStringLiteral("profile.extrude")) {
            return QStringLiteral(
                "Prompt fordert eine Layer-Aktion, keine Geometrieerstellung. "
                "Liefere ein korrigiertes action_proposal mit layers.create/layers.ensureMany und ohne geometry.create.");
        }
    }
    return {};
}

QJsonObject BricsCadPage::normalizedAgentAction(const QJsonObject& action) const
{
    QJsonObject normalized = action;
    if (normalized.value("tool").toString().trimmed().isEmpty()
        && !normalized.value("name").toString().trimmed().isEmpty()) {
        normalized.insert("tool", normalized.value("name").toString().trimmed());
    }

    QJsonObject params;
    if (normalized.value("params").isObject()) {
        params = normalized.value("params").toObject();
    } else if (normalized.value("arguments").isObject()) {
        params = normalized.value("arguments").toObject();
    } else if (normalized.value("input").isObject()) {
        params = normalized.value("input").toObject();
    }

    QString tool = normalized.value("tool").toString().trimmed();
    const QJsonObject inputSchema = toolDefinition(tool).value("inputSchema").toObject();
    const QJsonObject schemaProperties = inputSchema.value(QStringLiteral("properties")).toObject();
    const QJsonArray required = inputSchema.value("required").toArray();
    for (const QJsonValue& value : required) {
        const QString key = value.toString();
        if (!key.isEmpty()
            && !params.contains(key)
            && normalized.contains(key)
            && !normalized.value(key).isUndefined()
            && !normalized.value(key).isNull()) {
            params.insert(key, normalized.value(key));
        }
    }

    // Structural unit/axis aliases only: values and action order remain unchanged.
    // Dimension axes are top-level values; coordinates remain nested in origin,
    // point, position, coordinates, center or vector and are never rewritten.
    const auto moveDimensionAlias = [&params, &schemaProperties](const QString& source, const QString& target) {
        if (!params.contains(source) || params.contains(target) || !schemaProperties.contains(target)) {
            return;
        }
        params.insert(target, params.value(source));
        if (!schemaProperties.contains(source)) {
            params.remove(source);
        }
    };
    const QString depthField = schemaProperties.contains(QStringLiteral("depth"))
        ? QStringLiteral("depth") : QStringLiteral("length");

    // Apply the axis convention to every dimension-bearing tool contract.
    // Selector/vector/point tools have no width/depth/height fields and are
    // therefore unaffected by this schema gate.
    moveDimensionAlias(QStringLiteral("widthMm"), QStringLiteral("width"));
    moveDimensionAlias(QStringLiteral("depthMm"), depthField);
    moveDimensionAlias(QStringLiteral("lengthMm"), depthField);
    moveDimensionAlias(QStringLiteral("x"), QStringLiteral("width"));
    moveDimensionAlias(QStringLiteral("y"), depthField);

    if (tool == QStringLiteral("geometry.create")) {
        const QString geometry = params.value(QStringLiteral("geometry")).toString().trimmed().toLower();

        const bool rectangle = geometry == QStringLiteral("rectangle");
        if (rectangle) {
            // In a 2D rectangle the second dimension is the Y length/depth;
            // "heightMm" is a common model alias, not an extrusion height.
            moveDimensionAlias(QStringLiteral("heightMm"), depthField);
        } else {
            moveDimensionAlias(QStringLiteral("heightMm"), QStringLiteral("height"));
        }
        if (!rectangle) {
            moveDimensionAlias(QStringLiteral("z"), QStringLiteral("height"));
        }
    } else {
        moveDimensionAlias(QStringLiteral("heightMm"), QStringLiteral("height"));
        moveDimensionAlias(QStringLiteral("z"), QStringLiteral("height"));
    }

    normalized.insert(QStringLiteral("tool"), tool);
    normalized.insert(QStringLiteral("params"), params);
    return normalized;

    // Legacy parameter guessing below is unreachable. Compatibility now only
    // maps field names and never changes tool meaning or parameter values.
    if (tool == "geometry.create"
        && params.value("geometry").toString().compare(QStringLiteral("rectangle"), Qt::CaseInsensitive) == 0
        && params.contains("height")
        && !params.contains("depth")
        && !params.contains("length")
        && !params.contains("depthMm")
        && !params.contains("lengthMm")
        && !params.contains("y")) {
        params.insert("depth", params.value("height"));
        params.remove("height");
    }

    if ((tool == QStringLiteral("rectangles.extrude")
            || tool == QStringLiteral("circle.extrude")
            || tool == QStringLiteral("profile.extrude"))
        && !params.contains(QStringLiteral("heightMm"))) {
        if (params.contains(QStringLiteral("z"))) {
            params.insert(QStringLiteral("heightMm"), params.value(QStringLiteral("z")));
        } else if (params.contains(QStringLiteral("height"))) {
            params.insert(QStringLiteral("heightMm"), params.value(QStringLiteral("height")));
        }
    }

    if (tool == QStringLiteral("layers.create")) {
        const QString originalName = params.value(QStringLiteral("name")).toString().trimmed();
        if (!promptExplicitlyNamesLayer(m_lastAgentUserPrompt, originalName)) {
            const QString normalizedName = normalizedBricsCadLayerName(originalName);
            if (!normalizedName.isEmpty()) {
                params.insert(QStringLiteral("name"), normalizedName);
            }
        } else {
            params.insert(QStringLiteral("name"), originalName);
        }
    } else if (tool == QStringLiteral("layers.ensureMany")) {
        const QJsonArray layers = params.value(QStringLiteral("layers")).toArray();
        if (!layers.isEmpty()) {
            QJsonArray normalizedLayers;
            for (const QJsonValue& value : layers) {
                QJsonObject layer = value.toObject();
                const QString originalName = layer.value(QStringLiteral("name")).toString().trimmed();
                if (!promptExplicitlyNamesLayer(m_lastAgentUserPrompt, originalName)) {
                    const QString normalizedName = normalizedBricsCadLayerName(originalName);
                    if (!normalizedName.isEmpty()) {
                        layer.insert(QStringLiteral("name"), normalizedName);
                    }
                } else {
                    layer.insert(QStringLiteral("name"), originalName);
                }
                normalizedLayers.append(layer);
            }
            params.insert(QStringLiteral("layers"), normalizedLayers);
        }
    }

    params = paramsWithConcreteHandleSelector(
        tool,
        params,
        QStringLiteral("%1\n%2\n%3")
            .arg(m_lastAgentUserPrompt,
                normalized.value(QStringLiteral("reason")).toString(),
                normalized.value(QStringLiteral("message")).toString()),
        m_currentSelection);

    if ((tool == QStringLiteral("geometry.query")
            || tool == QStringLiteral("measurement.bbox")
            || tool == QStringLiteral("measurement.length")
            || tool == QStringLiteral("measurement.area"))
        && promptRequestsCadDataQuery(m_lastAgentUserPrompt)) {
        const QString activePrompt = m_lastAgentUserPrompt.toLower();
        const bool promptMentionsSelection = textMentionsAny(activePrompt, {
            QStringLiteral("auswahl"),
            QStringLiteral("selekt"),
            QStringLiteral("selected"),
            QStringLiteral("selection"),
        });
        const bool promptMentionsExplicitLayer = activePrompt.contains(QStringLiteral("layer \""))
            || activePrompt.contains(QStringLiteral("layer '"))
            || activePrompt.contains(QStringLiteral("layer 0"))
            || activePrompt.contains(QStringLiteral("layer:"))
            || activePrompt.contains(QStringLiteral("ebene \""))
            || activePrompt.contains(QStringLiteral("ebene '"));
        const bool promptRequestsAllLayers = textMentionsAny(activePrompt, {
            QStringLiteral("alle layer"),
            QStringLiteral("allen layer"),
            QStringLiteral("alle ebenen"),
            QStringLiteral("allen ebenen"),
            QStringLiteral("alle objekte"),
            QStringLiteral("all objects"),
            QStringLiteral("all entities"),
        });

        QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
        if (selector.isEmpty()) {
            selector.insert(QStringLiteral("scope"), promptMentionsSelection
                ? QStringLiteral("selection")
                : QStringLiteral("currentSpace"));
        } else if (selector.value(QStringLiteral("scope")).toString().trimmed().isEmpty()) {
            selector.insert(QStringLiteral("scope"), promptMentionsSelection
                ? QStringLiteral("selection")
                : QStringLiteral("currentSpace"));
        }
        if (promptRequestsAllLayers && !promptMentionsExplicitLayer) {
            selector.remove(QStringLiteral("layer"));
            selector.remove(QStringLiteral("layers"));
            QJsonObject filters = params.value(QStringLiteral("filters")).toObject();
            filters.remove(QStringLiteral("layer"));
            filters.remove(QStringLiteral("layers"));
            if (filters.isEmpty()) {
                params.remove(QStringLiteral("filters"));
            } else {
                params.insert(QStringLiteral("filters"), filters);
            }
        }
        params.insert(QStringLiteral("selector"), selector);

        if (tool == QStringLiteral("geometry.query")) {
            QJsonArray include = params.value(QStringLiteral("include")).toArray();
            appendJsonStringUnique(include, QStringLiteral("metrics"));
            appendJsonStringUnique(include, QStringLiteral("geometry"));
            appendJsonStringUnique(include, QStringLiteral("dimensions"));
            params.insert(QStringLiteral("include"), include);
            if (params.value(QStringLiteral("limit")).toInt(0) < 1) {
                params.insert(QStringLiteral("limit"), 500);
            }
        } else if (!params.contains(QStringLiteral("includeObjects"))) {
            params.insert(QStringLiteral("includeObjects"), true);
        }
    }

    if (tool == QStringLiteral("bim.objects.query")) {
        if (!params.value(QStringLiteral("selector")).isObject()
            && !jsonObjectHasAnyKey(params, {
                QStringLiteral("handle"), QStringLiteral("handles"),
                QStringLiteral("name"), QStringLiteral("names")})) {
            params.insert(QStringLiteral("selector"), QJsonObject{
                {QStringLiteral("scope"), QStringLiteral("currentSpace")},
            });
        }
        QJsonArray include = params.value(QStringLiteral("include")).toArray();
        appendJsonStringUnique(include, QStringLiteral("core"));
        appendJsonStringUnique(include, QStringLiteral("geometry"));
        const QString activePrompt = workflowTrainingSearchText(m_lastAgentUserPrompt);
        if (textMentionsAny(activePrompt, {
                QStringLiteral("eigenschaft"), QStringLiteral("properties"),
                QStringLiteral("property"), QStringLiteral("vollstaendig"),
                QStringLiteral("vollstÃƒÂ¤ndig")})) {
            appendJsonStringUnique(include, QStringLiteral("properties"));
        }
        params.insert(QStringLiteral("include"), include);
        if (params.value(QStringLiteral("limit")).toInt(0) < 1) {
            params.insert(QStringLiteral("limit"), 100);
        }
        if (params.value(QStringLiteral("offset")).toInt(-1) < 0) {
            params.insert(QStringLiteral("offset"), 0);
        }
    }

    normalized.insert("params", params);
    return normalized;
}

QJsonArray BricsCadPage::agentProposalActions(const QJsonObject& proposal) const
{
    const QJsonArray actions = proposal.value("actions").toArray();
    if (!actions.isEmpty()) {
        QJsonArray normalizedActions;
        for (const QJsonValue& value : actions) {
            if (value.isObject()) {
                normalizedActions.append(normalizedAgentAction(value.toObject()));
            }
        }
        return normalizedActions;
    }
    if (!proposal.value(QStringLiteral("tool")).toString().trimmed().isEmpty()) {
        return QJsonArray{normalizedAgentAction(QJsonObject{
            {QStringLiteral("tool"), proposal.value(QStringLiteral("tool"))},
            {QStringLiteral("params"), proposal.value(QStringLiteral("params"))},
            {QStringLiteral("reason"), proposal.value(QStringLiteral("reason"))},
        })};
    }
    return {};

    // Legacy virtual action expansion below is unreachable.
    QJsonArray expanded;
    if (!actions.isEmpty()) {
        for (const QJsonValue& value : actions) {
            if (value.isObject()) {
                const QJsonArray actionExpansion = expandedAgentActions(value.toObject());
                for (const QJsonValue& expandedValue : actionExpansion) {
                    expanded.append(expandedValue);
                }
            }
        }
        return expanded;
    }

    if (!proposal.value("tool").toString().trimmed().isEmpty()) {
        return expandedAgentActions(QJsonObject{
            {"tool", proposal.value("tool").toString()},
            {"params", proposal.value("params").toObject()},
            {"reason", proposal.value("reason").toString()},
        });
    }
    return expanded;
}

bool BricsCadPage::proposalRequiresUserConfirmation(const QJsonObject& proposal) const
{
    const QJsonArray actions = agentProposalActions(proposal);
    if (actions.isEmpty()) {
        return true;
    }
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        const QJsonObject definition = toolDefinition(action.value(QStringLiteral("tool")).toString());
        if (definition.isEmpty()) {
            return true;
        }
        const bool readOnly = definition.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")
            || definition.value(QStringLiteral("kind")).toString() == QStringLiteral("query")
            || definition.value(QStringLiteral("confirmationRequired")).toBool(true) == false;
        if (!readOnly) {
            return true;
        }
    }
    return false;
}

QJsonArray BricsCadPage::expandedAgentActions(const QJsonObject& action) const
{
    const QJsonObject normalized = normalizedAgentAction(action);
    const QString tool = normalized.value("tool").toString().trimmed();
    const QJsonObject params = normalized.value("params").toObject();

    QJsonArray expanded;
    if (tool != QStringLiteral("layers.ensureMany") && tool != QStringLiteral("bim.create")) {
        expanded.append(normalized);
        return expanded;
    }

    if (tool == QStringLiteral("bim.create")) {
        const QJsonObject source = params.value(QStringLiteral("source")).toObject();
        const QString mode = source.value(QStringLiteral("mode")).toString(
            params.value(QStringLiteral("mode")).toString()).trimmed();
        const QString classification = params.value(QStringLiteral("classification")).toString(
            params.value(QStringLiteral("class")).toString()).trimmed();
        if (classification.isEmpty() || mode.isEmpty()) {
            return expanded;
        }
        const QString reason = normalized.value(QStringLiteral("reason")).toString(
            params.value(QStringLiteral("reason")).toString());
        auto sourceValue = [&source, &params](const QString& key) -> QJsonValue {
            return source.contains(key) ? source.value(key) : params.value(key);
        };
        auto sourceNumber = [&sourceValue](const QString& primary, const QString& fallback = {}) -> QJsonValue {
            const QJsonValue primaryValue = sourceValue(primary);
            if (!primaryValue.isUndefined() && !primaryValue.isNull()) {
                return primaryValue;
            }
            if (!fallback.isEmpty()) {
                return sourceValue(fallback);
            }
            return {};
        };
        const QString layer = source.value(QStringLiteral("layer")).toString(
            params.value(QStringLiteral("layer")).toString()).trimmed();
        const QJsonValue heightValue = sourceNumber(QStringLiteral("heightMm"), QStringLiteral("height"));
        const QJsonObject classifyParams{
            {QStringLiteral("classification"), classification},
            {QStringLiteral("target"), QStringLiteral("lastExtruded")},
            {QStringLiteral("selector"), QJsonObject{
                {QStringLiteral("scope"), QStringLiteral("lastExtruded")},
                {QStringLiteral("kind"), QStringLiteral("solid")}}},
            {QStringLiteral("saveBefore"), false},
        };

        if (mode.compare(QStringLiteral("box"), Qt::CaseInsensitive) == 0) {
            QJsonObject createParams{
                {QStringLiteral("geometry"), QStringLiteral("box")},
                {QStringLiteral("saveBefore"), params.value(QStringLiteral("saveBefore")).toBool(false)},
            };
            const QJsonValue origin = sourceValue(QStringLiteral("origin"));
            if (origin.isObject()) {
                createParams.insert(QStringLiteral("origin"), origin);
            } else if (sourceValue(QStringLiteral("position")).isObject()) {
                createParams.insert(QStringLiteral("origin"), sourceValue(QStringLiteral("position")));
            }
            for (const auto& key : {
                     QStringLiteral("width"), QStringLiteral("widthMm"),
                     QStringLiteral("depth"), QStringLiteral("depthMm"),
                     QStringLiteral("length"), QStringLiteral("lengthMm"),
                     QStringLiteral("height"), QStringLiteral("heightMm")}) {
                const QJsonValue value = sourceValue(key);
                if (!value.isUndefined() && !value.isNull()) {
                    createParams.insert(key, value);
                }
            }
            if (!layer.isEmpty()) {
                createParams.insert(QStringLiteral("layer"), layer);
            }
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("geometry.create")},
                {QStringLiteral("params"), createParams},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            QJsonObject classifyParamsMutable = classifyParams;
            classifyParamsMutable.remove(QStringLiteral("target"));
            classifyParamsMutable.insert(QStringLiteral("selector"), QJsonObject{
                {QStringLiteral("scope"), QStringLiteral("lastResult")},
                {QStringLiteral("kind"), QStringLiteral("solid")}});
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("bim.classify")},
                {QStringLiteral("params"), classifyParamsMutable},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            return expanded;
        }

        if (heightValue.isUndefined() || heightValue.isNull()) {
            return expanded;
        }

        if (mode.compare(QStringLiteral("rectangleExtrusion"), Qt::CaseInsensitive) == 0) {
            const QJsonObject selector = source.value(QStringLiteral("selector")).toObject();
            if (selector.isEmpty()) {
                QJsonObject createParams{
                    {QStringLiteral("geometry"), QStringLiteral("rectangle")},
                    {QStringLiteral("saveBefore"), params.value(QStringLiteral("saveBefore")).toBool(false)},
                };
                const QJsonValue origin = sourceValue(QStringLiteral("origin"));
                if (origin.isObject()) {
                    createParams.insert(QStringLiteral("origin"), origin);
                } else if (sourceValue(QStringLiteral("position")).isObject()) {
                    createParams.insert(QStringLiteral("origin"), sourceValue(QStringLiteral("position")));
                }
                for (const auto& key : {
                         QStringLiteral("width"), QStringLiteral("widthMm"),
                         QStringLiteral("depth"), QStringLiteral("depthMm"),
                         QStringLiteral("length"), QStringLiteral("lengthMm")}) {
                    const QJsonValue value = sourceValue(key);
                    if (!value.isUndefined() && !value.isNull()) {
                        createParams.insert(key, value);
                    }
                }
                if (!layer.isEmpty()) {
                    createParams.insert(QStringLiteral("layer"), layer);
                }
                expanded.append(QJsonObject{
                    {QStringLiteral("tool"), QStringLiteral("geometry.create")},
                    {QStringLiteral("params"), createParams},
                    {QStringLiteral("reason"), reason},
                    {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
                });
            }
            QJsonObject extrudeParams{
                {QStringLiteral("heightMm"), heightValue},
                {QStringLiteral("selector"), selector.isEmpty()
                    ? QJsonObject{{QStringLiteral("scope"), QStringLiteral("lastResult")}, {QStringLiteral("kind"), QStringLiteral("rectangle")}}
                    : selector},
                {QStringLiteral("saveBefore"), selector.isEmpty() ? false : params.value(QStringLiteral("saveBefore")).toBool(false)},
            };
            if (!layer.isEmpty()) {
                extrudeParams.insert(QStringLiteral("layer"), layer);
            }
            if (!source.value(QStringLiteral("detail")).toString().trimmed().isEmpty()) {
                extrudeParams.insert(QStringLiteral("detail"), source.value(QStringLiteral("detail")));
            }
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("rectangles.extrude")},
                {QStringLiteral("params"), extrudeParams},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("bim.classify")},
                {QStringLiteral("params"), classifyParams},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            return expanded;
        }

        if (mode.compare(QStringLiteral("profileExtrusion"), Qt::CaseInsensitive) == 0) {
            const QJsonObject selector = source.value(QStringLiteral("selector")).toObject(
                params.value(QStringLiteral("selector")).toObject());
            if (selector.isEmpty()) {
                return expanded;
            }
            QJsonObject extrudeParams{
                {QStringLiteral("heightMm"), heightValue},
                {QStringLiteral("selector"), selector},
                {QStringLiteral("saveBefore"), params.value(QStringLiteral("saveBefore")).toBool(false)},
            };
            if (!layer.isEmpty()) {
                extrudeParams.insert(QStringLiteral("layer"), layer);
            }
            if (!source.value(QStringLiteral("detail")).toString().trimmed().isEmpty()) {
                extrudeParams.insert(QStringLiteral("detail"), source.value(QStringLiteral("detail")));
            }
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("profile.extrude")},
                {QStringLiteral("params"), extrudeParams},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            expanded.append(QJsonObject{
                {QStringLiteral("tool"), QStringLiteral("bim.classify")},
                {QStringLiteral("params"), classifyParams},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("virtualSource"), QStringLiteral("bim.create")},
            });
            return expanded;
        }

        return expanded;
    }

    const QJsonArray layers = params.value("layers").toArray();
    for (const QJsonValue& value : layers) {
        const QJsonObject layer = value.toObject();
        const QString name = normalizedBricsCadLayerName(layer.value("name").toString());
        if (name.isEmpty()) {
            continue;
        }

        QJsonObject createParams{{"name", name}};
        if (layer.contains("colorIndex") && !layer.value("colorIndex").isNull()) {
            createParams.insert("colorIndex", layer.value("colorIndex"));
        }
        expanded.append(QJsonObject{
            {"tool", "layers.create"},
            {"params", createParams},
            {"reason", normalized.value("reason").toString(params.value("reason").toString())},
            {"virtualSource", "layers.ensureMany"},
        });
    }

    return expanded;
}

QJsonObject BricsCadPage::agentPreflightParams(const QJsonObject& proposal) const
{
    QJsonArray actions;
    bool runtimeHandleResultPending = false;
    const QJsonArray proposalActions = agentProposalActions(proposal);
    for (int actionIndex = 0; actionIndex < proposalActions.size(); ++actionIndex) {
        const QJsonObject action = proposalActions.at(actionIndex).toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString();
        QJsonObject actionParams = action.value(QStringLiteral("params")).toObject();
        const bool autoBimHandles = actionParams.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false);
        const bool resolvesRuntimeHandles = actionParams.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPointsFromLastQuery")).toBool(false)
            || actionParams.value(QStringLiteral("autoCreatedGeometryHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPolylineHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPolylineHandlesFromLastQuery")).toBool(false)
            || actionParams.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
            || autoBimHandles
            || actionParams.contains(QStringLiteral("createdGeometryHandleIndex"))
            || !actionParams.value(QStringLiteral("createdGeometryHandleIndexes")).toArray().isEmpty();
        const QString selectorScope = actionParams.value(QStringLiteral("selector")).toObject()
            .value(QStringLiteral("scope")).toString().trimmed();
        if (autoBimHandles) {
            actions.append(QJsonObject{
                {QStringLiteral("tool"), brxValidationToolName(tool)},
                {QStringLiteral("params"), actionParams},
                {QStringLiteral("clientActionIndex"), actionIndex},
            });
            runtimeHandleResultPending = true;
            continue;
        }
        if (resolvesRuntimeHandles) {
            runtimeHandleResultPending = true;
            continue;
        }
        if (runtimeHandleResultPending
            && selectorScope.compare(QStringLiteral("lastResult"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        runtimeHandleResultPending = false;
        actions.append(QJsonObject{
            {QStringLiteral("tool"), brxValidationToolName(tool)},
            {QStringLiteral("params"), actionParams},
            {QStringLiteral("clientActionIndex"), actionIndex},
        });
    }

    return QJsonObject{
        {"source", "agent_preflight"},
        {"actions", actions},
    };
}

QJsonObject proposalWithBimValidationTargets(
    QJsonObject proposal,
    const QJsonObject& validationResult,
    QJsonArray actions)
{
    const QJsonArray validationActions = validationResult.value(QStringLiteral("actions")).toArray();
    bool expandedProposalStored = false;
    for (int validationIndex = 0; validationIndex < validationActions.size(); ++validationIndex) {
        const QJsonObject validationAction = validationActions.at(validationIndex).toObject();
        const int actionIndex = validationAction.value(QStringLiteral("clientActionIndex")).toInt(validationIndex);
        if (actionIndex < 0 || actionIndex >= actions.size()) {
            continue;
        }
        QJsonObject action = actions.at(actionIndex).toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString();
        if (tool != QStringLiteral("bim.selection.set")
            && tool != QStringLiteral("bim.move")) {
            continue;
        }
        action.insert(QStringLiteral("params"), paramsWithBimValidationTargets(
            action.value(QStringLiteral("params")).toObject(), validationAction));
        actions.replace(actionIndex, action);
        expandedProposalStored = true;
    }
    if (expandedProposalStored) {
        proposal.insert(QStringLiteral("actions"), actions);
        proposal.remove(QStringLiteral("tool"));
        proposal.remove(QStringLiteral("params"));
    }
    proposal.insert(QStringLiteral("preflightValidation"), validationResult);
    return proposal;
}

QString actionCompletionText(const QJsonObject& action, const QJsonObject& result)
{
    const QString tool = action.value("tool").toString();
    const QJsonObject params = action.value("params").toObject();
    const QString summary = result.value("summary").toString().trimmed();

    if (tool == QStringLiteral("layers.create")) {
        const QString name = params.value("name").toString().trimmed();
        return name.isEmpty() ? QStringLiteral("Layer angelegt") : QString("Layer \"%1\" angelegt").arg(name);
    }
    if (tool == QStringLiteral("layers.rename")) {
        const QString oldName = params.value("oldName").toString().trimmed();
        const QString newName = params.value("newName").toString().trimmed();
        if (!oldName.isEmpty() && !newName.isEmpty()) {
            return QString("Layer \"%1\" in \"%2\" umbenannt").arg(oldName, newName);
        }
        return QStringLiteral("Layer umbenannt");
    }
    if (tool == QStringLiteral("layers.setColor")) {
        const QString name = params.value("name").toString().trimmed();
        const int colorIndex = params.value("colorIndex").toInt();
        if (!name.isEmpty() && colorIndex > 0) {
            return QString("Layer \"%1\" auf Farbe %2 gesetzt").arg(name).arg(colorIndex);
        }
        return QStringLiteral("Layerfarbe gesetzt");
    }
    if (tool == QStringLiteral("geometry.create")) {
        const QString geometry = params.value("geometry").toString(params.value("type").toString()).trimmed();
        return geometry.isEmpty() ? QStringLiteral("Geometrie erstellt") : QString("%1 erstellt").arg(geometry);
    }
    if (tool == QStringLiteral("entity.setName")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        return name.isEmpty() ? QStringLiteral("Entity benannt") : QString("Entity als \"%1\" benannt").arg(name);
    }
    if (tool == QStringLiteral("geometry.query")
        || tool == QStringLiteral("bim.objects.query")
        || tool == QStringLiteral("selection.describe")
        || tool == QStringLiteral("entity.describe")) {
        const int count = result.value(QStringLiteral("count")).toInt(result.value(QStringLiteral("objects")).toArray().size());
        return QString("%1 Objekt%2 gefunden").arg(count).arg(count == 1 ? QString() : QStringLiteral("e"));
    }
    if (!summary.isEmpty()) {
        return repairMojibakeText(summary);
    }
    if (tool == QStringLiteral("document.save")) {
        return QStringLiteral("Zeichnung gespeichert");
    }
    return tool.isEmpty() ? QStringLiteral("Aktion abgeschlossen") : QString("%1 abgeschlossen").arg(tool);
}

QJsonObject executionStatsForActions(const QJsonArray& actions, const QJsonArray& results)
{
    int layerCreateRequested = 0;
    int layerCreated = 0;
    int layerSkippedExisting = 0;
    int layerColorSet = 0;
    int geometryCreates = 0;
    int boxCreates = 0;
    int bimClassifies = 0;
    int bimClassified = 0;
    int explicitFailures = 0;
    QJsonArray createdLayerNames;
    QJsonArray skippedLayerNames;
    QJsonArray createdGeometryHandles;

    for (const QJsonValue& value : actions) {
        if (value.toObject().value("tool").toString() == QStringLiteral("layers.create")) {
            ++layerCreateRequested;
        }
    }

    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        const QString tool = item.value("tool").toString();
        const QJsonObject params = item.value("params").toObject();
        const QJsonObject result = item.value("result").toObject();
        const QString summary = repairMojibakeText(result.value("summary").toString()).trimmed();
        const QString summaryLower = summary.toLower();
        explicitFailures += std::max(0, result.value("failed").toInt(0));

        if (tool == QStringLiteral("layers.create")) {
            const QString name = repairMojibakeText(params.value("name").toString()).trimmed();
            if (summaryLower.contains(QStringLiteral("skipped existing"))
                || summaryLower.contains(QStringLiteral("existiert bereits"))
                || summaryLower.contains(QStringLiteral("uebersprungen"))
                || summaryLower.contains(QStringLiteral("ÃƒÂ¼bersprungen"))) {
                ++layerSkippedExisting;
                if (!name.isEmpty()) {
                    skippedLayerNames.append(name);
                }
            } else {
                ++layerCreated;
                if (!name.isEmpty()) {
                    createdLayerNames.append(name);
                }
            }
        } else if (tool == QStringLiteral("layers.setColor")) {
            ++layerColorSet;
        } else if (tool == QStringLiteral("geometry.create")) {
            ++geometryCreates;
            if (result.value("geometry").toString().compare(QStringLiteral("box"), Qt::CaseInsensitive) == 0) {
                ++boxCreates;
            }
            const QString handle = result.value("handle").toString().trimmed();
            if (!handle.isEmpty()) {
                createdGeometryHandles.append(handle);
            }
        } else if (tool == QStringLiteral("bim.classify")) {
            ++bimClassifies;
            bimClassified += std::max(0, result.value("classified").toInt(0));
        }
    }

    const int requested = actions.size();
    const int completed = results.size();
    const int failed = std::max(0, requested - completed) + explicitFailures;

    QJsonObject stats{
        {"actionsRequested", requested},
        {"actionsCompleted", completed},
        {"failed", failed},
    };
    if (layerCreateRequested > 0) {
        stats.insert("layerCreatesRequested", layerCreateRequested);
        stats.insert("layersCreated", layerCreated);
        stats.insert("layersSkippedExisting", layerSkippedExisting);
        stats.insert("createdLayerNames", createdLayerNames);
        stats.insert("skippedExistingLayerNames", skippedLayerNames);
    }
    if (layerColorSet > 0) {
        stats.insert("layerColorsSet", layerColorSet);
    }
    if (geometryCreates > 0) {
        stats.insert("geometryCreated", geometryCreates);
        stats.insert("boxSolidsCreated", boxCreates);
        stats.insert("createdGeometryHandles", createdGeometryHandles);
    }
    if (bimClassifies > 0) {
        stats.insert("bimClassifyActions", bimClassifies);
        stats.insert("bimWallsClassified", bimClassified);
    }
    return stats;
}

QString agentCompletionSummary(const QJsonArray& actions, const QJsonArray& results, const QString& fallbackSummary)
{
    const QJsonObject stats = executionStatsForActions(actions, results);
    const int layerCreateRequested = stats.value("layerCreatesRequested").toInt(0);
    const int boxSolidsCreated = stats.value("boxSolidsCreated").toInt(0);
    const int bimWallsClassified = stats.value("bimWallsClassified").toInt(0);
    if (boxSolidsCreated > 0) {
        const int failed = stats.value("failed").toInt(0);
        if (failed == 0 && bimWallsClassified > 0) {
            return QString("Erledigt. Ich habe %1 Wandkoerper erstellt und %2 davon als BIM-Waende klassifiziert.")
                .arg(boxSolidsCreated)
                .arg(bimWallsClassified);
        }
        if (failed == 0) {
            return QString("Erledigt. Ich habe %1 Wandkoerper erstellt.").arg(boxSolidsCreated);
        }
    }
    if (layerCreateRequested > 0 && actions.size() > 3) {
        const int created = stats.value("layersCreated").toInt(0);
        const int skipped = stats.value("layersSkippedExisting").toInt(0);
        const int failed = stats.value("failed").toInt(0);
        if (failed == 0) {
            if (created > 0 && skipped > 0) {
                return QString("Erledigt. Ich habe %1 neue Layer angelegt; %2 waren bereits vorhanden und wurden unveraendert uebernommen.")
                    .arg(created)
                    .arg(skipped);
            }
            if (created > 0) {
                return QString("Erledigt. Ich habe %1 neue Layer angelegt.").arg(created);
            }
            if (skipped > 0) {
                return QString("Erledigt. Alle %1 Layer waren bereits vorhanden und wurden unveraendert uebernommen.").arg(skipped);
            }
        }
    }

    QStringList items;
    for (int i = 0; i < actions.size(); ++i) {
        const QJsonObject action = actions.at(i).toObject();
        QJsonObject result;
        if (i < results.size()) {
            result = results.at(i).toObject().value("result").toObject();
        }
        const QString text = actionCompletionText(action, result).trimmed();
        if (!text.isEmpty()) {
            items << text;
        }
    }

    if (items.isEmpty()) {
        return fallbackSummary.trimmed().isEmpty()
            ? QStringLiteral("Erledigt.")
            : QString("Erledigt. %1").arg(fallbackSummary.trimmed());
    }

    if (items.size() == 1) {
        return QString("Erledigt. %1.").arg(items.first());
    }

    return QString("Erledigt. Ich habe %1 Aktionen in BricsCAD ausgefuehrt: %2.")
        .arg(items.size())
        .arg(items.join(QStringLiteral("; ")));
}

void BricsCadPage::clearLastBricsCadExecution()
{
    m_lastBricsCadExecution = {};
}

void BricsCadPage::recordLastBricsCadExecution(
    const QJsonObject& proposal,
    const QJsonArray& plannedActions,
    const QJsonArray& results,
    const QJsonObject& batchResult)
{
    QJsonArray executedActions;
    for (int i = 0; i < results.size(); ++i) {
        const QJsonObject stepResult = results.at(i).toObject();
        const QJsonObject plannedAction = i < plannedActions.size()
            ? plannedActions.at(i).toObject()
            : QJsonObject{};
        const QString tool = stepResult.value(QStringLiteral("tool")).toString(
            plannedAction.value(QStringLiteral("tool")).toString()).trimmed();
        if (tool.isEmpty()) {
            continue;
        }
        QJsonObject action{
            {QStringLiteral("tool"), tool},
            {QStringLiteral("params"), stepResult.value(QStringLiteral("params")).toObject(
                plannedAction.value(QStringLiteral("params")).toObject())},
        };
        const QString reason = plannedAction.value(QStringLiteral("reason")).toString().trimmed();
        if (!reason.isEmpty()) {
            action.insert(QStringLiteral("reason"), reason);
        }
        executedActions.append(action);
    }

    const QJsonObject executionStats = batchResult.value(QStringLiteral("executionStats")).toObject(
        executionStatsForActions(plannedActions, results));
    const int requested = batchResult.value(QStringLiteral("actionsRequested")).toInt(plannedActions.size());
    const int completed = batchResult.value(QStringLiteral("actionsCompleted")).toInt(results.size());
    const int failed = batchResult.value(QStringLiteral("failed")).toInt(
        executionStats.value(QStringLiteral("failed")).toInt(std::max(0, requested - completed)));
    const bool saveEligible = requested > 0
        && completed == requested
        && failed == 0
        && executedActions.size() == requested;

    const QString executionId = QStringLiteral("brx-exec-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    m_lastBricsCadExecution = QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.bricscad.execution.v1")},
        {QStringLiteral("executionId"), executionId},
        {QStringLiteral("sessionId"), m_agentSessionId},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("sourcePrompt"), m_lastAgentUserPrompt},
        {QStringLiteral("proposal"), proposal},
        {QStringLiteral("plannedActions"), plannedActions},
        {QStringLiteral("executedActions"), executedActions},
        {QStringLiteral("batchResult"), batchResult},
        {QStringLiteral("results"), results},
        {QStringLiteral("executionStats"), executionStats},
        {QStringLiteral("actionsRequested"), requested},
        {QStringLiteral("actionsCompleted"), completed},
        {QStringLiteral("failed"), failed},
        {QStringLiteral("saveEligible"), saveEligible},
    };
    saveCurrentAgentSession();
}

QJsonObject BricsCadPage::bricsCadExecutionForMessage(const QString& messageId) const
{
    const QString trimmedMessageId = messageId.trimmed();
    if (!trimmedMessageId.isEmpty()) {
        for (const QJsonValue& value : m_agentConversation) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("messageId")).toString().trimmed() != trimmedMessageId) {
                continue;
            }
            const QJsonObject execution = item.value(QStringLiteral("bricsCadExecutionRecord")).toObject();
            if (!execution.isEmpty()) {
                return execution;
            }
            return {};
        }
    }

    if (!m_lastBricsCadExecution.isEmpty()) {
        const QString lastMessageId = m_lastBricsCadExecution.value(QStringLiteral("messageId")).toString().trimmed();
        if (trimmedMessageId.isEmpty() || lastMessageId.isEmpty() || lastMessageId == trimmedMessageId) {
            return m_lastBricsCadExecution;
        }
    }
    return {};
}

QJsonObject BricsCadPage::latestBricsCadExecutionFromConversation() const
{
    for (int i = m_agentConversation.size() - 1; i >= 0; --i) {
        const QJsonObject item = m_agentConversation.at(i).toObject();
        const QJsonObject execution = item.value(QStringLiteral("bricsCadExecutionRecord")).toObject();
        if (execution.value(QStringLiteral("schema")).toString()
                == QStringLiteral("barebone.qt.bricscad.execution.v1")
            && execution.value(QStringLiteral("sessionId")).toString() == m_agentSessionId
            && execution.value(QStringLiteral("saveEligible")).toBool(false)) {
            return execution;
        }
    }
    return {};
}

QVariantMap BricsCadPage::bricsCadExecutionMessageExtra() const
{
    if (m_lastBricsCadExecution.value(QStringLiteral("schema")).toString()
            != QStringLiteral("barebone.qt.bricscad.execution.v1")
        || m_lastBricsCadExecution.value(QStringLiteral("sessionId")).toString() != m_agentSessionId
        || !m_lastBricsCadExecution.value(QStringLiteral("saveEligible")).toBool(false)) {
        return {};
    }

    return QVariantMap{
        {QStringLiteral("bricsCadExecutionId"), m_lastBricsCadExecution.value(QStringLiteral("executionId")).toString()},
        {QStringLiteral("workflowSaveEligible"), true},
        {QStringLiteral("bricsCadExecutionRecord"), m_lastBricsCadExecution.toVariantMap()},
    };
}

QJsonObject BricsCadPage::workflowFromBricsCadExecution(const QJsonObject& execution, const QString& sessionTitle, QString* errorMessage) const
{
    if (errorMessage) {
        errorMessage->clear();
    }
    const QJsonObject cleanExecution = repairedMojibakeJsonValue(execution).toObject();
    if (cleanExecution.isEmpty()
        || cleanExecution.value(QStringLiteral("schema")).toString()
            != QStringLiteral("barebone.qt.bricscad.execution.v1")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Keine abgeschlossene BricsCAD-Ausfuehrung zum Speichern vorhanden.");
        }
        return {};
    }
    if (cleanExecution.value(QStringLiteral("sessionId")).toString() != m_agentSessionId) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Die letzte BricsCAD-Ausfuehrung gehoert nicht zur aktiven Sitzung.");
        }
        return {};
    }
    if (!cleanExecution.value(QStringLiteral("saveEligible")).toBool(false)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Die letzte BricsCAD-Ausfuehrung wurde nicht vollstaendig erfolgreich abgeschlossen.");
        }
        return {};
    }

    const QJsonArray plannedActions = cleanExecution.value(QStringLiteral("plannedActions")).toArray();
    const QJsonArray executedActions = cleanExecution.value(QStringLiteral("executedActions")).toArray();
    if (plannedActions.isEmpty() || executedActions.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Die letzte BricsCAD-Ausfuehrung enthaelt keine speicherbaren Aktionen.");
        }
        return {};
    }

    QJsonArray preferredTools;
    QStringList seenTools;
    QJsonArray steps;
    for (int i = 0; i < plannedActions.size(); ++i) {
        const QJsonObject action = plannedActions.at(i).toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString().trimmed();
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (tool.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Die letzte BricsCAD-Ausfuehrung enthaelt eine Aktion ohne Tool.");
            }
            return {};
        }
        if (!seenTools.contains(tool)) {
            seenTools << tool;
            preferredTools.append(tool);
        }

        const QJsonObject definition = toolDefinition(tool);
        QString stepTitle = action.value(QStringLiteral("title")).toString().trimmed();
        if (stepTitle.isEmpty()) {
            stepTitle = definition.value(QStringLiteral("title")).toString(tool);
        }
        if (stepTitle.isEmpty()) {
            stepTitle = QStringLiteral("Aktion %1").arg(i + 1);
        }

        QJsonObject step{
            {QStringLiteral("id"), QStringLiteral("step_%1_%2")
                .arg(i + 1)
                .arg(workflowSlug(tool))},
            {QStringLiteral("title"), stepTitle},
            {QStringLiteral("tool"), tool},
            {QStringLiteral("paramsTemplate"), params},
        };
        const QString reason = action.value(QStringLiteral("reason")).toString().trimmed();
        if (!reason.isEmpty()) {
            step.insert(QStringLiteral("description"), reason);
        }
        steps.append(step);
    }

    const QString sourcePrompt = repairMojibakeText(cleanExecution.value(QStringLiteral("sourcePrompt")).toString()).trimmed();
    const QJsonObject proposal = cleanExecution.value(QStringLiteral("proposal")).toObject();
    const QJsonObject batchResult = cleanExecution.value(QStringLiteral("batchResult")).toObject();
    QString title;

    QJsonObject workflow{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.workflow.v1")},
        {QStringLiteral("title"), QString{}},
        {QStringLiteral("description"), QString{}},
        {QStringLiteral("triggerExamples"), sourcePrompt.isEmpty()
            ? QJsonArray{} : QJsonArray{sourcePrompt}},
        {QStringLiteral("requiredSlots"), QJsonArray{}},
        {QStringLiteral("optionalSlots"), QJsonObject{}},
        {QStringLiteral("derivedValues"), QJsonArray{}},
        {QStringLiteral("preferredTools"), preferredTools},
        {QStringLiteral("steps"), steps},
        {QStringLiteral("executionBatches"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("batch_1")},
                {QStringLiteral("title"), QStringLiteral("Letzte BricsCAD-Ausfuehrung")},
                {QStringLiteral("mode"), QStringLiteral("sequential")},
                {QStringLiteral("stopOnFailure"), true},
                {QStringLiteral("steps"), steps},
            },
        }},
        {QStringLiteral("constructionStrategy"), QJsonArray{
            QStringLiteral("Deterministisch aus der letzten erfolgreich von BRX ausgefuehrten Aktion erzeugt."),
        }},
        {QStringLiteral("forbidden"), QJsonArray{}},
        {QStringLiteral("validationExamples"), QJsonArray{
            QJsonObject{
                {QStringLiteral("title"), QStringLiteral("Letzte erfolgreiche Ausfuehrung")},
                {QStringLiteral("prompt"), sourcePrompt},
                {QStringLiteral("actions"), executedActions},
            },
        }},
        {QStringLiteral("knownSlotValues"), QJsonObject{}},
        {QStringLiteral("authoringNotes"), QJsonObject{
            {QStringLiteral("source"), QStringLiteral("lastBricsCadExecution")},
            {QStringLiteral("createdAt"), cleanExecution.value(QStringLiteral("createdAt"))},
            {QStringLiteral("actionsRequested"), cleanExecution.value(QStringLiteral("actionsRequested"))},
            {QStringLiteral("actionsCompleted"), cleanExecution.value(QStringLiteral("actionsCompleted"))},
        }},
    };

    // Workflow identity describes the reusable topic, never concrete values
    // from the executed prompt (dimensions, coordinates, counts or names).
    QStringList actionTools;
    for (const QJsonValue& value : plannedActions) {
        const QString tool = value.toObject().value(QStringLiteral("tool")).toString().trimmed();
        if (!tool.isEmpty() && !actionTools.contains(tool)) {
            actionTools << tool;
        }
    }
    if (actionTools.size() == 1) {
        const QString tool = actionTools.first();
        const QJsonObject params = plannedActions.first().toObject().value(QStringLiteral("params")).toObject();
        if (tool == QStringLiteral("geometry.create")) {
            const QString geometry = params.value(QStringLiteral("geometry")).toString().trimmed().toLower();
            static const QHash<QString, QString> geometryTitles{
                {QStringLiteral("rectangle"), QStringLiteral("Rechteck-Geometrie erstellen")},
                {QStringLiteral("box"), QStringLiteral("Quader-Geometrie erstellen")},
                {QStringLiteral("circle"), QStringLiteral("Kreis-Geometrie erstellen")},
                {QStringLiteral("arc"), QStringLiteral("Bogen-Geometrie erstellen")},
                {QStringLiteral("line"), QStringLiteral("Linien-Geometrie erstellen")},
                {QStringLiteral("polyline"), QStringLiteral("Polylinien-Geometrie erstellen")},
                {QStringLiteral("point"), QStringLiteral("Punkt-Geometrie erstellen")},
            };
            title = geometryTitles.value(geometry, QStringLiteral("Geometrie erstellen"));
        } else if (tool == QStringLiteral("layers.create") || tool == QStringLiteral("layers.ensureMany")) {
            title = QStringLiteral("Layer erstellen");
        } else if (tool == QStringLiteral("rectangles.extrude")) {
            title = QStringLiteral("Rechteck-Geometrie extrudieren");
        } else if (tool == QStringLiteral("circle.extrude")) {
            title = QStringLiteral("Kreis-Geometrie extrudieren");
        } else if (tool == QStringLiteral("profile.extrude")) {
            title = QStringLiteral("Profil-Geometrie extrudieren");
        }
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(proposal.value(QStringLiteral("title")).toString());
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(proposal.value(QStringLiteral("intentSummary")).toString());
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(proposal.value(QStringLiteral("summary")).toString(
            proposal.value(QStringLiteral("message")).toString()));
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(batchResult.value(QStringLiteral("summary")).toString());
    }
    if (title.isEmpty() && !isGenericWorkflowSessionTitle(sessionTitle)) {
        title = workflowTitleSeed(sessionTitle);
    }
    if (title.isEmpty()) {
        title = workflowTitleSeed(sourcePrompt);
    }
    if (title.isEmpty()) {
        title = QStringLiteral("BricsCAD Ausfuehrung %1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    }
    workflow.insert(QStringLiteral("title"), title);
    workflow.insert(QStringLiteral("id"), workflowSlug(title));

    QString description = workflowTitleSeed(proposal.value(QStringLiteral("summary")).toString(
        proposal.value(QStringLiteral("message")).toString()));
    if (description.isEmpty()) {
        description = workflowTitleSeed(batchResult.value(QStringLiteral("summary")).toString());
    }
    if (description.isEmpty()) {
        description = sourcePrompt;
    }
    if (description.isEmpty()) {
        description = title;
    }
    workflow.insert(QStringLiteral("description"), description);

    ensureWorkflowIdentityForSessionSave(workflow, sessionTitle, sourcePrompt);
    workflow = normalizedWorkflowRuntimeSelectors(workflow);
    return workflow;
}

QJsonObject BricsCadPage::workflowFromLastBricsCadExecution(const QString& sessionTitle, QString* errorMessage) const
{
    return workflowFromBricsCadExecution(m_lastBricsCadExecution, sessionTitle, errorMessage);
}

void BricsCadPage::saveBricsCadWorkflowFromExecution(const QString& messageId, const QString& sessionTitle)
{
    QString error;
    const QJsonObject execution = bricsCadExecutionForMessage(messageId);
    QJsonObject workflow = workflowFromBricsCadExecution(execution, sessionTitle, &error);
    if (!error.isEmpty()) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), error);
        appendBridgeLog(QStringLiteral("BricsCAD Workflow Save: %1").arg(error));
        return;
    }

    validateWorkflowForTraining(workflow, error);
    if (error.isEmpty() && workflow.value(QStringLiteral("executionBatches")).toArray().isEmpty()) {
        error = QStringLiteral("executionBatches fehlt.");
    }
    const QJsonArray examples = workflow.value(QStringLiteral("validationExamples")).toArray();
    if (error.isEmpty() && examples.isEmpty()) {
        error = QStringLiteral("validationExamples fehlt.");
    }
    if (error.isEmpty()) {
        const QJsonArray actions = examples.first().toObject().value(QStringLiteral("actions")).toArray();
        if (actions.isEmpty()) {
            error = QStringLiteral("validationExamples[0].actions fehlt.");
        }
        for (int i = 0; error.isEmpty() && i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
            if (jsonContainsTemplatePlaceholder(action.value(QStringLiteral("params")))) {
                error = QStringLiteral("validationExamples[0].actions[%1] enthaelt einen Platzhalter.").arg(i);
            } else if (!validateAgentAction(action, error)) {
                error = QStringLiteral("validationExamples[0].actions[%1]: %2").arg(i).arg(error);
            }
        }
    }
    if (!error.isEmpty()) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflow konnte nicht aus der letzten BricsCAD-Ausfuehrung gespeichert werden: %1").arg(error));
        appendBridgeLog(QStringLiteral("BricsCAD Workflow Save: Validierung abgelehnt: %1").arg(error));
        return;
    }

    QString savedPath;
    if (!saveWorkflowFromTraining(workflow, &savedPath, &error)) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflow konnte nicht gespeichert werden: %1").arg(error));
        appendBridgeLog(QStringLiteral("BricsCAD Workflow Save: Speichern fehlgeschlagen: %1").arg(error));
        return;
    }

    const QString savedId = QFileInfo(savedPath).completeBaseName();
    QJsonObject savedWorkflow = loadBricsCadWorkflowById(savedId);
    if (!savedWorkflow.isEmpty()) {
        m_selectedWorkflowId = savedId;
        m_selectedWorkflow = savedWorkflow;
        m_selectedWorkflowSlotValues = savedWorkflow.value(QStringLiteral("knownSlotValues")).toObject();
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [map = savedWorkflow.toVariantMap()](AiWebBridge* target) {
                Q_EMIT target->selectedWorkflowChanged(map);
            });
        }
    }
    emitWorkflowListToWeb();
    appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Letzte BricsCAD-Ausfuehrung wurde als Workflow gespeichert: %1").arg(savedPath));
    appendBridgeLog(QStringLiteral("BricsCAD Workflow Save: aus letzter Ausfuehrung gespeichert %1").arg(savedPath));
}

void BricsCadPage::saveBricsCadWorkflowFromLastExecution(const QString& sessionTitle)
{
    saveBricsCadWorkflowFromExecution(QString{}, sessionTitle);
}

void BricsCadPage::preflightAgentProposal(const QString& rejectedContent, const QJsonObject& rejectedObject, const QJsonObject& proposal)
{
    if (!bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("actions.validate"))) {
        clearAgentProposal();
        setAgentBusy(false);
        appendBridgeLog("AI Agent: Vorschlag blockiert, BRX actions.validate fehlt");
        appendAgentChat("Barebone-Qt", "BRX Preflight ist nicht verfuegbar. Bitte BareboneBrx.brx neu laden, damit Vorschlaege vor der Bestaetigung trocken geprueft werden koennen.");
        if (m_brxAuthenticated) {
            m_capabilitiesRequested = false;
            requestBridgeCapabilities();
        }
        return;
    }

    const QJsonObject params = agentPreflightParams(proposal);
    const int actionCount = params.value("actions").toArray().size();
    const int proposalActionCount = agentProposalActions(proposal).size();
    appendBridgeLog(actionCount == proposalActionCount
        ? QString("Qt -> BRX: actions.validate %1 Aktion(en)").arg(actionCount)
        : QString("Qt -> BRX: actions.validate %1/%2 direkt pruefbare Aktion(en)").arg(actionCount).arg(proposalActionCount));
    setAgentBusy(true);

    const bool queued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        params,
        15000,
        [this, rejectedContent, rejectedObject, proposal](const QJsonObject& response) {
            const auto releaseReadyProposal = [this](QJsonObject readyProposal, QString context) {
                QTimer::singleShot(0, this, [this, readyProposal = std::move(readyProposal), context = std::move(context)]() mutable {
                    appendBridgeLog(QString("AI Agent: Preflight-Freigabe UI start (%1)").arg(context));
                    m_agentValidationRetries = 0;
                    setAgentProposal(readyProposal);
                    appendBridgeLog(QString("AI Agent: Preflight-Freigabe Proposal gesetzt (%1)").arg(context));
                    setAgentBusy(false);
                    appendBridgeLog(QString("AI Agent: Preflight-Freigabe idle gesetzt (%1)").arg(context));
                });
            };

            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            drawingContextStore().ingestValidationResult(proposal, response);
            const bool transportOk = response.value("ok").toBool(false);
            const bool valid = transportOk && result.value("valid").toBool(false);
            if (valid) {
                const QStringList warnings = stringsFromJsonArray(result.value("warnings").toArray());
                if (!warnings.isEmpty()) {
                    appendBridgeLog(QString("BRX Preflight: gueltig mit Warnungen: %1").arg(warnings.join("; ").left(500)));
                } else {
                    appendBridgeLog("BRX Preflight: gueltig");
                }
                QJsonObject readyProposal = proposalWithBimValidationTargets(
                    proposal, result, agentProposalActions(proposal));
                if (!warnings.isEmpty()) {
                    QJsonArray warningValues;
                    for (const QString& warning : warnings) {
                        warningValues.append(repairMojibakeText(warning));
                    }
                    readyProposal.insert("preflightWarnings", warningValues);
                }
                releaseReadyProposal(readyProposal, QStringLiteral("valid"));
                return;
            }

            const QString message = validationFailureMessage(response);
            appendBridgeLog(QString("BRX Preflight: abgelehnt: %1").arg(message.left(700).replace('\n', " | ")));
            const QJsonArray proposalActions = agentProposalActions(proposal);
            const QJsonObject repairGuidance = validationRepairGuidance(message);
            if (proposalCreatesMissingLayerBeforeUse(proposalActions, repairGuidance)) {
                QJsonObject readyProposal = proposal;
                QJsonArray warningValues;
                warningValues.append(repairMojibakeText(message));
                warningValues.append(QStringLiteral(
                    "Der fehlende Layer wird in einem vorherigen Batch-Schritt mit layers.create erzeugt. "
                    "Qt validiert und fuehrt diesen Schritt zuerst aus und prueft die abhaengige Aktion danach erneut mit BRX."));
                readyProposal.insert(QStringLiteral("preflightWarnings"), warningValues);
                readyProposal.insert(QStringLiteral("deferredRuntimePreflight"), true);
                readyProposal.insert(QStringLiteral("preflightRepairGuidance"), repairGuidance);
                appendBridgeLog(QString("BRX Preflight: Layer-Abhaengigkeit '%1' sicher bis zur Einzelaktionspruefung zurueckgestellt")
                    .arg(repairGuidance.value(QStringLiteral("layerName")).toString()));
                releaseReadyProposal(readyProposal, QStringLiteral("missing-layer"));
                return;
            }
            if (proposalPreflightFailureCanBeDeferred(proposalActions, message)) {
                QJsonObject readyProposal = proposal;
                QJsonArray warningValues;
                warningValues.append(repairMojibakeText(message));
                warningValues.append(QStringLiteral("Ein spaeterer Batch-Schritt verweist auf Objekte, die erst durch vorherige Schritte entstehen. Qt prueft jeden Schritt vor der Ausfuehrung erneut mit BRX."));
                readyProposal.insert(QStringLiteral("preflightWarnings"), warningValues);
                readyProposal.insert(QStringLiteral("deferredRuntimePreflight"), true);
                appendBridgeLog("BRX Preflight: runtime-abhaengiger Selector wird bis zur Einzelaktion zurueckgestellt");
                releaseReadyProposal(readyProposal, QStringLiteral("runtime-selector"));
                return;
            }
            clearAgentProposal();

            const bool validationResultAvailable = !result.isEmpty();
            if (validationResultAvailable
                && retryAgentAfterValidationFailure(
                    rejectedContent,
                    rejectedObject,
                    QString("BRX Preflight hat den action_proposal abgelehnt. Korrigiere tool/params oder frage fehlende Daten ab.\n%1").arg(message))) {
                return;
            }

            setAgentBusy(false);
            appendAgentChat("Barebone-Qt", QString("BRX Preflight abgelehnt: %1").arg(message));
        });

    if (!queued) {
        setAgentBusy(false);
        clearAgentProposal();
        appendBridgeLog("AI Agent: actions.validate konnte nicht gesendet werden");
        appendAgentChat("Barebone-Qt", "BRX Preflight konnte nicht gesendet werden. Vorschlag wurde nicht zur Bestaetigung freigegeben.");
    }
}

void BricsCadPage::handleAgentContextRequest(const QJsonObject& request)
{
    // Accept both the canonical flat shape and the response-v2 wrapper used by
    // some local models: {type:"context_request", request:{...}}.
    QJsonObject normalizedRequest = request.value(QStringLiteral("request")).toObject();
    if (normalizedRequest.isEmpty()) {
        normalizedRequest = request;
    }
    QString method = normalizedRequest.value(QStringLiteral("method")).toString().trimmed();
    QJsonObject params = normalizedRequest.value(QStringLiteral("params")).toObject();

    // Domain requests are a supported compact AI form. Resolve them to one
    // explicit read-only bridge method before applying the allowlist.
    if (method.isEmpty()) {
        QJsonArray domains = normalizedRequest.value(QStringLiteral("domains")).toArray();
        if (domains.isEmpty()) {
            domains = request.value(QStringLiteral("domains")).toArray();
        }
        const QStringList requestedDomains = stringsFromJsonArray(domains);
        if (requestedDomains.contains(QStringLiteral("selection"), Qt::CaseInsensitive)) {
            method = QStringLiteral("selection.describe");
            if (params.isEmpty()) {
                params = QJsonObject{
                    {QStringLiteral("include"), QJsonArray{
                        QStringLiteral("geometry"),
                        QStringLiteral("metrics"),
                        QStringLiteral("dimensions"),
                    }},
                    {QStringLiteral("limit"), 100},
                };
            }
        } else if (requestedDomains.contains(QStringLiteral("layers"), Qt::CaseInsensitive)) {
            method = QStringLiteral("layers.list");
        } else if (requestedDomains.contains(QStringLiteral("drawing"), Qt::CaseInsensitive)
            || requestedDomains.contains(QStringLiteral("geometry"), Qt::CaseInsensitive)
            || requestedDomains.contains(QStringLiteral("objects"), Qt::CaseInsensitive)) {
            method = QStringLiteral("geometry.query");
            if (params.isEmpty()) {
                params = QJsonObject{
                    {QStringLiteral("selector"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("currentSpace")}}},
                    {QStringLiteral("include"), QJsonArray{QStringLiteral("geometry"), QStringLiteral("metrics")}},
                    {QStringLiteral("limit"), 100},
                };
            }
        }
    }
    normalizedRequest.insert(QStringLiteral("method"), method);
    normalizedRequest.insert(QStringLiteral("params"), params);

    if (isChatWorkspace()) {
        appendAgentChat("Barebone-Qt", "AI Kontextabfrage abgelehnt: Im Chat Modus sind keine BricsCAD-Funktionen erlaubt.");
        appendBridgeLog(QString("AI Kontextabfrage blockiert: Chat Modus erlaubt kein BRX method=%1")
            .arg(method.isEmpty() ? QStringLiteral("<leer>") : method));
        return;
    }

    if (method == QStringLiteral("geometry.query")) {
        QJsonObject pointSelector = params.value(QStringLiteral("selector")).toObject();
        if (pointSelector.value(QStringLiteral("kind")).toString().compare(QStringLiteral("point"), Qt::CaseInsensitive) == 0) {
            pointSelector.insert(QStringLiteral("kind"), QStringLiteral("entity"));
            params.insert(QStringLiteral("selector"), pointSelector);
            QJsonObject filters = params.value(QStringLiteral("filters")).toObject();
            filters.insert(QStringLiteral("types"), QJsonArray{QStringLiteral("AcDbPoint")});
            params.insert(QStringLiteral("filters"), filters);
            normalizedRequest.insert(QStringLiteral("params"), params);
            appendBridgeLog("AI Kontextabfrage normalisiert: geometry.query selector.kind=point -> kind=entity + filters.types=AcDbPoint");
        }
    }
    if (!routeAllowsCadContext(m_lastAgentRoute)) {
        appendAgentChat("Barebone-Qt", "AI Kontextabfrage abgelehnt: Diese Route erlaubt keinen BricsCAD-Kontext.");
        return;
    }
    if (!isAllowedContextMethod(method)) {
        appendAgentChat("Barebone-Qt", QString("AI Kontextabfrage abgelehnt: %1 ist nicht als readOnly Methode freigegeben.")
            .arg(method.isEmpty() ? "<leer>" : method));
        return;
    }
    if (!m_brxAuthenticated) {
        appendAgentChat("Barebone-Qt", "AI Kontextabfrage kann nicht ausgefuehrt werden: BRX Plugin ist nicht verbunden.");
        return;
    }

    appendBridgeLog(QString("AI -> Qt Kontextabfrage: %1 %2")
        .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));

    const bool queued = sendBridgeRequest(
        method,
        params,
        15000,
        [this, normalizedRequest, method](const QJsonObject& response) {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            drawingContextStore().updateFromContextResponse(
                method,
                normalizedRequest.value(QStringLiteral("params")).toObject(),
                response);
            if (method == QStringLiteral("selection.describe")) {
                m_currentSelection = response.value(QStringLiteral("ok")).toBool(false)
                    ? response.value(QStringLiteral("result")).toObject().value(QStringLiteral("objects")).toArray()
                    : QJsonArray{};
            }
            if (!response.value("ok").toBool(false)) {
                const QString message = bridgeErrorMessage(response, "Kontextabfrage fehlgeschlagen");
                appendAgentChat("BRX", QString("Kontextabfrage %1 fehlgeschlagen: %2").arg(method, message));
                appendBridgeLog(QString("BRX -> Qt: ERROR context %1 %2").arg(method, message));
            } else {
                const QJsonObject result = response.value("result").toObject();
                const int count = result.value("count").toInt(result.value("layers").toArray().size());
                appendBridgeLog(QString("BRX -> Qt Kontext: %1 count=%2").arg(method).arg(count));
            }
            continueAgentWithContextResult(normalizedRequest, response);
        });

    if (!queued) {
        appendAgentChat("Barebone-Qt", QString("AI Kontextabfrage %1 konnte nicht an BRX gesendet werden.").arg(method));
    }
}

void BricsCadPage::continueAgentWithContextResult(const QJsonObject& contextRequest, const QJsonObject& contextResponse)
{
    QJsonObject route = normalizedAgentRouteForMode(m_lastAgentRoute, m_lastAgentUserPrompt, m_lastDocumentContext, m_chatMode);
    if (route.value("route").toString() == QStringLiteral("general_chat")
        || route.value("route").toString() == QStringLiteral("document_qa")) {
        route = normalizedAgentRouteForMode(
            makeAgentRoute(QStringLiteral("bricscad_question"), QStringLiteral("CAD-Kontextabfrage")),
            m_lastAgentUserPrompt,
            m_lastDocumentContext,
            m_chatMode);
    }

    QJsonObject envelope = agentRequestEnvelope(
        m_lastAgentUserPrompt.isEmpty() ? QStringLiteral("Nutze den abgefragten BricsCAD-Kontext.") : m_lastAgentUserPrompt,
        m_lastDocumentContext,
        route);
    envelope.insert("type", "context_result");
    envelope.insert("request", contextRequest);
    envelope.insert("response", contextResponse);
    const QString signature = QStringLiteral("%1:%2")
        .arg(contextRequest.value(QStringLiteral("method")).toString(),
            QString::fromUtf8(QJsonDocument(contextRequest.value(QStringLiteral("params")).toObject()).toJson(QJsonDocument::Compact)));
    const QJsonObject result = contextResponse.value(QStringLiteral("result")).toObject();
    const int count = result.value(QStringLiteral("count")).toInt(
        result.value(QStringLiteral("objects")).toArray().size());
    if (count == 0 && signature == m_lastAgentContextRequestSignature) {
        ++m_repeatedAgentContextRequestCount;
    } else {
        m_lastAgentContextRequestSignature = signature;
        m_repeatedAgentContextRequestCount = count == 0 ? 1 : 0;
    }

    if (count == 0 && m_repeatedAgentContextRequestCount >= 3) {
        envelope.insert(QStringLiteral("contextLoopGuard"), QJsonObject{
            {QStringLiteral("requestSignature"), signature},
            {QStringLiteral("emptyResultCount"), m_repeatedAgentContextRequestCount},
            {QStringLiteral("furtherIdenticalRequestsAllowed"), false},
            {QStringLiteral("policy"), QStringLiteral(
                "Die identische leere Kontextabfrage darf nicht erneut gesendet werden. Entscheide anhand von Prompt, Verlauf und vorhandenen Capabilities, "
                "ob ein anderer read-only Kontext benoetigt wird, eine Rueckfrage sinnvoll ist oder ein Vorschlag erstellt werden kann.")},
        });
    }

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(
        envelope,
        compact,
        false,
        QStringLiteral("drawing_context_result"));
}

void BricsCadPage::executeAgentProposal()
{
    QString errorMessage;
    if (!validateAgentProposal(m_pendingAgentProposal, errorMessage)) {
        appendAgentChat("Barebone-Qt", QString("Vorschlag kann nicht ausgefuehrt werden: %1").arg(errorMessage));
        clearAgentProposal();
        return;
    }

    const QJsonObject executedProposal = m_pendingAgentProposal;
    const QJsonArray actions = agentProposalActions(executedProposal);

    if (actions.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Vorschlag kann nicht ausgefuehrt werden: keine Aktionen gefunden");
        clearAgentProposal();
        return;
    }

    clearLastBricsCadExecution();
    clearAgentProposal();
    saveCurrentAgentSession();
    setAgentBusy(true);

    const bool automaticReadOnly = !executedProposal.value(QStringLiteral("requiresConfirmation")).toBool(true);
    if (actions.size() > 1) {
        appendBridgeLog(QString("AI Agent: %1; fuehre %2 Aktionen nacheinander aus")
            .arg(automaticReadOnly ? QStringLiteral("read-only automatisch") : QStringLiteral("Nutzer bestaetigt"))
            .arg(actions.size()));
    } else {
        const QJsonObject action = actions.first().toObject();
        appendBridgeLog(QString("AI Agent: %1; fuehre %2 ueber %3 aus")
            .arg(automaticReadOnly ? QStringLiteral("read-only automatisch") : QStringLiteral("Nutzer bestaetigt"),
                action.value("tool").toString(),
                bridgeMethodForTool(action.value("tool").toString())));
    }

    executeAgentActionBatch(executedProposal, actions, 0, {});
}

void BricsCadPage::executeAgentActionBatch(const QJsonObject& proposal, const QJsonArray& actions, int index, QJsonArray results)
{
    if (index >= actions.size()) {
        const int total = actions.size();
        const QString fallbackTool = !results.isEmpty()
            ? results.first().toObject().value("tool").toString()
            : proposal.value("tool").toString();
        const QString resultSummary = total > 1
            ? QString("Batch ausgefuehrt: %1 Aktionen abgeschlossen.").arg(total)
            : QString("%1 wurde erfolgreich ausgefuehrt.").arg(fallbackTool);
        const QJsonObject executionStats = executionStatsForActions(actions, results);

        QJsonObject batchResult{
            {"schema", "barebone.qt.agent.batch.result.v1"},
            {"summary", resultSummary},
            {"actionsRequested", total},
            {"actionsCompleted", total},
            {"failed", executionStats.value("failed").toInt(0)},
            {"executionStats", executionStats},
            {"results", results},
        };
        const QString geometryTablesMarkdown = brxGeometryResultTablesMarkdown(results);
        if (!geometryTablesMarkdown.isEmpty()) {
            batchResult.insert(QStringLiteral("geometryTablesMarkdown"), geometryTablesMarkdown);
        }
        const QJsonObject compactBatchResult = compactBatchResultForAgent(batchResult);
        m_lastAgentToolResult = compactBatchResult;
        recordLastBricsCadExecution(proposal, actions, results, batchResult);
        drawingContextStore().recordActionBatchResult(batchResult);
        m_pendingAgentDraft = {};
        m_agentValidationRetries = 0;
        clearAgentProposal();

        appendBridgeLog(QString("BRX Batch: %1").arg(QString::fromUtf8(QJsonDocument(batchResult).toJson(QJsonDocument::Compact)).left(1600)));

        const QString finalSummary = agentCompletionSummary(actions, results, resultSummary);
        m_agentConversation.append(QJsonObject{
            {"role", "assistant"},
            {"content", QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "tool_result"},
                {"message", finalSummary},
                {"status", "completed"},
                {"batch", total > 1},
                {"result", compactBatchResult},
            }).toJson(QJsonDocument::Compact))},
        });

        if (proposal.value("continueAfterSuccess").toBool(false)) {
            setAgentBusy(false);
            const QJsonObject response = actions.size() == 1 && !results.isEmpty()
                ? results.first().toObject().value("response").toObject()
                : batchResult;
            continueAgentAfterToolResult(proposal, response);
        } else if (!proposal.value(QStringLiteral("requiresConfirmation")).toBool(true)
            && std::any_of(actions.begin(), actions.end(), [](const QJsonValue& value) {
                return value.toObject().value(QStringLiteral("tool")).toString()
                    == QStringLiteral("bim.objects.query");
            })) {
            setAgentBusy(false);
            appendAgentChat(QStringLiteral("AI"), geometryTablesMarkdown.isEmpty()
                ? finalSummary
                : QStringLiteral("%1\n\n%2").arg(finalSummary, geometryTablesMarkdown),
                bricsCadExecutionMessageExtra());
        } else {
            requestAgentExecutionSummary(proposal, actions, results, batchResult, finalSummary);
        }
        return;
    }

    const QJsonObject action = actions.at(index).toObject();
    const QString tool = action.value("tool").toString();
    const QString bridgeMethod = bridgeMethodForTool(tool);
    const bool needsBimQueryHandles = action.value(QStringLiteral("params")).toObject()
        .value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false);
    QJsonObject params = paramsWithRuntimeBatchHandles(tool, action.value("params").toObject(), results);
    params = paramsWithConcreteHandleSelector(tool, params, m_lastAgentUserPrompt, m_currentSelection);
    if (needsBimQueryHandles) {
        const QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
        if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("handles"), Qt::CaseInsensitive) != 0
            || selector.value(QStringLiteral("handles")).toArray().isEmpty()) {
            const QString message = QStringLiteral("Runtime-Bindung fuer %1 lieferte keine konkreten BIM-Handles aus der vorherigen bim.objects.query-Abfrage; BRX-Aufruf wurde verhindert.").arg(tool);
            appendBridgeLog(QStringLiteral("Qt Runtime Guard: %1").arg(message));
            QJsonObject failedProposal = proposal;
            failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
            failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{{QStringLiteral("tool"), tool}, {QStringLiteral("params"), params}});
            continueAgentAfterToolFailure(failedProposal, QJsonObject{}, message);
            return;
        }
    }
    if (tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids")) {
        const QJsonObject selector = params.value(QStringLiteral("selector")).toObject();
        if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("handles"), Qt::CaseInsensitive) != 0
            || selector.value(QStringLiteral("handles")).toArray().isEmpty()) {
            const QString message = QStringLiteral("Runtime-Bindung fuer %1 lieferte keine konkreten Polylinienhandles; BRX-Aufruf wurde verhindert.").arg(tool);
            appendBridgeLog(QStringLiteral("Qt Runtime Guard: %1").arg(message));
            QJsonObject failedProposal = proposal;
            failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
            failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{{QStringLiteral("tool"), tool}, {QStringLiteral("params"), params}});
            continueAgentAfterToolFailure(failedProposal, QJsonObject{}, message);
            return;
        }
    }
    auto actionModifiesDrawing = [this](const QJsonObject& candidate) {
        const QString candidateTool = candidate.value(QStringLiteral("tool")).toString();
        const QJsonObject definition = toolDefinition(candidateTool);
        if (definition.isEmpty()) {
            return true;
        }
        const QString risk = definition.value(QStringLiteral("risk")).toString(QStringLiteral("modifiesDrawing"));
        return risk != QStringLiteral("readOnly")
            && definition.value(QStringLiteral("confirmationRequired")).toBool(true);
    };
    const bool currentActionModifiesDrawing = actionModifiesDrawing(action);
    bool previousMutatingActionExecuted = false;
    for (int priorIndex = 0; priorIndex < index; ++priorIndex) {
        if (actionModifiesDrawing(actions.at(priorIndex).toObject())) {
            previousMutatingActionExecuted = true;
            break;
        }
    }
    if (currentActionModifiesDrawing) {
        params.insert("saveBefore", !m_agentPreActionSaveCompleted && !previousMutatingActionExecuted);
    } else {
        params.remove("saveBefore");
    }

    auto executeCurrentAction = [this, proposal, actions, index, results, tool, bridgeMethod, currentActionModifiesDrawing](const QJsonObject& validatedParams) mutable {
        const QJsonObject params = validatedParams;
        appendBridgeLog(QString("Qt -> BRX Batch %1/%2: %3 saveBefore=%4")
            .arg(index + 1)
            .arg(actions.size())
            .arg(bridgeMethod)
            .arg(params.value("saveBefore").toBool(false) ? "true" : "false"));

        const bool queued = sendBridgeActionRequest(
            bridgeMethod,
            params,
            30000,
            [this, proposal, actions, index, results, tool, bridgeMethod, params, currentActionModifiesDrawing](const QJsonObject& response) mutable {
                appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
                if (!response.value("ok").toBool(false)) {
                    const QString message = bridgeErrorMessage(response, "Tool-Ausfuehrung fehlgeschlagen");
                    appendAgentChat("BRX", QString("%1 fehlgeschlagen: %2").arg(tool, message));
                    appendBridgeLog(QString("BRX -> Qt: ERROR %1 %2").arg(bridgeMethod, message));
                    QJsonObject failedProposal = proposal;
                    failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
                    failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{
                        {"tool", tool},
                        {"params", params},
                    });
                    failedProposal.insert(QStringLiteral("completedResults"), compactToolStepResultsForAgent(results));
                    failedProposal.insert(QStringLiteral("failurePolicy"),
                        QStringLiteral("Die Batch-Ausfuehrung wurde bei dieser Aktion gestoppt. Korrigiere den gesamten verbleibenden Vorschlag oder frage gezielt nach."));
                    continueAgentAfterToolFailure(
                        failedProposal,
                        compactBrxResponseForAgent(response),
                        QStringLiteral("Batch-Aktion %1/%2 (%3) fehlgeschlagen: %4")
                            .arg(index + 1)
                            .arg(actions.size())
                            .arg(tool)
                            .arg(message));
                    return;
                }

                const QJsonObject result = response.value("result").toObject();
                if (currentActionModifiesDrawing) {
                    m_agentPreActionSaveCompleted = true;
                }
                drawingContextStore().updateFromContextResponse(bridgeMethod, params, response);
                if (tool == QStringLiteral("pipes.validateNetwork")
                    && result.contains(QStringLiteral("valid"))
                    && !result.value(QStringLiteral("valid")).toBool(false)) {
                    const QString message = QStringLiteral("pipes.validateNetwork meldet valid=false (connected=%1, insideFloorPlan=%2, startConnected=%3, components=%4).")
                        .arg(result.value(QStringLiteral("connected")).toBool(false) ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(result.value(QStringLiteral("insideFloorPlan")).toBool(false) ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(result.value(QStringLiteral("startConnected")).toBool(false) ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(result.value(QStringLiteral("components")).toInt());
                    appendBridgeLog(QStringLiteral("BRX -> Qt: Netzvalidierung fehlgeschlagen: %1").arg(message));
                    QJsonObject failedProposal = proposal;
                    failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
                    failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{{QStringLiteral("tool"), tool}, {QStringLiteral("params"), params}});
                    continueAgentAfterToolFailure(failedProposal, compactBrxResponseForAgent(response), message);
                    return;
                }
                m_lastAgentToolResult = compactBrxResponseForAgent(result);
                m_pendingAgentDraft = {};
                m_agentValidationRetries = 0;

                appendBridgeLog(QString("BRX -> Qt: %1 ausgefuehrt result=%2")
                    .arg(bridgeMethod, QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(1000)));

                const QJsonObject stepResult{
                    {"index", index + 1},
                    {"tool", tool},
                    {"bridgeMethod", bridgeMethod},
                    {"params", params},
                    {"response", response},
                    {"result", result},
                };
                results.append(stepResult);

                QString workflowGateError;
                if (!floorPlanBatchVerificationGatePassed(proposal, actions, index, results, workflowGateError)) {
                    const QJsonObject gateAction = actions.at(index).toObject();
                    const bool finalCreatedGeometryInspection =
                        gateAction.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.query")
                        && gateAction.value(QStringLiteral("params")).toObject().value(QStringLiteral("inspection")).toString()
                            == QStringLiteral("createdGeometryBatch");
                    const QString message = finalCreatedGeometryInspection
                        ? QStringLiteral(
                            "Batch-Ausfuehrung abgeschlossen, aber die abschliessende Geometriepruefung ist fehlgeschlagen. %1")
                            .arg(workflowGateError)
                        : QStringLiteral(
                            "Batch-Ausfuehrung gestoppt: Die Innenkontur-Pruefung ist fehlgeschlagen; die Aussenwaende wurden nicht erzeugt. %1")
                            .arg(workflowGateError);
                    appendBridgeLog(QStringLiteral("Qt Workflow-Gate: %1").arg(message));
                    appendAgentChat(QStringLiteral("Barebone-Qt"), message);

                    const QJsonObject executionStats = executionStatsForActions(actions, results);
                    const QJsonObject batchResult{
                        {QStringLiteral("schema"), QStringLiteral("barebone.qt.agent.batch.result.v1")},
                        {QStringLiteral("summary"), message},
                        {QStringLiteral("actionsRequested"), actions.size()},
                        {QStringLiteral("actionsCompleted"), results.size()},
                        {QStringLiteral("failed"), 1},
                        {QStringLiteral("stoppedBy"), QStringLiteral("workflowVerificationGate")},
                        {QStringLiteral("executionStats"), executionStats},
                        {QStringLiteral("results"), results},
                    };
                    m_lastAgentToolResult = compactBatchResultForAgent(batchResult);
                    drawingContextStore().recordActionBatchResult(batchResult);
                    m_pendingAgentDraft = {};
                    m_agentValidationRetries = 0;
                    setAgentBusy(false);
                    return;
                }

                if (index + 1 < actions.size()) {
                    QTimer::singleShot(kAgentBatchActionDelayMs, this, [this, proposal, actions, index, results]() mutable {
                        executeAgentActionBatch(proposal, actions, index + 1, results);
                    });
                } else {
                    executeAgentActionBatch(proposal, actions, index + 1, results);
                }
            });
        if (!queued) {
            appendAgentChat("Barebone-Qt", QString("BRX Plugin ist nicht verbunden. %1 wurde nicht gesendet.").arg(tool));
            setAgentBusy(false);
        }
    };

    if (actions.size() <= 1) {
        executeCurrentAction(params);
        return;
    }

    if (!bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("actions.validate"))) {
        appendAgentChat("Barebone-Qt", "Batch-Ausfuehrung gestoppt: BRX Preflight ist nicht verfuegbar.");
        appendBridgeLog("AI Batch: actions.validate fehlt fuer Einzelaktions-Preflight");
        setAgentBusy(false);
        return;
    }

    QJsonArray preflightActions;
    preflightActions.append(QJsonObject{
        {"tool", brxValidationToolName(tool)},
        {"params", params},
    });
    const QJsonObject preflightParams{
        {"source", "agent_batch_step_preflight"},
        {"actions", preflightActions},
    };

    appendBridgeLog(QString("Qt -> BRX: actions.validate Batch-Aktion %1/%2")
        .arg(index + 1)
        .arg(actions.size()));

    const bool preflightQueued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        preflightParams,
        15000,
        [this, proposal, actions, index, results, tool, bridgeMethod, params, executeCurrentAction](const QJsonObject& response) mutable {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            drawingContextStore().ingestValidationResult(QJsonObject{
                {QStringLiteral("source"), QStringLiteral("agent_batch_step_preflight")},
                {QStringLiteral("index"), index + 1},
                {QStringLiteral("tool"), tool},
                {QStringLiteral("params"), params},
            }, response);
            const bool valid = response.value("ok").toBool(false) && result.value("valid").toBool(false);
            if (valid) {
                appendBridgeLog(QString("BRX Preflight: Batch-Aktion %1/%2 gueltig")
                    .arg(index + 1)
                    .arg(actions.size()));
                QJsonObject validatedParams = params;
                if (tool.startsWith(QStringLiteral("bim."))) {
                    const QJsonArray validationActions = result.value(QStringLiteral("actions")).toArray();
                    if (!validationActions.isEmpty()) {
                        validatedParams = paramsWithBimValidationTargets(
                            validatedParams, validationActions.first().toObject());
                    }
                }
                executeCurrentAction(validatedParams);
                return;
            }

            const QString message = validationFailureMessage(response);
            appendBridgeLog(QString("BRX Preflight: Batch-Aktion %1/%2 abgelehnt: %3")
                .arg(index + 1)
                .arg(actions.size())
                .arg(message.left(700).replace('\n', " | ")));
            QJsonObject failedProposal = proposal;
            failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
            failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{
                {"tool", tool},
                {"params", params},
            });
            failedProposal.insert(QStringLiteral("completedResults"), compactToolStepResultsForAgent(results));
            failedProposal.insert(QStringLiteral("failurePolicy"),
                QStringLiteral("Die Batch-Ausfuehrung wurde vor dieser Aktion gestoppt. Korrigiere den gesamten verbleibenden Vorschlag oder frage gezielt nach."));
            continueAgentAfterToolFailure(
                failedProposal,
                compactBrxResponseForAgent(response),
                QStringLiteral("Batch-Aktion %1/%2 (%3) wurde im BRX Preflight abgelehnt: %4")
                    .arg(index + 1)
                    .arg(actions.size())
                    .arg(tool)
                    .arg(message));
        });
    if (!preflightQueued) {
        appendAgentChat("Barebone-Qt", "Batch-Ausfuehrung gestoppt: BRX Plugin ist nicht verbunden. actions.validate wurde nicht gesendet.");
        setAgentBusy(false);
    }
}

void BricsCadPage::requestAgentExecutionSummary(
    const QJsonObject& proposal,
    const QJsonArray& actions,
    const QJsonArray& results,
    const QJsonObject& batchResult,
    const QString& fallbackSummary)
{
    appendBridgeLog("AI Agent: fasse BRX Ergebnis fuer den Chat zusammen");
    const QJsonObject summaryRoute = normalizedAgentRouteForMode(
        makeAgentRoute(QStringLiteral("execution_summary"), QStringLiteral("Abschlusszusammenfassung")),
        QStringLiteral("Fasse die abgeschlossene BricsCAD-Ausfuehrung kurz zusammen."),
        m_lastDocumentContext,
        m_chatMode);
    m_lastAgentRoute = summaryRoute;

    QJsonObject envelope = agentRequestEnvelope(
        QStringLiteral("Fasse die abgeschlossene BricsCAD-Ausfuehrung kurz zusammen."),
        m_lastDocumentContext,
        summaryRoute);
    const QJsonArray compactResults = compactToolStepResultsForAgent(results);
    const QJsonObject compactBatchResult = compactBatchResultForAgent(batchResult);
    envelope.insert("type", "execution_summary");
    envelope.insert("completedProposal", proposal);
    envelope.insert("executedActions", actions);
    envelope.insert("toolResults", compactResults);
    envelope.insert("batchResult", compactBatchResult);
    envelope.insert("executionStats", batchResult.value("executionStats").toObject(executionStatsForActions(actions, results)));
    const QString geometryTablesMarkdown = brxGeometryResultTablesMarkdown(results);
    if (!geometryTablesMarkdown.isEmpty()) {
        envelope.insert(QStringLiteral("geometryTablesMarkdown"), geometryTablesMarkdown);
        envelope.insert(QStringLiteral("geometryTablesInstruction"),
            QStringLiteral("Wenn geometryTablesMarkdown vorhanden ist, uebernimm diese Markdown-Tabelle in die Chatnachricht und ergaenze hoechstens einen kurzen Einordnungssatz."));
    }
    envelope.insert("fallbackSummary", fallbackSummary);
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
    envelope.insert("includeConversationHistory", true);
    envelope.insert("instruction",
        QStringLiteral("Erstelle aus completedProposal, executedActions, executionStats, toolResults und batchResult eine kurze Abschlussnachricht im ChatGPT-Stil. "
        "Antworte ausschliesslich mit genau einem JSON-Objekt: {\"type\":\"message\",\"message\":\"...\"}. "
        "%1 Schreibe natuerlich, maximal zwei kurze Saetze. "
        "Wenn geometryTablesMarkdown vorhanden ist, gib die Tabelle vollstaendig als Markdown-Pipe-Tabelle in message aus; dann darf message laenger als zwei Saetze sein. "
        "Nutze executionStats fuer konkrete Zahlen, z.B. wie viele Layer neu angelegt wurden, wie viele boxSolidsCreated entstanden sind und wie viele bimWallsClassified wurden. "
        "Zaehle Layer-Aktionen nicht als Wandkoerper oder Geometrieobjekte. "
        "Erwaehne keine internen Qt-/BRX-Details, keine JSON-Daten, keine Validierung und keine Denkprozesse. "
        "Behaupte nur, was in den Ergebnissen erfolgreich abgeschlossen wurde. "
        "Falls die Ergebnisse unklar sind, nutze fallbackSummary als Grundlage.")
            .arg(aiLanguageInstruction(m_config)));

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    setAgentBusy(false);
    sendAgentEnvelope(envelope, compact, false, "execution_summary");
}

bool BricsCadPage::selectedWorkflowVerificationPassed(const QJsonObject& response, QString& errorMessage) const
{
    errorMessage.clear();
    QJsonObject activeWorkflow;
    for (const QJsonValue& value : selectedWorkflowObjectsForRoute(m_lastAgentRoute)) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")).toString()
            == QStringLiteral("grundriss_und_aussenwaende_einzeichnen")) {
            activeWorkflow = candidate;
            break;
        }
    }
    if (activeWorkflow.isEmpty()) {
        return true;
    }

    QJsonObject queryResult;
    QJsonObject areaResult;
    QJsonObject bboxResult;
    bool containsVerificationAction = false;
    const QJsonArray results = response.value(QStringLiteral("results")).toArray();
    for (const QJsonValue& value : results) {
        const QJsonObject step = value.toObject();
        const QString tool = step.value(QStringLiteral("tool")).toString();
        const QJsonObject result = step.value(QStringLiteral("result")).toObject();
        if (tool == QStringLiteral("geometry.query")) {
            queryResult = result;
            containsVerificationAction = true;
        } else if (tool == QStringLiteral("measurement.area")) {
            areaResult = result;
            containsVerificationAction = true;
        } else if (tool == QStringLiteral("measurement.bbox")) {
            bboxResult = result;
            containsVerificationAction = true;
        }
    }
    // The preceding contour-creation proposal also continues, but it has no
    // verification actions yet. Only the dedicated read-only batch is gated.
    if (!containsVerificationAction) {
        return true;
    }
    if (queryResult.isEmpty() || areaResult.isEmpty() || bboxResult.isEmpty()) {
        errorMessage = QStringLiteral(
            "Der Pruef-Batch muss geometry.query, measurement.area und measurement.bbox gemeinsam ausfuehren.");
        return false;
    }

    const auto calculationContour = [this, &response]() {
        QJsonObject calculation = response.value(QStringLiteral("calculation")).toObject();
        QJsonObject contour = calculation.value(QStringLiteral("contour")).toObject();
        if (!contour.isEmpty()) {
            return contour;
        }
        calculation = m_lastAgentRoute.value(QStringLiteral("calculationResult")).toObject();
        return calculation.value(QStringLiteral("contour")).toObject();
    };

    const QJsonObject contour = calculationContour();
    double expectedWidth = contour.value(QStringLiteral("width")).toDouble(0.0);
    double expectedLength = contour.value(QStringLiteral("depth")).toDouble(0.0);

    if (expectedWidth <= 0.0 || expectedLength <= 0.0) {
        QJsonObject values = activeWorkflow.value(QStringLiteral("knownSlotValues")).toObject();
        if (m_selectedWorkflow.value(QStringLiteral("id")).toString()
            == activeWorkflow.value(QStringLiteral("id")).toString()) {
            for (auto it = m_selectedWorkflowSlotValues.begin(); it != m_selectedWorkflowSlotValues.end(); ++it) {
                values.insert(it.key(), it.value());
            }
        }
        const QJsonObject explicitValues = workflowTrainingSlotValuesFromPrompt(
            m_lastAgentUserPrompt, QJsonArray{}, activeWorkflow);
        for (auto it = explicitValues.constBegin(); it != explicitValues.constEnd(); ++it) {
            values.insert(it.key(), it.value());
        }
        const bool explicitArea = explicitValues.contains(QStringLiteral("roomAreaM2"));
        const bool explicitWidth = explicitValues.contains(QStringLiteral("roomWidthMm"));
        const bool explicitLength = explicitValues.contains(QStringLiteral("roomLengthMm"));
        expectedWidth = values.value(QStringLiteral("roomWidthMm")).toDouble(10000.0);
        expectedLength = values.value(QStringLiteral("roomLengthMm")).toDouble(10000.0);
        const double requestedAreaMm2 = values.value(QStringLiteral("roomAreaM2")).toDouble(100.0) * 1000000.0;
        if (explicitArea && !explicitWidth && !explicitLength) {
            expectedWidth = std::sqrt(requestedAreaMm2);
            expectedLength = expectedWidth;
        } else if (explicitArea && explicitWidth && !explicitLength && expectedWidth > 0.0) {
            expectedLength = requestedAreaMm2 / expectedWidth;
        } else if (explicitArea && !explicitWidth && explicitLength && expectedLength > 0.0) {
            expectedWidth = requestedAreaMm2 / expectedLength;
        }
    }
    const double expectedArea = expectedWidth * expectedLength;
    constexpr double dimensionToleranceMm = 1.0;
    constexpr double areaToleranceMm2 = 10000.0;

    if (queryResult.value(QStringLiteral("count")).toInt(-1) != 1) {
        errorMessage = QStringLiteral("geometry.query lieferte nicht exakt eine neu erstellte Rechteckkontur.");
        return false;
    }
    if (areaResult.value(QStringLiteral("measured")).toInt(-1) != 1
        || areaResult.value(QStringLiteral("failed")).toInt(-1) != 0) {
        errorMessage = QStringLiteral("measurement.area hat die neue Innenkontur nicht eindeutig und fehlerfrei gemessen.");
        return false;
    }
    const double actualArea = areaResult.value(QStringLiteral("totalArea")).toDouble(-1.0);
    if (actualArea < 0.0 || std::abs(actualArea - expectedArea) > areaToleranceMm2) {
        errorMessage = QStringLiteral("Innenflaeche Soll %1 m2, Ist %2 m2 (Toleranz 0,01 m2).")
            .arg(expectedArea / 1000000.0, 0, 'f', 4)
            .arg(actualArea / 1000000.0, 0, 'f', 4);
        return false;
    }
    if (bboxResult.value(QStringLiteral("measured")).toInt(-1) != 1
        || bboxResult.value(QStringLiteral("failed")).toInt(-1) != 0) {
        errorMessage = QStringLiteral("measurement.bbox hat die neue Innenkontur nicht eindeutig und fehlerfrei gemessen.");
        return false;
    }
    const QJsonObject dimensions = bboxResult.value(QStringLiteral("dimensions")).toObject();
    const double actualWidth = dimensions.value(QStringLiteral("widthX")).toDouble(-1.0);
    const double actualLength = dimensions.value(QStringLiteral("depthY")).toDouble(-1.0);
    const double actualHeight = dimensions.value(QStringLiteral("heightZ")).toDouble(-1.0);
    if (actualWidth < 0.0 || actualLength < 0.0 || actualHeight < 0.0
        || std::abs(actualWidth - expectedWidth) > dimensionToleranceMm
        || std::abs(actualLength - expectedLength) > dimensionToleranceMm
        || std::abs(actualHeight) > dimensionToleranceMm) {
        errorMessage = QStringLiteral(
            "Innenmasse Soll %1 ? %2 ? 0 mm, Ist %3 ? %4 ? %5 mm (Toleranz 1 mm).")
            .arg(expectedWidth, 0, 'f', 2).arg(expectedLength, 0, 'f', 2)
            .arg(actualWidth, 0, 'f', 2).arg(actualLength, 0, 'f', 2).arg(actualHeight, 0, 'f', 2);
        return false;
    }

    return true;
}

bool BricsCadPage::floorPlanBatchVerificationGatePassed(
    const QJsonObject& proposal,
    const QJsonArray& actions,
    int completedActionIndex,
    const QJsonArray& results,
    QString& errorMessage) const
{
    errorMessage.clear();
    if (proposal.value(QStringLiteral("workflowBatchMode")).toString()
        != QStringLiteral("floor_plan_interior_verification_and_walls_v1")) {
        return true;
    }
    if (completedActionIndex < 0 || completedActionIndex >= actions.size()) {
        return true;
    }
    const QJsonObject completedAction = actions.at(completedActionIndex).toObject();
    const bool finalCreatedGeometryInspection =
        completedAction.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.query")
        && completedAction.value(QStringLiteral("params")).toObject().value(QStringLiteral("inspection")).toString()
            == QStringLiteral("createdGeometryBatch");
    if (finalCreatedGeometryInspection) {
        const QJsonObject latestResult = results.isEmpty()
            ? QJsonObject{}
            : results.last().toObject().value(QStringLiteral("result")).toObject();
        const QJsonArray expectedHandles = uniqueStringArray(createdGeometryHandlesFromBatchResults(results));
        const QJsonArray objects = latestResult.value(QStringLiteral("objects")).toArray();
        const int actualCount = latestResult.value(QStringLiteral("count")).toInt(objects.size());
        if (expectedHandles.isEmpty() || actualCount != expectedHandles.size()) {
            errorMessage = QStringLiteral("Abschliessende Geometriepruefung erwartete %1 erzeugte Objekte, BRX lieferte %2.")
                .arg(expectedHandles.size())
                .arg(actualCount);
            return false;
        }
        return true;
    }
    if (completedAction.value(QStringLiteral("tool")).toString() != QStringLiteral("measurement.bbox")) {
        return true;
    }

    bool hasFollowingWallMutation = false;
    for (int i = completedActionIndex + 1; i < actions.size(); ++i) {
        const QJsonObject action = actions.at(i).toObject();
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (action.value(QStringLiteral("tool")).toString() == QStringLiteral("geometry.create")
            && params.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("box"), Qt::CaseInsensitive) == 0) {
            hasFollowingWallMutation = true;
            break;
        }
    }
    if (!hasFollowingWallMutation) {
        return true;
    }

    const QJsonObject verificationResponse{
        {QStringLiteral("results"), results},
        {QStringLiteral("calculation"), proposal.value(QStringLiteral("calculation")).toObject()},
    };
    return selectedWorkflowVerificationPassed(verificationResponse, errorMessage);
}

void BricsCadPage::continueAgentAfterToolResult(const QJsonObject& proposal, const QJsonObject& response)
{
    if (!proposal.value("continueAfterSuccess").toBool(false)) {
        return;
    }

    const QString nextIntent = proposal.value("nextIntent").toString().trimmed();
    if (nextIntent.isEmpty()) {
        appendBridgeLog("AI Agent: continueAfterSuccess ohne nextIntent ignoriert");
        return;
    }

    QString verificationError;
    if (!selectedWorkflowVerificationPassed(response, verificationError)) {
        appendBridgeLog(QStringLiteral("Qt Workflow-Pruefung: Batch-Fortsetzung blockiert: %1").arg(verificationError));
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral(
            "Die Grundrisspruefung ist fehlgeschlagen; die A??enwaende wurden nicht erzeugt. %1").arg(verificationError));
        return;
    }

    appendAgentChat("Barebone-Qt", QString("Setze mehrstufigen Ablauf fort: %1").arg(nextIntent));

    QJsonObject envelope = agentRequestEnvelope(
        nextIntent,
        m_lastDocumentContext,
        normalizedAgentRouteForMode(m_lastAgentRoute, nextIntent, m_lastDocumentContext, m_chatMode));
    envelope.insert("type", "tool_result");
    envelope.insert("nextIntent", nextIntent);
    envelope.insert("completedProposal", proposal);
    envelope.insert("toolResponse", compactBrxResponseForAgent(response));
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
    if (proposal.value(QStringLiteral("workflowStage")).toString()
        == QStringLiteral("interior_contour_created")) {
        envelope.insert("instruction", QStringLiteral(
            "Die Innenkontur wurde erstellt. Liefere jetzt genau einen gemeinsamen Read-only-action_proposal mit geometry.query, measurement.area und measurement.bbox fuer selector.scope=lastResult. "
            "Setze continueAfterSuccess=true, nextIntent auf den urspruenglichen Wunsch und workflowStage=interior_contour_verified. Erzeuge in diesem Schritt noch keine Waende."));
    } else {
        envelope.insert("instruction", QStringLiteral(
            "Die vorbereitende Read-only-Aktion ist abgeschlossen. Nutze toolResponse und calculationResult. Wiederhole keine erfolgreiche Abfrage. "
            "Wenn die Workflow-Pruefung erfolgreich war, liefere alle vier berechneten A??enwand-Boxen gemeinsam als action_proposal."));
    }

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, "tool_result");
}

void BricsCadPage::continueAgentAfterToolFailure(
    const QJsonObject& proposal,
    const QJsonObject& response,
    const QString& errorMessage)
{
    if (m_agentValidationRetries >= kMaxAgentRepairRetries) {
        appendBridgeLog(QString("AI Agent Loop: BRX-Fehlerkorrektur nach %1 Versuchen abgebrochen: %2")
            .arg(kMaxAgentRepairRetries)
            .arg(errorMessage));
        appendAgentChat("Barebone-Qt", QString("BRX-Fehler konnte nicht automatisch korrigiert werden: %1").arg(errorMessage));
        return;
    }

    ++m_agentValidationRetries;
    appendBridgeLog(QString("AI Agent Loop %1/%2: BRX-Ausfuehrung fehlgeschlagen, korrigiere Vorschlag: %3")
        .arg(m_agentValidationRetries)
        .arg(kMaxAgentRepairRetries)
        .arg(errorMessage.left(240)));
    m_lastAgentRoute = normalizedAgentRouteForMode(
        makeAgentRoute(QStringLiteral("validation_retry"), QStringLiteral("BRX-Fehlerkorrektur")),
        m_lastAgentUserPrompt,
        m_lastDocumentContext,
        m_chatMode);
    QJsonObject envelope = agentRequestEnvelope(m_lastAgentUserPrompt.isEmpty()
        ? QStringLiteral("Korrigiere die fehlgeschlagene BricsCAD-Aktion.")
        : m_lastAgentUserPrompt,
        m_lastDocumentContext,
        m_lastAgentRoute);
    envelope.insert("type", "tool_error");
    envelope.insert("executionError", errorMessage);
    envelope.insert("failedProposal", proposal);
    envelope.insert("toolResponse", response);
    envelope.insert("context", currentAgentContext());
    envelope.insert("capabilities", QJsonObject{});
    envelope.insert("capabilitySummary", capabilitySummary(m_brxCapabilities));
    const QJsonArray effectiveTools = availableAgentToolsForRoute(
        normalizedAgentRouteForMode(m_lastAgentRoute, m_lastAgentUserPrompt, m_lastDocumentContext, m_chatMode),
        m_lastAgentUserPrompt);
    const QJsonObject failedActionForRepair = firstRepairActionFromObject(QJsonObject{
        {QStringLiteral("proposal"), proposal},
        {QStringLiteral("failedAction"), proposal.value(QStringLiteral("failedAction")).toObject()},
    });
    QJsonObject repairToolParams{
        {QStringLiteral("phase"), QStringLiteral("execution")},
        {QStringLiteral("retry"), m_agentValidationRetries},
        {QStringLiteral("prompt"), m_lastAgentUserPrompt},
        {QStringLiteral("executionError"), errorMessage},
        {QStringLiteral("error"), errorMessage},
        {QStringLiteral("failedProposal"), proposal},
        {QStringLiteral("toolResponse"), response},
    };
    if (!failedActionForRepair.isEmpty()) {
        repairToolParams.insert(QStringLiteral("failedTool"), failedActionForRepair.value(QStringLiteral("tool")).toString());
        repairToolParams.insert(QStringLiteral("failedParams"), failedActionForRepair.value(QStringLiteral("params")).toObject());
    }
    const QJsonArray allRepairTools = availableAgentTools();
    QJsonObject repairMode = BrxAgent::repairToolContext(allRepairTools, repairToolParams);
    repairMode.insert(QStringLiteral("recentExecution"), drawingContextStore().executionHistory(QJsonObject{
        {QStringLiteral("limit"), 8},
    }));
    repairMode.insert(QStringLiteral("drawingContext"), drawingContextStore().inspect(QJsonObject{
        {QStringLiteral("limit"), 40},
    }));
    envelope.insert(QStringLiteral("repairMode"), repairMode);
    const QStringList repairCandidateNames = stringsFromJsonArray(repairMode.value(QStringLiteral("candidateToolNames")).toArray());
    const QJsonArray augmentedTools = mergeToolArraysByName(
        effectiveTools,
        BrxAgent::toolsByNames(allRepairTools, repairCandidateNames));
    QJsonArray boundedTools = augmentedTools;
    while (boundedTools.size() > 8) {
        boundedTools.removeAt(boundedTools.size() - 1);
    }
    envelope.insert("effectiveTools", boundedTools);
    envelope.insert("repairCandidateTools", BrxAgent::toolsByNames(allRepairTools, repairCandidateNames));
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentRepairRetries},
        {"policy", "The user-confirmed tool call failed in BRX. Correct the next response instead of repeating the same proposal."},
    });
    envelope.insert("instruction",
        "Die bestaetigte Aktion wurde von BRX abgelehnt. Wiederhole nicht denselben Vorschlag. "
        "Nutze executionError, failedProposal, repairMode.candidateToolNames, repairCandidateTools und effectiveTools, um params oder tool zu korrigieren. "
        "Wenn du eine korrigierte Aktion ausfuehren willst, antworte mit genau einem action_proposal. "
        "Nutze dabei keine direkten BricsCAD-DB-Schreibvorgaenge, keine AcDb-/LayerTable-/EntityTable-Mutationen und keine Pseudo-Tools; nur effectiveTools[].name ist erlaubt. "
        "Wenn echte Informationen fehlen, nutze ask_user. Wenn Sitzungsverlauf fehlt, nutze context_request mit conversationAccess.allowedMethods. "
        "Wenn Zeichnungskontext fehlt, nutze context_request mit method und params; fuer Auswahl immer method=selection.describe. "
        "scope=selection ist bereits ein gueltiger Selector und darf nicht erneut beim Nutzer erfragt werden. Nach einem verlorenen Pickfirst-Set lies die Auswahl read-only und plane mit den gelieferten Handles. "
        "Wenn der urspruengliche Wunsch mehrere unabhaengige Aktionen hat, nutze actions[] statt continueAfterSuccess. "
        "Nutze continueAfterSuccess nur, wenn das Ergebnis einer Aktion fuer die naechste AI-Entscheidung benoetigt wird.");

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, QString("tool_error_loop_%1").arg(m_agentValidationRetries));
}

void BricsCadPage::appendAgentChat(const QString& speaker, const QString& message, const QVariantMap& extra)
{
    if (!m_agentBridge) {
        return;
    }

    const QString visibleMessage = repairMojibakeText(message);

    QVariantMap payload{
        {"speaker", speaker},
        {"message", visibleMessage.trimmed()},
        {"time", QDateTime::currentDateTime().toString("HH:mm 'Uhr'")},
    };
    for (auto it = extra.cbegin(); it != extra.cend(); ++it) {
        payload.insert(it.key(), it.value());
    }
    if (speaker == QStringLiteral("AI") && m_nextAgentMessageContinuationAvailable) {
        payload.insert(QStringLiteral("continuationAvailable"), true);
        payload.insert(QStringLiteral("continuationReason"), m_nextAgentMessageContinuationReason);
    }
    if (speaker == QStringLiteral("AI")) {
        QString messageId = payload.value(QStringLiteral("messageId")).toString().trimmed();
        if (messageId.isEmpty()) {
            messageId = QStringLiteral("ai-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            payload.insert(QStringLiteral("messageId"), messageId);
        }
        QJsonObject bricsCadExecutionRecord = QJsonObject::fromVariantMap(
            payload.value(QStringLiteral("bricsCadExecutionRecord")).toMap());
        if (!bricsCadExecutionRecord.isEmpty()) {
            bricsCadExecutionRecord.insert(QStringLiteral("messageId"), messageId);
            const QString executionId = bricsCadExecutionRecord.value(QStringLiteral("executionId")).toString().trimmed();
            if (!executionId.isEmpty()) {
                payload.insert(QStringLiteral("bricsCadExecutionId"), executionId);
            }
            payload.insert(QStringLiteral("workflowSaveEligible"),
                bricsCadExecutionRecord.value(QStringLiteral("saveEligible")).toBool(false));
            payload.insert(QStringLiteral("bricsCadExecutionRecord"), bricsCadExecutionRecord.toVariantMap());
            if (m_lastBricsCadExecution.value(QStringLiteral("executionId")).toString() == executionId) {
                m_lastBricsCadExecution = bricsCadExecutionRecord;
            }
        }
        const QString visible = visibleMessage.trimmed();
        bool conversationUpdated = false;
        for (int i = m_agentConversation.size() - 1; i >= 0; --i) {
            QJsonObject item = m_agentConversation.at(i).toObject();
            if (item.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")
                || !item.value(QStringLiteral("messageId")).toString().trimmed().isEmpty()) {
                continue;
            }
            const QString content = item.value(QStringLiteral("content")).toString();
            bool parsed = false;
            const QJsonObject parsedObject = jsonObjectFromAiContent(content, &parsed);
            const bool legacyAgentMessage = parsed
                && parsedObject.value(QStringLiteral("schema")).toString()
                    == QStringLiteral("barebone.agent.response.v2")
                && parsedObject.value(QStringLiteral("type")).toString()
                    == QStringLiteral("message")
                && parsedObject.contains(QStringLiteral("message"));
            const QString candidate = legacyAgentMessage
                ? parsedObject.value(QStringLiteral("message")).toString().trimmed()
                : repairMojibakeText(content).trimmed();
            if (candidate == visible) {
                item.insert(QStringLiteral("messageId"), messageId);
                if (!bricsCadExecutionRecord.isEmpty()) {
                    item.insert(QStringLiteral("bricsCadExecutionId"),
                        bricsCadExecutionRecord.value(QStringLiteral("executionId")).toString());
                    item.insert(QStringLiteral("workflowSaveEligible"),
                        bricsCadExecutionRecord.value(QStringLiteral("saveEligible")).toBool(false));
                    item.insert(QStringLiteral("bricsCadExecutionRecord"), bricsCadExecutionRecord);
                }
                m_agentConversation.replace(i, item);
                conversationUpdated = true;
                break;
            }
        }
        if (!bricsCadExecutionRecord.isEmpty() || conversationUpdated) {
            saveCurrentAgentSession();
        }
    }
    emitWebMessage(m_agentBridge, payload);
}

void BricsCadPage::emitSessionTitleSuggestion(const QString& title) const
{
    if (!m_agentBridge) {
        return;
    }
    const QString cleanTitle = cleanedGeneralWorkflowHeading(title).left(64).trimmed();
    if (cleanTitle.isEmpty() || isGenericGeneralWorkflowName(cleanTitle)) {
        return;
    }
    emitToWebAsync(m_agentBridge, [sessionId = m_agentSessionId, cleanTitle](AiWebBridge* target) {
        Q_EMIT target->sessionTitleSuggested(sessionId, cleanTitle);
    });
}

void BricsCadPage::clearAgentProposal()
{
    m_pendingAgentProposal = {};
    if (m_agentBridge) {
        emitWebProposalCleared(m_agentBridge);
    }
}

void BricsCadPage::setAgentWaitingForUser(const QJsonObject& reply)
{
    if (!m_agentBridge) {
        return;
    }

    QStringList lines;
    QString message = repairMojibakeText(reply.value("message").toString()).trimmed();
    const QJsonArray missing = reply.value("missing").toArray();
    if (!missing.isEmpty()) {
        QStringList values;
        for (const QJsonValue& value : missing) {
            const QString text = value.toString();
            if (!text.isEmpty()) {
                values << text;
            }
        }
        if (!values.isEmpty()) {
            lines << QString("Fehlt: %1").arg(values.join(", "));
        }
    }

    if (message.isEmpty()) {
        QStringList readableMissing;
        for (const QJsonValue& value : missing) {
            const QString field = value.toString().trimmed().toLower();
            if (field == QStringLiteral("center") || field == QStringLiteral("origin")) {
                readableMissing << QStringLiteral("Mittelpunkt bzw. Ursprung als x-, y- und z-Koordinate");
            } else if (field == QStringLiteral("point") || field == QStringLiteral("position")) {
                readableMissing << QStringLiteral("Position als x-, y- und z-Koordinate");
            } else if (!field.isEmpty()) {
                readableMissing << repairMojibakeText(value.toString()).trimmed();
            }
        }
        message = readableMissing.isEmpty()
            ? QStringLiteral("Welche konkrete Angabe soll ich für die Ausführung verwenden?")
            : QStringLiteral("Bitte gib noch %1 an.").arg(readableMissing.join(QStringLiteral(", ")));
    }

    emitWebProposal(m_agentBridge, QVariantMap{
        {"title", "Rückfrage"},
        {"summary", message},
        {"details", lines.join("\n")},
        {"canRun", false},
    });
}

void BricsCadPage::setAgentProposal(const QJsonObject& proposal)
{
    m_pendingAgentProposal = proposal;
    m_pendingAgentDraft = {};

    const QJsonArray actions = agentProposalActions(proposal);
    QString summary = repairMojibakeText(proposal.value("summary").toString(
        proposal.value("message").toString())).trimmed();
    if (isGenericPreparedActionText(summary)) {
        summary.clear();
    }
    const auto displayToolDefinition = [this](const QString& name) {
        for (const QJsonValue& value : availableAgentTools()) {
            const QJsonObject tool = value.toObject();
            if (tool.value(QStringLiteral("name")).toString() == name) {
                return tool;
            }
        }
        return QJsonObject{};
    };
    const auto actionDescription = [&displayToolDefinition](const QJsonObject& action, int index) {
        const QString tool = action.value(QStringLiteral("tool")).toString().trimmed();
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        const QJsonObject definition = displayToolDefinition(tool);
        const QString title = repairMojibakeText(definition.value(QStringLiteral("title")).toString(tool)).trimmed();
        const bool saveBefore = params.value(QStringLiteral("saveBefore")).toBool(false);
        const QString saveText = saveBefore
            ? QStringLiteral(" mit vorheriger Sicherung")
            : QString();
        if (tool == QStringLiteral("layers.create")) {
            const QString name = repairMojibakeText(params.value(QStringLiteral("name")).toString()).trimmed();
            return QStringLiteral("%1. Layer \"%2\" per layers.create anlegen%3.")
                .arg(index + 1)
                .arg(name.isEmpty() ? QStringLiteral("<ohne Namen>") : name, saveText);
        }
        if (tool == QStringLiteral("layers.ensureMany")) {
            QStringList names;
            for (const QJsonValue& layerValue : params.value(QStringLiteral("layers")).toArray()) {
                const QString name = layerValue.isObject()
                    ? layerValue.toObject().value(QStringLiteral("name")).toString()
                    : layerValue.toString();
                const QString clean = repairMojibakeText(name).trimmed();
                if (!clean.isEmpty()) {
                    names << clean;
                }
            }
            return QStringLiteral("%1. %2 Layer per layers.ensureMany sicherstellen%3: %4.")
                .arg(index + 1)
                .arg(names.size())
                .arg(saveText, names.isEmpty() ? QStringLiteral("keine Namen angegeben") : names.join(QStringLiteral(", ")));
        }
        if (tool == QStringLiteral("geometry.create")) {
            const QString geometry = repairMojibakeText(params.value(QStringLiteral("geometry")).toString()).trimmed();
            const QString layer = repairMojibakeText(params.value(QStringLiteral("layer")).toString()).trimmed();
            QStringList parts;
            if (!geometry.isEmpty()) {
                parts << QStringLiteral("Geometrie=%1").arg(geometry);
            }
            if (!layer.isEmpty()) {
                parts << QStringLiteral("Layer=%1").arg(layer);
            }
            for (const QString& key : {QStringLiteral("width"), QStringLiteral("depth"), QStringLiteral("height"), QStringLiteral("radius")}) {
                if (params.contains(key)) {
                    parts << QStringLiteral("%1=%2").arg(key, QString::number(params.value(key).toDouble()));
                }
            }
            return QStringLiteral("%1. %2 (%3) ausführen%4%5.")
                .arg(index + 1)
                .arg(title.isEmpty() ? tool : title, tool, saveText,
                    parts.isEmpty() ? QString() : QStringLiteral(" mit %1").arg(parts.join(QStringLiteral(", "))));
        }
        const QString paramsText = repairMojibakeText(QString::fromUtf8(
            QJsonDocument(params).toJson(QJsonDocument::Compact))).trimmed();
        return QStringLiteral("%1. %2 (%3) ausführen%4 mit Parametern %5.")
            .arg(index + 1)
            .arg(title.isEmpty() ? tool : title,
                tool.isEmpty() ? QStringLiteral("<Tool fehlt>") : tool,
                saveText,
                paramsText.isEmpty() ? QStringLiteral("{}") : paramsText);
    };
    const auto plannedExecutionSummary = [&actions, &actionDescription]() {
        if (actions.isEmpty()) {
            return QStringLiteral("Die AI hat eine BricsCAD-Ausführung vorbereitet, aber keine Aktionen geliefert.");
        }
        QStringList descriptions;
        for (int i = 0; i < actions.size() && i < 5; ++i) {
            descriptions << actionDescription(actions.at(i).toObject(), i);
        }
        if (actions.size() > 5) {
            descriptions << QStringLiteral("Weitere %1 Aktionen folgen im Batch.").arg(actions.size() - 5);
        }
        return actions.size() == 1
            ? QStringLiteral("Geplante BricsCAD-Ausführung: %1").arg(descriptions.first())
            : QStringLiteral("Geplante BricsCAD-Batch-Ausführung mit %1 Aktionen: %2")
                .arg(actions.size())
                .arg(descriptions.join(QStringLiteral(" ")));
    };

    QStringList lines;
    const QString aiDetails = repairMojibakeText(proposal.value(QStringLiteral("details")).toString(
        proposal.value(QStringLiteral("description")).toString())).trimmed();
    if (!aiDetails.isEmpty()) {
        lines << QStringLiteral("Geplante Ausfuehrung:");
        lines << aiDetails;
    }
    if (actions.size() > 1) {
        lines << QString("Batch: %1 Aktionen").arg(actions.size());
        lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
        lines << QStringLiteral("Ausfuehrung: intern als Batch, zu BRX einzeln mit Einzel-Preflight");
        lines << QString("Speichern: nur vor der ersten Aktion");
        lines << QString("Pause zwischen Aktionen: %1 ms").arg(kAgentBatchActionDelayMs);
        for (int i = 0; i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
            const QString tool = action.value("tool").toString();
            const QString paramsText = QString::fromUtf8(QJsonDocument(action.value("params").toObject()).toJson(QJsonDocument::Compact));
            lines << QString("%1. %2 %3").arg(i + 1).arg(tool.isEmpty() ? QStringLiteral("<fehlt>") : tool, paramsText);
        }
    } else {
        const QJsonObject action = actions.isEmpty() ? QJsonObject{} : actions.first().toObject();
        const QString tool = action.value("tool").toString();
        const QJsonObject params = action.value("params").toObject();
        const QJsonObject definition = displayToolDefinition(tool);
        const QString paramsText = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));

        lines << QString("Werkzeug: %1").arg(definition.value("title").toString(tool));
        lines << QString("Name: %1").arg(tool.isEmpty() ? QStringLiteral("<fehlt>") : tool);
        lines << QString("Kategorie: %1").arg(definition.value("category").toString("general"));
        lines << QString("Bridge: %1").arg(definition.value("bridgeMethod").toString(tool));
        lines << QString("Risiko: %1").arg(definition.value("risk").toString("modifiesDrawing"));
        lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
        lines << QString("Parameter: %1").arg(paramsText.isEmpty() ? QStringLiteral("{}") : paramsText);
    }

    const QJsonArray validatedActions = proposal.value(QStringLiteral("preflightValidation")).toObject()
        .value(QStringLiteral("actions")).toArray();
    for (int i = 0; i < validatedActions.size(); ++i) {
        const QJsonObject validatedAction = validatedActions.at(i).toObject();
        const QStringList handles = jsonStringArrayToStringList(
            validatedAction.value(QStringLiteral("resolvedHandles")).toArray());
        const QJsonArray fingerprints = validatedAction.value(QStringLiteral("targetFingerprints")).toArray();
        if (handles.isEmpty() && fingerprints.isEmpty()) {
            continue;
        }
        const int clientActionIndex = validatedAction.value(QStringLiteral("clientActionIndex")).toInt(-1);
        lines << QString("BRX-Ziele Aktion %1: %2")
            .arg(clientActionIndex >= 0 ? clientActionIndex + 1 : i + 1)
            .arg(handles.isEmpty() ? QStringLiteral("keine") : handles.mid(0, 12).join(QStringLiteral(", ")));
        for (int fingerprintIndex = 0; fingerprintIndex < fingerprints.size() && fingerprintIndex < 12; ++fingerprintIndex) {
            const QJsonObject fingerprint = fingerprints.at(fingerprintIndex).toObject();
            lines << QString("- %1 | %2 | %3")
                .arg(fingerprint.value(QStringLiteral("handle")).toString(QStringLiteral("?")),
                    fingerprint.value(QStringLiteral("bimType")).toString(QStringLiteral("?")),
                    fingerprint.value(QStringLiteral("guid")).toString(QStringLiteral("?")));
        }
    }

    const QStringList warnings = stringsFromJsonArray(proposal.value("preflightWarnings").toArray());
    if (!warnings.isEmpty()) {
        lines << QString("BRX-Hinweise:");
        for (const QString& warning : warnings.mid(0, 5)) {
            lines << QString("- %1").arg(repairMojibakeText(warning));
        }
        if (warnings.size() > 5) {
            lines << QString("- plus %1 weitere Hinweise").arg(warnings.size() - 5);
        }
    }

    const QString reason = proposal.value("reason").toString().trimmed();
    if (!reason.isEmpty()) {
        lines << QString("Grund: %1").arg(reason);
    }
    const QJsonArray assumptions = proposal.value("assumptions").toArray();
    if (!assumptions.isEmpty()) {
        lines << QString("Annahmen:");
        for (const QJsonValue& value : assumptions) {
            const QString assumption = repairMojibakeText(value.toString()).trimmed();
            if (!assumption.isEmpty()) {
                lines << QString("- %1").arg(assumption);
            }
        }
    }
    const QString nextIntent = proposal.value("nextIntent").toString().trimmed();
    if (!nextIntent.isEmpty()) {
        lines << QString("Danach: %1").arg(nextIntent);
    }

    if (m_agentBridge) {
        const QString displaySummary = summary.isEmpty() ? plannedExecutionSummary() : summary;
        emitWebProposal(m_agentBridge, QVariantMap{
            {"title", actions.size() > 1 ? "AI Batch-Vorschlag bereit" : "AI Vorschlag bereit"},
            {"summary", displaySummary},
            {"details", lines.join("\n")},
            {"canRun", true},
        });
    }
}

void BricsCadPage::setAgentBusy(bool busy)
{
    if (m_agentBusy == busy) {
        return;
    }
    m_agentBusy = busy;
    if (m_agentBridge) {
        emitWebStatus(m_agentBridge, busy ? QStringLiteral("thinking") : QStringLiteral("idle"));
    }
    if (!busy) {
        processDeferredAgentPrompt();
    }
}

void BricsCadPage::processDeferredAgentPrompt()
{
    if (m_agentBusy || m_deferredAgentPrompts.isEmpty()) {
        return;
    }
    const DeferredAgentPrompt next = m_deferredAgentPrompts.takeFirst();
    QTimer::singleShot(0, this, [this, next]() {
        if (!m_agentBusy) {
            appendBridgeLog(QString("AI Runtime: eingereihten Foreground-Prompt gestartet queue=%1")
                .arg(m_deferredAgentPrompts.size()));
            sendAgentPrompt(next.prompt, next.documentContext);
        } else {
            m_deferredAgentPrompts.prepend(next);
        }
    });
}

void BricsCadPage::cancelCurrentOperation()
{
    ++m_operationGeneration;
    m_parallelPreparationActive = false;
    m_activeReasoningRunId.clear();
    m_deferredAgentPrompts.clear();
    if (m_bricsCadCoordinator) {
        m_bricsCadCoordinator->cancel();
    }
    if (m_aiRuntime) {
        m_aiRuntime->abortAll();
    }

    m_workflowTestActions = {};

    if (m_aiNetwork) {
        const QList<QNetworkReply*> replies = m_aiNetwork->findChildren<QNetworkReply*>();
        for (QNetworkReply* reply : replies) {
            if (reply && reply->isRunning() && reply->operation() != QNetworkAccessManager::GetOperation) {
                reply->abort();
            }
        }
    }

    const QList<int> requestIds = m_pendingRequests.keys();
    for (int id : requestIds) {
        PendingBridgeRequest pending = m_pendingRequests.take(id);
        if (pending.timeout) {
            pending.timeout->stop();
            pending.timeout->deleteLater();
        }
    }

    clearAgentProposal();
    m_pendingAgentDraft = {};
    m_pendingAgentProposal = {};
    m_queuedAgentPrompt.clear();
    m_queuedAgentRoute = {};
    m_lastFocusedConversationContext = {};
    m_agentValidationRetries = 0;
    m_trainingValidationRetries = 0;
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    m_trainingFinalSavePending = false;
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    clearWorkflowTrainingPrompts();
    setAgentBusy(false);

    appendBridgeLog("AI Agent: laufender Prozess durch Nutzer gestoppt");
    appendAgentChat("Barebone-Qt", "Laufender Prozess wurde gestoppt.");
}

bool BricsCadPage::isAgentConfirmation(const QString& prompt) const
{
    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    return normalized == "ja"
        || normalized == "ok"
        || normalized == "ausfuehren"
        || normalized == QStringLiteral("ausfÃƒÂ¼hren")
        || normalized == "mach"
        || normalized == "mach das"
        || normalized == "bestaetigen"
        || normalized == QStringLiteral("bestÃƒÂ¤tigen");
}

bool BricsCadPage::validateAgentProposal(const QJsonObject& proposal, QString& errorMessage) const
{
    if (proposal.isEmpty()) {
        errorMessage = "kein offener Vorschlag";
        return false;
    }

    QJsonArray sourceActions = proposal.value("actions").toArray();
    if (sourceActions.isEmpty() && !proposal.value("tool").toString().trimmed().isEmpty()) {
        sourceActions.append(QJsonObject{
            {"tool", proposal.value("tool").toString()},
            {"params", proposal.value("params").toObject()},
        });
    }
    for (const QJsonValue& value : sourceActions) {
        const QJsonObject action = normalizedAgentAction(value.toObject());
        if (action.value("tool").toString() == QStringLiteral("layers.ensureMany")
            && !validateLayersEnsureManyParams(action.value("params").toObject(), errorMessage)) {
            return false;
        }
    }

    const QJsonArray actions = agentProposalActions(proposal);
    if (actions.isEmpty()) {
        errorMessage = QStringLiteral("tool oder actions fehlen");
        return false;
    }
    if (actions.size() > kMaxAgentBatchActions) {
        errorMessage = QStringLiteral("Batch enthaelt %1 Aktionen, erlaubt sind maximal %2")
            .arg(actions.size())
            .arg(kMaxAgentBatchActions);
        return false;
    }
    bool priorBimQuery = false;
    for (int i = 0; i < actions.size(); ++i) {
        const QJsonObject action = actions.at(i).toObject();
        if (action.value(QStringLiteral("continueAfterSuccess")).toBool(false)
            || !action.value(QStringLiteral("nextIntent")).toString().trimmed().isEmpty()) {
            errorMessage = QStringLiteral("Batch-Aktion %1 darf keine eigene Folgeausfuehrung verwenden").arg(i + 1);
            return false;
        }
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
            && !priorBimQuery) {
            errorMessage = QStringLiteral("Batch-Aktion %1 nutzt eine BIM-Ergebnisreferenz ohne vorherige bim.objects.query-Abfrage")
                .arg(i + 1);
            return false;
        }
        QString actionError;
        if (!validateAgentAction(action, actionError)) {
            errorMessage = QStringLiteral("Batch-Aktion %1 ist nicht gueltig: %2").arg(i + 1).arg(actionError);
            return false;
        }
        priorBimQuery = priorBimQuery
            || action.value(QStringLiteral("tool")).toString() == QStringLiteral("bim.objects.query");
    }
    if (!proposal.value(QStringLiteral("nextIntent")).toString().trimmed().isEmpty()
        && !proposal.value(QStringLiteral("continueAfterSuccess")).toBool(false)) {
        errorMessage = QStringLiteral("nextIntent ist gesetzt, aber continueAfterSuccess ist false");
        return false;
    }
    return true;

    // Legacy semantic workflow validation below is unreachable. It is kept
    // only until saved workflows using its private conventions are migrated.
    if (m_selectedWorkflowId == QStringLiteral("workflow_01_floor_plan_contour")
        || m_selectedWorkflowId == QStringLiteral("workflow_02_exterior_walls")) {
        int extrusionCount = 0;
        bool coversAllFourProfiles = false;
        QSet<int> individuallyExtrudedIndexes;
        for (const QJsonValue& actionValue : actions) {
            const QJsonObject action = actionValue.toObject();
            if (action.value(QStringLiteral("tool")).toString() != QStringLiteral("rectangles.extrude")) {
                continue;
            }
            ++extrusionCount;
            const QJsonObject params = action.value(QStringLiteral("params")).toObject();
            const QJsonArray indexes = params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray();
            if (params.contains(QStringLiteral("createdGeometryHandleIndex"))) {
                individuallyExtrudedIndexes.insert(params.value(QStringLiteral("createdGeometryHandleIndex")).toInt(-1));
            }
            const bool mergedContourIndexes = indexes.size() == 4
                && indexes.at(0).toInt(-1) == 1
                && indexes.at(1).toInt(-1) == 2
                && indexes.at(2).toInt(-1) == 3
                && indexes.at(3).toInt(-1) == 4;
            coversAllFourProfiles = coversAllFourProfiles
                || mergedContourIndexes
                || (params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)
                    && indexes.size() == 4
                    && mergedContourIndexes);
        }
        const bool fourExplicitWallExtrusions = extrusionCount == 4
            && individuallyExtrudedIndexes == QSet<int>({1, 2, 3, 4});
        if (!coversAllFourProfiles && !fourExplicitWallExtrusions) {
            errorMessage = "Der kombinierte Grundrissworkflow muss alle vier Aussenwandprofile extrudieren. Die Kontur mit lokalem Geometrieindex 0 ist ausgeschlossen; gemeinsam sind ausschliesslich die Indizes 1 bis 4 zulaessig. Eine einzelne scope=lastResult-Extrusion ist verboten.";
            return false;
        }
    }
    if (m_selectedWorkflowId == QStringLiteral("workflow_03_interior_walls")) {
        bool coversAllFivePartitions = false;
        for (const QJsonValue& actionValue : actions) {
            const QJsonObject action = actionValue.toObject();
            if (action.value(QStringLiteral("tool")).toString() != QStringLiteral("rectangles.extrude")) {
                continue;
            }
            const QJsonObject params = action.value(QStringLiteral("params")).toObject();
            coversAllFivePartitions = params.value(QStringLiteral("createdGeometryHandleIndexes")).toArray().size() == 5;
        }
        if (!coversAllFivePartitions) {
            errorMessage = "Workflow 03 muss die fuenf Innenwandprofile gemeinsam ueber die lokalen createdGeometryHandleIndexes 0 bis 4 extrudieren. scope=lastResult allein ist verboten.";
            return false;
        }
    }
    if (isSystemRouteWorkflowId(m_selectedWorkflowId)
        && m_selectedWorkflowId != QStringLiteral("workflow_05_system_pipe_contours")) {
        int pointCount = 0;
        int pointQueryIndex = -1;
        int polylineCount = 0;
        int validationIndex = -1;
        bool minimumClearanceValid = true;
        const int requiredClearanceMm = m_selectedWorkflowId == QStringLiteral("workflow_06_ventilation_route") ? 400 : 100;
        for (int i = 0; i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
            const QString tool = action.value(QStringLiteral("tool")).toString();
            const QJsonObject params = action.value(QStringLiteral("params")).toObject();
            const QString geometry = params.value(QStringLiteral("geometry")).toString();
            if (tool == QStringLiteral("geometry.create") && geometry.compare(QStringLiteral("point"), Qt::CaseInsensitive) == 0) ++pointCount;
            if (tool == QStringLiteral("geometry.query") && params.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)) pointQueryIndex = i;
            if (tool == QStringLiteral("geometry.create") && geometry.compare(QStringLiteral("polyline"), Qt::CaseInsensitive) == 0) ++polylineCount;
            if (tool == QStringLiteral("pipes.validateNetwork")) {
                validationIndex = i;
                minimumClearanceValid = minimumClearanceValid
                    && params.value(QStringLiteral("minimumClearanceMm")).toInt(0) >= requiredClearanceMm;
            }
        }
        if (pointCount < 3 || pointQueryIndex < 0 || polylineCount < 1 || validationIndex < 0 || validationIndex <= pointQueryIndex) {
            errorMessage = "Fachnetzworkflow braucht Punkt-Erzeugung, gebundenen Punkt-Readback, mindestens eine Polylinie aus Readback-Punkten und abschliessendes pipes.validateNetwork in dieser Reihenfolge.";
            return false;
        }
        if (!minimumClearanceValid) {
            errorMessage = QStringLiteral("Fachnetzworkflow unterschreitet den verbindlichen Mindestabstand von %1 mm.")
                .arg(requiredClearanceMm);
            return false;
        }
    }
    if (actions.size() > 1) {
        if (actions.size() > kMaxAgentBatchActions) {
            errorMessage = QString("Batch enthaelt %1 Aktionen, erlaubt sind maximal %2").arg(actions.size()).arg(kMaxAgentBatchActions);
            return false;
        }
        if (proposal.value("continueAfterSuccess").toBool(false)
            || !proposal.value("nextIntent").toString().trimmed().isEmpty()) {
            bool allReadOnly = true;
            for (const QJsonValue& value : actions) {
                const QJsonObject definition = toolDefinition(value.toObject().value(QStringLiteral("tool")).toString());
                const bool readOnly = !definition.isEmpty()
                    && (definition.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")
                        || definition.value(QStringLiteral("kind")).toString() == QStringLiteral("query")
                        || !definition.value(QStringLiteral("confirmationRequired")).toBool(true));
                allReadOnly = allReadOnly && readOnly;
            }
            if (!allReadOnly) {
                errorMessage = "Nur ein ausschliesslich aus Read-only-Abfragen bestehender Vorbereitungs-Batch darf continueAfterSuccess/nextIntent verwenden";
                return false;
            }
        }
        bool priorCreatedGeometryInBatch = false;
        bool priorBimQueryInBatch = false;
        for (int i = 0; i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
            const QString actionTool = action.value(QStringLiteral("tool")).toString();
            const QJsonObject actionParams = action.value(QStringLiteral("params")).toObject();
            if (actionParams.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
                && !priorBimQueryInBatch) {
                errorMessage = QString("Batch-Aktion %1 (%2) nutzt autoBimHandlesFromLastQuery ohne vorherige bim.objects.query-Abfrage")
                    .arg(i + 1).arg(actionTool);
                return false;
            }
            if (actionTool == QStringLiteral("rectangles.extrude")
                || actionTool == QStringLiteral("circle.extrude")
                || actionTool == QStringLiteral("profile.extrude")) {
                const bool virtualBimCreate = action.value(QStringLiteral("virtualSource")).toString()
                    == QStringLiteral("bim.create");
                const QString targetLayer = actionParams.value(QStringLiteral("layer")).toString().trimmed();
                const QJsonObject actionSelector = actionParams.value(QStringLiteral("selector")).toObject();
                const QString selectorKind = actionSelector.value(QStringLiteral("kind")).toString().trimmed();
                const QString selectorShape = actionSelector.value(QStringLiteral("shape")).toString().trimmed();
                const bool profileCircleSelector = actionTool == QStringLiteral("profile.extrude")
                    && (selectorKind.compare(QStringLiteral("circle"), Qt::CaseInsensitive) == 0
                        || selectorShape.compare(QStringLiteral("circle"), Qt::CaseInsensitive) == 0);
                const bool layerOptionalWithSourceProfile = actionTool == QStringLiteral("circle.extrude")
                    || profileCircleSelector
                    || virtualBimCreate;
                if (layerOptionalWithSourceProfile && targetLayer.isEmpty()) {
                    // circle.extrude is backed by profile.extrude. When the
                    // dedicated circle tool is unavailable, profile.extrude
                    // with selector.kind/shape=circle is the compatible
                    // fallback. BRX assigns the created cylinder to the source
                    // circle layer when the user did not request a different
                    // target layer.
                } else {
                if (targetLayer.isEmpty()) {
                    errorMessage = QString("Batch-Aktion %1 (%2) braucht params.layer als expliziten Ziellayer")
                        .arg(i + 1).arg(actionTool);
                    return false;
                }
                if (i + 1 >= actions.size()) {
                    errorMessage = QString("Batch-Aktion %1 (%2) muss unmittelbar von entity.setLayer fuer den erzeugten Solid gefolgt werden")
                        .arg(i + 1).arg(actionTool);
                    return false;
                }
                const QJsonObject layerAction = actions.at(i + 1).toObject();
                const QJsonObject layerParams = layerAction.value(QStringLiteral("params")).toObject();
                const QJsonObject layerSelector = layerParams.value(QStringLiteral("selector")).toObject();
                if (layerAction.value(QStringLiteral("tool")).toString() != QStringLiteral("entity.setLayer")
                    || layerParams.value(QStringLiteral("layer")).toString().trimmed().compare(targetLayer, Qt::CaseInsensitive) != 0
                    || layerSelector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("lastResult"), Qt::CaseInsensitive) != 0) {
                    errorMessage = QString("Batch-Aktion %1 (%2) muss unmittelbar entity.setLayer mit selector.scope=lastResult und layer='%3' nachschalten")
                        .arg(i + 1).arg(actionTool, targetLayer);
                    return false;
                }
                }
            }
            if (priorCreatedGeometryInBatch
                && promptLooksLikeRectangularRoomWallRun(m_lastAgentUserPrompt)
                && (actionTool == QStringLiteral("rectangles.extrude")
                    || actionTool == QStringLiteral("circle.extrude")
                    || actionTool == QStringLiteral("selection.set")
                    || actionTool == QStringLiteral("bim.classify"))) {
                const QJsonObject selector = actionParams.value(QStringLiteral("selector")).toObject();
                if (selector.value(QStringLiteral("scope")).toString().compare(QStringLiteral("currentSpace"), Qt::CaseInsensitive) == 0
                    && selector.contains(QStringLiteral("layer"))
                    && !selector.contains(QStringLiteral("handles"))
                    && !actionParams.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)) {
                    errorMessage = QString("Batch-Aktion %1 ist zu breit: Nach selbst erzeugten Raumwaenden keine currentSpace/Layer-Selektoren fuer bestehende Zeichnungsobjekte verwenden; nutze exakte Handles oder autoHandlesFromBatch.").arg(i + 1);
                    return false;
                }
            }
            if (action.value("continueAfterSuccess").toBool(false)
                || !action.value("nextIntent").toString().trimmed().isEmpty()) {
                errorMessage = QString("Batch-Aktion %1 darf keine eigene Folgeausfuehrung verwenden").arg(i + 1);
                return false;
            }
            QString actionError;
            if (!validateAgentAction(action, actionError)) {
                errorMessage = QString("Batch-Aktion %1 ist nicht gueltig: %2").arg(i + 1).arg(actionError);
                return false;
            }
            if (actionTool == QStringLiteral("geometry.create")) {
                priorCreatedGeometryInBatch = true;
            }
            if (actionTool == QStringLiteral("bim.objects.query")) {
                priorBimQueryInBatch = true;
            }
        }
        return true;
    }

    if (actions.size() == 1) {
        const QJsonObject action = actions.first().toObject();
        const QString nextIntent = proposal.value("nextIntent").toString().trimmed();
        const bool repeatedPrompt = QRegularExpression(QStringLiteral(R"(\b([2-9]|[1-9][0-9]+)\b)")).match(m_lastAgentUserPrompt).hasMatch()
            || textMentionsAny(m_lastAgentUserPrompt.toLower(), {"mehrere", "viele", "zehn", "zwei", "drei", "vier", "fuenf", "fÃƒÂ¼nf", "sechs", "sieben", "acht", "neun"});
        const bool loopLikeNextIntent = textMentionsAny(nextIntent.toLower(), {"naechst", "nÃƒÂ¤chst", "next", "weiter"});
        if (proposal.value("continueAfterSuccess").toBool(false) && repeatedPrompt && loopLikeNextIntent) {
            errorMessage = "Mehrere unabhaengige Wiederholaktionen muessen als action_proposal mit actions[] gebuendelt werden; nicht per continueAfterSuccess einzeln nachfordern";
            return false;
        }
        if (!proposal.value("nextIntent").toString().trimmed().isEmpty()
            && !proposal.value("continueAfterSuccess").toBool(false)) {
            errorMessage = "nextIntent ist gesetzt, aber continueAfterSuccess ist false";
            return false;
        }
        return validateAgentAction(action, errorMessage);
    }

    errorMessage = "tool oder actions fehlen";
    return false;
}

bool BricsCadPage::validateAgentAction(const QJsonObject& action, QString& errorMessage) const
{
    const QString tool = action.value("tool").toString().trimmed();
    if (tool.isEmpty()) {
        errorMessage = "tool fehlt";
        return false;
    }

    const QJsonValue paramsValue = action.value("params");
    if (!paramsValue.isObject()) {
        errorMessage = "params muss ein JSON-Objekt sein";
        return false;
    }

    const QJsonObject definition = toolDefinition(tool);
    if (definition.isEmpty()) {
        errorMessage = QString("unbekanntes oder nicht freigegebenes Tool \"%1\"").arg(tool);
        return false;
    }

    const QString toolKind = definition.value("kind").toString("action");
    const QString toolRisk = definition.value("risk").toString();
    const bool executableBridgeTool = toolKind == QStringLiteral("action")
        || toolKind == QStringLiteral("query")
        || toolRisk == QStringLiteral("readOnly");
    if (!executableBridgeTool) {
        errorMessage = QString("\"%1\" ist kein ausfuehrbares BRX-Bridge-Tool").arg(tool);
        return false;
    }

    const QString bridgeMethod = definition.value("bridgeMethod").toString(tool);
    if (bridgeMethod.isEmpty()) {
        errorMessage = QString("%1 hat keine Bridge-Methode").arg(tool);
        return false;
    }

    if (!m_brxCapabilities.isEmpty()) {
        bool methodKnown = false;
        QString unavailableReason;
        const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
        for (const QJsonValue& value : methods) {
            const QJsonObject method = value.toObject();
            if (method.value("name").toString() == bridgeMethod) {
                methodKnown = bridgeCapabilityMethodAvailable(method);
                unavailableReason = method.value(QStringLiteral("availability")).toObject()
                                        .value(QStringLiteral("reason")).toString(
                                            method.value(QStringLiteral("unavailableReason")).toString());
                break;
            }
        }
        if (!methodKnown) {
            errorMessage = QString("Bridge-Methode \"%1\" ist laut BRX Capabilities nicht verfuegbar%2")
                .arg(bridgeMethod, unavailableReason.trimmed().isEmpty()
                        ? QString()
                        : QStringLiteral(": %1").arg(unavailableReason.trimmed()));
            return false;
        }
    }

    const QJsonObject params = paramsValue.toObject();
    QJsonObject structuralSchema = definition.value(QStringLiteral("inputSchema")).toObject();
    if (params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
        && (tool == QStringLiteral("bim.selection.set") || tool == QStringLiteral("bim.move"))) {
        QJsonArray required;
        for (const QJsonValue& value : structuralSchema.value(QStringLiteral("required")).toArray()) {
            if (value.toString() != QStringLiteral("selector")) {
                required.append(value);
            }
        }
        structuralSchema.insert(QStringLiteral("required"), required);
    }
    if (params.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
        && (tool == QStringLiteral("measurement.area") || tool == QStringLiteral("measurement.bbox"))) {
        QJsonArray required;
        for (const QJsonValue& value : structuralSchema.value(QStringLiteral("required")).toArray()) {
            if (value.toString() != QStringLiteral("selector")) {
                required.append(value);
            }
        }
        structuralSchema.insert(QStringLiteral("required"), required);
    }
    return validateToolParams(params, structuralSchema, errorMessage);

    // Legacy prompt interpretation below is intentionally outside the active
    // validator path. Prompt and workflow plausibility belong to the planning AI.
    QString activePrompt = m_lastAgentUserPrompt;
    const QString draftSourcePrompt = m_pendingAgentDraft.value(QStringLiteral("_sourcePrompt")).toString().trimmed();
    if (!draftSourcePrompt.isEmpty()) {
        activePrompt.append(QLatin1Char('\n')).append(draftSourcePrompt);
    }
    const QString actionValidationContext = action.value(QStringLiteral("_validationContext")).toString().trimmed();
    if (!actionValidationContext.isEmpty()) {
        activePrompt.append(QLatin1Char('\n')).append(actionValidationContext);
    }
    activePrompt = activePrompt.toLower();
    if ((tool == QStringLiteral("geometry.move")
            || tool == QStringLiteral("brx.sdk.entity.transformBy")
            || tool == QStringLiteral("brx.sdk.blockReference.setPosition"))
        && textMentionsAny(activePrompt, {"face", "flaeche", "flÃƒÂ¤che", "seite", "stirnseite", "verlaengern", "verlÃƒÂ¤ngern", "extend"})) {
        errorMessage = QStringLiteral("%1 verschiebt ganze Entities und ist nicht fuer einzelnes Face-Verlaengern freigegeben. Nutze measurement.bbox plus Ersatz-Workflow oder melde fehlende Capability.").arg(tool);
        return false;
    }
    if (promptRequestsEntityRename(activePrompt)
        && tool == QStringLiteral("layers.rename")) {
        errorMessage = "Entity-/BIM-Wand-Umbenennung ist kein Layer-Umbenennen und aktuell nicht als freigegebenes BRX-Action-Tool vorhanden. Nutze keine Layer-Rename-Ersatzaktion; antworte mit message/plan oder hole nur read-only Kontext.";
        return false;
    }

    if ((tool == QStringLiteral("geometry.rotate") || tool == QStringLiteral("brx.sdk.entity.rotateBy"))
        && !promptMentionsRotationAngle(activePrompt)
        && (params.contains(QStringLiteral("angleDeg")) || params.contains(QStringLiteral("angleRad")))) {
        errorMessage = QStringLiteral("%1 darf angleDeg/angleRad nicht aus Beispielen, Lessons oder Defaults raten. Wenn der Nutzer keinen Rotationswinkel nennt, frage mit ask_user gezielt nach dem Winkel.").arg(tool);
        return false;
    }

    if (tool == QStringLiteral("bim.objects.query")) {
        const int limit = params.value(QStringLiteral("limit")).toInt(100);
        if (limit < 1 || limit > 500) {
            errorMessage = "bim.objects.query.params.limit muss zwischen 1 und 500 liegen";
            return false;
        }
        if (params.value(QStringLiteral("offset")).toInt(0) < 0) {
            errorMessage = "bim.objects.query.params.offset darf nicht negativ sein";
            return false;
        }
    }

    if (tool == QStringLiteral("geometry.scale") || tool == QStringLiteral("brx.sdk.entity.scaleBy")) {
        if (params.contains("xFactor") || params.contains("yFactor") || params.contains("zFactor")) {
            errorMessage = QStringLiteral("%1 ist nur fuer uniforme Skalierung mit factor freigegeben; xFactor/yFactor/zFactor sind nicht erlaubt. Fuer Verlaengern in einer Achse measurement.bbox plus Ersatz-Workflow verwenden.").arg(tool);
            return false;
        }
        const double factor = params.value("factor").toDouble(0.0);
        if (factor <= 0.0) {
            errorMessage = QStringLiteral("%1 braucht factor > 0").arg(tool);
            return false;
        }
    }

    if (tool == QStringLiteral("bim.classify") || tool == QStringLiteral("brx.sdk.bim.classification.set")) {
        if (!promptAllowsBimWallClassification(activePrompt)) {
            errorMessage = QStringLiteral("%1 ist nur erlaubt, wenn der Nutzer BIM/Klassifizierung verlangt oder eine klare architektonische Wandaufgabe mit Wandstaerke/Wandhoehe/Raumbezug beschreibt.").arg(tool);
            return false;
        }
        const QJsonObject selector = params.value("selector").toObject();
        if (selector.value("scope").toString().compare(QStringLiteral("currentSpace"), Qt::CaseInsensitive) == 0
            && !selector.contains(QStringLiteral("layer"))
            && !selector.contains(QStringLiteral("handles"))) {
            errorMessage = QStringLiteral("%1 mit selector.scope=currentSpace braucht einen Layer- oder Handle-Filter, damit nicht zu breit klassifiziert wird.").arg(tool);
            return false;
        }
    }
    if (tool == QStringLiteral("layers.create")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        if (!isValidBricsCadLayerName(name)) {
            errorMessage = "layers.create.params.name muss ein nichtleerer Layername ohne Sonderzeichen sein; erlaubt sind Buchstaben, Ziffern, Leerzeichen und deutsche Umlaute.";
            return false;
        }
    }

    QJsonObject inputSchema = definition.value("inputSchema").toObject();
    if (params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
        && tool != QStringLiteral("bim.selection.set")
        && tool != QStringLiteral("bim.move")) {
        errorMessage = QStringLiteral("%1 darf autoBimHandlesFromLastQuery nicht verwenden; BIM-Folgeaktionen muessen bim.move oder bim.selection.set nutzen.").arg(tool);
        return false;
    }

    if (params.value(QStringLiteral("autoBimHandlesFromLastQuery")).toBool(false)
        && (tool == QStringLiteral("bim.selection.set")
            || tool == QStringLiteral("bim.move"))) {
        QJsonArray requiredWithoutRuntimeSelector;
        for (const QJsonValue& value : inputSchema.value(QStringLiteral("required")).toArray()) {
            if (value.toString() != QStringLiteral("selector")) {
                requiredWithoutRuntimeSelector.append(value);
            }
        }
        inputSchema.insert(QStringLiteral("required"), requiredWithoutRuntimeSelector);
    }
    if (params.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
        && (tool == QStringLiteral("measurement.area")
            || tool == QStringLiteral("measurement.bbox"))) {
        QJsonArray requiredWithoutRuntimeSelector;
        for (const QJsonValue& value : inputSchema.value(QStringLiteral("required")).toArray()) {
            if (value.toString() != QStringLiteral("selector")) {
                requiredWithoutRuntimeSelector.append(value);
            }
        }
        inputSchema.insert(QStringLiteral("required"), requiredWithoutRuntimeSelector);
    }
    return validateToolParams(params, inputSchema, errorMessage);
}

bool BricsCadPage::validateLayersEnsureManyParams(const QJsonObject& params, QString& errorMessage) const
{
    const QJsonArray layers = params.value("layers").toArray();
    if (layers.isEmpty()) {
        errorMessage = "layers.ensureMany braucht params.layers mit mindestens einem Layer";
        return false;
    }
    if (layers.size() > kMaxAgentBatchActions) {
        errorMessage = QString("layers.ensureMany enthaelt %1 Layer, erlaubt sind maximal %2")
            .arg(layers.size())
            .arg(kMaxAgentBatchActions);
        return false;
    }

    for (int i = 0; i < layers.size(); ++i) {
        const QJsonObject layer = layers.at(i).toObject();
        const QString name = layer.value("name").toString().trimmed();
        if (name.isEmpty()) {
            errorMessage = QString("layers.ensureMany.layers[%1].name fehlt").arg(i);
            return false;
        }
        if (!isValidBricsCadLayerName(name)) {
            errorMessage = QString("layers.ensureMany.layers[%1].name muss ein Layername ohne Sonderzeichen sein; erlaubt sind Buchstaben, Ziffern, Leerzeichen und deutsche Umlaute").arg(i);
            return false;
        }
        if (layer.contains("colorIndex") && !layer.value("colorIndex").isNull()) {
            const int colorIndex = layer.value("colorIndex").toInt(-1);
            if (colorIndex < 1 || colorIndex > 255) {
                errorMessage = QString("layers.ensureMany.layers[%1].colorIndex muss zwischen 1 und 255 liegen").arg(i);
                return false;
            }
        }
    }
    return true;
}

bool BricsCadPage::validateToolParams(const QJsonObject& params, const QJsonObject& inputSchema, QString& errorMessage) const
{
    if (inputSchema.isEmpty()) {
        errorMessage = "Tool hat kein inputSchema";
        return false;
    }
    return validateSchemaValue(params, inputSchema, "params", errorMessage);
}

bool BricsCadPage::validateSchemaValue(const QJsonValue& value, const QJsonObject& schema, const QString& path, QString& errorMessage) const
{
    const QString type = schema.value("type").toString();
    if (!schema.value("const").isUndefined() && value != schema.value("const")) {
        QJsonArray expected;
        expected.append(schema.value("const"));
        errorMessage = QString("%1 muss %2 sein")
            .arg(path, QString::fromUtf8(QJsonDocument(expected).toJson(QJsonDocument::Compact)));
        return false;
    }

    const QJsonArray allowed = schema.value("enum").toArray();
    if (!allowed.isEmpty()) {
        bool matches = false;
        for (const QJsonValue& allowedValue : allowed) {
            if (value == allowedValue) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            errorMessage = QString("%1 ist nicht in der erlaubten Enum-Liste").arg(path);
            return false;
        }
    }

    if (type == "object" || (!schema.contains("type") && value.isObject())) {
        if (!value.isObject()) {
            errorMessage = QString("%1 muss ein Objekt sein").arg(path);
            return false;
        }
        const QJsonObject object = value.toObject();
        const QJsonArray required = schema.value("required").toArray();
        for (const QJsonValue& requiredValue : required) {
            const QString key = requiredValue.toString();
            if (!object.contains(key) || object.value(key).isUndefined() || object.value(key).isNull()) {
                errorMessage = QString("%1.%2 fehlt").arg(path, key);
                return false;
            }
        }

        const QJsonArray oneOfRequired = schema.value("oneOfRequired").toArray();
        if (!oneOfRequired.isEmpty()) {
            QStringList alternatives;
            bool matchedAlternative = false;
            for (const QJsonValue& groupValue : oneOfRequired) {
                QStringList keys;
                if (groupValue.isArray()) {
                    const QJsonArray group = groupValue.toArray();
                    for (const QJsonValue& keyValue : group) {
                        const QString key = keyValue.toString();
                        if (!key.isEmpty()) {
                            keys << key;
                        }
                    }
                } else {
                    const QString key = groupValue.toString();
                    if (!key.isEmpty()) {
                        keys << key;
                    }
                }
                if (keys.isEmpty()) {
                    continue;
                }

                bool groupMatches = true;
                for (const QString& key : keys) {
                    if (!object.contains(key) || object.value(key).isUndefined() || object.value(key).isNull()) {
                        groupMatches = false;
                        break;
                    }
                }
                alternatives << keys.join("+");
                if (groupMatches) {
                    matchedAlternative = true;
                    break;
                }
            }

            if (!matchedAlternative) {
                errorMessage = QString("%1 braucht eine dieser Feldgruppen: %2").arg(path, alternatives.join(" oder "));
                return false;
            }
        }

        const QJsonObject properties = schema.value("properties").toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (!object.contains(it.key()) || object.value(it.key()).isUndefined()) {
                continue;
            }
            if (!validateSchemaValue(object.value(it.key()), it.value().toObject(), path + "." + it.key(), errorMessage)) {
                return false;
            }
        }
        return true;
    }

    if (type == "array") {
        if (!value.isArray()) {
            errorMessage = QString("%1 muss eine Liste sein").arg(path);
            return false;
        }
        const QJsonObject itemSchema = schema.value("items").toObject();
        if (!itemSchema.isEmpty()) {
            const QJsonArray array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) {
                if (!validateSchemaValue(array.at(i), itemSchema, QString("%1[%2]").arg(path).arg(i), errorMessage)) {
                    return false;
                }
            }
        }
        return true;
    }

    if (type == "string" && !value.isString()) {
        errorMessage = QString("%1 muss Text sein").arg(path);
        return false;
    }
    if (type == "number" && !value.isDouble()) {
        errorMessage = QString("%1 muss eine Zahl sein").arg(path);
        return false;
    }
    if (type == "boolean" && !value.isBool()) {
        errorMessage = QString("%1 muss true/false sein").arg(path);
        return false;
    }
    if (type == "number" && schema.contains("minimum") && value.toDouble() < schema.value("minimum").toDouble()) {
        errorMessage = QString("%1 muss mindestens %2 sein").arg(path).arg(schema.value("minimum").toDouble());
        return false;
    }

    return true;
}

static ParsedModelOutput parseModelOutput(const QJsonObject& response, AgentResponseKind responseKind)
{
    ParsedModelOutput parsed;
    QStringList reasoningParts;
    QStringList finalParts;
    parsed.truncated = aiResponseWasTruncated(response);

    const auto appendUnique = [](QStringList& list, const QString& value) {
        const QString text = value.trimmed();
        if (!text.isEmpty() && !list.contains(text)) {
            list << text;
        }
    };

    const QJsonArray choices = response.value("choices").toArray();
    if (!choices.isEmpty()) {
        const QJsonObject firstChoice = choices.first().toObject();
        const QJsonObject messageObject = firstChoice.value("message").toObject();
        appendUnique(reasoningParts, messageObject.value("reasoning").toString());
        appendUnique(reasoningParts, messageObject.value("reasoning_content").toString());
        const QJsonObject reasoningObject = messageObject.value("reasoning").toObject();
        appendUnique(reasoningParts, reasoningObject.value("content").toString(
            reasoningObject.value("text").toString()));
        QString content = messageObject.value("content").toString();
        if (content.isEmpty()) {
            content = firstChoice.value("text").toString();
        }
        if (!content.trimmed().isEmpty()) {
            appendUnique(finalParts, content);
        }
    }

    if (finalParts.isEmpty()) {
        const QJsonArray output = response.value("output").toArray();
        for (const QJsonValue& outputValue : output) {
            const QJsonObject item = outputValue.toObject();
            const QString itemType = item.value("type").toString().toLower();
            const bool itemIsReasoning = itemType.contains("reasoning");

            const QString directText = item.value("text").toString();
            if (!directText.trimmed().isEmpty()) {
                appendUnique(itemIsReasoning ? reasoningParts : finalParts, directText);
            }

            const QJsonArray summaryArray = item.value("summary").toArray();
            for (const QJsonValue& summaryValue : summaryArray) {
                if (summaryValue.isString()) {
                    appendUnique(reasoningParts, summaryValue.toString());
                    continue;
                }
                const QJsonObject summaryObject = summaryValue.toObject();
                appendUnique(reasoningParts, summaryObject.value("text").toString(
                    summaryObject.value("summary_text").toString()));
            }

            const QJsonArray contentArray = item.value("content").toArray();
            for (const QJsonValue& contentValue : contentArray) {
                const QJsonObject contentObject = contentValue.toObject();
                const QString contentType = contentObject.value("type").toString().toLower();
                const bool contentIsReasoning = itemIsReasoning
                    || contentType.contains("reasoning")
                    || contentType.contains("summary");
                const QString text = contentObject.value("text").toString(
                    contentObject.value("output_text").toString());
                if (!text.trimmed().isEmpty()) {
                    appendUnique(contentIsReasoning ? reasoningParts : finalParts, text);
                }
            }
        }
    }

    if (!finalParts.isEmpty()) {
        parsed.finalText = finalAiMessageSegment(finalParts.join("\n").trimmed());
    } else {
        parsed.finalText = finalAiMessageSegment(response.value("output_text").toString());
    }
    parsed.reasoning = reasoningParts.join(QStringLiteral("\n\n")).trimmed();

    if (responseKind == AgentResponseKind::StructuredJson && !parsed.finalText.isEmpty()) {
        bool ok = false;
        parsed.structuredValue = jsonObjectFromAiContent(parsed.finalText, &ok);
        if (!ok) {
            parsed.structuredValue = {};
        }
    }
    return parsed;
}

QString BricsCadPage::aiChatCompletionContent(const QJsonObject& response, QString* reasoningText) const
{
    const ParsedModelOutput parsed = parseModelOutput(response, AgentResponseKind::VisibleMarkdown);
    if (reasoningText) {
        *reasoningText = parsed.reasoning;
    }
    return parsed.finalText;
}

QJsonArray BricsCadPage::availableAgentTools() const
{
    if (!kAgentActionToolsEnabled) {
        return {};
    }
    return BrxAgent::buildToolCatalog(m_brxCapabilities);
}

QJsonArray BricsCadPage::availableAgentToolsForRoute(const QJsonObject& route, const QString& prompt) const
{
    if (!isBricsCadMode()) {
        return {};
    }
    return ToolWorkflowAgent::effectiveTools(
        availableAgentTools(),
        route,
        prompt,
        selectedWorkflowObjectsForRoute(route),
        m_pendingAgentDraft.value(QStringLiteral("_sourcePrompt")).toString());
}

QJsonArray BricsCadPage::readOnlyMethodsForRoute(const QJsonObject& route) const
{
    if (!routeAllowsCadContext(route)) {
        return {};
    }

    QJsonArray readOnlyMethods;
    auto appendLocalMethods = [&readOnlyMethods]() {
        for (const QJsonValue& value : BrxAgent::localContextMethods()) {
            const QString name = value.toString();
            readOnlyMethods.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("risk"), QStringLiteral("readOnly")},
                {QStringLiteral("source"), QStringLiteral("qt-context-broker")},
            });
        }
    };

    if (!m_brxAuthenticated) {
        appendLocalMethods();
        return readOnlyMethods;
    }

    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (bridgeCapabilityMethodAvailable(method)
            && method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }
    if (!readOnlyMethods.isEmpty()) {
        appendLocalMethods();
        return readOnlyMethods;
    }

    for (const QString& method : QStringList{
             QStringLiteral("actions.list"),
             QStringLiteral("layers.list"),
             QStringLiteral("geometry.query"),
             QStringLiteral("bim.objects.query"),
             QStringLiteral("selection.describe"),
             QStringLiteral("entity.describe"),
             QStringLiteral("measurement.bbox"),
             QStringLiteral("measurement.length"),
             QStringLiteral("measurement.area")}) {
        readOnlyMethods.append(QJsonObject{{"name", method}, {"risk", "readOnly"}});
    }
    appendLocalMethods();
    return readOnlyMethods;
}

QJsonObject BricsCadPage::toolDefinition(const QString& name) const
{
    const QJsonArray scopedTools = routeAllowsCadActions(m_lastAgentRoute)
        ? availableAgentToolsForRoute(m_lastAgentRoute, m_lastAgentUserPrompt)
        : availableAgentTools();
    for (const QJsonValue& value : scopedTools) {
        const QJsonObject tool = value.toObject();
        if (tool.value("name").toString() == name) {
            return tool;
        }
    }
    if (routeAllowsCadActions(m_lastAgentRoute)) {
        for (const QJsonValue& value : availableAgentTools()) {
            const QJsonObject tool = value.toObject();
            if (tool.value("name").toString() == name) {
                return tool;
            }
        }
    }
    return {};
}

QString BricsCadPage::bridgeMethodForTool(const QString& name) const
{
    const QJsonObject tool = toolDefinition(name);
    return tool.value("bridgeMethod").toString(name);
}

bool BricsCadPage::isAllowedContextMethod(const QString& method) const
{
    static const QStringList fallbackReadOnlyMethods{
        "capabilities.list",
        "actions.list",
        "layers.list",
        "geometry.query",
        "bim.objects.query",
        "selection.describe",
        "entity.describe",
        "measurement.bbox",
        "measurement.length",
        "measurement.area",
    };

    if (method.isEmpty()) {
        return false;
    }
    for (const QJsonValue& value : BrxAgent::localContextMethods()) {
        if (value.toString() == method) {
            return true;
        }
    }

    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    if (!methods.isEmpty()) {
        for (const QJsonValue& value : methods) {
            const QJsonObject item = value.toObject();
            if (item.value("name").toString() == method) {
                return bridgeCapabilityMethodAvailable(item)
                    && item.value("risk").toString() == "readOnly";
            }
        }
        return false;
    }

    return fallbackReadOnlyMethods.contains(method);
}

void BricsCadPage::requestBridgeCapabilities()
{
    if (!m_brxAuthenticated || m_capabilitiesRequested) {
        return;
    }
    m_capabilitiesRequested = true;
    appendBridgeLog("Qt -> BRX: capabilities.list");

    const bool queued = sendBridgeRequest(
        "capabilities.list",
        {},
        10000,
        [this](const QJsonObject& response) {
            m_capabilitiesRequested = false;
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            if (!response.value("ok").toBool(false)) {
                const QString error = bridgeErrorMessage(response, "Capabilities konnten nicht geladen werden");
                appendBridgeLog(QString("BRX -> Qt: ERROR capabilities.list %1").arg(error));
                appendAgentChat("Barebone-Qt", QString("BRX Toolliste konnte nicht geladen werden: %1").arg(error));
                continueQueuedAgentPrompt();
                return;
            }
            m_brxCapabilities = response.value("result").toObject();
            drawingContextStore().ingestCapabilityResponse(QStringLiteral("capabilities.list"), response);
            const int methodCount = m_brxCapabilities.value("methods").toArray().size();
            const int commandCount = m_brxCapabilities.value("commands").toArray().size();
            const int toolCount = availableAgentTools().size();
            Q_UNUSED(methodCount);
            Q_UNUSED(commandCount);
            emitCapabilitiesStatusToWeb();
            if (toolCount <= 0) {
                QStringList methodNames;
                for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
                    const QJsonObject method = value.toObject();
                    methodNames << QString("%1:%2")
                        .arg(method.value("name").toString("<leer>"),
                             method.value("kind").toString("<kind fehlt>"));
                }
                appendBridgeLog(QString("BRX Capabilities enthalten keine Agent-Tools. Methoden=%1").arg(methodNames.join(", ")));
                appendAgentChat("Barebone-Qt", QString("BRX Toolliste enthaelt keine freigegebenen Agent-Tools. Methoden: %1").arg(methodNames.join(", ")));
            }
            continueQueuedAgentPrompt();
        });
    if (!queued) {
        m_capabilitiesRequested = false;
        appendBridgeLog("Qt -> BRX: capabilities.list konnte nicht gesendet werden, erzwinge BRX Reconnect");
        appendAgentChat("Barebone-Qt", "BRX Toolliste konnte nicht angefragt werden. Verbindung wird neu aufgebaut, Prompt bleibt in der Warteschlange.");
        forceBridgeReconnect("capabilities.list konnte nicht gesendet werden", true);
    }
}





DrawingContextStore& BricsCadPage::drawingContextStore()
{
    return m_bricsCadCoordinator->drawingContextStore();
}

const DrawingContextStore& BricsCadPage::drawingContextStore() const
{
    return m_bricsCadCoordinator->drawingContextStore();
}

QJsonObject BricsCadPage::currentAgentContext() const
{
    return QJsonObject{
        {"brxConnected", m_brxAuthenticated},
        {"units", "mm"},
        {"contextSource", "BRX readOnlyMethods"},
        {"saveBeforeDefault", true},
        {"lastToolResultAvailable", !m_lastAgentToolResult.isEmpty()},
        {"lastToolResultSchema", m_lastAgentToolResult.value("schema").toString()},
        {"capabilitiesLoaded", !m_brxCapabilities.isEmpty()},
        {"currentSelection", m_currentSelection},
        {"drawingContextStore", drawingContextStore().agentContext()},
        {"brxContextManifest", drawingContextStore().contextManifest()},
    };
}

QJsonObject BricsCadPage::compactAgentStateSummary(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route) const
{
    QJsonArray recentMessages;
    const int recentPreviewCount = m_lastFocusedConversationContext.isEmpty() ? 6 : 2;
    const qsizetype start = std::max<qsizetype>(0, m_agentConversation.size() - recentPreviewCount);
    for (qsizetype i = start; i < m_agentConversation.size(); ++i) {
        const QJsonObject message = m_agentConversation.at(i).toObject();
        QString content = repairMojibakeText(message.value(QStringLiteral("content")).toString())
            .replace("\r\n", "\n")
            .trimmed();
        if (content.size() > 360) {
            content = content.left(357) + QStringLiteral("...");
        }
        recentMessages.append(QJsonObject{
            {"role", message.value(QStringLiteral("role")).toString()},
            {"contentPreview", content},
        });
    }

    QJsonObject documentSummary;
    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    if (!sanitizedContext.isEmpty()) {
        const QString selectedText = sanitizedContext.value(QStringLiteral("selectedText")).toString();
        documentSummary = QJsonObject{
            {"available", !selectedText.trimmed().isEmpty()},
            {"title", sanitizedContext.value(QStringLiteral("title")).toString()},
            {"fileName", sanitizedContext.value(QStringLiteral("fileName")).toString()},
            {"pageRange", sanitizedContext.value(QStringLiteral("pageRange"))},
            {"charCount", selectedText.size()},
        };
    }

    QJsonArray workflowSummaries;
    for (const QJsonValue& value : selectedWorkflowObjectsForRoute(route)) {
        const QJsonObject workflow = value.toObject();
        workflowSummaries.append(QJsonObject{
            {"id", workflow.value("id").toString()},
            {"title", workflow.value("title").toString()},
            {"summary", workflowCompactSummaryForSelector(workflow)},
            {"preferredTools", stringsToJsonArray(workflowToolNamesForSelector(workflow, 10))},
            {"slots", stringsToJsonArray(workflowSlotNamesForSelector(workflow, 10))},
        });
    }
    int pendingProposalActionCount = m_pendingAgentProposal.value(QStringLiteral("actions")).toArray().size();
    if (pendingProposalActionCount == 0
        && !m_pendingAgentProposal.value(QStringLiteral("tool")).toString().trimmed().isEmpty()) {
        pendingProposalActionCount = 1;
    }

    return QJsonObject{
        {"schema", "barebone.agent.compact-state.v1"},
        {"lastUserPromptPreview", repairMojibakeText(prompt).left(700)},
        {"route", route.value("route").toString()},
        {"conversationMessages", m_agentConversation.size()},
        {"recentMessages", recentMessages},
        {"documentContext", documentSummary},
        {"pendingProposalAvailable", !m_pendingAgentProposal.isEmpty()},
        {"pendingProposalActionCount", pendingProposalActionCount},
        {"pendingDraftAvailable", !m_pendingAgentDraft.isEmpty()},
        {"lastToolResultAvailable", !m_lastAgentToolResult.isEmpty()},
        {"lastToolResultSummary", m_lastAgentToolResult.value("summary").toString()},
        {"drawingContextManifest", drawingContextStore().manifest()},
        {"selectedWorkflows", workflowSummaries},
        {"compressionPolicy", "Nutze diese Zusammenfassung als Orientierung. Wenn Details fehlen, frage gezielt nach oder nutze erlaubte read-only Kontextmethoden statt lange Historie zu rekonstruieren."},
    };
}

QJsonObject capabilitySummary(const QJsonObject& capabilities)
{
    int actionTools = 0;
    int readOnlyMethods = 0;
    int unavailableMethods = 0;
    const QJsonArray methods = capabilities.value(QStringLiteral("methods")).toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        const QString name = method.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        if (!bridgeCapabilityMethodAvailable(method)) {
            ++unavailableMethods;
            continue;
        }
        if (method.value(QStringLiteral("kind")).toString() == QStringLiteral("action")) {
            ++actionTools;
        } else if (method.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")) {
            ++readOnlyMethods;
        }
    }
    const QJsonDocument compactDoc(capabilities);
    return QJsonObject{
        {"methods", actionTools + readOnlyMethods},
        {"actionToolCount", actionTools},
        {"readOnlyMethodCount", readOnlyMethods},
        {"unavailableMethodCount", unavailableMethods},
        {"commandCount", capabilities.value(QStringLiteral("commands")).toArray().size()},
        {"capabilityHash", QString::number(qHash(QString::fromUtf8(compactDoc.toJson(QJsonDocument::Compact))), 16)},
    };
}

QJsonObject BricsCadPage::agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route) const
{
    const QJsonObject sanitizedContext = sanitizedPromptContext(documentContext);
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, sanitizedContext, m_chatMode);
    const QJsonArray selectedWorkflows = selectedWorkflowObjectsForRoute(normalizedRoute);

    if (isChatWorkspace()) {
        QJsonArray workflowCapsules;
        for (int i = 0; i < selectedWorkflows.size() && i < 3; ++i) {
            workflowCapsules.append(workflowCapsuleForAgent(selectedWorkflows.at(i).toObject(), i == 0));
        }
        QJsonObject envelope{
            {QStringLiteral("schema"), QStringLiteral("barebone.general.markdown.request.v1")},
            {QStringLiteral("userPrompt"), prompt},
            {QStringLiteral("route"), normalizedRoute},
            {QStringLiteral("includeConversationHistory"), true},
            {QStringLiteral("selectedWorkflow"), selectedWorkflowSummary()},
            {QStringLiteral("workflowCapsules"), workflowCapsules},
        };
        const QJsonObject documents = sanitizedDocumentContext(documentContext);
        if (!documents.isEmpty()) {
            envelope.insert(QStringLiteral("documentContext"), documents);
        }
        return envelope;
    }

    BricsCadFinalAgent::BuildInput finalInput;
    finalInput.prompt = prompt;
    finalInput.route = normalizedRoute;
    finalInput.drawingContext = currentAgentContext();
    finalInput.effectiveTools = availableAgentToolsForRoute(normalizedRoute, prompt);
    finalInput.selectedWorkflows = selectedWorkflows;
    finalInput.workflowHints = workflowHintObjectsForRoute(normalizedRoute);
    finalInput.pendingProposal = m_pendingAgentProposal;
    finalInput.pendingDraft = m_pendingAgentDraft;
    finalInput.lastToolResult = m_lastAgentToolResult;
    finalInput.reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
    finalInput.delegatedValueChoice = promptDelegatesValueChoice(prompt);
    return BricsCadFinalAgent::buildEnvelope(finalInput);
}


bool BricsCadPage::ensureBridgeCapabilitiesForPrompt(const QString& prompt)
{
    if (!m_brxAuthenticated) {
        m_queuedAgentPrompt.clear();
        appendAgentChat("Barebone-Qt", "BRX Plugin ist nicht verbunden. AI-Prompt wird nicht gesendet, weil keine verbindliche Toolliste vorliegt. Bitte BareboneBrx.brx in BricsCAD laden bzw. die Verbindung herstellen.");
        appendBridgeLog("AI Agent: Prompt blockiert, BRX ist nicht verbunden");
        return false;
    }

    if (!availableAgentTools().isEmpty()) {
        return true;
    }

    m_queuedAgentPrompt = prompt;
    appendAgentChat("Barebone-Qt", "BRX ist verbunden, aber die Toolliste fehlt. Lade Capabilities neu und sende den Prompt danach weiter.");
    appendBridgeLog("AI Agent: Prompt wartet auf BRX Capabilities");
    setAgentBusy(true);
    requestBridgeCapabilities();
    return false;
}

void BricsCadPage::continueQueuedAgentPrompt()
{
    const QString prompt = m_queuedAgentPrompt.trimmed();
    QJsonObject route = m_queuedAgentRoute;
    m_queuedAgentPrompt.clear();
    m_queuedAgentRoute = {};
    if (prompt.isEmpty()) {
        return;
    }

    route = normalizedAgentRouteForMode(route, prompt, m_lastDocumentContext, m_chatMode);
    if (!m_brxAuthenticated || (routeAllowsCadActions(route) && availableAgentTools().isEmpty())) {
        setAgentBusy(false);
        appendAgentChat("Barebone-Qt", routeAllowsCadActions(route)
            ? QStringLiteral("BRX Toolliste konnte nicht geladen werden. CAD-Aktion wurde nicht an die AI gesendet.")
            : QStringLiteral("BRX Capabilities konnten nicht geladen werden. Ich beantworte ohne Live-Zeichnungskontext."));
        appendBridgeLog("AI Agent: queued prompt verworfen, keine BRX Capabilities");
        if (!routeAllowsCadActions(route)) {
            sendUnifiedAgentRequest(prompt, m_lastDocumentContext, route);
        }
        return;
    }

    setAgentBusy(false);
    appendBridgeLog("AI Agent: queued prompt wird nach Capabilities-Laden fortgesetzt");
    continueUnifiedAgentRequest(prompt, m_lastDocumentContext, route);
}

void BricsCadPage::handleBridgeSocket(QTcpSocket* socket)
{
    if (m_brxSocket && m_brxSocket != socket) {
        appendBridgeLog("BRX Verbindung ersetzt");
        m_brxSocket->disconnectFromHost();
    }

    m_brxSocket = socket;
    m_brxReadBuffer.clear();
    m_brxJsonAccumulator.clear();
    m_brxAuthenticated = false;
    m_capabilitiesRequested = false;
    m_brxCapabilities = {};
    socket->setParent(this);

    appendBridgeLog(QString("BRX -> Qt: TCP verbunden von %1:%2")
        .arg(socket->peerAddress().toString())
        .arg(socket->peerPort()));
    setPluginStatus("BRX Plugin verbunden, Authentifizierung laeuft...", true);

    QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        m_brxReadBuffer.append(socket->readAll());
        while (true) {
            const qsizetype newlineIndex = m_brxReadBuffer.indexOf('\n');
            if (newlineIndex < 0) {
                if (m_brxReadBuffer.size() > 8 * 1024 * 1024) {
                    appendBridgeLog("BRX -> Qt: Zeilenpuffer zu gross, verwerfe unvollstaendige Nachricht");
                    m_brxReadBuffer.clear();
                }
                break;
            }

            const QByteArray line = m_brxReadBuffer.left(newlineIndex).trimmed();
            m_brxReadBuffer.remove(0, newlineIndex + 1);
            if (!line.isEmpty()) {
                QByteArray candidate;
                if (m_brxJsonAccumulator.isEmpty()) {
                    candidate = line;
                } else {
                    candidate = m_brxJsonAccumulator + '\n' + line;
                }

                bool incomplete = false;
                if (handleBridgeLine(candidate, &incomplete)) {
                    m_brxJsonAccumulator.clear();
                } else if (incomplete) {
                    if (m_brxJsonAccumulator.isEmpty()) {
                        appendBridgeLog(QString("BRX -> Qt: JSON Teil empfangen (%1 Bytes), warte auf Fortsetzung")
                            .arg(candidate.size()));
                    }
                    m_brxJsonAccumulator = candidate;
                    if (m_brxJsonAccumulator.size() > 8 * 1024 * 1024) {
                        appendBridgeLog("BRX -> Qt: JSON-Akkumulator zu gross, verwerfe unvollstaendige Nachricht");
                        m_brxJsonAccumulator.clear();
                    }
                } else {
                    m_brxJsonAccumulator.clear();
                }
            }
        }
    });

    QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (m_brxSocket == socket) {
            m_brxSocket = nullptr;
            m_brxAuthenticated = false;
            m_brxReadBuffer.clear();
            m_brxJsonAccumulator.clear();
            drawingContextStore().clear();
            m_currentSelection = {};
            if (!m_preserveQueuedAgentPromptOnDisconnect) {
                m_queuedAgentPrompt.clear();
            }
            m_preserveQueuedAgentPromptOnDisconnect = false;
            m_capabilitiesRequested = false;
            m_brxCapabilities = {};
            failPendingBridgeRequests("BRX Plugin Verbindung wurde getrennt");
            setPluginStatus("BRX Plugin nicht verbunden", false);
            m_bridgeStatus->setText(QString("Bereit auf %1:%2, warte auf BRX Plugin").arg(kBridgeHost).arg(kBridgePort));
            appendBridgeLog("BRX Verbindung getrennt");
        }
        socket->deleteLater();
    });
}

bool BricsCadPage::handleBridgeLine(const QByteArray& line, bool* incomplete)
{
    if (incomplete) {
        *incomplete = false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const bool isIncomplete =
            parseError.error == QJsonParseError::UnterminatedObject
            || parseError.error == QJsonParseError::UnterminatedArray
            || parseError.error == QJsonParseError::UnterminatedString;
        if (isIncomplete) {
            if (incomplete) {
                *incomplete = true;
            }
            return false;
        }

        QString prefix = QString::fromUtf8(line.left(160));
        prefix.replace('\r', "\\r");
        prefix.replace('\n', "\\n");
        appendBridgeLog(QString("BRX -> Qt: ungueltiges JSON (%1, %2 Bytes, Anfang: %3)")
            .arg(parseError.errorString())
            .arg(line.size())
            .arg(prefix));
        return false;
    }

    const QJsonObject message = document.object();
    if (line.size() > 4096) {
        appendBridgeLog(QString("BRX -> Qt: JSON ok (%1 Bytes, type=%2, id=%3)")
            .arg(line.size())
            .arg(message.value("type").toString())
            .arg(message.contains("id") ? QString::number(message.value("id").toInt()) : QStringLiteral("-")));
    }

    handleBridgeMessage(message);
    return true;
}

void BricsCadPage::handleBridgeMessage(const QJsonObject& message)
{
    const QString type = message.value("type").toString();
    if (type == "hello") {
        const QString token = message.value("token").toString();
        if (token != m_bridgeToken) {
            appendBridgeLog("BRX -> Qt: hello mit ungueltigem Token");
            sendBridgeMessage({
                {"type", "error"},
                {"error", "invalid-token"},
            });
            if (m_brxSocket) {
                m_brxSocket->disconnectFromHost();
            }
            return;
        }

        m_brxAuthenticated = true;
        setPluginStatus("BRX Plugin verbunden", true);
        m_bridgeStatus->setText(QString("BRX verbunden auf %1:%2").arg(kBridgeHost).arg(kBridgePort));
        const QString bridgeBuild = message.value("bridgeBuild").toString();
        appendBridgeLog(QString("BRX -> Qt: hello %1%2")
            .arg(message.value("plugin").toString("BareboneBrx"))
            .arg(bridgeBuild.isEmpty() ? QString() : QString(" (%1)").arg(bridgeBuild)));
        sendBridgeMessage({
            {"type", "event"},
            {"event", "hello.ok"},
            {"protocol", 1},
        });
        if (isBricsCadMode() || m_trainingMode) {
            requestBridgeCapabilities();
        } else {
            appendBridgeLog("AI Agent: Allgemeiner Modus aktiv; BRX Capabilities werden nicht automatisch geladen");
        }
        return;
    }

    if (!m_brxAuthenticated) {
        appendBridgeLog("BRX -> Qt: Nachricht vor Authentifizierung verworfen");
        return;
    }

    if (type == "response") {
        completeBridgeRequest(message.value("id").toInt(), message);
        return;
    }

    if (type == "event") {
        // Realtime drawing/selection events are intentionally ignored. The
        // only retained event is explicit BRX debug output; drawing context is
        // fetched synchronously for a concrete Qt prompt.
        if (message.value("event").toString() == QStringLiteral("debug")) {
            appendBridgeLog(QString("BRX Debug: %1").arg(message.value("message").toString()));
        }
        return;
    }

    appendBridgeLog(QString("BRX -> Qt: unbekannter Nachrichtentyp %1").arg(type));
}

bool BricsCadPage::sendBridgeMessage(const QJsonObject& message)
{
    if (!m_brxSocket || m_brxSocket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    const QByteArray line = toJsonLine(message);
    return m_brxSocket->write(line) == line.size();
}

bool BricsCadPage::sendBridgeRequest(
    const QString& method,
    const QJsonObject& params,
    int timeoutMs,
    std::function<void(const QJsonObject&)> handler)
{
    if (!m_brxAuthenticated || !m_brxSocket || m_brxSocket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    const int id = m_nextRequestId++;
    auto* timeout = new QTimer(this);
    timeout->setSingleShot(true);

    PendingBridgeRequest pending;
    pending.method = method;
    pending.timeout = timeout;
    pending.operationGeneration = m_operationGeneration;
    pending.handler = std::move(handler);
    m_pendingRequests.insert(id, pending);

    QObject::connect(timeout, &QTimer::timeout, this, [this, id]() {
        if (!m_pendingRequests.contains(id)) {
            return;
        }

        PendingBridgeRequest pending = m_pendingRequests.take(id);
        if (pending.timeout) {
            pending.timeout->deleteLater();
        }

        QJsonObject response;
        response.insert("id", id);
        response.insert("type", "response");
        response.insert("ok", false);
        response.insert("error", QString("Zeitueberschreitung beim Warten auf BRX Antwort fuer %1").arg(pending.method));
        if (pending.operationGeneration != m_operationGeneration) {
            return;
        }
        if (pending.handler) {
            pending.handler(response);
        }
    });

    QJsonObject request;
    request.insert("id", id);
    request.insert("type", "request");
    request.insert("method", method);
    request.insert("params", params);

    if (!sendBridgeMessage(request)) {
        PendingBridgeRequest failed = m_pendingRequests.take(id);
        if (failed.timeout) {
            failed.timeout->deleteLater();
        }
        return false;
    }

    timeout->start(timeoutMs);
    return true;
}

bool BricsCadPage::sendBridgeActionRequest(
    const QString& method,
    const QJsonObject& params,
    int timeoutMs,
    std::function<void(const QJsonObject&)> handler)
{
    return sendBridgeRequest(
        method,
        params,
        timeoutMs,
        [handler = std::move(handler)](const QJsonObject& response) mutable {
            handler(response);
        });
}

void BricsCadPage::completeBridgeRequest(int id, const QJsonObject& message)
{
    if (!m_pendingRequests.contains(id)) {
        appendBridgeLog(QString("BRX -> Qt: Antwort ohne offenen Request id=%1").arg(id));
        return;
    }

    PendingBridgeRequest pending = m_pendingRequests.take(id);
    if (pending.timeout) {
        pending.timeout->stop();
        pending.timeout->deleteLater();
    }
    if (pending.operationGeneration != m_operationGeneration) {
        appendBridgeLog(QString("BRX -> Qt: veraltete Antwort ignoriert id=%1 method=%2").arg(id).arg(pending.method));
        return;
    }
    if (pending.handler) {
        pending.handler(message);
    }
}

void BricsCadPage::failPendingBridgeRequests(const QString& message)
{
    const QList<int> ids = m_pendingRequests.keys();
    for (int id : ids) {
        PendingBridgeRequest pending = m_pendingRequests.take(id);
        if (pending.timeout) {
            pending.timeout->stop();
            pending.timeout->deleteLater();
        }

        QJsonObject response;
        response.insert("id", id);
        response.insert("type", "response");
        response.insert("ok", false);
        response.insert("error", message);
        if (pending.operationGeneration != m_operationGeneration) {
            continue;
        }
        if (pending.handler) {
            pending.handler(response);
        }
    }
}

void BricsCadPage::forceBridgeReconnect(const QString& reason, bool preserveQueuedPrompt)
{
    appendBridgeLog(QString("BRX Reconnect: %1").arg(reason));
    m_preserveQueuedAgentPromptOnDisconnect = preserveQueuedPrompt;
    m_capabilitiesRequested = false;
    m_brxCapabilities = {};

    if (!m_brxSocket) {
        m_brxAuthenticated = false;
        setPluginStatus("BRX Plugin nicht verbunden", false);
        return;
    }

    m_brxAuthenticated = false;
    m_brxSocket->abort();
    setPluginStatus("BRX Verbindung wird neu aufgebaut...", false);
}

void BricsCadPage::setPluginStatus(const QString& message, bool connected)
{
    if (m_pluginStatus) {
        const QString color = connected ? "#1f7a3f" : "#9a3412";
        m_pluginStatus->setText(QString("<span style='color:%1;font-weight:700'>%2</span>")
            .arg(color, message.toHtmlEscaped()));
    }

    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [message, connected](AiWebBridge* target) {
            Q_EMIT target->bridgeStatusChanged(message, connected);
        });
    }
}

void BricsCadPage::setLocalAiStatus(const QString& message, bool connected)
{
    m_localAiStatusMessage = message;
    m_localAiReachable = connected;

    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [message, connected](AiWebBridge* target) {
            Q_EMIT target->localAiStatusChanged(message, connected);
        });
    }
}

void BricsCadPage::writeBridgeToken() const
{
    QFile tokenFile(bridgeTokenFilePath());
    if (tokenFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        tokenFile.write(m_bridgeToken.toUtf8());
        tokenFile.write("\n");
    }
}

void BricsCadPage::startBridgeServer()
{
    writeBridgeToken();

    m_bridgeServer = new QTcpServer(this);
    QObject::connect(m_bridgeServer, &QTcpServer::newConnection, this, [this]() {
        while (m_bridgeServer->hasPendingConnections()) {
            handleBridgeSocket(m_bridgeServer->nextPendingConnection());
        }
    });

    const bool listening = m_bridgeServer->listen(QHostAddress::LocalHost, kBridgePort);
    if (!listening) {
        m_bridgeStatus->setText(QString("127.0.0.1:%1 konnte nicht geoeffnet werden: %2")
            .arg(kBridgePort)
            .arg(m_bridgeServer->errorString()));
        appendBridgeLog("Serverstart fehlgeschlagen");
        return;
    }

    m_bridgeStatus->setText(QString("Bereit auf %1:%2, warte auf BRX Plugin").arg(kBridgeHost).arg(kBridgePort));
    appendBridgeLog(QString("Server bereit, Token: %1").arg(bridgeTokenFilePath()));
}

void BricsCadPage::appendBridgeLog(const QString& message)
{
    const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    const QString line = QString("[%1] %2").arg(stamp, message);
    QFile logFile(QDir::temp().filePath(QStringLiteral("BareboneQtApp.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        logFile.write(line.toUtf8());
        logFile.write("\n");
    }
    if (m_bridgeLog) {
        m_bridgeLog->appendPlainText(line);
    }
    if (m_agentBridge) {
        emitWebBridgeLog(m_agentBridge, line);
    }
}

void BricsCadPage::emitCapabilitiesStatusToWeb() const
{
    if (!m_agentBridge || m_brxCapabilities.isEmpty()) {
        return;
    }
    if (!isBricsCadMode() && !m_trainingMode) {
        return;
    }

    const int methodCount = m_brxCapabilities.value("methods").toArray().size();
    const int commandCount = m_brxCapabilities.value("commands").toArray().size();
    const int toolCount = availableAgentTools().size();

    const QString line = QString("[%1] BRX -> Qt: %2 Capabilities, %3 Commands, %4 Agent-Tools")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
        .arg(methodCount)
        .arg(commandCount)
        .arg(toolCount);
    emitWebBridgeLog(m_agentBridge, line);
}

void BricsCadPage::resetAgentConversation()
{
    ++m_operationGeneration;
    m_parallelPreparationActive = false;
    m_activeReasoningRunId.clear();
    if (m_bricsCadCoordinator) {
        m_bricsCadCoordinator->cancel();
    }
    if (m_aiRuntime) {
        m_aiRuntime->abortAll();
    }
    m_agentConversation = {};
    m_pendingAgentProposal = {};
    m_pendingAgentDraft = {};
    m_lastAgentToolResult = {};
    m_lastBricsCadExecution = {};
    m_lastDocumentContext = {};
    m_lastFocusedConversationContext = {};
    m_agentValidationRetries = 0;
    clearAgentProposal();
    setAgentBusy(false);
    saveCurrentAgentSession();
    emitContextBudget();
}

void BricsCadPage::saveCurrentAgentSession()
{
    if (m_agentSessionId.trimmed().isEmpty()) {
        return;
    }

    m_agentSessions.insert(m_agentSessionId, AgentSessionState{
        m_agentConversation,
        m_pendingAgentProposal,
        m_pendingAgentDraft,
        m_lastAgentToolResult,
        m_lastBricsCadExecution,
    });
}

QJsonArray BricsCadPage::conversationFromWebHistory(const QVariantList& history) const
{
    QJsonArray conversation;
    for (const QVariant& item : history) {
        const QVariantMap map = item.toMap();
        if (map.value(QStringLiteral("kind")).toString()
            == QStringLiteral("thinking")) {
            continue;
        }
        const QString speaker = map.value(QStringLiteral("speaker")).toString();
        const QString message = map.value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty()) {
            continue;
        }

        if (speaker == "Du") {
            conversation.append(QJsonObject{{"role", "user"}, {"content", message}, {"messageId", map.value(QStringLiteral("messageId")).toString()}});
        } else if (speaker == "AI") {
            QJsonObject item{
                {"role", "assistant"},
                {"content", message},
                {"messageId", map.value(QStringLiteral("messageId")).toString()},
            };
            const QJsonObject execution = QJsonObject::fromVariantMap(
                map.value(QStringLiteral("bricsCadExecutionRecord")).toMap());
            if (!execution.isEmpty()) {
                QString executionId = map.value(QStringLiteral("bricsCadExecutionId")).toString().trimmed();
                if (executionId.isEmpty()) {
                    executionId = execution.value(QStringLiteral("executionId")).toString();
                }
                item.insert(QStringLiteral("bricsCadExecutionRecord"), execution);
                item.insert(QStringLiteral("bricsCadExecutionId"), executionId);
                const QVariant workflowSaveEligible = map.value(QStringLiteral("workflowSaveEligible"));
                item.insert(QStringLiteral("workflowSaveEligible"),
                    workflowSaveEligible.isValid()
                        ? workflowSaveEligible.toBool()
                        : execution.value(QStringLiteral("saveEligible")).toBool(false));
            }
            conversation.append(item);
        }
    }
    return conversation;
}

void BricsCadPage::openAgentSession(const QString& sessionId, const QVariantList& history)
{
    const QString normalizedSessionId = sessionId.trimmed().isEmpty()
        ? QStringLiteral("session-default")
        : sessionId.trimmed();

    if (normalizedSessionId == m_agentSessionId && m_agentSessions.contains(normalizedSessionId)) {
        return;
    }

    ++m_operationGeneration;
    m_parallelPreparationActive = false;
    m_activeReasoningRunId.clear();
    if (m_bricsCadCoordinator) {
        m_bricsCadCoordinator->cancel();
    }
    if (m_aiRuntime) {
        m_aiRuntime->abortAll();
    }

    saveCurrentAgentSession();
    m_agentSessionId = normalizedSessionId;
    m_lastFocusedConversationContext = {};

    if (m_agentSessions.contains(m_agentSessionId)) {
        const AgentSessionState state = m_agentSessions.value(m_agentSessionId);
        m_agentConversation = state.conversation;
        m_pendingAgentProposal = state.pendingProposal;
        m_pendingAgentDraft = state.pendingDraft;
        m_lastAgentToolResult = state.lastToolResult;
        m_lastBricsCadExecution = state.lastBricsCadExecution;
        if (m_lastBricsCadExecution.isEmpty()) {
            m_lastBricsCadExecution = latestBricsCadExecutionFromConversation();
        }
        m_lastDocumentContext = {};
        m_lastFocusedConversationContext = {};
    } else {
        m_agentConversation = conversationFromWebHistory(history);
        m_pendingAgentProposal = {};
        m_pendingAgentDraft = {};
        m_lastAgentToolResult = {};
        m_lastBricsCadExecution = latestBricsCadExecutionFromConversation();
        m_lastDocumentContext = {};
        m_lastFocusedConversationContext = {};
        saveCurrentAgentSession();
    }

    m_agentValidationRetries = 0;
    setAgentBusy(false);
    if (!m_pendingAgentProposal.isEmpty()) {
        setAgentProposal(m_pendingAgentProposal);
    } else {
        clearAgentProposal();
    }
    appendBridgeLog(QString("AI Agent: Sitzung aktiv %1").arg(m_agentSessionId));
    emitContextBudget();
    emitCapabilitiesStatusToWeb();
}
