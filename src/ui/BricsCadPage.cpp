#include "BricsCadPage.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHostAddress>
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
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>

namespace {

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

constexpr const char* kBrxSdkRoot = "C:/Program Files/Bricsys/BRXSDK/BRX26.1.05.0";
constexpr const char* kBrxPluginName = "BareboneBrx.brx";
constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47626;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";
constexpr bool kAgentActionToolsEnabled = true;
constexpr int kMaxAgentValidationRetries = 20;
constexpr int kMaxAgentBatchActions = 50;
constexpr int kAgentBatchActionDelayMs = 1000;
constexpr int kAiModelResponseTimeoutMs = 10 * 60 * 1000;
constexpr int kLocalAiUnreachablePollIntervalMs = 15 * 1000;
constexpr int kLocalAiReachablePollIntervalMs = 60 * 1000;
constexpr int kWorkflowTrainingAiTimeoutMs = kAiModelResponseTimeoutMs;
constexpr int kWorkflowTrainingOutputTokens = 16384;
constexpr int kWorkflowTrainingCompactOutputTokens = 8192;
constexpr qsizetype kMaxDocumentContextChars = 90000;

bool useResponsesApiForModel(const QString& model)
{
    return model.compare(QStringLiteral("openai/gpt-oss-20b"), Qt::CaseInsensitive) == 0;
}

bool useResponsesApiForProvider(const QString& provider, const QString& model)
{
    return provider.compare(QStringLiteral("official"), Qt::CaseInsensitive) == 0
        || useResponsesApiForModel(model);
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

        normalized += QLatin1Char('-');
    }

    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    normalized.replace(QRegularExpression(QStringLiteral("\\s*-\\s*")), QStringLiteral(" - "));
    normalized.replace(QRegularExpression(QStringLiteral("(\\s*-\\s*){2,}")), QStringLiteral(" - "));
    normalized = normalized.trimmed();
    while (normalized.startsWith(QLatin1Char('-'))) {
        normalized.remove(0, 1);
        normalized = normalized.trimmed();
    }
    while (normalized.endsWith(QLatin1Char('-'))) {
        normalized.chop(1);
        normalized = normalized.trimmed();
    }
    return normalized;
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
        "Wenn der gewünschte Seitenbereich oder Inhalt nicht enthalten ist, sage das klar und fordere einen engeren/anderen Bereich an.\n"
        "Dokumente:\n%2\n\n"
        "%3")
        .arg(prompt,
            metadataLines.isEmpty() ? QStringLiteral("- Angehängtes Dokument") : metadataLines.join('\n'),
            selectedText);
}

bool bridgeCapabilitiesContainMethod(const QJsonObject& capabilities, const QString& methodName)
{
    const QJsonArray methods = capabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        if (value.toObject().value("name").toString() == methodName) {
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

QJsonObject brxToolCatalog()
{
    return agentResourceJsonObject(QStringLiteral(":/agent/capabilities/brx-tools.json"));
}

QJsonObject brxToolPolicyForName(const QString& name)
{
    const QJsonArray tools = brxToolCatalog().value(QStringLiteral("tools")).toArray();
    for (const QJsonValue& value : tools) {
        const QJsonObject tool = value.toObject();
        if (tool.value(QStringLiteral("name")).toString() == name) {
            return tool;
        }
    }
    return {};
}

bool brxToolCatalogContains(const QString& name)
{
    return !brxToolPolicyForName(name).isEmpty();
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

QStringList policyRefsForToolName(const QString& name)
{
    Q_UNUSED(name);
    return {};
}

void enrichAgentToolDefinition(QJsonObject& tool)
{
    const QString name = tool.value("name").toString();
    const QJsonObject inputSchema = tool.value("inputSchema").toObject();
    const QJsonObject properties = inputSchema.value("properties").toObject();
    const QJsonObject apiDoc = tool.value("apiDoc").toObject();

    tool.insert("source", tool.value("virtual").toBool(false) ? QStringLiteral("qt") : QStringLiteral("brx"));
    tool.insert("effectiveContractSource", tool.value("virtual").toBool(false)
        ? QStringLiteral("qt-policy")
        : QStringLiteral("brx-capability+qt-policy"));
    tool.insert("policyRefs", stringsToJsonArray(policyRefsForToolName(name)));

    QJsonObject capabilities = tool.value("capabilities").toObject();
    capabilities.insert("requiresConfirmation", tool.value("confirmationRequired").toBool(true));
    capabilities.insert("risk", tool.value("risk").toString("modifiesDrawing"));
    capabilities.insert("bridgeMethod", tool.value("bridgeMethod").toString(name));
    capabilities.insert("supportsSelector", properties.contains("selector"));
    capabilities.insert("supportsTarget", properties.contains("target"));
    capabilities.insert("supportsHandles", properties.contains("handles"));
    capabilities.insert("supportsSaveBefore", properties.contains("saveBefore"));
    capabilities.insert("supportsReason", properties.contains("reason"));
    capabilities.insert("hasExamples", apiDoc.value("post").toObject().value("examples").isArray()
        || apiDoc.value("examples").isArray());
    tool.insert("capabilities", capabilities);

    QJsonArray genericHints;
    appendJsonStringUnique(genericHints, "Nutze dieses Tool nur, wenn die Nutzerabsicht exakt zur Toolbeschreibung und zum inputSchema passt.");
    appendJsonStringUnique(genericHints, "Alle Pflichtfelder aus inputSchema/apiDoc.post.required muessen in params enthalten sein.");
    appendJsonStringUnique(genericHints, "Qt validiert Toolname und Parameter lokal; BRX prueft den Vorschlag danach per actions.validate.");
    mergeJsonStringArray(tool, "agentHints", genericHints);

    QJsonArray genericConstraints;
    appendJsonStringUnique(genericConstraints, "Erfinde keine zusaetzlichen Parameter ausserhalb des Schemas.");
    appendJsonStringUnique(genericConstraints, "Nutze keine direkten AcDb-, LayerTable-, EntityTable- oder sonstigen Datenbank-Schreibzugriffe.");
    if (properties.contains("saveBefore")) {
        appendJsonStringUnique(genericConstraints, "saveBefore ist optional; bei einzelnen riskanten Schreibaktionen kann true verwendet werden, bei Qt-Batches setzt Qt nur die erste Aktion auf true.");
    }
    mergeJsonStringArray(tool, "semanticConstraints", genericConstraints);

    if (!tool.contains("unsupportedOperations")) {
        tool.insert("unsupportedOperations", QJsonArray{});
    }
    if (!tool.contains("examples")) {
        const QJsonArray postExamples = apiDoc.value("post").toObject().value("examples").toArray();
        const QJsonArray rootExamples = apiDoc.value("examples").toArray();
        if (!postExamples.isEmpty()) {
            tool.insert("examples", postExamples);
        } else if (!rootExamples.isEmpty()) {
            tool.insert("examples", rootExamples);
        }
    }

    const QJsonObject catalogEntry = brxToolPolicyForName(name);
    if (!catalogEntry.isEmpty()) {
        tool.insert("domain", catalogEntry.value("domain").toString(tool.value("category").toString()));
        tool.insert("keywords", catalogEntry.value("keywords").toArray());
        tool.insert("policy", catalogEntry.value("policy").toString());
        tool.insert("summary", catalogEntry.value("summary").toString(tool.value("description").toString()));
        tool.insert("catalogPolicy", catalogEntry);
        mergeJsonStringArray(tool, "agentHints", catalogEntry.value("agentHints").toArray());
        mergeJsonStringArray(tool, "semanticConstraints", catalogEntry.value("semanticConstraints").toArray());
        mergeJsonStringArray(tool, "unsupportedOperations", catalogEntry.value("unsupportedOperations").toArray());
        const QJsonArray catalogExamples = catalogEntry.value("examples").toArray();
        if (!catalogExamples.isEmpty()) {
            tool.insert("examples", catalogExamples);
        }
    }

}

bool documentContextHasText(const QJsonObject& context)
{
    return !context.value("selectedText").toString().trimmed().isEmpty();
}

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
    const QString normalized = prompt.toLower();
    return textMentionsAny(normalized, {
               QStringLiteral("wand"),
               QStringLiteral("waende"),
               QStringLiteral("wände"),
               QStringLiteral("wall"),
               QStringLiteral("walls"),
           })
        && textMentionsAny(normalized, {
               QStringLiteral("wandstaerke"),
               QStringLiteral("wandstärke"),
               QStringLiteral("wanddicke"),
               QStringLiteral("wandhoehe"),
               QStringLiteral("wandhöhe"),
               QStringLiteral("hoehe der waende"),
               QStringLiteral("höhe der wände"),
               QStringLiteral("raum"),
               QStringLiteral("grundriss"),
           });
}

bool promptAllowsBimWallClassification(const QString& prompt)
{
    return promptRequestsBimClassification(prompt)
        || promptDescribesArchitecturalWalls(prompt);
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
    });
    const bool explanatoryQuestion =
        normalized.startsWith(QStringLiteral("was ist"))
        || normalized.startsWith(QStringLiteral("was bedeutet"))
        || normalized.startsWith(QStringLiteral("erklaere"))
        || normalized.startsWith(QStringLiteral("erkläre"))
        || normalized.contains(QStringLiteral("definition"));
    const bool mentionsBimWallAction = mentionsCad
        && !explanatoryQuestion
        && promptAllowsBimWallClassification(normalized);
    const bool mentionsCadAction = mentionsCad && !explanatoryQuestion && textMentionsAny(normalized, {
        QStringLiteral("erstelle"),
        QStringLiteral("erstellen"),
        QStringLiteral("zeichne"),
        QStringLiteral("anlegen"),
        QStringLiteral("lege"),
        QStringLiteral("loesche"),
        QStringLiteral("lösche"),
        QStringLiteral("aendere"),
        QStringLiteral("ändere"),
        QStringLiteral("setze"),
        QStringLiteral("verschiebe"),
        QStringLiteral("kopiere"),
        QStringLiteral("extrudi"),
        QStringLiteral("klassifiz"),
        QStringLiteral("klassifizi"),
        QStringLiteral("zuweis"),
        QStringLiteral("umwandel"),
        QStringLiteral("wandle"),
        QStringLiteral("faerbe"),
        QStringLiteral("färbe"),
        QStringLiteral("skaliere"),
        QStringLiteral("speichere"),
        QStringLiteral("fuehre"),
        QStringLiteral("führe"),
        QStringLiteral("ausführen"),
        QStringLiteral("ausfuehren"),
    }) || mentionsBimWallAction;

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
        QString fallbackRoute = fallbackRouteNameForPrompt(prompt, documentContext);
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
    if (route == QStringLiteral("general_chat")) {
        return QJsonArray{"message"};
    }
    if (route == QStringLiteral("document_qa")) {
        return QJsonArray{"message", "ask_user"};
    }
    if (route == QStringLiteral("document_qa_with_cad_context")
        || route == QStringLiteral("bricscad_question")) {
        return QJsonArray{"message", "ask_user", "context_request"};
    }
    if (route == QStringLiteral("execution_summary")) {
        return QJsonArray{"message"};
    }
    if (route == QStringLiteral("bricscad_action")
        || route == QStringLiteral("validation_retry")) {
        QJsonArray types{"message", "ask_user", "context_request", "action_proposal", "workflow_run_proposal"};
        types.append("plan");
        return types;
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
    text.replace(QStringLiteral("ä"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ö"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ü"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ß"), QStringLiteral("ss"));
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
    text = repairMojibakeText(text).trimmed();
    if (text.isEmpty()) {
        return {};
    }

    if ((text.startsWith(QStringLiteral("\\[")) && text.endsWith(QStringLiteral("\\]")))
        || (text.startsWith(QStringLiteral("\\(")) && text.endsWith(QStringLiteral("\\)")))) {
        text = text.mid(2, text.size() - 4).trimmed();
    } else if (text.startsWith(QStringLiteral("$$")) && text.endsWith(QStringLiteral("$$")) && text.size() > 4) {
        text = text.mid(2, text.size() - 4).trimmed();
    } else if (text.startsWith(QLatin1Char('$')) && text.endsWith(QLatin1Char('$')) && text.size() > 2) {
        text = text.mid(1, text.size() - 2).trimmed();
    }

    return text.trimmed();
}

QString normalizeGeneralWorkflowDelimitedMath(
    QString text,
    const QRegularExpression& expression,
    const QString& openDelimiter,
    const QString& closeDelimiter)
{
    qsizetype offset = 0;
    QRegularExpressionMatch match = expression.match(text, offset);
    while (match.hasMatch()) {
        const QString formula = normalizedGeneralWorkflowLatex(match.captured(1));
        if (formula.isEmpty()) {
            offset = match.capturedEnd(0);
        } else {
            const QString replacement = QStringLiteral("%1%2%3").arg(openDelimiter, formula, closeDelimiter);
            text.replace(match.capturedStart(0), match.capturedLength(0), replacement);
            offset = match.capturedStart(0) + replacement.size();
        }
        match = expression.match(text, offset);
    }
    return text;
}

QString normalizeGeneralWorkflowDisplayMathInText(QString text)
{
    text = normalizeGeneralWorkflowDelimitedMath(
        text,
        QRegularExpression(QStringLiteral(R"(\\\[([\s\S]*?)\\\])")),
        QStringLiteral("\\["),
        QStringLiteral("\\]"));
    text = normalizeGeneralWorkflowDelimitedMath(
        text,
        QRegularExpression(QStringLiteral(R"(\$\$([\s\S]*?)\$\$)")),
        QStringLiteral("$$"),
        QStringLiteral("$$"));
    text = normalizeGeneralWorkflowDelimitedMath(
        text,
        QRegularExpression(QStringLiteral(R"(\\\(([\s\S]*?)\\\))")),
        QStringLiteral("\\("),
        QStringLiteral("\\)"));
    return text;
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
    text.replace(QStringLiteral("\\r\\n"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    text.replace(QRegularExpression(QStringLiteral("\\s*\\\\\\[\\s*")), QStringLiteral("\n\\["));
    text.replace(QRegularExpression(QStringLiteral("\\s*\\\\\\]\\s*")), QStringLiteral("\\]\n"));

    const QList<QRegularExpression> formulaPatterns{
        QRegularExpression(QStringLiteral(R"((Grundgleichung(?:\s+lautet)?\s*:?)\s*\$([^$\n]{1,240}=[^$\n]{1,240})\$)"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(((?:Grundgleichung|Formel)(?:\s+lautet)?\s*:?)\s+([^\n.]{1,240}=[^\n.]{1,240})(?:\.)?)"), QRegularExpression::CaseInsensitiveOption),
    };

    for (const QRegularExpression& expression : formulaPatterns) {
        qsizetype offset = 0;
        QRegularExpressionMatch match = expression.match(text, offset);
        while (match.hasMatch()) {
            const QString prefix = match.captured(1).trimmed();
            const QString formula = normalizedGeneralWorkflowLatex(match.captured(2));
            if (formula.isEmpty() || match.captured(0).contains(QStringLiteral("\\["))) {
                offset = match.capturedEnd(0);
            } else {
                const QString replacement = QStringLiteral("%1\n\n\\[%2\\]\n").arg(prefix, formula);
                text.replace(match.capturedStart(0), match.capturedLength(0), replacement);
                offset = match.capturedStart(0) + replacement.size();
            }
            match = expression.match(text, offset);
        }
    }

    text = normalizeGeneralWorkflowDisplayMathInText(text);
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
    return cells.size() >= 2 ? cells : QStringList{};
}

bool isMarkdownTableSeparatorLine(const QString& line)
{
    const QStringList cells = markdownTableCells(line);
    if (cells.size() < 2) {
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
        || trimmed.startsWith(QStringLiteral("• "))
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
                    return QStringLiteral("Formatierungsfehler: tables[%1].rows[%2].cells[%3] ist leer; nutze einen Textwert oder bewusst '—'")
                        .arg(i)
                        .arg(rowIndex)
                        .arg(columnIndex);
                }
            }
        }
    }
    return {};
}

QString latexFormattingValidationError(const QString& text, const QString& location)
{
    Q_UNUSED(text);
    Q_UNUSED(location);
    return {};
}

QString blockFormattingValidationError(const QString& text, int blockIndex)
{
    const QString location = QStringLiteral("display.blocks[%1].text").arg(blockIndex);
    if (text.contains(QStringLiteral("\\n"))) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt rohe \\n-Escape-Sequenzen statt echter Zeilenumbrueche").arg(location);
    }
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
    if (!workflow.contains(QStringLiteral("tags"))) {
        workflow.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("Chat"), QStringLiteral("AI-Entwurf")});
    }
    if (!workflow.contains(QStringLiteral("verificationStatus"))) {
        workflow.insert(QStringLiteral("verificationStatus"), QStringLiteral("AI-Entwurf"));
    }
    if (!workflow.contains(QStringLiteral("contextSummary"))) {
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
    return text.isEmpty() ? QStringLiteral("—") : text;
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

QString generalWorkflowFormulasMarkdown(const QJsonArray& formulas)
{
    QStringList lines;
    for (const QJsonValue& value : formulas) {
        const QJsonObject formula = value.toObject();
        const QString name = repairMojibakeText(formula.value(QStringLiteral("name")).toString(formula.value(QStringLiteral("id")).toString())).trimmed();
        const QString latex = latexFromGeneralWorkflowFormula(formula);
        if (!latex.isEmpty()) {
            if (!name.isEmpty()) {
                lines << QStringLiteral("**%1**").arg(name);
            }
            lines << QStringLiteral("\\[%1\\]").arg(latex);
        }
    }
    return lines.join(QLatin1Char('\n')).trimmed();
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
    lines << QString() << QStringLiteral("**Absätze**");
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
    if (toolName == QStringLiteral("command.execute")
        || toolName == QStringLiteral("document.save")) {
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
    });
    const bool mentionsMove = textMentionsAny(normalizedPrompt, {
        QStringLiteral("verschieb"),
        QStringLiteral("verschieben"),
        QStringLiteral("verschiebungsvektor"),
        QStringLiteral("vektor"),
        QStringLiteral("move"),
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
    if (mentionsMove && toolName == QStringLiteral("geometry.move")) {
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
            || toolName == QStringLiteral("geometry.move")
            || toolName == QStringLiteral("geometry.copy")
            || toolName == QStringLiteral("geometry.rotate")
            || toolName == QStringLiteral("geometry.scale")
            || toolName == QStringLiteral("rectangles.extrude")
            || toolName == QStringLiteral("bim.classify")
            || toolName == QStringLiteral("selection.set")
            || toolName == QStringLiteral("layers.ensureMany");
    }

    return toolName == QStringLiteral("geometry.create")
        || toolName == QStringLiteral("rectangles.extrude")
        || toolName == QStringLiteral("bim.classify")
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
                "delta", "verschiebung", "verschieben", "move", "extend", "verlaengern", "verlängern"});
    }
    if (field == "direction" || field == "axis") {
        return textMentionsAny(normalized, {
            "x-richtung", "x richtung", "+x", "-x", "in x", "nach x",
            "y-richtung", "y richtung", "+y", "-y", "in y", "nach y",
            "z-richtung", "z richtung", "+z", "-z", "in z", "nach z",
            "rechts", "links", "oben", "unten"});
    }
    if (field == "face" || field == "subentity" || field == "subent" || field == "surface") {
        return textMentionsAny(normalized, {"face", "flaeche", "fläche", "seite", "stirnseite"});
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
    if (field == "layer") {
        return normalized.contains("layer");
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
    field.remove(QRegularExpression(QStringLiteral("[^a-z0-9äöüß]")));
    if (field == QStringLiteral("height")
        || field == QStringLiteral("heightmm")
        || field == QStringLiteral("hoehe")
        || field == QStringLiteral("höhe")
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
        || field == QStringLiteral("länge")
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

    if (textMentionsAny(normalized, {"wie viel", "wieviel", "distanz", "abstand", "offset", "versatz", "verschieb", "verlaeng", "verläng", "mm"})
        && textProvidesMissingField(prompt, "distance", draft)) {
        fields << "distance";
    }
    if (textMentionsAny(normalized, {"hoehe", "höhe", "height", "hoch", "z"})
        && textProvidesMissingField(prompt, "heightMm", draft)) {
        fields << "heightMm";
    }
    if (textMentionsAny(normalized, {"richtung", "achse", "axis", "direction"})
        && textProvidesMissingField(prompt, "direction", draft)) {
        fields << "direction";
    }
    if (textMentionsAny(normalized, {"face", "flaeche", "fläche", "seite", "stirnseite"})
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
    return error.isEmpty() ? fallback : error;
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
        || normalized == QStringLiteral("bestätigen")
        || normalized == QStringLiteral("freigeben")
        || normalized == QStringLiteral("review bestaetigt")
        || normalized == QStringLiteral("review bestätigt");
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
        || normalized == QStringLiteral("workflow löschen")
        || normalized == QStringLiteral("workflow entfernen")
        || normalized == QStringLiteral("planungsthema loeschen")
        || normalized == QStringLiteral("planungsthema löschen")
        || normalized == QStringLiteral("planungsthema entfernen")
        || normalized == QStringLiteral("berechnung loeschen")
        || normalized == QStringLiteral("berechnung löschen")
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
        || normalized == QStringLiteral("workflow ausführen")
        || normalized == QStringLiteral("workflow starten")
        || normalized == QStringLiteral("workflow testen")
        || normalized == QStringLiteral("ausfuehren")
        || normalized == QStringLiteral("ausführen")
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
                    && text != QStringLiteral("–")) {
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

QString workflowCompactSummaryForSelector(const QJsonObject& workflow)
{
    QString summary = repairMojibakeText(workflow.value("compactSummary").toString()).trimmed();
    if (summary.isEmpty()) {
        summary = repairMojibakeText(workflow.value("description").toString()).trimmed();
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
    return summary.left(420);
}

QStringList workflowToolNamesForSelector(const QJsonObject& workflow, int maxCount = 12)
{
    QStringList tools;
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
                || part == QStringLiteral("für")
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
                {"steps", steps},
            });
        }
        capsule.insert(QStringLiteral("executionBatches"), batches);

        QJsonArray flatSteps;
        for (const QJsonValue& stepValue : workflow.value(QStringLiteral("steps")).toArray()) {
            flatSteps.append(workflowStepCapsule(stepValue.toObject(), true));
        }
        if (!flatSteps.isEmpty()) {
            capsule.insert(QStringLiteral("steps"), flatSteps);
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
            || term == QStringLiteral("alle")
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
            {"required", QJsonArray{"schema", "type", "message"}},
            {"schema", "barebone.agent.response.v2"},
            {"allowedTypes", QJsonArray{"message", "ask_user", "context_request", "action_proposal", "workflow_run_proposal", "plan"}},
        }},
        {"actionProposal", QJsonObject{
            {"required", QJsonArray{"type", "message", "proposal"}},
            {"proposalRequired", QJsonArray{"requiresConfirmation", "actions"}},
            {"actionShape", QJsonObject{
                {"required", QJsonArray{"tool", "params"}},
                {"toolSource", "tools[].name"},
                {"paramsSource", "tools[].inputSchema"},
            }},
            {"forbiddenTopLevelFields", QJsonArray{"tool", "params", "actions"}},
        }},
        {"workflowRunProposal", QJsonObject{
            {"required", QJsonArray{"type", "message", "baseWorkflowId", "workflowUsage", "slotValues", "stepPlan", "actions", "requiresConfirmation"}},
            {"workflowUsageDecision", QJsonArray{"direct", "adapted", "extended", "deviated", "no_fit"}},
            {"stepPlanAction", QJsonArray{"use", "override", "skip", "insert"}},
            {"actionShape", QJsonObject{
                {"required", QJsonArray{"tool", "params"}},
                {"toolSource", "tools[].name"},
                {"paramsSource", "tools[].inputSchema"},
            }},
            {"policy", "Prefer workflow_run_proposal when workflowCapsules contains a useful workflow. User-facing message must describe only the concrete proposal; workflowUsage.reason is internal."},
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

QString withoutAiControlTokens(QString content)
{
    content.replace(QRegularExpression(QStringLiteral(R"(<\|[^>]*\|>)")), QStringLiteral(" "));
    return content.trimmed();
}

QString finalAiMessageSegment(QString content)
{
    content = content.trimmed();
    const QString messageToken = QStringLiteral("<|message|>");
    const int messageIndex = content.lastIndexOf(messageToken);
    if (messageIndex >= 0) {
        return content.mid(messageIndex + messageToken.size()).trimmed();
    }

    const QString finalToken = QStringLiteral("<|channel|>final");
    const int finalIndex = content.lastIndexOf(finalToken);
    if (finalIndex >= 0) {
        return content.mid(finalIndex + finalToken.size()).trimmed();
    }

    return content;
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
    if (!text.contains(QStringLiteral("Ã"))
        && !text.contains(QStringLiteral("Â"))
        && !text.contains(QStringLiteral("â"))
        && !text.contains(QStringLiteral("Î"))
        && !text.contains(QStringLiteral("Ï"))) {
        return text;
    }

    const QList<QPair<QString, QString>> replacements = {
        {QStringLiteral("â€ž"), QStringLiteral("„")},
        {QStringLiteral("â€œ"), QStringLiteral("“")},
        {QStringLiteral("â€"), QStringLiteral("”")},
        {QStringLiteral("â€™"), QStringLiteral("’")},
        {QStringLiteral("â€˜"), QStringLiteral("‘")},
        {QStringLiteral("â€“"), QStringLiteral("–")},
        {QStringLiteral("â€”"), QStringLiteral("—")},
        {QStringLiteral("â€¦"), QStringLiteral("…")},
        {QStringLiteral("â€¯"), QStringLiteral(" ")},
        {QStringLiteral("â€‘"), QStringLiteral("-")},
        {QStringLiteral("â»"), QStringLiteral("⁻")},
        {QStringLiteral("âº"), QStringLiteral("⁺")},
        {QStringLiteral("â‚‚"), QStringLiteral("₂")},
        {QStringLiteral("â‚ƒ"), QStringLiteral("₃")},
        {QStringLiteral("â‰ˆ"), QStringLiteral("≈")},
        {QStringLiteral("â‰¤"), QStringLiteral("≤")},
        {QStringLiteral("â‰¥"), QStringLiteral("≥")},
        {QStringLiteral("â†’"), QStringLiteral("→")},
        {QStringLiteral("Ã¤"), QStringLiteral("ä")},
        {QStringLiteral("Ã¶"), QStringLiteral("ö")},
        {QStringLiteral("Ã¼"), QStringLiteral("ü")},
        {QStringLiteral("Ã„"), QStringLiteral("Ä")},
        {QStringLiteral("Ã–"), QStringLiteral("Ö")},
        {QStringLiteral("Ãœ"), QStringLiteral("Ü")},
        {QStringLiteral("ÃŸ"), QStringLiteral("ß")},
        {QStringLiteral("Ã©"), QStringLiteral("é")},
        {QStringLiteral("Ã¨"), QStringLiteral("è")},
        {QStringLiteral("Ã¡"), QStringLiteral("á")},
        {QStringLiteral("Ã³"), QStringLiteral("ó")},
        {QStringLiteral("Ã±"), QStringLiteral("ñ")},
        {QStringLiteral("Ã§"), QStringLiteral("ç")},
        {QStringLiteral("Ã—"), QStringLiteral("×")},
        {QStringLiteral("Â«"), QStringLiteral("«")},
        {QStringLiteral("Â»"), QStringLiteral("»")},
        {QStringLiteral("Â°"), QStringLiteral("°")},
        {QStringLiteral("Î”"), QStringLiteral("Δ")},
        {QStringLiteral("Î´"), QStringLiteral("δ")},
        {QStringLiteral("Îµ"), QStringLiteral("ε")},
        {QStringLiteral("Ï"), QStringLiteral("ρ")},
        {QStringLiteral("Ï"), QStringLiteral("φ")},
        {QStringLiteral("Ï€"), QStringLiteral("π")},
        {QStringLiteral("Â"), QStringLiteral("")},
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
    content.replace(QStringLiteral("<|channel|>final"), QString());
    content.replace(QStringLiteral("<|constrain|>json"), QString());
    content.replace(QStringLiteral("<|message|>"), QString());
    content = withoutAiControlTokens(content);
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
            && (nestedMessage.startsWith('{') || nestedMessage.contains(QStringLiteral("<|message|>")))) {
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
    text.replace(QStringLiteral("ä"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ö"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ü"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ß"), QStringLiteral("ss"));
    text.replace(QStringLiteral("²"), QStringLiteral("2"));
    text.replace(',', '.');
    return text;
}

QString workflowSlotNameFromValue(const QJsonValue& value)
{
    if (value.isObject()) {
        return value.toObject().value("name").toString().trimmed();
    }
    return value.toString().trimmed();
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

    const QStringList referencedSlots = workflowTemplateReferencedSlots(workflow);
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
        || normalized.contains(QStringLiteral("nutze dafür"))
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
            || combined.contains(QStringLiteral("separat ausführen"))
            || combined.contains(QStringLiteral("einzeln ausfuehren"))
            || combined.contains(QStringLiteral("einzeln ausführen")))) {
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
        QStringLiteral("löschen"),
        QStringLiteral("ueberspring"),
        QStringLiteral("überspring"),
        QStringLiteral("streichen"),
        QStringLiteral("weg lassen"),
        QStringLiteral("weglassen"),
        QStringLiteral("ueberfluessig"),
        QStringLiteral("überflüssig"),
        QStringLiteral("nicht benoetigt"),
        QStringLiteral("nicht benötigt"),
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
        || normalized.contains(QStringLiteral("im nächsten schritt"))
        || normalized.contains(QStringLiteral("naechster schritt"))
        || normalized.contains(QStringLiteral("nächster schritt"))
        || normalized.contains(QStringLiteral("zusaetzlich"))
        || normalized.contains(QStringLiteral("zusätzlich"))
        || normalized.contains(QStringLiteral("hinzufueg"))
        || normalized.contains(QStringLiteral("hinzufüg"))
        || normalized.contains(QStringLiteral("einfueg"))
        || normalized.contains(QStringLiteral("einfüg"))) {
        return true;
    }
    return (normalized.contains(QStringLiteral("erst "))
            || normalized.contains(QStringLiteral("zuerst"))
            || normalized.contains(QStringLiteral("vorher"))
            || normalized.contains(QStringLiteral("davor")))
        && (normalized.contains(QStringLiteral("danach"))
            || normalized.contains(QStringLiteral("anschliessend"))
            || normalized.contains(QStringLiteral("anschließend"))
            || normalized.contains(QStringLiteral("naechsten schritt"))
            || normalized.contains(QStringLiteral("nächsten schritt")));
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
        || tool == QStringLiteral("profile.extrude")) {
        return true;
    }
    if (tool == QStringLiteral("selection.set")) {
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
            || tool == QStringLiteral("rectangles.extrude")
            || tool == QStringLiteral("profile.extrude")
            || tool.startsWith(QStringLiteral("geometry."));
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
    const QString normalized = prompt.toLower();
    return promptDescribesArchitecturalWalls(normalized)
        && textMentionsAny(normalized, {
               QStringLiteral("4 wand"),
               QStringLiteral("4 waende"),
               QStringLiteral("4 wände"),
               QStringLiteral("vier wand"),
               QStringLiteral("vier waende"),
               QStringLiteral("vier wände"),
               QStringLiteral("rechteckiger raum"),
               QStringLiteral("raum bilden"),
           });
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
    const double roomLength = slots.value(QStringLiteral("roomLengthMm")).toDouble(0.0);
    const double roomWidth = slots.value(QStringLiteral("roomWidthMm")).toDouble(0.0);
    const double wallThickness = slots.value(QStringLiteral("wallThicknessMm")).toDouble(0.0);
    const double wallHeight = slots.value(QStringLiteral("wallHeightMm")).toDouble(0.0);
    if (roomLength <= 0.0 || roomWidth <= 0.0 || wallThickness <= 0.0 || wallHeight <= 0.0) {
        return proposal;
    }

    QString layerName = slots.value(QStringLiteral("layerName")).toString().trimmed();
    if (layerName.isEmpty()) {
        layerName = layerNameFromProposalActions(proposal);
    }

    const double outerLength = roomLength + (2.0 * wallThickness);
    QJsonArray actions;
    if (!layerName.isEmpty()) {
        actions.append(QJsonObject{
            {"tool", "layers.create"},
            {"params", QJsonObject{{"name", layerName}, {"saveBefore", true}}},
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

QJsonObject paramsWithRuntimeBatchHandles(const QString& tool, QJsonObject params, const QJsonArray& previousResults)
{
    if (tool != QStringLiteral("bim.classify")
        || !params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)) {
        return params;
    }

    const QJsonArray handles = createdGeometryHandlesFromBatchResults(previousResults);
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
    return action.value("tool").toString().trimmed() == QStringLiteral("selection.set")
        && action.value(paramsKey).toObject().value("selector").isObject();
}

bool workflowToolCanUsePreviousSelection(const QString& tool)
{
    return tool == QStringLiteral("geometry.move")
        || tool == QStringLiteral("geometry.copy")
        || tool == QStringLiteral("geometry.rotate")
        || tool == QStringLiteral("geometry.scale")
        || tool == QStringLiteral("geometry.delete")
        || tool == QStringLiteral("rectangles.extrude")
        || tool == QStringLiteral("profile.extrude")
        || tool == QStringLiteral("bim.classify");
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
            if (tool == QStringLiteral("bim.classify") && !hasTarget) {
                params.insert(QStringLiteral("target"), QStringLiteral("selection"));
                action.insert(paramsKey, params);
                if (changed) {
                    *changed = true;
                }
            } else if (tool != QStringLiteral("bim.classify")) {
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

BricsCadPage::BricsCadPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_aiNetwork(new QNetworkAccessManager(this))
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
    QObject::connect(m_agentBridge, &AiWebBridge::workflowTrainingRunConfirmed, this, [this]() {
        confirmWorkflowTrainingRun();
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
    QObject::connect(m_agentBridge, &AiWebBridge::workflowDeleteRequested, this, [this](const QString& workflowId) {
        QString deletedPath;
        QString errorMessage;
        if (!deleteWorkflowById(workflowId, &deletedPath, &errorMessage)) {
            appendAgentChat("Barebone-Qt", errorMessage.isEmpty()
                ? QStringLiteral("Workflow konnte nicht geloescht werden.")
                : errorMessage);
            return;
        }
        appendAgentChat("Barebone-Qt", QString("Workflow geloescht: %1").arg(deletedPath));
    });
    QObject::connect(m_agentBridge, &AiWebBridge::workflowSelectionCleared, this, [this]() {
        clearSelectedWorkflowForChat();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::messagePdfExportRequested, this, [this](const QString& messageId, const QString& suggestedTitle) {
        exportAgentMessageToPdf(messageId, suggestedTitle);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::messageWorkflowSaveRequested, this, [this](const QString& messageId, const QString& messageText, const QString& sessionTitle) {
        saveGeneralWorkflowFromMessage(messageId, messageText, sessionTitle);
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
        setLocalAiStatus(QStringLiteral("Lokale AI URL ungültig"), false);
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
            setLocalAiStatus(QStringLiteral("Lokale AI Antwort ungültig"), false);
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
    const QString& prompt,
    const QJsonObject& documentContext,
    int requestedOutputTokens) const
{
    const QString systemPrompt =
        QStringLiteral("Du bist ein allgemeiner AI Chat-Assistent in Barebone-Qt. %1 ")
            .arg(aiLanguageInstruction(m_config))
        + QStringLiteral(
            "Ohne freigegebene Tools sollst du keine Aktionen in BricsCAD behaupten. "
            "Wenn die Nutzeranfrage einen Dokumentkontext enthaelt, nutze diesen als primaere Quelle und verweise bei PDFs nach Moeglichkeit auf Seiten. "
            "Bei Berechnungen nenne immer zuerst die Grundgleichung, erklaere danach kurz alle in der Grundgleichung verwendeten Symbole, danach die Werte mit SI-Einheiten und erst dann das Ergebnis. "
            "Fuehre Einheiten in jeder eingesetzten Zahl und in jedem Zwischenschritt mit; schreibe Summen nicht als reine Zahlenkette mit Einheit nur am Ende. "
            "Wenn Werte nicht in SI-Einheiten vorliegen, zeige zuerst die Umrechnung in SI-Einheiten. "
            "Wenn der Nutzer nach Rechtschreibfehlern, Tippfehlern oder Korrektur fragt, liste gefundene Stellen mit Original und Korrektur; wenn du keine offensichtlichen Fehler findest, sage das ausdruecklich.");

    const int maxHistoryMessages = static_cast<int>(m_agentConversation.size());
    const int budget = inputBudgetTokens(requestedOutputTokens);
    auto build = [&](int historyMessages, const QJsonObject& context) {
        QJsonArray messages;
        messages.append(QJsonObject{{"role", "system"}, {"content", systemPrompt}});
        const qsizetype start = std::max<qsizetype>(0, m_agentConversation.size() - historyMessages);
        for (qsizetype i = start; i < m_agentConversation.size(); ++i) {
            messages.append(m_agentConversation.at(i).toObject());
        }
        messages.append(QJsonObject{{"role", "user"}, {"content", promptWithDocumentContext(prompt, context)}});
        return messages;
    };

    bool docMinimized = false;
    QJsonObject context = sanitizedDocumentContext(documentContext);
    for (int historyMessages = maxHistoryMessages; historyMessages >= 0; --historyMessages) {
        QJsonArray messages = build(historyMessages, context);
        const int estimated = estimateTokensForMessages(messages);
        if (estimated <= budget) {
            return {messages, {}, estimated, historyMessages, historyMessages < maxHistoryMessages || docMinimized};
        }
    }

    const int docBudget = std::max(300, budget / 2);
    context = documentContextWithTokenBudget(context, docBudget, &docMinimized);
    QJsonArray messages = build(0, context);
    int estimated = estimateTokensForMessages(messages);
    if (estimated > budget && !context.isEmpty()) {
        context = documentContextWithTokenBudget(context, std::max(180, budget / 4), &docMinimized);
        messages = build(0, context);
        estimated = estimateTokensForMessages(messages);
    }

    return {messages, {}, estimated, 0, true};
}

void BricsCadPage::emitContextBudget(int estimatedTokens, bool minimized, const QString& detail) const
{
    if (!m_agentBridge) {
        return;
    }

    const int maxTokens = effectiveContextWindowTokens();
    const int usedTokens = estimatedTokens >= 0
        ? estimatedTokens
        : estimateTokensForMessages(m_agentConversation);
    const int percent = maxTokens > 0
        ? std::clamp(static_cast<int>((static_cast<double>(usedTokens) / static_cast<double>(maxTokens)) * 100.0), 0, 100)
        : 0;

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
        {"detail", detail},
    }](AiWebBridge* target) {
        Q_EMIT target->contextBudgetChanged(payload);
    });
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
    if (isChatWorkspace()) {
        enabled = false;
    }
    if (m_trainingMode == enabled) {
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [enabled = m_trainingMode](AiWebBridge* target) {
                Q_EMIT target->trainingModeApplied(enabled);
            });
        }
        return;
    }

    m_trainingMode = enabled;
    if (isChatWorkspace()) {
        m_trainingMode = false;
        enabled = false;
    }
    if (m_trainingMode && !isChatWorkspace() && m_chatMode != QStringLiteral("bricscad")) {
        m_chatMode = QStringLiteral("bricscad");
        appendBridgeLog("AI Agent: Trainingsmodus nutzt BricsCAD Modus");
    } else if (isChatWorkspace()) {
        m_chatMode = QStringLiteral("general");
    }
    clearAgentProposal();
    m_pendingAgentDraft = {};
    m_pendingAgentProposal = {};
    m_agentValidationRetries = 0;
    m_trainingValidationRetries = 0;
    m_trainingRejectedResponseSignatures.clear();
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    m_trainingRunPending = false;
    m_trainingFinalSavePending = false;
    m_trainingPendingRunWorkflow = {};
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    clearWorkflowTrainingPrompts();
    if (m_trainingMode) {
        m_trainingConversation = {};
        if (isChatWorkspace() && !m_selectedWorkflow.isEmpty()) {
            m_trainingWorkflowContext = m_selectedWorkflow;
            m_trainingSlotValues = m_selectedWorkflow.value(QStringLiteral("knownSlotValues")).toObject();
        } else {
            m_selectedWorkflowId.clear();
            m_selectedWorkflow = {};
            m_selectedWorkflowSlotValues = {};
            m_trainingWorkflowContext = {};
            m_trainingSlotValues = {};
        }
        m_trainingMissing = {};
        m_trainingPhase = QStringLiteral("collecting");
    } else {
        m_trainingPhase = QStringLiteral("idle");
    }

    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [enabled = m_trainingMode](AiWebBridge* target) {
            Q_EMIT target->trainingModeApplied(enabled);
        });
        if (m_trainingMode && !isChatWorkspace()) {
            emitToWebAsync(m_agentBridge, [](AiWebBridge* target) {
                Q_EMIT target->selectedWorkflowChanged(QVariantMap{});
            });
        }
    }

    if (m_trainingMode) {
        appendBridgeLog(isChatWorkspace() ? "General Workflow Training: aktiviert" : "Workflow Training: aktiviert");
        if (!isChatWorkspace() && m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
            requestBridgeCapabilities();
        }
        if (!isChatWorkspace()) {
            appendAgentChat("Barebone-Qt", "Trainingsmodus aktiv. Beschreibe den neuen BricsCAD Workflow, den wir erstellen wollen.");
        }
    } else {
        appendBridgeLog(isChatWorkspace() ? "General Workflow Training: deaktiviert" : "Workflow Training: deaktiviert");
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
        appendBridgeLog("AI Agent: Prompt ignoriert, weil bereits eine Anfrage laeuft");
        appendAgentChat("Barebone-Qt", "Die AI verarbeitet noch eine Anfrage. Nutze Stoppen oder warte auf die Antwort.");
        return;
    }

    const QString prompt = promptText.trimmed();
    if (prompt.isEmpty()) {
        return;
    }

    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    appendAgentChat("Du", prompt);
    m_agentRejectedResponseSignatures.clear();

    if (isAgentConfirmation(prompt) && !m_pendingAgentProposal.isEmpty()) {
        executeAgentProposal();
        return;
    }

    if (handleWorkflowRunUserResponse(prompt)) {
        return;
    }

    if (!isChatWorkspace() && !m_trainingMode && handleSelectedWorkflowPrompt(prompt)) {
        return;
    }

    if (isChatWorkspace()) {
        if (isGeneralWorkflowDeleteText(prompt)) {
            if (m_selectedWorkflow.isEmpty()) {
                appendAgentChat("Barebone-Qt", "Es ist kein gespeicherter Workflow ausgewählt, der geloescht werden kann.");
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

    if (m_trainingMode) {
        m_agentValidationRetries = 0;
        m_trainingValidationRetries = 0;
        m_trainingRejectedResponseSignatures.clear();
        m_lastAgentUserPrompt = prompt;
        m_lastDocumentContext = {};
        clearAgentProposal();
        const QJsonObject detectedSlots = workflowTrainingSlotValuesFromPrompt(prompt, m_trainingMissing, m_trainingWorkflowContext);
        if (!detectedSlots.isEmpty()) {
            mergeWorkflowTrainingSlotValues(detectedSlots);
            appendBridgeLog(QString("Workflow Training Slots: %1")
                .arg(QString::fromUtf8(QJsonDocument(detectedSlots).toJson(QJsonDocument::Compact))));
        }
        if (m_trainingFinalSavePending && isWorkflowTrainingExplicitSaveText(prompt)) {
            confirmWorkflowTrainingFinalSave();
            return;
        }
        if (m_trainingFinalSavePending) {
            clearWorkflowTrainingPrompts();
            m_trainingFinalSavePending = false;
            m_trainingFinalSaveWorkflow = {};
            m_trainingFinalSaveActions = {};
            appendBridgeLog("Workflow Training Final Save: Nutzer bearbeitet weiter, finaler Speichern-Hinweis verworfen");
        }
        const bool asksToRunTrainingDraft = isWorkflowTrainingRunText(prompt) || isWorkflowTrainingExplicitSaveText(prompt);
        if (!m_trainingSaveReviewPending
            && asksToRunTrainingDraft
            && workflowHasExecutableTrainingContent(m_trainingWorkflowContext)) {
            m_trainingRunPending = true;
            m_trainingPendingRunWorkflow = m_trainingWorkflowContext;
            confirmWorkflowTrainingRun();
            return;
        }
        if (m_trainingRunPending && !asksToRunTrainingDraft) {
            clearWorkflowTrainingPrompts();
            m_trainingRunPending = false;
            m_trainingPendingRunWorkflow = {};
            appendBridgeLog("Workflow Training Run: Nutzer bearbeitet weiter, Ausfuehren-Hinweis verworfen");
        }
        if (!m_trainingSaveReviewPending && isWorkflowTrainingExplicitSaveText(prompt)) {
            if (!m_trainingWorkflowContext.isEmpty()) {
                appendBridgeLog("Workflow Training: Nutzer fordert Speichern; AI soll ausfuehrbaren Workflow-Entwurf erzeugen");
                sendWorkflowTrainingPrompt(QStringLiteral(
                    "Der Nutzer moechte den aktuellen Workflow ausfuehren und danach speichern. "
                    "Wandle den aktiven Workflow-Entwurf jetzt in einen type=workflow_draft mit ausfuehrbaren executionBatches[].steps und flachen steps um. "
                    "Ergaenze konkrete paramsTemplate anhand effectiveTools. "
                    "Wenn eine bestimmte Layer-Zeichnung gefordert ist, setze geometry.create.paramsTemplate.layer explizit."),
                    true);
                return;
            }
        }
        if (m_trainingSaveReviewPending) {
            if (isWorkflowTrainingReviewConfirmationText(prompt)) {
                confirmWorkflowTrainingSaveReview();
                return;
            }

            appendBridgeLog("Workflow Training Review: Nutzerantwort wird zur Ueberarbeitung an die AI gegeben");
            clearWorkflowTrainingPrompts();
            m_trainingSaveReviewPending = false;
            m_trainingReviewConfirmed = false;
            m_trainingPendingReviewContent.clear();
            m_trainingPendingReviewReply = {};
            m_trainingPendingReviewWorkflow = {};
            m_trainingPhase = QStringLiteral("reviewing");
            sendWorkflowTrainingPrompt(QString(
                "Der Nutzer hat auf die Vor-Speicher-Review geantwortet und moeglicherweise Korrekturen ergaenzt:\n%1\n\n"
                "Ueberarbeite den aktiven Workflow anhand dieser Antwort. Denke die Punkte erneut durch. "
                "Antworte mit type=workflow_draft und gehe die Zusammenfassung wieder punktweise durch: Titel, Beschreibung, Eingaben, Formeln, Batch-Ausfuehrungen, Validierungsbeispiel und offene Risiken. "
                "Stelle konkrete questions, falls noch etwas unklar ist. Antworte nicht mit workflow_update, bis der Nutzer die Review bestaetigt.")
                .arg(prompt),
                true);
            return;
        }
        sendWorkflowTrainingPrompt(prompt);
        return;
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
        routeUnifiedAgentPrompt(prompt, sanitizedContext);
        return;
    }

    const QJsonObject previousRoute = m_lastAgentRoute;
    const bool continueOpenCadQuestion = !m_pendingAgentDraft.isEmpty()
        && routeAllowsCadActions(previousRoute);
    m_agentValidationRetries = 0;
    m_lastAgentUserPrompt = prompt;
    m_lastDocumentContext = sanitizedContext;
    m_lastAgentRoute = continueOpenCadQuestion ? previousRoute : QJsonObject{};

    if (!m_pendingAgentProposal.isEmpty()) {
        if (m_agentBridge) {
            emitWebProposalCleared(m_agentBridge);
        }
        appendBridgeLog("AI Agent: offener Vorschlag wird als Kontext an die AI weitergegeben");
    }

    if (!m_pendingAgentDraft.isEmpty()) {
        appendBridgeLog("AI Agent: offener Plan/Draft wird als Kontext an die AI weitergegeben");
    }

    if (continueOpenCadQuestion) {
        appendBridgeLog("AI Agent: Nutzerantwort ergaenzt offene CAD-Rueckfrage; Router wird uebersprungen");
        continueUnifiedAgentRequest(prompt, sanitizedContext, previousRoute);
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
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    const QJsonObject fallbackRoute = normalizedAgentRouteForMode(
        makeAgentRoute(
            fallbackRouteNameForPrompt(prompt, documentContext),
            QStringLiteral("Lokale Router-Heuristik")),
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
        if (!officialProvider) {
            payload.insert("temperature", 0.0);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.0);
        payload.insert("max_tokens", 512);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    setAgentBusy(true);
    appendBridgeLog(QString("Qt -> AI Router: mode=%1 provider=%2 endpoint=%3 model=%4 timeoutMs=%5")
        .arg(mode, provider, url.toString(), model)
        .arg(kAiModelResponseTimeoutMs));

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, documentContext, fallbackRoute, mode, operationGeneration]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendBridgeLog(QString("AI Router: Fehler http=%1 %2, Fallback route=%3")
                .arg(httpStatus)
                .arg(reply->errorString())
                .arg(fallbackRoute.value("route").toString()));
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Router Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            reply->deleteLater();
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendBridgeLog(QString("AI Router: OpenAI JSON ungueltig (%1), Fallback route=%2")
                .arg(parseError.errorString(), fallbackRoute.value("route").toString()));
            appendBridgeLog(QString("AI Router Body: %1").arg(QString::fromUtf8(body).left(800)));
            reply->deleteLater();
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        if (content.isEmpty()) {
            appendBridgeLog(QString("AI Router: leere Antwort, Fallback route=%1")
                .arg(fallbackRoute.value("route").toString()));
            reply->deleteLater();
            continueUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
            return;
        }

        bool parsed = false;
        const QJsonObject parsedRoute = jsonObjectFromAiContent(content, &parsed);
        QJsonObject route = parsed
            ? normalizedAgentRouteForMode(parsedRoute, prompt, documentContext, mode, QStringLiteral("AI Router"))
            : fallbackRoute;
        if (mode == QStringLiteral("bricscad")
            && fallbackRoute.value("route").toString() == QStringLiteral("bricscad_action")
            && route.value("route").toString() != QStringLiteral("bricscad_action")) {
            route = normalizedAgentRouteForMode(
                makeAgentRoute(
                    QStringLiteral("bricscad_action"),
                    QString("Qt Override: lokale Heuristik erkennt CAD-Aktionsabsicht; AI Router war %1 (%2)")
                        .arg(route.value("route").toString(), route.value("reason").toString().left(160))),
                prompt,
                documentContext,
                mode);
        }
        appendBridgeLog(QString("AI Router: mode=%1 route=%2 profile=%3 reason=%4")
            .arg(mode,
                route.value("route").toString(),
                route.value("capabilityProfile").toString(),
                route.value("reason").toString().left(240)));
        reply->deleteLater();
        continueUnifiedAgentRequest(prompt, documentContext, route);
    });
}

void BricsCadPage::continueUnifiedAgentRequest(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, documentContext, m_chatMode);
    m_lastAgentRoute = normalizedRoute;

    if (!routeAllowsCadActions(normalizedRoute) && !m_pendingAgentDraft.isEmpty()) {
        appendBridgeLog("AI Agent: offener CAD-Draft verworfen, Route ist nicht ausfuehrend");
        m_pendingAgentDraft = {};
    }

    if (routeAllowsCadActions(normalizedRoute)) {
        m_queuedAgentRoute = normalizedRoute;
        if (!ensureBridgeCapabilitiesForPrompt(prompt)) {
            return;
        }
        if (normalizedRoute.value(QStringLiteral("selectedTools")).toArray().isEmpty()
            && !normalizedRoute.value(QStringLiteral("toolSelectionAttempted")).toBool(false)) {
            selectToolsForUnifiedAgentRequest(prompt, documentContext, normalizedRoute);
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

    if (routeAllowsCadContext(normalizedRoute) && shouldPrefetchSelectionDescription(prompt)) {
        sendUnifiedAgentRequestWithPrefetchedContext(
            prompt,
            "selection.describe",
            QJsonObject{
                {"include", QJsonArray{"geometry"}},
                {"limit", 100},
            },
            documentContext,
            normalizedRoute);
        return;
    }

    sendUnifiedAgentRequest(prompt, documentContext, normalizedRoute);
}

void BricsCadPage::selectToolsForUnifiedAgentRequest(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, documentContext, m_chatMode);
    const QJsonArray plainTools = compactToolSelectorList();
    const QJsonArray plainWorkflows = compactWorkflowSelectorList();
    if (plainTools.isEmpty() && plainWorkflows.isEmpty()) {
        QJsonObject fallbackRoute = normalizedRoute;
        fallbackRoute.insert(QStringLiteral("selectedTools"), QJsonArray{});
        fallbackRoute.insert(QStringLiteral("selectedWorkflows"), QJsonArray{});
        sendUnifiedAgentRequest(prompt, documentContext, fallbackRoute);
        return;
    }

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid() || url.scheme().isEmpty()) {
        appendBridgeLog("AI Tool Selector: ungueltige AI Server URL, nutze lokale Toolauswahl");
        sendUnifiedAgentRequest(prompt, documentContext, normalizedRoute);
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    QJsonObject selectorInput{
        {"schema", "barebone.agent.selection.input.v1"},
        {"userPrompt", prompt},
        {"route", normalizedRoute},
        {"plainTools", plainTools},
        {"plainWorkflows", plainWorkflows},
        {"policy", "Waehle zuerst relevante Workflows aus plainWorkflows[].id, wenn ein vorhandener Workflow die Nutzerabsicht ganz oder teilweise abdeckt. Waehle danach nur Toolnamen aus plainTools[].name. Antworte ohne Markdown mit genau einem JSON-Objekt. Maximal 8 Tools und maximal 3 Workflows. Nimm notwendige Abhaengigkeiten mit auf, z.B. layers.create wenn Objekte in einem neuen Layer erzeugt werden. Waehle rectangles.extrude/profile.extrude nur, wenn der Prompt ausdruecklich vorhandene Rechtecke/Profile extrudieren will. Waehle bim.classify bei ausdruecklicher BIM-/Klassifizierungsabsicht oder bei klaren architektonischen Wandaufgaben mit Wandstaerke/Wandhoehe/Raumbezug. Wenn neue 3D-Waende als Boxen erzeugt werden sollen, nutze geometry.create plus layers.create und bei Wandaufgaben zusaetzlich bim.classify."},
        {"responseShape", QJsonObject{
            {"schema", "barebone.agent.selection.v1"},
            {"tools", QJsonArray{"tool.name"}},
            {"workflows", QJsonArray{"workflow.id"}},
            {"needsWorkflow", false},
            {"needsCadContext", true},
            {"confidence", 0.0},
            {"reason", "kurze Begruendung"},
        }},
    };

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content",
            "Du bist der Barebone-Qt Tool- und Workflow-Selector. "
            "Du beantwortest nicht die Nutzerfrage und erzeugst keine CAD-Aktion. "
            "Waehle zuerst passende vorhandene Workflows aus der Plain-Workflowliste und danach relevante Toolnamen aus der Plain-Toolliste. "
            "Nutze Workflows nur, wenn Titel, Trigger, Kurzfassung oder Werkzeugschritte zur Nutzerabsicht passen. "
            "Waehle keine Extrude-Tools nur wegen Hoehenangaben, wenn geometry.create das Ziel direkt erzeugen kann. "
            "Waehle bim.classify bei ausdruecklicher BIM-/Klassifizierungsabsicht oder wenn der Prompt architektonische Waende mit Wandstaerke/Wandhoehe/Raumbezug erzeugt. "
            "Antworte ausschliesslich als JSON: {\"schema\":\"barebone.agent.selection.v1\",\"tools\":[\"...\"],\"workflows\":[\"...\"],\"needsWorkflow\":false,\"needsCadContext\":true,\"confidence\":0.0,\"reason\":\"...\"}."},
    });
    messages.append(QJsonObject{
        {"role", "user"},
        {"content", QString::fromUtf8(QJsonDocument(selectorInput).toJson(QJsonDocument::Compact))},
    });

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", 512);
        payload.insert("reasoning", QJsonObject{{"effort", "low"}});
        if (!officialProvider) {
            payload.insert("temperature", 0.0);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.0);
        payload.insert("max_tokens", 512);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    setAgentBusy(true);
    appendBridgeLog(QString("Qt -> AI Tool/Workflow Selector: tools=%1 workflows=%2 provider=%3 endpoint=%4 model=%5 timeoutMs=%6")
        .arg(plainTools.size())
        .arg(plainWorkflows.size())
        .arg(provider, url.toString(), model)
        .arg(kAiModelResponseTimeoutMs));

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, documentContext, normalizedRoute, operationGeneration]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        QJsonObject selectedRoute = normalizedRoute;
        QStringList selectedToolNames;
        QStringList selectedWorkflowIds;
        const QSet<QString> knownToolNames = [&]() {
            QSet<QString> names;
            for (const QJsonValue& value : availableAgentTools()) {
                const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
                if (!name.isEmpty()) {
                    names.insert(name);
                }
            }
            return names;
        }();
        const QHash<QString, QString> knownWorkflowIds = [&]() {
            QHash<QString, QString> ids;
            for (const QJsonValue& value : compactWorkflowSelectorList()) {
                const QJsonObject workflow = value.toObject();
                const QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
                if (id.isEmpty()) {
                    continue;
                }
                ids.insert(workflowSlug(id), id);
                const QString fileName = workflow.value(QStringLiteral("fileName")).toString();
                ids.insert(workflowSlug(fileName), id);
                ids.insert(workflowSlug(QFileInfo(fileName).baseName()), id);
                ids.insert(workflowSlug(workflow.value(QStringLiteral("title")).toString()), id);
            }
            return ids;
        }();

        if (reply->error() != QNetworkReply::NoError) {
            appendBridgeLog(QString("AI Tool/Workflow Selector: Fehler http=%1 %2, nutze lokale Auswahl")
                .arg(httpStatus)
                .arg(reply->errorString()));
        } else {
            QJsonParseError parseError;
            const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
                appendBridgeLog(QString("AI Tool/Workflow Selector: ungueltige OpenAI Antwort (%1), nutze lokale Auswahl")
                    .arg(parseError.errorString()));
            } else {
                QString reasoningText;
                const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
                bool parsed = false;
                const QJsonObject selectorReply = jsonObjectFromAiContent(content, &parsed);
                if (parsed) {
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("tools")).toArray()) {
                        const QString name = value.toString().trimmed();
                        if (knownToolNames.contains(name) && !selectedToolNames.contains(name) && selectedToolNames.size() < 8) {
                            selectedToolNames << name;
                        }
                    }
                    auto appendWorkflowCandidate = [&](const QJsonValue& value) {
                        QString candidate;
                        if (value.isString()) {
                            candidate = value.toString();
                        } else if (value.isObject()) {
                            const QJsonObject object = value.toObject();
                            candidate = object.value(QStringLiteral("id")).toString(
                                object.value(QStringLiteral("workflowId")).toString(
                                    object.value(QStringLiteral("title")).toString()));
                        }
                        const QString id = knownWorkflowIds.value(workflowSlug(candidate));
                        if (!id.isEmpty() && !selectedWorkflowIds.contains(id) && selectedWorkflowIds.size() < 3) {
                            selectedWorkflowIds << id;
                        }
                    };
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("workflows")).toArray()) {
                        appendWorkflowCandidate(value);
                    }
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("workflowIds")).toArray()) {
                        appendWorkflowCandidate(value);
                    }
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("selectedWorkflows")).toArray()) {
                        appendWorkflowCandidate(value);
                    }
                }
                for (const QString& workflowId : selectedWorkflowIds) {
                    QString workflowError;
                    const QJsonObject workflow = loadWorkflowById(workflowId, nullptr, &workflowError);
                    for (const QString& tool : workflowToolNamesForSelector(workflow, 12)) {
                        if (knownToolNames.contains(tool) && !selectedToolNames.contains(tool) && selectedToolNames.size() < 8) {
                            selectedToolNames << tool;
                        }
                    }
                }
                if (!selectedToolNames.isEmpty()) {
                    appendBridgeLog(QString("AI Tool/Workflow Selector: tools=%1 workflows=%2 reason=%3")
                        .arg(selectedToolNames.join(QStringLiteral(",")),
                            selectedWorkflowIds.join(QStringLiteral(",")),
                            selectorReply.value(QStringLiteral("reason")).toString().left(220)));
                } else if (!selectedWorkflowIds.isEmpty()) {
                    appendBridgeLog(QString("AI Tool/Workflow Selector: workflows=%1 reason=%2")
                        .arg(selectedWorkflowIds.join(QStringLiteral(",")),
                            selectorReply.value(QStringLiteral("reason")).toString().left(220)));
                } else {
                    appendBridgeLog("AI Tool/Workflow Selector: keine gueltige Auswahl, nutze lokale Auswahl");
                }
            }
        }

        if (selectedWorkflowIds.isEmpty()) {
            const QStringList localWorkflows = localWorkflowSelectionForPrompt(compactWorkflowSelectorList(), prompt, 3);
            for (const QString& workflowId : localWorkflows) {
                const QString id = knownWorkflowIds.value(workflowSlug(workflowId), workflowId);
                if (!id.isEmpty() && !selectedWorkflowIds.contains(id) && selectedWorkflowIds.size() < 3) {
                    selectedWorkflowIds << id;
                }
            }
            if (!selectedWorkflowIds.isEmpty()) {
                appendBridgeLog(QString("AI Tool/Workflow Selector: lokale Workflow-Auswahl %1")
                    .arg(selectedWorkflowIds.join(QStringLiteral(","))));
            }
        }

        if (!selectedWorkflowIds.isEmpty()) {
            for (const QString& workflowId : selectedWorkflowIds) {
                QString workflowError;
                const QJsonObject workflow = loadWorkflowById(workflowId, nullptr, &workflowError);
                for (const QString& tool : workflowToolNamesForSelector(workflow, 12)) {
                    if (knownToolNames.contains(tool) && !selectedToolNames.contains(tool) && selectedToolNames.size() < 8) {
                        selectedToolNames << tool;
                    }
                }
            }
        }

        if (!selectedToolNames.isEmpty()) {
            selectedRoute.insert(QStringLiteral("selectedTools"), stringsToJsonArray(selectedToolNames));
        }
        if (!selectedWorkflowIds.isEmpty()) {
            selectedRoute.insert(QStringLiteral("selectedWorkflows"), stringsToJsonArray(selectedWorkflowIds));
        }
        selectedRoute.insert(QStringLiteral("toolSelectionAttempted"), true);
        selectedRoute.insert(QStringLiteral("workflowSelectionAttempted"), true);
        setAgentBusy(false);
        continueUnifiedAgentRequest(prompt, documentContext, selectedRoute);
        reply->deleteLater();
    });
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

