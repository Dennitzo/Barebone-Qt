#include "BricsCadLearningAgent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

constexpr const char* kLearningResourcePath = ":/agent/bricscad-learning/brx-learning.json";

QString repairLearningText(QString text);

QStringList jsonStringArray(const QJsonArray& array)
{
    QStringList values;
    for (const QJsonValue& value : array) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty() && !values.contains(text)) {
            values << text;
        }
    }
    return values;
}

QString clippedText(QString text, int maxChars)
{
    text = repairLearningText(text).trimmed();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QRegularExpression(QStringLiteral("[ \\t\\n\\r]+")), QStringLiteral(" "));
    if (maxChars > 0 && text.size() > maxChars) {
        text = text.left(std::max(12, maxChars - 3)).trimmed() + QStringLiteral("...");
    }
    return text;
}

void appendStringUnique(QJsonArray& array, const QString& value)
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

QString charSeq(std::initializer_list<ushort> codepoints)
{
    QString value;
    for (const ushort codepoint : codepoints) {
        value.append(QChar(codepoint));
    }
    return value;
}

QString repairLearningText(QString text)
{
    text.replace(charSeq({0x00c2, 0x00a0}), QStringLiteral(" "));
    text.replace(QChar(0x00c2), QString());
    text.replace(charSeq({0x00e2, 0x20ac, 0x2018}), QStringLiteral("-"));
    text.replace(charSeq({0x00e2, 0x20ac, 0x2019}), QStringLiteral("'"));
    text.replace(charSeq({0x00e2, 0x20ac, 0x201c}), QStringLiteral("-"));
    text.replace(charSeq({0x00e2, 0x20ac, 0x201d}), QStringLiteral("-"));
    text.replace(charSeq({0x00e2, 0x20ac, 0x0153}), QStringLiteral("\""));
    text.replace(charSeq({0x00e2, 0x20ac, 0x009d}), QStringLiteral("\""));
    text.replace(charSeq({0x00e2, 0x20ac, 0x017e}), QStringLiteral("\""));
    text.replace(charSeq({0x00e2, 0x20ac, 0x00a6}), QStringLiteral("..."));
    text.replace(charSeq({0x00e2, 0x20ac, 0x00af}), QStringLiteral(" "));
    return text.simplified();
}

QJsonValue repairedLearningValue(const QJsonValue& value)
{
    if (value.isString()) {
        return repairLearningText(value.toString());
    }
    if (value.isArray()) {
        QJsonArray repaired;
        for (const QJsonValue& item : value.toArray()) {
            repaired.append(repairedLearningValue(item));
        }
        return repaired;
    }
    if (value.isObject()) {
        QJsonObject repaired;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            repaired.insert(it.key(), repairedLearningValue(it.value()));
        }
        return repaired;
    }
    return value;
}

QString jsonObjectTitle(const QJsonObject& object)
{
    return repairLearningText(object.value(QStringLiteral("title")).toString(
        object.value(QStringLiteral("topic")).toString(
            object.value(QStringLiteral("id")).toString()))).trimmed();
}

QString slugText(QString value)
{
    value = value.toLower();
    value.replace(QRegularExpression(QStringLiteral("[Ã¤Ã„]")), QStringLiteral("ae"));
    value.replace(QRegularExpression(QStringLiteral("[Ã¶Ã–]")), QStringLiteral("oe"));
    value.replace(QRegularExpression(QStringLiteral("[Ã¼Ãœ]")), QStringLiteral("ue"));
    value.replace(QStringLiteral("ÃŸ"), QStringLiteral("ss"));
    value.replace(QRegularExpression(QStringLiteral(R"([^a-z0-9_\.]+)")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral(R"(_{2,})")), QStringLiteral("_"));
    value = value.trimmed();
    while (value.startsWith(QLatin1Char('_'))) {
        value.remove(0, 1);
    }
    while (value.endsWith(QLatin1Char('_'))) {
        value.chop(1);
    }
    return value.left(72);
}

QJsonArray mergedStringArray(QJsonArray base, const QJsonArray& incoming, int maxCount = 64)
{
    for (const QJsonValue& value : incoming) {
        appendStringUnique(base, value.toString());
        if (base.size() >= maxCount) {
            break;
        }
    }
    return base;
}

QString objectSignature(const QJsonObject& object)
{
    QJsonDocument document(object);
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

QJsonArray mergedObjectArray(QJsonArray base, const QJsonArray& incoming, int maxCount = 24)
{
    QSet<QString> seen;
    for (const QJsonValue& value : base) {
        if (value.isObject()) {
            seen.insert(objectSignature(value.toObject()));
        } else if (value.isString()) {
            seen.insert(value.toString());
        }
    }
    for (const QJsonValue& value : incoming) {
        if (base.size() >= maxCount) {
            break;
        }
        const QString signature = value.isObject()
            ? objectSignature(value.toObject())
            : value.toString();
        if (signature.trimmed().isEmpty() || seen.contains(signature)) {
            continue;
        }
        seen.insert(signature);
        base.append(value);
    }
    return base;
}

QJsonArray firstObjects(const QJsonArray& source, int maxCount)
{
    QJsonArray result;
    for (int i = 0; i < source.size() && i < maxCount; ++i) {
        result.append(source.at(i));
    }
    return result;
}

double boundedConfidence(double value)
{
    return std::max(0.05, std::min(0.98, value));
}

QString normalizedOutcome(QString outcome)
{
    outcome = outcome.trimmed().toLower();
    if (outcome == QStringLiteral("success")) {
        return QStringLiteral("execution_success");
    }
    if (outcome == QStringLiteral("failure")) {
        return QStringLiteral("execution_failure");
    }
    return outcome.isEmpty() ? QStringLiteral("used") : outcome;
}

double confidenceDeltaForOutcome(const QString& outcome)
{
    if (outcome == QStringLiteral("execution_success")) {
        return 0.04;
    }
    if (outcome == QStringLiteral("repair_success")) {
        return 0.05;
    }
    if (outcome == QStringLiteral("user_positive")) {
        return 0.03;
    }
    if (outcome == QStringLiteral("execution_failure")) {
        return -0.08;
    }
    if (outcome == QStringLiteral("repair_failure")) {
        return -0.07;
    }
    if (outcome == QStringLiteral("user_complaint")) {
        return -0.10;
    }
    return 0.0;
}

bool isPositiveRuntimeOutcome(const QString& outcome)
{
    return outcome == QStringLiteral("execution_success")
        || outcome == QStringLiteral("repair_success")
        || outcome == QStringLiteral("user_positive");
}

bool isNegativeRuntimeOutcome(const QString& outcome)
{
    return outcome == QStringLiteral("execution_failure")
        || outcome == QStringLiteral("repair_failure")
        || outcome == QStringLiteral("user_complaint");
}

bool isAuxiliaryRuntimeTool(const QString& tool)
{
    return tool == QStringLiteral("selection.set")
        || tool == QStringLiteral("selection.describe")
        || tool == QStringLiteral("capabilities.list")
        || tool == QStringLiteral("actions.list")
        || tool == QStringLiteral("actions.validate")
        || tool == QStringLiteral("commands.list")
        || tool == QStringLiteral("layers.list");
}

QString primaryRuntimeTool(const QStringList& tools)
{
    for (auto it = tools.crbegin(); it != tools.crend(); ++it) {
        if (!isAuxiliaryRuntimeTool(*it)) {
            return *it;
        }
    }
    return tools.isEmpty() ? QString() : tools.last();
}

QString runtimeTopicForTool(const QString& tool, const QString& prompt)
{
    const QString normalizedPrompt = prompt.toLower();
    if (tool == QStringLiteral("geometry.copy")) {
        return normalizedPrompt.contains(QStringLiteral("handle"))
            ? QStringLiteral("Objekt per Handle kopieren")
            : QStringLiteral("Objekt kopieren");
    }
    if (tool == QStringLiteral("geometry.move")) {
        return QStringLiteral("Objekt verschieben");
    }
    if (tool == QStringLiteral("geometry.rotate")) {
        return QStringLiteral("Objekt drehen");
    }
    if (tool == QStringLiteral("geometry.scale")) {
        return QStringLiteral("Objekt skalieren");
    }
    if (tool == QStringLiteral("geometry.delete")) {
        return QStringLiteral("Objekt loeschen");
    }
    if (tool == QStringLiteral("entity.setLayer")) {
        return QStringLiteral("Objekten Layer zuweisen");
    }
    if (tool == QStringLiteral("geometry.query")) {
        return QStringLiteral("Objektdaten abfragen");
    }
    if (tool == QStringLiteral("measurement.bbox")) {
        return QStringLiteral("Objektmasse per Bounding Box abfragen");
    }
    if (tool == QStringLiteral("layers.create")) {
        return QStringLiteral("Layer erstellen");
    }
    if (tool == QStringLiteral("layers.rename")) {
        return QStringLiteral("Layer umbenennen");
    }
    if (tool == QStringLiteral("bim.classify")) {
        return QStringLiteral("Objekte als BIM klassifizieren");
    }
    if (tool == QStringLiteral("rectangles.extrude") || tool == QStringLiteral("profile.extrude")) {
        return QStringLiteral("Profile extrudieren");
    }
    return tool.isEmpty()
        ? QStringLiteral("BRX Aktion aus erfolgreicher Ausfuehrung")
        : QStringLiteral("%1 anwenden").arg(tool);
}

QJsonArray compactRuntimeActions(const QJsonArray& actions, int maxCount = 8)
{
    QJsonArray compact;
    for (int i = 0; i < actions.size() && i < maxCount; ++i) {
        const QJsonObject action = actions.at(i).toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString().trimmed();
        if (tool.isEmpty()) {
            continue;
        }
        QJsonObject row{
            {QStringLiteral("tool"), tool},
        };
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (!params.isEmpty()) {
            row.insert(QStringLiteral("paramsTemplate"), params);
        }
        const QString title = action.value(QStringLiteral("title")).toString().trimmed();
        if (!title.isEmpty()) {
            row.insert(QStringLiteral("title"), clippedText(title, 120));
        }
        compact.append(row);
    }
    return compact;
}

QJsonArray runtimeConversationPrompts(const QJsonObject& event)
{
    QJsonArray prompts;
    const QJsonArray recent = event.value(QStringLiteral("recentConversation")).toArray();
    for (const QJsonValue& value : recent) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user")) {
            continue;
        }
        appendStringUnique(prompts, clippedText(message.value(QStringLiteral("content")).toString(
            message.value(QStringLiteral("preview")).toString()), 260));
    }
    return prompts;
}

