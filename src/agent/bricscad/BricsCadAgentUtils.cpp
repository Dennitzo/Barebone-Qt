#include "BricsCadAgentUtils.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace BricsCadAgentUtils {

QString normalizedSearchText(QString text)
{
    text = text.toLower();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar(0x00E4), QStringLiteral("ae"));
    text.replace(QChar(0x00F6), QStringLiteral("oe"));
    text.replace(QChar(0x00FC), QStringLiteral("ue"));
    text.replace(QChar(0x00DF), QStringLiteral("ss"));
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9_./:+\\-\\s]+")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.trimmed();
}

QString workflowSlug(QString text)
{
    text = normalizedSearchText(text);
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    text.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    return text.trimmed().trimmed();
}

bool textMentionsAny(const QString& normalizedText, const QStringList& needles)
{
    for (const QString& needle : needles) {
        if (!needle.trimmed().isEmpty()
            && normalizedText.contains(normalizedSearchText(needle))) {
            return true;
        }
    }
    return false;
}

QStringList stringsFromJsonArray(const QJsonArray& values)
{
    QStringList strings;
    for (const QJsonValue& value : values) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty() && !strings.contains(text)) {
            strings.append(text);
        }
    }
    return strings;
}

QJsonArray stringsToJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        if (!value.trimmed().isEmpty()) {
            array.append(value.trimmed());
        }
    }
    return array;
}

QStringList toolNamesForLog(const QJsonArray& tools, int maxCount)
{
    QStringList names;
    for (const QJsonValue& value : tools) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
        }
        if (names.size() >= maxCount) {
            break;
        }
    }
    return names;
}

QStringList routeWorkflowIds(const QJsonObject& route, int maxCount)
{
    QStringList ids;
    for (const QJsonValue& value : route.value(QStringLiteral("selectedWorkflows")).toArray()) {
        const QString id = value.toString().trimmed();
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
        if (ids.size() >= maxCount) {
            break;
        }
    }
    return ids;
}

bool capabilityMethodAvailable(const QJsonObject& method)
{
    const QString name = method.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return false;
    }
    if (method.contains(QStringLiteral("available"))) {
        return method.value(QStringLiteral("available")).toBool(true);
    }
    if (method.contains(QStringLiteral("enabled"))) {
        return method.value(QStringLiteral("enabled")).toBool(true);
    }
    return true;
}

bool capabilitiesContainMethod(const QJsonObject& capabilities, const QString& methodName)
{
    for (const QJsonValue& value : capabilities.value(QStringLiteral("methods")).toArray()) {
        const QJsonObject method = value.toObject();
        if (method.value(QStringLiteral("name")).toString() == methodName
            && capabilityMethodAvailable(method)) {
            return true;
        }
    }
    return false;
}

bool shouldPrefetchSelectionDescription(const QString& prompt)
{
    const QString normalized = normalizedSearchText(prompt);
    return textMentionsAny(normalized, {
        QStringLiteral("auswahl"),
        QStringLiteral("selekt"),
        QStringLiteral("markiert"),
        QStringLiteral("selected"),
        QStringLiteral("pickfirst"),
        QStringLiteral("diese"),
        QStringLiteral("dieses"),
        QStringLiteral("den ausgewaehlten"),
    });
}

QString bridgeErrorMessage(const QJsonObject& response, const QString& fallback)
{
    const QString error = response.value(QStringLiteral("error")).toString().trimmed();
    if (!error.isEmpty()) {
        return error;
    }
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const QString resultError = result.value(QStringLiteral("error")).toString().trimmed();
    return resultError.isEmpty() ? fallback : resultError;
}

static QJsonArray limitedArray(const QJsonArray& source, int limit)
{
    QJsonArray out;
    for (int i = 0; i < source.size() && i < limit; ++i) {
        out.append(source.at(i));
    }
    return out;
}

QJsonObject compactBrxResponseForAgent(const QJsonObject& response)
{
    QJsonObject compact;
    for (auto it = response.constBegin(); it != response.constEnd(); ++it) {
        if (it.value().isArray()) {
            const int limit = it.key() == QStringLiteral("objects") ? 80 : 120;
            compact.insert(it.key(), limitedArray(it.value().toArray(), limit));
        } else if (it.value().isObject()) {
            compact.insert(it.key(), it.value());
        } else {
            compact.insert(it.key(), it.value());
        }
    }
    return compact;
}

QJsonArray workflowDisplaySteps(const QJsonObject& workflow)
{
    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    if (!steps.isEmpty()) {
        return steps;
    }
    for (const QJsonValue& batchValue : workflow.value(QStringLiteral("executionBatches")).toArray()) {
        for (const QJsonValue& stepValue : batchValue.toObject().value(QStringLiteral("steps")).toArray()) {
            steps.append(stepValue);
        }
    }
    return steps;
}