void BricsCadPage::sendUnifiedAgentRequestWithPrefetchedContext(
    const QString& prompt,
    const QString& method,
    const QJsonObject& params,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    if (!m_brxAuthenticated) {
        sendUnifiedAgentRequest(prompt, documentContext, route);
        return;
    }

    appendBridgeLog(QString("Qt -> BRX Prefetch: %1 %2")
        .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
    setAgentBusy(true);

    const bool queued = sendBridgeRequest(
        method,
        params,
        15000,
        [this, prompt, method, params, documentContext, route](const QJsonObject& response) {
            setAgentBusy(false);
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            QJsonObject envelope = agentRequestEnvelope(prompt, documentContext, route);
            envelope.insert("prefetchedContext", QJsonObject{
                {"request", QJsonObject{{"method", method}, {"params", params}}},
                {"response", response},
            });
            sendAgentEnvelope(envelope, prompt, true, "prompt+context");
        });

    if (!queued) {
        setAgentBusy(false);
        sendUnifiedAgentRequest(prompt, documentContext, route);
    }
}

void BricsCadPage::sendAgentEnvelope(const QJsonObject& envelope, const QString& userHistoryContent, bool storeUserMessage, const QString& logLabel)
{
    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) {
        appendAgentChat("Barebone-Qt", QString("Ungueltige AI Server URL: %1").arg(baseUrl));
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        appendAgentChat("Barebone-Qt", "Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key in den Einstellungen gespeichert.");
        return;
    }

    const QJsonObject requestRoute = envelope.value("route").toObject();
    const bool plainGeneralResponse = m_chatMode == QStringLiteral("general")
        && !routeAllowsCadActions(requestRoute);
    const QString corePrompt = agentResourceText(QStringLiteral(":/agent/policies/core.md"));
    const QString generalPlainPrompt = QStringLiteral(
        "Du bist der AI Assistent fuer Barebone-Qt. %1\n\n"
        "Der Nutzer befindet sich im Allgemeinen Modus. Der eingehende User-Content ist ein JSON-Envelope, aber deine Antwort soll eine normale direkte Chatantwort sein.\n"
        "Nutze aus dem Envelope vor allem userPrompt, documentContext, selectedWorkflow, selectedWorkflows, workflowCapsules, compactState und conversation. Ignoriere JSON-responseContract-Vorgaben, solange keine CAD-Aktion freigegeben ist.\n"
        "Wenn selectedWorkflow oder workflowCapsules einen allgemeinen Workflow enthalten, behandle dessen Tabellen, Formeln, Beispiele, Annahmen und contextSummary als primaeren Kontext fuer die Antwort.\n"
        "Bei Berechnungen gilt: erst die Grundgleichung, dann alle verwendeten Symbole kurz erklaeren, dann Werte mit SI-Einheiten einsetzen, dann Zwischenergebnisse und Endergebnis. Jede eingesetzte Zahl und jeder Summand muss seine Einheit tragen; keine reinen Zahlenketten mit Einheit nur am Ende. Wenn Eingaben nicht in SI-Einheiten vorliegen, zeige zuerst die SI-Umrechnung.\n"
        "Wenn du LaTeX schreibst, formatiere beschreibende Indizes nicht kursiv; nutze z. B. _{\\\\mathrm{...}} statt _{...}. Schreibe Grad Celsius KaTeX-kompatibel als {}^\\\\circ\\\\mathrm{C}, z. B. 20\\\\,{}^\\\\circ\\\\mathrm{C} statt 20\\\\,^\\\\circ\\\\mathrm{C}.\n"
        "Antworte nicht als JSON-Objekt und verwende keinen Barebone-Agent-Antworttyp. Markdown fuer Listen, Tabellen und Codebloecke ist erlaubt.\n"
        "Schlage keine CAD-Tools, keine BRX-Ausfuehrung und keine Aktionen vor. Wenn Live-BricsCAD-Daten noetig waeren, erklaere knapp, dass dafuer der BricsCAD-Modus/BRX-Kontext erforderlich ist.")
        .arg(aiLanguageInstruction(m_config));
    const QJsonObject systemMessage{
        {"role", "system"},
        {"content", plainGeneralResponse
            ? generalPlainPrompt
            : (corePrompt.isEmpty()
            ? QStringLiteral("Du bist der AI Assistent fuer Barebone-Qt. %1 Antworte ausschliesslich mit einem gueltigen JSON-Objekt gemaess responseContract.")
                .arg(aiLanguageInstruction(m_config))
            : corePrompt)},
    };
    const bool includeConversationHistory = envelope.value("includeConversationHistory").toBool(true);
    const int requestedOutputTokens = dynamicOutputTokenTarget(
        useResponsesApi ? 2048 : 1024,
        useResponsesApi ? 8192 : 8192,
        16);
    const int initialOutputTokens = adjustedOutputTokenLimit(requestedOutputTokens);
    const int budget = inputBudgetTokens(initialOutputTokens);
    const int totalHistoryMessages = includeConversationHistory
        ? static_cast<int>(m_agentConversation.size())
        : 0;

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
            builtMessages.append(item);
            recentConversation.append(item);
        }

        QJsonObject outboundEnvelope = sourceEnvelope;
        outboundEnvelope.insert("conversation", recentConversation);
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
        ContextBuildResult best = buildAgentMessages(totalHistoryMessages, sourceEnvelope, {});
        best.minimized = forceMinimized;
        if (best.estimatedTokens <= budget) {
            return best;
        }

        ContextBuildResult smallest = best;
        const int summaryCharBudget = std::clamp(budget, 1200, 16384);
        for (int recentHistoryMessages = totalHistoryMessages - 1; recentHistoryMessages >= 0; --recentHistoryMessages) {
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

    ContextBuildResult contextBuild = buildBestContext(envelope, false);

    if (contextBuild.estimatedTokens > budget) {
        QJsonObject minimizedEnvelope = envelope;
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

    const QJsonArray outboundMessages = contextBuild.messages;
    const int outputTokens = adjustedOutputTokenLimitForMessages(outboundMessages, requestedOutputTokens);
    QString contextDetail;
    if (contextBuild.minimized) {
        QStringList details;
        if (contextBuild.compressedHistoryMessages > 0) {
            details << QStringLiteral("%1 aeltere Nachrichten komprimiert").arg(contextBuild.compressedHistoryMessages);
        }
        if (contextBuild.historyMessages < totalHistoryMessages) {
            details << QStringLiteral("%1 juengste Nachrichten vollstaendig").arg(contextBuild.historyMessages);
        }
        contextDetail = details.isEmpty()
            ? QStringLiteral("Kontext automatisch minimiert")
            : QStringLiteral("Kontext automatisch minimiert: %1").arg(details.join(QStringLiteral(", ")));
    }
    emitContextBudget(
        contextBuild.estimatedTokens,
        contextBuild.minimized,
        contextDetail);

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", outboundMessages);
        payload.insert("max_output_tokens", outputTokens);
        const QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
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
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    setAgentBusy(true);
    const QString reasoningLog = useResponsesApi
        ? QString(" reasoning=%1").arg(normalizedReasoningEffort(m_reasoningEffort))
        : QString();
    const QJsonObject route = requestRoute;
    const QJsonObject modePolicy = envelope.value("modePolicy").toObject();
    const QStringList toolNames = toolNamesForLog(envelope.value("effectiveTools").toArray());
    const QStringList policyNames = jsonStringArrayToStringList(envelope.value("policyRefs").toArray());
    appendBridgeLog(QString("AI Envelope: mode=%1 route=%2 profile=%3 toolsSent=%4 policyRefs=%5")
        .arg(modePolicy.value("mode").toString(m_chatMode),
            route.value("route").toString("<leer>"),
            route.value("capabilityProfile").toString("<leer>"),
            toolNames.isEmpty() ? QStringLiteral("-") : toolNames.join(","),
            policyNames.isEmpty() ? QStringLiteral("-") : policyNames.join(",")));
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
        appendBridgeLog(QString("AI Kontext: automatisch minimiert used=%1/%2 tokens recentHistory=%3 compressedHistory=%4")
            .arg(contextBuild.estimatedTokens)
            .arg(effectiveContextWindowTokens())
            .arg(contextBuild.historyMessages)
            .arg(contextBuild.compressedHistoryMessages));
    }

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, userHistoryContent, storeUserMessage, operationGeneration, plainGeneralResponse]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendAgentChat("AI", QString("Fehler bei der AI Anfrage: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(reply->errorString()));
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("AI", QString("Antwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            appendBridgeLog(QString("AI Body: %1").arg(QString::fromUtf8(body).left(800)));
            reply->deleteLater();
            return;
        }

        QString reasoningText;
        const QJsonObject responseObject = responseDocument.object();
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseObject, &reasoningText));
        m_nextAgentMessageContinuationAvailable = plainGeneralResponse && aiResponseWasTruncated(responseObject);
        m_nextAgentMessageContinuationReason = m_nextAgentMessageContinuationAvailable
            ? aiResponseTruncationReason(responseObject)
            : QString();
        if (content.trimmed().isEmpty()) {
            m_nextAgentMessageContinuationAvailable = false;
            m_nextAgentMessageContinuationReason.clear();
            if (plainGeneralResponse) {
                appendBridgeLog(QString("AI Allgemeiner Modus: leere Antwort reasoningChars=%1").arg(reasoningText.size()));
                appendAgentChat("AI", "Leere Antwort erhalten.");
                reply->deleteLater();
                return;
            }
            if (retryAgentAfterValidationFailure(
                    reasoningText,
                    {},
                    "Die AI hat Reasoning geliefert, aber keine finale Barebone-JSON-Antwort. Antworte jetzt ausschliesslich mit genau einem gueltigen JSON-Objekt.")) {
                reply->deleteLater();
                return;
            }
            appendAgentChat("AI", "Leere Antwort erhalten.");
            reply->deleteLater();
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
        handleAgentReply(content);
        m_nextAgentMessageContinuationAvailable = false;
        m_nextAgentMessageContinuationReason.clear();
        reply->deleteLater();
    });
}