QJsonArray cappedOutcomeHistory(QJsonArray history, QJsonObject event, int maxCount = 20)
{
    event.insert(QStringLiteral("at"), event.value(QStringLiteral("at")).toString(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)));
    const QString prompt = event.value(QStringLiteral("prompt")).toString();
    if (prompt.size() > 700) {
        event.insert(QStringLiteral("prompt"), prompt.left(700));
    }
    const QString feedback = event.value(QStringLiteral("feedback")).toString();
    if (feedback.size() > 700) {
        event.insert(QStringLiteral("feedback"), feedback.left(700));
    }
    const QString summary = event.value(QStringLiteral("summary")).toString();
    if (summary.size() > 700) {
        event.insert(QStringLiteral("summary"), summary.left(700));
    }
    history.append(event);
    while (history.size() > maxCount) {
        history.removeAt(0);
    }
    return history;
}

QString lessonIdFromValue(const QJsonValue& value)
{
    QString id;
    if (value.isString()) {
        id = value.toString();
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        id = object.value(QStringLiteral("id")).toString(
            object.value(QStringLiteral("lessonId")).toString(
                object.value(QStringLiteral("workflowId")).toString()));
    }
    id = id.trimmed();
    if (id.startsWith(QStringLiteral("lesson:"))) {
        id = id.mid(7).trimmed();
    }
    return id;
}

bool isProtectedWorkflow(const QJsonObject& lesson)
{
    return lesson.value(QStringLiteral("updateProtected")).toBool(false)
        || lesson.value(QStringLiteral("source")).toString() == QStringLiteral("canonical_building_workflow");
}

bool isAiOwnedWorkflow(const QJsonObject& lesson)
{
    if (isProtectedWorkflow(lesson)) {
        return false;
    }
    const QString source = lesson.value(QStringLiteral("source")).toString().trimmed();
    return source == QStringLiteral("ai_runtime")
        || source == QStringLiteral("brx_runtime")
        || source == QStringLiteral("local_ai")
        || lesson.value(QStringLiteral("id")).toString().startsWith(QStringLiteral("ai_"));
}

QJsonObject normalizedAiWorkflow(QJsonObject lesson, const QString& now)
{
    const QString title = repairLearningText(lesson.value(QStringLiteral("title")).toString(
        lesson.value(QStringLiteral("topic")).toString(
            lesson.value(QStringLiteral("intent")).toString()))).trimmed();
    lesson.insert(QStringLiteral("title"), title);
    lesson.insert(QStringLiteral("topic"), lesson.value(QStringLiteral("topic")).toString(title));
    lesson.insert(QStringLiteral("description"), lesson.value(QStringLiteral("description")).toString(
        lesson.value(QStringLiteral("intent")).toString()));
    lesson.insert(QStringLiteral("status"), QStringLiteral("active"));
    lesson.insert(QStringLiteral("source"), QStringLiteral("ai_runtime"));
    lesson.insert(QStringLiteral("updateProtected"), false);
    lesson.insert(QStringLiteral("intentPatterns"), lesson.value(QStringLiteral("intentPatterns")).toArray());
    lesson.insert(QStringLiteral("assumptions"), lesson.value(QStringLiteral("assumptions")).toArray());
    lesson.insert(QStringLiteral("requiredSlots"), lesson.value(QStringLiteral("requiredSlots")).toArray());
    lesson.insert(QStringLiteral("knownSlotValues"), lesson.value(QStringLiteral("knownSlotValues")).toObject());
    lesson.insert(QStringLiteral("derivedValues"), lesson.value(QStringLiteral("derivedValues")).toObject());
    lesson.insert(QStringLiteral("strategy"), lesson.value(QStringLiteral("strategy")).toArray());
    lesson.insert(QStringLiteral("executionBatches"), lesson.value(QStringLiteral("executionBatches")).toArray());
    QJsonArray validationExamples = lesson.value(QStringLiteral("validationExamples")).toArray();
    if (validationExamples.isEmpty()) {
        validationExamples = lesson.value(QStringLiteral("successfulExamples")).toArray();
    }
    lesson.insert(QStringLiteral("validationExamples"), validationExamples);
    lesson.remove(QStringLiteral("successfulExamples"));
    lesson.insert(QStringLiteral("knownFailures"), lesson.value(QStringLiteral("knownFailures")).toArray());
    lesson.insert(QStringLiteral("repairRules"), lesson.value(QStringLiteral("repairRules")).toArray());
    lesson.insert(QStringLiteral("recommendedTools"), lesson.value(QStringLiteral("recommendedTools")).toArray());
    lesson.insert(QStringLiteral("createdAt"), lesson.value(QStringLiteral("createdAt")).toString(now));
    lesson.insert(QStringLiteral("updatedAt"), now);
    return lesson;
}

} // namespace

void BricsCadLearningAgent::setProjectRootPath(const QString& path)
{
    m_projectRootPath = path;
}

QString BricsCadLearningAgent::projectLearningPath() const
{
    if (m_projectRootPath.trimmed().isEmpty()) {
        return {};
    }
    return QDir(m_projectRootPath).filePath(QStringLiteral("agent/bricscad-learning/brx-learning.json"));
}

bool BricsCadLearningAgent::loadFromPath(const QString& path, QJsonObject* document, QString* errorMessage) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        if (errorMessage) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }
    if (document) {
        *document = parsed.object();
    }
    return true;
}

bool BricsCadLearningAgent::load(QString* errorMessage)
{
    QJsonObject loaded;
    const QString projectPath = projectLearningPath();
    QString localError;
    if (!projectPath.isEmpty() && QFileInfo::exists(projectPath) && loadFromPath(projectPath, &loaded, &localError)) {
        m_document = repairedLearningValue(loaded).toObject();
        m_sourcePath = projectPath;
        m_loadedFromProjectFile = true;
        rebuildIndexes();
        return true;
    }

    if (loadFromPath(QString::fromLatin1(kLearningResourcePath), &loaded, &localError)) {
        m_document = repairedLearningValue(loaded).toObject();
        m_sourcePath = QString::fromLatin1(kLearningResourcePath);
        m_loadedFromProjectFile = false;
        rebuildIndexes();
        return true;
    }

    if (errorMessage) {
        *errorMessage = localError.isEmpty()
            ? QStringLiteral("brx-learning.json konnte nicht geladen werden.")
            : localError;
    }
    m_document = {};
    m_sourcePath.clear();
    m_loadedFromProjectFile = false;
    rebuildIndexes();
    return false;
}

void BricsCadLearningAgent::rebuildIndexes()
{
    m_toolProfilesByName = {};
    for (const QJsonValue& value : m_document.value(QStringLiteral("toolProfiles")).toArray()) {
        const QJsonObject profile = value.toObject();
        const QString name = profile.value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            m_toolProfilesByName.insert(name, profile);
        }
    }

    m_lessonsById = {};
    for (const QJsonValue& value : m_document.value(QStringLiteral("lessons")).toArray()) {
        const QJsonObject lesson = value.toObject();
        const QString id = lesson.value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty()) {
            m_lessonsById.insert(id, lesson);
        }
    }
}

QJsonObject BricsCadLearningAgent::document() const
{
    return m_document;
}

QJsonObject BricsCadLearningAgent::metadata() const
{
    QJsonObject metadata = m_document.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("loaded"), !m_document.isEmpty());
    metadata.insert(QStringLiteral("updatedAt"), m_document.value(QStringLiteral("updatedAt")).toString());
    metadata.insert(QStringLiteral("sourcePath"), m_sourcePath);
    metadata.insert(QStringLiteral("source"), m_loadedFromProjectFile ? QStringLiteral("project") : QStringLiteral("resource"));
    metadata.insert(QStringLiteral("loadedFromProjectFile"), m_loadedFromProjectFile);
    return metadata;
}

QJsonObject BricsCadLearningAgent::policy() const
{
    return m_document.value(QStringLiteral("learningPolicy")).toObject();
}

