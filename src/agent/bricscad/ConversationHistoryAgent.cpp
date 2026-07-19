#include "ConversationHistoryAgent.h"

#include "BricsCadAgentUtils.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

QString compactText(QString text, int maxChars)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    text = text.trimmed();
    if (text.size() > maxChars) {
        text = text.left(std::max(12, maxChars - 3)).trimmed() + QStringLiteral("...");
    }
    return text;
}

} // namespace

QJsonArray ConversationHistoryAgent::compactConversation(const QJsonArray& conversation, int maxMessages, int maxChars)
{
    QJsonArray history;
    const int conversationSize = static_cast<int>(conversation.size());
    const int start = std::max(0, conversationSize - maxMessages);
    for (int i = start; i < conversationSize; ++i) {
        const QJsonObject message = conversation.at(i).toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        const QString content = compactText(message.value(QStringLiteral("content")).toString(), maxChars);
        if (role.trimmed().isEmpty() || content.trimmed().isEmpty()) {
            continue;
        }
        history.append(QJsonObject{
            {QStringLiteral("index"), i},
            {QStringLiteral("role"), role},
            {QStringLiteral("content"), content},
        });
    }
    return history;
}

QJsonObject ConversationHistoryAgent::fallback(const QString& prompt)
{
    const QString topic = BricsCadAgentUtils::normalizedSearchText(prompt).left(80);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.focused-conversation-context.v1")},
        {QStringLiteral("topic"), topic.isEmpty() ? QStringLiteral("Aktueller Prompt") : topic},
        {QStringLiteral("relevantSummary"), QString()},
        {QStringLiteral("relevantMessageIndexes"), QJsonArray{}},
        {QStringLiteral("omittedTopics"), QJsonArray{}},
        {QStringLiteral("confidence"), 0.0},
    };
}

QJsonObject ConversationHistoryAgent::normalize(QJsonObject object, const QString& prompt, const QJsonArray& compactConversation)
{
    if (object.value(QStringLiteral("schema")).toString().isEmpty()) {
        object.insert(QStringLiteral("schema"), QStringLiteral("barebone.agent.focused-conversation-context.v1"));
    }
    if (object.value(QStringLiteral("topic")).toString().trimmed().isEmpty()) {
        object.insert(QStringLiteral("topic"), BricsCadAgentUtils::normalizedSearchText(prompt).left(80));
    }
    QJsonArray indexes;
    QSet<int> seen;
    for (const QJsonValue& value : object.value(QStringLiteral("relevantMessageIndexes")).toArray()) {
        int index = -1;
        if (value.isDouble()) {
            index = value.toInt(-1);
        } else {
            bool ok = false;
            index = value.toString().toInt(&ok);
            if (!ok) {
                index = -1;
            }
        }
        if (index < 0) {
            continue;
        }
        // Accept original indexes. Also map compact-array positions for older prompts.
        bool existsAsOriginal = false;
        for (const QJsonValue& compactValue : compactConversation) {
            if (compactValue.toObject().value(QStringLiteral("index")).toInt(-1) == index) {
                existsAsOriginal = true;
                break;
            }
        }
        if (!existsAsOriginal && index < compactConversation.size()) {
            index = compactConversation.at(index).toObject().value(QStringLiteral("index")).toInt(-1);
        }
        if (index >= 0 && !seen.contains(index)) {
            seen.insert(index);
            indexes.append(index);
        }
    }
    const QString summary = object.value(QStringLiteral("relevantSummary")).toString().trimmed();
    if (indexes.isEmpty() && !summary.isEmpty() && !compactConversation.isEmpty()) {
        const int lastIndex = compactConversation.last().toObject().value(QStringLiteral("index")).toInt(-1);
        if (lastIndex >= 0) {
            indexes.append(lastIndex);
        }
    }
    object.insert(QStringLiteral("relevantMessageIndexes"), indexes);
    if (!object.value(QStringLiteral("omittedTopics")).isArray()) {
        object.insert(QStringLiteral("omittedTopics"), QJsonArray{});
    }
    object.insert(QStringLiteral("confidence"),
        std::clamp(object.value(QStringLiteral("confidence")).toDouble(0.0), 0.0, 1.0));
    return object;
}

void ConversationHistoryAgent::run(const Request& request, LocalAiAgentRuntime& runtime, Callback callback) const
{
    const QJsonArray history = compactConversation(request.conversation);
    if (history.size() <= 1) {
        QJsonObject result = fallback(request.prompt);
        result.insert(QStringLiteral("ready"), true);
        callback(result);
        return;
    }

    QJsonObject input{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.history-slot.request.v1")},
        {QStringLiteral("currentPrompt"), request.prompt},
        {QStringLiteral("conversation"), history},
        {QStringLiteral("outputShape"), QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.focused-conversation-context.v1")},
            {QStringLiteral("topic"), QString()},
            {QStringLiteral("relevantSummary"), QString()},
            {QStringLiteral("relevantMessageIndexes"), QJsonArray{}},
            {QStringLiteral("omittedTopics"), QJsonArray{}},
            {QStringLiteral("confidence"), 0.0},
        }},
    };

    LocalAiAgentRuntime::JsonRequest jsonRequest;
    jsonRequest.slot = QStringLiteral("history");
    jsonRequest.kind = QStringLiteral("BricsCadPreparation-history");
    jsonRequest.systemInstruction = QStringLiteral(
        "Du bist der Nachrichtenverlauf-Agent fuer eine BricsCAD-Ausfuehrung. "
        "Komprimiere ausschliesslich promptrelevante Werte, Entscheidungen, offene Rueckfragen und erfolgreiche Toolergebnisse. "
        "Keine Policies, keine Toolkarten, keine CAD-Aktion. Antworte nur als kompaktes JSON nach outputShape.");
    jsonRequest.input = input;
    jsonRequest.requiredFields = {
        QStringLiteral("schema"),
        QStringLiteral("topic"),
        QStringLiteral("relevantSummary"),
        QStringLiteral("relevantMessageIndexes"),
        QStringLiteral("omittedTopics"),
        QStringLiteral("confidence"),
    };
    jsonRequest.maxOutputTokens = 1024;
    jsonRequest.priority = 90;
    jsonRequest.cancellable = true;
    jsonRequest.dedupeKey = QStringLiteral("bricscad-preparation:%1:%2:history")
        .arg(request.sessionId)
        .arg(request.operationGeneration);
    jsonRequest.operationGeneration = request.operationGeneration;
    jsonRequest.bricsCadMode = request.bricsCadMode;

    runtime.submitJson(jsonRequest, [callback = std::move(callback), prompt = request.prompt, history](const LocalAiAgentRuntime::JsonResult& result) mutable {
        QJsonObject object;
        if (result.ok) {
            object = normalize(result.object, prompt, history);
            object.insert(QStringLiteral("ready"), true);
        } else {
            object = fallback(prompt);
            object.insert(QStringLiteral("ready"), true);
            object.insert(QStringLiteral("source"), QStringLiteral("deterministic-empty-history"));
            object.insert(QStringLiteral("error"), result.error);
        }
        callback(object);
    });
}