QString BricsCadPage::workflowsDirectoryPath() const
{
    QDir root(bareboneProjectRootPath());
    return root.filePath(QStringLiteral("agent/workflows"));
}

QString BricsCadPage::generalWorkflowsDirectoryPath() const
{
    QDir root(bareboneProjectRootPath());
    return root.filePath(QStringLiteral("agent/general-workflows"));
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
        workflows.append(QJsonObject{
            {"fileName", source.fileName},
            {"id", id},
            {"title", title},
            {"description", repairMojibakeText(workflow.value(QStringLiteral("description")).toString()).trimmed()},
            {"kind", "general"},
            {"readOnly", source.bundled},
            {"createdAt", workflowTimestampIso(source, workflow, QStringLiteral("createdAt"))},
            {"modifiedAt", workflowTimestampIso(source, workflow, QStringLiteral("modifiedAt"))},
            {"verificationStatus", repairMojibakeText(workflow.value(QStringLiteral("verificationStatus")).toString()).trimmed()},
        });
    }
    return workflows;
}

QJsonArray BricsCadPage::workflowTrainingIndex() const
{
    if (isChatWorkspace()) {
        return generalWorkflowIndex();
    }
    if (isChatWorkspace()) {
        return {};
    }

    QJsonArray workflows;
    QSet<QString> seen;

    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        workflowsDirectoryPath(),
        QStringLiteral(":/agent/workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        QString parseError;
        if (!readAgentWorkflowJson(source.path, &workflow, &parseError)) {
            workflows.append(QJsonObject{
                {"fileName", source.fileName},
                {"error", parseError},
                {"createdAt", workflowTimestampIso(source, workflow, QStringLiteral("createdAt"))},
                {"modifiedAt", workflowTimestampIso(source, workflow, QStringLiteral("modifiedAt"))},
            });
            continue;
        }

        const QString id = workflow.value("id").toString(QFileInfo(source.fileName).baseName());
        const QString slug = workflowSlug(id);
        if (seen.contains(slug)) {
            continue;
        }
        seen.insert(slug);

        const QByteArray compact = QJsonDocument(workflow).toJson(QJsonDocument::Compact);
        if (compact.size() > 12000) {
            workflow = workflowCapsuleForAgent(workflow, true);
            workflow.insert(QStringLiteral("note"), QStringLiteral("Workflow wurde für die Overlay-Vorschau komprimiert; die Datei ist größer als 12k."));
        }

        workflows.append(QJsonObject{
            {"fileName", source.fileName},
            {"id", workflow.value("id").toString(QFileInfo(source.fileName).baseName())},
            {"title", workflow.value("title").toString(QFileInfo(source.fileName).baseName())},
            {"readOnly", source.bundled},
            {"createdAt", workflowTimestampIso(source, workflow, QStringLiteral("createdAt"))},
            {"modifiedAt", workflowTimestampIso(source, workflow, QStringLiteral("modifiedAt"))},
            {"workflow", workflow},
        });
    }
    return workflows;
}

QJsonArray BricsCadPage::compactWorkflowSelectorList() const
{
    QJsonArray compactWorkflows;
    const QJsonArray workflows = workflowTrainingIndex();
    for (const QJsonValue& value : workflows) {
        const QJsonObject indexed = value.toObject();
        const QJsonObject workflow = indexed.value("workflow").toObject();
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
    if (isChatWorkspace()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Im Chat-Arbeitsbereich sind keine Workflows aktiv.");
        }
        return {};
    }

    const QString normalizedId = workflowSlug(workflowId);
    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        workflowsDirectoryPath(),
        QStringLiteral(":/agent/workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject parsed;
        if (!readAgentWorkflowJson(source.path, &parsed)) {
            continue;
        }
        QJsonObject workflow = normalizedGeneralWorkflowDraft(parsed);
        const QString id = workflow.value("id").toString(QFileInfo(source.fileName).baseName());
        if (workflowSlug(id) == normalizedId || workflowSlug(QFileInfo(source.fileName).baseName()) == normalizedId) {
            if (fileName) {
                *fileName = source.fileName;
            }
            workflow.insert(QStringLiteral("fileName"), source.fileName);
            workflow.insert(QStringLiteral("readOnly"), source.bundled);
            return workflow;
        }
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("Workflow \"%1\" wurde nicht gefunden.").arg(workflowId);
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

    const QString normalizedId = workflowSlug(workflowId);
    if (normalizedId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow-ID fehlt.");
        }
        return false;
    }

    const QVector<AgentWorkflowFile> files = agentWorkflowFiles(QStringList{
        workflowsDirectoryPath(),
        QStringLiteral(":/agent/workflows"),
    });
    for (const AgentWorkflowFile& source : files) {
        QJsonObject workflow;
        if (!readAgentWorkflowJson(source.path, &workflow)) {
            continue;
        }
        const QString id = workflow.value(QStringLiteral("id")).toString(QFileInfo(source.fileName).baseName());
        if (workflowSlug(id) != normalizedId && workflowSlug(QFileInfo(source.fileName).baseName()) != normalizedId) {
            continue;
        }
        if (source.bundled) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Mitgelieferte Workflows aus dem App-Bundle koennen nicht geloescht werden.");
            }
            return false;
        }

        QFile file(source.path);
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
        if (workflowSlug(m_selectedWorkflowId) == normalizedId
            || workflowSlug(m_selectedWorkflow.value(QStringLiteral("id")).toString()) == normalizedId) {
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

    if (errorMessage) {
        *errorMessage = QStringLiteral("Workflow wurde nicht gefunden.");
    }
    return false;
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
        {"preferredTools", m_selectedWorkflow.value("preferredTools").toArray()},
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

    for (const QString& id : routeWorkflowIds(route, 3)) {
        QString errorMessage;
        QJsonObject workflow = loadWorkflowById(id, nullptr, &errorMessage);
        if (!workflow.isEmpty()) {
            appendWorkflow(workflow);
        }
    }

    if (!m_selectedWorkflow.isEmpty()) {
        appendWorkflow(m_selectedWorkflow);
    }
    return workflows;
}

void BricsCadPage::selectWorkflowForChat(const QString& workflowId)
{
    QString fileName;
    QString errorMessage;
    QJsonObject workflow = isChatWorkspace()
        ? loadGeneralWorkflowById(workflowId, &fileName, &errorMessage)
        : loadWorkflowById(workflowId, &fileName, &errorMessage);
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

QJsonObject BricsCadPage::workflowSlotValuesForPrompt(const QJsonObject& workflow, const QString& prompt)
{
    QJsonObject values = workflow.value("knownSlotValues").toObject();
    for (auto it = m_selectedWorkflowSlotValues.begin(); it != m_selectedWorkflowSlotValues.end(); ++it) {
        values.insert(it.key(), it.value());
    }

    const QJsonObject detected = workflowTrainingSlotValuesFromPrompt(
        prompt,
        workflow.value("requiredSlots").toArray(),
        workflow);
    for (auto it = detected.begin(); it != detected.end(); ++it) {
        values.insert(it.key(), it.value());
    }

    const QString normalized = repairMojibakeText(prompt).toLower();
    if (workflow.value("optionalSlots").toObject().contains(QStringLiteral("classifyAsBimWall"))) {
        if (normalized.contains(QStringLiteral("ohne bim"))
            || normalized.contains(QStringLiteral("nicht klassifiz"))
            || normalized.contains(QStringLiteral("ohne klassifiz"))) {
            values.insert(QStringLiteral("classifyAsBimWall"), false);
        } else if (normalized.contains(QStringLiteral("bim"))
            || normalized.contains(QStringLiteral("klassifiz"))) {
            values.insert(QStringLiteral("classifyAsBimWall"), true);
        }
    }

    m_selectedWorkflowSlotValues = values;
    return values;
}

QStringList BricsCadPage::missingWorkflowSlots(const QJsonObject& workflow, const QJsonObject& slotValues) const
{
    QStringList missing;
    const QStringList executionSlots = workflowExecutionReferencedSlots(workflow);
    auto hasSlotValue = [&](const QString& name, const QString& canonical) {
        if (slotValues.contains(name) || slotValues.contains(canonical)) {
            return true;
        }
        for (auto it = slotValues.constBegin(); it != slotValues.constEnd(); ++it) {
            if (canonicalWorkflowSlot(it.key()) == canonical) {
                return true;
            }
        }
        return false;
    };

    const QJsonArray required = workflow.value("requiredSlots").toArray();
    for (const QJsonValue& value : required) {
        const QString name = workflowSlotNameFromValue(value);
        if (name.isEmpty()) {
            continue;
        }
        const QString canonical = canonicalWorkflowSlot(name);
        if (!executionSlots.isEmpty() && !executionSlots.contains(canonical)) {
            continue;
        }
        if (!hasSlotValue(name, canonical)) {
            missing << name;
        }
    }
    missing.removeDuplicates();
    return missing;
}

QJsonArray BricsCadPage::workflowRunActions(const QJsonObject& workflow, const QJsonObject& slotValues, QString& errorMessage) const
{
    bool normalizedSelectors = false;
    const QJsonObject normalizedWorkflow = normalizedWorkflowRuntimeSelectors(workflow, &normalizedSelectors);
    Q_UNUSED(normalizedSelectors);

    QJsonArray steps;
    const QJsonArray batches = normalizedWorkflow.value("executionBatches").toArray();
    if (!batches.isEmpty()) {
        for (int batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
            const QJsonValue& batchValue = batches.at(batchIndex);
            const QJsonObject batch = batchValue.toObject();
            if (!workflowConditionAllowsStep(batch.value("condition"), slotValues)) {
                continue;
            }
            const QJsonArray batchSteps = batch.value("steps").toArray();
            for (int stepIndex = 0; stepIndex < batchSteps.size(); ++stepIndex) {
                QJsonObject step = batchSteps.at(stepIndex).toObject();
                step.insert(QStringLiteral("_workflowSource"), QJsonObject{
                    {"kind", "executionBatch"},
                    {"batchIndex", batchIndex},
                    {"stepIndex", stepIndex},
                    {"batchId", batch.value("id").toString()},
                    {"stepId", step.value("id").toString()},
                });
                steps.append(step);
            }
        }
    } else {
        const QJsonArray flatSteps = normalizedWorkflow.value("steps").toArray();
        for (int stepIndex = 0; stepIndex < flatSteps.size(); ++stepIndex) {
            QJsonObject step = flatSteps.at(stepIndex).toObject();
            step.insert(QStringLiteral("_workflowSource"), QJsonObject{
                {"kind", "steps"},
                {"stepIndex", stepIndex},
                {"stepId", step.value("id").toString()},
            });
            steps.append(step);
        }
    }

    QJsonArray actions;
    for (int i = 0; i < steps.size(); ++i) {
        const QJsonObject step = steps.at(i).toObject();
        if (!workflowConditionAllowsStep(step.value("condition"), slotValues)) {
            continue;
        }
        const QString tool = step.value("tool").toString().trimmed();
        if (tool.isEmpty()) {
            errorMessage = QStringLiteral("Workflow-Schritt %1 hat kein tool.").arg(i + 1);
            return {};
        }
        const QJsonValue paramsValue = workflowTemplateValue(step.value("paramsTemplate"), slotValues);
        if (!paramsValue.isObject()) {
            errorMessage = QStringLiteral("Workflow-Schritt %1 erzeugt keine JSON-Parameter.").arg(i + 1);
            return {};
        }
        QJsonObject action{
            {"tool", tool},
            {"params", paramsValue.toObject()},
            {"workflowStepId", step.value("id").toString()},
            {"workflowStepTitle", step.value("title").toString(tool)},
            {"workflowStepSource", step.value("_workflowSource").toObject()},
            {"workflowStep", step},
            {"_validationContext", workflowValidationContextForStep(normalizedWorkflow, step)},
        };
        QString actionError;
        if (!validateAgentAction(action, actionError)) {
            errorMessage = QStringLiteral("Workflow-Schritt %1 ist nicht gueltig: %2")
                .arg(i + 1)
                .arg(actionError);
            return {};
        }
        actions.append(action);
    }

    if (actions.isEmpty()) {
        errorMessage = QStringLiteral("Workflow enthaelt keine ausfuehrbaren Schritte fuer die aktuellen Slotwerte.");
    }
    return actions;
}

bool BricsCadPage::prepareSelectedWorkflowRun(const QString& prompt)
{
    if (m_selectedWorkflow.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Kein Workflow ausgewaehlt. Oeffne das Workflow-Overlay und waehle zuerst einen Workflow aus.");
        return true;
    }
    if (!m_brxAuthenticated) {
        appendAgentChat("Barebone-Qt", "Workflow-Ausfuehrung braucht eine aktive BRX-Verbindung.");
        return true;
    }
    if (m_brxCapabilities.isEmpty()) {
        m_queuedAgentPrompt = prompt;
        m_queuedAgentRoute = makeAgentRoute(QStringLiteral("bricscad_action"), QStringLiteral("Workflow-Ausfuehrung"));
        m_queuedAgentRoute.insert(QStringLiteral("workflowRun"), true);
        appendBridgeLog("Workflow Ausfuehrung wartet auf BRX Capabilities");
        setAgentBusy(true);
        requestBridgeCapabilities();
        return true;
    }

    QString workflowFileName;
    QString workflowLoadError;
    QJsonObject runWorkflow = loadWorkflowById(m_selectedWorkflowId, &workflowFileName, &workflowLoadError);
    if (runWorkflow.isEmpty()) {
        runWorkflow = m_selectedWorkflow;
    } else {
        m_selectedWorkflow = runWorkflow;
        m_selectedWorkflowId = runWorkflow.value(QStringLiteral("id")).toString(QFileInfo(workflowFileName).baseName());
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [workflowMap = m_selectedWorkflow.toVariantMap()](AiWebBridge* target) {
                Q_EMIT target->selectedWorkflowChanged(workflowMap);
            });
        }
    }

    const QJsonObject slotValues = workflowSlotValuesForPrompt(runWorkflow, prompt);
    return prepareWorkflowRunFromWorkflow(runWorkflow, slotValues, prompt, QStringLiteral("saved_workflow"));
}

bool BricsCadPage::prepareTrainingDraftWorkflowRun(const QString& prompt)
{
    if (!m_trainingMode || m_trainingWorkflowContext.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Kein Workflow-Entwurf im Trainingsmodus vorhanden.");
        return true;
    }
    if (!m_brxAuthenticated) {
        appendAgentChat("Barebone-Qt", "Workflow-Ausfuehrung braucht eine aktive BRX-Verbindung.");
        return true;
    }
    if (m_brxCapabilities.isEmpty()) {
        m_trainingRunPending = true;
        m_trainingPendingRunWorkflow = m_trainingWorkflowContext;
        m_queuedAgentPrompt = prompt;
        m_queuedAgentRoute = makeAgentRoute(QStringLiteral("bricscad_action"), QStringLiteral("Workflow-Entwurf ausfuehren"));
        m_queuedAgentRoute.insert(QStringLiteral("workflowRun"), true);
        m_queuedAgentRoute.insert(QStringLiteral("workflowRunKind"), QStringLiteral("training_context"));
        appendBridgeLog("Workflow Training Run wartet auf BRX Capabilities");
        setAgentBusy(true);
        requestBridgeCapabilities();
        return true;
    }

    QJsonObject runWorkflow = m_trainingWorkflowContext;
    QJsonObject slotValues = runWorkflow.value(QStringLiteral("knownSlotValues")).toObject();
    for (auto it = m_trainingSlotValues.begin(); it != m_trainingSlotValues.end(); ++it) {
        slotValues.insert(it.key(), it.value());
    }
    const QJsonObject detected = workflowTrainingSlotValuesFromPrompt(
        prompt,
        runWorkflow.value(QStringLiteral("requiredSlots")).toArray(),
        runWorkflow);
    for (auto it = detected.begin(); it != detected.end(); ++it) {
        slotValues.insert(it.key(), it.value());
    }
    m_trainingSlotValues = slotValues;
    runWorkflow.insert(QStringLiteral("knownSlotValues"), slotValues);
    m_trainingWorkflowContext = runWorkflow;
    const QString runKind = workflowMatchesSelectedWorkflow(runWorkflow, m_selectedWorkflow, m_selectedWorkflowId)
        ? QStringLiteral("saved_workflow")
        : QStringLiteral("training_draft");
    return prepareWorkflowRunFromWorkflow(runWorkflow, slotValues, prompt, runKind);
}

bool BricsCadPage::prepareWorkflowRunFromWorkflow(
    const QJsonObject& workflow,
    const QJsonObject& slotValues,
    const QString& prompt,
    const QString& runKind)
{
    Q_UNUSED(prompt);
    const QString workflowId = workflow.value("id").toString(m_selectedWorkflowId);
    const QString workflowTitle = workflow.value("title").toString(workflowId.isEmpty() ? QStringLiteral("Workflow") : workflowId);
    const QStringList missing = missingWorkflowSlots(workflow, slotValues);
    if (!missing.isEmpty()) {
        appendAgentChat("AI", QString("Fuer den Workflow \"%1\" fehlen noch Angaben:\n- %2")
            .arg(workflowTitle, missing.join(QStringLiteral("\n- "))));
        return true;
    }

    QString errorMessage;
    const QJsonArray actions = workflowRunActions(workflow, slotValues, errorMessage);
    if (actions.isEmpty()) {
        appendAgentChat("Barebone-Qt", QString("Workflow kann nicht vorbereitet werden: %1").arg(errorMessage));
        return true;
    }

    QJsonObject proposal{
        {"schema", "barebone.agent.workflow.run.v1"},
        {"source", "workflow_run"},
        {"runKind", runKind},
        {"workflowId", workflowId},
        {"workflowTitle", workflowTitle},
        {"message", QStringLiteral("Ich fuehre den Workflow \"%1\" Schritt fuer Schritt aus.").arg(workflowTitle)},
        {"summary", QStringLiteral("Workflow \"%1\" mit %2 Schritten vorbereiten.").arg(workflowTitle).arg(actions.size())},
        {"requiresConfirmation", true},
        {"continueAfterSuccess", false},
        {"actions", actions},
        {"slotValues", slotValues},
        {"workflow", workflow},
    };

    appendBridgeLog(QString("Workflow Run: Vorschlag kind=%1 id=%2 mit %3 Aktionen")
        .arg(runKind, workflowId)
        .arg(actions.size()));
    setAgentBusy(false);
    setAgentProposal(proposal);
    return true;
}

bool BricsCadPage::handleSelectedWorkflowPrompt(const QString& prompt)
{
    if (m_selectedWorkflow.isEmpty()) {
        return false;
    }
    if (!m_trainingMode && !isBricsCadMode()) {
        return false;
    }

    const QString normalized = repairMojibakeText(prompt).toLower();
    const bool asksToRun = normalized.contains(QStringLiteral("ausfuehr"))
        || normalized.contains(QStringLiteral("ausführ"))
        || normalized.contains(QStringLiteral("start"))
        || normalized.contains(QStringLiteral("testen"))
        || normalized.contains(QStringLiteral("test"));
    const bool asksToEdit = normalized.contains(QStringLiteral("bearbeit"))
        || normalized.contains(QStringLiteral("erweiter"))
        || normalized.contains(QStringLiteral("korrigier"));
    const bool asksToClear = normalized.contains(QStringLiteral("abwaehl"))
        || normalized.contains(QStringLiteral("abwähl"))
        || normalized.contains(QStringLiteral("deaktivier"));
    const bool mentionsWorkflow = normalized.contains(QStringLiteral("workflow"))
        || normalized.contains(QStringLiteral("arbeitsablauf"))
        || normalized.contains(m_selectedWorkflow.value("title").toString().toLower());
    if (!mentionsWorkflow && !asksToRun && !asksToEdit && !asksToClear) {
        return false;
    }

    if (asksToClear) {
        clearSelectedWorkflowForChat();
        appendAgentChat("Barebone-Qt", "Workflow-Auswahl aufgehoben.");
        return true;
    }

    if (asksToEdit) {
        setTrainingMode(true);
        m_trainingWorkflowContext = m_selectedWorkflow;
        m_trainingSlotValues = m_selectedWorkflow.value("knownSlotValues").toObject();
        m_trainingMissing = {};
        appendBridgeLog(QString("Workflow Training: ausgewaehlten Workflow geladen: %1")
            .arg(m_selectedWorkflow.value("id").toString(m_selectedWorkflowId)));
        sendWorkflowTrainingPrompt(QStringLiteral("Bearbeite den aktiven Workflow \"%1\" anhand dieser Nutzeranweisung:\n%2")
            .arg(m_selectedWorkflow.value("title").toString(m_selectedWorkflowId), prompt));
        return true;
    }

    if (asksToRun) {
        return prepareSelectedWorkflowRun(prompt);
    }

    return false;
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

        if (it.key() == QStringLiteral("optionalSlots")
            && it.value().isObject()
            && merged.value(it.key()).isObject()) {
            QJsonObject optionalSlots = merged.value(it.key()).toObject();
            const QJsonObject incoming = it.value().toObject();
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
    const QStringList executionSlots = workflowTemplateReferencedSlots(m_trainingWorkflowContext);
    for (const QJsonValue& value : missing) {
        const QString name = workflowSlotNameFromValue(value);
        const QString canonical = canonicalWorkflowSlot(name);
        if (!executionSlots.isEmpty() && !executionSlots.contains(canonical)) {
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
                 QStringLiteral("profile.extrude"),
                 QStringLiteral("bim.classify"),
                 QStringLiteral("selection.set"),
                 QStringLiteral("command.execute"),
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
            "Hilf dem Nutzer, versionierte agent/workflows/*.json Dateien zu erstellen. "
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
            "Setze requiredSlots nur fuer Werte, die in paramsTemplate, condition oder derivedValues tatsaechlich per {{slot}} verwendet werden. Wenn vorhandene Objekte nur selektiert werden und feste Werte genannt sind, darf requiredSlots leer sein. "
            "Wenn der Nutzer 'relativ', 'Verschiebungsvektor', 'Vektor' oder x/y/z-Werte fuer eine Verschiebung nennt, ist damit geometry.move.params.vector gemeint; frage dann nicht nach absoluter Zielkoordinate. "
            "Wenn vorhandene Objekte selektiert und bearbeitet werden, frage nicht nach Erzeugungsdaten wie rectangleWidthMm, rectangleHeightMm oder extrudeHeightMm; diese sind nur fuer neue Geometrie noetig. "
            "Wenn der Nutzer sagt, dass eine Angabe nicht gebraucht wird, pruefe aktiv den aktuellen Workflow und entferne unreferenzierte requiredSlots/missing Felder statt dieselbe Rueckfrage zu wiederholen. "
            "Nutze fuer ausfuehrbare Werkzeugschritte workflow.steps[].tool und workflow.steps[].paramsTemplate exakt nach effectiveTools[].inputSchema/apiPost. "
            "Wenn der Nutzer verlangt, Geometrie in einem bestimmten Layer zu zeichnen, muss der geometry.create Schritt den Parameter paramsTemplate.layer exakt mit diesem Layernamen enthalten; layers.create allein setzt den Ziellayer fuer folgende Geometrie nicht automatisch. "
            "Wenn ein Schritt selection.set verwendet und der naechste Schritt auf dieser Auswahl arbeiten soll, muss der naechste Schritt trotzdem explizit paramsTemplate.selector={\"scope\":\"selection\"} oder target=\"selection\" enthalten. "
            "Batch-Ausfuehrungen modellierst du in workflow.executionBatches mit mode=sequential, stopOnFailure=true und steps[].tool/paramsTemplate. "
            "Spiegele dieselben ausfuehrbaren Schritte zusaetzlich in der flachen workflow.steps Liste, sobald du workflow_update erzeugst. "
            "Schreibe constructionStrategy als JSON-Array mit einem kurzen String pro Strategiepunkt; keine eingebetteten '\\n', keine nummerierte Liste in einem einzelnen String. "
            "Formeln speicherst du als workflow.derivedValues mit name, expression, dependsOn, unit und example; Qt fuehrt keine Formeln aus, daher muessen validationExamples konkrete Beispielwerte enthalten. "
            "Wenn ein Schritt unmittelbar zuvor mit geometry.create ein Rechteck erzeugt, verwende im naechsten rectangles.extrude selector={\"scope\":\"lastResult\",\"kind\":\"rectangle\"}, nicht scope=selection. "
            "Bei normalen mehrschrittigen Vorschlaegen kann ein direkt nachfolgendes bim.classify target=lastExtruded nutzen. "
            "Bei gespeicherten Workflows, die Schritt fuer Schritt mit Nutzerfeedback ausgefuehrt werden, bevorzuge nach dem Extrudieren einen expliziten Selector oder selection.set und danach bim.classify target=selection, weil jeder Schritt einzeln per BRX validiert wird. "
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
            {"toolPolicy", "preferredTools und steps[].tool duerfen nur bekannte toolNames/effectiveTools[].name verwenden. paramsTemplate darf nur Parameter aus inputSchema.properties/apiPost.bodySchema verwenden. Beispiele muessen BRX actions.validate bestehen."},
            {"editingPolicy", "Bei Bearbeitung immer den bestehenden Workflow erhalten und nur gezielt erweitern/veraendern."},
            {"slotPolicy", "requiredSlots ist eine Liste aus strings oder Objekten mit name/type/description und darf leer sein. Nimm nur Slots auf, die wirklich in paramsTemplate, condition oder derivedValues referenziert werden; keine unbenutzten Breiten/Hoehen fuer bereits vorhandene Geometrie. optionalSlots ist immer ein Objekt nach slotName, kein Array."},
            {"draftPolicy", "Bei langen Erstprompts erst workflow_draft mit workflowDraft erzeugen. workflow_update nur nach Nutzerbestaetigung oder wenn der Nutzer explizit speichern/aktualisieren will."},
            {"calculationPolicy", "Formeln gehoeren in derivedValues als name/expression/dependsOn/unit/example. Schreibe konkrete Beispielwerte in knownSlotValues und validationExamples; Qt wertet expression nicht aus."},
            {"batchPolicy", "Komplexe Ablaufe gehoeren in executionBatches[].steps mit mode=sequential und stopOnFailure=true. Fuer workflow_update muss zusaetzlich workflow.steps die gleiche ausfuehrbare Sequenz flach enthalten."},
            {"validationPolicy", "workflow_update muss validationExamples[].actions mit konkreten, platzhalterfreien Beispielaktionen enthalten. Verkettete Beispiele duerfen lastResult nutzen, aber nicht eine leere selection voraussetzen. Fuer step-by-step Workflows nach einer Extrusion zuerst selection.set verwenden und danach bim.classify target=selection."},
        }},
        {"knownToolNames", toolNames},
        {"effectiveTools", effectiveTools},
        {"existingWorkflows", existingWorkflows},
        {"trainingState", workflowTrainingState()},
        {"activeWorkflow", activeWorkflow},
        {"selectedWorkflow", selectedWorkflowSummary()},
        {"workflowsDirectory", workflowsDirectoryPath()},
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

    for (auto it = paramsTemplate.begin(); it != paramsTemplate.end(); ++it) {
        if (!properties.contains(it.key())) {
            errorMessage = QStringLiteral("%1.paramsTemplate.%2 ist kein Parameter von %3").arg(prefix, it.key(), tool);
            return false;
        }
        if (!jsonContainsTemplatePlaceholder(it.value())) {
            QString schemaError;
            if (!validateSchemaValue(it.value(), properties.value(it.key()).toObject(), prefix + ".paramsTemplate." + it.key(), schemaError)) {
                errorMessage = schemaError;
                return false;
            }
        }
    }

    const QJsonArray required = inputSchema.value("required").toArray();
    for (const QJsonValue& value : required) {
        const QString key = value.toString();
        if (!key.isEmpty() && !paramsTemplate.contains(key)) {
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
            for (int stepIndex = 0; stepIndex < batchSteps.size(); ++stepIndex) {
                const QJsonObject step = batchSteps.at(stepIndex).toObject();
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
            }
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

    for (int i = 0; i < steps.size(); ++i) {
        const QJsonObject step = steps.at(i).toObject();
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
    }

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
    if (workflow.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("workflow Objekt fehlt");
        }
        return false;
    }

    QJsonObject normalized = workflow;
    if (normalized.value("schema").toString().trimmed().isEmpty()) {
        normalized.insert("schema", QStringLiteral("barebone.agent.workflow.v1"));
    }

    QString id = workflowSlug(normalized.value("id").toString());
    if (id == QStringLiteral("workflow")) {
        id = workflowSlug(normalized.value("title").toString());
    }
    if (id == QStringLiteral("workflow")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("workflow.id oder workflow.title fehlt");
        }
        return false;
    }
    normalized.insert("id", id);

    if (normalized.value("title").toString().trimmed().isEmpty()) {
        normalized.insert("title", id);
    }
    if (!normalized.value("triggerExamples").isArray()) {
        normalized.insert("triggerExamples", QJsonArray{});
    }
    if (!normalized.value("requiredSlots").isArray()) {
        normalized.insert("requiredSlots", QJsonArray{});
    }
    if (!normalized.value("optionalSlots").isObject()) {
        normalized.insert("optionalSlots", QJsonObject{});
    }
    if (!normalized.value("derivedValues").isArray()) {
        normalized.insert("derivedValues", QJsonArray{});
    }
    if (!normalized.value("preferredTools").isArray()) {
        normalized.insert("preferredTools", QJsonArray{});
    }
    if (!normalized.value("constructionStrategy").isArray()) {
        normalized.insert("constructionStrategy", QJsonArray{});
    }
    if (!normalized.value("forbidden").isArray()) {
        normalized.insert("forbidden", QJsonArray{});
    }
    normalized = workflowWithPrunedRequiredSlots(normalized);

    QDir dir(workflowsDirectoryPath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow-Ordner konnte nicht angelegt werden: %1").arg(dir.absolutePath());
        }
        return false;
    }

    const QString path = dir.filePath(id + QStringLiteral(".json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow-Datei konnte nicht geschrieben werden: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(normalized).toJson(QJsonDocument::Indented));
    file.close();

    if (file.error() != QFile::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workflow-Datei konnte nicht abgeschlossen werden: %1").arg(file.errorString());
        }
        return false;
    }

    if (savedPath) {
        *savedPath = QFileInfo(path).absoluteFilePath();
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
    m_trainingRunPending = false;
    m_trainingFinalSavePending = false;
    m_trainingPendingRunWorkflow = {};
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
        "Wenn selectedWorkflow leer ist, muss title ein kurzer, passender Anzeigename fuer das konkrete Thema sein und id muss daraus als stabiler snake_case Dateiname abgeleitet werden. "
        "title und id duerfen nicht Workflow, Neuer Workflow, Chat Workflow, General Workflow, Workflow speichern oder aehnlich generisch heissen. "
        "Wenn selectedWorkflow vorhanden ist, behalte dessen id exakt bei und aendere title nur bei ausdruecklichem Umbenennungswunsch des Nutzers. "
        "Keine BricsCAD-Workflow-Felder wie steps, executionBatches, validationExamples oder tools. "
        "Wenn LaTeX vorkommt, schreibe beschreibende Indizes nicht kursiv, z. B. _{\\\\mathrm{...}} statt _{...}. Schreibe Grad Celsius KaTeX-kompatibel als {}^\\\\circ\\\\mathrm{C}, z. B. 20\\\\,{}^\\\\circ\\\\mathrm{C} statt 20\\\\,^\\\\circ\\\\mathrm{C}. "
        "Korrigiere bei Formatierungsfehlern nur die Formatierung; erhalte alle fachlichen Details, Tabellenwerte, Beispiele und Nutzerkorrekturen vollstaendig. "
        "Tabellen muessen als tables[] mit columns/rows oder als gueltige Markdown-Pipe-Tabelle mit |---|---|-Trennzeile ausgegeben werden. "
        "Listen muessen echte Markdown-Listen mit je einem Punkt pro Zeile sein. "
        "Nutze keine HTML-Tags wie <br>, keine tabulatorgetrennten Klartexttabellen und kein loses Sprachlabel 'text' vor Formeln.")
        .arg(errorMessage, rejectedSample, repeatedInstruction);

    saveGeneralWorkflowFromTraining(
        saveContext.value(QStringLiteral("userInstruction")).toString(),
        m_generalWorkflowSaveRetries,
        retryInstruction,
        rejectedSample);
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

void BricsCadPage::confirmWorkflowTrainingRun()
{
    if (!m_trainingMode || !m_trainingRunPending || m_trainingPendingRunWorkflow.isEmpty()) {
        clearWorkflowTrainingPrompts();
        appendBridgeLog("Workflow Training Run: Ausfuehren ignoriert, weil kein ausfuehrbarer Entwurf bereitsteht");
        return;
    }

    clearWorkflowTrainingPrompts();
    clearAgentProposal();
    m_trainingRunPending = false;
    prepareTrainingDraftWorkflowRun(QStringLiteral("Workflow ausfuehren"));
    if (m_pendingAgentProposal.value(QStringLiteral("source")).toString() == QStringLiteral("workflow_run")
        && m_pendingAgentProposal.value(QStringLiteral("runKind")).toString() == QStringLiteral("training_draft")) {
        executeAgentProposal();
    }
}

void BricsCadPage::confirmWorkflowTrainingFinalSave()
{
    if ((!m_trainingMode && !isChatWorkspace()) || !m_trainingFinalSavePending || m_trainingFinalSaveWorkflow.isEmpty()) {
        clearWorkflowTrainingPrompts();
        appendBridgeLog("Workflow Training Final Save: Speichern ignoriert, weil kein getesteter Entwurf bereitsteht");
        return;
    }

    if (isChatWorkspace()) {
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
        appendBridgeLog(QString("General Workflow Training Final Save: gespeichert: %1").arg(savedPath));
        appendAgentChat("Barebone-Qt", QString("Workflow gespeichert: %1").arg(m_selectedWorkflow.value(QStringLiteral("title")).toString(m_selectedWorkflowId)));
        return;
    }

    clearWorkflowTrainingPrompts();
    QJsonObject workflow = workflowWithRunValidationExample(m_trainingFinalSaveWorkflow, m_trainingFinalSaveActions);
    if (!m_trainingSlotValues.isEmpty()) {
        workflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }

    QString savedPath;
    QString errorMessage;
    if (!saveWorkflowFromTraining(workflow, &savedPath, &errorMessage)) {
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht gespeichert werden: %1").arg(errorMessage));
        emitWorkflowTrainingFinalSavePrompt();
        return;
    }

    m_trainingFinalSavePending = false;
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    m_trainingWorkflowContext = workflow;
    m_trainingSlotValues = workflow.value(QStringLiteral("knownSlotValues")).toObject(m_trainingSlotValues);
    m_trainingPhase = QStringLiteral("saved");
    m_selectedWorkflowId = workflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId);
    m_selectedWorkflow = workflow;
    m_selectedWorkflowSlotValues = m_trainingSlotValues;
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [workflowMap = m_selectedWorkflow.toVariantMap()](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(workflowMap);
        });
    }
    emitWorkflowListToWeb();
    appendBridgeLog(QString("Workflow Training Final Save: gespeichert: %1").arg(savedPath));
    appendAgentChat("AI", QString("Workflow wurde gespeichert.\nGespeichert: %1").arg(savedPath));
    clearWorkflowRunState();
}