QString BricsCadLearningAgent::sourcePath() const
{
    return m_sourcePath;
}

bool BricsCadLearningAgent::loadedFromProjectFile() const
{
    return m_loadedFromProjectFile;
}

QJsonObject BricsCadLearningAgent::toolProfile(const QString& name) const
{
    return m_toolProfilesByName.value(name).toObject();
}

QJsonArray BricsCadLearningAgent::toolProfiles() const
{
    return m_document.value(QStringLiteral("toolProfiles")).toArray();
}

QJsonObject BricsCadLearningAgent::compactToolProfile(const QJsonObject& profile)
{
    QJsonObject compact{
        {QStringLiteral("id"), QStringLiteral("tool:%1").arg(profile.value(QStringLiteral("name")).toString())},
        {QStringLiteral("title"), profile.value(QStringLiteral("name")).toString()},
        {QStringLiteral("name"), profile.value(QStringLiteral("name")).toString()},
        {QStringLiteral("domain"), profile.value(QStringLiteral("domain")).toString()},
        {QStringLiteral("kind"), QStringLiteral("tool")},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("description"), profile.value(QStringLiteral("summary")).toString().left(420)},
        {QStringLiteral("summary"), profile.value(QStringLiteral("summary")).toString().left(220)},
        {QStringLiteral("keywords"), firstObjects(profile.value(QStringLiteral("keywords")).toArray(), 16)},
        {QStringLiteral("policy"), profile.value(QStringLiteral("policy")).toString()},
        {QStringLiteral("agentHints"), firstObjects(profile.value(QStringLiteral("agentHints")).toArray(), 4)},
        {QStringLiteral("semanticConstraints"), firstObjects(profile.value(QStringLiteral("semanticConstraints")).toArray(), 4)},
        {QStringLiteral("unsupportedOperations"), firstObjects(profile.value(QStringLiteral("unsupportedOperations")).toArray(), 4)},
        {QStringLiteral("examples"), firstObjects(profile.value(QStringLiteral("examples")).toArray(), 3)},
    };
    return compact;
}

QJsonObject BricsCadLearningAgent::compactRepairRule(const QJsonObject& rule, const QJsonObject& metadata)
{
    const QString id = rule.value(QStringLiteral("id")).toString(slugText(jsonObjectTitle(rule)));
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("repair:%1").arg(id)},
        {QStringLiteral("ruleId"), id},
        {QStringLiteral("title"), rule.value(QStringLiteral("topic")).toString(id)},
        {QStringLiteral("topic"), rule.value(QStringLiteral("topic")).toString(id)},
        {QStringLiteral("kind"), QStringLiteral("repair_rule")},
        {QStringLiteral("status"), rule.value(QStringLiteral("status")).toString(QStringLiteral("active"))},
        {QStringLiteral("description"), rule.value(QStringLiteral("instruction")).toString(rule.value(QStringLiteral("repair")).toString()).left(900)},
        {QStringLiteral("failurePattern"), rule.value(QStringLiteral("failurePattern")).toString()},
        {QStringLiteral("instruction"), rule.value(QStringLiteral("instruction")).toString()},
        {QStringLiteral("recommendedTools"), firstObjects(rule.value(QStringLiteral("tools")).toArray(), 12)},
        {QStringLiteral("source"), rule.value(QStringLiteral("source")).toString()},
        {QStringLiteral("updatedAt"), rule.value(QStringLiteral("updatedAt")).toString()},
        {QStringLiteral("learningMetadata"), metadata},
    };
}

QJsonObject BricsCadLearningAgent::compactExample(const QJsonObject& example, const QJsonObject& metadata)
{
    const QString id = example.value(QStringLiteral("id")).toString(slugText(jsonObjectTitle(example)));
    QJsonArray tools;
    for (const QJsonValue& value : example.value(QStringLiteral("actions")).toArray()) {
        appendStringUnique(tools, value.toObject().value(QStringLiteral("tool")).toString());
    }
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("example:%1").arg(id)},
        {QStringLiteral("exampleId"), id},
        {QStringLiteral("title"), example.value(QStringLiteral("title")).toString(id)},
        {QStringLiteral("kind"), QStringLiteral("example")},
        {QStringLiteral("status"), example.value(QStringLiteral("status")).toString(QStringLiteral("active"))},
        {QStringLiteral("description"), example.value(QStringLiteral("description")).toString(example.value(QStringLiteral("intent")).toString()).left(900)},
        {QStringLiteral("recommendedTools"), tools},
        {QStringLiteral("actions"), firstObjects(example.value(QStringLiteral("actions")).toArray(), 8)},
        {QStringLiteral("source"), example.value(QStringLiteral("source")).toString()},
        {QStringLiteral("updatedAt"), example.value(QStringLiteral("updatedAt")).toString(example.value(QStringLiteral("createdAt")).toString())},
        {QStringLiteral("learningMetadata"), metadata},
    };
}

QJsonObject BricsCadLearningAgent::compactLesson(const QJsonObject& lesson, bool detailed)
{
    const QString title = lesson.value(QStringLiteral("title")).toString(
        lesson.value(QStringLiteral("topic")).toString(lesson.value(QStringLiteral("id")).toString()));
    QJsonObject compact{
        {QStringLiteral("id"), lesson.value(QStringLiteral("id")).toString()},
        {QStringLiteral("title"), title},
        {QStringLiteral("topic"), lesson.value(QStringLiteral("topic")).toString(title)},
        {QStringLiteral("intent"), lesson.value(QStringLiteral("intent")).toString().left(detailed ? 900 : 260)},
        {QStringLiteral("description"), lesson.value(QStringLiteral("intent")).toString().left(detailed ? 900 : 260)},
        {QStringLiteral("status"), lesson.value(QStringLiteral("status")).toString(QStringLiteral("active"))},
        {QStringLiteral("kind"), QStringLiteral("learning")},
        {QStringLiteral("source"), lesson.value(QStringLiteral("source")).toString()},
        {QStringLiteral("updateProtected"), isProtectedWorkflow(lesson)},
        {QStringLiteral("aiOwned"), isAiOwnedWorkflow(lesson)},
        {QStringLiteral("readOnly"), isProtectedWorkflow(lesson)},
        {QStringLiteral("sourceFile"), lesson.value(QStringLiteral("sourceFile")).toString()},
        {QStringLiteral("recommendedTools"), firstObjects(lesson.value(QStringLiteral("recommendedTools")).toArray(), detailed ? 18 : 8)},
        {QStringLiteral("intentPatterns"), firstObjects(lesson.value(QStringLiteral("intentPatterns")).toArray(), detailed ? 8 : 3)},
        {QStringLiteral("knownFailures"), firstObjects(lesson.value(QStringLiteral("knownFailures")).toArray(), detailed ? 8 : 3)},
        {QStringLiteral("repairRules"), firstObjects(lesson.value(QStringLiteral("repairRules")).toArray(), detailed ? 8 : 3)},
        {QStringLiteral("validationExamples"), firstObjects(lesson.value(QStringLiteral("validationExamples")).toArray(), detailed ? 3 : 1)},
        {QStringLiteral("strategy"), firstObjects(lesson.value(QStringLiteral("strategy")).toArray(), detailed ? 12 : 4)},
        {QStringLiteral("confidence"), lesson.value(QStringLiteral("confidence")).toDouble()},
        {QStringLiteral("usageCount"), lesson.value(QStringLiteral("usageCount")).toInt()},
        {QStringLiteral("successCount"), lesson.value(QStringLiteral("successCount")).toInt()},
        {QStringLiteral("failureCount"), lesson.value(QStringLiteral("failureCount")).toInt()},
        {QStringLiteral("complaintCount"), lesson.value(QStringLiteral("complaintCount")).toInt()},
        {QStringLiteral("positiveFeedbackCount"), lesson.value(QStringLiteral("positiveFeedbackCount")).toInt()},
        {QStringLiteral("lastOutcome"), lesson.value(QStringLiteral("lastOutcome")).toString()},
        {QStringLiteral("lastOutcomeAt"), lesson.value(QStringLiteral("lastOutcomeAt")).toString()},
        {QStringLiteral("updatedAt"), lesson.value(QStringLiteral("updatedAt")).toString()},
    };
    if (detailed) {
        compact.insert(QStringLiteral("validActionShapes"), firstObjects(lesson.value(QStringLiteral("validActionShapes")).toArray(), 12));
        compact.insert(QStringLiteral("executionBatches"), firstObjects(lesson.value(QStringLiteral("executionBatches")).toArray(), 8));
        compact.insert(QStringLiteral("assumptions"), firstObjects(lesson.value(QStringLiteral("assumptions")).toArray(), 10));
        compact.insert(QStringLiteral("knownSlotValues"), lesson.value(QStringLiteral("knownSlotValues")).toObject());
        compact.insert(QStringLiteral("requiredSlots"), lesson.value(QStringLiteral("requiredSlots")).toArray());
        compact.insert(QStringLiteral("derivedValues"), lesson.value(QStringLiteral("derivedValues")).toObject());
    }
    return compact;
}