QStringList workflowToolNames(const QJsonObject& workflow, int maxCount)
{
    QStringList names = stringsFromJsonArray(workflow.value(QStringLiteral("preferredTools")).toArray());
    for (const QJsonValue& stepValue : workflowDisplaySteps(workflow)) {
        const QString tool = stepValue.toObject().value(QStringLiteral("tool")).toString().trimmed();
        if (!tool.isEmpty() && !names.contains(tool)) {
            names.append(tool);
        }
        if (names.size() >= maxCount) {
            break;
        }
    }
    while (names.size() > maxCount) {
        names.removeLast();
    }
    return names;
}

QJsonArray workflowStepSummary(const QJsonObject& workflow, int maxCount)
{
    QJsonArray summary;
    const QJsonArray steps = workflowDisplaySteps(workflow);
    for (int i = 0; i < steps.size() && i < maxCount; ++i) {
        const QJsonObject step = steps.at(i).toObject();
        summary.append(QJsonObject{
            {QStringLiteral("id"), step.value(QStringLiteral("id")).toString()},
            {QStringLiteral("title"), step.value(QStringLiteral("title")).toString()},
            {QStringLiteral("tool"), step.value(QStringLiteral("tool")).toString()},
        });
    }
    return summary;
}

QJsonObject workflowCapsule(const QJsonObject& workflow, bool detailed)
{
    QJsonObject capsule{
        {QStringLiteral("id"), workflow.value(QStringLiteral("id")).toString()},
        {QStringLiteral("title"), workflow.value(QStringLiteral("title")).toString()},
        {QStringLiteral("description"), workflow.value(QStringLiteral("description")).toString().left(detailed ? 1800 : 700)},
        {QStringLiteral("preferredTools"), stringsToJsonArray(workflowToolNames(workflow, detailed ? 18 : 8))},
        {QStringLiteral("knownSlotValues"), workflow.value(QStringLiteral("knownSlotValues")).toObject()},
    };
    if (detailed) {
        capsule.insert(QStringLiteral("executionBatches"), workflow.value(QStringLiteral("executionBatches")).toArray());
        capsule.insert(QStringLiteral("valueResolution"), workflow.value(QStringLiteral("valueResolution")).toObject());
        capsule.insert(QStringLiteral("constructionStrategy"), workflow.value(QStringLiteral("constructionStrategy")).toArray());
        capsule.insert(QStringLiteral("forbidden"), workflow.value(QStringLiteral("forbidden")).toArray());
        capsule.insert(QStringLiteral("stepSummary"), workflowStepSummary(workflow, 12));
    } else {
        capsule.insert(QStringLiteral("stepSummary"), workflowStepSummary(workflow, 6));
    }
    return capsule;
}

static QString workflowSearchText(const QJsonObject& workflow)
{
    QStringList parts{
        workflow.value(QStringLiteral("id")).toString(),
        workflow.value(QStringLiteral("title")).toString(),
        workflow.value(QStringLiteral("description")).toString(),
    };
    for (const QJsonValue& value : workflow.value(QStringLiteral("keywords")).toArray()) {
        parts << value.toString();
    }
    for (const QJsonValue& value : workflow.value(QStringLiteral("triggerExamples")).toArray()) {
        parts << value.toString();
    }
    for (const QString& tool : workflowToolNames(workflow, 16)) {
        parts << tool;
    }
    return normalizedSearchText(parts.join(QLatin1Char(' ')));
}

QStringList localWorkflowSelection(const QJsonArray& compactWorkflows, const QString& prompt, int maxCount)
{
    const QString normalizedPrompt = normalizedSearchText(prompt);
    struct ScoredWorkflow {
        QString id;
        int score = 0;
    };
    QVector<ScoredWorkflow> scored;
    for (const QJsonValue& value : compactWorkflows) {
        const QJsonObject workflow = value.toObject();
        const QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        const QString haystack = workflowSearchText(workflow);
        int score = 0;
        for (const QString& token : normalizedPrompt.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (token.size() < 3) {
                continue;
            }
            if (haystack.contains(token)) {
                score += token.size() >= 8 ? 3 : 1;
            }
        }
        const QString title = normalizedSearchText(workflow.value(QStringLiteral("title")).toString());
        if (!title.isEmpty() && normalizedPrompt.contains(title.left(std::min<int>(18, static_cast<int>(title.size()))))) {
            score += 8;
        }
        if (score > 0) {
            scored.append(ScoredWorkflow{id, score});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const ScoredWorkflow& left, const ScoredWorkflow& right) {
        return left.score > right.score;
    });
    QStringList ids;
    for (const ScoredWorkflow& item : scored) {
        if (!ids.contains(item.id)) {
            ids.append(item.id);
        }
        if (ids.size() >= maxCount) {
            break;
        }
    }
    return ids;
}

} // namespace BricsCadAgentUtils