void BricsCadPage::emitWorkflowTrainingRunPrompt()
{
    if (!m_agentBridge || !m_trainingRunPending) {
        return;
    }

    const QJsonObject workflow = m_trainingPendingRunWorkflow.isEmpty()
        ? m_trainingWorkflowContext
        : m_trainingPendingRunWorkflow;
    const QString title = repairMojibakeText(workflow.value("title").toString()).trimmed();
    const QJsonObject signatureObject{
        {"kind", QStringLiteral("training_run")},
        {"id", workflowSlug(workflow.value("id").toString())},
        {"title", title},
        {"steps", workflowDisplaySteps(workflow)},
    };
    const QString signature = QString::fromUtf8(QJsonDocument(signatureObject).toJson(QJsonDocument::Compact));
    if (m_trainingPromptSignature == signature) {
        return;
    }
    m_trainingPromptSignature = signature;

    QVariantMap payload;
    payload.insert(QStringLiteral("title"), QStringLiteral("Workflow testen"));
    payload.insert(QStringLiteral("message"), title.isEmpty()
        ? QStringLiteral("Wenn die Zusammenfassung passt, kann der Entwurf Schritt fuer Schritt in BricsCAD ausgefuehrt und validiert werden. Weitere Eingaben bearbeiten den Entwurf weiter.")
        : QStringLiteral("Wenn die Zusammenfassung fuer \"%1\" passt, kann der Entwurf Schritt fuer Schritt in BricsCAD ausgefuehrt und validiert werden. Weitere Eingaben bearbeiten den Entwurf weiter.").arg(title));
    payload.insert(QStringLiteral("buttonText"), QStringLiteral("Ausführen"));
    payload.insert(QStringLiteral("action"), QStringLiteral("run"));
    payload.insert(QStringLiteral("workflowTitle"), title);
    emitToWebAsync(m_agentBridge, [payload](AiWebBridge* target) {
        Q_EMIT target->workflowTrainingRunPromptChanged(payload);
    });
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
            ? QStringLiteral("Mit Speichern wird ein neuer Workflow angelegt. Wenn der Titel anders lauten soll, antworte vor dem Speichern mit dem gewünschten Titel.")
            : QStringLiteral("Titelvorschlag: \"%1\". Mit Speichern wird immer ein neuer Workflow angelegt. Wenn der Titel anders lauten soll, antworte vor dem Speichern mit dem gewünschten Titel.").arg(title))
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
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
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
        {"content",
            "Du bist der Barebone-Qt Workflow-Autorenagent im Trainingsmodus. "
            "Du erstellst neue agent/workflows JSON-Dateien. "
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
            "Wenn Geometrie in einem bestimmten Layer entstehen soll, setze in geometry.create.paramsTemplate.layer den exakten Layernamen; ein vorheriges layers.create reicht nicht aus, um den Zeichenlayer implizit umzuschalten. "
            "Speichere keine starren Prompt-zu-Command-Regeln, sondern Slots, Defaults, Strategien, Constraints, Beispiele und bevorzugte Tools. "
            "Falls compactContext=true ist, konzentriere dich auf eine kurze gueltige JSON-Antwort und vermeide lange Erklaerungen."},
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
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", outputTokens);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kWorkflowTrainingAiTimeoutMs);

    setAgentBusy(true);
    appendBridgeLog(QString("Qt -> AI Workflow Training: provider=%1 endpoint=%2 model=%3 compact=%4 timeoutMs=%5 maxTokens=%6 context=%7")
        .arg(provider,
            url.toString(),
            model,
            compactContext ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(kWorkflowTrainingAiTimeoutMs)
        .arg(outputTokens)
        .arg(effectiveContextWindowTokens()));

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, compactContext, operationGeneration]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
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
                reply->deleteLater();
                sendWorkflowTrainingPrompt(prompt, true);
                return;
            }
            appendAgentChat("AI", QString("Fehler bei der Workflow-Training-Anfrage: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("AI", QString("Workflow-Training-Antwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            appendBridgeLog(QString("AI Workflow Body: %1").arg(QString::fromUtf8(body).left(800)));
            reply->deleteLater();
            return;
        }

        const QJsonObject responseObject = responseDocument.object();
        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseObject, &reasoningText));
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
                reply->deleteLater();
                return;
            }

            if (!compactContext) {
                sendWorkflowTrainingPrompt(
                    QStringLiteral("Deine letzte Workflow-Training-Antwort wurde vom Modell wegen max_tokens abgeschnitten. Antworte jetzt kurz und ausschliesslich mit barebone.workflow.training.response.v1 JSON. Wenn es ein Entwurf ist, nutze type=workflow_draft mit kurzer workflowDraft-Zusammenfassung. Wenn noch Daten fehlen, nutze type=ask_user mit message, missing und questions, aber ohne workflow.steps und ohne validationExamples. Wenn nur neue Angaben erkannt wurden, nutze slot_update. Wenn genug Daten vorhanden sind oder der Nutzer speichern will, nutze workflow_update kompakt."),
                    true);
                reply->deleteLater();
                return;
            }

            appendAgentChat("AI", "Die Workflow-Training-Antwort wurde vom Modell abgeschnitten. Bitte sende die Trainingsanweisung noch einmal knapper oder reduziere die geforderten Workflow-Details.");
            reply->deleteLater();
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
                reply->deleteLater();
                return;
            }
            appendAgentChat("AI", "Leere Antwort im Trainingsmodus erhalten. Bitte sende die Trainingsanweisung noch einmal knapper oder stelle Reasoning auf niedrig.");
            reply->deleteLater();
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
        reply->deleteLater();
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

    QJsonObject saveContext{
        {QStringLiteral("schema"), QStringLiteral("barebone.general.workflow.save.message.v1")},
        {QStringLiteral("messageId"), messageId.trimmed()},
        {QStringLiteral("sessionTitle"), repairMojibakeText(sessionTitle).trimmed()},
        {QStringLiteral("selectedWorkflow"), m_selectedWorkflow},
        {QStringLiteral("conversation"), QJsonArray{
            QJsonObject{
                {QStringLiteral("speaker"), QStringLiteral("AI")},
                {QStringLiteral("message"), cleanText},
            },
        }},
        {QStringLiteral("userInstruction"), QStringLiteral("Speichere ausschliesslich diese AI-Nachricht als allgemeinen Workflow.")},
    };

    QJsonObject workflow = generalWorkflowFromAiText(cleanText, saveContext);
    const QString preferredTitle = titleCandidateFromSaveContext(saveContext);
    if (!preferredTitle.isEmpty()) {
        workflow.insert(QStringLiteral("title"), preferredTitle);
        workflow.insert(QStringLiteral("id"), workflowSlug(preferredTitle));
        workflow = normalizedGeneralWorkflowDraft(workflow);
    }
    const QString validationError = generalWorkflowDraftValidationError(workflow);
    if (!validationError.isEmpty()) {
        appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(validationError));
        return;
    }
    const QString formattingError = generalWorkflowFormattingValidationError(workflow);
    if (!formattingError.isEmpty()) {
        appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(formattingError));
        return;
    }

    m_generalWorkflowSaveRetries = 0;
    m_generalWorkflowSaveRejectedSignatures.clear();
    m_lastGeneralWorkflowSaveContext = saveContext;
    m_trainingFinalSavePending = true;
    m_trainingFinalSaveWorkflow = workflow;
    m_trainingFinalSaveActions = {};
    m_trainingWorkflowContext = workflow;
    m_trainingPhase = QStringLiteral("general_ready_to_save");
    clearWorkflowTrainingPrompts();
    appendAgentChat("AI", generalWorkflowDraftMessageForChat(workflow));
    emitWorkflowTrainingFinalSavePrompt();
}

void BricsCadPage::saveGeneralWorkflowFromTraining(
    const QString& userInstruction,
    int retryCount,
    const QString& validationError,
    const QString& rejectedContent)
{
    if (!isChatWorkspace()) {
        appendAgentChat("Barebone-Qt", "Allgemeine Workflows koennen nur im Chat gespeichert werden.");
        return;
    }

    if (m_agentConversation.isEmpty() && m_selectedWorkflow.isEmpty() && m_trainingFinalSaveWorkflow.isEmpty()) {
        appendAgentChat("Barebone-Qt", "Es gibt noch keinen Kontext, aus dem ein Workflow erstellt werden kann.");
        return;
    }

    if (retryCount == 0 && validationError.trimmed().isEmpty()) {
        m_generalWorkflowSaveRetries = 0;
        m_generalWorkflowSaveRejectedSignatures.clear();
        m_lastGeneralWorkflowSaveContext = {};
    }

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
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

    QJsonObject saveContext{
        {"schema", "barebone.general.workflow.save.request.v1"},
        {"selectedWorkflow", m_selectedWorkflow},
        {"pendingWorkflow", m_trainingFinalSaveWorkflow},
        {"userInstruction", repairMojibakeText(userInstruction).trimmed()},
        {"conversation", compactConversation},
        {"conversationCompression", QJsonObject{
            {"mode", QStringLiteral("full")},
            {"messagesIncluded", compactConversation.size()},
            {"messagesTotal", m_agentConversation.size()},
        }},
        {"namingPolicy", QJsonObject{
            {"newWorkflowTitle", QStringLiteral("Wenn selectedWorkflow leer ist, erzeuge einen kurzen, treffenden Titel direkt aus dem konkreten Thema des Inhalts. Dieser Titel wird in der Sidebar angezeigt.")},
            {"idPolicy", QStringLiteral("Wenn selectedWorkflow leer ist, leite id als stabilen snake_case Dateinamen aus title ab. Keine generischen IDs wie workflow oder neuer_workflow.")},
            {"overwritePolicy", QStringLiteral("Wenn selectedWorkflow vorhanden ist, behalte dessen id bei und aktualisiere den title nur, wenn der Nutzer eine Umbenennung verlangt.")},
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
            {"content",
                "Du erstellst aus dem gesamten Chatkontext einen detaillierten allgemeinen Barebone-Qt Workflow. "
                "Antworte ausschliesslich mit genau einem JSON-Objekt. Keine Markdown-Antwort, kein Codeblock, keine Erklaerung ausserhalb von JSON. "
                "Das JSON muss schema='barebone.general.workflow.save.response.v1' verwenden. "
                "Pflichtfelder: schema, id, title, description, blocks. blocks ist ein Array aus Objekten mit id, title, text. "
                "Optionale Felder: tables, inputs, formulas, examples, assumptions, warnings, sourceRefs, verificationStatus, tags. "
                "Erstelle keine knappe Kurzfassung. Kombiniere detailreich alle fachlichen Informationen aus conversation, selectedWorkflow, pendingWorkflow und userInstruction. "
                "Bewahre Tabellen, Formeln, Beispiele, Rechenschritte, Annahmen, Hinweise, Nutzerkorrekturen und offene Unsicherheiten. "
                "Unterteile nur grob nach Absätzen; fachliche Tiefe gehoert in blocks[].text, nicht in eine komplexe JSON-Verschachtelung. "
                "Wenn LaTeX vorkommt, schreibe beschreibende Indizes nicht kursiv, z. B. _{\\\\mathrm{...}} statt _{...}. Schreibe Grad Celsius KaTeX-kompatibel als {}^\\\\circ\\\\mathrm{C}, z. B. 20\\\\,{}^\\\\circ\\\\mathrm{C} statt 20\\\\,^\\\\circ\\\\mathrm{C}. "
                "Bei Berechnungen muss nach jeder Grundgleichung eine kurze Symbolerklaerung der verwendeten Groessen folgen. "
                "Rechenschritte muessen Einheiten an jeder eingesetzten Zahl und jedem Summanden fuehren; keine reine Zahlenkette mit Einheit nur am Ende. "
                "Wenn Eingabewerte nicht in SI-Einheiten vorliegen, zeige zuerst die Umrechnung in SI-Einheiten. "
                "Tabellen muessen entweder als tables[] mit columns und rows oder als gueltige Markdown-Pipe-Tabelle mit Header, |---|---|-Trennzeile und Datenzeilen ausgegeben werden; keine tabulatorgetrennten Tabellen. "
                "Aufzaehlungen muessen echte Markdown-Listen sein, je ein Punkt pro Zeile mit '- ' oder '1. '; keine zusammengeklebten Listen in einer Zeile. "
                "Nutze keine HTML-Tags wie <br>; verwende echte Zeilenumbrueche. "
                "Bei Korrektur-Retries wegen Formatierung darfst du fachliche Inhalte nicht kuerzen oder neu zusammenfassen; korrigiere nur die betroffenen Formatierungen und erhalte alle Details. "
                "Wenn selectedWorkflow leer ist, musst du selbst einen kurzen, passenden title fuer das konkrete Thema erstellen; dieser title wird in der Sidebar angezeigt. "
                "Leite id daraus als stabilen snake_case Dateinamen ab. title und id duerfen nicht Workflow, Neuer Workflow, Chat Workflow, General Workflow oder aehnlich generisch heissen. "
                "Wenn selectedWorkflow vorhanden ist, behalte dessen id exakt bei und aendere title nur bei ausdruecklichem Umbenennungswunsch des Nutzers. "
                "Nutze keine BricsCAD-Workflow-Felder wie steps, executionBatches, validationExamples oder tools. "
                "Wenn pendingWorkflow vorhanden ist, ueberarbeite diesen anhand userInstruction. "
                "Wenn selectedWorkflow vorhanden ist, ueberarbeite und ueberschreibe ihn statt eine neue ID zu erfinden. "
                "Wenn Werte AI-Entwurf sind, setze verificationStatus='AI-Entwurf'."},
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
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", outputTokens);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    setAgentBusy(true);
    clearWorkflowTrainingPrompts();
    appendBridgeLog(QString("Qt -> AI General Workflow Prepare Save: retry=%1 provider=%2 endpoint=%3 model=%4")
        .arg(retryCount)
        .arg(provider, url.toString(), model));

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, operationGeneration, saveContext]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI General Workflow Prepare Save Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
            appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht vorbereitet werden: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendAgentChat("Barebone-Qt", QString("Speicherantwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString()));
            reply->deleteLater();
            return;
        }

        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        bool parsed = false;
        QJsonObject workflow = jsonObjectFromAiContent(content, &parsed);
        if (!parsed) {
            appendBridgeLog(QString("AI General Workflow Prepare Save Content: %1").arg(content.left(1000)));
            if (retryGeneralWorkflowSaveAfterValidationFailure(
                    saveContext,
                    content,
                    QStringLiteral("Die AI hat kein gueltiges Workflow-JSON geliefert. Antworte ausschliesslich mit genau einem JSON-Objekt nach barebone.general.workflow.save.response.v1."))) {
                reply->deleteLater();
                return;
            }
            appendAgentChat("Barebone-Qt", "Workflow konnte nicht vorbereitet werden: Die AI hat kein gueltiges Workflow-JSON geliefert.");
            reply->deleteLater();
            return;
        }

        workflow = generalWorkflowFromSaveResponse(workflow);
        const QString preferredTitle = titleCandidateFromSaveContext(saveContext);
        if (!preferredTitle.isEmpty()) {
            workflow.insert(QStringLiteral("title"), preferredTitle);
            workflow.insert(QStringLiteral("id"), workflowSlug(preferredTitle));
            workflow = normalizedGeneralWorkflowDraft(workflow);
        }
        const QString validationError = generalWorkflowDraftValidationError(workflow);
        if (!validationError.isEmpty()) {
            const QString rejectedJson = QString::fromUtf8(QJsonDocument(workflow).toJson(QJsonDocument::Compact));
            if (retryGeneralWorkflowSaveAfterValidationFailure(saveContext, rejectedJson, validationError)) {
                reply->deleteLater();
                return;
            }
            appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(validationError));
            reply->deleteLater();
            return;
        }

        const QString formattingError = generalWorkflowFormattingValidationError(workflow);
        if (!formattingError.isEmpty()) {
            const QString rejectedJson = QString::fromUtf8(QJsonDocument(workflow).toJson(QJsonDocument::Compact));
            if (retryGeneralWorkflowSaveAfterValidationFailure(saveContext, rejectedJson, formattingError)) {
                reply->deleteLater();
                return;
            }
            appendAgentChat("Barebone-Qt", QString("Workflow ist noch nicht speicherbereit: %1").arg(formattingError));
            reply->deleteLater();
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
        reply->deleteLater();
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
        const bool executableDraft = workflowHasExecutableTrainingContent(m_trainingWorkflowContext);
        if (executableDraft) {
            if (!m_trainingRunPending) {
                appendBridgeLog("Workflow Training Run: ausfuehrbarer Entwurf wartet auf Nutzerbestaetigung");
            }
            m_trainingRunPending = true;
            m_trainingPendingRunWorkflow = m_trainingWorkflowContext;
            m_trainingSaveReviewPending = false;
            m_trainingReviewConfirmed = false;
            m_trainingPendingReviewWorkflow = {};
            m_trainingPendingReviewReply = {};
            m_trainingPendingReviewContent.clear();
        }
        appendAgentChat("AI", workflowDraftMessageForChat(reply, displayDraft, m_trainingMissing));
        if (m_trainingRunPending && workflowHasExecutableTrainingContent(m_trainingPendingRunWorkflow)) {
            emitWorkflowTrainingRunPrompt();
        }
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
            if (workflowHasExecutableTrainingContent(m_trainingWorkflowContext)) {
                m_trainingRunPending = true;
                m_trainingPendingRunWorkflow = m_trainingWorkflowContext;
                m_trainingSaveReviewPending = false;
                m_trainingReviewConfirmed = false;
                m_trainingPendingReviewContent.clear();
                m_trainingPendingReviewReply = {};
                m_trainingPendingReviewWorkflow = {};
                appendAgentChat("AI", workflowDraftMessageForChat(reply, m_trainingWorkflowContext, m_trainingMissing));
                emitWorkflowTrainingRunPrompt();
                return;
            }
            appendAgentChat("Barebone-Qt", "Workflow-Entwurf enthaelt noch keine ausfuehrbaren Schritte. Bitte ergaenze steps oder executionBatches.");
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
        if (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(m_lastAgentRoute)) {
            const QString plainMessage = removeReasoningLeak(repairMojibakeText(content)).trimmed();
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
            appendAgentChat("AI", fallbackMessage);
            m_agentValidationRetries = 0;
            return;
        }
    }
    if (type == "assistant_message") {
        type = "message";
    } else if (type == "tool_proposal") {
        type = "action_proposal";
    } else if (type == "workflow_proposal" || type == "workflow_run") {
        type = "workflow_run_proposal";
    } else if (type == "operation_plan") {
        type = "plan";
    }
    const QString message = reply.value("message").toString();
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

    if (type == "action_proposal" || type == "workflow_run_proposal") {
        if (!routeAllowsCadActions(m_lastAgentRoute)) {
            clearAgentProposal();
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    "Die aktuelle Route erlaubt keine CAD-Aktion. Antworte fuer diese Anfrage mit type=message oder ask_user; tools sind nicht freigegeben.")) {
                return;
            }
            appendAgentChat("Barebone-Qt", "AI Vorschlag abgelehnt: Diese Anfrage ist nicht als CAD-Aktion freigegeben.");
            return;
        }
        if (!m_brxAuthenticated) {
            clearAgentProposal();
            appendAgentChat("Barebone-Qt", "BRX Plugin ist nicht verbunden. CAD-Aktionen koennen erst vorgeschlagen und validiert werden, wenn BricsCAD verbunden ist.");
            return;
        }
        const bool workflowRunProposal = type == QStringLiteral("workflow_run_proposal");
        QJsonObject proposal = normalizedAgentProposal(reply);
        if (!message.trimmed().isEmpty()) {
            proposal.insert("summary", message.trimmed());
        }
        if (workflowRunProposal) {
            const QJsonObject workflowUsage = reply.value(QStringLiteral("workflowUsage")).toObject();
            QString baseWorkflowId = reply.value(QStringLiteral("baseWorkflowId")).toString(
                proposal.value(QStringLiteral("baseWorkflowId")).toString(
                    workflowUsage.value(QStringLiteral("baseWorkflowId")).toString()));
            if (baseWorkflowId.trimmed().isEmpty()) {
                const QStringList routeWorkflows = routeWorkflowIds(m_lastAgentRoute, 1);
                if (!routeWorkflows.isEmpty()) {
                    baseWorkflowId = routeWorkflows.first();
                }
            }
            QString workflowError;
            QString baseWorkflowFileName;
            QJsonObject baseWorkflow = loadWorkflowById(baseWorkflowId, &baseWorkflowFileName, &workflowError);
            if (baseWorkflow.isEmpty()) {
                for (const QJsonValue& value : selectedWorkflowObjectsForRoute(m_lastAgentRoute)) {
                    const QJsonObject candidate = value.toObject();
                    const QString candidateId = candidate.value(QStringLiteral("id")).toString();
                    if (baseWorkflowId.trimmed().isEmpty()
                        || workflowSlug(candidateId) == workflowSlug(baseWorkflowId)) {
                        baseWorkflow = candidate;
                        if (baseWorkflowId.trimmed().isEmpty()) {
                            baseWorkflowId = candidateId;
                        }
                        break;
                    }
                }
            }

            const QString decision = workflowUsage.value(QStringLiteral("decision")).toString(QStringLiteral("adapted")).trimmed();
            const QString reason = workflowUsage.value(QStringLiteral("reason")).toString().trimmed();
            appendBridgeLog(QString("AI Workflow-Auswahl: decision=%1 workflow=%2 reason=%3")
                .arg(decision.isEmpty() ? QStringLiteral("<leer>") : decision,
                    baseWorkflowId.isEmpty() ? QStringLiteral("<kein Workflow>") : baseWorkflowId,
                    reason.left(260)));

            proposal.insert(QStringLiteral("source"), QStringLiteral("workflow_run"));
            const bool persistentWorkflowRun = !baseWorkflowFileName.trimmed().isEmpty()
                || workflowMatchesSelectedWorkflow(baseWorkflow, m_selectedWorkflow, m_selectedWorkflowId);
            proposal.insert(QStringLiteral("runKind"), persistentWorkflowRun ? QStringLiteral("saved_workflow") : QStringLiteral("agent_workflow"));
            proposal.insert(QStringLiteral("workflowId"), baseWorkflowId);
            proposal.insert(QStringLiteral("workflowTitle"), baseWorkflow.value(QStringLiteral("title")).toString(baseWorkflowId));
            proposal.insert(QStringLiteral("workflowUsage"), workflowUsage);
            proposal.insert(QStringLiteral("stepPlan"), reply.value(QStringLiteral("stepPlan")).toArray(proposal.value(QStringLiteral("stepPlan")).toArray()));
            proposal.insert(QStringLiteral("slotValues"), reply.value(QStringLiteral("slotValues")).toObject(proposal.value(QStringLiteral("slotValues")).toObject()));
            if (!baseWorkflow.isEmpty()) {
                proposal.insert(QStringLiteral("workflow"), baseWorkflow);
            }
            proposal.insert(QStringLiteral("requiresConfirmation"), true);
            proposal.insert(QStringLiteral("continueAfterSuccess"), false);
            proposal.remove(QStringLiteral("nextIntent"));
        } else {
            proposal = normalizedRectangularRoomWallProposal(proposal, m_lastAgentUserPrompt);
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
        const QJsonArray actions = agentProposalActions(proposal);
        if (workflowRunProposal) {
            appendBridgeLog(QString("AI Workflow-Run-Vorschlag: %1 Aktionen workflow=%2")
                .arg(actions.size())
                .arg(proposal.value(QStringLiteral("workflowId")).toString()));
        } else if (actions.size() > 1) {
            appendBridgeLog(QString("AI Batch-Vorschlag: %1 Aktionen").arg(actions.size()));
        } else {
            const QJsonObject action = actions.isEmpty() ? QJsonObject{} : actions.first().toObject();
            appendBridgeLog(QString("AI Vorschlag: %1 params=%2")
                .arg(action.value("tool").toString(proposal.value("tool").toString()),
                    QString::fromUtf8(QJsonDocument(action.value("params").toObject(proposal.value("params").toObject())).toJson(QJsonDocument::Compact))));
        }
        preflightAgentProposal(content, proposal, proposal);
        return;
    }

    if (type == "context_request") {
        handleAgentContextRequest(reply);
        return;
    }

    if (type == "ask_user") {
        const QJsonArray missing = reply.value("missing").toArray();
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
        QStringList providedFields = providedMissingFields(m_lastAgentUserPrompt, missing, m_pendingAgentDraft);
        const QStringList inferredFields = inferredProvidedFieldsFromAskMessage(m_lastAgentUserPrompt, message, m_pendingAgentDraft);
        for (const QString& inferredField : inferredFields) {
            if (missingContainsEquivalentField(missing, inferredField)) {
                providedFields.append(inferredField);
            }
        }
        providedFields.removeDuplicates();
        if (!providedFields.isEmpty()) {
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    QString("ask_user fragt nach Feldern, die der Nutzer bereits beantwortet hat: %1. Nutze pendingDraft, userPrompt und inputSchema/apiDoc.post und erzeuge jetzt einen gueltigen action_proposal oder frage nur nach wirklich fehlenden Feldern. Bei geometry.create rectangle gilt: zweite 2D-Abmessung als depth/length senden, nicht als height. Bei Verschieben/Verlaengern/Face-Bewegung ist eine mm-Angabe die Distanz/Offset-Angabe.")
                        .arg(providedFields.join(", ")))) {
                return;
            }
        }
        if (m_pendingAgentDraft.isEmpty()
            && routeAllowsCadActions(m_lastAgentRoute)
            && !missing.isEmpty()) {
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
        setAgentWaitingForUser(reply);
        if (m_pendingAgentDraft.isEmpty()) {
            appendBridgeLog("AI Agent: Rueckfrage ohne Draft");
        } else {
            appendBridgeLog("AI Agent: Rueckfrage mit Draft gespeichert");
        }
        return;
    }

    if (type == "message") {
        if (!m_pendingAgentDraft.isEmpty() && routeAllowsCadActions(m_lastAgentRoute)) {
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    "Es gibt einen offenen pendingDraft. Behandle die Nutzerantwort als Ergaenzung zu diesem Draft und antworte nicht mit einer allgemeinen message. Nutze userPrompt, pendingDraft, tools und inputSchema/apiDoc.post, um entweder action_proposal, context_request oder eine gezielte ask_user-Rueckfrage zu erzeugen.")) {
                return;
            }
        }
        if (!message.isEmpty()) {
            appendAgentChat("AI", message);
        }
        if (!m_pendingAgentProposal.isEmpty()) {
            appendBridgeLog("AI Agent: offener Vorschlag verworfen, AI hat keinen aktualisierten Vorschlag geliefert");
            clearAgentProposal();
        }
        m_agentValidationRetries = 0;
        return;
    }

    if (type == "plan") {
        const QJsonArray missingCapabilities = reply.value("missingCapabilities").toArray();
        const bool acceptPlan = !routeAllowsCadActions(m_lastAgentRoute)
            || !toolsAvailableForRoute
            || !missingCapabilities.isEmpty();
        if (!acceptPlan) {
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    "Antworttyp plan ist fuer eine ausfuehrbare CAD-Anfrage mit verfuegbaren Tools nicht final. Nutze keinen Plan mit Pseudo-Actions. Gib entweder eine echte Rueckfrage ask_user oder einen action_proposal mit tool exakt aus tools[].name und params gemaess inputSchema/apiDoc.post zurueck. Fuer mehrstufige Workflows schlage den ersten sicheren Schritt vor und setze continueAfterSuccess/nextIntent.")) {
                return;
            }
        }
        m_pendingAgentDraft = reply;
        clearAgentProposal();
        appendBridgeLog(QString("AI Agent: Plan missing=%1")
            .arg(QString::fromUtf8(QJsonDocument(reply.value("missingCapabilities").toArray()).toJson(QJsonDocument::Compact))));
        if (!message.isEmpty()) {
            appendAgentChat("AI", message);
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

    if (m_agentValidationRetries >= kMaxAgentValidationRetries) {
        appendBridgeLog(QString("AI Agent Loop: abgebrochen nach %1 Versuchen: %2")
            .arg(kMaxAgentValidationRetries)
            .arg(errorMessage));
        return false;
    }

    ++m_agentValidationRetries;
    appendBridgeLog(QString("AI Agent Loop %1/%2: %3")
        .arg(m_agentValidationRetries)
        .arg(kMaxAgentValidationRetries)
        .arg(errorMessage.left(260)));

    QJsonObject envelope = agentRequestEnvelope(m_lastAgentUserPrompt.isEmpty()
        ? QStringLiteral("Korrigiere deine letzte Antwort.")
        : m_lastAgentUserPrompt,
        m_lastDocumentContext,
        normalizedAgentRouteForMode(m_lastAgentRoute, m_lastAgentUserPrompt, m_lastDocumentContext, m_chatMode));
    envelope.insert("type", "validation_error");
    envelope.insert("validationError", errorMessage);
    envelope.insert("rejectedContent", rejectedContent.left(4000));
    if (!rejectedObject.isEmpty()) {
        envelope.insert("rejectedObject", rejectedObject);
    }
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentValidationRetries},
        {"repeatedRejectedResponse", repeatedResponse},
        {"policy", "Your previous response was not shown to the user. Correct it using the available tools and schemas."},
    });
    QString retryInstruction =
        QStringLiteral("Korrigiere deine letzte Antwort. Antworte ausschliesslich mit einem gueltigen JSON-Objekt. "
        "Nutze Barebone-Agent-JSON v2 mit schema=\"barebone.agent.response.v2\". "
        "Nutze keinen freien Plan und keine Pseudo-Actions. Wenn die Aufgabe ausfuehrbar ist, liefere genau einen action_proposal fuer den naechsten sicheren Schritt. "
        "Wenn workflowCapsules im Envelope vorhanden sind und ein Workflow passt, darfst du stattdessen workflow_run_proposal mit konkreten actions[], slotValues und stepPlan liefern. "
        "Wenn ein Workflow nicht passt, wiederhole keine no_fit-Begruendung als Chattext; korrigiere intern und plane mit effectiveTools. "
        "Direkte BricsCAD-DB-Schreibvorgaenge, AcDb-/LayerTable-/EntityTable-Mutationen und Pseudo-Tools fuer DB-Writes sind verboten; nutze ausschliesslich tools[].name. "
        "Wenn mehrere unabhaengige Aktionen mit bekannten Parametern erforderlich sind, liefere ein action_proposal mit proposal.actions:[{\"tool\":\"...\",\"params\":{...}},...] und proposal.continueAfterSuccess=false. "
        "Fuer mehrere Layer mit Namen/Farben nutze bevorzugt layers.ensureMany mit params.layers. "
        "Nutze continueAfterSuccess nicht, um Batch-Aufgaben wie mehrere Layer oder mehrere gleichartige Objekte einzeln nachzufordern. "
        "tool muss exakt einem tools[].name entsprechen. params muessen inputSchema/apiDoc.post erfuellen. "
        "Wenn validationError mit BRX Preflight beginnt, wiederhole nicht denselben Vorschlag; nutze die dort genannten Fehler, fehlenden Daten und Hinweise verbindlich. ");
    if (repeatedResponse) {
        retryInstruction += QStringLiteral("Deine letzte Antwort war strukturell identisch zu einer bereits abgelehnten Antwort. Aendere Toolwahl, Params, stepPlan oder frage gezielt nach; dieselbe Antwort ist verboten. ");
    }
    retryInstruction += QStringLiteral("Wenn echte Informationen fehlen, nutze ask_user mit missing und einem draft. "
        "Wenn du Zeichnungskontext brauchst, nutze context_request mit exakt einer readOnlyMethods[].name Methode. "
        "Wenn die Anfrage allgemein ist, nutze type=message.");
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
        const QString message = proposal.value("message").toString().trimmed();
        if (!message.isEmpty()) {
            normalized.insert("message", message);
            normalized.insert("summary", message);
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

    if (tool == QStringLiteral("layers.create")) {
        const QString originalName = params.value(QStringLiteral("name")).toString();
        const QString normalizedName = normalizedBricsCadLayerName(originalName);
        if (!normalizedName.isEmpty()) {
            params.insert(QStringLiteral("name"), normalizedName);
        }
    } else if (tool == QStringLiteral("layers.ensureMany")) {
        const QJsonArray layers = params.value(QStringLiteral("layers")).toArray();
        if (!layers.isEmpty()) {
            QJsonArray normalizedLayers;
            for (const QJsonValue& value : layers) {
                QJsonObject layer = value.toObject();
                const QString normalizedName = normalizedBricsCadLayerName(layer.value(QStringLiteral("name")).toString());
                if (!normalizedName.isEmpty()) {
                    layer.insert(QStringLiteral("name"), normalizedName);
                }
                normalizedLayers.append(layer);
            }
            params.insert(QStringLiteral("layers"), normalizedLayers);
        }
    }

    normalized.insert("params", params);
    return normalized;
}