QJsonArray BricsCadLearningAgent::lessonIndex() const
{
    QJsonArray index;
    for (const QJsonValue& value : m_document.value(QStringLiteral("lessons")).toArray()) {
        const QJsonObject lesson = value.toObject();
        if (lesson.value(QStringLiteral("status")).toString(QStringLiteral("active")) == QStringLiteral("merged")) {
            continue;
        }
        QJsonObject row = compactLesson(lesson, true);
        row.insert(QStringLiteral("learningMetadata"), metadata());
        index.append(row);
    }
    return index;
}

QJsonArray BricsCadLearningAgent::learningIndex() const
{
    QJsonArray index;
    const QJsonObject meta = metadata();

    for (const QJsonValue& value : m_document.value(QStringLiteral("toolProfiles")).toArray()) {
        QJsonObject profile = compactToolProfile(value.toObject());
        profile.insert(QStringLiteral("learningMetadata"), meta);
        index.append(profile);
    }

    for (const QJsonValue& value : m_document.value(QStringLiteral("lessons")).toArray()) {
        const QJsonObject lesson = value.toObject();
        if (lesson.value(QStringLiteral("status")).toString(QStringLiteral("active")) == QStringLiteral("merged")) {
            continue;
        }
        QJsonObject row = compactLesson(lesson, true);
        row.insert(QStringLiteral("kind"), QStringLiteral("lesson"));
        row.insert(QStringLiteral("learningMetadata"), meta);
        index.append(row);

        int exampleIndex = 0;
        for (const QJsonValue& exampleValue : lesson.value(QStringLiteral("validationExamples")).toArray()) {
            QJsonObject example = exampleValue.toObject();
            if (example.value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
                example.insert(QStringLiteral("id"), QStringLiteral("%1_example_%2")
                    .arg(lesson.value(QStringLiteral("id")).toString(), QString::number(++exampleIndex)));
            }
            if (example.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
                example.insert(QStringLiteral("title"), QStringLiteral("%1 - Beispiel %2")
                    .arg(lesson.value(QStringLiteral("topic")).toString(lesson.value(QStringLiteral("id")).toString()), QString::number(exampleIndex)));
            }
            example.insert(QStringLiteral("source"), example.value(QStringLiteral("source")).toString(QStringLiteral("lesson")));
            example.insert(QStringLiteral("lessonId"), lesson.value(QStringLiteral("id")).toString());
            QJsonObject rowExample = compactExample(example, meta);
            rowExample.insert(QStringLiteral("lessonId"), lesson.value(QStringLiteral("id")).toString());
            index.append(rowExample);
        }
    }

    for (const QJsonValue& value : m_document.value(QStringLiteral("repairRules")).toArray()) {
        index.append(compactRepairRule(value.toObject(), meta));
    }
    for (const QJsonValue& value : m_document.value(QStringLiteral("successfulExamples")).toArray()) {
        index.append(compactExample(value.toObject(), meta));
    }

    return index;
}

QJsonObject BricsCadLearningAgent::lessonById(const QString& id) const
{
    return m_lessonsById.value(id).toObject();
}

QString BricsCadLearningAgent::normalizedText(QString text)
{
    text = text.toLower();
    text.replace(QRegularExpression(QStringLiteral("[äÄ]")), QStringLiteral("ae"));
    text.replace(QRegularExpression(QStringLiteral("[öÖ]")), QStringLiteral("oe"));
    text.replace(QRegularExpression(QStringLiteral("[üÜ]")), QStringLiteral("ue"));
    text.replace(QStringLiteral("ß"), QStringLiteral("ss"));
    text.replace(QRegularExpression(QStringLiteral(R"([^a-z0-9_\.]+)")), QStringLiteral(" "));
    return text.simplified();
}

QString BricsCadLearningAgent::lessonSearchText(const QJsonObject& lesson)
{
    QStringList parts;
    auto appendValue = [&](const QJsonValue& value, auto&& appendValueRef) -> void {
        if (value.isString()) {
            parts << value.toString();
        } else if (value.isArray()) {
            for (const QJsonValue& item : value.toArray()) {
                appendValueRef(item, appendValueRef);
            }
        } else if (value.isObject()) {
            const QJsonObject object = value.toObject();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                parts << it.key();
                appendValueRef(it.value(), appendValueRef);
            }
        }
    };

    for (const QString& key : {
             QStringLiteral("id"),
             QStringLiteral("topic"),
             QStringLiteral("title"),
             QStringLiteral("intent"),
             QStringLiteral("intentPatterns"),
             QStringLiteral("promptExamples"),
             QStringLiteral("recommendedTools"),
             QStringLiteral("strategy"),
             QStringLiteral("knownFailures"),
             QStringLiteral("repairRules"),
             QStringLiteral("validActionShapes"),
             QStringLiteral("executionBatches"),
             QStringLiteral("validationExamples"),
             QStringLiteral("requiredSlots"),
             QStringLiteral("knownSlotValues"),
             QStringLiteral("derivedValues"),
         }) {
        appendValue(lesson.value(key), appendValue);
    }
    return normalizedText(parts.join(QLatin1Char(' ')));
}

int BricsCadLearningAgent::scoreLesson(const QJsonObject& lesson, const QStringList& terms, const QString& normalizedPrompt)
{
    if (lesson.value(QStringLiteral("status")).toString(QStringLiteral("active")) != QStringLiteral("active")) {
        return 0;
    }

    const QString haystack = lessonSearchText(lesson);
    int score = 0;
    for (const QString& term : terms) {
        if (haystack.contains(term)) {
            score += term.size() > 5 ? 3 : 2;
        }
    }

    const QString title = normalizedText(lesson.value(QStringLiteral("topic")).toString());
    if (!title.isEmpty() && (normalizedPrompt.contains(title.left(48)) || title.contains(normalizedPrompt.left(48)))) {
        score += 18;
    }
    for (const QJsonValue& value : lesson.value(QStringLiteral("intentPatterns")).toArray()) {
        const QString pattern = normalizedText(value.toString());
        if (!pattern.isEmpty() && (normalizedPrompt.contains(pattern.left(48)) || pattern.contains(normalizedPrompt.left(48)))) {
            score += 14;
        }
    }
    if (score <= 0) {
        return 0;
    }

    const double confidence = lesson.value(QStringLiteral("confidence")).toDouble(0.55);
    score += static_cast<int>((confidence - 0.55) * 20.0);
    score += std::min(8, lesson.value(QStringLiteral("usageCount")).toInt(0) / 2);
    score += std::min(10, lesson.value(QStringLiteral("successCount")).toInt(0));
    score -= std::min(12, lesson.value(QStringLiteral("failureCount")).toInt(0) * 2);
    score -= std::min(10, lesson.value(QStringLiteral("complaintCount")).toInt(0) * 2);

    const QString lastOutcome = lesson.value(QStringLiteral("lastOutcome")).toString();
    if (lastOutcome == QStringLiteral("execution_success")
        || lastOutcome == QStringLiteral("repair_success")
        || lastOutcome == QStringLiteral("user_positive")) {
        score += 3;
    } else if (lastOutcome == QStringLiteral("execution_failure")
        || lastOutcome == QStringLiteral("repair_failure")
        || lastOutcome == QStringLiteral("user_complaint")) {
        score -= 4;
    }
    return std::max(0, score);
}

QJsonArray BricsCadLearningAgent::relevantLessons(const QString& prompt, int maxCount) const
{
    const QString normalizedPrompt = normalizedText(prompt);
    QStringList terms = normalizedPrompt.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
    terms.erase(std::remove_if(terms.begin(), terms.end(), [](const QString& term) {
        return term.size() < 3
            || term == QStringLiteral("und")
            || term == QStringLiteral("oder")
            || term == QStringLiteral("der")
            || term == QStringLiteral("die")
            || term == QStringLiteral("das")
            || term == QStringLiteral("mit")
            || term == QStringLiteral("alle")
            || term == QStringLiteral("eine")
            || term == QStringLiteral("einen")
            || term == QStringLiteral("bitte");
    }), terms.end());
    terms.removeDuplicates();

    QVector<QPair<int, QJsonObject>> scored;
    for (const QJsonValue& value : m_document.value(QStringLiteral("lessons")).toArray()) {
        const QJsonObject lesson = value.toObject();
        const int score = scoreLesson(lesson, terms, normalizedPrompt);
        if (score >= 4) {
            QJsonObject compact = compactLesson(lesson, scored.isEmpty());
            compact.insert(QStringLiteral("matchScore"), score);
            scored.append(qMakePair(score, compact));
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first == b.first) {
            return a.second.value(QStringLiteral("id")).toString() < b.second.value(QStringLiteral("id")).toString();
        }
        return a.first > b.first;
    });

    QJsonArray result;
    for (int i = 0; i < scored.size() && i < maxCount; ++i) {
        result.append(scored.at(i).second);
    }
    return result;
}

QJsonObject BricsCadLearningAgent::contextForPrompt(const QString& prompt, int maxLessons) const
{
    const QJsonArray lessons = relevantLessons(prompt, maxLessons);
    QJsonArray tools;
    for (const QJsonValue& value : m_document.value(QStringLiteral("toolProfiles")).toArray()) {
        const QJsonObject profile = value.toObject();
        if (!profile.isEmpty()) {
            tools.append(compactToolProfile(profile));
        }
    }

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.bricscad-learning-context.v2")},
        {QStringLiteral("metadata"), metadata()},
        {QStringLiteral("policy"), policy()},
        {QStringLiteral("lessons"), lessons},
        {QStringLiteral("toolProfiles"), tools},
        {QStringLiteral("repairRules"), firstObjects(m_document.value(QStringLiteral("repairRules")).toArray(), 8)},
        {QStringLiteral("workflowStructure"), QJsonObject{
            {QStringLiteral("requiredFields"), QJsonArray{
                QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("intent"), QStringLiteral("intentPatterns"),
                QStringLiteral("assumptions"), QStringLiteral("requiredSlots"), QStringLiteral("knownSlotValues"),
                QStringLiteral("derivedValues"), QStringLiteral("strategy"), QStringLiteral("executionBatches"),
                QStringLiteral("validationExamples"), QStringLiteral("knownFailures"), QStringLiteral("repairRules"),
                QStringLiteral("recommendedTools"), QStringLiteral("status"), QStringLiteral("source"),
                QStringLiteral("updateProtected"), QStringLiteral("createdAt"), QStringLiteral("updatedAt")}},
            {QStringLiteral("updatePolicy"), QStringLiteral("Workflows mit updateProtected=true oder source=canonical_building_workflow sind read-only. Die lokale AI darf nur source=ai_runtime und updateProtected=false anlegen oder aktualisieren.")},
            {QStringLiteral("executionPolicy"), QStringLiteral("executionBatches muessen sequential, stopOnFailure=true und konkrete steps mit tool und paramsTemplate enthalten; validationExamples enthalten platzhalterfreie actions.")},
        }},
        {QStringLiteral("selectionPolicy"), QStringLiteral("Nutze diese Lessons als kompaktes Erfahrungswissen, nicht als starre Rezepte. Pruefe vor der Anwendung, ob Prompt, Zeichnungskontext und Berechnung wirklich zur Lesson passen. confidence, usageCount, successCount, failureCount, complaintCount und lastOutcome sind Zuverlaessigkeitssignale: erfolgreiche, haeufig bestaetigte Lessons bevorzugen; Lessons mit Fehlern oder Nutzerkritik vorsichtig pruefen. Uebernimm feste Filter wie layer=0, Handles, Namen, Winkel oder Beispielmasse nur, wenn der Nutzer sie im aktuellen Prompt nennt. Bei selbst erzeugten Objekten muessen Folgeschritte exakte Handles, lastResult/lastExtruded oder autoHandlesFromBatch nutzen; keine breiten currentSpace/Layer-Selektoren auf bestehende Zeichnungsobjekte. BRX Runtime-Capabilities bleiben verbindlich. Erst read-only pruefen, Defaults ableiten, Berechnung plausibilisieren, validieren, dann gezielt rueckfragen.")},
    };
}

bool BricsCadLearningAgent::saveProjectDocument(QString* errorMessage) const
{
    const QString path = projectLearningPath();
    if (path.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Projektpfad fuer brx-learning.json ist nicht bekannt.");
        }
        return false;
    }

    QDir dir(QFileInfo(path).absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Learning-Verzeichnis konnte nicht erstellt werden: %1").arg(dir.absolutePath());
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(m_document).toJson(QJsonDocument::Indented));
    file.write("\n");
    return true;
}

QString BricsCadLearningAgent::generatedLessonId(const QJsonObject& lesson, const QJsonArray& lessons) const
{
    QString base = slugText(lesson.value(QStringLiteral("id")).toString(
        lesson.value(QStringLiteral("topic")).toString(
            lesson.value(QStringLiteral("title")).toString(
                lesson.value(QStringLiteral("intent")).toString()))));
    if (base.isEmpty()) {
        base = QStringLiteral("lesson");
    }
    if (!base.startsWith(QStringLiteral("ai_")) && !base.startsWith(QStringLiteral("seed_"))) {
        base = QStringLiteral("ai_%1").arg(base);
    }

    QSet<QString> existing;
    for (const QJsonValue& value : lessons) {
        existing.insert(value.toObject().value(QStringLiteral("id")).toString());
    }
    if (!existing.contains(base)) {
        return base;
    }
    int suffix = 2;
    QString candidate;
    do {
        candidate = QStringLiteral("%1_%2").arg(base).arg(suffix++);
    } while (existing.contains(candidate));
    return candidate;
}