QJsonArray BricsCadPage::agentProposalActions(const QJsonObject& proposal) const
{
    const QJsonArray actions = proposal.value("actions").toArray();
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

QJsonArray BricsCadPage::expandedAgentActions(const QJsonObject& action) const
{
    const QJsonObject normalized = normalizedAgentAction(action);
    const QString tool = normalized.value("tool").toString().trimmed();
    const QJsonObject params = normalized.value("params").toObject();

    QJsonArray expanded;
    if (tool != QStringLiteral("layers.ensureMany")) {
        expanded.append(normalized);
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
    for (const QJsonValue& value : agentProposalActions(proposal)) {
        const QJsonObject action = value.toObject();
        actions.append(QJsonObject{
            {"tool", action.value("tool").toString()},
            {"params", action.value("params").toObject()},
        });
    }

    return QJsonObject{
        {"source", "agent_preflight"},
        {"actions", actions},
    };
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
    if (tool == QStringLiteral("command.execute")) {
        const QString commandLine = params.value("commandLine").toString().trimmed();
        return commandLine.isEmpty()
            ? QStringLiteral("nativen BricsCAD-Befehl gesendet")
            : QString("nativen BricsCAD-Befehl gesendet: %1").arg(commandLine.left(120));
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
                || summaryLower.contains(QStringLiteral("übersprungen"))) {
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
    appendBridgeLog(QString("Qt -> BRX: actions.validate %1 Aktion(en)").arg(actionCount));
    setAgentBusy(true);

    const bool queued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        params,
        15000,
        [this, rejectedContent, rejectedObject, proposal](const QJsonObject& response) {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            const bool transportOk = response.value("ok").toBool(false);
            const bool valid = transportOk && result.value("valid").toBool(false);
            if (valid) {
                const QStringList warnings = stringsFromJsonArray(result.value("warnings").toArray());
                if (!warnings.isEmpty()) {
                    appendBridgeLog(QString("BRX Preflight: gueltig mit Warnungen: %1").arg(warnings.join("; ").left(500)));
                } else {
                    appendBridgeLog("BRX Preflight: gueltig");
                }
                m_agentValidationRetries = 0;
                setAgentBusy(false);
                QJsonObject readyProposal = proposal;
                if (!warnings.isEmpty()) {
                    QJsonArray warningValues;
                    for (const QString& warning : warnings) {
                        warningValues.append(repairMojibakeText(warning));
                    }
                    readyProposal.insert("preflightWarnings", warningValues);
                }
                setAgentProposal(readyProposal);
                return;
            }

            const QString message = validationFailureMessage(response);
            appendBridgeLog(QString("BRX Preflight: abgelehnt: %1").arg(message.left(700).replace('\n', " | ")));
            const QJsonArray proposalActions = agentProposalActions(proposal);
            if (proposalPreflightFailureCanBeDeferred(proposalActions, message)) {
                QJsonObject readyProposal = proposal;
                QJsonArray warningValues;
                warningValues.append(repairMojibakeText(message));
                warningValues.append(QStringLiteral("Ein spaeterer Batch-Schritt verweist auf Objekte, die erst durch vorherige Schritte entstehen. Qt prueft jeden Schritt vor der Ausfuehrung erneut mit BRX."));
                readyProposal.insert(QStringLiteral("preflightWarnings"), warningValues);
                readyProposal.insert(QStringLiteral("deferredRuntimePreflight"), true);
                appendBridgeLog("BRX Preflight: runtime-abhaengiger Selector wird bis zur Einzelaktion zurueckgestellt");
                m_agentValidationRetries = 0;
                setAgentBusy(false);
                setAgentProposal(readyProposal);
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
    const QString method = request.value("method").toString().trimmed();
    const QJsonObject params = request.value("params").toObject();
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
        [this, request, method](const QJsonObject& response) {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            if (!response.value("ok").toBool(false)) {
                const QString message = bridgeErrorMessage(response, "Kontextabfrage fehlgeschlagen");
                appendAgentChat("BRX", QString("Kontextabfrage %1 fehlgeschlagen: %2").arg(method, message));
                appendBridgeLog(QString("BRX -> Qt: ERROR context %1 %2").arg(method, message));
            } else {
                const QJsonObject result = response.value("result").toObject();
                const int count = result.value("count").toInt(result.value("layers").toArray().size());
                appendBridgeLog(QString("BRX -> Qt Kontext: %1 count=%2").arg(method).arg(count));
            }
            continueAgentWithContextResult(request, response);
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

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, true, "context_result");
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

    clearAgentProposal();
    setAgentBusy(true);

    const bool workflowRun = executedProposal.value("source").toString() == QStringLiteral("workflow_run");
    if (workflowRun) {
        const QString runKind = executedProposal.value(QStringLiteral("runKind")).toString(QStringLiteral("saved_workflow"));
        m_workflowRunState.active = true;
        m_workflowRunState.awaitingUser = false;
        m_workflowRunState.runKind = runKind;
        m_workflowRunState.proposal = executedProposal;
        m_workflowRunState.actions = actions;
        m_workflowRunState.results = {};
        m_workflowRunState.workingWorkflow = executedProposal.value(QStringLiteral("workflow")).toObject(m_selectedWorkflow);
        m_workflowRunState.draftCheckpointWorkflow = m_workflowRunState.workingWorkflow;
        m_workflowRunState.slotValues = executedProposal.value(QStringLiteral("slotValues")).toObject(m_selectedWorkflowSlotValues);
        m_workflowRunState.acceptedStepIndexes = {};
        m_workflowRunState.acceptedConcreteActions = {};
        m_workflowRunState.nextIndex = 0;
        m_workflowRunState.currentStepIndex = -1;
        m_workflowRunState.repairAttemptCount = 0;
        m_workflowRunState.runGeneration = m_operationGeneration;
        m_workflowRunState.repairing = false;
        m_workflowRunState.readyToFinalSave = false;
        m_workflowRunState.lastStepResponse = {};
        appendBridgeLog(QString("Workflow Run: Nutzer bestaetigt; kind=%1 starte Schritt 1/%2")
            .arg(runKind)
            .arg(actions.size()));
    } else if (actions.size() > 1) {
        appendBridgeLog(QString("AI Agent: Nutzer bestaetigt; fuehre %1 Aktionen nacheinander aus").arg(actions.size()));
    } else {
        const QJsonObject action = actions.first().toObject();
        appendBridgeLog(QString("AI Agent: Nutzer bestaetigt; fuehre %1 ueber %2 aus")
            .arg(action.value("tool").toString(), bridgeMethodForTool(action.value("tool").toString())));
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
        m_lastAgentToolResult = batchResult;
        m_pendingAgentDraft = {};
        m_agentValidationRetries = 0;

        appendBridgeLog(QString("BRX Batch: %1").arg(QString::fromUtf8(QJsonDocument(batchResult).toJson(QJsonDocument::Compact)).left(1600)));
        if (proposal.value("source").toString() == QStringLiteral("workflow_run")) {
            clearWorkflowRunState();
            if (m_agentBridge) {
                emitToWebAsync(m_agentBridge, [payload = QVariantMap{
                    {"ok", batchResult.value("failed").toInt(0) == 0},
                    {"message", resultSummary},
                    {"result", batchResult.toVariantMap()},
                }](AiWebBridge* target) {
                    Q_EMIT target->workflowRunFinished(payload);
                });
            }
        }

        const QString finalSummary = agentCompletionSummary(actions, results, resultSummary);

        m_agentConversation.append(QJsonObject{
            {"role", "assistant"},
            {"content", QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "tool_result"},
                {"message", finalSummary},
                {"status", "completed"},
                {"batch", total > 1},
                {"result", batchResult},
            }).toJson(QJsonDocument::Compact))},
        });

        if (proposal.value("continueAfterSuccess").toBool(false) && actions.size() == 1) {
            setAgentBusy(false);
            QJsonObject response;
            if (!results.isEmpty()) {
                response = results.first().toObject().value("response").toObject();
            }
            continueAgentAfterToolResult(proposal, response);
        } else {
            requestAgentExecutionSummary(proposal, actions, results, batchResult, finalSummary);
        }
        return;
    }

    const QJsonObject action = actions.at(index).toObject();
    const QString tool = action.value("tool").toString();
    const QString bridgeMethod = bridgeMethodForTool(tool);
    QJsonObject params = paramsWithRuntimeBatchHandles(tool, action.value("params").toObject(), results);
    if (actions.size() > 1) {
        params.insert("saveBefore", index == 0);
    }

    auto executeCurrentAction = [this, proposal, actions, index, results, tool, bridgeMethod, params]() mutable {
        appendBridgeLog(QString("Qt -> BRX Batch %1/%2: %3 saveBefore=%4")
            .arg(index + 1)
            .arg(actions.size())
            .arg(bridgeMethod)
            .arg(params.value("saveBefore").toBool(false) ? "true" : "false"));

        const bool queued = sendBridgeRequest(
            bridgeMethod,
            params,
            30000,
            [this, proposal, actions, index, results, tool, bridgeMethod, params](const QJsonObject& response) mutable {
                appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
                if (!response.value("ok").toBool(false)) {
                    const QString message = bridgeErrorMessage(response, "Tool-Ausfuehrung fehlgeschlagen");
                    appendAgentChat("BRX", QString("%1 fehlgeschlagen: %2").arg(tool, message));
                    appendBridgeLog(QString("BRX -> Qt: ERROR %1 %2").arg(bridgeMethod, message));
                    if (proposal.value("source").toString() == QStringLiteral("workflow_run")) {
                        const QJsonObject stepResult{
                            {"index", index + 1},
                            {"tool", tool},
                            {"bridgeMethod", bridgeMethod},
                            {"params", params},
                            {"response", response},
                            {"error", message},
                        };
                        results.append(stepResult);
                        pauseWorkflowRunAfterStep(proposal, actions, index, results, stepResult);
                        return;
                    }
                    QJsonObject failedProposal = proposal;
                    failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
                    failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{
                        {"tool", tool},
                        {"params", params},
                    });
                    failedProposal.insert(QStringLiteral("completedResults"), results);
                    failedProposal.insert(QStringLiteral("failurePolicy"),
                        QStringLiteral("Die Batch-Ausfuehrung wurde bei dieser Aktion gestoppt. Korrigiere den gesamten verbleibenden Vorschlag oder frage gezielt nach."));
                    continueAgentAfterToolFailure(
                        failedProposal,
                        response,
                        QStringLiteral("Batch-Aktion %1/%2 (%3) fehlgeschlagen: %4")
                            .arg(index + 1)
                            .arg(actions.size())
                            .arg(tool)
                            .arg(message));
                    return;
                }

                const QJsonObject result = response.value("result").toObject();
                m_lastAgentToolResult = result;
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

                if (proposal.value("source").toString() == QStringLiteral("workflow_run")) {
                    pauseWorkflowRunAfterStep(proposal, actions, index, results, stepResult);
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

    if (actions.size() <= 1 && proposal.value("source").toString() != QStringLiteral("workflow_run")) {
        executeCurrentAction();
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
        {"tool", tool},
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
            const bool valid = response.value("ok").toBool(false) && result.value("valid").toBool(false);
            if (valid) {
                appendBridgeLog(QString("BRX Preflight: Batch-Aktion %1/%2 gueltig")
                    .arg(index + 1)
                    .arg(actions.size()));
                executeCurrentAction();
                return;
            }

            const QString message = validationFailureMessage(response);
            appendBridgeLog(QString("BRX Preflight: Batch-Aktion %1/%2 abgelehnt: %3")
                .arg(index + 1)
                .arg(actions.size())
                .arg(message.left(700).replace('\n', " | ")));
            if (proposal.value("source").toString() == QStringLiteral("workflow_run")) {
                const QJsonObject stepResult{
                    {"index", index + 1},
                    {"tool", tool},
                    {"bridgeMethod", bridgeMethod},
                    {"params", params},
                    {"response", response},
                    {"error", message},
                    {"phase", "preflight"},
                };
                QJsonArray nextResults = results;
                nextResults.append(stepResult);
                pauseWorkflowRunAfterStep(proposal, actions, index, nextResults, stepResult);
                return;
            }
            QJsonObject failedProposal = proposal;
            failedProposal.insert(QStringLiteral("failedActionIndex"), index + 1);
            failedProposal.insert(QStringLiteral("failedAction"), QJsonObject{
                {"tool", tool},
                {"params", params},
            });
            failedProposal.insert(QStringLiteral("completedResults"), results);
            failedProposal.insert(QStringLiteral("failurePolicy"),
                QStringLiteral("Die Batch-Ausfuehrung wurde vor dieser Aktion gestoppt. Korrigiere den gesamten verbleibenden Vorschlag oder frage gezielt nach."));
            continueAgentAfterToolFailure(
                failedProposal,
                response,
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

void BricsCadPage::clearWorkflowRunState()
{
    m_workflowRunState = WorkflowRunState{};
}

bool BricsCadPage::saveWorkflowRunWorkingWorkflow(QString* errorMessage)
{
    if (!m_workflowRunState.active || m_workflowRunState.workingWorkflow.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Kein aktiver Workflow-Lauf vorhanden.");
        }
        return false;
    }

    QJsonObject workflow = m_workflowRunState.workingWorkflow;
    workflow.remove(QStringLiteral("fileName"));
    QString savedPath;
    QString saveError;
    if (!saveWorkflowFromTraining(workflow, &savedPath, &saveError)) {
        if (errorMessage) {
            *errorMessage = saveError;
        }
        return false;
    }

    const QString workflowId = workflow.value(QStringLiteral("id")).toString(m_selectedWorkflowId);
    QString fileName;
    QJsonObject reloaded = loadWorkflowById(workflowId, &fileName, nullptr);
    if (reloaded.isEmpty()) {
        reloaded = workflow;
    }
    m_workflowRunState.workingWorkflow = reloaded;
    m_selectedWorkflowId = reloaded.value(QStringLiteral("id")).toString(workflowId);
    m_selectedWorkflow = reloaded;
    m_selectedWorkflowSlotValues = reloaded.value(QStringLiteral("knownSlotValues")).toObject(m_workflowRunState.slotValues);
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [workflowMap = m_selectedWorkflow.toVariantMap()](AiWebBridge* target) {
            Q_EMIT target->selectedWorkflowChanged(workflowMap);
        });
    }
    emitWorkflowListToWeb();
    appendBridgeLog(QString("Workflow Run: Arbeitsstand gespeichert: %1").arg(savedPath));
    return true;
}

bool BricsCadPage::workflowRunShouldPersistWorkingWorkflow() const
{
    if (!m_workflowRunState.active || m_workflowRunState.workingWorkflow.isEmpty()) {
        return false;
    }
    if (m_workflowRunState.runKind == QStringLiteral("saved_workflow")) {
        return true;
    }
    if (m_workflowRunState.runKind != QStringLiteral("agent_workflow")) {
        return false;
    }
    if (workflowMatchesSelectedWorkflow(m_workflowRunState.workingWorkflow, m_selectedWorkflow, m_selectedWorkflowId)) {
        return true;
    }

    const QString workflowId = m_workflowRunState.workingWorkflow.value(QStringLiteral("id")).toString().trimmed();
    if (workflowId.isEmpty()) {
        return false;
    }
    QString fileName;
    const QJsonObject savedWorkflow = loadWorkflowById(workflowId, &fileName, nullptr);
    return !savedWorkflow.isEmpty() && !fileName.trimmed().isEmpty();
}

QJsonObject BricsCadPage::replaceWorkflowRunStep(QJsonObject workflow, const QJsonObject& source, const QJsonObject& replacementStep) const
{
    QJsonObject step = replacementStep;
    step.remove(QStringLiteral("_workflowSource"));
    step.remove(QStringLiteral("params"));

    const QString kind = source.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("executionBatch")) {
        QJsonArray batches = workflow.value(QStringLiteral("executionBatches")).toArray();
        const int batchIndex = source.value(QStringLiteral("batchIndex")).toInt(-1);
        const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
        if (batchIndex >= 0 && batchIndex < batches.size()) {
            QJsonObject batch = batches.at(batchIndex).toObject();
            QJsonArray steps = batch.value(QStringLiteral("steps")).toArray();
            if (stepIndex >= 0 && stepIndex < steps.size()) {
                steps.replace(stepIndex, step);
                batch.insert(QStringLiteral("steps"), steps);
                batches.replace(batchIndex, batch);
                workflow.insert(QStringLiteral("executionBatches"), batches);

                QJsonArray flattened;
                for (const QJsonValue& batchValue : batches) {
                    const QJsonArray batchSteps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
                    for (const QJsonValue& value : batchSteps) {
                        flattened.append(value);
                    }
                }
                workflow.insert(QStringLiteral("steps"), flattened);
                return workflow;
            }
        }
    }

    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
    if (stepIndex >= 0 && stepIndex < steps.size()) {
        steps.replace(stepIndex, step);
        workflow.insert(QStringLiteral("steps"), steps);
    }
    return workflow;
}

QJsonObject BricsCadPage::insertWorkflowRunStep(QJsonObject workflow, const QJsonObject& source, const QJsonObject& insertedStep, bool beforeCurrent) const
{
    QJsonObject step = insertedStep;
    step.remove(QStringLiteral("_workflowSource"));
    step.remove(QStringLiteral("params"));

    const QString kind = source.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("executionBatch")) {
        QJsonArray batches = workflow.value(QStringLiteral("executionBatches")).toArray();
        const int batchIndex = source.value(QStringLiteral("batchIndex")).toInt(-1);
        const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
        if (batchIndex >= 0 && batchIndex < batches.size()) {
            QJsonObject batch = batches.at(batchIndex).toObject();
            QJsonArray steps = batch.value(QStringLiteral("steps")).toArray();
            const int insertIndex = std::clamp(stepIndex + (beforeCurrent ? 0 : 1), 0, static_cast<int>(steps.size()));
            steps.insert(insertIndex, step);
            batch.insert(QStringLiteral("steps"), steps);
            batches.replace(batchIndex, batch);
            workflow.insert(QStringLiteral("executionBatches"), batches);

            QJsonArray flattened;
            for (const QJsonValue& batchValue : batches) {
                const QJsonArray batchSteps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
                for (const QJsonValue& value : batchSteps) {
                    flattened.append(value);
                }
            }
            workflow.insert(QStringLiteral("steps"), flattened);
            return workflow;
        }
    }

    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
    if (stepIndex >= 0) {
        const int insertIndex = std::clamp(stepIndex + (beforeCurrent ? 0 : 1), 0, static_cast<int>(steps.size()));
        steps.insert(insertIndex, step);
        workflow.insert(QStringLiteral("steps"), steps);
    }
    return workflow;
}

QJsonObject BricsCadPage::insertWorkflowRunStepAtIndex(QJsonObject workflow, int stepIndex, const QJsonObject& insertedStep) const
{
    QJsonObject step = insertedStep;
    step.remove(QStringLiteral("_workflowSource"));
    step.remove(QStringLiteral("params"));

    if (stepIndex < 0) {
        stepIndex = 0;
    }

    QJsonArray batches = workflow.value(QStringLiteral("executionBatches")).toArray();
    if (!batches.isEmpty()) {
        int cursor = 0;
        for (int batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
            QJsonObject batch = batches.at(batchIndex).toObject();
            QJsonArray batchSteps = batch.value(QStringLiteral("steps")).toArray();
            if (stepIndex <= cursor + batchSteps.size()) {
                const int localIndex = std::clamp(stepIndex - cursor, 0, static_cast<int>(batchSteps.size()));
                batchSteps.insert(localIndex, step);
                batch.insert(QStringLiteral("steps"), batchSteps);
                batches.replace(batchIndex, batch);
                workflow.insert(QStringLiteral("executionBatches"), batches);

                QJsonArray flattened;
                for (const QJsonValue& batchValue : batches) {
                    const QJsonArray steps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
                    for (const QJsonValue& value : steps) {
                        flattened.append(value);
                    }
                }
                workflow.insert(QStringLiteral("steps"), flattened);
                return workflow;
            }
            cursor += batchSteps.size();
        }

        QJsonObject lastBatch = batches.last().toObject();
        QJsonArray lastSteps = lastBatch.value(QStringLiteral("steps")).toArray();
        lastSteps.append(step);
        lastBatch.insert(QStringLiteral("steps"), lastSteps);
        batches.replace(batches.size() - 1, lastBatch);
        workflow.insert(QStringLiteral("executionBatches"), batches);

        QJsonArray flattened;
        for (const QJsonValue& batchValue : batches) {
            const QJsonArray steps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
            for (const QJsonValue& value : steps) {
                flattened.append(value);
            }
        }
        workflow.insert(QStringLiteral("steps"), flattened);
        return workflow;
    }

    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    const int insertIndex = std::clamp(stepIndex, 0, static_cast<int>(steps.size()));
    steps.insert(insertIndex, step);
    workflow.insert(QStringLiteral("steps"), steps);
    return workflow;
}

QJsonObject BricsCadPage::removeWorkflowRunStep(QJsonObject workflow, const QJsonObject& source) const
{
    const QString kind = source.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("executionBatch")) {
        QJsonArray batches = workflow.value(QStringLiteral("executionBatches")).toArray();
        const int batchIndex = source.value(QStringLiteral("batchIndex")).toInt(-1);
        const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
        if (batchIndex >= 0 && batchIndex < batches.size()) {
            QJsonObject batch = batches.at(batchIndex).toObject();
            QJsonArray steps = batch.value(QStringLiteral("steps")).toArray();
            if (stepIndex >= 0 && stepIndex < steps.size()) {
                steps.removeAt(stepIndex);
                if (steps.isEmpty()) {
                    batches.removeAt(batchIndex);
                } else {
                    batch.insert(QStringLiteral("steps"), steps);
                    batches.replace(batchIndex, batch);
                }
                workflow.insert(QStringLiteral("executionBatches"), batches);

                QJsonArray flattened;
                for (const QJsonValue& batchValue : batches) {
                    const QJsonArray batchSteps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
                    for (const QJsonValue& value : batchSteps) {
                        flattened.append(value);
                    }
                }
                workflow.insert(QStringLiteral("steps"), flattened);
                return workflow;
            }
        }
    }

    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    const int stepIndex = source.value(QStringLiteral("stepIndex")).toInt(-1);
    if (stepIndex >= 0 && stepIndex < steps.size()) {
        steps.removeAt(stepIndex);
        workflow.insert(QStringLiteral("steps"), steps);
    }
    return workflow;
}

QJsonObject BricsCadPage::removeWorkflowRunStepAtIndex(QJsonObject workflow, int stepIndex) const
{
    if (stepIndex < 0) {
        return workflow;
    }

    QJsonArray batches = workflow.value(QStringLiteral("executionBatches")).toArray();
    int cursor = 0;
    for (int batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
        QJsonObject batch = batches.at(batchIndex).toObject();
        QJsonArray batchSteps = batch.value(QStringLiteral("steps")).toArray();
        for (int i = 0; i < batchSteps.size(); ++i) {
            if (cursor == stepIndex) {
                batchSteps.removeAt(i);
                if (batchSteps.isEmpty()) {
                    batches.removeAt(batchIndex);
                } else {
                    batch.insert(QStringLiteral("steps"), batchSteps);
                    batches.replace(batchIndex, batch);
                }
                workflow.insert(QStringLiteral("executionBatches"), batches);

                QJsonArray flattened;
                for (const QJsonValue& batchValue : batches) {
                    const QJsonArray steps = batchValue.toObject().value(QStringLiteral("steps")).toArray();
                    for (const QJsonValue& value : steps) {
                        flattened.append(value);
                    }
                }
                workflow.insert(QStringLiteral("steps"), flattened);
                return workflow;
            }
            ++cursor;
        }
    }

    QJsonArray steps = workflow.value(QStringLiteral("steps")).toArray();
    if (stepIndex >= 0 && stepIndex < steps.size()) {
        steps.removeAt(stepIndex);
        workflow.insert(QStringLiteral("steps"), steps);
    }
    return workflow;
}

QJsonObject BricsCadPage::workflowStepFromRepairReply(const QJsonObject& reply, const QJsonObject& currentAction) const
{
    QJsonObject step = reply.value(QStringLiteral("step")).toObject();
    if (step.isEmpty()) {
        step = reply.value(QStringLiteral("workflowStep")).toObject();
    }
    if (step.isEmpty()) {
        step = reply.value(QStringLiteral("replacementStep")).toObject();
    }

    const QJsonObject oldStep = currentAction.value(QStringLiteral("workflowStep")).toObject();
    if (step.isEmpty()) {
        QJsonObject action = reply.value(QStringLiteral("action")).toObject();
        if (action.isEmpty()) {
            action = reply.value(QStringLiteral("repairedAction")).toObject();
        }
        action = normalizedAgentAction(action);
        if (!action.isEmpty()) {
            step = oldStep;
            step.insert(QStringLiteral("tool"), action.value(QStringLiteral("tool")).toString());
            step.insert(QStringLiteral("paramsTemplate"), action.value(QStringLiteral("params")).toObject());
        }
    }

    if (step.isEmpty()) {
        return {};
    }

    if (step.value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
        step.insert(QStringLiteral("id"), oldStep.value(QStringLiteral("id")).toString(currentAction.value(QStringLiteral("workflowStepId")).toString()));
    }
    if (step.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        step.insert(QStringLiteral("title"), oldStep.value(QStringLiteral("title")).toString(currentAction.value(QStringLiteral("workflowStepTitle")).toString()));
    }
    if (!step.value(QStringLiteral("requiresSlots")).isArray() && oldStep.value(QStringLiteral("requiresSlots")).isArray()) {
        step.insert(QStringLiteral("requiresSlots"), oldStep.value(QStringLiteral("requiresSlots")).toArray());
    }
    if (!step.contains(QStringLiteral("condition")) && oldStep.contains(QStringLiteral("condition"))) {
        step.insert(QStringLiteral("condition"), oldStep.value(QStringLiteral("condition")));
    }
    if (!step.value(QStringLiteral("paramsTemplate")).isObject() && step.value(QStringLiteral("params")).isObject()) {
        step.insert(QStringLiteral("paramsTemplate"), step.value(QStringLiteral("params")).toObject());
    }
    step.remove(QStringLiteral("params"));
    step.remove(QStringLiteral("_workflowSource"));
    return step;
}

QJsonObject BricsCadPage::workflowInsertedStepFromRepairReply(const QJsonObject& reply, const QJsonObject& currentAction) const
{
    QJsonObject step = reply.value(QStringLiteral("step")).toObject();
    if (step.isEmpty()) {
        step = reply.value(QStringLiteral("insertedStep")).toObject();
    }
    if (step.isEmpty()) {
        step = reply.value(QStringLiteral("newStep")).toObject();
    }
    if (step.isEmpty()) {
        step = reply.value(QStringLiteral("workflowStep")).toObject();
    }

    if (step.isEmpty()) {
        QJsonObject action = reply.value(QStringLiteral("action")).toObject();
        if (action.isEmpty()) {
            action = reply.value(QStringLiteral("insertedAction")).toObject();
        }
        if (action.isEmpty()) {
            action = reply.value(QStringLiteral("newAction")).toObject();
        }
        action = normalizedAgentAction(action);
        if (!action.isEmpty()) {
            step.insert(QStringLiteral("tool"), action.value(QStringLiteral("tool")).toString());
            step.insert(QStringLiteral("paramsTemplate"), action.value(QStringLiteral("params")).toObject());
        }
    }

    if (step.isEmpty()) {
        return {};
    }

    if (!step.value(QStringLiteral("paramsTemplate")).isObject() && step.value(QStringLiteral("params")).isObject()) {
        step.insert(QStringLiteral("paramsTemplate"), step.value(QStringLiteral("params")).toObject());
    }

    const QString oldId = currentAction.value(QStringLiteral("workflowStepId")).toString(
        currentAction.value(QStringLiteral("workflowStep")).toObject().value(QStringLiteral("id")).toString());
    const QString tool = step.value(QStringLiteral("tool")).toString().trimmed();
    QString id = step.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty() || (!oldId.isEmpty() && workflowSlug(id) == workflowSlug(oldId))) {
        QString toolSlug = workflowSlug(tool);
        if (toolSlug.isEmpty()) {
            toolSlug = QStringLiteral("step");
        }
        id = QStringLiteral("insert_%1_before_%2").arg(toolSlug, oldId.isEmpty() ? QStringLiteral("current") : workflowSlug(oldId));
        step.insert(QStringLiteral("id"), id);
    }

    QString title = repairMojibakeText(step.value(QStringLiteral("title")).toString()).trimmed();
    const QString oldTitle = repairMojibakeText(currentAction.value(QStringLiteral("workflowStepTitle")).toString()).trimmed();
    if (title.isEmpty() || (!oldTitle.isEmpty() && title == oldTitle && tool != currentAction.value(QStringLiteral("tool")).toString())) {
        title = tool.isEmpty() ? QStringLiteral("Eingefuegter Workflow-Schritt") : QStringLiteral("Eingefuegter Schritt: %1").arg(tool);
        step.insert(QStringLiteral("title"), title);
    }

    step.remove(QStringLiteral("params"));
    step.remove(QStringLiteral("_workflowSource"));
    return step;
}