int BricsCadLearningAgent::matchingLessonIndex(const QJsonObject& lesson, const QJsonArray& lessons) const
{
    const QString id = lesson.value(QStringLiteral("id")).toString().trimmed();
    const QString topic = normalizedText(lesson.value(QStringLiteral("topic")).toString(
        lesson.value(QStringLiteral("title")).toString()));
    const QString intent = normalizedText(lesson.value(QStringLiteral("intent")).toString(
        lesson.value(QStringLiteral("description")).toString()));
    QSet<QString> incomingTools;
    for (const QString& tool : jsonStringArray(lesson.value(QStringLiteral("recommendedTools")).toArray())) {
        incomingTools.insert(tool);
    }

    int bestIndex = -1;
    int bestScore = 0;
    for (int i = 0; i < lessons.size(); ++i) {
        const QJsonObject existing = lessons.at(i).toObject();
        if (!id.isEmpty() && existing.value(QStringLiteral("id")).toString() == id) {
            return i;
        }
        if (isProtectedWorkflow(existing)) {
            continue;
        }

        int score = 0;
        const QString existingTopic = normalizedText(existing.value(QStringLiteral("topic")).toString());
        const QString existingIntent = normalizedText(existing.value(QStringLiteral("intent")).toString());
        if (!topic.isEmpty() && !existingTopic.isEmpty()
            && (topic == existingTopic || topic.contains(existingTopic) || existingTopic.contains(topic))) {
            score += 40;
        }
        if (!intent.isEmpty() && !existingIntent.isEmpty()
            && (intent.left(80) == existingIntent.left(80)
                || intent.contains(existingIntent.left(80))
                || existingIntent.contains(intent.left(80)))) {
            score += 20;
        }
        const QStringList existingTools = jsonStringArray(existing.value(QStringLiteral("recommendedTools")).toArray());
        for (const QString& tool : existingTools) {
            if (incomingTools.contains(tool)) {
                score += 4;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }
    return bestScore >= 24 ? bestIndex : -1;
}

void BricsCadLearningAgent::refreshMetadata()
{
    const QJsonArray lessons = m_document.value(QStringLiteral("lessons")).toArray();
    int active = 0;
    int deprecated = 0;
    int protectedCount = 0;
    int aiOwnedCount = 0;
    int examples = 0;
    for (const QJsonValue& value : lessons) {
        const QJsonObject lesson = value.toObject();
        const QString status = lesson.value(QStringLiteral("status")).toString(QStringLiteral("active"));
        if (status == QStringLiteral("active")) {
            ++active;
        } else if (status == QStringLiteral("deprecated")) {
            ++deprecated;
        }
        if (isProtectedWorkflow(lesson)) {
            ++protectedCount;
        } else if (isAiOwnedWorkflow(lesson)) {
            ++aiOwnedCount;
        }
        examples += lesson.value(QStringLiteral("validationExamples")).toArray().size();
    }

    QJsonObject meta = m_document.value(QStringLiteral("metadata")).toObject();
    meta.insert(QStringLiteral("lessonCount"), lessons.size());
    meta.insert(QStringLiteral("activeCount"), active);
    meta.insert(QStringLiteral("deprecatedCount"), deprecated);
    meta.insert(QStringLiteral("protectedWorkflowCount"), protectedCount);
    meta.insert(QStringLiteral("aiOwnedWorkflowCount"), aiOwnedCount);
    meta.insert(QStringLiteral("repairRuleCount"), m_document.value(QStringLiteral("repairRules")).toArray().size());
    meta.insert(QStringLiteral("successfulExampleCount"), examples);
    meta.insert(QStringLiteral("validationExampleCount"), examples);
    meta.insert(QStringLiteral("toolProfileCount"), m_document.value(QStringLiteral("toolProfiles")).toArray().size());
    m_document.insert(QStringLiteral("metadata"), meta);
    m_document.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

bool BricsCadLearningAgent::upsertLesson(QJsonObject lesson, QString* appliedChange, QString* errorMessage)
{
    QString topic = lesson.value(QStringLiteral("topic")).toString(
        lesson.value(QStringLiteral("title")).toString()).trimmed();
    QString intent = lesson.value(QStringLiteral("intent")).toString(
        lesson.value(QStringLiteral("description")).toString()).trimmed();
    if (topic.isEmpty() && intent.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Learning-Lesson ohne topic/title oder intent wurde abgelehnt.");
        }
        return false;
    }
    if (topic.isEmpty()) {
        topic = intent.left(90).trimmed();
    }
    if (intent.isEmpty()) {
        intent = topic;
    }

    QJsonArray recommendedTools;
    for (const QString& tool : jsonStringArray(lesson.value(QStringLiteral("recommendedTools")).toArray())) {
        if (!m_toolProfilesByName.value(tool).toObject().isEmpty()) {
            appendStringUnique(recommendedTools, tool);
        }
    }
    if (recommendedTools.isEmpty()) {
        for (const QString& tool : jsonStringArray(lesson.value(QStringLiteral("tools")).toArray())) {
            if (!m_toolProfilesByName.value(tool).toObject().isEmpty()) {
                appendStringUnique(recommendedTools, tool);
            }
        }
    }

    QJsonArray lessons = m_document.value(QStringLiteral("lessons")).toArray();
    lesson.insert(QStringLiteral("topic"), topic);
    lesson.insert(QStringLiteral("title"), topic);
    lesson.insert(QStringLiteral("intent"), intent);
    lesson.insert(QStringLiteral("description"), intent);
    lesson.insert(QStringLiteral("recommendedTools"), recommendedTools);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    lesson = normalizedAiWorkflow(lesson, now);

    const int existingIndex = matchingLessonIndex(lesson, lessons);
    if (existingIndex >= 0) {
        QJsonObject existing = lessons.at(existingIndex).toObject();
        if (!isAiOwnedWorkflow(existing)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Workflow ist schreibgeschuetzt und darf nicht durch die lokale AI aktualisiert werden: %1")
                    .arg(existing.value(QStringLiteral("id")).toString());
            }
            return false;
        }
        if (intent.size() > existing.value(QStringLiteral("intent")).toString().size()) {
            existing.insert(QStringLiteral("intent"), intent);
            existing.insert(QStringLiteral("description"), intent);
        }
        if (topic.size() > existing.value(QStringLiteral("topic")).toString().size()) {
            existing.insert(QStringLiteral("topic"), topic);
            existing.insert(QStringLiteral("title"), topic);
        }
        for (const QString& key : {
                 QStringLiteral("intentPatterns"),
                 QStringLiteral("promptExamples"),
                 QStringLiteral("recommendedTools"),
                 QStringLiteral("assumptions"),
                 QStringLiteral("strategy"),
                 QStringLiteral("knownFailures"),
                 QStringLiteral("repairRules"),
             }) {
            existing.insert(key, mergedStringArray(existing.value(key).toArray(), lesson.value(key).toArray()));
        }
        for (const QString& key : {
                 QStringLiteral("validActionShapes"),
                 QStringLiteral("executionBatches"),
                 QStringLiteral("validationExamples"),
             }) {
            existing.insert(key, mergedObjectArray(existing.value(key).toArray(), lesson.value(key).toArray()));
        }
        for (const QString& key : {
                 QStringLiteral("successCount"),
                 QStringLiteral("failureCount"),
                 QStringLiteral("complaintCount"),
                 QStringLiteral("positiveFeedbackCount"),
             }) {
            const int incomingCount = lesson.value(key).toInt(0);
            if (incomingCount > 0) {
                existing.insert(key, existing.value(key).toInt(0) + incomingCount);
            }
        }
        if (lesson.contains(QStringLiteral("lastOutcome"))) {
            existing.insert(QStringLiteral("lastOutcome"), lesson.value(QStringLiteral("lastOutcome")).toString());
        }
        if (lesson.contains(QStringLiteral("lastOutcomeAt"))) {
            existing.insert(QStringLiteral("lastOutcomeAt"), lesson.value(QStringLiteral("lastOutcomeAt")).toString());
        }
        const double incomingConfidence = lesson.value(QStringLiteral("confidence")).toDouble(-1.0);
        if (incomingConfidence >= 0.05) {
            const double currentConfidence = existing.value(QStringLiteral("confidence")).toDouble(0.55);
            existing.insert(QStringLiteral("confidence"), boundedConfidence(std::max(currentConfidence, incomingConfidence)));
        }
        existing.insert(QStringLiteral("updatedAt"), now);
        existing.insert(QStringLiteral("source"), QStringLiteral("ai_runtime"));
        existing.insert(QStringLiteral("updateProtected"), false);
        existing.insert(QStringLiteral("usageCount"), existing.value(QStringLiteral("usageCount")).toInt() + 1);
        lessons.replace(existingIndex, existing);
        if (appliedChange) {
            *appliedChange = QStringLiteral("Lesson zusammengefuehrt: %1").arg(existing.value(QStringLiteral("id")).toString());
        }
    } else {
        lesson.insert(QStringLiteral("id"), generatedLessonId(lesson, lessons));
        lesson.insert(QStringLiteral("createdAt"), lesson.value(QStringLiteral("createdAt")).toString(now));
        lesson.insert(QStringLiteral("usageCount"), lesson.value(QStringLiteral("usageCount")).toInt(0));
        lesson.insert(QStringLiteral("confidence"), lesson.value(QStringLiteral("confidence")).toDouble(0.55));
        lessons.append(lesson);
        if (appliedChange) {
            *appliedChange = QStringLiteral("Lesson angelegt: %1").arg(lesson.value(QStringLiteral("id")).toString());
        }
    }
    m_document.insert(QStringLiteral("lessons"), lessons);
    return true;
}

bool BricsCadLearningAgent::upsertRepairRule(QJsonObject rule, QString* appliedChange, QString* errorMessage)
{
    const QString instruction = rule.value(QStringLiteral("instruction")).toString(
        rule.value(QStringLiteral("repair")).toString()).trimmed();
    const QString failure = rule.value(QStringLiteral("failurePattern")).toString(
        rule.value(QStringLiteral("message")).toString()).trimmed();
    if (instruction.isEmpty() && failure.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Repair-Regel ohne failurePattern oder instruction wurde abgelehnt.");
        }
        return false;
    }

    QJsonArray tools;
    for (const QString& tool : jsonStringArray(rule.value(QStringLiteral("tools")).toArray())) {
        if (!m_toolProfilesByName.value(tool).toObject().isEmpty()) {
            appendStringUnique(tools, tool);
        }
    }
    rule.insert(QStringLiteral("tools"), tools);
    rule.insert(QStringLiteral("instruction"), instruction);
    rule.insert(QStringLiteral("failurePattern"), failure);
    rule.insert(QStringLiteral("status"), rule.value(QStringLiteral("status")).toString(QStringLiteral("active")));
    rule.insert(QStringLiteral("source"), rule.value(QStringLiteral("source")).toString(QStringLiteral("ai_runtime")));
    const QString id = rule.value(QStringLiteral("id")).toString(slugText(rule.value(QStringLiteral("topic")).toString(failure))).trimmed();
    rule.insert(QStringLiteral("id"), id.isEmpty() ? QStringLiteral("ai_repair_rule") : id);
    rule.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QJsonArray rules = m_document.value(QStringLiteral("repairRules")).toArray();
    for (int i = 0; i < rules.size(); ++i) {
        QJsonObject existing = rules.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == rule.value(QStringLiteral("id")).toString()
            || normalizedText(existing.value(QStringLiteral("failurePattern")).toString()) == normalizedText(failure)) {
            existing.insert(QStringLiteral("instruction"), instruction.isEmpty() ? existing.value(QStringLiteral("instruction")).toString() : instruction);
            existing.insert(QStringLiteral("failurePattern"), failure.isEmpty() ? existing.value(QStringLiteral("failurePattern")).toString() : failure);
            existing.insert(QStringLiteral("tools"), mergedStringArray(existing.value(QStringLiteral("tools")).toArray(), tools));
            existing.insert(QStringLiteral("updatedAt"), rule.value(QStringLiteral("updatedAt")).toString());
            rules.replace(i, existing);
            m_document.insert(QStringLiteral("repairRules"), rules);
            if (appliedChange) {
                *appliedChange = QStringLiteral("Repair-Regel aktualisiert: %1").arg(existing.value(QStringLiteral("id")).toString());
            }
            return true;
        }
    }

    rules.append(rule);
    m_document.insert(QStringLiteral("repairRules"), rules);
    if (appliedChange) {
        *appliedChange = QStringLiteral("Repair-Regel angelegt: %1").arg(rule.value(QStringLiteral("id")).toString());
    }
    return true;
}