void BricsCadPage::requestWorkflowStepRepair(const QString& userFeedback, const QString& failureContext)
{
    if (!m_workflowRunState.active || m_workflowRunState.currentStepIndex < 0) {
        appendAgentChat("Barebone-Qt", "Workflow-Korrektur nicht moeglich: kein aktiver Schritt.");
        return;
    }
    if (m_workflowRunState.repairAttemptCount >= kMaxAgentValidationRetries) {
        setAgentBusy(false);
        m_workflowRunState.awaitingUser = true;
        m_workflowRunState.repairing = true;
        m_workflowRunState.repairAttemptCount = 0;
        appendBridgeLog(QString("Workflow Step Repair: abgebrochen nach %1 Versuchen: %2")
            .arg(kMaxAgentValidationRetries)
            .arg(failureContext.left(240)));
        appendAgentChat("Barebone-Qt", QString("Workflow-Schritt konnte nach %1 Korrekturversuchen nicht automatisch validiert werden. Bitte formuliere die Korrektur konkreter.")
            .arg(kMaxAgentValidationRetries));
        return;
    }

    ++m_workflowRunState.repairAttemptCount;
    m_workflowRunState.awaitingUser = false;
    m_workflowRunState.repairing = true;

    const int index = m_workflowRunState.currentStepIndex;
    const QJsonObject currentAction = (index >= 0 && index < m_workflowRunState.actions.size())
        ? m_workflowRunState.actions.at(index).toObject()
        : QJsonObject{};
    QStringList repairTools;
    const QString currentTool = currentAction.value(QStringLiteral("tool")).toString().trimmed();
    if (!currentTool.isEmpty()) {
        repairTools << currentTool;
    }
    const QJsonObject currentWorkflowStep = currentAction.value(QStringLiteral("workflowStep")).toObject();
    const QString workflowStepTool = currentWorkflowStep.value(QStringLiteral("tool")).toString().trimmed();
    if (!workflowStepTool.isEmpty() && !repairTools.contains(workflowStepTool)) {
        repairTools << workflowStepTool;
    }
    for (const QString& tool : workflowToolNamesForSelector(m_workflowRunState.workingWorkflow, 10)) {
        if (!tool.isEmpty() && !repairTools.contains(tool)) {
            repairTools << tool;
        }
    }
    QJsonObject repairRoute = normalizedAgentRouteForMode(
        makeAgentRoute(QStringLiteral("bricscad_action"), QStringLiteral("Workflow-Schritt reparieren")),
        userFeedback,
        {},
        QStringLiteral("bricscad"));
    if (!repairTools.isEmpty()) {
        repairRoute.insert(QStringLiteral("selectedTools"), stringsToJsonArray(repairTools));
        repairRoute.insert(QStringLiteral("toolSelectionAttempted"), true);
    }
    const QJsonArray repairEffectiveTools = availableAgentToolsForRoute(repairRoute, userFeedback);
    const QJsonObject repairRequest{
        {"schema", "barebone.workflow.step_repair.request.v1"},
        {"attempt", m_workflowRunState.repairAttemptCount},
        {"maxAttempts", kMaxAgentValidationRetries},
        {"userFeedback", userFeedback},
        {"failureContext", failureContext},
        {"repairDialog", m_workflowRunState.repairDialog},
        {"workflow", m_workflowRunState.workingWorkflow},
        {"slotValues", m_workflowRunState.slotValues},
        {"stepIndex", index + 1},
        {"totalSteps", m_workflowRunState.actions.size()},
        {"currentAction", currentAction},
        {"lastStepResponse", m_workflowRunState.lastStepResponse},
        {"previousResults", m_workflowRunState.results},
        {"capabilities", QJsonObject{}},
        {"capabilitySummary", capabilitySummary(m_brxCapabilities)},
        {"effectiveTools", repairEffectiveTools},
        {"responseContract", QJsonObject{
            {"schema", "barebone.workflow.step_repair.response.v1"},
            {"allowedTypes", QJsonArray{"ask_user", "step_update", "step_insert"}},
            {"askUserShape", QJsonObject{
                {"type", "ask_user"},
                {"message", "Eine konkrete fachliche Rueckfrage als vollstaendiger Satz; niemals nur 'kurze Rueckfrage'."},
                {"questions", QJsonArray{"Konkrete Frage mit Bezug zum aktuellen Schritt, BRX-Fehler oder fehlenden Wert."}},
            }},
            {"stepUpdateShape", QJsonObject{{"type", "step_update"}, {"message", "kurze Begruendung"}, {"step", QJsonObject{{"id", "gleich oder korrigiert"}, {"title", "Titel"}, {"tool", "tool.name"}, {"paramsTemplate", QJsonObject{}}}}}},
            {"stepInsertShape", QJsonObject{{"type", "step_insert"}, {"message", "kurze Begruendung"}, {"position", "before_current|after_current"}, {"step", QJsonObject{{"id", "neue eindeutige id"}, {"title", "Titel"}, {"tool", "tool.name"}, {"paramsTemplate", QJsonObject{}}}}}},
        }},
        {"policy", "Repariere den aktuellen Workflow-Lauf minimal. Verwende step_update, wenn der aktuelle Schritt ersetzt werden muss. Verwende step_insert, wenn der Nutzer eine zusaetzliche Aktion vor oder nach dem aktuellen Schritt verlangt, z.B. 'erst verschieben und im naechsten Schritt rotieren'. Behalte bei step_update id/title moeglichst bei; nutze bei step_insert eine neue eindeutige id. Verwende nur effectiveTools[].name und deren Schemas/Policies. Beruecksichtige repairDialog verbindlich: Wiederhole keine Frage, die der Nutzer dort bereits beantwortet hat. Wenn der Nutzer Tool-/Funktionssuche verlangt, liefere bevorzugt step_update oder step_insert. Wenn BRX meldet, dass ein Selector keine Objekte findet, schlage einen expliziteren Selector oder einen vorgeschalteten selection.set Schritt als step_insert nur dann vor, wenn zusaetzliche Auswahl wirklich notwendig ist; sonst frage gezielt nach."},
    };

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid() || (officialProvider && m_config.aiApiKey().trimmed().isEmpty())) {
        setAgentBusy(false);
        appendAgentChat("Barebone-Qt", "Workflow-Schritt kann nicht mit der AI repariert werden: AI-Konfiguration ist unvollstaendig.");
        return;
    }

    QJsonArray messages{
        QJsonObject{
            {"role", "system"},
            {"content",
                "Du reparierst genau einen Schritt eines Barebone-Qt Workflows. "
                "Antworte ausschliesslich mit JSON gemaess barebone.workflow.step_repair.response.v1. "
                "Nutze ask_user nur fuer fehlende fachliche Informationen und formuliere dann eine konkrete Frage mit Kontext. "
                "Schreibe niemals nur 'Kurze Rueckfrage', 'Rueckfrage' oder Platzhaltertexte. "
                "Wiederhole keine Rueckfrage, die in repairDialog bereits beantwortet wurde; nutze dann step_update oder erklaere in step_update/message, dass das Tool die gewünschte Variante nicht unterstuetzt. "
                "Wenn der Nutzer dich auffordert, eine geeignete Funktion oder Tool-Alternative zu suchen, liefere step_update statt ask_user, sofern die Tooldaten dafuer reichen. "
                "Nutze step_update fuer eine konkrete korrigierte Version des aktuellen Schritts. "
                "Nutze step_insert mit position=before_current oder after_current, wenn der Nutzer einen zusaetzlichen Schritt verlangt; ersetze dann nicht den bestehenden Schritt. "
                "Erfinde keine Tools und keine direkten BricsCAD-Datenbank-Schreibvorgaenge."},
        },
        QJsonObject{
            {"role", "user"},
            {"content", QString::fromUtf8(QJsonDocument(repairRequest).toJson(QJsonDocument::Compact))},
        },
    };

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", adjustedOutputTokenLimitForMessages(messages, 2048));
        QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort == QStringLiteral("high")) {
            reasoningEffort = QStringLiteral("medium");
        }
        if (reasoningEffort != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", adjustedOutputTokenLimitForMessages(messages, 2048));
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    setAgentBusy(true);
    appendBridgeLog(QString("Workflow Step Repair %1/%2: Schritt %3/%4 wird mit AI ueberarbeitet")
        .arg(m_workflowRunState.repairAttemptCount)
        .arg(kMaxAgentValidationRetries)
        .arg(index + 1)
        .arg(m_workflowRunState.actions.size()));

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, userFeedback, operationGeneration]() {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = QStringLiteral("AI Fehler bei Workflow-Schritt-Reparatur: %1").arg(reply->errorString());
            reply->deleteLater();
            requestWorkflowStepRepair(userFeedback, error);
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            const QString error = QStringLiteral("AI Reparaturantwort war kein gueltiges OpenAI JSON: %1").arg(parseError.errorString());
            reply->deleteLater();
            requestWorkflowStepRepair(userFeedback, error);
            return;
        }
        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        reply->deleteLater();
        handleWorkflowStepRepairReply(content, userFeedback);
    });
}

void BricsCadPage::handleWorkflowStepRepairReply(const QString& content, const QString& userFeedback)
{
    bool parsed = false;
    const QJsonObject reply = jsonObjectFromAiContent(content, &parsed);
    if (!parsed) {
        requestWorkflowStepRepair(userFeedback, QStringLiteral("Die AI-Reparaturantwort war kein gueltiges JSON."));
        return;
    }

    const QString type = reply.value(QStringLiteral("type")).toString().trimmed();
    const QString message = repairMojibakeText(reply.value(QStringLiteral("message")).toString()).trimmed();
    if (type == QStringLiteral("ask_user")) {
        QStringList lines;
        const bool genericMessage = isGenericWorkflowAskUserText(message);
        if (!message.isEmpty() && !genericMessage) {
            lines << message;
        }
        const QJsonArray questions = reply.value(QStringLiteral("questions")).toArray();
        for (const QJsonValue& value : questions) {
            const QString question = repairMojibakeText(value.toString()).trimmed();
            if (!question.isEmpty()
                && !isGenericWorkflowAskUserText(question)
                && workflowTrainingQuestionNeedsAnswer(question, m_workflowRunState.slotValues)) {
                lines << QStringLiteral("- %1").arg(question);
            }
        }
        if (lines.isEmpty()) {
            appendBridgeLog("Workflow Step Repair: generische ask_user Rueckfrage ohne verwertbaren Inhalt abgelehnt");
            requestWorkflowStepRepair(
                userFeedback,
                QStringLiteral("type=ask_user ist unzureichend: Die Rueckfrage enthaelt nur einen Platzhalter oder keinen konkreten Inhalt. Liefere eine konkrete step_update-Korrektur oder stelle eine konkrete fachliche Frage mit Bezug auf den aktuellen Schritt, den BRX-Fehler und die vorhandenen effectiveTools. Schreibe niemals nur 'Kurze Rueckfrage'."));
            return;
        }

        const QString questionText = lines.join(QLatin1Char('\n'));
        const QString questionSignature = workflowTrainingSearchText(questionText).simplified().left(700);
        const bool repeatedQuestion = !questionSignature.isEmpty()
            && m_workflowRunState.repairAskSignatures.contains(questionSignature);
        const bool answeredQuestion = workflowStepRepairQuestionAlreadyAnswered(
            questionText,
            userFeedback,
            m_workflowRunState.repairDialog);
        if (repeatedQuestion || answeredQuestion) {
            appendBridgeLog(QString("Workflow Step Repair: ask_user Rueckfrage abgelehnt (%1)")
                .arg(repeatedQuestion ? QStringLiteral("wiederholt") : QStringLiteral("bereits beantwortet")));
            requestWorkflowStepRepair(
                userFeedback,
                QStringLiteral("type=ask_user ist unzureichend: Diese Rueckfrage wurde bereits gestellt oder durch den Nutzer beantwortet. Nutze den repairDialog verbindlich und liefere jetzt step_update. Wenn die gewünschte Rotation um den jeweiligen Solid-Mittelpunkt mit den vorhandenen Tools nicht abbildbar ist, liefere keine weitere Rueckfrage, sondern einen korrigierten unterstuetzten Schritt oder eine klare technische Begruendung im step_update-Kontext."));
            return;
        }

        if (!questionSignature.isEmpty()) {
            m_workflowRunState.repairAskSignatures << questionSignature;
            while (m_workflowRunState.repairAskSignatures.size() > 8) {
                m_workflowRunState.repairAskSignatures.removeFirst();
            }
        }
        m_workflowRunState.awaitingUser = true;
        m_workflowRunState.repairing = true;
        m_workflowRunState.repairDialog.append(QJsonObject{
            {"speaker", "assistant"},
            {"message", questionText},
        });
        appendAgentChat("AI", lines.join('\n'));
        return;
    }

    if (type == QStringLiteral("step_insert")
        || type == QStringLiteral("workflow_step_insert")
        || type == QStringLiteral("action_insert")) {
        const int index = m_workflowRunState.currentStepIndex;
        const QJsonObject currentAction = (index >= 0 && index < m_workflowRunState.actions.size())
            ? m_workflowRunState.actions.at(index).toObject()
            : QJsonObject{};
        const QJsonObject insertedStep = workflowInsertedStepFromRepairReply(reply, currentAction);
        if (insertedStep.isEmpty()) {
            requestWorkflowStepRepair(userFeedback, QStringLiteral("step_insert enthaelt keinen gueltigen Workflow-Schritt."));
            return;
        }
        validateAndExecuteWorkflowInsertedStep(
            insertedStep,
            workflowRepairInsertionBeforeCurrent(userFeedback, reply),
            userFeedback);
        return;
    }

    if (type != QStringLiteral("step_update")
        && type != QStringLiteral("workflow_step_update")
        && type != QStringLiteral("action_update")) {
        requestWorkflowStepRepair(userFeedback, QStringLiteral("Die AI muss mit type=ask_user, type=step_update oder type=step_insert antworten."));
        return;
    }

    const int index = m_workflowRunState.currentStepIndex;
    const QJsonObject currentAction = (index >= 0 && index < m_workflowRunState.actions.size())
        ? m_workflowRunState.actions.at(index).toObject()
        : QJsonObject{};
    const QJsonObject replacementStep = workflowStepFromRepairReply(reply, currentAction);
    if (replacementStep.isEmpty()) {
        requestWorkflowStepRepair(userFeedback, QStringLiteral("step_update enthaelt keinen gueltigen Workflow-Schritt."));
        return;
    }
    const QString replacementTool = replacementStep.value(QStringLiteral("tool")).toString().trimmed();
    const QString currentTool = currentAction.value(QStringLiteral("tool")).toString().trimmed();
    if (workflowRepairFeedbackRequestsStepInsertion(userFeedback)
        && !replacementTool.isEmpty()
        && !currentTool.isEmpty()
        && replacementTool != currentTool) {
        appendBridgeLog("Workflow Step Repair: step_update als step_insert behandelt, weil Nutzer einen zusaetzlichen Schritt verlangt hat");
        QJsonObject insertedStep = replacementStep;
        const QString oldId = currentAction.value(QStringLiteral("workflowStepId")).toString();
        if (workflowSlug(insertedStep.value(QStringLiteral("id")).toString()) == workflowSlug(oldId)) {
            insertedStep.insert(QStringLiteral("id"), QStringLiteral("insert_%1_before_%2")
                .arg(workflowSlug(replacementTool), oldId.isEmpty() ? QStringLiteral("current") : workflowSlug(oldId)));
        }
        validateAndExecuteWorkflowInsertedStep(insertedStep, true, userFeedback);
        return;
    }
    validateAndExecuteWorkflowRepairedStep(replacementStep, userFeedback);
}

void BricsCadPage::validateAndExecuteWorkflowRepairedStep(const QJsonObject& replacementStep, const QString& userFeedback)
{
    const int index = m_workflowRunState.currentStepIndex;
    if (!m_workflowRunState.active || index < 0 || index >= m_workflowRunState.actions.size()) {
        appendAgentChat("Barebone-Qt", "Workflow-Korrektur kann nicht ausgefuehrt werden: Schrittzustand fehlt.");
        return;
    }

    QString stepError;
    if (!validateWorkflowStepForTraining(replacementStep, index, stepError)) {
        requestWorkflowStepRepair(userFeedback, stepError);
        return;
    }

    const QJsonObject source = m_workflowRunState.actions.at(index).toObject().value(QStringLiteral("workflowStepSource")).toObject();
    QJsonObject candidateWorkflow = replaceWorkflowRunStep(m_workflowRunState.workingWorkflow, source, replacementStep);
    QString actionsError;
    QJsonArray candidateActions = workflowRunActions(candidateWorkflow, m_workflowRunState.slotValues, actionsError);
    if (candidateActions.isEmpty() || index >= candidateActions.size()) {
        requestWorkflowStepRepair(userFeedback, actionsError.isEmpty()
            ? QStringLiteral("Korrigierter Workflow erzeugt keine Aktion fuer den aktuellen Schritt.")
            : actionsError);
        return;
    }

    QJsonObject repairedAction = candidateActions.at(index).toObject();
    QJsonObject repairedParams = repairedAction.value(QStringLiteral("params")).toObject();
    if (candidateActions.size() > 1) {
        repairedParams.insert(QStringLiteral("saveBefore"), index == 0);
        repairedAction.insert(QStringLiteral("params"), repairedParams);
        candidateActions.replace(index, repairedAction);
    }
    const QString tool = repairedAction.value(QStringLiteral("tool")).toString();
    const QString bridgeMethod = bridgeMethodForTool(tool);

    QJsonArray preflightActions;
    preflightActions.append(QJsonObject{{"tool", tool}, {"params", repairedParams}});
    appendBridgeLog(QString("Workflow Step Repair: BRX prueft korrigierten Schritt %1/%2")
        .arg(index + 1)
        .arg(candidateActions.size()));
    setAgentBusy(true);
    const bool preflightQueued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        QJsonObject{{"source", "workflow_step_repair"}, {"actions", preflightActions}},
        15000,
        [this, candidateWorkflow, candidateActions, repairedAction, repairedParams, tool, bridgeMethod, index, userFeedback](const QJsonObject& response) mutable {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            const bool valid = response.value("ok").toBool(false) && result.value("valid").toBool(false);
            if (!valid) {
                setAgentBusy(false);
                const QString message = validationFailureMessage(response);
                appendBridgeLog(QString("Workflow Step Repair: BRX lehnt korrigierten Schritt ab: %1").arg(message.left(700).replace('\n', " | ")));
                requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Preflight lehnt die Korrektur ab: %1").arg(message));
                return;
            }

            appendBridgeLog(QString("Workflow Step Repair: BRX bestaetigt korrigierten Schritt %1/%2")
                .arg(index + 1)
                .arg(candidateActions.size()));
            const bool queued = sendBridgeRequest(
                bridgeMethod,
                repairedParams,
                30000,
                [this, candidateWorkflow, candidateActions, repairedAction, repairedParams, tool, bridgeMethod, index, userFeedback](const QJsonObject& runResponse) mutable {
                    appendJsonDebugLines(m_bridgeLog, runResponse.value("debug").toArray());
                    if (!runResponse.value("ok").toBool(false)) {
                        setAgentBusy(false);
                        const QString message = bridgeErrorMessage(runResponse, QStringLiteral("Korrigierter Workflow-Schritt fehlgeschlagen"));
                        appendBridgeLog(QString("Workflow Step Repair: Ausfuehrung fehlgeschlagen: %1").arg(message.left(700)));
                        requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Ausfuehrung der Korrektur fehlgeschlagen: %1").arg(message));
                        return;
                    }

                    setAgentBusy(false);
                    m_workflowRunState.workingWorkflow = candidateWorkflow;
                    m_workflowRunState.actions = candidateActions;
                    m_workflowRunState.proposal.insert(QStringLiteral("workflow"), candidateWorkflow);
                    m_workflowRunState.proposal.insert(QStringLiteral("actions"), candidateActions);
                    m_workflowRunState.repairAttemptCount = 0;
                    m_workflowRunState.repairing = false;

                    QJsonArray results = m_workflowRunState.results;
                    if (!results.isEmpty()
                        && results.last().toObject().value(QStringLiteral("index")).toInt() == index + 1) {
                        results.removeLast();
                    }
                    const QJsonObject stepResult{
                        {"index", index + 1},
                        {"tool", tool},
                        {"bridgeMethod", bridgeMethod},
                        {"params", repairedParams},
                        {"response", runResponse},
                        {"result", runResponse.value(QStringLiteral("result")).toObject()},
                        {"repaired", true},
                    };
                    results.append(stepResult);
                    pauseWorkflowRunAfterStep(m_workflowRunState.proposal, candidateActions, index, results, stepResult);
                });
            if (!queued) {
                setAgentBusy(false);
                requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Plugin ist nicht verbunden. Korrigierter Schritt wurde nicht gesendet."));
            }
        });
    if (!preflightQueued) {
        setAgentBusy(false);
        requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Preflight konnte fuer den korrigierten Schritt nicht gesendet werden."));
    }
}

void BricsCadPage::validateAndExecuteWorkflowInsertedStep(const QJsonObject& insertedStep, bool beforeCurrent, const QString& userFeedback)
{
    const int index = m_workflowRunState.currentStepIndex;
    if (!m_workflowRunState.active || index < 0 || index >= m_workflowRunState.actions.size()) {
        appendAgentChat("Barebone-Qt", "Workflow-Schritt kann nicht eingefuegt werden: Schrittzustand fehlt.");
        return;
    }

    QString stepError;
    if (!validateWorkflowStepForTraining(insertedStep, index, stepError)) {
        requestWorkflowStepRepair(userFeedback, stepError);
        return;
    }

    const QJsonObject source = m_workflowRunState.actions.at(index).toObject().value(QStringLiteral("workflowStepSource")).toObject();
    QJsonObject candidateWorkflow = source.isEmpty()
        ? insertWorkflowRunStepAtIndex(m_workflowRunState.workingWorkflow, beforeCurrent ? index : index + 1, insertedStep)
        : insertWorkflowRunStep(m_workflowRunState.workingWorkflow, source, insertedStep, beforeCurrent);

    QString actionsError;
    QJsonArray candidateActions = workflowRunActions(candidateWorkflow, m_workflowRunState.slotValues, actionsError);
    if (candidateActions.size() <= m_workflowRunState.actions.size()) {
        QJsonObject fallbackWorkflow = insertWorkflowRunStepAtIndex(m_workflowRunState.workingWorkflow, beforeCurrent ? index : index + 1, insertedStep);
        QString fallbackError;
        const QJsonArray fallbackActions = workflowRunActions(fallbackWorkflow, m_workflowRunState.slotValues, fallbackError);
        if (fallbackActions.size() > candidateActions.size()) {
            candidateWorkflow = fallbackWorkflow;
            candidateActions = fallbackActions;
            actionsError = fallbackError;
        }
    }
    if (candidateActions.isEmpty() || candidateActions.size() <= m_workflowRunState.actions.size()) {
        requestWorkflowStepRepair(userFeedback, actionsError.isEmpty()
            ? QStringLiteral("Eingefuegter Workflow-Schritt wurde nicht in die Aktionsliste uebernommen.")
            : actionsError);
        return;
    }

    const int insertedIndex = std::clamp(beforeCurrent ? index : index + 1, 0, static_cast<int>(candidateActions.size()) - 1);
    QJsonObject insertedAction = candidateActions.at(insertedIndex).toObject();
    QJsonObject insertedParams = insertedAction.value(QStringLiteral("params")).toObject();
    if (candidateActions.size() > 1) {
        insertedParams.insert(QStringLiteral("saveBefore"), insertedIndex == 0);
        insertedAction.insert(QStringLiteral("params"), insertedParams);
        candidateActions.replace(insertedIndex, insertedAction);
    }

    const QString tool = insertedAction.value(QStringLiteral("tool")).toString();
    const QString bridgeMethod = bridgeMethodForTool(tool);

    QJsonArray preflightActions;
    preflightActions.append(QJsonObject{{"tool", tool}, {"params", insertedParams}});
    appendBridgeLog(QString("Workflow Step Repair: BRX prueft eingefuegten Schritt %1/%2")
        .arg(insertedIndex + 1)
        .arg(candidateActions.size()));
    setAgentBusy(true);
    const bool preflightQueued = sendBridgeRequest(
        QStringLiteral("actions.validate"),
        QJsonObject{{"source", "workflow_step_insert"}, {"actions", preflightActions}},
        15000,
        [this, candidateWorkflow, candidateActions, insertedAction, insertedParams, tool, bridgeMethod, insertedIndex, beforeCurrent, userFeedback](const QJsonObject& response) mutable {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            const QJsonObject result = response.value("result").toObject();
            const bool valid = response.value("ok").toBool(false) && result.value("valid").toBool(false);
            if (!valid) {
                setAgentBusy(false);
                const QString message = validationFailureMessage(response);
                appendBridgeLog(QString("Workflow Step Repair: BRX lehnt eingefuegten Schritt ab: %1").arg(message.left(700).replace('\n', " | ")));
                requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Preflight lehnt den eingefuegten Schritt ab: %1").arg(message));
                return;
            }

            appendBridgeLog(QString("Workflow Step Repair: BRX bestaetigt eingefuegten Schritt %1/%2")
                .arg(insertedIndex + 1)
                .arg(candidateActions.size()));
            const bool queued = sendBridgeRequest(
                bridgeMethod,
                insertedParams,
                30000,
                [this, candidateWorkflow, candidateActions, insertedAction, insertedParams, tool, bridgeMethod, insertedIndex, beforeCurrent, userFeedback](const QJsonObject& runResponse) mutable {
                    appendJsonDebugLines(m_bridgeLog, runResponse.value("debug").toArray());
                    if (!runResponse.value("ok").toBool(false)) {
                        setAgentBusy(false);
                        const QString message = bridgeErrorMessage(runResponse, QStringLiteral("Eingefuegter Workflow-Schritt fehlgeschlagen"));
                        appendBridgeLog(QString("Workflow Step Repair: Ausfuehrung eingefuegter Schritt fehlgeschlagen: %1").arg(message.left(700)));
                        requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Ausfuehrung des eingefuegten Schritts fehlgeschlagen: %1").arg(message));
                        return;
                    }

                    setAgentBusy(false);
                    m_workflowRunState.workingWorkflow = candidateWorkflow;
                    m_workflowRunState.actions = candidateActions;
                    m_workflowRunState.proposal.insert(QStringLiteral("workflow"), candidateWorkflow);
                    m_workflowRunState.proposal.insert(QStringLiteral("actions"), candidateActions);
                    m_workflowRunState.repairAttemptCount = 0;
                    m_workflowRunState.repairing = false;

                    QJsonArray results = m_workflowRunState.results;
                    if (beforeCurrent
                        && !results.isEmpty()
                        && results.last().toObject().value(QStringLiteral("index")).toInt() == m_workflowRunState.currentStepIndex + 1) {
                        results.removeLast();
                    }
                    const QJsonObject stepResult{
                        {"index", insertedIndex + 1},
                        {"tool", tool},
                        {"bridgeMethod", bridgeMethod},
                        {"params", insertedParams},
                        {"response", runResponse},
                        {"result", runResponse.value(QStringLiteral("result")).toObject()},
                        {"inserted", true},
                    };
                    results.append(stepResult);
                    pauseWorkflowRunAfterStep(m_workflowRunState.proposal, candidateActions, insertedIndex, results, stepResult);
                });
            if (!queued) {
                setAgentBusy(false);
                requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Plugin ist nicht verbunden. Eingefuegter Schritt wurde nicht gesendet."));
            }
        });
    if (!preflightQueued) {
        setAgentBusy(false);
        requestWorkflowStepRepair(userFeedback, QStringLiteral("BRX Preflight konnte fuer den eingefuegten Schritt nicht gesendet werden."));
    }
}

void BricsCadPage::pauseWorkflowRunAfterStep(
    const QJsonObject& proposal,
    const QJsonArray& actions,
    int index,
    const QJsonArray& results,
    const QJsonObject& stepResponse)
{
    const bool stepFailed = !stepResponse.value("error").toString().trimmed().isEmpty()
        || !stepResponse.value("response").toObject().value("ok").toBool(true);
    const QJsonObject action = (index >= 0 && index < actions.size())
        ? actions.at(index).toObject()
        : QJsonObject{};
    const QString stepTitle = repairMojibakeText(action.value("workflowStepTitle").toString(
        action.value("tool").toString())).trimmed();
    const QString tool = action.value("tool").toString().trimmed();
    const QJsonObject result = stepResponse.value("result").toObject();
    const QString completion = actionCompletionText(action, result).trimmed();
    const bool trainingDraftRun = m_workflowRunState.runKind == QStringLiteral("training_draft")
        || proposal.value(QStringLiteral("runKind")).toString() == QStringLiteral("training_draft");
    const bool agentWorkflowRun = m_workflowRunState.runKind == QStringLiteral("agent_workflow")
        || proposal.value(QStringLiteral("runKind")).toString() == QStringLiteral("agent_workflow");

    m_workflowRunState.active = true;
    m_workflowRunState.awaitingUser = true;
    m_workflowRunState.proposal = proposal;
    m_workflowRunState.actions = actions;
    m_workflowRunState.results = results;
    m_workflowRunState.nextIndex = index + 1;
    m_workflowRunState.currentStepIndex = index;
    m_workflowRunState.repairing = false;
    m_workflowRunState.lastStepResponse = stepResponse;

    setAgentBusy(false);
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [payload = QVariantMap{
            {"step", index + 1},
            {"total", actions.size()},
            {"status", stepFailed ? "failed_waiting_user" : "waiting_user"},
            {"tool", tool},
        }](AiWebBridge* target) {
            Q_EMIT target->workflowRunProgress(payload);
        });
    }

    appendBridgeLog(QString("Workflow Run: Schritt %1/%2 wartet auf Nutzerfeedback")
        .arg(index + 1)
        .arg(actions.size()));

    QStringList lines;
    if (stepFailed) {
        const QString error = repairMojibakeText(stepResponse.value("error").toString()).trimmed();
        lines << QString("Workflow-Schritt %1/%2 wurde gestoppt: %3")
            .arg(index + 1)
            .arg(actions.size())
            .arg(stepTitle.isEmpty() ? tool : stepTitle);
        lines << QString("BRX meldet: %1").arg(error.isEmpty() ? QStringLiteral("unbekannter Fehler") : error);
        lines << QStringLiteral("Ich gehe nicht zum naechsten Schritt weiter.");
        lines << QStringLiteral("Antworte mit einer Korrektur oder mit \"Workflow bearbeiten\", damit ich genau diesen Schritt im Workflow-Reparaturmodus ueberarbeite.");
    } else {
        lines << QString("Workflow-Schritt %1/%2 ausgefuehrt: %3")
            .arg(index + 1)
            .arg(actions.size())
            .arg(stepTitle.isEmpty() ? tool : stepTitle);
        if (!completion.isEmpty()) {
            lines << completion;
        }
        if (index + 1 >= actions.size()) {
            lines << (trainingDraftRun
                ? QStringLiteral("Hat der letzte Schritt in BricsCAD gepasst? Antworte exakt mit \"ja\", dann uebernehme ich den Schritt in den Entwurf und biete danach Speichern an. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")
                : (agentWorkflowRun
                    ? QStringLiteral("Hat der letzte Schritt in BricsCAD gepasst? Antworte exakt mit \"ja\", dann schliesse ich den Workflow-Lauf ab. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")
                    : QStringLiteral("Hat der letzte Schritt in BricsCAD gepasst? Antworte exakt mit \"ja\", dann speichere ich den Schritt und schliesse den Workflow-Lauf ab. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")));
        } else {
            lines << (trainingDraftRun
                ? QStringLiteral("Hat dieser Schritt in BricsCAD geklappt? Antworte exakt mit \"ja\" fuer den naechsten Schritt; der Schritt wird nur im Entwurf uebernommen. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")
                : (agentWorkflowRun
                    ? QStringLiteral("Hat dieser Schritt in BricsCAD geklappt? Antworte exakt mit \"ja\" fuer den naechsten Schritt. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")
                    : QStringLiteral("Hat dieser Schritt in BricsCAD geklappt? Antworte exakt mit \"ja\" fuer den naechsten Schritt. Jede andere Antwort nutze ich als Korrekturkontext fuer diesen Schritt.")));
        }
    }
    appendAgentChat("AI", lines.join('\n'));
}

QJsonObject BricsCadPage::workflowWithRunValidationExample(QJsonObject workflow, const QJsonArray& acceptedActions) const
{
    if (!m_trainingSlotValues.isEmpty()) {
        workflow.insert(QStringLiteral("knownSlotValues"), m_trainingSlotValues);
    }
    QJsonArray actions;
    for (const QJsonValue& value : acceptedActions) {
        const QJsonObject action = value.toObject();
        const QString tool = action.value(QStringLiteral("tool")).toString().trimmed();
        const QJsonObject params = action.value(QStringLiteral("params")).toObject();
        if (!tool.isEmpty() && !params.isEmpty()) {
            actions.append(QJsonObject{{"tool", tool}, {"params", params}});
        }
    }
    if (!actions.isEmpty()) {
        QJsonArray examples = workflow.value(QStringLiteral("validationExamples")).toArray();
        QJsonObject example{
            {"title", QStringLiteral("Aus erfolgreichem Trainingslauf")},
            {"actions", actions},
        };
        const QString actionsSignature = QString::fromUtf8(
            QJsonDocument(QJsonObject{{"actions", actions}}).toJson(QJsonDocument::Compact));
        bool hasSameActions = false;
        int trainingRunExampleIndex = -1;
        for (int i = 0; i < examples.size(); ++i) {
            const QJsonObject existing = examples.at(i).toObject();
            const QString existingSignature = QString::fromUtf8(
                QJsonDocument(QJsonObject{{"actions", existing.value(QStringLiteral("actions")).toArray()}})
                    .toJson(QJsonDocument::Compact));
            if (existingSignature == actionsSignature) {
                hasSameActions = true;
            }
            if (existing.value(QStringLiteral("title")).toString() == QStringLiteral("Aus erfolgreichem Trainingslauf")) {
                trainingRunExampleIndex = i;
            }
        }
        if (!hasSameActions) {
            if (trainingRunExampleIndex >= 0) {
                examples.replace(trainingRunExampleIndex, example);
            } else {
                examples.append(example);
            }
        }
        workflow.insert(QStringLiteral("validationExamples"), examples);
    }
    return workflow;
}