bool BricsCadLearningAgent::appendSuccessfulExample(QJsonObject example, QString* appliedChange, QString* errorMessage)
{
    const QJsonArray actions = example.value(QStringLiteral("actions")).toArray();
    if (actions.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Erfolgsbeispiel ohne actions wurde abgelehnt.");
        }
        return false;
    }
    for (const QJsonValue& value : actions) {
        const QString tool = value.toObject().value(QStringLiteral("tool")).toString().trimmed();
        if (tool.isEmpty() || m_toolProfilesByName.value(tool).toObject().isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Erfolgsbeispiel enthaelt unbekanntes Tool: %1").arg(tool.isEmpty() ? QStringLiteral("<leer>") : tool);
            }
            return false;
        }
    }
    if (example.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        example.insert(QStringLiteral("title"), QStringLiteral("AI Erfolgsbeispiel"));
    }
    if (example.value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
        example.insert(QStringLiteral("id"), QStringLiteral("ai_example_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    }
    example.insert(QStringLiteral("source"), example.value(QStringLiteral("source")).toString(QStringLiteral("ai_runtime")));
    example.insert(QStringLiteral("createdAt"), example.value(QStringLiteral("createdAt")).toString(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)));

    QJsonArray examples = m_document.value(QStringLiteral("successfulExamples")).toArray();
    examples = mergedObjectArray(examples, QJsonArray{example}, 200);
    m_document.insert(QStringLiteral("successfulExamples"), examples);
    if (appliedChange) {
        *appliedChange = QStringLiteral("Erfolgsbeispiel gespeichert: %1").arg(example.value(QStringLiteral("id")).toString());
    }
    return true;
}

bool BricsCadLearningAgent::applyLearningUpdate(const QJsonObject& update, QStringList* appliedChanges, QString* errorMessage)
{
    if (update.isEmpty()) {
        return true;
    }
    if (m_document.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("brx-learning.json ist nicht geladen.");
        }
        return false;
    }

    QStringList changes;
    auto applyLessonValue = [&](const QJsonValue& value) -> bool {
        if (!value.isObject()) {
            return true;
        }
        QString change;
        QString error;
        if (!upsertLesson(value.toObject(), &change, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }
            return false;
        }
        if (!change.isEmpty()) {
            changes << change;
        }
        return true;
    };
    if (update.value(QStringLiteral("lesson")).isObject() && !applyLessonValue(update.value(QStringLiteral("lesson")))) {
        return false;
    }
    for (const QJsonValue& value : update.value(QStringLiteral("lessons")).toArray()) {
        if (!applyLessonValue(value)) {
            return false;
        }
    }

    auto applyRepairValue = [&](const QJsonValue& value) -> bool {
        if (!value.isObject()) {
            return true;
        }
        QString change;
        QString error;
        if (!upsertRepairRule(value.toObject(), &change, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }
            return false;
        }
        if (!change.isEmpty()) {
            changes << change;
        }
        return true;
    };
    for (const QJsonValue& value : update.value(QStringLiteral("repairRules")).toArray()) {
        if (!applyRepairValue(value)) {
            return false;
        }
    }
    if (update.value(QStringLiteral("repairRule")).isObject() && !applyRepairValue(update.value(QStringLiteral("repairRule")))) {
        return false;
    }

    auto applyExampleValue = [&](const QJsonValue& value) -> bool {
        if (!value.isObject()) {
            return true;
        }
        QString change;
        QString error;
        if (!appendSuccessfulExample(value.toObject(), &change, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }
            return false;
        }
        if (!change.isEmpty()) {
            changes << change;
        }
        return true;
    };
    for (const QJsonValue& value : update.value(QStringLiteral("successfulExamples")).toArray()) {
        if (!applyExampleValue(value)) {
            return false;
        }
    }
    for (const QJsonValue& value : update.value(QStringLiteral("examples")).toArray()) {
        if (!applyExampleValue(value)) {
            return false;
        }
    }

    if (changes.isEmpty()) {
        return true;
    }
    refreshMetadata();
    if (!saveProjectDocument(errorMessage)) {
        return false;
    }
    rebuildIndexes();
    if (appliedChanges) {
        *appliedChanges = changes;
    }
    return true;
}

bool BricsCadLearningAgent::recordLessonUse(const QJsonArray& lessonIds, const QJsonObject& event, QStringList* appliedChanges, QString* errorMessage)
{
    if (lessonIds.isEmpty()) {
        return true;
    }
    if (m_document.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("brx-learning.json ist nicht geladen.");
        }
        return false;
    }

    QStringList ids;
    for (const QJsonValue& value : lessonIds) {
        const QString id = lessonIdFromValue(value);
        if (!id.isEmpty() && !ids.contains(id)) {
            ids << id;
        }
    }
    if (ids.isEmpty()) {
        return true;
    }

    QJsonArray lessons = m_document.value(QStringLiteral("lessons")).toArray();
    QStringList changes;
    const QString outcome = normalizedOutcome(event.value(QStringLiteral("outcome")).toString());
    const QStringList eventTools = jsonStringArray(event.value(QStringLiteral("tools")).toArray());
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const double delta = confidenceDeltaForOutcome(outcome);
    bool changed = false;

    for (int i = 0; i < lessons.size(); ++i) {
        QJsonObject lesson = lessons.at(i).toObject();
        const QString id = lesson.value(QStringLiteral("id")).toString().trimmed();
        if (!ids.contains(id)) {
            continue;
        }
        if (isProtectedWorkflow(lesson)) {
            continue;
        }
        const QStringList lessonTools = jsonStringArray(lesson.value(QStringLiteral("recommendedTools")).toArray());
        if (!eventTools.isEmpty() && !lessonTools.isEmpty()) {
            bool sharesTool = false;
            for (const QString& tool : eventTools) {
                if (lessonTools.contains(tool)) {
                    sharesTool = true;
                    break;
                }
            }
            if (!sharesTool) {
                continue;
            }
        }

        lesson.insert(QStringLiteral("usageCount"), lesson.value(QStringLiteral("usageCount")).toInt(0) + 1);
        if (outcome == QStringLiteral("execution_success") || outcome == QStringLiteral("repair_success")) {
            lesson.insert(QStringLiteral("successCount"), lesson.value(QStringLiteral("successCount")).toInt(0) + 1);
        } else if (outcome == QStringLiteral("execution_failure") || outcome == QStringLiteral("repair_failure")) {
            lesson.insert(QStringLiteral("failureCount"), lesson.value(QStringLiteral("failureCount")).toInt(0) + 1);
        } else if (outcome == QStringLiteral("user_complaint")) {
            lesson.insert(QStringLiteral("complaintCount"), lesson.value(QStringLiteral("complaintCount")).toInt(0) + 1);
        } else if (outcome == QStringLiteral("user_positive")) {
            lesson.insert(QStringLiteral("positiveFeedbackCount"), lesson.value(QStringLiteral("positiveFeedbackCount")).toInt(0) + 1);
        }

        const double currentConfidence = lesson.value(QStringLiteral("confidence")).toDouble(0.55);
        lesson.insert(QStringLiteral("confidence"), boundedConfidence(currentConfidence + delta));
        lesson.insert(QStringLiteral("lastOutcome"), outcome);
        lesson.insert(QStringLiteral("lastOutcomeAt"), now);
        lesson.insert(QStringLiteral("updatedAt"), now);

        QJsonObject storedEvent = event;
        storedEvent.insert(QStringLiteral("outcome"), outcome);
        storedEvent.insert(QStringLiteral("at"), now);
        storedEvent.insert(QStringLiteral("confidenceDelta"), delta);
        storedEvent.insert(QStringLiteral("confidenceAfter"), lesson.value(QStringLiteral("confidence")).toDouble());
        lesson.insert(QStringLiteral("outcomeHistory"), cappedOutcomeHistory(lesson.value(QStringLiteral("outcomeHistory")).toArray(), storedEvent));

        lessons.replace(i, lesson);
        changes << QStringLiteral("%1: %2, confidence=%3, usage=%4")
            .arg(id,
                outcome,
                QString::number(lesson.value(QStringLiteral("confidence")).toDouble(), 'f', 2),
                QString::number(lesson.value(QStringLiteral("usageCount")).toInt()));
        changed = true;
    }

    if (!changed) {
        return true;
    }

    m_document.insert(QStringLiteral("lessons"), lessons);
    refreshMetadata();
    if (!saveProjectDocument(errorMessage)) {
        return false;
    }
    rebuildIndexes();
    if (appliedChanges) {
        *appliedChanges = changes;
    }
    return true;
}