bool BricsCadPage::checkpointTrainingDraftWorkflowRunStep(QString* errorMessage)
{
    if (!m_workflowRunState.active || m_workflowRunState.workingWorkflow.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Kein aktiver Trainingsentwurf-Lauf vorhanden.");
        }
        return false;
    }

    QJsonObject workflow = m_workflowRunState.workingWorkflow;
    workflow.insert(QStringLiteral("knownSlotValues"), m_workflowRunState.slotValues);
    m_workflowRunState.draftCheckpointWorkflow = workflow;
    m_trainingWorkflowContext = workflow;
    m_trainingSlotValues = m_workflowRunState.slotValues;
    m_trainingPendingRunWorkflow = workflow;
    m_trainingPhase = QStringLiteral("testing");
    appendBridgeLog("Workflow Training Run: Schritt im Entwurf zwischengespeichert");
    return true;
}

bool BricsCadPage::handleWorkflowRunUserResponse(const QString& prompt)
{
    if (!m_workflowRunState.active || !m_workflowRunState.awaitingUser) {
        return false;
    }

    const QString normalized = repairMojibakeText(prompt).trimmed().toLower();
    if (handleWorkflowRunStepRemoval(prompt)) {
        return true;
    }

    if (m_workflowRunState.repairing) {
        m_workflowRunState.repairDialog.append(QJsonObject{
            {"speaker", "user"},
            {"message", prompt},
        });
        requestWorkflowStepRepair(prompt);
        return true;
    }

    const bool lastStepFailed = !m_workflowRunState.lastStepResponse.value("error").toString().trimmed().isEmpty()
        || !m_workflowRunState.lastStepResponse.value("response").toObject().value("ok").toBool(true);
    const bool exactYes = normalized == QStringLiteral("ja");

    if (exactYes && !lastStepFailed) {
        QString saveError;
        const bool trainingDraftRun = m_workflowRunState.runKind == QStringLiteral("training_draft");
        const bool persistWorkflowRun = workflowRunShouldPersistWorkingWorkflow();
        if (trainingDraftRun) {
            if (!checkpointTrainingDraftWorkflowRunStep(&saveError)) {
                appendAgentChat("Barebone-Qt", QString("Workflow-Schritt wurde ausgefuehrt, aber nicht im Entwurf uebernommen: %1").arg(saveError));
                return true;
            }
        } else if (persistWorkflowRun) {
            if (!saveWorkflowRunWorkingWorkflow(&saveError)) {
                appendAgentChat("Barebone-Qt", QString("Workflow-Schritt wurde ausgefuehrt, aber nicht gespeichert: %1").arg(saveError));
                return true;
            }
        }
        m_workflowRunState.acceptedStepIndexes.append(m_workflowRunState.currentStepIndex + 1);
        const QJsonObject acceptedAction = (m_workflowRunState.currentStepIndex >= 0
                && m_workflowRunState.currentStepIndex < m_workflowRunState.actions.size())
            ? m_workflowRunState.actions.at(m_workflowRunState.currentStepIndex).toObject()
            : QJsonObject{};
        if (!acceptedAction.isEmpty()) {
            m_workflowRunState.acceptedConcreteActions.append(QJsonObject{
                {"tool", acceptedAction.value(QStringLiteral("tool")).toString()},
                {"params", m_workflowRunState.lastStepResponse.value(QStringLiteral("params")).toObject(acceptedAction.value(QStringLiteral("params")).toObject())},
            });
        }
        QJsonObject proposal = m_workflowRunState.proposal;
        QJsonArray actions = m_workflowRunState.actions;
        QJsonArray results = m_workflowRunState.results;
        const int nextIndex = m_workflowRunState.nextIndex;
        if (nextIndex >= actions.size()) {
            if (m_agentBridge) {
                emitToWebAsync(m_agentBridge, [payload = QVariantMap{
                    {"ok", true},
                    {"message", QStringLiteral("Workflow-Ausfuehrung abgeschlossen.")},
                }](AiWebBridge* target) {
                    Q_EMIT target->workflowRunFinished(payload);
                });
            }
            if (trainingDraftRun) {
                m_trainingFinalSavePending = true;
                m_trainingFinalSaveActions = m_workflowRunState.acceptedConcreteActions;
                m_trainingFinalSaveWorkflow = workflowWithRunValidationExample(
                    m_workflowRunState.workingWorkflow,
                    m_trainingFinalSaveActions);
                m_trainingWorkflowContext = m_trainingFinalSaveWorkflow;
                m_trainingSlotValues = m_workflowRunState.slotValues;
                m_trainingPhase = QStringLiteral("tested");
                QJsonObject finalWorkflow = m_trainingFinalSaveWorkflow;
                QJsonArray finalActions = m_trainingFinalSaveActions;
                clearWorkflowRunState();
                m_trainingFinalSavePending = true;
                m_trainingFinalSaveWorkflow = finalWorkflow;
                m_trainingFinalSaveActions = finalActions;
                setAgentBusy(false);
                appendAgentChat("AI", "Workflow-Test abgeschlossen. Alle Schritte wurden bestaetigt. Speichere den Workflow jetzt, wenn der Entwurf als Datei angelegt werden soll.");
                emitWorkflowTrainingFinalSavePrompt();
                return true;
            }
            clearWorkflowRunState();
            setAgentBusy(false);
            appendAgentChat("AI", persistWorkflowRun
                ? QStringLiteral("Workflow-Ausfuehrung abgeschlossen. Alle bestaetigten Schritte wurden gespeichert.")
                : QStringLiteral("Workflow-Ausfuehrung abgeschlossen. Alle bestaetigten Schritte wurden ausgefuehrt."));
            return true;
        }
        m_workflowRunState.awaitingUser = false;
        m_workflowRunState.repairing = false;
        m_workflowRunState.repairAttemptCount = 0;
        setAgentBusy(true);
        executeAgentActionBatch(proposal, actions, nextIndex, results);
        return true;
    }

    if (exactYes && lastStepFailed) {
        appendAgentChat("AI", "Der letzte Workflow-Schritt ist fehlgeschlagen. Ich kann ihn nicht mit \"ja\" akzeptieren. Bitte beschreibe die Korrektur.");
        return true;
    }

    QJsonObject stepState = m_workflowRunState.lastStepResponse;
    stepState.insert(QStringLiteral("userFeedback"), prompt);
    m_workflowRunState.lastStepResponse = stepState;
    m_workflowRunState.repairDialog = QJsonArray{
        QJsonObject{
            {"speaker", "user"},
            {"message", prompt},
        },
    };
    m_workflowRunState.repairAskSignatures.clear();
    requestWorkflowStepRepair(prompt);
    return true;
}

bool BricsCadPage::handleWorkflowRunStepRemoval(const QString& prompt)
{
    int targetIndex = -1;
    if (!workflowRunStepRemovalRequested(
            prompt,
            m_workflowRunState.currentStepIndex,
            m_workflowRunState.actions.size(),
            &targetIndex)) {
        return false;
    }

    if (targetIndex < 0 || targetIndex >= m_workflowRunState.actions.size()) {
        appendAgentChat("Barebone-Qt", QStringLiteral("Workflow-Schritt konnte nicht entfernt werden: Schrittindex ist ungueltig."));
        return true;
    }

    const QJsonObject removedAction = m_workflowRunState.actions.at(targetIndex).toObject();
    const QString removedTitle = repairMojibakeText(removedAction.value(QStringLiteral("workflowStepTitle")).toString(
        removedAction.value(QStringLiteral("tool")).toString())).trimmed();
    const QJsonObject source = removedAction.value(QStringLiteral("workflowStepSource")).toObject();
    QJsonObject updatedWorkflow = source.isEmpty()
        ? removeWorkflowRunStepAtIndex(m_workflowRunState.workingWorkflow, targetIndex)
        : removeWorkflowRunStep(m_workflowRunState.workingWorkflow, source);

    QString actionsError;
    QJsonArray updatedActions = workflowRunActions(updatedWorkflow, m_workflowRunState.slotValues, actionsError);
    if (updatedActions.size() >= m_workflowRunState.actions.size()) {
        QJsonObject fallbackWorkflow = removeWorkflowRunStepAtIndex(m_workflowRunState.workingWorkflow, targetIndex);
        QString fallbackError;
        const QJsonArray fallbackActions = workflowRunActions(fallbackWorkflow, m_workflowRunState.slotValues, fallbackError);
        if (!fallbackActions.isEmpty() && fallbackActions.size() < updatedActions.size()) {
            updatedWorkflow = fallbackWorkflow;
            updatedActions = fallbackActions;
            actionsError = fallbackError;
        }
    }
    if (updatedActions.isEmpty() && !actionsError.isEmpty()) {
        appendAgentChat("Barebone-Qt", QString("Workflow-Schritt konnte nicht entfernt werden: %1").arg(actionsError));
        return true;
    }
    if (updatedActions.size() >= m_workflowRunState.actions.size()) {
        appendAgentChat("Barebone-Qt", QStringLiteral("Workflow-Schritt konnte nicht entfernt werden: Der Schritt wurde im Workflow nicht eindeutig gefunden."));
        return true;
    }

    QJsonArray updatedResults = m_workflowRunState.results;
    if (targetIndex >= 0 && targetIndex < updatedResults.size()) {
        updatedResults.removeAt(targetIndex);
    }
    QJsonArray updatedAcceptedActions;
    for (int i = 0; i < m_workflowRunState.acceptedConcreteActions.size(); ++i) {
        if (i != targetIndex) {
            updatedAcceptedActions.append(m_workflowRunState.acceptedConcreteActions.at(i));
        }
    }
    QJsonArray updatedAcceptedIndexes;
    for (const QJsonValue& value : m_workflowRunState.acceptedStepIndexes) {
        const int acceptedIndex = value.toInt();
        if (acceptedIndex == targetIndex + 1) {
            continue;
        }
        updatedAcceptedIndexes.append(acceptedIndex > targetIndex + 1 ? acceptedIndex - 1 : acceptedIndex);
    }

    m_workflowRunState.workingWorkflow = updatedWorkflow;
    m_workflowRunState.draftCheckpointWorkflow = updatedWorkflow;
    m_workflowRunState.actions = updatedActions;
    m_workflowRunState.results = updatedResults;
    QJsonObject updatedProposal = m_workflowRunState.proposal;
    updatedProposal.insert(QStringLiteral("actions"), updatedActions);
    updatedProposal.insert(QStringLiteral("workflow"), updatedWorkflow);
    m_workflowRunState.proposal = updatedProposal;
    m_workflowRunState.acceptedConcreteActions = updatedAcceptedActions;
    m_workflowRunState.acceptedStepIndexes = updatedAcceptedIndexes;
    m_workflowRunState.awaitingUser = false;
    m_workflowRunState.repairing = false;
    m_workflowRunState.repairAttemptCount = 0;
    m_workflowRunState.repairDialog = {};
    m_workflowRunState.repairAskSignatures.clear();

    const bool trainingDraftRun = m_workflowRunState.runKind == QStringLiteral("training_draft");
    const bool persistWorkflowRun = workflowRunShouldPersistWorkingWorkflow();
    QString saveError;
    if (trainingDraftRun) {
        if (!checkpointTrainingDraftWorkflowRunStep(&saveError)) {
            appendAgentChat("Barebone-Qt", QString("Workflow-Schritt wurde entfernt, aber der Entwurf konnte nicht aktualisiert werden: %1").arg(saveError));
            return true;
        }
    } else if (persistWorkflowRun) {
        if (!saveWorkflowRunWorkingWorkflow(&saveError)) {
            appendAgentChat("Barebone-Qt", QString("Workflow-Schritt wurde entfernt, aber der Workflow konnte nicht gespeichert werden: %1").arg(saveError));
            return true;
        }
    }

    appendBridgeLog(QString("Workflow Run: Schritt %1 entfernt: %2")
        .arg(targetIndex + 1)
        .arg(removedTitle.isEmpty() ? removedAction.value(QStringLiteral("tool")).toString() : removedTitle));

    const int nextIndex = std::min(targetIndex, static_cast<int>(updatedActions.size()));
    if (nextIndex >= updatedActions.size()) {
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [payload = QVariantMap{
                {"ok", true},
                {"message", QStringLiteral("Workflow-Schritt entfernt; Workflow-Lauf abgeschlossen.")},
            }](AiWebBridge* target) {
                Q_EMIT target->workflowRunFinished(payload);
            });
        }
        if (trainingDraftRun) {
            m_trainingFinalSavePending = true;
            m_trainingFinalSaveActions = m_workflowRunState.acceptedConcreteActions;
            m_trainingFinalSaveWorkflow = workflowWithRunValidationExample(
                m_workflowRunState.workingWorkflow,
                m_trainingFinalSaveActions);
            m_trainingWorkflowContext = m_trainingFinalSaveWorkflow;
            m_trainingSlotValues = m_workflowRunState.slotValues;
            m_trainingPhase = QStringLiteral("tested");
            QJsonObject finalWorkflow = m_trainingFinalSaveWorkflow;
            QJsonArray finalActions = m_trainingFinalSaveActions;
            clearWorkflowRunState();
            m_trainingFinalSavePending = true;
            m_trainingFinalSaveWorkflow = finalWorkflow;
            m_trainingFinalSaveActions = finalActions;
            setAgentBusy(false);
            appendAgentChat("AI", QString("Workflow-Schritt %1 wurde entfernt. Der Workflow-Test ist abgeschlossen; du kannst den bereinigten Entwurf jetzt speichern.")
                .arg(targetIndex + 1));
            emitWorkflowTrainingFinalSavePrompt();
            return true;
        }
        clearWorkflowRunState();
        setAgentBusy(false);
        appendAgentChat("AI", QString("Workflow-Schritt %1 wurde entfernt. Workflow-Ausfuehrung abgeschlossen.").arg(targetIndex + 1));
        return true;
    }

    appendAgentChat("AI", QString("Workflow-Schritt %1 wurde entfernt. Ich fahre mit dem naechsten verbleibenden Schritt fort.").arg(targetIndex + 1));
    setAgentBusy(true);
    executeAgentActionBatch(updatedProposal, updatedActions, nextIndex, updatedResults);
    return true;
}

void BricsCadPage::continueWorkflowStepReview(
    const QJsonObject& proposal,
    const QJsonArray& actions,
    int index,
    QJsonArray results,
    const QJsonObject& stepResponse)
{
    const bool stepFailed = !stepResponse.value("error").toString().trimmed().isEmpty()
        || !stepResponse.value("response").toObject().value("ok").toBool(true);
    const int nextIndex = index + 1;

    auto continueNext = [this, proposal, actions, index, results]() mutable {
        QTimer::singleShot(kAgentBatchActionDelayMs, this, [this, proposal, actions, index, results]() mutable {
            executeAgentActionBatch(proposal, actions, index + 1, results);
        });
    };

    auto stopWithFailure = [this, stepResponse](const QString& message) {
        setAgentBusy(false);
        appendAgentChat("Barebone-Qt", message.trimmed().isEmpty()
            ? QStringLiteral("Workflow-Ausfuehrung gestoppt.")
            : message);
        if (m_agentBridge) {
            emitToWebAsync(m_agentBridge, [payload = QVariantMap{
                {"ok", false},
                {"message", message},
                {"step", stepResponse.toVariantMap()},
            }](AiWebBridge* target) {
                Q_EMIT target->workflowRunFinished(payload);
            });
        }
    };

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid() || (officialProvider && m_config.aiApiKey().trimmed().isEmpty())) {
        if (stepFailed) {
            stopWithFailure(QStringLiteral("Workflow-Schritt %1/%2 ist fehlgeschlagen und konnte nicht durch die AI geprueft werden.")
                .arg(index + 1)
                .arg(actions.size()));
        } else {
            continueNext();
        }
        return;
    }

    QJsonArray remainingActions;
    const int actionCount = static_cast<int>(actions.size());
    for (int i = std::min(actionCount, nextIndex); i < actionCount; ++i) {
        remainingActions.append(actions.at(i));
    }

    const QJsonObject reviewRequest{
        {"schema", "barebone.workflow.step_review.request.v1"},
        {"workflow", selectedWorkflowSummary()},
        {"completedStep", stepResponse},
        {"stepIndex", index + 1},
        {"totalSteps", actions.size()},
        {"remainingActions", remainingActions},
        {"previousResults", results},
        {"allowedDecisions", QJsonArray{"continue", "ask_user", "repair_action", "repair_workflow", "stop_success", "stop_failed"}},
        {"responseShape", QJsonObject{
            {"schema", "barebone.workflow.step_review.response.v1"},
            {"decision", "continue|ask_user|repair_action|repair_workflow|stop_success|stop_failed"},
            {"message", "kurze deutsche Begruendung"},
            {"action", "nur bei repair_action: {tool, params}"},
            {"targetIndex", "optional 1-basierter Aktionsindex fuer repair_action"},
        }},
        {"policy", "Pruefe den BRX-Log kompakt. Bei Erfolg normalerweise continue. Bei Fehler repariere nur mit bekannten Tools und Parametern oder waehle repair_workflow."},
    };

    QJsonArray messages{
        QJsonObject{
            {"role", "system"},
            {"content",
                "Du pruefst einen einzelnen Workflow-Ausfuehrungsschritt fuer Barebone-Qt. "
                "Antworte ausschliesslich mit einem JSON-Objekt gemaess barebone.workflow.step_review.response.v1. "
                "Wenn der Schritt erfolgreich war und kein Risiko sichtbar ist, nutze decision=continue. "
                "Wenn BRX einen Syntax- oder Parameterfehler meldet und eine sichere Korrektur moeglich ist, nutze repair_action mit action. "
                "Wenn der Workflow selbst geaendert werden muss, nutze repair_workflow. "
                "Wenn Nutzerdaten fehlen, nutze ask_user."},
        },
        QJsonObject{
            {"role", "user"},
            {"content", QString::fromUtf8(QJsonDocument(reviewRequest).toJson(QJsonDocument::Compact))},
        },
    };

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", adjustedOutputTokenLimitForMessages(messages, 1024));
        QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort == QStringLiteral("high")) {
            reasoningEffort = QStringLiteral("medium");
        }
        if (reasoningEffort != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.1);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.1);
        payload.insert("max_tokens", adjustedOutputTokenLimitForMessages(messages, 1024));
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    appendBridgeLog(QString("Qt -> AI Workflow Step Review: step=%1/%2 failed=%3")
        .arg(index + 1)
        .arg(actions.size())
        .arg(stepFailed ? QStringLiteral("true") : QStringLiteral("false")));
    if (m_agentBridge) {
        emitToWebAsync(m_agentBridge, [payload = QVariantMap{
            {"step", index + 1},
            {"total", actions.size()},
            {"status", stepFailed ? "failed_review" : "review"},
            {"tool", stepResponse.value("tool").toString()},
        }](AiWebBridge* target) {
            Q_EMIT target->workflowRunProgress(payload);
        });
    }

    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, proposal, actions, index, results, stepResponse, stepFailed, continueNext, stopWithFailure, operationGeneration]() mutable {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendBridgeLog(QString("AI Workflow Step Review: Fehler %1").arg(reply->errorString()));
            reply->deleteLater();
            if (stepFailed) {
                stopWithFailure(QStringLiteral("Workflow-Schritt %1/%2 ist fehlgeschlagen: %3")
                    .arg(index + 1)
                    .arg(actions.size())
                    .arg(stepResponse.value("error").toString()));
            } else {
                continueNext();
            }
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendBridgeLog(QString("AI Workflow Step Review: ungueltige OpenAI Antwort %1").arg(parseError.errorString()));
            reply->deleteLater();
            if (stepFailed) {
                stopWithFailure(QStringLiteral("Workflow-Schritt %1/%2 ist fehlgeschlagen und konnte nicht geprueft werden.")
                    .arg(index + 1)
                    .arg(actions.size()));
            } else {
                continueNext();
            }
            return;
        }

        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        bool parsed = false;
        const QJsonObject decisionObject = jsonObjectFromAiContent(content, &parsed);
        if (!parsed) {
            appendBridgeLog(QString("AI Workflow Step Review: JSON konnte nicht gelesen werden: %1").arg(content.left(400)));
            reply->deleteLater();
            if (stepFailed) {
                stopWithFailure(QStringLiteral("Workflow-Schritt %1/%2 ist fehlgeschlagen: %3")
                    .arg(index + 1)
                    .arg(actions.size())
                    .arg(stepResponse.value("error").toString()));
            } else {
                continueNext();
            }
            return;
        }

        const QString decision = decisionObject.value("decision").toString(
            decisionObject.value("type").toString()).trimmed();
        const QString message = repairMojibakeText(decisionObject.value("message").toString()).trimmed();
        appendBridgeLog(QString("AI Workflow Step Review: decision=%1 message=%2")
            .arg(decision.isEmpty() ? QStringLiteral("<leer>") : decision,
                message.left(240)));

        reply->deleteLater();

        if (decision == QStringLiteral("continue")) {
            continueNext();
            return;
        }

        if (decision == QStringLiteral("repair_action")) {
            QJsonObject repairAction = decisionObject.value("action").toObject();
            if (repairAction.isEmpty()) {
                repairAction = decisionObject.value("repairedAction").toObject();
            }
            repairAction = normalizedAgentAction(repairAction);
            QString actionError;
            if (repairAction.isEmpty() || !validateAgentAction(repairAction, actionError)) {
                stopWithFailure(QStringLiteral("AI konnte den Workflow-Schritt nicht gueltig reparieren: %1")
                    .arg(actionError.isEmpty() ? QStringLiteral("Korrektur fehlt") : actionError));
                return;
            }

            int targetIndex = decisionObject.value("targetIndex").toInt(stepFailed ? index + 1 : index + 2) - 1;
            targetIndex = std::clamp(targetIndex, 0, static_cast<int>(actions.size()) - 1);
            QJsonArray repairedActions = actions;
            repairedActions.replace(targetIndex, repairAction);
            QJsonArray repairedResults = results;
            if (stepFailed && !repairedResults.isEmpty()) {
                repairedResults.removeLast();
            }
            appendBridgeLog(QString("Workflow Step Review: Aktion %1/%2 durch AI-Korrektur ersetzt")
                .arg(targetIndex + 1)
                .arg(actions.size()));
            executeAgentActionBatch(proposal, repairedActions, targetIndex, repairedResults);
            return;
        }

        if (decision == QStringLiteral("repair_workflow")) {
            startWorkflowRepairFromStep(proposal, stepResponse, message);
            return;
        }

        if (decision == QStringLiteral("ask_user")) {
            setAgentBusy(false);
            appendAgentChat("AI", message.isEmpty()
                ? QStringLiteral("Der Workflow braucht vor dem naechsten Schritt eine Entscheidung.")
                : message);
            return;
        }

        if (decision == QStringLiteral("stop_success")) {
            const QString fallback = message.isEmpty()
                ? QStringLiteral("Workflow-Ausfuehrung abgeschlossen.")
                : message;
            QJsonObject batchResult{
                {"schema", "barebone.qt.agent.batch.result.v1"},
                {"summary", fallback},
                {"actionsRequested", actions.size()},
                {"actionsCompleted", results.size()},
                {"failed", 0},
                {"executionStats", executionStatsForActions(actions, results)},
                {"results", results},
            };
            requestAgentExecutionSummary(proposal, actions, results, batchResult, fallback);
            return;
        }

        stopWithFailure(message.isEmpty()
            ? QStringLiteral("Workflow-Ausfuehrung wurde durch die AI gestoppt.")
            : message);
    });
}

void BricsCadPage::startWorkflowRepairFromStep(const QJsonObject& proposal, const QJsonObject& stepState, const QString& reason)
{
    Q_UNUSED(proposal);
    setAgentBusy(false);
    setTrainingMode(true);
    m_trainingWorkflowContext = m_selectedWorkflow;
    m_trainingSlotValues = m_selectedWorkflowSlotValues;
    m_trainingMissing = {};
    m_trainingConversation = {};
    m_trainingValidationRetries = 0;
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = true;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    m_trainingPhase = QStringLiteral("repairing");

    QJsonObject repairContext{
        {"reason", reason},
        {"failedStep", stepState},
        {"workflow", m_selectedWorkflow},
    };
    appendBridgeLog("Workflow Step Review: Workflow-Reparatur im Trainingsmodus gestartet");
    sendWorkflowTrainingPrompt(QStringLiteral(
        "Der ausgewaehlte Workflow ist bei einem Testlauf auffaellig geworden. "
        "Analysiere BRX-Log, Nutzerfeedback, fehlgeschlagenen Schritt und Workflow. "
        "Korrigiere nur den betroffenen Schritt und erhalte den restlichen Workflow. "
        "Der Nutzer will diesen Testfehler beheben; wenn die Korrektur eindeutig ist, antworte direkt mit type=workflow_update und einem vollstaendigen Workflow. "
        "Nutze workflow_draft nur, wenn fuer die Korrektur wirklich eine Rueckfrage noetig ist. "
        "Fuer step-by-step Workflows gilt: keine spaeteren Schritte mit leeren Runtime-Zielen wie lastExtruded speichern, wenn ein expliziter Selector oder selection.set sicherer ist. "
        "Die Korrektur wird danach von Qt validiert, gespeichert und spaeter erneut Schritt fuer Schritt getestet.\n%1")
        .arg(QString::fromUtf8(QJsonDocument(repairContext).toJson(QJsonDocument::Compact))), true);
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
    envelope.insert("type", "execution_summary");
    envelope.insert("completedProposal", proposal);
    envelope.insert("executedActions", actions);
    envelope.insert("toolResults", results);
    envelope.insert("batchResult", batchResult);
    envelope.insert("executionStats", batchResult.value("executionStats").toObject(executionStatsForActions(actions, results)));
    envelope.insert("fallbackSummary", fallbackSummary);
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
    envelope.insert("includeConversationHistory", true);
    envelope.insert("instruction",
        QStringLiteral("Erstelle aus completedProposal, executedActions, executionStats, toolResults und batchResult eine kurze Abschlussnachricht im ChatGPT-Stil. "
        "Antworte ausschliesslich mit genau einem JSON-Objekt: {\"type\":\"message\",\"message\":\"...\"}. "
        "%1 Schreibe natuerlich, maximal zwei kurze Saetze. "
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

    appendAgentChat("Barebone-Qt", QString("Setze mehrstufigen Ablauf fort: %1").arg(nextIntent));

    QJsonObject envelope = agentRequestEnvelope(
        nextIntent,
        m_lastDocumentContext,
        normalizedAgentRouteForMode(m_lastAgentRoute, nextIntent, m_lastDocumentContext, m_chatMode));
    envelope.insert("type", "tool_result");
    envelope.insert("nextIntent", nextIntent);
    envelope.insert("completedProposal", proposal);
    envelope.insert("toolResponse", response);
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, "tool_result");
}

void BricsCadPage::continueAgentAfterToolFailure(
    const QJsonObject& proposal,
    const QJsonObject& response,
    const QString& errorMessage)
{
    if (m_agentValidationRetries >= kMaxAgentValidationRetries) {
        appendBridgeLog(QString("AI Agent Loop: BRX-Fehlerkorrektur nach %1 Versuchen abgebrochen: %2")
            .arg(kMaxAgentValidationRetries)
            .arg(errorMessage));
        appendAgentChat("Barebone-Qt", QString("BRX-Fehler konnte nicht automatisch korrigiert werden: %1").arg(errorMessage));
        return;
    }

    ++m_agentValidationRetries;
    appendBridgeLog(QString("AI Agent Loop %1/%2: BRX-Ausfuehrung fehlgeschlagen, korrigiere Vorschlag: %3")
        .arg(m_agentValidationRetries)
        .arg(kMaxAgentValidationRetries)
        .arg(errorMessage.left(240)));
    m_lastAgentRoute = normalizedAgentRouteForMode(
        makeAgentRoute(QStringLiteral("validation_retry"), QStringLiteral("BRX-Fehlerkorrektur")),
        m_lastAgentUserPrompt,
        m_lastDocumentContext,
        m_chatMode);
    QStringList retryTools;
    for (const QJsonValue& value : agentProposalActions(proposal)) {
        const QString tool = value.toObject().value(QStringLiteral("tool")).toString().trimmed();
        if (!tool.isEmpty() && !retryTools.contains(tool)) {
            retryTools << tool;
        }
    }
    const QString failedTool = proposal.value(QStringLiteral("failedAction")).toObject()
        .value(QStringLiteral("tool")).toString().trimmed();
    if (!failedTool.isEmpty() && !retryTools.contains(failedTool)) {
        retryTools << failedTool;
    }
    if (!retryTools.isEmpty()) {
        m_lastAgentRoute.insert(QStringLiteral("selectedTools"), stringsToJsonArray(retryTools));
        m_lastAgentRoute.insert(QStringLiteral("toolSelectionAttempted"), true);
    }

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
    envelope.insert("tools", effectiveTools);
    envelope.insert("effectiveTools", effectiveTools);
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentValidationRetries},
        {"policy", "The user-confirmed tool call failed in BRX. Correct the next response instead of repeating the same proposal."},
    });
    envelope.insert("instruction",
        "Die bestaetigte Aktion wurde von BRX abgelehnt. Wiederhole nicht denselben Vorschlag. "
        "Nutze executionError, failedProposal, tools[].inputSchema und apiDoc.post, um params oder tool zu korrigieren. "
        "Wenn du eine korrigierte Aktion ausfuehren willst, antworte mit genau einem action_proposal. "
        "Nutze dabei keine direkten BricsCAD-DB-Schreibvorgaenge, keine AcDb-/LayerTable-/EntityTable-Mutationen und keine Pseudo-Tools; nur tools[].name ist erlaubt. "
        "Wenn echte Informationen fehlen, nutze ask_user. Wenn Zeichnungskontext fehlt, nutze context_request. "
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

    const QString visibleMessage = speaker == QStringLiteral("AI")
        ? repairMojibakeText(removeReasoningLeak(message))
        : repairMojibakeText(message);

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
    emitWebMessage(m_agentBridge, payload);
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
    const QString message = reply.value("message").toString();
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

    emitWebProposal(m_agentBridge, QVariantMap{
        {"title", "Warte auf deine Antwort"},
        {"summary", message.trimmed().isEmpty() ? QStringLiteral("Der Agent wartet auf weitere Angaben.") : message.trimmed()},
        {"details", lines.join("\n")},
        {"canRun", false},
    });
}