bool BricsCadLearningAgent::upsertRuntimeLessonFromEvent(
    const QJsonObject& event,
    QStringList* affectedLessonIds,
    QStringList* appliedChanges,
    QString* errorMessage)
{
    if (affectedLessonIds) {
        affectedLessonIds->clear();
    }
    if (appliedChanges) {
        appliedChanges->clear();
    }
    if (m_document.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("brx-learning.json ist nicht geladen.");
        }
        return false;
    }

    const QString outcome = normalizedOutcome(event.value(QStringLiteral("outcome")).toString());
    if (!isPositiveRuntimeOutcome(outcome)) {
        return true;
    }
    if (event.value(QStringLiteral("source")).toString() == QStringLiteral("user_feedback")) {
        return true;
    }

    QStringList tools = jsonStringArray(event.value(QStringLiteral("tools")).toArray());
    for (const QJsonValue& value : event.value(QStringLiteral("actions")).toArray()) {
        const QString tool = value.toObject().value(QStringLiteral("tool")).toString().trimmed();
        if (!tool.isEmpty() && !tools.contains(tool)) {
            tools << tool;
        }
    }
    if (tools.isEmpty()) {
        return true;
    }

    QJsonArray recommendedTools;
    for (const QString& tool : tools) {
        if (!m_toolProfilesByName.value(tool).toObject().isEmpty()) {
            appendStringUnique(recommendedTools, tool);
        }
    }
    if (recommendedTools.isEmpty()) {
        return true;
    }

    const QString primaryTool = primaryRuntimeTool(jsonStringArray(recommendedTools));
    if (primaryTool.isEmpty() || isAuxiliaryRuntimeTool(primaryTool)) {
        return true;
    }

    QStringList selectedLessonIds;
    for (const QJsonValue& value : event.value(QStringLiteral("selectedLessonIds")).toArray()) {
        const QString id = lessonIdFromValue(value);
        if (!id.isEmpty() && !selectedLessonIds.contains(id)) {
            selectedLessonIds << id;
        }
    }
    if (!selectedLessonIds.isEmpty()) {
        for (const QJsonValue& value : m_document.value(QStringLiteral("lessons")).toArray()) {
            const QJsonObject lesson = value.toObject();
            if (!selectedLessonIds.contains(lesson.value(QStringLiteral("id")).toString())) {
                continue;
            }
            const QStringList lessonTools = jsonStringArray(lesson.value(QStringLiteral("recommendedTools")).toArray());
            if (lessonTools.contains(primaryTool)) {
                return true;
            }
        }
    }

    const QString prompt = clippedText(event.value(QStringLiteral("prompt")).toString(), 500);
    const QString summary = clippedText(event.value(QStringLiteral("summary")).toString(), 700);
    const QJsonObject focus = event.value(QStringLiteral("focusedConversationContext")).toObject();
    const QString focusTopic = clippedText(focus.value(QStringLiteral("topic")).toString(), 220);
    const QString focusSummary = clippedText(focus.value(QStringLiteral("relevantSummary")).toString(), 900);
    const QJsonArray conversationPrompts = runtimeConversationPrompts(event);

    QString intentSource = prompt;
    if (!focusTopic.isEmpty()) {
        intentSource = focusTopic + QStringLiteral(" ") + intentSource;
    }
    for (const QJsonValue& value : conversationPrompts) {
        intentSource += QLatin1Char(' ') + value.toString();
    }

    const QString topic = runtimeTopicForTool(primaryTool, intentSource);
    QStringList intentLines;
    intentLines << QStringLiteral("Aus erfolgreicher BRX-Ausfuehrung gelernt: %1.").arg(topic);
    if (!prompt.isEmpty()) {
        intentLines << QStringLiteral("Aktueller Prompt: %1").arg(prompt);
    }
    if (!focusSummary.isEmpty()) {
        intentLines << QStringLiteral("Fokussierter Verlauf: %1").arg(focusSummary);
    }
    if (!summary.isEmpty()) {
        intentLines << QStringLiteral("Ergebnis: %1").arg(summary);
    }
    intentLines << QStringLiteral("Nutze feste Handles, Namen, Koordinaten, Layer, Winkel oder Beispielwerte nur, wenn der Nutzer sie im aktuellen Prompt nennt oder sie per read-only Kontext erneut bestaetigt wurden.");

    QJsonArray intentPatterns = conversationPrompts;
    appendStringUnique(intentPatterns, prompt);
    appendStringUnique(intentPatterns, focusTopic);
    appendStringUnique(intentPatterns, topic);

    QJsonArray strategy;
    strategy.append(QStringLiteral("Vor mutierenden Aktionen Zeichnungskontext oder Auswahl pruefen, wenn Handle, Layer, Position oder Objektart unklar sind."));
    strategy.append(QStringLiteral("Die Lesson ist Erfahrungswissen: Parameterform uebernehmen, Beispielwerte nicht blind wiederverwenden."));
    strategy.append(QStringLiteral("BRX actions.validate bleibt vor der Ausfuehrung verbindlich."));

    QJsonArray assumptions;
    assumptions.append(QStringLiteral("Feste Runtime-Werte aus successfulExamples sind Beispiele und muessen fuer neue Prompts neu aus Prompt oder Zeichnungskontext abgeleitet werden."));
    assumptions.append(QStringLiteral("Bei vorhandenen Objekten bevorzugt selector.scope=handles verwenden, wenn ein Handle genannt oder zuvor gelesen wurde."));

    QJsonArray validActionShapes = compactRuntimeActions(event.value(QStringLiteral("actions")).toArray());
    QJsonArray validationExamples;
    if (!validActionShapes.isEmpty()) {
        QJsonArray validationActions;
        for (const QJsonValue& value : validActionShapes) {
            const QJsonObject step = value.toObject();
            validationActions.append(QJsonObject{
                {QStringLiteral("tool"), step.value(QStringLiteral("tool")).toString()},
                {QStringLiteral("params"), step.value(QStringLiteral("paramsTemplate")).toObject()},
            });
        }
        validationExamples.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("runtime_example_%1").arg(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("title"), QStringLiteral("Aus erfolgreicher BRX-Ausfuehrung")},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("focusedTopic"), focusTopic},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("tools"), recommendedTools},
            {QStringLiteral("actions"), validationActions},
            {QStringLiteral("source"), QStringLiteral("brx_runtime")},
            {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        });
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonObject lesson{
        {QStringLiteral("topic"), topic},
        {QStringLiteral("title"), topic},
        {QStringLiteral("intent"), intentLines.join(QLatin1Char('\n'))},
        {QStringLiteral("description"), intentLines.join(QLatin1Char('\n'))},
        {QStringLiteral("intentPatterns"), intentPatterns},
        {QStringLiteral("promptExamples"), intentPatterns},
        {QStringLiteral("recommendedTools"), recommendedTools},
        {QStringLiteral("strategy"), strategy},
        {QStringLiteral("assumptions"), assumptions},
        {QStringLiteral("validActionShapes"), validActionShapes},
        {QStringLiteral("requiredSlots"), QJsonArray{}},
        {QStringLiteral("knownSlotValues"), QJsonObject{}},
        {QStringLiteral("derivedValues"), QJsonObject{}},
        {QStringLiteral("executionBatches"), validActionShapes.isEmpty() ? QJsonArray{} : QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("ai_runtime_actions")},
            {QStringLiteral("title"), topic},
            {QStringLiteral("mode"), QStringLiteral("sequential")},
            {QStringLiteral("stopOnFailure"), true},
            {QStringLiteral("steps"), validActionShapes},
        }}},
        {QStringLiteral("validationExamples"), validationExamples},
        {QStringLiteral("knownFailures"), QJsonArray{}},
        {QStringLiteral("repairRules"), QJsonArray{}},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("source"), QStringLiteral("brx_runtime")},
        {QStringLiteral("updateProtected"), false},
        {QStringLiteral("createdAt"), now},
        {QStringLiteral("updatedAt"), now},
        {QStringLiteral("usageCount"), 1},
        {QStringLiteral("successCount"), 1},
        {QStringLiteral("failureCount"), 0},
        {QStringLiteral("complaintCount"), 0},
        {QStringLiteral("positiveFeedbackCount"), 0},
        {QStringLiteral("confidence"), 0.62},
        {QStringLiteral("lastOutcome"), outcome},
        {QStringLiteral("lastOutcomeAt"), now},
    };

    QString change;
    if (!upsertLesson(lesson, &change, errorMessage)) {
        return false;
    }

    QStringList affectedIds;
    const QJsonArray lessons = m_document.value(QStringLiteral("lessons")).toArray();
    const int lessonIndex = matchingLessonIndex(lesson, lessons);
    if (lessonIndex >= 0) {
        const QString id = lessons.at(lessonIndex).toObject().value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty()) {
            affectedIds << id;
        }
    }

    refreshMetadata();
    if (!saveProjectDocument(errorMessage)) {
        return false;
    }
    rebuildIndexes();

    if (affectedLessonIds) {
        *affectedLessonIds = affectedIds;
    }
    if (appliedChanges && !change.isEmpty()) {
        *appliedChanges = QStringList{change};
    }
    return true;
}

bool BricsCadLearningAgent::deprecateLesson(const QString& id, QString* errorMessage)
{
    const QString trimmed = id.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Lesson-ID fehlt.");
        }
        return false;
    }

    QJsonArray lessons = m_document.value(QStringLiteral("lessons")).toArray();
    bool found = false;
    for (int i = 0; i < lessons.size(); ++i) {
        QJsonObject lesson = lessons.at(i).toObject();
        if (lesson.value(QStringLiteral("id")).toString() != trimmed) {
            continue;
        }
        if (isProtectedWorkflow(lesson)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Workflow ist schreibgeschuetzt und kann nicht deaktiviert werden: %1").arg(trimmed);
            }
            return false;
        }
        lesson.insert(QStringLiteral("status"), QStringLiteral("deprecated"));
        lesson.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        lessons.replace(i, lesson);
        found = true;
        break;
    }
    if (!found) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Lesson wurde nicht gefunden: %1").arg(trimmed);
        }
        return false;
    }

    m_document.insert(QStringLiteral("lessons"), lessons);
    QJsonObject meta = m_document.value(QStringLiteral("metadata")).toObject();
    int active = 0;
    int deprecated = 0;
    for (const QJsonValue& value : lessons) {
        const QString status = value.toObject().value(QStringLiteral("status")).toString(QStringLiteral("active"));
        if (status == QStringLiteral("active")) {
            ++active;
        } else if (status == QStringLiteral("deprecated")) {
            ++deprecated;
        }
    }
    meta.insert(QStringLiteral("lessonCount"), lessons.size());
    meta.insert(QStringLiteral("activeCount"), active);
    meta.insert(QStringLiteral("deprecatedCount"), deprecated);
    m_document.insert(QStringLiteral("metadata"), meta);
    m_document.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    if (!saveProjectDocument(errorMessage)) {
        return false;
    }
    rebuildIndexes();
    return true;
}

bool BricsCadLearningAgent::preferLesson(const QString& id, QString* errorMessage)
{
    const QJsonObject lesson = lessonById(id);
    if (lesson.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Lesson wurde nicht gefunden: %1").arg(id);
        }
        return false;
    }
    if (lesson.value(QStringLiteral("status")).toString(QStringLiteral("active")) != QStringLiteral("active")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Lesson ist nicht aktiv: %1").arg(id);
        }
        return false;
    }
    return true;
}