void BricsCadPage::setAgentProposal(const QJsonObject& proposal)
{
    m_pendingAgentProposal = proposal;
    m_pendingAgentDraft = {};

    const QJsonArray actions = agentProposalActions(proposal);
    const QString summary = proposal.value("summary").toString(
        proposal.value("message").toString()).trimmed();

    QStringList lines;
    if (actions.size() > 1) {
        const bool workflowRun = proposal.value("source").toString() == QStringLiteral("workflow_run");
        lines << QString("%1: %2 Aktionen").arg(workflowRun ? QStringLiteral("Workflow-Schritte") : QStringLiteral("Batch")).arg(actions.size());
        lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
        lines << (workflowRun
            ? QStringLiteral("Ausfuehrung: Schritt fuer Schritt; Qt wartet nach jedem BRX-Ergebnis auf deine Rueckmeldung")
            : QStringLiteral("Ausfuehrung: intern als Batch, zu BRX einzeln mit Einzel-Preflight"));
        lines << QString("Speichern: nur vor der ersten Aktion");
        if (!workflowRun) {
            lines << QString("Pause zwischen Aktionen: %1 ms").arg(kAgentBatchActionDelayMs);
        }
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
        const QJsonObject definition = toolDefinition(tool);
        const QString paramsText = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));

        lines << QString("Werkzeug: %1").arg(definition.value("title").toString(tool));
        lines << QString("Name: %1").arg(tool.isEmpty() ? QStringLiteral("<fehlt>") : tool);
        lines << QString("Kategorie: %1").arg(definition.value("category").toString("general"));
        lines << QString("Bridge: %1").arg(definition.value("bridgeMethod").toString(tool));
        lines << QString("Risiko: %1").arg(definition.value("risk").toString("modifiesDrawing"));
        lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
        lines << QString("Parameter: %1").arg(paramsText.isEmpty() ? QStringLiteral("{}") : paramsText);
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
        const bool workflowRun = proposal.value("source").toString() == QStringLiteral("workflow_run");
        emitWebProposal(m_agentBridge, QVariantMap{
            {"title", workflowRun ? "Workflow-Ausfuehrung bereit" : (actions.size() > 1 ? "AI Batch-Vorschlag bereit" : "AI Vorschlag bereit")},
            {"summary", summary.isEmpty() ? QStringLiteral("Der Agent hat eine BricsCAD-Aktion vorbereitet.") : summary},
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
}

void BricsCadPage::cancelCurrentOperation()
{
    ++m_operationGeneration;

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
    m_agentValidationRetries = 0;
    m_trainingValidationRetries = 0;
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    m_trainingRunPending = false;
    m_trainingFinalSavePending = false;
    m_trainingPendingRunWorkflow = {};
    m_trainingFinalSaveWorkflow = {};
    m_trainingFinalSaveActions = {};
    clearWorkflowTrainingPrompts();
    clearWorkflowRunState();
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
        || normalized == QStringLiteral("ausführen")
        || normalized == "mach"
        || normalized == "mach das"
        || normalized == "bestaetigen"
        || normalized == QStringLiteral("bestätigen");
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
    if (actions.size() > 1) {
        if (actions.size() > kMaxAgentBatchActions) {
            errorMessage = QString("Batch enthaelt %1 Aktionen, erlaubt sind maximal %2").arg(actions.size()).arg(kMaxAgentBatchActions);
            return false;
        }
        if (proposal.value("continueAfterSuccess").toBool(false)
            || !proposal.value("nextIntent").toString().trimmed().isEmpty()) {
            errorMessage = "Batch-Vorschlaege duerfen keine automatische Folgeausfuehrung mit continueAfterSuccess/nextIntent verwenden";
            return false;
        }
        for (int i = 0; i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
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
        }
        return true;
    }

    if (actions.size() == 1) {
        const QJsonObject action = actions.first().toObject();
        const QString nextIntent = proposal.value("nextIntent").toString().trimmed();
        const bool repeatedPrompt = QRegularExpression(QStringLiteral(R"(\b([2-9]|[1-9][0-9]+)\b)")).match(m_lastAgentUserPrompt).hasMatch()
            || textMentionsAny(m_lastAgentUserPrompt.toLower(), {"mehrere", "viele", "zehn", "zwei", "drei", "vier", "fuenf", "fünf", "sechs", "sieben", "acht", "neun"});
        const bool loopLikeNextIntent = textMentionsAny(nextIntent.toLower(), {"naechst", "nächst", "next", "weiter"});
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

    if (definition.value("kind").toString("action") != "action") {
        errorMessage = QString("\"%1\" ist keine ausfuehrbare Action").arg(tool);
        return false;
    }

    const QString bridgeMethod = definition.value("bridgeMethod").toString(tool);
    if (bridgeMethod.isEmpty()) {
        errorMessage = QString("%1 hat keine Bridge-Methode").arg(tool);
        return false;
    }

    if (!m_brxCapabilities.isEmpty()) {
        bool methodKnown = false;
        const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
        for (const QJsonValue& value : methods) {
            if (value.toObject().value("name").toString() == bridgeMethod) {
                methodKnown = true;
                break;
            }
        }
        if (!methodKnown) {
            errorMessage = QString("Bridge-Methode \"%1\" ist laut BRX Capabilities nicht verfuegbar").arg(bridgeMethod);
            return false;
        }
    }

    const QJsonObject params = paramsValue.toObject();
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
    if (tool == "geometry.move"
        && textMentionsAny(activePrompt, {"face", "flaeche", "fläche", "seite", "stirnseite", "verlaengern", "verlängern", "extend"})) {
        errorMessage = "geometry.move verschiebt ganze Entities und ist nicht fuer einzelnes Face-Verlaengern freigegeben. Nutze measurement.bbox plus Ersatz-Workflow oder melde fehlende Capability.";
        return false;
    }

    if (tool == "geometry.scale") {
        if (params.contains("xFactor") || params.contains("yFactor") || params.contains("zFactor")) {
            errorMessage = "geometry.scale ist nur fuer uniforme Skalierung mit factor freigegeben; xFactor/yFactor/zFactor sind nicht erlaubt. Fuer Verlaengern in einer Achse measurement.bbox plus Ersatz-Workflow verwenden.";
            return false;
        }
        const double factor = params.value("factor").toDouble(0.0);
        if (factor <= 0.0) {
            errorMessage = "geometry.scale braucht factor > 0";
            return false;
        }
    }

    if (tool == "bim.classify") {
        if (!promptAllowsBimWallClassification(activePrompt)) {
            errorMessage = "bim.classify ist nur erlaubt, wenn der Nutzer BIM/Klassifizierung verlangt oder eine klare architektonische Wandaufgabe mit Wandstaerke/Wandhoehe/Raumbezug beschreibt.";
            return false;
        }
        const QJsonObject selector = params.value("selector").toObject();
        if (selector.value("scope").toString().compare(QStringLiteral("currentSpace"), Qt::CaseInsensitive) == 0
            && !selector.contains(QStringLiteral("layer"))
            && !selector.contains(QStringLiteral("handles"))) {
            errorMessage = "bim.classify mit selector.scope=currentSpace braucht einen Layer- oder Handle-Filter, damit nicht zu breit klassifiziert wird.";
            return false;
        }
    }

    return validateToolParams(params, definition.value("inputSchema").toObject(), errorMessage);
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
        const QString name = normalizedBricsCadLayerName(layer.value("name").toString());
        if (name.isEmpty()) {
            errorMessage = QString("layers.ensureMany.layers[%1].name fehlt").arg(i);
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

QString BricsCadPage::aiChatCompletionContent(const QJsonObject& response, QString* reasoningText) const
{
    QStringList reasoningParts;
    QStringList finalParts;

    const auto appendUnique = [](QStringList& list, const QString& value) {
        const QString text = value.trimmed();
        if (!text.isEmpty() && !list.contains(text)) {
            list << text;
        }
    };

    const QJsonArray choices = response.value("choices").toArray();
    if (!choices.isEmpty()) {
        const QJsonObject firstChoice = choices.first().toObject();
        QString content = firstChoice.value("message").toObject().value("content").toString();
        if (content.isEmpty()) {
            content = firstChoice.value("text").toString();
        }
        if (!content.trimmed().isEmpty()) {
            return finalAiMessageSegment(content);
        }
    }

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

    if (reasoningText) {
        *reasoningText = reasoningParts.join("\n\n").trimmed();
    }

    if (!finalParts.isEmpty()) {
        return finalAiMessageSegment(finalParts.join("\n").trimmed());
    }

    const QString outputText = response.value("output_text").toString();
    if (!outputText.trimmed().isEmpty()) {
        return finalAiMessageSegment(outputText);
    }

    return {};
}

QJsonArray BricsCadPage::availableAgentTools() const
{
    if (!kAgentActionToolsEnabled) {
        return {};
    }

    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    if (methods.isEmpty()) {
        return {};
    }

    QJsonArray tools;
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value("kind").toString() != "action") {
            continue;
        }

        const QString name = method.value("name").toString();
        if (name.isEmpty()) {
            continue;
        }
        if (name == "layers.batch") {
            continue;
        }

        QJsonObject tool;
        tool.insert("name", name);
        tool.insert("title", name);
        tool.insert("description", method.value("description").toString());
        tool.insert("bridgeMethod", name);
        tool.insert("kind", method.value("kind").toString("action"));
        tool.insert("risk", method.value("risk").toString("modifiesDrawing"));
        tool.insert("category", method.value("category").toString("bridge"));
        tool.insert("resultSchema", method.value("resultSchema").toString());
        tool.insert("confirmationRequired", method.value("risk").toString() != "readOnly");
        tool.insert("inputSchema", method.value("paramsSchema").toObject(QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}));
        if (method.contains("apiDoc")) {
            tool.insert("apiDoc", method.value("apiDoc").toObject());
        }
        enrichAgentToolDefinition(tool);
        tools.append(tool);
    }

    tools.append(layersEnsureManyToolDefinition());
    return tools;
}

QJsonArray BricsCadPage::agentToolsByNames(const QStringList& names) const
{
    if (names.isEmpty()) {
        return {};
    }

    QSet<QString> wanted;
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty()) {
            wanted.insert(trimmed);
        }
    }
    if (wanted.isEmpty()) {
        return {};
    }

    QJsonArray selected;
    for (const QJsonValue& value : availableAgentTools()) {
        const QJsonObject tool = value.toObject();
        if (wanted.contains(tool.value(QStringLiteral("name")).toString())) {
            selected.append(tool);
        }
    }
    return selected;
}

QJsonArray BricsCadPage::compactToolSelectorList() const
{
    QJsonArray tools;
    for (const QJsonValue& value : availableAgentTools()) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }

        QJsonArray parameterNames;
        const QJsonObject properties = tool.value(QStringLiteral("inputSchema")).toObject()
            .value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            parameterNames.append(it.key());
        }

        QJsonObject compact{
            {"name", name},
            {"title", tool.value(QStringLiteral("title")).toString()},
            {"domain", tool.value(QStringLiteral("domain")).toString(tool.value(QStringLiteral("category")).toString())},
            {"summary", tool.value(QStringLiteral("summary")).toString(tool.value(QStringLiteral("policy")).toString()).left(240)},
            {"keywords", tool.value(QStringLiteral("keywords")).toArray()},
            {"parameters", parameterNames},
        };

        const QJsonArray hints = tool.value(QStringLiteral("agentHints")).toArray();
        if (!hints.isEmpty()) {
            QJsonArray compactHints;
            for (int i = 0; i < hints.size() && i < 3; ++i) {
                compactHints.append(hints.at(i).toString().left(180));
            }
            compact.insert(QStringLiteral("hints"), compactHints);
        }
        const QJsonArray unsupported = tool.value(QStringLiteral("unsupportedOperations")).toArray();
        if (!unsupported.isEmpty()) {
            compact.insert(QStringLiteral("unsupported"), unsupported);
        }
        tools.append(compact);
    }
    return tools;
}

QJsonArray BricsCadPage::availableAgentToolsForRoute(const QJsonObject& route, const QString& prompt) const
{
    if (!routeAllowsCadActions(route)) {
        return {};
    }

    const QJsonArray allTools = availableAgentTools();
    if (allTools.isEmpty()) {
        return {};
    }

    const QStringList selectedTools = jsonStringArrayToStringList(route.value(QStringLiteral("selectedTools")).toArray());
    if (!selectedTools.isEmpty()) {
        const QJsonArray selected = agentToolsByNames(selectedTools);
        if (!selected.isEmpty()) {
            return selected;
        }
    }

    const QString normalized = prompt.toLower();
    QStringList selectedWorkflowTools;
    for (const QJsonValue& value : m_selectedWorkflow.value("preferredTools").toArray()) {
        const QString tool = value.toString().trimmed();
        if (!tool.isEmpty() && !selectedWorkflowTools.contains(tool)) {
            selectedWorkflowTools << tool;
        }
    }
    for (const QJsonValue& workflowValue : selectedWorkflowObjectsForRoute(route)) {
        const QJsonObject workflow = workflowValue.toObject();
        for (const QString& tool : workflowToolNamesForSelector(workflow, 12)) {
            if (!tool.isEmpty() && !selectedWorkflowTools.contains(tool)) {
                selectedWorkflowTools << tool;
            }
        }
    }
    QStringList pendingProposalTools;
    for (const QJsonValue& value : agentProposalActions(m_pendingAgentProposal)) {
        const QString tool = value.toObject().value("tool").toString().trimmed();
        if (!tool.isEmpty() && !pendingProposalTools.contains(tool)) {
            pendingProposalTools << tool;
        }
    }
    const bool layerIntent = textMentionsAny(normalized, {
        QStringLiteral("layer"),
        QStringLiteral("ebene"),
        QStringLiteral("tga"),
        QStringLiteral("heizung"),
        QStringLiteral("sanit"),
        QStringLiteral("lueft"),
        QStringLiteral("lüft"),
        QStringLiteral("elektro"),
    });
    const bool geometryIntent = textMentionsAny(normalized, {
        QStringLiteral("geometrie"),
        QStringLiteral("kreis"),
        QStringLiteral("rechteck"),
        QStringLiteral("linie"),
        QStringLiteral("polyline"),
        QStringLiteral("wand"),
        QStringLiteral("solid"),
        QStringLiteral("bim"),
        QStringLiteral("klassifiz"),
        QStringLiteral("klassifizi"),
        QStringLiteral("box"),
        QStringLiteral("extrusion"),
        QStringLiteral("extrudi"),
        QStringLiteral("verschiebe"),
        QStringLiteral("kopiere"),
        QStringLiteral("rotiere"),
        QStringLiteral("skaliere"),
        QStringLiteral("loesche"),
        QStringLiteral("lösche"),
        QStringLiteral("auswahl"),
        QStringLiteral("selekt"),
    });
    const bool createIntent = geometryIntent && textMentionsAny(normalized, {
        QStringLiteral("zeichne"),
        QStringLiteral("zeichnen"),
        QStringLiteral("erstelle"),
        QStringLiteral("erzeuge"),
        QStringLiteral("anlegen"),
        QStringLiteral("neu"),
        QStringLiteral("wand"),
        QStringLiteral("waende"),
        QStringLiteral("wände"),
        QStringLiteral("rechteck"),
        QStringLiteral("linie"),
        QStringLiteral("kreis"),
        QStringLiteral("box"),
        QStringLiteral("raum"),
        QStringLiteral("grundriss"),
    });
    const bool extrudeIntent = geometryIntent && textMentionsAny(normalized, {
        QStringLiteral("extrudi"),
        QStringLiteral("hoehe"),
        QStringLiteral("höhe"),
        QStringLiteral("3d"),
        QStringLiteral("solid"),
        QStringLiteral("wandhoehe"),
        QStringLiteral("wandhöhe"),
    });
    const bool classifyIntent = geometryIntent && promptAllowsBimWallClassification(normalized);
    const bool selectionIntent = geometryIntent && textMentionsAny(normalized, {
        QStringLiteral("auswahl"),
        QStringLiteral("selekt"),
        QStringLiteral("alle"),
        QStringLiteral("layer"),
    });
    const bool moveIntent = geometryIntent && textMentionsAny(normalized, {QStringLiteral("verschiebe"), QStringLiteral("move")});
    const bool copyIntent = geometryIntent && textMentionsAny(normalized, {QStringLiteral("kopiere"), QStringLiteral("copy")});
    const bool rotateIntent = geometryIntent && textMentionsAny(normalized, {QStringLiteral("rotiere"), QStringLiteral("rotate")});
    const bool scaleIntent = geometryIntent && textMentionsAny(normalized, {QStringLiteral("skaliere"), QStringLiteral("scale")});
    const bool deleteIntent = geometryIntent && textMentionsAny(normalized, {QStringLiteral("loesche"), QStringLiteral("lösche"), QStringLiteral("delete")});
    const bool saveIntent = textMentionsAny(normalized, {QStringLiteral("speicher"), QStringLiteral("save")});

    if (!layerIntent && !geometryIntent && selectedWorkflowTools.isEmpty() && pendingProposalTools.isEmpty()) {
        return allTools;
    }

    QJsonArray filtered;
    for (const QJsonValue& value : allTools) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value("name").toString();
        bool include = false;
        if (layerIntent) {
            include = name.startsWith(QStringLiteral("layers."))
                || name == QStringLiteral("layers.ensureMany")
                || name == QStringLiteral("command.execute");
        }
        if (geometryIntent) {
            include = include
                || (createIntent && name == QStringLiteral("geometry.create"))
                || (moveIntent && name == QStringLiteral("geometry.move"))
                || (copyIntent && name == QStringLiteral("geometry.copy"))
                || (rotateIntent && name == QStringLiteral("geometry.rotate"))
                || (scaleIntent && name == QStringLiteral("geometry.scale"))
                || (deleteIntent && name == QStringLiteral("geometry.delete"))
                || (extrudeIntent && (name == QStringLiteral("rectangles.extrude") || name == QStringLiteral("profile.extrude")))
                || (classifyIntent && name == QStringLiteral("bim.classify"))
                || (selectionIntent && name.startsWith(QStringLiteral("selection.")))
                || name == QStringLiteral("command.execute");
        }
        if (saveIntent && name == QStringLiteral("document.save")) {
            include = true;
        }
        if (selectedWorkflowTools.contains(name)) {
            include = true;
        }
        if (pendingProposalTools.contains(name)) {
            include = true;
        }
        if (include) {
            filtered.append(tool);
        }
    }

    return filtered.isEmpty() ? allTools : filtered;
}

QJsonArray BricsCadPage::readOnlyMethodsForRoute(const QJsonObject& route) const
{
    if (!routeAllowsCadContext(route)) {
        return {};
    }
    if (!m_brxAuthenticated) {
        return {};
    }

    QJsonArray readOnlyMethods;
    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }
    if (!readOnlyMethods.isEmpty()) {
        return readOnlyMethods;
    }

    for (const QString& method : QStringList{
             QStringLiteral("actions.list"),
             QStringLiteral("layers.list"),
             QStringLiteral("geometry.query"),
             QStringLiteral("selection.describe"),
             QStringLiteral("entity.describe"),
             QStringLiteral("measurement.bbox"),
             QStringLiteral("measurement.length"),
             QStringLiteral("measurement.area")}) {
        readOnlyMethods.append(QJsonObject{{"name", method}, {"risk", "readOnly"}});
    }
    return readOnlyMethods;
}

QJsonObject BricsCadPage::layersEnsureManyToolDefinition() const
{
    QJsonObject tool{
        {"name", "layers.ensureMany"},
        {"title", "Mehrere Layer sicher anlegen"},
        {"description", "Qt-internes virtuelles Tool fuer kompakte Layer-Batches. Barebone-Qt expandiert layers[] vor Preflight und Ausfuehrung in einzelne layers.create-Aktionen."},
        {"bridgeMethod", "layers.create"},
        {"kind", "action"},
        {"risk", "modifiesDrawing"},
        {"category", "layer"},
        {"confirmationRequired", true},
        {"virtual", true},
        {"expandsTo", "layers.create[]"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"required", QJsonArray{"layers"}},
            {"properties", QJsonObject{
                {"layers", QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"required", QJsonArray{"name"}},
                        {"properties", QJsonObject{
                            {"name", QJsonObject{{"type", "string"}}},
                            {"colorIndex", QJsonObject{{"type", "number"}, {"minimum", 1}}},
                        }},
                    }},
                }},
                {"reason", QJsonObject{{"type", "string"}}},
            }},
        }},
        {"apiDoc", QJsonObject{
            {"method", "layers.ensureMany"},
            {"virtual", true},
            {"required", QJsonArray{"layers"}},
            {"bodySchema", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"layers", "array of {name, optional colorIndex}; max batch count applies"},
                    {"reason", "optional reason shown in proposal"},
                }},
            }},
            {"examples", QJsonArray{
                QJsonObject{{"layers", QJsonArray{
                    QJsonObject{{"name", "Heizung"}, {"colorIndex", 1}},
                    QJsonObject{{"name", "Sanitär"}, {"colorIndex", 5}},
                }}},
            }},
        }},
    };
    enrichAgentToolDefinition(tool);
    return tool;
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
        "commands.list",
        "layers.list",
        "geometry.query",
        "selection.describe",
        "entity.describe",
        "measurement.bbox",
        "measurement.length",
        "measurement.area",
    };

    if (method.isEmpty()) {
        return false;
    }

    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    if (!methods.isEmpty()) {
        for (const QJsonValue& value : methods) {
            const QJsonObject item = value.toObject();
            if (item.value("name").toString() == method) {
                return item.value("risk").toString() == "readOnly";
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
            const int methodCount = m_brxCapabilities.value("methods").toArray().size();
            const int commandCount = m_brxCapabilities.value("commands").toArray().size();
            const int toolCount = availableAgentTools().size();
            appendBridgeLog(QString("BRX -> Qt: %1 Capabilities, %2 Commands, %3 Action-Tools")
                .arg(methodCount)
                .arg(commandCount)
                .arg(toolCount));
            QStringList missingCatalogEntries;
            for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
                const QJsonObject method = value.toObject();
                const QString name = method.value("name").toString();
                if (method.value("kind").toString() == QStringLiteral("action")
                    && name != QStringLiteral("layers.batch")
                    && !brxToolCatalogContains(name)) {
                    missingCatalogEntries << name;
                }
            }
            if (!brxToolCatalogContains(QStringLiteral("layers.ensureMany"))) {
                missingCatalogEntries << QStringLiteral("layers.ensureMany");
            }
            if (!missingCatalogEntries.isEmpty()) {
                appendBridgeLog(QString("BRX Tool-Policy-Katalog: fehlende Eintraege fuer %1")
                    .arg(missingCatalogEntries.join(QStringLiteral(", "))));
            }
            emitCapabilitiesStatusToWeb();
            if (toolCount <= 0) {
                QStringList methodNames;
                for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
                    const QJsonObject method = value.toObject();
                    methodNames << QString("%1:%2")
                        .arg(method.value("name").toString("<leer>"),
                             method.value("kind").toString("<kind fehlt>"));
                }
                appendBridgeLog(QString("BRX Capabilities enthalten keine Action-Tools. Methoden=%1").arg(methodNames.join(", ")));
                appendAgentChat("Barebone-Qt", QString("BRX Toolliste enthaelt keine ausfuehrbaren Action-Tools. Methoden: %1").arg(methodNames.join(", ")));
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
    };
}

QJsonObject BricsCadPage::compactAgentStateSummary(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route) const
{
    QJsonArray recentMessages;
    const qsizetype start = std::max<qsizetype>(0, m_agentConversation.size() - 6);
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

    return QJsonObject{
        {"schema", "barebone.agent.compact-state.v1"},
        {"lastUserPromptPreview", repairMojibakeText(prompt).left(700)},
        {"route", route.value("route").toString()},
        {"conversationMessages", m_agentConversation.size()},
        {"recentMessages", recentMessages},
        {"documentContext", documentSummary},
        {"pendingProposalAvailable", !m_pendingAgentProposal.isEmpty()},
        {"pendingProposalActionCount", agentProposalActions(m_pendingAgentProposal).size()},
        {"pendingDraftAvailable", !m_pendingAgentDraft.isEmpty()},
        {"lastToolResultAvailable", !m_lastAgentToolResult.isEmpty()},
        {"lastToolResultSummary", m_lastAgentToolResult.value("summary").toString()},
        {"selectedWorkflows", workflowSummaries},
        {"compressionPolicy", "Nutze diese Zusammenfassung als Orientierung. Wenn Details fehlen, frage gezielt nach oder nutze erlaubte read-only Kontextmethoden statt lange Historie zu rekonstruieren."},
    };
}

QJsonObject capabilitySummary(const QJsonObject& capabilities)
{
    QJsonArray actionTools;
    QJsonArray readOnlyMethods;
    QJsonArray commands;
    const QJsonArray methods = capabilities.value(QStringLiteral("methods")).toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        const QString name = method.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        if (method.value(QStringLiteral("kind")).toString() == QStringLiteral("action")) {
            actionTools.append(name);
        } else if (method.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")) {
            readOnlyMethods.append(name);
        }
    }
    for (const QJsonValue& value : capabilities.value(QStringLiteral("commands")).toArray()) {
        const QJsonObject command = value.toObject();
        const QString name = command.value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            commands.append(name);
        }
    }
    return QJsonObject{
        {"methods", methods.size()},
        {"actionTools", actionTools},
        {"readOnlyMethods", readOnlyMethods},
        {"commands", commands},
    };
}

QJsonObject BricsCadPage::agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route) const
{
    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, sanitizedContext, m_chatMode);
    const QJsonArray tools = availableAgentToolsForRoute(normalizedRoute, prompt);
    const QJsonArray selectedWorkflows = selectedWorkflowObjectsForRoute(normalizedRoute);
    QJsonArray workflowCapsules;
    for (int i = 0; i < selectedWorkflows.size() && i < 3; ++i) {
        workflowCapsules.append(workflowCapsuleForAgent(selectedWorkflows.at(i).toObject(), i == 0));
    }
    const QJsonArray readOnlyMethods = readOnlyMethodsForRoute(normalizedRoute);
    const QStringList policyRefs = policyRefsForRoute(normalizedRoute, prompt, sanitizedContext);
    const QJsonObject modePolicy = modePolicyForMode(m_chatMode, normalizedRoute);
    const bool cadContextAllowed = modePolicy.value("cadContextAllowed").toBool(false);

    QJsonObject responseContract = agentResourceJsonObject(QStringLiteral(":/agent/contracts/response-v2.json"));
    if (responseContract.isEmpty()) {
        responseContract = agentResponseContractObject();
    }
    const QJsonObject routeRule = responseContract.value("routeRules").toObject()
        .value(normalizedRoute.value("route").toString()).toObject();
    if (!routeRule.isEmpty()) {
        responseContract.insert("activeRoute", normalizedRoute.value("route").toString());
        responseContract.insert("activeAllowedTypes", routeRule.value("allowedTypes").toArray());
    }
    if (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(normalizedRoute)) {
        responseContract = QJsonObject{
            {"schema", "barebone.general.response.v1"},
            {"format", "plain_text"},
            {"policy", QStringLiteral("%1 Antworte direkt als normale Chatantwort. Kein JSON-Objekt, kein Barebone-Agent-Antworttyp. Markdown ist erlaubt. Bei Berechnungen: zuerst Grundgleichung, dann Symbolerklärung, dann SI-Umrechnung, dann Rechenschritte mit Einheit an jeder Zahl und jedem Summanden, dann Ergebnis.")
                .arg(aiLanguageInstruction(m_config))},
        };
    }

    QString executionMode = QStringLiteral("message-only");
    if (normalizedRoute.value("route").toString() == QStringLiteral("document_qa")) {
        executionMode = QStringLiteral("document-qa");
    } else if (normalizedRoute.value("route").toString() == QStringLiteral("bricscad_question")) {
        executionMode = QStringLiteral("read-only-cad");
    } else if (routeAllowsCadActions(normalizedRoute)) {
        executionMode = QStringLiteral("confirmed-actions");
    }

    QJsonObject envelope;
    envelope.insert("schema", "barebone.agent.request.v1");
    envelope.insert("userPrompt", prompt);
    envelope.insert("route", normalizedRoute);
    envelope.insert("capabilityProfile", normalizedRoute.value("capabilityProfile").toString());
    envelope.insert("modePolicy", modePolicy);
    envelope.insert("includeConversationHistory", true);
    envelope.insert("cadContext", cadContextAllowed ? currentAgentContext() : QJsonObject{});
    envelope.insert("context", cadContextAllowed
        ? currentAgentContext()
        : QJsonObject{
            {"mode", m_chatMode},
            {"brxConnected", m_brxAuthenticated},
            {"capabilitiesLoaded", !m_brxCapabilities.isEmpty()},
            {"lastToolResultAvailable", !m_lastAgentToolResult.isEmpty()},
        });
    envelope.insert("policyRefs", stringsToJsonArray(policyRefs));
    envelope.insert("policyText", policyTextForRefs(policyRefs));
    envelope.insert("responseContract", responseContract);
    envelope.insert("documentContext", (!sanitizedContext.isEmpty() && routeAllowsDocumentContext(normalizedRoute))
        ? sanitizedContext
        : QJsonObject{});
    envelope.insert("compactState", compactAgentStateSummary(prompt, sanitizedContext, normalizedRoute));
    envelope.insert("capabilities", QJsonObject{});
    envelope.insert("capabilitySummary", (cadContextAllowed && m_brxAuthenticated)
        ? capabilitySummary(m_brxCapabilities)
        : QJsonObject{});
    envelope.insert("readOnlyMethods", readOnlyMethods);
    envelope.insert("geometryDataModel", cadContextAllowed
        ? QJsonObject{
            {"sourceOfTruth", "BRX readOnlyMethods"},
            {"recommendedFlow", QJsonArray{"actions.list", "layers.list", "geometry.query", "selection.describe", "entity.describe", "measurement.bbox", "measurement.length", "measurement.area"}},
            {"fields", QJsonArray{"handle", "type", "kind", "shape", "layer", "bounds", "geometry.vertices", "metrics.length", "metrics.area", "metrics.height", "metrics.volume"}},
            {"policy", "Use fetched geometry data to classify entities. Do not assume an action exists just because the user asks for it."},
        }
        : QJsonObject{});
    envelope.insert("operationLimits", QJsonObject{
        {"subentityFaceMove", QJsonObject{
            {"available", false},
            {"reason", "No confirmed action tool for selecting and transforming individual AcDb3dSolid faces is exposed yet."},
            {"policy", "Do not invent solid.face.move, face.move or use geometry.move for a single face. geometry.move moves whole entities."},
        }},
        {"directDatabaseWrites", QJsonObject{
            {"available", false},
            {"reason", "Direct BricsCAD database writes are disabled because DB write mutations have caused renderer instability."},
            {"policy", "Never propose AcDb writes, LayerTable writes, EntityTable writes, direct database mutation workflows, or pseudo tools for database writes. Use only tools[].name exposed by Qt."},
        }},
    });
    envelope.insert("actionToolsEnabled", kAgentActionToolsEnabled);
    envelope.insert("reasoning", QJsonObject{{"effort", normalizedReasoningEffort(m_reasoningEffort)}});
    envelope.insert("executionPolicy", QJsonObject{
        {"mode", executionMode},
        {"toolProposalAllowed", routeAllowsCadActions(normalizedRoute) && kAgentActionToolsEnabled && !tools.isEmpty()},
        {"cadActionsRequireBrx", modePolicy.value("cadActionsRequireBrx").toBool(false)},
        {"allowedResponseTypes", routeAllowedResponseTypes(normalizedRoute.value("route").toString(), !tools.isEmpty())},
        {"whenNoToolFits", "plan"},
        {"batchActionsAllowed", true},
        {"maxBatchActions", kMaxAgentBatchActions},
        {"batchDelayMs", kAgentBatchActionDelayMs},
        {"batchPolicy", "Use proposal.actions[] for independent repeated actions with known params. For multiple layer creates with names/colors, prefer the virtual Qt tool layers.ensureMany; Qt expands it into individual layers.create actions. Qt executes internal batches as individual BRX requests, waits for each BRX response, sets saveBefore=true only on the first action, and stops on the first failure. Do not use continueAfterSuccess for simple repetition."},
        {"preflightValidation", "Before user confirmation, Qt calls BRX actions.validate for the whole proposal. During internal batch execution Qt also calls BRX actions.validate for the current single action before sending it. If validation rejects a proposal, correct params/tool or ask_user for missing data; never repeat the same invalid proposal."},
        {"nativeCommandPolicy", "The agent may choose command.execute when a native BricsCAD command is the better fit. command.execute must contain exactly one complete command line from commands.list, no semicolon or newline, and is always validated by BRX actions.validate before user confirmation."},
        {"databaseWritePolicy", "Direct BricsCAD DB writes are forbidden. Proposals must use only tools[].name; never suggest AcDb, LayerTable, EntityTable, database mutation, or DB batch write operations."},
    });
    envelope.insert("effectiveTools", tools);
    envelope.insert("tools", tools);
    envelope.insert("pendingProposal", m_pendingAgentProposal);
    envelope.insert("pendingDraft", m_pendingAgentDraft);
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("selectedWorkflow", selectedWorkflowSummary());
    envelope.insert("selectedWorkflows", workflowCapsules);
    envelope.insert("workflowCapsules", workflowCapsules);
    envelope.insert("workflowSelectionPolicy", QJsonObject{
        {"source", "AI compact selector + active workflow chip"},
        {"selectedWorkflowCount", workflowCapsules.size()},
        {"policy", "Wenn workflowCapsules vorhanden sind, pruefe diese zuerst als bevorzugten kompakten Loesungskontext. Wenn ein Workflow direkt oder angepasst passt, antworte bevorzugt mit type=workflow_run_proposal und konkreten actions[]. Du darfst Slotwerte aendern, Schritte ueberschreiben/ueberspringen/einfuegen oder zusaetzliche effectiveTools verwenden, wenn stepPlan den Grund enthaelt. Wenn kein Workflow passt, nutze workflowUsage.decision=no_fit intern und plane mit effectiveTools; die no_fit-Begruendung gehoert nicht in die normale Chatnachricht."},
        {"workflowRunProposalRequiredWhenFit", routeAllowsCadActions(normalizedRoute) && !workflowCapsules.isEmpty()},
    });
    envelope.insert("conversationMode", "unified-agent-envelope");
    envelope.insert("expectedResponse", (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(normalizedRoute))
        ? QStringLiteral("plain-text-chat-message")
        : QStringLiteral("barebone-agent-json-v2-strict-object"));
    return envelope;
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

    if (route.value(QStringLiteral("workflowRun")).toBool(false)) {
        setAgentBusy(false);
        const QString queuedRunKind = route.value(QStringLiteral("workflowRunKind")).toString();
        if (queuedRunKind == QStringLiteral("training_draft")
            || queuedRunKind == QStringLiteral("training_context")) {
            prepareTrainingDraftWorkflowRun(prompt);
        } else {
            prepareSelectedWorkflowRun(prompt);
        }
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
    if (routeAllowsCadContext(route) && shouldPrefetchSelectionDescription(prompt)) {
        sendUnifiedAgentRequestWithPrefetchedContext(
            prompt,
            "selection.describe",
            QJsonObject{
                {"include", QJsonArray{"geometry"}},
                {"limit", 100},
            },
            m_lastDocumentContext,
            route);
        return;
    }
    sendUnifiedAgentRequest(prompt, m_lastDocumentContext, route);
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
        const QString event = message.value("event").toString();
        if (event == "debug") {
            appendBridgeLog(QString("BRX Debug: %1").arg(message.value("message").toString()));
        } else if (event == "selection.changed") {
            m_currentSelection = message.value("selection").toArray();
            appendBridgeLog(QString("BRX Auswahl: %1 Geometrien im AI Kontext").arg(m_currentSelection.size()));
        } else if (!event.isEmpty()) {
            appendBridgeLog(QString("BRX Event: %1").arg(event));
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

    const QString line = QString("[%1] BRX -> Qt: %2 Capabilities, %3 Commands, %4 Action-Tools")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
        .arg(methodCount)
        .arg(commandCount)
        .arg(toolCount);
    emitWebBridgeLog(m_agentBridge, line);
}

void BricsCadPage::resetAgentConversation()
{
    m_agentConversation = {};
    m_pendingAgentProposal = {};
    m_pendingAgentDraft = {};
    m_lastAgentToolResult = {};
    m_lastDocumentContext = {};
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
    });
}

QJsonArray BricsCadPage::conversationFromWebHistory(const QVariantList& history) const
{
    QJsonArray conversation;
    for (const QVariant& item : history) {
        const QVariantMap map = item.toMap();
        const QString speaker = map.value(QStringLiteral("speaker")).toString();
        const QString message = map.value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty()) {
            continue;
        }

        if (speaker == "Du") {
            conversation.append(QJsonObject{{"role", "user"}, {"content", message}});
        } else if (speaker == "AI") {
            conversation.append(QJsonObject{{"role", "assistant"}, {"content", message}});
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

    saveCurrentAgentSession();
    m_agentSessionId = normalizedSessionId;

    if (m_agentSessions.contains(m_agentSessionId)) {
        const AgentSessionState state = m_agentSessions.value(m_agentSessionId);
        m_agentConversation = state.conversation;
        m_pendingAgentProposal = state.pendingProposal;
        m_pendingAgentDraft = state.pendingDraft;
        m_lastAgentToolResult = state.lastToolResult;
        m_lastDocumentContext = {};
    } else {
        m_agentConversation = conversationFromWebHistory(history);
        m_pendingAgentProposal = {};
        m_pendingAgentDraft = {};
        m_lastAgentToolResult = {};
        m_lastDocumentContext = {};
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

