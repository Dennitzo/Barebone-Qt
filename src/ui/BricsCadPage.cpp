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

void emitWebMathFormattingRepairCompleted(AiWebBridge* bridge, QString messageId, int revision, QString markdown)
{
    emitToWebAsync(bridge, [messageId = std::move(messageId), revision, markdown = std::move(markdown)](AiWebBridge* target) {
        Q_EMIT target->mathFormattingRepairCompleted(messageId, revision, markdown);
    });
}

void emitWebMathFormattingRepairFailed(AiWebBridge* bridge, QString messageId, int revision, QString errorMessage)
{
    emitToWebAsync(bridge, [messageId = std::move(messageId), revision, errorMessage = std::move(errorMessage)](AiWebBridge* target) {
        Q_EMIT target->mathFormattingRepairFailed(messageId, revision, errorMessage);
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
constexpr int kFocusedContextAiTimeoutMs = 90 * 1000;
constexpr int kLocalAiUnreachablePollIntervalMs = 15 * 1000;
constexpr int kLocalAiReachablePollIntervalMs = 60 * 1000;
constexpr int kWorkflowTrainingAiTimeoutMs = kAiModelResponseTimeoutMs;
constexpr int kWorkflowTrainingOutputTokens = 16384;
constexpr int kWorkflowTrainingCompactOutputTokens = 8192;
constexpr qsizetype kMaxDocumentContextChars = 90000;
constexpr int kMaxMathFormattingRepairAttempts = 2;

QJsonObject katexFormattingContract()
{
    return QJsonObject{
        {"schema", "barebone.katex.formatting-contract.v1"},
        {"katexVersion", "0.16.10"},
        {"sourceOfTruth", "The WebView KaTeX runtime validates every candidate with renderToString(..., {throwOnError:true, strict:'error'})."},
        {"styleContract", QJsonArray{
            "Use \\(...\\) for inline math and \\[...\\] for display math.",
            "Do not emit raw LaTeX outside math delimiters.",
            "Group multi-character subscripts and superscripts with braces.",
            "Use \\mathrm{...} for units and descriptive indices inside formulas.",
            "Render units upright, never italic; wrap unit tokens and compound units such as m, s, Pa, kW or m^{3}/s with \\mathrm{...}.",
            "Use KaTeX-compatible operators and commands; rewrite unsupported LaTeX to supported KaTeX syntax.",
            "Use \\cdot for multiplication in formulas.",
            "Use \\dot{V} for volumetric flow rate or time derivatives with a dot above the symbol; never write V. or V\\.",
            "Keep table cell formulas as valid inline math and keep display formulas out of table cells.",
            "Keep list item formulas contiguous; do not split formulas into PDF-extracted symbol lines.",
            "Use valid KaTeX syntax for fractions, roots, sums, integrals, matrices, delimiters, cases and aligned multi-line formulas.",
            "Preserve all numbers, units, calculations and factual statements; repair formatting only.",
        }},
        {"diagnosticsPolicy", QJsonArray{
            "Use katexDiagnostics from the WebView as concrete repair targets.",
            "If diagnostics mention a parse error, rewrite the whole affected formula, not only the failing token.",
            "Return complete Markdown, not a fragment.",
        }},
        {"references", QJsonArray{
            "https://katex.org/docs/supported.html",
            "https://katex.org/docs/api",
            "https://katex.org/docs/options",
        }},
    };
}

QString katexFormattingInstructionText()
{
    return QStringLiteral(
        "Wenn du Markdown mit Formeln ausgibst, folge dem katexFormattingContract aus dem Envelope. "
        "Die Zielsyntax ist KaTeX 0.16.10: Inline-Formeln in \\(...\\), Display-Formeln in \\[...\\], "
        "mehrteilige Indizes/Exponenten geklammert, beschreibende Indizes und Einheiten mit \\mathrm{...}, "
        "Einheiten in Formeln immer aufrecht und nicht kursiv schreiben, also z.B. \\mathrm{m}, \\mathrm{s}, \\mathrm{kW} oder \\mathrm{m^{3}/s}; "
        "Multiplikation mit \\cdot, Volumenstrom und zeitliche Ableitungen mit Punkt oben als \\dot{V} statt V. oder V\\., keine rohe LaTeX-Ausgabe ausserhalb von Math-Delimitern. "
        "Aendere bei Formatierungsreparaturen keine fachlichen Inhalte, Zahlen oder Berechnungen.");
}

bool useResponsesApiForModel(const QString& model)
{
    static const QStringList reasoningModels{
        QStringLiteral("openai/gpt-oss-20b"),
        QStringLiteral("google/gemma-4-26b-a4b-qat"),
        QStringLiteral("google/gemma-4-31b-qat"),
    };
    return std::any_of(reasoningModels.cbegin(), reasoningModels.cend(), [&model](const QString& candidate) {
        return model.compare(candidate, Qt::CaseInsensitive) == 0;
    });
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

        normalized += QLatin1Char(' ');
    }

    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    normalized = normalized.trimmed();
    return normalized;
}

bool hasBricsCadSubjectTopicLayerStructure(const QString& name)
{
    return normalizedBricsCadLayerName(name).split(QLatin1Char(' '), Qt::SkipEmptyParts).size() >= 2;
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
            QStringLiteral("layers.create"),
            QStringLiteral("layers.ensureMany")}},
        {QStringLiteral("requiredSequence"), QJsonArray{
            QStringLiteral("1. Den exakt genannten Layer mit layers.create oder layers.ensureMany anlegen."),
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
        QStringLiteral("maße"),
        QStringLiteral("hoehe"),
        QStringLiteral("hÃ¶he"),
        QStringLiteral("breite"),
        QStringLiteral("tiefe"),
        QStringLiteral("laenge"),
        QStringLiteral("lÃ¤nge"),
        QStringLiteral("flaeche"),
        QStringLiteral("flÃ¤che"),
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
        QStringLiteral("ergÃ¤nz"),
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
        });
    return (dataSubject && dataVerb) || allObjects;
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
        QStringLiteral("bim wände"),
        QStringLiteral("bim-wände"),
        QStringLiteral("wand"),
        QStringLiteral("wände"),
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
        QStringLiteral(R"((?:^|[^\p{L}\d])[-+]?\d+(?:\.\d+)?\s*(?:°|grad|degree|degrees|deg)\b)"),
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

void enrichAgentToolDefinition(QJsonObject& tool, const QJsonObject& catalogEntry = {})
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
    const bool mentionsCadDataQuery = !explanatoryQuestion
        && promptRequestsCadDataQuery(normalized);
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
    if (mode == QStringLiteral("bricscad")
        && normalized.value("route").toString() == QStringLiteral("bricscad_question")
        && promptRequestsCadDataQuery(prompt)) {
        normalized.insert("route", QStringLiteral("bricscad_action"));
        normalized.insert("capabilityProfile", capabilityProfileForRoute(QStringLiteral("bricscad_action")));
        normalized.insert("reason", QString("%1; dataQueryPolicy=read-only CAD tools enabled")
            .arg(normalized.value("reason").toString(fallbackReason)));
    }
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
        return QJsonArray{"message", "context_request"};
    }
    if (route == QStringLiteral("document_qa")) {
        return QJsonArray{"message", "ask_user", "context_request"};
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
        QJsonArray types{"message", "ask_user", "context_request", "action_proposal"};
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
    const int inlineOpen = text.count(QStringLiteral("\\("));
    const int inlineClose = text.count(QStringLiteral("\\)"));
    const int displayOpen = text.count(QStringLiteral("\\["));
    const int displayClose = text.count(QStringLiteral("\\]"));
    if (inlineOpen != inlineClose || displayOpen != displayClose) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt unvollstaendige Math-Delimiter; nutze vollstaendige Inline- oder Display-Formeln.").arg(location);
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    int formulaFragmentLines = 0;
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.size() <= 6
            && line.contains(QRegularExpression(QStringLiteral(R"(^([=+\-*/()[\]{}]|\p{Greek}|[A-Za-z]|\d+|[∘°])+$)")))) {
            ++formulaFragmentLines;
        }
    }
    if (formulaFragmentLines >= 4) {
        return QStringLiteral("Formatierungsfehler: %1 enthaelt wahrscheinlich zeilenweise zerlegte mathematische Fragmente; rekonstruiere sie als zusammenhaengende KaTeX-kompatible Formeln.").arg(location);
    }
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
    const QString latexError = latexFormattingValidationError(text, location);
    if (!latexError.isEmpty()) {
        return latexError;
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
    const QVector<Column> columns{
        {QStringLiteral("Handle"), {QStringLiteral("handle")}},
        {QStringLiteral("Typ"), {QStringLiteral("type")}},
        {QStringLiteral("Layer"), {QStringLiteral("layer")}},
        {QStringLiteral("Breite X"), {QStringLiteral("dimensions"), QStringLiteral("widthX")}},
        {QStringLiteral("Tiefe Y"), {QStringLiteral("dimensions"), QStringLiteral("depthY")}},
        {QStringLiteral("Hoehe Z"), {QStringLiteral("dimensions"), QStringLiteral("heightZ")}},
        {QStringLiteral("Laenge"), {QStringLiteral("metrics"), QStringLiteral("length")}},
        {QStringLiteral("Flaeche"), {QStringLiteral("metrics"), QStringLiteral("area")}},
        {QStringLiteral("Volumen"), {QStringLiteral("metrics"), QStringLiteral("volume")}},
        {QStringLiteral("Hinweis"), {QStringLiteral("error")}},
    };

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
        lines << QStringLiteral("| %1 |").arg(QStringList{
            QStringLiteral("..."),
            QStringLiteral("%1 weitere Objekte").arg(objects.size() - maxRows),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
            QStringLiteral(""),
        }.join(QStringLiteral(" | ")));
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QString brxGeometryResultTablesMarkdown(const QJsonArray& results)
{
    QStringList blocks;
    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        const QString tool = item.value(QStringLiteral("tool")).toString();
        if (tool != QStringLiteral("geometry.query")
            && tool != QStringLiteral("selection.describe")
            && tool != QStringLiteral("entity.describe")
            && tool != QStringLiteral("measurement.bbox")) {
            continue;
        }

        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        const QJsonArray objects = result.value(QStringLiteral("objects")).toArray();
        if (objects.isEmpty()) {
            continue;
        }

        const int index = item.value(QStringLiteral("index")).toInt(blocks.size() + 1);
        const QString title = QStringLiteral("%1 Ergebnis %2 (%3 Objekte)")
            .arg(tool)
            .arg(index)
            .arg(result.value(QStringLiteral("count")).toInt(objects.size()));
        blocks << brxGeometryObjectsMarkdownTable(title, objects);
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
        QStringLiteral("classification"),
        QStringLiteral("bounds"),
        QStringLiteral("dimensions"),
        QStringLiteral("metrics"),
        QStringLiteral("ok"),
        QStringLiteral("success"),
        QStringLiteral("error"),
    };
    for (const QString& key : keys) {
        if (object.contains(key)) {
            compact.insert(key, object.value(key));
        }
    }
    return compact;
}

QJsonArray compactGeometryObjectsForAgent(const QJsonArray& objects, int maxCount = 80)
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
            || toolName == QStringLiteral("bim.classify")
            || toolName == QStringLiteral("selection.set")
            || toolName == QStringLiteral("layers.ensureMany");
    }
    if (mentionsGeometryQuery) {
        return toolName == QStringLiteral("geometry.query")
            || toolName == QStringLiteral("selection.describe")
            || toolName == QStringLiteral("entity.describe")
            || toolName == QStringLiteral("measurement.bbox")
            || toolName == QStringLiteral("selection.set");
    }

    return toolName == QStringLiteral("geometry.create")
        || toolName == QStringLiteral("geometry.query")
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
            {"schema", "barebone.agent.response.v2"},
            {"allowedTypes", QJsonArray{"message", "ask_user", "context_request", "action_proposal", "plan"}},
            {"required", QJsonArray{"schema", "type"}},
            {"sessionTitlePolicy", "Set sessionTitle as a top-level field on every response, never inside proposal, draft, metadata, or learningUpdate. Derive a short, specific German session title from compactState, userPrompt and the conversation focus; max 6 words; avoid generic titles."},
        }},
        {"sessionTitle", QJsonObject{
            {"recommended", true},
            {"maxWords", 6},
            {"policy", "Kurzer Sitzungsname aus komprimiertem Kontext; keine generischen Titel wie Neuer Chat, Allgemeiner Chat, Workflow oder Frage."},
        }},
        {"learningUpdate", QJsonObject{
            {"optional", true},
            {"bricscadOnly", true},
            {"policy", "Optionales Metadatum, kein Antworttyp. Kuratierte Workflows mit updateProtected=true oder source=canonical_building_workflow sind unveraenderlich. Die lokale AI darf nur eigene Workflows mit source=ai_runtime und updateProtected=false anlegen oder aktualisieren. Neue Workflows verwenden dieselbe kanonische Struktur wie brx-learning.json."},
            {"lessonFields", QJsonArray{"id", "title", "intent", "intentPatterns", "discipline", "dependsOn", "requiredArtifacts", "producesArtifacts", "requiredContext", "assumptions", "requiredSlots", "knownSlotValues", "derivedValues", "strategy", "executionBatches", "validationExamples", "knownFailures", "repairRules", "recommendedTools", "status", "source", "updateProtected"}},
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
            {"policy", "Legacy only for general workflow context. BricsCAD mode uses action_proposal plus brxLearningContext instead of workflow_run_proposal."},
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
    text.replace(QChar(0x00e4), QStringLiteral("ae"));
    text.replace(QChar(0x00f6), QStringLiteral("oe"));
    text.replace(QChar(0x00fc), QStringLiteral("ue"));
    text.replace(QChar(0x00df), QStringLiteral("ss"));
    text.replace(QChar(0x00b2), QStringLiteral("2"));
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

    if (params.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPointsFromLastQuery")).toBool(false)
        || params.value(QStringLiteral("autoPolylineHandlesFromBatch")).toBool(false)
        || params.value(QStringLiteral("autoPolylineHandlesFromLastQuery")).toBool(false)
        || params.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
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
    const QString normalized = workflowTrainingSearchText(prompt);
    return promptDescribesArchitecturalWalls(normalized)
        && textMentionsAny(normalized, {
               QStringLiteral("4 wand"),
               QStringLiteral("4 waende"),
               QStringLiteral("4 aussenwand"),
               QStringLiteral("4 aussenwaende"),
               QStringLiteral("4 wände"),
               QStringLiteral("vier wand"),
               QStringLiteral("vier waende"),
               QStringLiteral("vier aussenwand"),
               QStringLiteral("vier aussenwaende"),
               QStringLiteral("vier wände"),
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
        || tool == QStringLiteral("geometry.copy")
        || tool == QStringLiteral("geometry.rotate")
        || tool == QStringLiteral("geometry.scale")
        || tool == QStringLiteral("geometry.delete")
        || tool == QStringLiteral("entity.setLayer")
        || tool == QStringLiteral("entity.setName")
        || tool == QStringLiteral("rectangles.extrude")
        || tool == QStringLiteral("profile.extrude")
        || tool == QStringLiteral("bim.classify");
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

QJsonArray latestCreatedSolidHandlesFromBatchResults(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        const QString tool = item.value(QStringLiteral("tool")).toString();
        if (tool != QStringLiteral("rectangles.extrude")
            && tool != QStringLiteral("profile.extrude")) {
            continue;
        }

        QJsonArray handles = affectedHandlesFromBatchResultItem(item);
        const QString handle = item.value(QStringLiteral("result")).toObject().value(QStringLiteral("handle")).toString().trimmed();
        if (!handle.isEmpty()) {
            handles.append(handle);
        }
        handles = uniqueStringArray(handles);
        if (!handles.isEmpty()) {
            return handles;
        }
    }

    QJsonArray geometryCreateBoxHandles;
    for (const QJsonValue& value : previousResults) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("geometry.create")) {
            continue;
        }
        const QJsonObject result = item.value(QStringLiteral("result")).toObject();
        if (result.value(QStringLiteral("geometry")).toString().compare(QStringLiteral("box"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        for (const QJsonValue& handleValue : affectedHandlesFromBatchResultItem(item)) {
            geometryCreateBoxHandles.append(handleValue);
        }
        const QString handle = result.value(QStringLiteral("handle")).toString().trimmed();
        if (!handle.isEmpty()) {
            geometryCreateBoxHandles.append(handle);
        }
    }
    geometryCreateBoxHandles = uniqueStringArray(geometryCreateBoxHandles);
    if (!geometryCreateBoxHandles.isEmpty()) {
        return geometryCreateBoxHandles;
    }
    return {};
}

QJsonArray latestSelectionSetHandlesFromBatchResults(const QJsonArray& previousResults)
{
    for (int i = previousResults.size() - 1; i >= 0; --i) {
        const QJsonObject item = previousResults.at(i).toObject();
        if (item.value(QStringLiteral("tool")).toString() != QStringLiteral("selection.set")) {
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

    if ((tool == QStringLiteral("rectangles.extrude") || tool == QStringLiteral("profile.extrude"))
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

    QJsonArray handles = latestCreatedSolidHandlesFromBatchResults(previousResults);
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
    return action.value("tool").toString().trimmed() == QStringLiteral("selection.set")
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
    m_brxLearning.setProjectRootPath(bareboneProjectRootPath());
    QString learningError;
    m_brxLearning.load(&learningError);

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
    if (m_brxLearning.document().isEmpty()) {
        appendBridgeLog(QStringLiteral("BRX Learning: brx-learning.json konnte nicht geladen werden: %1").arg(learningError));
    } else {
        appendBridgeLog(QStringLiteral("BRX Learning: %1 Tools, %2 Lessons, %3 Repair-Regeln, Quelle=%4")
            .arg(m_brxLearning.metadata().value(QStringLiteral("toolProfileCount")).toInt(m_brxLearning.toolProfiles().size()))
            .arg(m_brxLearning.metadata().value(QStringLiteral("lessonCount")).toInt())
            .arg(m_brxLearning.metadata().value(QStringLiteral("repairRuleCount")).toInt())
            .arg(m_brxLearning.metadata().value(QStringLiteral("source")).toString()));
    }

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
    QObject::connect(m_agentBridge, &AiWebBridge::workflowTestRequested, this, [this](const QString& workflowId) {
        runBrxWorkflowTest(workflowId);
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
        appendAgentChat("Barebone-Qt", isChatWorkspace()
            ? QString("Workflow geloescht: %1").arg(deletedPath)
            : QString("Learning-Lesson deaktiviert. Gespeichert in: %1").arg(deletedPath));
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
    QObject::connect(m_agentBridge, &AiWebBridge::mathFormattingRepairRequested, this, [this](const QString& messageId, int revision, const QString& markdown, const QString& diagnosticsJson) {
        requestMathFormattingRepair(messageId, revision, markdown, diagnosticsJson);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::mathFormattingRepairAccepted, this, [this](const QString& messageId, int revision, const QString& markdown) {
        acceptMathFormattingRepair(messageId, revision, markdown);
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
        const int index = value.toInt(-1);
        if (index < 0 || index >= m_agentConversation.size() || seenIndexes.contains(index)) {
            continue;
        }
        seenIndexes.insert(index);
        relevantIndexes.append(index);
        if (relevantIndexes.size() >= 32) {
            break;
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

QJsonObject BricsCadPage::compactSessionHistoryMessage(int index, bool fullText, int maxChars) const
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

    const int boundedMaxChars = std::clamp(maxChars, 120, 4000);
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

QJsonArray BricsCadPage::sessionHistoryMessagesRange(int start, int count, bool fullText) const
{
    QJsonArray messages;
    if (m_agentConversation.isEmpty() || count <= 0) {
        return messages;
    }
    const int boundedStart = std::clamp(start, 0, static_cast<int>(m_agentConversation.size()) - 1);
    const int boundedCount = std::clamp(count, 1, 12);
    const int end = std::min(static_cast<int>(m_agentConversation.size()), boundedStart + boundedCount);
    for (int i = boundedStart; i < end; ++i) {
        const QJsonObject item = compactSessionHistoryMessage(i, fullText);
        if (!item.isEmpty()) {
            messages.append(item);
        }
    }
    return messages;
}

QJsonArray BricsCadPage::sessionHistoryMessagesForQuery(const QString& query, int limit, bool fullText) const
{
    QJsonArray messages;
    if (m_agentConversation.isEmpty()) {
        return messages;
    }

    QString normalizedQuery = repairMojibakeText(query).toLower();
    if (normalizedQuery.trimmed().isEmpty()) {
        normalizedQuery = repairMojibakeText(m_lastAgentUserPrompt).toLower();
    }
    normalizedQuery.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+"), QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(" "));
    const QStringList terms = normalizedQuery.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int boundedLimit = std::clamp(limit, 1, 8);

    QVector<QPair<double, int>> scored;
    for (int i = 0; i < m_agentConversation.size(); ++i) {
        QString content = compactSessionHistoryMessage(i, true, 3200).value(QStringLiteral("content")).toString().toLower();
        content.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+"), QRegularExpression::UseUnicodePropertiesOption), QStringLiteral(" "));
        double score = 0.0;
        if (!normalizedQuery.trimmed().isEmpty() && content.contains(normalizedQuery.trimmed())) {
            score += 10.0;
        }
        int acceptedTerms = 0;
        for (const QString& term : terms) {
            if (term.size() < 3) {
                continue;
            }
            ++acceptedTerms;
            if (content.contains(term)) {
                score += 2.0;
            }
            if (acceptedTerms >= 16) {
                break;
            }
        }
        if (score <= 0.0 && terms.isEmpty()) {
            score = 0.1;
        }
        if (score > 0.0) {
            const double recencyBoost = static_cast<double>(i + 1) / static_cast<double>(std::max<qsizetype>(1, m_agentConversation.size())) * 0.5;
            scored.append(qMakePair(score + recencyBoost, i));
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
        if (left.first == right.first) {
            return left.second > right.second;
        }
        return left.first > right.first;
    });

    for (const auto& candidate : scored) {
        const QJsonObject item = compactSessionHistoryMessage(candidate.second, fullText);
        if (!item.isEmpty()) {
            messages.append(item);
        }
        if (messages.size() >= boundedLimit) {
            break;
        }
    }
    return messages;
}

bool BricsCadPage::isSessionHistoryContextMethod(const QString& method) const
{
    return method == QStringLiteral("session.history.query")
        || method == QStringLiteral("session.history.range")
        || method == QStringLiteral("session.history.recent");
}

QJsonObject BricsCadPage::sessionHistoryContextResponse(const QString& method, const QJsonObject& params) const
{
    QJsonObject response{
        {"schema", "barebone.session.history.context-result.v1"},
        {"ok", true},
        {"method", method},
        {"messageCount", m_agentConversation.size()},
        {"indexesAreZeroBased", true},
    };

    if (method == QStringLiteral("session.history.recent")) {
        const int count = std::clamp(params.value(QStringLiteral("count")).toInt(6), 1, 10);
        const int start = std::max(0, static_cast<int>(m_agentConversation.size()) - count);
        response.insert(QStringLiteral("messages"), sessionHistoryMessagesRange(start, count, params.value(QStringLiteral("fullText")).toBool(true)));
        response.insert(QStringLiteral("limit"), 10);
        return response;
    }

    if (method == QStringLiteral("session.history.range")) {
        const int start = std::max(0, params.value(QStringLiteral("start")).toInt(0));
        int count = params.value(QStringLiteral("count")).toInt(-1);
        if (count <= 0 && params.contains(QStringLiteral("end"))) {
            count = params.value(QStringLiteral("end")).toInt(start) - start + 1;
        }
        count = std::clamp(count <= 0 ? 6 : count, 1, 12);
        response.insert(QStringLiteral("messages"), sessionHistoryMessagesRange(start, count, params.value(QStringLiteral("fullText")).toBool(true)));
        response.insert(QStringLiteral("limit"), 12);
        return response;
    }

    if (method == QStringLiteral("session.history.query")) {
        const QString query = params.value(QStringLiteral("query")).toString(m_lastAgentUserPrompt).trimmed();
        const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(8), 1, 8);
        response.insert(QStringLiteral("query"), query);
        response.insert(QStringLiteral("messages"), sessionHistoryMessagesForQuery(query, limit, params.value(QStringLiteral("fullText")).toBool(true)));
        response.insert(QStringLiteral("limit"), 8);
        return response;
    }

    response.insert(QStringLiteral("ok"), false);
    response.insert(QStringLiteral("error"), QStringLiteral("Unbekannte session.history Methode."));
    return response;
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
        {"recentMessages", recentCount > 0 ? sessionHistoryMessagesRange(recentStart, recentCount, true) : QJsonArray{}},
        {"topicIndex", topicIndex},
        {"allowedMethods", QJsonArray{
            QJsonObject{
                {"method", "session.history.query"},
                {"description", "Themensuche im bereinigten Sitzungsverlauf; maximal 8 Treffer."},
                {"params", QJsonObject{{"query", "string"}, {"limit", "1..8"}, {"fullText", "boolean optional"}}},
            },
            QJsonObject{
                {"method", "session.history.range"},
                {"description", "Konkreten Nachrichtenbereich nach Index laden; maximal 12 Nachrichten."},
                {"params", QJsonObject{{"start", "zero-based index"}, {"count", "1..12"}, {"fullText", "boolean optional"}}},
            },
            QJsonObject{
                {"method", "session.history.recent"},
                {"description", "Mehr unmittelbare Vorgaengernachrichten laden; maximal 10 Nachrichten."},
                {"params", QJsonObject{{"count", "1..10"}, {"fullText", "boolean optional"}}},
            },
        }},
        {"policy", "Wenn focusedConversationContext fuer Korrektur, Fehlersuche oder Verweise wie 'wie vorher' nicht reicht, antworte mit type=context_request und einer session.history.* Methode. Diese Methoden liefern nur bereinigte Chatnachrichten, keine Tools, Policies oder Systemprompts."},
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
        && usedTokens <= 0
        && m_lastContextBudgetUsedTokens > 0;
    if (preserveLastFetchedUsage) {
        usedTokens = m_lastContextBudgetUsedTokens;
    } else if (usedTokens > 0 || !m_agentBusy) {
        m_lastContextBudgetUsedTokens = usedTokens;
    }
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
        {"preservedWhileThinking", preserveLastFetchedUsage},
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
        appendBridgeLog("AI Agent: Prompt ignoriert, weil bereits eine Anfrage laeuft");
        appendAgentChat("Barebone-Qt", "Die AI verarbeitet noch eine Anfrage. Nutze Stoppen oder warte auf die Antwort.");
        return;
    }

    const QString prompt = promptText.trimmed();
    if (prompt.isEmpty()) {
        return;
    }

    const bool confirmsPendingProposal = isAgentConfirmation(prompt) && !m_pendingAgentProposal.isEmpty();
    if (isBricsCadMode() && !confirmsPendingProposal) {
        evaluatePendingBrxLearningFeedback(prompt);
    }

    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    appendAgentChat("Du", prompt);
    m_agentRejectedResponseSignatures.clear();

    if (confirmsPendingProposal) {
        executeAgentProposal();
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
    m_repeatedAgentContextRequestCount = 0;
    m_lastAgentContextRequestSignature.clear();
    m_agentContextLoopBlocked = false;
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
        const QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", QStringLiteral("low")}});
        }
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

    if (!normalizedRoute.value(QStringLiteral("conversationFocusAttempted")).toBool(false)) {
        requestFocusedConversationContext(
            prompt,
            documentContext,
            normalizedRoute,
            [this, prompt, documentContext, normalizedRoute](const QJsonObject& focusedContext) {
                m_lastFocusedConversationContext = focusedContext;
                QJsonObject focusedRoute = normalizedRoute;
                focusedRoute.insert(QStringLiteral("conversationFocusAttempted"), true);
                continueUnifiedAgentRequest(prompt, documentContext, focusedRoute);
            });
        return;
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

    if (isBricsCadMode()
        && routeAllowsCadContext(normalizedRoute)
        && m_brxAuthenticated
        && !normalizedRoute.value(QStringLiteral("drawingContextPrefetchAttempted")).toBool(false)) {
        QJsonObject drawingRoute = normalizedRoute;
        drawingRoute.insert(QStringLiteral("drawingContextPrefetchAttempted"), true);
        sendUnifiedAgentRequestWithDrawingContext(prompt, documentContext, drawingRoute);
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
    const QJsonArray plainLearningLessons = compactWorkflowSelectorList();
    if (plainTools.isEmpty() && plainLearningLessons.isEmpty()) {
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
        {"plainLearningLessons", plainLearningLessons},
        {"focusedConversationContext", m_lastFocusedConversationContext},
        {"brxLearningContext", QJsonObject{}},
        {"policy", "Waehle im BricsCAD-Modus relevante Lessons aus plainLearningLessons[].id, wenn eine Lesson die Nutzerabsicht ganz oder teilweise abdeckt. Nutze focusedConversationContext als promptbezogenen Verlaufskontext; der aktuelle Prompt bleibt massgeblich. Diese IDs sind Learning-Lessons, keine ausfuehrbaren Workflows. Waehle danach nur Toolnamen aus plainTools[].name; alle BRX-Capabilities in plainTools sind freigegebene Agent-Tools, inklusive read-only Diagnose/Context Tools und layers.batch. Antworte ohne Markdown mit genau einem JSON-Objekt. Maximal 12 Tools und maximal 3 Lessons; wenn die passende Auswahl unsicher ist, waehle eher die noetigen Diagnose-Tools wie capabilities.list, actions.list, layers.list, entity.describe oder actions.validate mit. Lessons sind nur Erfahrungswissen: uebernimm feste Filter wie layer=0, Handles, Namen oder Winkel nur, wenn der Nutzer sie im aktuellen Prompt nennt. Fuer Tabellen, Objektdaten, Abmessungen, Hoehe, Breite, Tiefe, Bounds oder Messwerte waehle geometry.query und measurement.bbox; bei alle Objekte/alle Layer keinen Layerfilter setzen. Nimm notwendige Abhaengigkeiten mit auf, z.B. layers.create wenn Objekte in einem neuen Layer erzeugt werden. Waehle layers.rename nur, wenn wirklich ein Layer/eine Ebene umbenannt werden soll; fuer vorhandene Objekte auf einen anderen Layer nutze entity.setLayer. Waehle rectangles.extrude/profile.extrude nur, wenn der Prompt ausdruecklich vorhandene Rechtecke/Profile extrudieren will. Waehle bim.classify bei ausdruecklicher BIM-/Klassifizierungsabsicht oder bei klaren architektonischen Wandaufgaben mit Wandstaerke/Wandhoehe/Raumbezug. Wenn neue 3D-Waende als Boxen erzeugt werden sollen, nutze geometry.create plus layers.create und bei Wandaufgaben zusaetzlich bim.classify. Nutze brxLearningContext fuer Repair-Regeln und Try-before-fail."},
        {"responseShape", QJsonObject{
            {"schema", "barebone.agent.selection.v1"},
            {"tools", QJsonArray{"tool.name"}},
            {"lessons", QJsonArray{"lesson.id"}},
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
            "Du bist der Barebone-Qt Tool- und Learning-Selector. "
            "Du beantwortest nicht die Nutzerfrage und erzeugst keine CAD-Aktion. "
            "Waehle zuerst passende vorhandene Learning-Lessons aus plainLearningLessons und danach relevante Toolnamen aus plainTools. "
            "Nutze Lessons nur, wenn Titel, Trigger, Kurzfassung, Repair-Regeln oder Werkzeugschritte zur Nutzerabsicht passen. "
            "Uebernimm aus Lessons keine festen Filter wie layer=0, Handles, Namen oder Winkel, wenn der Nutzer sie im aktuellen Prompt nicht nennt. "
            "Bei Tabellen, Objektdaten und Abmessungen waehle read-only Tools; fuer alle Objekte nutze currentSpace ohne Layerfilter. "
            "Waehle keine Extrude-Tools nur wegen Hoehenangaben, wenn geometry.create das Ziel direkt erzeugen kann. "
            "Waehle bim.classify bei ausdruecklicher BIM-/Klassifizierungsabsicht oder wenn der Prompt architektonische Waende mit Wandstaerke/Wandhoehe/Raumbezug erzeugt. "
            "Waehle layers.rename nur fuer Layer/Ebenen, niemals fuer BIM-Waende, Solids, Objekte oder Entities; fuer Layerzuweisung bestehender Entities waehle entity.setLayer. "
            "Antworte ausschliesslich als JSON: {\"schema\":\"barebone.agent.selection.v1\",\"tools\":[\"...\"],\"lessons\":[\"...\"],\"needsWorkflow\":false,\"needsCadContext\":true,\"confidence\":0.0,\"reason\":\"...\"}."},
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
        if (normalizedReasoningEffort(m_reasoningEffort) != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", "low"}});
        }
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
    appendBridgeLog(QString("Qt -> AI Tool/Learning Selector: tools=%1 lessons=%2 provider=%3 endpoint=%4 model=%5 timeoutMs=%6")
        .arg(plainTools.size())
        .arg(plainLearningLessons.size())
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
        QStringList selectedLessonIds;
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
        const QHash<QString, QString> knownLessonIds = [&]() {
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
                appendBridgeLog(QString("AI Tool/Learning Selector: Fehler http=%1 %2, nutze lokale Auswahl")
                .arg(httpStatus)
                .arg(reply->errorString()));
        } else {
            QJsonParseError parseError;
            const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
                appendBridgeLog(QString("AI Tool/Learning Selector: ungueltige OpenAI Antwort (%1), nutze lokale Auswahl")
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
                        const QString id = knownLessonIds.value(workflowSlug(candidate));
                        if (!id.isEmpty() && !selectedLessonIds.contains(id) && selectedLessonIds.size() < 3) {
                            selectedLessonIds << id;
                        }
                    };
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("lessons")).toArray()) {
                        appendWorkflowCandidate(value);
                    }
                    for (const QJsonValue& value : selectorReply.value(QStringLiteral("lessonIds")).toArray()) {
                        appendWorkflowCandidate(value);
                    }
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
                for (const QString& lessonId : selectedLessonIds) {
                    const QJsonObject lesson = m_brxLearning.lessonById(lessonId);
                    for (const QString& tool : jsonStringArrayToStringList(lesson.value(QStringLiteral("recommendedTools")).toArray())) {
                        if (knownToolNames.contains(tool) && !selectedToolNames.contains(tool) && selectedToolNames.size() < 8) {
                            selectedToolNames << tool;
                        }
                    }
                }
                if (!selectedToolNames.isEmpty()) {
                    appendBridgeLog(QString("AI Tool/Learning Selector: tools=%1 lessons=%2")
                        .arg(selectedToolNames.join(QStringLiteral(",")),
                            selectedLessonIds.join(QStringLiteral(","))));
                } else if (!selectedLessonIds.isEmpty()) {
                    appendBridgeLog(QString("AI Tool/Learning Selector: lessons=%1")
                        .arg(selectedLessonIds.join(QStringLiteral(","))));
                } else {
                    appendBridgeLog("AI Tool/Learning Selector: keine gueltige Auswahl, nutze lokale Auswahl");
                }
            }
        }

        if (selectedLessonIds.isEmpty()) {
            const QStringList localWorkflows = localWorkflowSelectionForPrompt(compactWorkflowSelectorList(), prompt, 3);
            for (const QString& workflowId : localWorkflows) {
                const QString id = knownLessonIds.value(workflowSlug(workflowId), workflowId);
                if (!id.isEmpty() && !selectedLessonIds.contains(id) && selectedLessonIds.size() < 3) {
                    selectedLessonIds << id;
                }
            }
            if (!selectedLessonIds.isEmpty()) {
                appendBridgeLog(QString("AI Tool/Learning Selector: lokale Lesson-Auswahl %1")
                    .arg(selectedLessonIds.join(QStringLiteral(","))));
            }
        }

        if (!selectedLessonIds.isEmpty()) {
            for (const QString& lessonId : selectedLessonIds) {
                const QJsonObject lesson = m_brxLearning.lessonById(lessonId);
                for (const QString& tool : jsonStringArrayToStringList(lesson.value(QStringLiteral("recommendedTools")).toArray())) {
                    if (knownToolNames.contains(tool) && !selectedToolNames.contains(tool) && selectedToolNames.size() < 8) {
                        selectedToolNames << tool;
                    }
                }
            }
        }

        if (!selectedToolNames.isEmpty()) {
            selectedRoute.insert(QStringLiteral("selectedTools"), stringsToJsonArray(selectedToolNames));
        }
        if (!selectedLessonIds.isEmpty()) {
            selectedRoute.insert(QStringLiteral("selectedWorkflows"), stringsToJsonArray(selectedLessonIds));
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

void BricsCadPage::requestFocusedConversationContext(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route,
    std::function<void(const QJsonObject&)> continuation)
{
    Q_UNUSED(documentContext);
    Q_UNUSED(route);

    if (m_agentConversation.isEmpty()) {
        continuation(fallbackFocusedConversationContext(prompt));
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
        appendBridgeLog(QStringLiteral("AI Kontext-Fokus: ungueltige AI Server URL, nutze alte Kontextkuerzung"));
        continuation({});
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        continuation({});
        return;
    }

    QJsonArray history;
    const int maxMessages = 120;
    const int start = std::max(0, static_cast<int>(m_agentConversation.size()) - maxMessages);
    for (int i = start; i < m_agentConversation.size(); ++i) {
        QJsonObject item = compactSessionHistoryMessage(i, true, 1800);
        if (!item.isEmpty()) {
            history.append(item);
        }
    }

    QJsonObject focusInput{
        {"schema", "barebone.agent.context-focus.request.v1"},
        {"currentPrompt", prompt},
        {"conversationMessageCount", m_agentConversation.size()},
        {"historyTruncatedToLastMessages", start > 0 ? maxMessages : 0},
        {"conversation", history},
        {"instruction",
            "Fasse den bisherigen Sitzungsverlauf ausschliesslich bezogen auf den aktuellen Prompt zusammen. "
            "Nutze keine Tools, keine Policies, keinen Systemprompt und keine BRX-Annahmen. "
            "Entscheide selbst, welche bisherigen Nachrichten thematisch relevant sind. "
            "Lasse fremde Themen weg, nenne sie nur kurz in omittedTopics. "
            "Antworte ausschliesslich mit einem JSON-Objekt nach outputSchema."},
        {"outputSchema", QJsonObject{
            {"schema", "barebone.agent.focused-conversation-context.v1"},
            {"required", QJsonArray{"topic", "relevantSummary", "relevantMessageIndexes", "omittedTopics", "confidence"}},
            {"fields", QJsonObject{
                {"topic", "kurzer deutscher Themenname fuer den aktuellen Prompt"},
                {"relevantSummary", "kompakte Zusammenfassung nur der promptrelevanten Verlaufsteile"},
                {"relevantMessageIndexes", "Array zero-based message indexes aus conversation, die relevant sind"},
                {"omittedTopics", "kurze Liste bewusst weggelassener anderer Themen"},
                {"confidence", "0..1 wie sicher die Auswahl vollstaendig ist"},
            }},
        }},
    };

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "user"},
        {"content", QString::fromUtf8(QJsonDocument(focusInput).toJson(QJsonDocument::Compact))},
    });

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", 1536);
        if (normalizedReasoningEffort(m_reasoningEffort) != QStringLiteral("none")) {
            payload.insert("reasoning", QJsonObject{{"effort", "low"}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.0);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.0);
        payload.insert("max_tokens", 1536);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kFocusedContextAiTimeoutMs);

    appendBridgeLog("AI Agent: fokussiere Sitzungsverlauf fuer aktuellen Prompt");
    setAgentBusy(true);
    const int operationGeneration = m_operationGeneration;
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, continuation = std::move(continuation), prompt, operationGeneration]() mutable {
        if (operationGeneration != m_operationGeneration) {
            reply->deleteLater();
            return;
        }
        setAgentBusy(false);

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendBridgeLog(QString("AI Kontext-Fokus: Fehler http=%1 %2, nutze alte Kontextkuerzung")
                .arg(httpStatus)
                .arg(reply->errorString()));
            reply->deleteLater();
            continuation({});
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            appendBridgeLog(QString("AI Kontext-Fokus: OpenAI JSON ungueltig (%1), nutze alte Kontextkuerzung")
                .arg(parseError.errorString()));
            reply->deleteLater();
            continuation({});
            return;
        }

        QString reasoningText;
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        bool parsed = false;
        const QJsonObject parsedObject = jsonObjectFromAiContent(content, &parsed);
        const QJsonObject focusedContext = parsed
            ? normalizedFocusedConversationContext(parsedObject, prompt)
            : QJsonObject{};
        if (focusedContext.isEmpty()) {
            appendBridgeLog("AI Kontext-Fokus: keine gueltige fokussierte Zusammenfassung, nutze alte Kontextkuerzung");
            reply->deleteLater();
            continuation({});
            return;
        }
        appendBridgeLog(QString("AI Kontext-Fokus: topic=%1 confidence=%2 relevant=%3")
            .arg(focusedContext.value(QStringLiteral("topic")).toString().left(120))
            .arg(focusedContext.value(QStringLiteral("confidence")).toDouble(), 0, 'f', 2)
            .arg(focusedContext.value(QStringLiteral("relevantMessageIndexes")).toArray().size()));
        reply->deleteLater();
        continuation(focusedContext);
    });
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

void BricsCadPage::sendUnifiedAgentRequestWithDrawingContext(
    const QString& prompt,
    const QJsonObject& documentContext,
    const QJsonObject& route)
{
    if (!m_brxAuthenticated) {
        sendUnifiedAgentRequest(prompt, documentContext, route);
        return;
    }

    QJsonArray requests;
    if (bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("layers.list"))) {
        requests.append(QJsonObject{
            {"method", "layers.list"},
            {"params", QJsonObject{}},
            {"purpose", "Aktuelle Layernamen und Layerzustand der Zeichnung lesen."},
        });
    }
    if (bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("geometry.query"))) {
        requests.append(QJsonObject{
            {"method", "geometry.query"},
            {"params", QJsonObject{
                {"selector", QJsonObject{{"scope", "currentSpace"}}},
                {"include", QJsonArray{"metrics", "geometry", "dimensions"}},
                {"limit", 200},
            }},
            {"purpose", "Aktuelle Objekte im Modelspace kompakt lesen; keine Auswahl oder Mutation."},
        });
    }
    if (shouldPrefetchSelectionDescription(prompt)
        && bridgeCapabilitiesContainMethod(m_brxCapabilities, QStringLiteral("selection.describe"))) {
        requests.append(QJsonObject{
            {"method", "selection.describe"},
            {"params", QJsonObject{
                {"include", QJsonArray{"geometry", "metrics", "dimensions"}},
                {"limit", 100},
            }},
            {"purpose", "Aktuelle BricsCAD-Auswahl lesen, weil der Prompt Auswahlkontext anspricht."},
        });
    }

    if (requests.isEmpty()) {
        sendUnifiedAgentRequest(prompt, documentContext, route);
        return;
    }

    appendBridgeLog(QString("Qt -> BRX Zeichnungskontext: %1")
        .arg(jsonStringArrayToStringList([&requests]() {
            QJsonArray methods;
            for (const QJsonValue& value : requests) {
                methods.append(value.toObject().value(QStringLiteral("method")).toString());
            }
            return methods;
        }()).join(QStringLiteral(", "))));

    QSharedPointer<QJsonArray> results(new QJsonArray);
    QSharedPointer<int> index(new int(0));
    QSharedPointer<std::function<void()>> runNext(new std::function<void()>);
    QPointer<BricsCadPage> guard(this);

    auto finish = [this, prompt, documentContext, route, results]() {
        QJsonObject envelope = agentRequestEnvelope(prompt, documentContext, route);
        envelope.insert("prefetchedDrawingContext", QJsonObject{
            {"schema", "barebone.brx.prefetched-drawing-context.v1"},
            {"source", "BRX read-only snapshot before final agent run"},
            {"policy", "Nutze diesen aktuellen Zeichnungszustand als primaeren BricsCAD-Kontext fuer Toolwahl, Parameter, Layernamen, Handles, Objekttypen, Abmessungen und Plausibilitaetspruefung. Er ist read-only und ersetzt keine BRX-Preflight-Validierung."},
            {"requests", *results},
        });
        sendAgentEnvelope(envelope, prompt, true, "prompt+drawing-context");
    };

    *runNext = [this, guard, prompt, documentContext, route, requests, results, index, runNext, finish]() mutable {
        if (!guard) {
            return;
        }
        if (*index >= requests.size()) {
            setAgentBusy(false);
            appendBridgeLog(QString("BRX -> Qt Zeichnungskontext: %1 Abfragen bereit").arg(results->size()));
            finish();
            return;
        }

        const QJsonObject item = requests.at(*index).toObject();
        const QString method = item.value(QStringLiteral("method")).toString();
        const QJsonObject params = item.value(QStringLiteral("params")).toObject();
        const QString purpose = item.value(QStringLiteral("purpose")).toString();
        appendBridgeLog(QString("Qt -> BRX Zeichnungskontext %1/%2: %3 %4")
            .arg(*index + 1)
            .arg(requests.size())
            .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));

        const bool queued = sendBridgeRequest(
            method,
            params,
            15000,
            [this, guard, method, params, purpose, results, index, runNext](const QJsonObject& response) mutable {
                if (!guard) {
                    return;
                }
                appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
                QJsonObject entry{
                    {"method", method},
                    {"params", params},
                    {"purpose", purpose},
                    {"ok", response.value(QStringLiteral("ok")).toBool(false)},
                };
                if (response.value(QStringLiteral("ok")).toBool(false)) {
                    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
                    entry.insert(QStringLiteral("result"), compactBrxResponseForAgent(result));
                    const int count = result.value(QStringLiteral("count")).toInt(
                        result.value(QStringLiteral("objects")).toArray().size()
                            + result.value(QStringLiteral("layers")).toArray().size());
                    entry.insert(QStringLiteral("count"), count);
                    appendBridgeLog(QString("BRX -> Qt Zeichnungskontext: %1 count=%2").arg(method).arg(count));
                } else {
                    const QString error = response.value(QStringLiteral("error")).toString(QStringLiteral("Kontextabfrage fehlgeschlagen"));
                    entry.insert(QStringLiteral("error"), error);
                    appendBridgeLog(QString("BRX -> Qt Zeichnungskontext ERROR: %1 %2").arg(method, error.left(400)));
                }
                results->append(entry);
                ++(*index);
                (*runNext)();
            });
        if (!queued) {
            results->append(QJsonObject{
                {"method", method},
                {"params", params},
                {"purpose", purpose},
                {"ok", false},
                {"error", "BRX Kontextabfrage konnte nicht gesendet werden."},
            });
            ++(*index);
            (*runNext)();
        }
    };

    setAgentBusy(true);
    (*runNext)();
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
        "Der Nutzer befindet sich im Allgemeinen Modus. Der eingehende User-Content ist ein JSON-Envelope. Antworte ausschliesslich mit genau einem JSON-Objekt nach schema barebone.agent.response.v2.\n"
        "Finale Pflichtfelder: schema, type=\"message\", message, sessionTitle. message enthaelt die normale direkte Chatantwort und wird im Chat angezeigt; sessionTitle ist nur Metadatum fuer den Sitzungsnamen.\n"
        "Wenn focusedConversationContext oder conversation nicht reicht, darfst du vor der finalen Antwort type=\"context_request\" mit method aus conversationAccess.allowedMethods senden. Erlaubt sind nur session.history.query, session.history.range oder session.history.recent; niemals Tools, BRX-Kontext oder Systemprompt anfordern.\n"
        "Erzeuge sessionTitle bei jeder Antwort selbst aus userPrompt, compactState und dem fachlichen Schwerpunkt der bisherigen Unterhaltung. Nutze einen kurzen, konkreten deutschen Titel mit hoechstens 6 Woertern; keine generischen Titel wie Neuer Chat, Allgemeiner Chat, Workflow oder Frage.\n"
        "Nutze aus dem Envelope vor allem userPrompt, documentContext, selectedWorkflow, workflowCapsules, compactState und conversation.\n"
        "Wenn selectedWorkflow oder workflowCapsules einen allgemeinen Workflow enthalten, behandle dessen Tabellen, Formeln, Beispiele, Annahmen und contextSummary als primaeren Kontext fuer die Antwort.\n"
        "Bei Berechnungen gilt: erst die Grundgleichung, dann alle verwendeten Symbole kurz erklaeren, dann Werte mit SI-Einheiten einsetzen, dann Zwischenergebnisse und Endergebnis. Jede eingesetzte Zahl und jeder Summand muss seine Einheit tragen; keine reinen Zahlenketten mit Einheit nur am Ende. Wenn Eingaben nicht in SI-Einheiten vorliegen, zeige zuerst die SI-Umrechnung.\n"
        "%2\n"
        "Markdown fuer Listen, Tabellen und Codebloecke ist innerhalb von message erlaubt. Keine Markdown-Antwort ausserhalb des JSON-Objekts.\n"
        "Schlage keine CAD-Tools, keine BRX-Ausfuehrung und keine Aktionen vor. Wenn Live-BricsCAD-Daten noetig waeren, erklaere knapp, dass dafuer der BricsCAD-Modus/BRX-Kontext erforderlich ist.")
        .arg(aiLanguageInstruction(m_config), katexFormattingInstructionText());
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
    const int localResponsesOutputMaximum = std::clamp(effectiveContextWindowTokens() / 4, 8192, 32768);
    const int requestedOutputTokens = dynamicOutputTokenTarget(
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
    const QJsonObject focusedConversationContext = envelope.value(QStringLiteral("focusedConversationContext")).toObject();
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
            builtMessages.append(item);
            recentConversation.append(item);
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
            const int maxFocusedRecentMessages = std::min(2, totalHistoryMessages);
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
            details << (hasFocusedConversationContext
                ? QStringLiteral("%1 aeltere Nachrichten ueber Fokus-Zusammenfassung ersetzt").arg(contextBuild.compressedHistoryMessages)
                : QStringLiteral("%1 aeltere Nachrichten komprimiert").arg(contextBuild.compressedHistoryMessages));
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
    const auto objectTokens = [](const QJsonObject& value) {
        return (QJsonDocument(value).toJson(QJsonDocument::Compact).size() + 3) / 4;
    };
    const auto arrayTokens = [](const QJsonArray& value) {
        return (QJsonDocument(value).toJson(QJsonDocument::Compact).size() + 3) / 4;
    };
    appendBridgeLog(QString("AI Kontextanteile: workflow=%1 tools=%2 cad=%3 policies=%4 responseContract=%5 pending=%6 historyMessages=%7")
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("brxLearningContext")).toObject()))
        .arg(arrayTokens(contextBuild.envelope.value(QStringLiteral("effectiveTools")).toArray()))
        .arg(objectTokens(contextBuild.envelope.value(QStringLiteral("cadContext")).toObject()))
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

QJsonArray BricsCadPage::workflowTrainingIndex() const
{
    if (isChatWorkspace()) {
        return generalWorkflowIndex();
    }
    return m_brxLearning.learningIndex();}

QJsonArray BricsCadPage::compactWorkflowSelectorList() const
{
    QJsonArray compactWorkflows;
    const QJsonArray workflows = isChatWorkspace()
        ? workflowTrainingIndex()
        : m_brxLearning.lessonIndex();
    for (const QJsonValue& value : workflows) {
        const QJsonObject indexed = value.toObject();
        const QJsonObject workflow = indexed.value("workflow").toObject(indexed);
        const QString id = indexed.value("id").toString(workflow.value("id").toString()).trimmed();
        const QString title = repairMojibakeText(indexed.value("title").toString(workflow.value("title").toString())).trimmed();
        if (id.isEmpty() && title.isEmpty()) {
            continue;
        }

        if (workflow.value(QStringLiteral("kind")).toString() == QStringLiteral("learning")) {
            compactWorkflows.append(QJsonObject{
                {"id", id},
                {"title", title.isEmpty() ? id : title},
                {"compactSummary", repairMojibakeText(workflow.value(QStringLiteral("description")).toString(workflow.value(QStringLiteral("intent")).toString())).left(420)},
                {"keywords", workflow.value(QStringLiteral("intentPatterns")).toArray()},
                {"triggerExamples", workflow.value(QStringLiteral("intentPatterns")).toArray()},
                {"preferredTools", workflow.value(QStringLiteral("recommendedTools")).toArray()},
                {"stepTools", workflow.value(QStringLiteral("recommendedTools")).toArray()},
                {"stepSummary", workflow.value(QStringLiteral("strategy")).toArray()},
                {"discipline", workflow.value(QStringLiteral("discipline")).toString()},
                {"kind", QStringLiteral("learning")},
            });
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

    if (errorMessage) {
        *errorMessage = QStringLiteral("BricsCAD nutzt BRX Learning statt Workflow-Dateien.");
    }
    Q_UNUSED(workflowId);
    Q_UNUSED(fileName);
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

    if (!m_brxLearning.deprecateLesson(workflowId, errorMessage)) {
        return false;
    }
    if (deletedPath) {
        *deletedPath = m_brxLearning.sourcePath();
    }
    emitWorkflowListToWeb();
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
        {"updateProtected", m_selectedWorkflow.value("updateProtected").toBool(false)},
        {"selectedSlotValues", m_selectedWorkflowSlotValues},
    };
}

QJsonArray BricsCadPage::selectedWorkflowObjectsForRoute(const QJsonObject& route) const
{
    if (!isChatWorkspace()) {
        return {};
    }

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

QJsonArray BricsCadPage::selectedLearningLessonsForRoute(const QJsonObject& route, const QString& prompt) const
{
    QJsonArray lessons;
    QSet<QString> seen;

    auto appendLesson = [&](QJsonObject lesson) {
        const QString id = lesson.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty() || seen.contains(id)) {
            return;
        }
        seen.insert(id);
        lesson.insert(QStringLiteral("kind"), QStringLiteral("learning"));
        lessons.append(lesson);
    };

    if (!m_selectedWorkflowId.isEmpty()) {
        appendLesson(m_brxLearning.lessonById(m_selectedWorkflowId));
    }

    for (const QString& id : routeWorkflowIds(route, 3)) {
        appendLesson(m_brxLearning.lessonById(id));
    }
    const QJsonArray relevant = m_brxLearning.relevantLessons(prompt, 3);
    for (const QJsonValue& value : relevant) {
        appendLesson(value.toObject());
    }
    return lessons;
}

void BricsCadPage::selectWorkflowForChat(const QString& workflowId)
{
    QString fileName;
    QString errorMessage;
    QJsonObject workflow;
    if (isChatWorkspace()) {
        workflow = loadGeneralWorkflowById(workflowId, &fileName, &errorMessage);
    } else {
        workflow = m_brxLearning.lessonById(workflowId);
        fileName = QStringLiteral("agent/bricscad-learning/brx-learning.json");
        if (workflow.isEmpty()) {
            errorMessage = QStringLiteral("Learning-Workflow wurde nicht gefunden.");
        }
    }
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

QJsonArray BricsCadPage::materializedWorkflowTestActions(const QJsonObject& workflow, QJsonObject* usedDefaults, QString* errorMessage) const
{
    QJsonObject slots = workflow.value(QStringLiteral("knownSlotValues")).toObject();
    const QJsonObject defaults = workflow.value(QStringLiteral("testDefaults")).toObject();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (!slots.contains(it.key()) || slots.value(it.key()).isNull()) slots.insert(it.key(), it.value());
    }
    if (usedDefaults) *usedDefaults = defaults;

    std::function<QJsonValue(const QJsonValue&, QString*)> resolve;
    resolve = [&](const QJsonValue& value, QString* error) -> QJsonValue {
        if (value.isArray()) {
            QJsonArray result;
            for (const QJsonValue& item : value.toArray()) {
                const QJsonValue resolved = resolve(item, error);
                if (error && !error->isEmpty()) return {};
                result.append(resolved);
            }
            return result;
        }
        if (value.isObject()) {
            QJsonObject result;
            const QJsonObject object = value.toObject();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                const QJsonValue resolved = resolve(it.value(), error);
                if (error && !error->isEmpty()) return {};
                result.insert(it.key(), resolved);
            }
            return result;
        }
        if (!value.isString()) return value;
        const QString text = value.toString();
        const QRegularExpression exact(QStringLiteral(R"(^\{\{([A-Za-z0-9_.]+)\}\}$)"));
        const QRegularExpressionMatch match = exact.match(text);
        if (match.hasMatch()) {
            const QString key = match.captured(1);
            if (!slots.contains(key) || slots.value(key).isNull()) {
                if (error) *error = QStringLiteral("Testwert '%1' ist nicht aufloesbar").arg(key);
                return {};
            }
            return slots.value(key);
        }
        QString output = text;
        QRegularExpression embedded(QStringLiteral(R"(\{\{([A-Za-z0-9_.]+)\}\})"));
        auto iterator = embedded.globalMatch(text);
        while (iterator.hasNext()) {
            const auto part = iterator.next();
            const QString key = part.captured(1);
            if (!slots.contains(key)) {
                if (error) *error = QStringLiteral("Testwert '%1' ist nicht aufloesbar").arg(key);
                return {};
            }
            output.replace(part.captured(0), workflowJsonValueSummary(slots.value(key)));
        }
        return output;
    };

    QJsonArray actions;
    for (const QJsonValue& batchValue : workflow.value(QStringLiteral("executionBatches")).toArray()) {
        const QJsonObject batch = batchValue.toObject();
        for (const QJsonValue& stepValue : batch.value(QStringLiteral("steps")).toArray()) {
            const QJsonObject step = stepValue.toObject();
            QString resolveError;
            const QJsonObject params = resolve(step.value(QStringLiteral("paramsTemplate")), &resolveError).toObject();
            if (!resolveError.isEmpty()) {
                if (errorMessage) *errorMessage = QStringLiteral("Schritt '%1': %2").arg(step.value(QStringLiteral("id")).toString(), resolveError);
                return {};
            }
            actions.append(QJsonObject{
                {QStringLiteral("id"), step.value(QStringLiteral("id"))},
                {QStringLiteral("title"), step.value(QStringLiteral("title"))},
                {QStringLiteral("batchId"), batch.value(QStringLiteral("id"))},
                {QStringLiteral("tool"), step.value(QStringLiteral("tool"))},
                {QStringLiteral("params"), params},
            });
        }
    }
    if (actions.isEmpty() && errorMessage) *errorMessage = QStringLiteral("Workflow enthaelt keine ausfuehrbaren Schritte");
    return actions;
}

void BricsCadPage::runBrxWorkflowTest(const QString& workflowId)
{
    if (!isBricsCadMode() || !m_brxAuthenticated || m_brxCapabilities.isEmpty()) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflowtest nicht gestartet: BricsCAD-Modus, BRX-Verbindung und Capabilities sind erforderlich."));
        return;
    }
    if (m_agentBusy) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflowtest nicht gestartet: Es laeuft bereits ein Vorgang."));
        return;
    }
    const QJsonObject workflow = m_brxLearning.lessonById(workflowId);
    if (workflow.isEmpty() || workflow.value(QStringLiteral("status")).toString(QStringLiteral("active")) != QStringLiteral("active")) {
        appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflowtest nicht gestartet: aktiver Workflow wurde nicht gefunden."));
        return;
    }
    selectWorkflowForChat(workflowId);
    m_workflowTestWorkflow = workflow;
    m_workflowTestStartedMs = QDateTime::currentMSecsSinceEpoch();
    QString error;
    QJsonObject defaults;
    const QJsonArray actions = materializedWorkflowTestActions(workflow, &defaults, &error);
    if (!error.isEmpty()) {
        finishBrxWorkflowTest(QStringLiteral("failed"), error);
        return;
    }
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        if (toolDefinition(action.value(QStringLiteral("tool")).toString()).isEmpty()) {
            finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("BRX Tool fehlt: %1").arg(action.value(QStringLiteral("tool")).toString()));
            return;
        }
    }
    m_workflowTestActions = actions;
    m_workflowTestDefaults = defaults;
    m_workflowTestStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_workflowTestGeneration = m_operationGeneration;
    setAgentBusy(true);
    appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Direkter BRX-Workflowtest gestartet: %1 (%2 Schritte). Keine AI-Aktionsplanung wird verwendet.")
        .arg(workflow.value(QStringLiteral("title")).toString(workflowId)).arg(actions.size()));
    if (m_agentBridge) Q_EMIT m_agentBridge->workflowRunProgress(QVariantMap{{"workflowId",workflowId},{"title",workflow.value("title").toString()},{"phase","materializing"},{"current",0},{"total",actions.size()}});
    executeBrxWorkflowTestStep(0, {});
}

void BricsCadPage::executeBrxWorkflowTestStep(int index, QJsonArray results)
{
    if (m_workflowTestGeneration != m_operationGeneration) {
        finishBrxWorkflowTest(QStringLiteral("cancelled"), QStringLiteral("Workflowtest wurde abgebrochen."), index, {}, results);
        return;
    }
    if (index >= m_workflowTestActions.size()) {
        finishBrxWorkflowTest(QStringLiteral("success"), QStringLiteral("Alle Schritte wurden durch BRX validiert und ausgefuehrt."), -1, {}, results);
        return;
    }
    const QJsonObject action = m_workflowTestActions.at(index).toObject();
    const QString tool = action.value(QStringLiteral("tool")).toString();
    const bool needsOrderedRooms = action.value(QStringLiteral("params")).toObject().value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false);
    QJsonObject params = paramsWithRuntimeBatchHandles(tool, action.value(QStringLiteral("params")).toObject(), results);
    if (m_workflowTestWorkflow.value(QStringLiteral("id")).toString() == QStringLiteral("workflow_03_interior_walls")
        && tool == QStringLiteral("rectangles.extrude")
        && params.value(QStringLiteral("selector")).toObject().value(QStringLiteral("handles")).toArray().size() != 5) {
        finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("Innenwandextrusion braucht exakt 5 neu erzeugte Profilhandles."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
        return;
    }
    if ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
        && params.value(QStringLiteral("selector")).toObject().value(QStringLiteral("handles")).toArray().isEmpty()) {
        finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("Runtime-Bindung lieferte keine Polylinienhandles."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
        return;
    }
    if (needsOrderedRooms && params.value(QStringLiteral("selector")).toObject().value(QStringLiteral("handles")).toArray().size() != 6) {
        finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("Raumzuordnung ist nicht eindeutig: erwartet werden genau 6 Raumkonturen in Grundriss Raeume."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
        return;
    }
    const QVariantMap progress{{"workflowId",m_workflowTestWorkflow.value("id").toString()},{"title",m_workflowTestWorkflow.value("title").toString()},{"phase","preflight"},{"current",index+1},{"total",m_workflowTestActions.size()},{"stepId",action.value("id").toString()},{"tool",tool}};
    if (m_agentBridge) Q_EMIT m_agentBridge->workflowRunProgress(progress);
    const QJsonObject preflight{{"source","workflow_direct_test"},{"actions",QJsonArray{QJsonObject{{"tool",tool},{"params",params}}}}};
    const bool preflightQueued = sendBridgeRequest(QStringLiteral("actions.validate"), preflight, 15000,
        [this,index,results,action,tool,params](const QJsonObject& validation) mutable {
            if (!validation.value("ok").toBool(false) || !validation.value("result").toObject().value("valid").toBool(false)) {
                finishBrxWorkflowTest(QStringLiteral("failed"), validationFailureMessage(validation), index, QJsonObject{{"tool",tool},{"params",params}}, results);
                return;
            }
            QTimer::singleShot(kAgentBatchActionDelayMs, this, [this,index,results,action,tool,params]() mutable {
                if (m_agentBridge) Q_EMIT m_agentBridge->workflowRunProgress(QVariantMap{{"workflowId",m_workflowTestWorkflow.value("id").toString()},{"title",m_workflowTestWorkflow.value("title").toString()},{"phase","executing"},{"current",index+1},{"total",m_workflowTestActions.size()},{"stepId",action.value("id").toString()},{"tool",tool}});
                const bool executionQueued = sendBridgeRequest(bridgeMethodForTool(tool), params, 30000,
                    [this,index,results,action,tool,params](const QJsonObject& response) mutable {
                        if (!response.value("ok").toBool(false)) {
                            finishBrxWorkflowTest(QStringLiteral("failed"), bridgeErrorMessage(response, QStringLiteral("BRX-Ausfuehrung fehlgeschlagen")), index, QJsonObject{{"tool",tool},{"params",params}}, results);
                            return;
                        }
                        const QJsonObject result = response.value("result").toObject();
                        if (m_workflowTestWorkflow.value(QStringLiteral("id")).toString() == QStringLiteral("workflow_03_interior_walls")
                            && tool == QStringLiteral("rectangles.extrude")
                            && result.value(QStringLiteral("affectedHandles")).toArray().size() != 5
                            && result.value(QStringLiteral("extruded")).toInt() != 5) {
                            finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("BRX hat nicht alle 5 Innenwandprofile extrudiert."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
                            return;
                        }
                        if (tool == QStringLiteral("pipes.validateNetwork") && result.contains("valid") && !result.value("valid").toBool(false)) {
                            finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("Fachliche Netzvalidierung meldet valid=false."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
                            return;
                        }
                        results.append(QJsonObject{{"index",index+1},{"tool",tool},{"params",params},{"response",response},{"result",result}});
                        QTimer::singleShot(kAgentBatchActionDelayMs, this, [this,index,results]() mutable {
                            executeBrxWorkflowTestStep(index+1, results);
                        });
                    });
                if (!executionQueued) finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("BRX-Ausfuehrungsanfrage konnte nicht gesendet werden."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
            });
        });
    if (!preflightQueued) finishBrxWorkflowTest(QStringLiteral("failed"), QStringLiteral("BRX-Preflight konnte nicht gesendet werden."), index, QJsonObject{{"tool",tool},{"params",params}}, results);
}

void BricsCadPage::finishBrxWorkflowTest(const QString& status, const QString& message, int failedIndex, const QJsonObject& failedAction, const QJsonArray& results)
{
    QJsonArray handles;
    for (const QJsonValue& value : results) {
        const QJsonObject result = value.toObject().value(QStringLiteral("result")).toObject();
        for (const QJsonValue& handle : result.value(QStringLiteral("affectedHandles")).toArray()) handles.append(handle);
        if (!result.value(QStringLiteral("handle")).toString().isEmpty()) handles.append(result.value(QStringLiteral("handle")));
    }
    const qint64 duration = m_workflowTestStartedMs > 0 ? QDateTime::currentMSecsSinceEpoch()-m_workflowTestStartedMs : 0;
    QJsonObject payload{{"schema","barebone.workflow.test.result.v1"},{"workflowId",m_workflowTestWorkflow.value("id").toString()},{"title",m_workflowTestWorkflow.value("title").toString()},{"status",status},{"message",message},{"completed",results.size()},{"total",m_workflowTestActions.size()},{"failedIndex",failedIndex+1},{"failedAction",failedAction},{"createdHandles",uniqueStringArray(handles)},{"testDefaults",m_workflowTestDefaults},{"durationMs",duration},{"results",results}};
    setAgentBusy(false);
    if (m_agentBridge) Q_EMIT m_agentBridge->workflowRunFinished(payload.toVariantMap());
    appendAgentChat(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflowtest %1: %2 (%3/%4 Schritte, %5 ms)")
        .arg(status == QStringLiteral("success") ? QStringLiteral("erfolgreich") : status, message).arg(results.size()).arg(m_workflowTestActions.size()).arg(duration));
    if (status == QStringLiteral("failed")) requestWorkflowFailureAnalysis(payload);
    m_workflowTestActions = {};
}

void BricsCadPage::requestWorkflowFailureAnalysis(const QJsonObject& result)
{
    if (!m_localAiReachable || m_config.aiProvider() == QStringLiteral("official") || !m_aiNetwork) return;
    const QString provider = m_config.aiProvider();
    const QString model = m_config.aiModel().trimmed().isEmpty() ? QStringLiteral("openai/gpt-oss-20b") : m_config.aiModel().trimmed();
    const bool responsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(normalizedAiBaseUrl(m_config.aiBaseUrl(), provider)
        + (responsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) return;
    QJsonObject compact{
        {"workflowId", result.value("workflowId")}, {"title", result.value("title")},
        {"failedIndex", result.value("failedIndex")}, {"failedAction", result.value("failedAction")},
        {"message", result.value("message")}, {"completed", result.value("completed")},
    };
    const QString instruction = QStringLiteral(
        "Analysiere ausschliesslich diesen fehlgeschlagenen deterministischen BricsCAD-Workflowtest. "
        "Antworte auf Deutsch in hoechstens 6 kurzen Saetzen mit Ursache und konkretem Hinweis fuer die Workflowdefinition. "
        "Erzeuge keine Actions, kein JSON, keinen Retry und keine Aenderung. Diagnose: %1")
        .arg(QString::fromUtf8(QJsonDocument(compact).toJson(QJsonDocument::Compact)));
    const QJsonArray messages{QJsonObject{{"role","system"},{"content","Du bist ein kompakter read-only BRX-Testdiagnostiker."}},QJsonObject{{"role","user"},{"content",instruction}}};
    QJsonObject payload{{"model",model}};
    if (responsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", 800);
        payload.insert("reasoning", QJsonObject{{"effort","low"}});
    } else {
        payload.insert("messages", messages);
        payload.insert("max_tokens", 800);
        payload.insert("temperature", 0.1);
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(60000);
    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this,reply]() {
        const QByteArray body = reply->readAll();
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonDocument document = QJsonDocument::fromJson(body);
            QString reasoning;
            const QString text = removeReasoningLeak(aiChatCompletionContent(document.object(), &reasoning)).trimmed();
            if (!text.isEmpty()) appendAgentChat(QStringLiteral("AI Testanalyse"), text);
        } else {
            appendBridgeLog(QStringLiteral("AI Testanalyse nicht verfuegbar: %1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
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
            "Legacy-Trainingsmodus ist deaktiviert; BricsCAD-Wissen liegt in agent/bricscad-learning/brx-learning.json. "
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
            "Wenn der Nutzer 'relativ', 'Verschiebungsvektor', 'Vektor' oder x/y/z-Werte fuer eine Verschiebung nennt, ist damit geometry.move.params.vector gemeint; frage dann nicht nach absoluter Zielkoordinate. "
            "Wenn vorhandene Objekte selektiert und bearbeitet werden, frage nicht nach Erzeugungsdaten wie rectangleWidthMm, rectangleHeightMm oder extrudeHeightMm; diese sind nur fuer neue Geometrie noetig. "
            "Wenn der Nutzer sagt, dass eine Angabe nicht gebraucht wird, pruefe aktiv den aktuellen Workflow und entferne unreferenzierte requiredSlots/missing Felder statt dieselbe Rueckfrage zu wiederholen. "
            "Nutze fuer ausfuehrbare Werkzeugschritte workflow.steps[].tool und workflow.steps[].paramsTemplate nach effectiveTools[].inputSchema/apiPost. Fuer punktbasierte Batch-Datenfluesse sind zusaetzlich die Qt-internen Felder autoPointHandlesFromBatch, createdPointHandleIndexes, autoPointsFromLastQuery, queriedPointIndexes, autoPolylineHandlesFromBatch, createdPolylineHandleIndexes und autoPolylineHandlesFromLastQuery erlaubt; sie werden vor dem BRX-Aufruf in konkrete Handles oder Punkte aufgeloest. "
            "Wenn der Nutzer verlangt, Geometrie in einem bestimmten Layer zu zeichnen, muss der geometry.create Schritt den Parameter paramsTemplate.layer exakt mit diesem Layernamen enthalten; layers.create allein setzt den Ziellayer fuer folgende Geometrie nicht automatisch. "
            "Wenn ein Schritt selection.set verwendet und der naechste Schritt auf dieser Auswahl arbeiten soll, muss der naechste Schritt trotzdem explizit paramsTemplate.selector={\"scope\":\"selection\"} oder target=\"selection\" enthalten. "
            "Batch-Ausfuehrungen modellierst du in workflow.executionBatches mit mode=sequential, stopOnFailure=true und steps[].tool/paramsTemplate. "
            "Spiegele dieselben ausfuehrbaren Schritte zusaetzlich in der flachen workflow.steps Liste, sobald du workflow_update erzeugst. "
            "Schreibe constructionStrategy als JSON-Array mit einem kurzen String pro Strategiepunkt; keine eingebetteten '\\n', keine nummerierte Liste in einem einzelnen String. "
            "Formeln speicherst du als workflow.derivedValues mit name, expression, dependsOn, unit und example; Qt fuehrt keine Formeln aus, daher muessen validationExamples konkrete Beispielwerte enthalten. "
            "Wenn ein Schritt unmittelbar zuvor mit geometry.create ein Rechteck erzeugt, verwende im naechsten rectangles.extrude selector={\"scope\":\"lastResult\",\"kind\":\"rectangle\"}, nicht scope=selection. "
            "Bei normalen mehrschrittigen Vorschlaegen kann ein direkt nachfolgendes bim.classify target=lastExtruded nutzen. "
            "Bei gespeicherten Workflows, die Schritt fuer Schritt mit Nutzerfeedback ausgefuehrt werden, bevorzuge nach dem Extrudieren einen expliziten Selector oder selection.set und danach bim.classify target=selection, weil jeder Schritt einzeln per BRX validiert wird. "
            "Wenn ein Workflow BricsCAD-Geometrie auswerten oder tabellarisch dokumentieren soll, nutze geometry.query, selection.describe oder entity.describe mit include=[\"metrics\",\"geometry\"] und einem sinnvollen positiven limit >= 1; nutze niemals limit=0. "
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
            {"toolPolicy", "preferredTools und steps[].tool duerfen nur bekannte toolNames/effectiveTools[].name verwenden. paramsTemplate verwendet Parameter aus inputSchema.properties/apiPost.bodySchema; fuer Punkt-Readback sind ausschliesslich die dokumentierten Qt-Runtime-Bindings autoPointHandlesFromBatch/createdPointHandleIndexes, autoPointsFromLastQuery/queriedPointIndexes, autoPolylineHandlesFromBatch/createdPolylineHandleIndexes und autoPolylineHandlesFromLastQuery zusaetzlich erlaubt. Beispiele muessen konkrete, platzhalterfreie BRX-Aktionen enthalten."},
            {"editingPolicy", "Bei Bearbeitung immer den bestehenden Workflow erhalten und nur gezielt erweitern/veraendern."},
            {"slotPolicy", "requiredSlots ist eine Liste aus strings oder Objekten mit name/type/description und darf leer sein. Referenzierte Werte gehoeren als Slots hinein; fachlich zwingender Prozesskontext darf mit requiredForExecution=true ohne direkte Toolreferenz erhalten bleiben. Technikraumname, Bounds und Startpunkt sind in den Gebaeude-Workflows 03 bis 07 Pflichtkontext. optionalSlots ist immer ein Objekt nach slotName, kein Array."},
            {"draftPolicy", "Bei langen Erstprompts erst workflow_draft mit workflowDraft erzeugen. workflow_update nur nach Nutzerbestaetigung oder wenn der Nutzer explizit speichern/aktualisieren will."},
            {"calculationPolicy", "Formeln gehoeren in derivedValues als name/expression/dependsOn/unit/example. Schreibe konkrete Beispielwerte in knownSlotValues und validationExamples; Qt wertet expression nicht aus."},
            {"batchPolicy", "Komplexe Ablaufe gehoeren in executionBatches[].steps mit mode=sequential und stopOnFailure=true. Fuer workflow_update muss zusaetzlich workflow.steps die gleiche ausfuehrbare Sequenz flach enthalten."},
            {"validationPolicy", "workflow_update muss validationExamples[].actions mit konkreten, platzhalterfreien Beispielaktionen enthalten. Verkettete Beispiele duerfen lastResult nutzen, aber nicht eine leere selection voraussetzen. Fuer step-by-step Workflows nach einer Extrusion zuerst selection.set verwenden und danach bim.classify target=selection."},
            {"tableOutputPolicy", "Fuer Geometrie-Auswertung als Tabelle nutze read-only Tools geometry.query, selection.describe oder entity.describe. Setze include auf metrics und bei Bedarf geometry. limit muss >= 1 sein; nutze nie limit=0, sondern z.B. 100, 250 oder 500. Qt erzeugt aus result.objects eine Markdown-Tabelle fuer Workflow-Lauf und BricsCAD-Chat."},
        }},
        {"knownToolNames", toolNames},
        {"effectiveTools", effectiveTools},
        {"existingWorkflows", existingWorkflows},
        {"katexFormattingContract", katexFormattingContract()},
        {"trainingState", workflowTrainingState()},
        {"activeWorkflow", activeWorkflow},
        {"selectedWorkflow", selectedWorkflowSummary()},
        {"brxLearningSource", m_brxLearning.sourcePath()},
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
        const bool booleanBinding = (tool == QStringLiteral("geometry.query") && key == QStringLiteral("autoPointHandlesFromBatch"))
            || (tool == QStringLiteral("geometry.create") && key == QStringLiteral("autoPointsFromLastQuery"))
            || ((tool == QStringLiteral("pipes.validateNetwork") || tool == QStringLiteral("pipes.createNetworkSolids"))
                && (key == QStringLiteral("autoPolylineHandlesFromBatch")
                    || key == QStringLiteral("autoPolylineHandlesFromLastQuery")));
        const bool roomBinding = key == QStringLiteral("autoRoomHandlesFromLastQuery")
            && (tool == QStringLiteral("measurement.bbox") || tool == QStringLiteral("measurement.area")
                || tool == QStringLiteral("annotations.createRoomDimensions") || tool == QStringLiteral("geometry.query"));
        if (booleanBinding || roomBinding) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
        }

        const bool indexBinding = (tool == QStringLiteral("geometry.query") && key == QStringLiteral("createdPointHandleIndexes"))
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
    Q_UNUSED(workflow);
    if (savedPath) {
        savedPath->clear();
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("BricsCAD speichert keine Workflow-Dateien mehr. Nutze BRX Learning Updates.");
    }
    return false;
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
        "%4 "
        "Korrigiere bei Formatierungsfehlern nur die Formatierung; erhalte alle fachlichen Details, Tabellenwerte, Beispiele und Nutzerkorrekturen vollstaendig. "
        "Tabellen muessen als tables[] mit columns/rows oder als gueltige Markdown-Pipe-Tabelle mit |---|---|-Trennzeile ausgegeben werden. "
        "Listen muessen echte Markdown-Listen mit je einem Punkt pro Zeile sein. "
        "Nutze keine HTML-Tags wie <br>, keine tabulatorgetrennten Klartexttabellen und kein loses Sprachlabel 'text' vor Formeln.")
        .arg(errorMessage, rejectedSample, repeatedInstruction, katexFormattingInstructionText());

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
        {"content", QStringLiteral(
            "Du bist der Barebone-Qt Workflow-Autorenagent im Trainingsmodus. "
            "Legacy-Trainingsmodus ist deaktiviert; BricsCAD-Wissen liegt in agent/bricscad-learning/brx-learning.json. "
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
            "Fuer tabellarische BricsCAD-Auswertungen nutze geometry.query, selection.describe oder entity.describe mit limit >= 1; Qt kann result.objects als Markdown-Tabelle im Workflow-Lauf und Chatfenster ausgeben. "
            "%1 "
            "Wenn Geometrie in einem bestimmten Layer entstehen soll, setze in geometry.create.paramsTemplate.layer den exakten Layernamen; ein vorheriges layers.create reicht nicht aus, um den Zeichenlayer implizit umzuschalten. "
            "Speichere keine starren Prompt-zu-Command-Regeln, sondern Slots, Defaults, Strategien, Constraints, Beispiele und bevorzugte Tools. "
            "Falls compactContext=true ist, konzentriere dich auf eine kurze gueltige JSON-Antwort und vermeide lange Erklaerungen.").arg(katexFormattingInstructionText())},
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

void BricsCadPage::applyBrxLearningUpdateFromReply(const QJsonObject& reply, const QString& responseType)
{
    if (isChatWorkspace()) {
        return;
    }
    if (responseType == QStringLiteral("action_proposal")
        || responseType == QStringLiteral("workflow_run_proposal")) {
        return;
    }

    const QJsonObject update = reply.value(QStringLiteral("learningUpdate")).toObject();
    if (update.isEmpty()) {
        return;
    }

    QStringList changes;
    QString errorMessage;
    if (!m_brxLearning.applyLearningUpdate(update, &changes, &errorMessage)) {
        appendBridgeLog(QStringLiteral("BRX Learning Update abgelehnt: %1")
            .arg(errorMessage.isEmpty() ? QStringLiteral("ungueltiges learningUpdate") : errorMessage));
        return;
    }
    if (changes.isEmpty()) {
        return;
    }
    appendBridgeLog(QStringLiteral("BRX Learning aktualisiert: %1").arg(changes.join(QStringLiteral("; ")).left(500)));
    emitWorkflowListToWeb();
}

QJsonArray BricsCadPage::applyBrxLearningRuntimeEvent(const QJsonArray& lessonIds, const QJsonObject& event, const QString& logPrefix)
{
    QJsonArray affectedLessonIds = lessonIds;
    if (isChatWorkspace()) {
        return affectedLessonIds;
    }

    QStringList allChanges;
    QString errorMessage;
    if (!lessonIds.isEmpty()) {
        QStringList changes;
        if (!m_brxLearning.recordLessonUse(lessonIds, event, &changes, &errorMessage)) {
            appendBridgeLog(QStringLiteral("BRX Learning Gewichtung abgelehnt: %1")
                .arg(errorMessage.isEmpty() ? QStringLiteral("ungueltiges Runtime-Event") : errorMessage));
            return affectedLessonIds;
        }
        allChanges << changes;
    }

    QStringList runtimeLessonIds;
    QStringList runtimeChanges;
    errorMessage.clear();
    if (!m_brxLearning.upsertRuntimeLessonFromEvent(event, &runtimeLessonIds, &runtimeChanges, &errorMessage)) {
        appendBridgeLog(QStringLiteral("BRX Runtime-Lesson abgelehnt: %1")
            .arg(errorMessage.isEmpty() ? QStringLiteral("ungueltiges Runtime-Event") : errorMessage));
    } else {
        for (const QString& id : runtimeLessonIds) {
            appendJsonStringUnique(affectedLessonIds, id);
        }
        allChanges << runtimeChanges;
    }

    if (!allChanges.isEmpty()) {
        appendBridgeLog(QStringLiteral("%1: %2").arg(logPrefix, allChanges.join(QStringLiteral("; ")).left(700)));
        emitWorkflowListToWeb();
    }
    return affectedLessonIds;
}

QJsonArray BricsCadPage::brxLearningToolsFromActions(const QJsonArray& actions) const
{
    QJsonArray tools;
    for (const QJsonValue& value : actions) {
        const QString tool = value.toObject().value(QStringLiteral("tool")).toString().trimmed();
        appendJsonStringUnique(tools, tool);
    }
    return tools;
}

QJsonArray BricsCadPage::brxLearningRecentConversation(int maxMessages) const
{
    QJsonArray recent;
    if (maxMessages <= 0 || m_agentConversation.isEmpty()) {
        return recent;
    }

    auto previewText = [](QString text, int maxChars) {
        text = repairMojibakeText(text).trimmed();
        bool parsed = false;
        const QJsonObject parsedObject = jsonObjectFromAiContent(text, &parsed);
        if (parsed) {
            QString message = repairMojibakeText(parsedObject.value(QStringLiteral("message")).toString()).trimmed();
            if (message.isEmpty()) {
                message = repairMojibakeText(parsedObject.value(QStringLiteral("summary")).toString()).trimmed();
            }
            if (message.isEmpty()) {
                const QJsonObject result = parsedObject.value(QStringLiteral("result")).toObject();
                message = repairMojibakeText(result.value(QStringLiteral("summary")).toString()).trimmed();
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

    const int total = static_cast<int>(m_agentConversation.size());
    const int start = std::max(0, total - maxMessages);
    for (int i = start; i < total; ++i) {
        const QJsonObject message = m_agentConversation.at(i).toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) {
            continue;
        }
        const QString preview = previewText(message.value(QStringLiteral("content")).toString(), 900);
        if (preview.isEmpty()) {
            continue;
        }
        recent.append(QJsonObject{
            {QStringLiteral("index"), i},
            {QStringLiteral("role"), role},
            {QStringLiteral("content"), preview},
        });
    }
    return recent;
}

QJsonArray BricsCadPage::brxLearningLessonIdsForRoute(const QJsonObject& route, const QString& prompt) const
{
    QJsonArray lessonIds;
    for (const QString& id : routeWorkflowIds(route, 3)) {
        appendJsonStringUnique(lessonIds, id);
    }
    if (!lessonIds.isEmpty()) {
        return lessonIds;
    }

    for (const QJsonValue& value : m_brxLearning.relevantLessons(prompt, 3)) {
        appendJsonStringUnique(lessonIds, value.toObject().value(QStringLiteral("id")).toString());
    }
    return lessonIds;
}

void BricsCadPage::recordBrxLearningExecutionOutcome(
    const QJsonObject& proposal,
    const QJsonArray& actions,
    const QJsonObject& batchResult,
    const QString& fallbackSummary)
{
    if (!isBricsCadMode()) {
        return;
    }

    const QJsonArray lessonIds = brxLearningLessonIdsForRoute(m_lastAgentRoute, m_lastAgentUserPrompt);

    const QJsonArray tools = brxLearningToolsFromActions(actions);
    const int failures = batchResult.value(QStringLiteral("failed")).toInt(
        batchResult.value(QStringLiteral("executionStats")).toObject().value(QStringLiteral("failed")).toInt(0));
    const QString outcome = failures > 0
        ? QStringLiteral("execution_failure")
        : QStringLiteral("execution_success");
    QJsonObject event{
        {QStringLiteral("source"), QStringLiteral("brx_runtime")},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("prompt"), m_lastAgentUserPrompt},
        {QStringLiteral("summary"), fallbackSummary},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("actions"), actions},
        {QStringLiteral("selectedLessonIds"), lessonIds},
        {QStringLiteral("focusedConversationContext"), m_lastFocusedConversationContext},
        {QStringLiteral("recentConversation"), brxLearningRecentConversation(12)},
        {QStringLiteral("actionCount"), actions.size()},
        {QStringLiteral("failureCount"), failures},
        {QStringLiteral("proposalTitle"), proposal.value(QStringLiteral("title")).toString()},
        {QStringLiteral("executionStats"), batchResult.value(QStringLiteral("executionStats")).toObject()},
    };
    const QJsonArray affectedLessonIds = applyBrxLearningRuntimeEvent(lessonIds, event, QStringLiteral("BRX Learning Gewichtung aktualisiert"));

    m_lastBrxLearningInteraction = {};
    m_lastBrxLearningInteraction.lessonIds = affectedLessonIds;
    m_lastBrxLearningInteraction.tools = tools;
    m_lastBrxLearningInteraction.actions = actions;
    m_lastBrxLearningInteraction.prompt = m_lastAgentUserPrompt;
    m_lastBrxLearningInteraction.summary = fallbackSummary;
    m_lastBrxLearningInteraction.awaitingFeedback = true;
    m_lastBrxLearningInteraction.operationGeneration = m_operationGeneration;
}

void BricsCadPage::recordBrxLearningExecutionFailure(const QJsonObject& proposal, const QString& errorMessage)
{
    if (!isBricsCadMode()) {
        return;
    }

    QJsonArray actions = agentProposalActions(proposal);
    const QJsonObject failedAction = proposal.value(QStringLiteral("failedAction")).toObject();
    if (!failedAction.isEmpty()) {
        actions.append(failedAction);
    }

    QJsonArray lessonIds = brxLearningLessonIdsForRoute(m_lastAgentRoute, m_lastAgentUserPrompt);
    if (lessonIds.isEmpty() && !m_lastBrxLearningInteraction.lessonIds.isEmpty()) {
        lessonIds = m_lastBrxLearningInteraction.lessonIds;
    }

    const QJsonArray tools = brxLearningToolsFromActions(actions);
    QJsonObject event{
        {QStringLiteral("source"), QStringLiteral("brx_runtime")},
        {QStringLiteral("outcome"), QStringLiteral("execution_failure")},
        {QStringLiteral("prompt"), m_lastAgentUserPrompt},
        {QStringLiteral("summary"), errorMessage},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("actions"), actions},
        {QStringLiteral("selectedLessonIds"), lessonIds},
        {QStringLiteral("focusedConversationContext"), m_lastFocusedConversationContext},
        {QStringLiteral("recentConversation"), brxLearningRecentConversation(12)},
        {QStringLiteral("actionCount"), actions.size()},
        {QStringLiteral("failureCount"), 1},
        {QStringLiteral("proposalTitle"), proposal.value(QStringLiteral("title")).toString()},
    };
    const QJsonArray affectedLessonIds = applyBrxLearningRuntimeEvent(lessonIds, event, QStringLiteral("BRX Learning Fehlergewichtung aktualisiert"));

    m_lastBrxLearningInteraction = {};
    m_lastBrxLearningInteraction.lessonIds = affectedLessonIds;
    m_lastBrxLearningInteraction.tools = tools;
    m_lastBrxLearningInteraction.actions = actions;
    m_lastBrxLearningInteraction.prompt = m_lastAgentUserPrompt;
    m_lastBrxLearningInteraction.summary = errorMessage;
    m_lastBrxLearningInteraction.awaitingFeedback = true;
    m_lastBrxLearningInteraction.operationGeneration = m_operationGeneration;
}

QString BricsCadPage::brxLearningFeedbackOutcome(const QString& prompt) const
{
    QString text = repairMojibakeText(prompt).toLower().simplified();
    text.replace(QStringLiteral("ä"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ö"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ü"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ß"), QStringLiteral("ss"));
    text.replace(QRegularExpression(QStringLiteral(R"([^\w\s]+)")), QStringLiteral(" "));
    text = text.simplified();
    if (text.isEmpty()) {
        return {};
    }

    const QStringList negativePhrases{
        QStringLiteral("nicht"),
        QStringLiteral("falsch"),
        QStringLiteral("fehler"),
        QStringLiteral("problem"),
        QStringLiteral("passt nicht"),
        QStringLiteral("klappt nicht"),
        QStringLiteral("geht nicht"),
        QStringLiteral("ging nicht"),
        QStringLiteral("funktioniert nicht"),
        QStringLiteral("hat nicht geklappt"),
        QStringLiteral("wurde nicht"),
        QStringLiteral("sollte"),
        QStringLiteral("statt"),
        QStringLiteral("korrigiere"),
        QStringLiteral("falsche"),
        QStringLiteral("falscher"),
        QStringLiteral("falschen"),
        QStringLiteral("nein"),
    };
    for (const QString& phrase : negativePhrases) {
        if (text.contains(phrase)) {
            return QStringLiteral("user_complaint");
        }
    }

    const QStringList positivePhrases{
        QStringLiteral("ja"),
        QStringLiteral("ok"),
        QStringLiteral("okay"),
        QStringLiteral("passt"),
        QStringLiteral("korrekt"),
        QStringLiteral("richtig"),
        QStringLiteral("hat geklappt"),
        QStringLiteral("funktioniert"),
        QStringLiteral("erfolgreich"),
        QStringLiteral("alles gut"),
    };
    for (const QString& phrase : positivePhrases) {
        if (text == phrase || text.contains(phrase)) {
            return QStringLiteral("user_positive");
        }
    }
    return {};
}

void BricsCadPage::evaluatePendingBrxLearningFeedback(const QString& prompt)
{
    if (!isBricsCadMode() || !m_lastBrxLearningInteraction.awaitingFeedback) {
        return;
    }

    const QString outcome = brxLearningFeedbackOutcome(prompt);
    if (outcome.isEmpty()) {
        m_lastBrxLearningInteraction.awaitingFeedback = false;
        return;
    }

    QJsonObject event{
        {QStringLiteral("source"), QStringLiteral("user_feedback")},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("prompt"), m_lastBrxLearningInteraction.prompt},
        {QStringLiteral("feedback"), prompt},
        {QStringLiteral("summary"), m_lastBrxLearningInteraction.summary},
        {QStringLiteral("tools"), m_lastBrxLearningInteraction.tools},
        {QStringLiteral("actions"), m_lastBrxLearningInteraction.actions},
        {QStringLiteral("operationGeneration"), m_lastBrxLearningInteraction.operationGeneration},
    };
    applyBrxLearningRuntimeEvent(
        m_lastBrxLearningInteraction.lessonIds,
        event,
        outcome == QStringLiteral("user_complaint")
            ? QStringLiteral("BRX Learning Nutzerkritik bewertet")
            : QStringLiteral("BRX Learning Nutzerbestaetigung bewertet"));
    m_lastBrxLearningInteraction.awaitingFeedback = false;
}

void BricsCadPage::requestMathFormattingRepair(const QString& messageId, int revision, const QString& markdown, const QString& diagnosticsJson)
{
    const QString normalizedMessageId = messageId.trimmed();
    const QString cleanMarkdown = repairMojibakeText(markdown).trimmed();
    if (normalizedMessageId.isEmpty() || cleanMarkdown.isEmpty()) {
        if (m_agentBridge) {
            emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, QStringLiteral("Math-Reparatur braucht messageId und Markdown."));
        }
        return;
    }

    PendingMathFormattingRepair pending = m_pendingMathFormattingRepairs.value(normalizedMessageId);
    if (pending.originalMarkdown.trimmed().isEmpty()) {
        pending.originalMarkdown = cleanMarkdown;
    }
    if (pending.sessionId.trimmed().isEmpty()) {
        pending.sessionId = m_agentSessionId;
    }
    pending.diagnosticsJson = diagnosticsJson;
    pending.revision = revision;
    ++pending.attempts;
    if (pending.attempts > kMaxMathFormattingRepairAttempts) {
        m_pendingMathFormattingRepairs.insert(normalizedMessageId, pending);
        if (m_agentBridge) {
            emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, QStringLiteral("Math-Reparatur nach zwei Versuchen abgebrochen."));
        }
        return;
    }
    m_pendingMathFormattingRepairs.insert(normalizedMessageId, pending);

    const QString provider = m_config.aiProvider();
    const bool officialProvider = provider == "official";
    const QString baseUrl = normalizedAiBaseUrl(m_config.aiBaseUrl(), provider);
    const QString model = m_config.aiModel().trimmed().isEmpty()
        ? (officialProvider ? QStringLiteral("gpt-5.5") : QStringLiteral("openai/gpt-oss-20b"))
        : m_config.aiModel().trimmed();
    const bool useResponsesApi = useResponsesApiForProvider(provider, model);
    const QUrl url(baseUrl + (useResponsesApi ? QStringLiteral("/responses") : QStringLiteral("/chat/completions")));
    if (!url.isValid()) {
        if (m_agentBridge) {
            emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, QStringLiteral("Ungueltige AI Server URL: %1").arg(baseUrl));
        }
        return;
    }
    if (officialProvider && m_config.aiApiKey().trimmed().isEmpty()) {
        if (m_agentBridge) {
            emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, QStringLiteral("Offizielle ChatGPT API ist aktiv, aber es ist kein Secret Key gespeichert."));
        }
        return;
    }

    QJsonParseError diagnosticsParseError;
    const QJsonDocument diagnosticsDocument = QJsonDocument::fromJson(diagnosticsJson.toUtf8(), &diagnosticsParseError);
    QJsonValue diagnosticsValue;
    if (diagnosticsParseError.error == QJsonParseError::NoError) {
        if (diagnosticsDocument.isArray()) {
            diagnosticsValue = diagnosticsDocument.array();
        } else if (diagnosticsDocument.isObject()) {
            diagnosticsValue = diagnosticsDocument.object();
        }
    }
    if (diagnosticsValue.isUndefined()) {
        diagnosticsValue = diagnosticsJson.left(8000);
    }

    QJsonObject repairRequest{
        {"schema", "barebone.katex.repair.request.v1"},
        {"messageId", normalizedMessageId},
        {"revision", revision},
        {"attempt", pending.attempts},
        {"markdown", cleanMarkdown},
        {"katexDiagnostics", diagnosticsValue},
        {"katexFormattingContract", katexFormattingContract()},
    };
    QJsonArray messages{
        QJsonObject{
            {"role", "system"},
            {"content", QStringLiteral(
                "Du bist ein reiner KaTeX-/Markdown-Formatierungsreparaturdienst fuer Barebone-Qt. "
                "Antworte ausschliesslich mit genau einem JSON-Objekt nach schema barebone.katex.repair.response.v1: "
                "{\"schema\":\"barebone.katex.repair.response.v1\",\"markdown\":\"...\"}. "
                "Gib keine Erklaerung, keinen Markdown-Codeblock und keine Felder ausserhalb dieses JSON aus. "
                "Repariere die komplette Nachricht gemaess katexFormattingContract und katexDiagnostics. "
                "Aendere keine fachlichen Inhalte, Zahlen, Einheiten, Berechnungen oder Aussagen. "
                "Wenn eine Formel fehlerhaft ist, rekonstruiere nur ihre KaTeX-kompatible Schreibweise. "
                "Der Rueckgabewert markdown muss die vollstaendige reparierte Nachricht enthalten.")},
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
        payload.insert("max_output_tokens", adjustedOutputTokenLimitForMessages(messages, 16384));
        const QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (reasoningEffort != "none") {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort == "high" ? QStringLiteral("low") : reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.0);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("max_tokens", adjustedOutputTokenLimitForMessages(messages, 16384));
        payload.insert("temperature", 0.0);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(kAiModelResponseTimeoutMs);

    appendBridgeLog(QString("Qt -> AI KaTeX Repair: message=%1 revision=%2 attempt=%3 diagnostics=%4")
        .arg(normalizedMessageId)
        .arg(revision)
        .arg(pending.attempts)
        .arg(diagnosticsJson.left(500)));

    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, normalizedMessageId, revision]() {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = QStringLiteral("KaTeX-Reparatur fehlgeschlagen: http=%1 %2")
                .arg(httpStatus)
                .arg(reply->errorString());
            appendBridgeLog(error);
            if (m_agentBridge) {
                emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, error);
            }
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument responseDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
            const QString error = QStringLiteral("KaTeX-Reparaturantwort ist kein gueltiges OpenAI JSON: %1").arg(parseError.errorString());
            appendBridgeLog(error);
            if (m_agentBridge) {
                emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, error);
            }
            reply->deleteLater();
            return;
        }

        QString reasoningText;
        QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText)).trimmed();
        bool parsed = false;
        QJsonObject repairObject = jsonObjectFromAiContent(content, &parsed);
        QString repairedMarkdown = parsed
            ? repairObject.value(QStringLiteral("markdown")).toString().trimmed()
            : QString();
        if (repairedMarkdown.isEmpty()) {
            content.remove(QRegularExpression(QStringLiteral(R"(^```(?:json|markdown|md|text)?\s*)"), QRegularExpression::CaseInsensitiveOption));
            content.remove(QRegularExpression(QStringLiteral(R"(\s*```$)")));
            repairedMarkdown = content.trimmed();
        }
        repairedMarkdown = repairMojibakeText(repairedMarkdown).trimmed();
        if (repairedMarkdown.isEmpty()) {
            const QString error = QStringLiteral("KaTeX-Reparaturantwort enthaelt keinen Markdown.");
            appendBridgeLog(error);
            if (m_agentBridge) {
                emitWebMathFormattingRepairFailed(m_agentBridge, normalizedMessageId, revision, error);
            }
            reply->deleteLater();
            return;
        }

        appendBridgeLog(QString("AI KaTeX Repair: Kandidat fuer message=%1 revision=%2 chars=%3")
            .arg(normalizedMessageId)
            .arg(revision)
            .arg(repairedMarkdown.size()));
        if (m_agentBridge) {
            emitWebMathFormattingRepairCompleted(m_agentBridge, normalizedMessageId, revision, repairedMarkdown);
        }
        reply->deleteLater();
    });
}

void BricsCadPage::acceptMathFormattingRepair(const QString& messageId, int revision, const QString& markdown)
{
    const QString normalizedMessageId = messageId.trimmed();
    auto it = m_pendingMathFormattingRepairs.find(normalizedMessageId);
    if (it == m_pendingMathFormattingRepairs.end()) {
        appendBridgeLog(QString("KaTeX Repair: keine ausstehende Reparatur fuer message=%1").arg(normalizedMessageId));
        return;
    }
    if (it->revision != revision) {
        appendBridgeLog(QString("KaTeX Repair: veraltete Bestaetigung ignoriert message=%1 revision=%2 expected=%3")
            .arg(normalizedMessageId)
            .arg(revision)
            .arg(it->revision));
        return;
    }

    const QString repairedMarkdown = repairMojibakeText(markdown).trimmed();
    if (!repairedMarkdown.isEmpty()) {
        const bool replaced = replaceAssistantConversationMessage(it->sessionId, it->originalMarkdown, repairedMarkdown);
        appendBridgeLog(QString("KaTeX Repair: message=%1 uebernommen conversationUpdated=%2")
            .arg(normalizedMessageId)
            .arg(replaced ? QStringLiteral("true") : QStringLiteral("false")));
        if (it->sessionId == m_agentSessionId) {
            saveCurrentAgentSession();
        }
        emitContextBudget();
    }
    m_pendingMathFormattingRepairs.erase(it);
}

bool BricsCadPage::replaceAssistantConversationMessage(const QString& sessionId, const QString& originalMarkdown, const QString& repairedMarkdown)
{
    if (sessionId == m_agentSessionId || sessionId.trimmed().isEmpty()) {
        return replaceAssistantConversationMessageIn(m_agentConversation, originalMarkdown, repairedMarkdown);
    }
    if (!m_agentSessions.contains(sessionId)) {
        return false;
    }
    AgentSessionState state = m_agentSessions.value(sessionId);
    const bool replaced = replaceAssistantConversationMessageIn(state.conversation, originalMarkdown, repairedMarkdown);
    if (replaced) {
        m_agentSessions.insert(sessionId, state);
    }
    return replaced;
}

bool BricsCadPage::replaceAssistantConversationMessageIn(QJsonArray& conversation, const QString& originalMarkdown, const QString& repairedMarkdown) const
{
    const QString original = repairMojibakeText(originalMarkdown).trimmed();
    const QString repaired = repairMojibakeText(repairedMarkdown).trimmed();
    if (original.isEmpty() || repaired.isEmpty()) {
        return false;
    }

    for (int i = conversation.size() - 1; i >= 0; --i) {
        QJsonObject item = conversation.at(i).toObject();
        if (item.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")) {
            continue;
        }

        const QString content = item.value(QStringLiteral("content")).toString();
        if (repairMojibakeText(removeReasoningLeak(content)).trimmed() == original) {
            item.insert(QStringLiteral("content"), repaired);
            conversation.replace(i, item);
            return true;
        }

        bool parsed = false;
        QJsonObject parsedObject = jsonObjectFromAiContent(content, &parsed);
        if (parsed && repairMojibakeText(parsedObject.value(QStringLiteral("message")).toString()).trimmed() == original) {
            parsedObject.insert(QStringLiteral("message"), repaired);
            item.insert(QStringLiteral("content"), QString::fromUtf8(QJsonDocument(parsedObject).toJson(QJsonDocument::Compact)));
            conversation.replace(i, item);
            return true;
        }
    }

    return false;
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
        {"katexFormattingContract", katexFormattingContract()},
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
                "Unterteile nur grob nach Absätzen; fachliche Tiefe gehoert in blocks[].text, nicht in eine komplexe JSON-Verschachtelung. "
                "%1 "
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
                "Wenn Werte AI-Entwurf sind, setze verificationStatus='AI-Entwurf'.").arg(katexFormattingInstructionText())},
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

    applyBrxLearningUpdateFromReply(reply, type);

    if (type == "action_proposal") {
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
        const bool workflowRunProposal = false;
        QJsonObject proposal = normalizedAgentProposal(reply);
        if (!message.trimmed().isEmpty()) {
            proposal.insert("summary", message.trimmed());
        }
        proposal = normalizedRectangularRoomWallProposal(proposal, m_lastAgentUserPrompt);
        if (!sessionTitleSuggestion.isEmpty()) {
            proposal.insert(QStringLiteral("sessionTitleSuggestion"), sessionTitleSuggestion);
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
        if (m_agentContextLoopBlocked && isSystemRouteWorkflowId(m_selectedWorkflowId)) {
            ++m_repeatedAgentContextRequestCount;
            if (m_repeatedAgentContextRequestCount > 3) {
                appendBridgeLog("AI Kontextloop Workflow 05: nach drei identischen leeren POINT-Abfragen gestoppt");
                appendAgentChat("Barebone-Qt", "Workflow 05 wurde gestoppt: Die AI hat trotz Loop-Guard erneut dieselbe leere POINT-Kontextabfrage angefordert. Bitte den Workflow erneut starten; Qt fordert dann direkt den Punkt-Erzeugungsvorschlag an.");
                setAgentBusy(false);
                return;
            }
            retryAgentAfterValidationFailure(
                content,
                reply,
                QStringLiteral("Workflow 05 Kontextloop: Vorhandene POINTS wurden bereits mit count=0 geprueft. Keine weitere Kontextabfrage senden. Erzeuge jetzt die benoetigten Layer und POINTS und liefere danach den action_proposal."));
            return;
        }
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
        if (!sessionTitleSuggestion.isEmpty()) {
            emitSessionTitleSuggestion(sessionTitleSuggestion);
        }
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
            if (!sessionTitleSuggestion.isEmpty()) {
                emitSessionTitleSuggestion(sessionTitleSuggestion);
            }
            appendAgentChat("AI", message, sessionTitleExtra);
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

    const QJsonObject repairGuidance = validationRepairGuidance(errorMessage);
    if (repairGuidance.value(QStringLiteral("category")).toString() == QStringLiteral("missing_layer")) {
        QStringList selectedTools = stringsFromJsonArray(m_lastAgentRoute.value(QStringLiteral("selectedTools")).toArray());
        for (const QString& tool : {QStringLiteral("layers.create"), QStringLiteral("layers.ensureMany")}) {
            if (!selectedTools.contains(tool)) {
                selectedTools.append(tool);
            }
        }
        m_lastAgentRoute.insert(QStringLiteral("selectedTools"), stringsToJsonArray(selectedTools));
        m_lastAgentRoute.insert(QStringLiteral("toolSelectionAttempted"), true);
        appendBridgeLog(QString("AI Agent Repair: fehlender Layer '%1' erkannt; verbindliche Reparatur layers.create -> urspruengliche Aktion")
            .arg(repairGuidance.value(QStringLiteral("layerName")).toString()));
    }

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
    if (!repairGuidance.isEmpty()) {
        envelope.insert(QStringLiteral("repairGuidance"), repairGuidance);
    }
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
        "Setze sessionTitle direkt auf Top-Level des Antwortobjekts, nicht in proposal, draft, metadata oder learningUpdate. "
        "Nutze keinen freien Plan und keine Pseudo-Actions. Wenn die Aufgabe ausfuehrbar ist, liefere genau einen action_proposal fuer den naechsten sicheren Schritt. "
        "Nutze im BricsCAD-Modus kein workflow_run_proposal; korrigiere intern und plane mit effectiveTools und brxLearningContext. "
        "Direkte BricsCAD-DB-Schreibvorgaenge, AcDb-/LayerTable-/EntityTable-Mutationen und Pseudo-Tools fuer DB-Writes sind verboten; nutze ausschliesslich tools[].name. "
        "Wenn mehrere unabhaengige Aktionen mit bekannten Parametern erforderlich sind, liefere ein action_proposal mit proposal.actions:[{\"tool\":\"...\",\"params\":{...}},...] und proposal.continueAfterSuccess=false. "
        "Fuer mehrere Layer mit Namen/Farben nutze bevorzugt layers.ensureMany mit params.layers. "
        "Nutze continueAfterSuccess nicht, um Batch-Aufgaben wie mehrere Layer oder mehrere gleichartige Objekte einzeln nachzufordern. "
        "tool muss exakt einem tools[].name entsprechen. params muessen inputSchema/apiDoc.post erfuellen. "
        "Wenn validationError mit BRX Preflight beginnt, wiederhole nicht denselben Vorschlag; nutze die dort genannten Fehler, fehlenden Daten und Hinweise verbindlich. ");
    if (!repairGuidance.isEmpty()) {
        retryInstruction += QStringLiteral(
            "repairGuidance ist eine verbindliche, maschinenlesbare Reparaturanweisung. "
            "Bei category=missing_layer erstelle den Layer exakt mit repairGuidance.layerName und layers.create oder layers.ensureMany. "
            "Ordne diese Aktion im selben proposal.actions-Array vor allen Aktionen an, die den Layer referenzieren; fuehre danach die urspruengliche Fachaktion aus. "
            "Die Eintraege in forbiddenWorkarounds sind verboten. Frage den Nutzer fuer diesen reparierbaren Fehler nicht nach dem Layernamen. ");
    }
    if (repeatedResponse) {
        retryInstruction += QStringLiteral("Deine letzte Antwort war strukturell identisch zu einer bereits abgelehnten Antwort. Aendere Toolwahl, Params, stepPlan oder frage gezielt nach; dieselbe Antwort ist verboten. ");
    }
    if (errorMessage.contains(QStringLiteral("layers.ensureMany braucht params.layers"), Qt::CaseInsensitive)
        && isSystemRouteWorkflowId(m_selectedWorkflowId)) {
        retryInstruction += QStringLiteral(
            "Fuer einen Fachnetzworkflow ist layers.ensureMany mit leerem params.layers verboten. "
            "Erzeuge nur die in der ausgewaehlten Lesson genannten Planungs- und Leitungslayer dieses Gewerks. ");
    }
    if (m_agentContextLoopBlocked && isSystemRouteWorkflowId(m_selectedWorkflowId)) {
        retryInstruction += QStringLiteral(
            "Weitere context_request Antworten sind fuer diesen Lauf verboten: geometry.query hat bereits bestaetigt, dass noch keine POINTS existieren. "
            "Das ist der erwartete Ausgangszustand. Erzeuge jetzt Layer und POINTS gemaess Workflow 05 und liefere den action_proposal. ");
    } else {
        retryInstruction += QStringLiteral(
            "Wenn du Sitzungsverlauf brauchst, nutze context_request mit einer Methode aus conversationAccess.allowedMethods. "
            "Wenn du Zeichnungskontext brauchst, nutze context_request mit exakt einer readOnlyMethods[].name Methode. ");
    }
    retryInstruction += QStringLiteral("Wenn echte Informationen fehlen, nutze ask_user mit missing und einem draft. Wenn die Anfrage allgemein ist, nutze type=message.");
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
    bool runtimeHandleResultPending = false;
    for (const QJsonValue& value : agentProposalActions(proposal)) {
        const QJsonObject action = value.toObject();
        const QJsonObject actionParams = action.value(QStringLiteral("params")).toObject();
        const bool resolvesRuntimeHandles = actionParams.value(QStringLiteral("autoHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPointHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPointsFromLastQuery")).toBool(false)
            || actionParams.value(QStringLiteral("autoPolylineHandlesFromBatch")).toBool(false)
            || actionParams.value(QStringLiteral("autoPolylineHandlesFromLastQuery")).toBool(false)
            || actionParams.value(QStringLiteral("autoRoomHandlesFromLastQuery")).toBool(false)
            || actionParams.contains(QStringLiteral("createdGeometryHandleIndex"))
            || !actionParams.value(QStringLiteral("createdGeometryHandleIndexes")).toArray().isEmpty();
        const QString selectorScope = actionParams.value(QStringLiteral("selector")).toObject()
            .value(QStringLiteral("scope")).toString().trimmed();
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
            {"tool", action.value("tool").toString()},
            {"params", actionParams},
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
    if (tool == QStringLiteral("entity.setName")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        return name.isEmpty() ? QStringLiteral("Entity benannt") : QString("Entity als \"%1\" benannt").arg(name);
    }
    if (tool == QStringLiteral("geometry.query")
        || tool == QStringLiteral("selection.describe")
        || tool == QStringLiteral("entity.describe")) {
        const int count = result.value(QStringLiteral("count")).toInt(result.value(QStringLiteral("objects")).toArray().size());
        return QString("%1 Objekt%2 gefunden").arg(count).arg(count == 1 ? QString() : QStringLiteral("e"));
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
                m_agentValidationRetries = 0;
                setAgentBusy(false);
                setAgentProposal(readyProposal);
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
    QJsonObject normalizedRequest = request;
    const QString method = request.value("method").toString().trimmed();
    QJsonObject params = request.value("params").toObject();
    if (isSessionHistoryContextMethod(method)) {
        appendBridgeLog(QString("AI -> Qt Sitzungsverlauf: %1 %2")
            .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
        const QJsonObject response = sessionHistoryContextResponse(method, params);
        const int count = response.value(QStringLiteral("messages")).toArray().size();
        appendBridgeLog(QString("Qt -> AI Sitzungsverlauf: %1 count=%2").arg(method).arg(count));
        continueAgentWithContextResult(request, response);
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
    const bool sessionHistoryContext = isSessionHistoryContextMethod(contextRequest.value(QStringLiteral("method")).toString());
    if (!sessionHistoryContext
        && (route.value("route").toString() == QStringLiteral("general_chat")
            || route.value("route").toString() == QStringLiteral("document_qa"))) {
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
    if (sessionHistoryContext) {
        envelope.insert("instruction", "Nutze den nachgeladenen Sitzungsverlauf nur als Zusatzkontext. Antworte danach final mit message oder fordere gezielt weiteren session.history Kontext an, wenn weiterhin konkrete Verlaufsteile fehlen.");
        appendBridgeLog("AI Agent: nutze nachgeladenen Sitzungsverlauf als Zusatzkontext");
    } else {
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

        if (isSystemRouteWorkflowId(m_selectedWorkflowId) && count == 0) {
            m_agentContextLoopBlocked = m_repeatedAgentContextRequestCount >= 2;
            if (m_agentContextLoopBlocked) {
                QJsonObject responseContract = envelope.value(QStringLiteral("responseContract")).toObject();
                QJsonArray allowedTypes;
                for (const QJsonValue& value : responseContract.value(QStringLiteral("activeAllowedTypes")).toArray()) {
                    if (value.toString() != QStringLiteral("context_request")) {
                        allowedTypes.append(value);
                    }
                }
                responseContract.insert(QStringLiteral("activeAllowedTypes"), allowedTypes);
                responseContract.insert(QStringLiteral("contextLoopPolicy"), QStringLiteral(
                    "Keine weitere Kontextabfrage: Workflow 05 muss jetzt Layer und POINTS erzeugen und action_proposal liefern."));
                envelope.insert(QStringLiteral("responseContract"), responseContract);
            }
            envelope.insert(QStringLiteral("contextLoopGuard"), QJsonObject{
                {QStringLiteral("workflowId"), m_selectedWorkflowId},
                {QStringLiteral("requestSignature"), signature},
                {QStringLiteral("emptyResultCount"), m_repeatedAgentContextRequestCount},
                {QStringLiteral("furtherIdenticalRequestsAllowed"), false},
                {QStringLiteral("meaning"), QStringLiteral("Keine vorhandenen POINTS ist der erwartete Ausgangszustand; Workflow 05 muss sie neu erzeugen.")},
            });
            envelope.insert(QStringLiteral("instruction"), QStringLiteral(
                "geometry.query hat keine vorhandenen POINTS gefunden. Wiederhole diese Abfrage nicht. "
                "Das ist fuer Workflow 05 kein fehlender Nutzerwert, sondern der erwartete Ausgangszustand. "
                "Erzeuge die sechs konkreten Layer, danach die System-POINTS, lies nur deren Runtime-Handles zurueck und liefere jetzt einen action_proposal. "
                "layers.ensureMany mit leerem params.layers ist verboten."));
        }
    }

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, "context_result");
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
        m_pendingAgentDraft = {};
        m_agentValidationRetries = 0;
        clearAgentProposal();

        appendBridgeLog(QString("BRX Batch: %1").arg(QString::fromUtf8(QJsonDocument(batchResult).toJson(QJsonDocument::Compact)).left(1600)));

        const QString finalSummary = agentCompletionSummary(actions, results, resultSummary);
        recordBrxLearningExecutionOutcome(proposal, actions, batchResult, finalSummary);

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
    envelope.insert("toolResponse", compactBrxResponseForAgent(response));
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
    const QStringList previousLessonIds = routeWorkflowIds(m_lastAgentRoute, 3);
    recordBrxLearningExecutionFailure(proposal, errorMessage);

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
    if (!previousLessonIds.isEmpty()) {
        m_lastAgentRoute.insert(QStringLiteral("selectedWorkflows"), stringsToJsonArray(previousLessonIds));
        m_lastAgentRoute.insert(QStringLiteral("workflowSelectionAttempted"), true);
    }
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
        "Wenn echte Informationen fehlen, nutze ask_user. Wenn Sitzungsverlauf fehlt, nutze context_request mit conversationAccess.allowedMethods. Wenn Zeichnungskontext fehlt, nutze context_request. "
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
        emitWebProposal(m_agentBridge, QVariantMap{
            {"title", actions.size() > 1 ? "AI Batch-Vorschlag bereit" : "AI Vorschlag bereit"},
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

    if (!m_workflowTestActions.isEmpty()) {
        finishBrxWorkflowTest(QStringLiteral("cancelled"), QStringLiteral("Workflowtest wurde durch den Nutzer abgebrochen."));
    }

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
            errorMessage = "Batch-Vorschlaege duerfen keine automatische Folgeausfuehrung mit continueAfterSuccess/nextIntent verwenden";
            return false;
        }
        bool priorCreatedGeometryInBatch = false;
        for (int i = 0; i < actions.size(); ++i) {
            const QJsonObject action = actions.at(i).toObject();
            const QString actionTool = action.value(QStringLiteral("tool")).toString();
            const QJsonObject actionParams = action.value(QStringLiteral("params")).toObject();
            if (actionTool == QStringLiteral("rectangles.extrude") || actionTool == QStringLiteral("profile.extrude")) {
                const QString targetLayer = actionParams.value(QStringLiteral("layer")).toString().trimmed();
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
            if (priorCreatedGeometryInBatch
                && promptLooksLikeRectangularRoomWallRun(m_lastAgentUserPrompt)
                && (actionTool == QStringLiteral("rectangles.extrude")
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
    if (promptRequestsEntityRename(activePrompt)
        && (tool == QStringLiteral("layers.rename") || tool == QStringLiteral("command.execute"))) {
        errorMessage = "Entity-/BIM-Wand-Umbenennung ist kein Layer-Umbenennen und aktuell nicht als freigegebenes BRX-Action-Tool vorhanden. Nutze keine Layer-Rename- oder Native-Command-Ersatzaktion; antworte mit message/plan oder hole nur read-only Kontext.";
        return false;
    }

    if (tool == QStringLiteral("geometry.rotate")
        && !promptMentionsRotationAngle(activePrompt)
        && (params.contains(QStringLiteral("angleDeg")) || params.contains(QStringLiteral("angleRad")))) {
        errorMessage = "geometry.rotate darf angleDeg/angleRad nicht aus Beispielen, Lessons oder Defaults raten. Wenn der Nutzer keinen Rotationswinkel nennt, frage mit ask_user gezielt nach dem Winkel.";
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
    if (tool == QStringLiteral("layers.create")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        if (!hasBricsCadSubjectTopicLayerStructure(name)) {
            errorMessage = "layers.create.params.name muss die Struktur 'Fachgebiet Thema' aus mindestens zwei Woertern besitzen; Sonderzeichen wie '-' und '_' sind verboten.";
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
        if (!hasBricsCadSubjectTopicLayerStructure(name)) {
            errorMessage = QString("layers.ensureMany.layers[%1].name muss die Struktur 'Fachgebiet Thema' besitzen; '-' und '_' sind verboten").arg(i);
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
        const QString name = method.value("name").toString();
        if (name.isEmpty()) {
            continue;
        }
        QJsonObject tool;
        tool.insert("name", name);
        tool.insert("title", name);
        tool.insert("description", method.value("description").toString());
        tool.insert("bridgeMethod", name);
        const QString risk = method.value("risk").toString("modifiesDrawing");
        tool.insert("kind", method.value("kind").toString(risk == QStringLiteral("readOnly") ? QStringLiteral("query") : QStringLiteral("action")));
        tool.insert("risk", method.value("risk").toString("modifiesDrawing"));
        tool.insert("category", method.value("category").toString("bridge"));
        tool.insert("resultSchema", method.value("resultSchema").toString());
        tool.insert("confirmationRequired", method.value("risk").toString() != "readOnly");
        tool.insert("inputSchema", method.value("paramsSchema").toObject(QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}));
        if (method.contains("apiDoc")) {
            tool.insert("apiDoc", method.value("apiDoc").toObject());
        }
        enrichAgentToolDefinition(tool, m_brxLearning.toolProfile(name));
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
        QStringList selectedToolNames = selectedTools;
        if (selectedToolNames.contains(QStringLiteral("rectangles.extrude"))
            || selectedToolNames.contains(QStringLiteral("profile.extrude"))) {
            if (!selectedToolNames.contains(QStringLiteral("entity.setLayer"))) {
                selectedToolNames << QStringLiteral("entity.setLayer");
            }
        }
        if (!isChatWorkspace() && promptRequestsCadDataQuery(prompt)) {
            for (const QString& toolName : QStringList{
                     QStringLiteral("geometry.query"),
                     QStringLiteral("measurement.bbox"),
                     QStringLiteral("measurement.length"),
                     QStringLiteral("measurement.area"),
                     QStringLiteral("entity.describe"),
                     QStringLiteral("selection.describe")}) {
                if (!selectedToolNames.contains(toolName)) {
                    selectedToolNames << toolName;
                }
            }
        }
        const QJsonArray selected = agentToolsByNames(selectedToolNames);
        if (!selected.isEmpty()) {
            return selected;
        }
    }

    if (isChatWorkspace()) {
        return {};
    }

    const QString normalized = workflowTrainingSearchText(prompt);
    QStringList selectedWorkflowTools;
    QJsonArray selectedWorkflowToolValues = m_selectedWorkflow.value("recommendedTools").toArray();
    if (selectedWorkflowToolValues.isEmpty()) {
        selectedWorkflowToolValues = m_selectedWorkflow.value("preferredTools").toArray();
    }
    for (const QJsonValue& value : selectedWorkflowToolValues) {
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
    for (const QJsonValue& lessonValue : selectedLearningLessonsForRoute(route, prompt)) {
        const QJsonObject lesson = lessonValue.toObject();
        for (const QString& tool : workflowToolNamesForSelector(lesson, 12)) {
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
    const bool handleReferenceIntent = QRegularExpression(
        QStringLiteral(R"(\b[a-f][0-9a-f]{1,7}\b)"),
        QRegularExpression::CaseInsensitiveOption).match(normalized).hasMatch();
    const bool dataQueryIntent = promptRequestsCadDataQuery(normalized);
    const bool geometryIntent = handleReferenceIntent || dataQueryIntent || textMentionsAny(normalized, {
        QStringLiteral("geometrie"),
        QStringLiteral("kreis"),
        QStringLiteral("rechteck"),
        QStringLiteral("linie"),
        QStringLiteral("polyline"),
        QStringLiteral("wand"),
        QStringLiteral("objekt"),
        QStringLiteral("object"),
        QStringLiteral("entity"),
        QStringLiteral("handle"),
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
        QStringLiteral("weise"),
        QStringLiteral("zuweis"),
        QStringLiteral("zuordn"),
        QStringLiteral("setze"),
        QStringLiteral("loesche"),
        QStringLiteral("lösche"),
        QStringLiteral("auswahl"),
        QStringLiteral("selekt"),
        QStringLiteral("waehle"),
        QStringLiteral("select"),
    });
    const bool entityRenameIntent = promptRequestsEntityRename(normalized);
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
        QStringLiteral("waehle"),
        QStringLiteral("select"),
        QStringLiteral("alle"),
        QStringLiteral("layer"),
    });
    const bool entityLayerIntent = layerIntent && geometryIntent && textMentionsAny(normalized, {
        QStringLiteral("weise"),
        QStringLiteral("zuweis"),
        QStringLiteral("zuordn"),
        QStringLiteral("setze"),
        QStringLiteral("assign"),
        QStringLiteral("auf layer"),
        QStringLiteral("in layer"),
        QStringLiteral("layer zu"),
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
        const bool readOnlyTool = tool.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")
            || tool.value(QStringLiteral("kind")).toString() == QStringLiteral("query");
        bool include = false;
        if (layerIntent && !entityRenameIntent) {
            include = name.startsWith(QStringLiteral("layers."))
                || name == QStringLiteral("layers.ensureMany")
                || readOnlyTool
                || name == QStringLiteral("command.execute");
        }
        if (geometryIntent) {
            include = include
                || readOnlyTool
                || (createIntent && name == QStringLiteral("geometry.create"))
                || (moveIntent && name == QStringLiteral("geometry.move"))
                || (copyIntent && name == QStringLiteral("geometry.copy"))
                || (rotateIntent && name == QStringLiteral("geometry.rotate"))
                || (scaleIntent && name == QStringLiteral("geometry.scale"))
                || (deleteIntent && name == QStringLiteral("geometry.delete"))
                || (extrudeIntent && (name == QStringLiteral("rectangles.extrude") || name == QStringLiteral("profile.extrude")))
                || (classifyIntent && name == QStringLiteral("bim.classify"))
                || (entityLayerIntent && name == QStringLiteral("entity.setLayer"))
                || (selectionIntent && name.startsWith(QStringLiteral("selection.")))
                || (!entityRenameIntent && name == QStringLiteral("command.execute"));
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
    enrichAgentToolDefinition(tool, m_brxLearning.toolProfile(QStringLiteral("layers.ensureMany")));
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
            appendBridgeLog(QString("BRX -> Qt: %1 Capabilities, %2 Commands, %3 Agent-Tools")
                .arg(methodCount)
                .arg(commandCount)
                .arg(toolCount));
            QStringList missingCatalogEntries;
            for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
                const QJsonObject method = value.toObject();
                const QString name = method.value("name").toString();
                if (!name.isEmpty() && m_brxLearning.toolProfile(name).isEmpty()) {
                    missingCatalogEntries << name;
                }
            }
            if (m_brxLearning.toolProfile(QStringLiteral("layers.ensureMany")).isEmpty()) {
                missingCatalogEntries << QStringLiteral("layers.ensureMany");
            }
            if (!missingCatalogEntries.isEmpty()) {
                appendBridgeLog(QString("BRX Learning Toolprofile: fehlende Eintraege fuer %1")
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
    QJsonArray learningSummaries;
    if (!isChatWorkspace()) {
        for (const QJsonValue& value : selectedLearningLessonsForRoute(route, prompt)) {
            const QJsonObject lesson = value.toObject();
            learningSummaries.append(QJsonObject{
                {"id", lesson.value(QStringLiteral("id")).toString()},
                {"topic", lesson.value(QStringLiteral("topic")).toString(lesson.value(QStringLiteral("title")).toString())},
                {"summary", lesson.value(QStringLiteral("description")).toString(lesson.value(QStringLiteral("intent")).toString()).left(420)},
                {"recommendedTools", lesson.value(QStringLiteral("recommendedTools")).toArray()},
                {"status", lesson.value(QStringLiteral("status")).toString()},
            });
        }
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
        {"selectedLearningLessons", learningSummaries},
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
    QStringList selectedToolNames;
    for (const QJsonValue& value : tools) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            selectedToolNames.append(name);
        }
    }
    QJsonObject brxLearningContext = isChatWorkspace()
        ? QJsonObject{}
        : m_brxLearning.contextForPrompt(prompt, 3, m_selectedWorkflowId, selectedToolNames);
    if (!isChatWorkspace() && !m_selectedWorkflowId.isEmpty()) {
        brxLearningContext.insert(QStringLiteral("selectedWorkflowId"), m_selectedWorkflowId);
    }
    QJsonArray workflowCapsules;
    for (int i = 0; i < selectedWorkflows.size() && i < 3; ++i) {
        workflowCapsules.append(workflowCapsuleForAgent(selectedWorkflows.at(i).toObject(), i == 0));
    }
    const QJsonArray readOnlyMethods = readOnlyMethodsForRoute(normalizedRoute);
    const QStringList policyRefs = policyRefsForRoute(normalizedRoute, prompt, sanitizedContext);
    const QJsonObject modePolicy = modePolicyForMode(m_chatMode, normalizedRoute);
    const bool cadContextAllowed = modePolicy.value("cadContextAllowed").toBool(false);
    const QJsonObject cadContext = cadContextAllowed ? currentAgentContext() : QJsonObject{};

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
    if (m_agentContextLoopBlocked && isSystemRouteWorkflowId(m_selectedWorkflowId)) {
        QJsonArray allowedTypes;
        for (const QJsonValue& value : responseContract.value(QStringLiteral("activeAllowedTypes")).toArray()) {
            if (value.toString() != QStringLiteral("context_request")) {
                allowedTypes.append(value);
            }
        }
        responseContract.insert(QStringLiteral("activeAllowedTypes"), allowedTypes);
        responseContract.insert(QStringLiteral("contextLoopPolicy"), QStringLiteral(
            "Workflow 05 hat bereits eine leere POINT-Abfrage erhalten. context_request ist fuer diesen Lauf gesperrt; erzeuge Layer und POINTS und antworte mit action_proposal."));
    }
    if (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(normalizedRoute)) {
        responseContract = QJsonObject{
            {"schema", "barebone.general.response.v1"},
            {"format", "barebone-agent-json-message-with-session-title"},
            {"allowedTypes", QJsonArray{"message", "context_request"}},
            {"required", QJsonArray{"schema", "type"}},
            {"historyContextPolicy", "Wenn du mehr Sitzungsverlauf brauchst, nutze vorher type=context_request mit einer session.history.* Methode aus conversationAccess.allowedMethods."},
            {"policy", QStringLiteral("%1 Antworte mit genau einem JSON-Objekt: schema='barebone.agent.response.v2', type='message', message='direkte Chatantwort', sessionTitle='kurzer Titel aus komprimiertem Kontext'. Markdown ist nur in message erlaubt. Bei Berechnungen: zuerst Grundgleichung, dann Symbolerklärung, dann SI-Umrechnung, dann Rechenschritte mit Einheit an jeder Zahl und jedem Summanden, dann Ergebnis. %2")
                .arg(aiLanguageInstruction(m_config), katexFormattingInstructionText())},
        };
    }

    if (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(normalizedRoute)) {
        responseContract.insert("policy", QStringLiteral("%1 Antworte mit genau einem JSON-Objekt. Final: schema='barebone.agent.response.v2', type='message', message='direkte Chatantwort', sessionTitle='kurzer Titel aus komprimiertem Kontext'. Wenn du mehr Sitzungsverlauf brauchst, nutze vorher type='context_request' mit einer session.history.* Methode aus conversationAccess.allowedMethods. Markdown ist nur in message erlaubt. Bei Berechnungen: zuerst Grundgleichung, dann Symbolerklaerung, dann SI-Umrechnung, dann Rechenschritte mit Einheit an jeder Zahl und jedem Summanden, dann Ergebnis. %2")
            .arg(aiLanguageInstruction(m_config), katexFormattingInstructionText()));
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
    if (!m_lastFocusedConversationContext.isEmpty()) {
        envelope.insert("focusedConversationContext", m_lastFocusedConversationContext);
        envelope.insert("conversationAccess", conversationAccessForFocusedContext(m_lastFocusedConversationContext));
    }
    envelope.insert("cadContext", cadContext);
    if (!cadContextAllowed) {
        envelope.insert("context", QJsonObject{
            {"mode", m_chatMode},
            {"brxConnected", m_brxAuthenticated},
            {"capabilitiesLoaded", !m_brxCapabilities.isEmpty()},
            {"lastToolResultAvailable", !m_lastAgentToolResult.isEmpty()},
        });
    }
    envelope.insert("policyRefs", stringsToJsonArray(policyRefs));
    envelope.insert("policyText", policyTextForRefs(policyRefs));
    envelope.insert("katexFormattingContract", katexFormattingContract());
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
    envelope.insert("brxLearningProfile", isChatWorkspace() ? QJsonObject{} : m_brxLearning.metadata());
    envelope.insert("brxLearningContext", brxLearningContext);
    envelope.insert("brxLearningUpdatePolicy", isChatWorkspace()
        ? QJsonObject{}
        : QJsonObject{
            {"allowed", true},
            {"fieldName", "learningUpdate"},
            {"policy", "Wenn aus einer erfolgreichen Ausfuehrung, Reparatur oder bestaetigtem Feedback wiederverwendbares BRX-Wissen entsteht, darfst du optional learningUpdate mitsenden. Workflows mit updateProtected=true oder source=canonical_building_workflow sind unveraenderlich. Aktualisiere ausschliesslich eigene Workflows mit source=ai_runtime und updateProtected=false; lege andernfalls einen neuen AI-Workflow in derselben kanonischen Struktur an. Erzeuge keine AI-Duplikate und nutze nur Toolnamen aus effectiveTools[].name. Rohr-/Luft- und Raumbeschriftungsworkflows muessen Technikraumname, Bounds und Startpunkt als requiredForExecution-Pflichtslots fuehren; Punktnetze verwenden die Qt-Runtime-Bindings aus der ausgewaehlten Lesson."},
            {"allowedTopLevelKeys", QJsonArray{"lesson", "lessons", "repairRule", "repairRules"}},
        });
    envelope.insert("tryBeforeFailPolicy", isChatWorkspace()
        ? QJsonObject{}
        : QJsonObject{
            {"policy", "Erst read-only Kontext abrufen, dann plausible Defaults ableiten, dann BRX-validieren. Frage nur gezielt nach, wenn Lesen, Defaults und Repair nicht reichen."},
            {"dataQueryPolicy", "Bei Tabellen, Objektdaten, Abmessungen, Hoehe, Breite, Tiefe, Bounds oder Messwerten zuerst geometry.query mit include=[\"metrics\",\"geometry\",\"dimensions\"] nutzen; wenn Dimensionen fehlen, measurement.bbox versuchen. Antworte nicht mit 'nicht verfuegbar', bevor diese read-only Fallbacks versucht wurden."},
            {"learningPolicy", "Lessons sind Erfahrungswissen, keine starren Rezepte. Pruefe Prompt, Zeichnungskontext und Berechnung, bevor du Lesson-Werte uebernimmst. Bei selbst erzeugten Objekten muessen Folgeschritte exakte Handles, lastResult/lastExtruded oder autoHandlesFromBatch nutzen; keine breiten currentSpace/Layer-Selektoren."},
            {"extrusionLayerPolicy", "Jede rectangles.extrude/profile.extrude Aktion braucht params.layer als expliziten Ziellayer. Direkt danach muss im selben proposal.actions Batch entity.setLayer mit selector.scope=lastResult und demselben layer folgen. Bei mehreren erzeugten Profilen exakte Handles oder createdGeometryHandleIndexes verwenden; scope=lastResult allein darf nicht stellvertretend fuer mehrere Profile verwendet werden. Zusammengehoerige Profile bevorzugt gemeinsam in einer atomaren Extrusionsaktion verarbeiten."},
            {"techniqueRoomPolicy", "In den Gebaeude-Workflows 03 bis 07 ist ein validierter Raum mit dem exakten Namen Technikraum Pflicht. Workflows 04 bis 07 duerfen ohne Technikraumbounds und einen darin liegenden Netzstartpunkt nicht fortfahren. Alle Rohr- und Luftnetze beginnen an diesem Punkt und bleiben innerhalb der validierten Grundrissbounds."},
            {"routePointPolicy", "Rohrwege nicht direkt als Polylinie erfinden: zuerst geometry.create POINT fuer den wandnahen Technikraum-Verteilerstart, jeden Bogen/Richtungswechsel/T-Knoten, jede Wanddurchfuehrung und jeden Nutzerendpunkt. geometry.query muss mit autoPointHandlesFromBatch/createdPointHandleIndexes exakt diese neuen Punkte lesen; geometry.create POLYLINE muss mit autoPointsFromLastQuery/queriedPointIndexes deren Readback-Koordinaten verwenden. pipes.validateNetwork bindet neue Polylinien ueber lokale autoPolylineHandlesFromBatch/createdPolylineHandleIndexes. Die kanonischen Workflows enden mit der validierten 2D-Netzkontur und erzeugen keine Rohrkoerper."},
            {"testDefaultPolicy", "Workflow testDefaults gelten ausschliesslich fuer den deterministischen Overlay-Test. Felder aus requiresUserConfirmationInAgentMode, insbesondere diameterMm, muessen bei regulaeren AI-Aktionen vom Nutzer genannt oder bestaetigt werden und duerfen nicht stillschweigend aus testDefaults uebernommen werden."},
            {"roomAreaPolicy", "Bei Raum- oder Aussenwand-Aufgaben mit Flaechenangabe die Innenflaeche rechnerisch pruefen. Wenn nur eine Flaeche ohne Seitenverhaeltnis genannt ist, quadratischen Innenraum als plausiblen Default ableiten und die Annahme nennen."},
            {"loopGuard", "Wiederhole keine identische Rueckfrage und keinen identischen ungueltigen Vorschlag. Nutze bei Wiederholung eine alternative Strategie."},
        });
    QStringList recentRejectedSignatures = m_agentRejectedResponseSignatures;
    while (recentRejectedSignatures.size() > 5) {
        recentRejectedSignatures.removeFirst();
    }
    envelope.insert("loopGuardState", QJsonObject{
        {"agentValidationRetries", m_agentValidationRetries},
        {"rejectedSignatures", stringsToJsonArray(recentRejectedSignatures)},
    });
    envelope.insert("geometryDataModel", cadContextAllowed
        ? QJsonObject{
            {"sourceOfTruth", "BRX readOnlyMethods"},
            {"recommendedFlow", QJsonArray{"actions.list", "layers.list", "geometry.query", "selection.describe", "entity.describe", "measurement.bbox", "measurement.length", "measurement.area"}},
            {"fields", QJsonArray{"handle", "type", "kind", "shape", "layer", "bounds", "dimensions.widthX", "dimensions.depthY", "dimensions.heightZ", "dimensions.unit", "geometry.vertices", "metrics.length", "metrics.area", "metrics.height", "metrics.volume"}},
            {"tableColumns", QJsonArray{"Handle", "Typ", "Layer", "Breite X", "Tiefe Y", "Hoehe Z", "Laenge", "Flaeche", "Volumen", "Hinweis"}},
            {"policy", "Use fetched geometry data to classify entities. Fuer alle Objekte/alle Layer selector.scope=currentSpace ohne selector.layer verwenden. Frage nicht nach Datei-Export, solange der Nutzer eine Tabelle im Chat verlangt oder keinen Export nennt. Do not assume an action exists just because the user asks for it."},
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
        {"entityRename", QJsonObject{
            {"available", false},
            {"reason", "No confirmed BRX action tool for renaming individual BIM walls, solids, objects, or entities is exposed yet."},
            {"policy", "Do not use layers.rename or command.execute as a substitute for entity/BIM wall renaming. layers.rename is only for layer names. If the user asks to rename BIM walls or solids, explain the missing capability or use read-only context only."},
        }},
        {"rotationAngle", QJsonObject{
            {"requiredFromUser", true},
            {"reason", "Rotation angle is a semantic design decision and must not be copied from examples or lessons."},
            {"policy", "For geometry.rotate, only set angleDeg/angleRad when the user prompt explicitly names the angle. Otherwise answer with ask_user for the missing angle."},
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
        {"extrusionLayerPolicy", "Every rectangles.extrude/profile.extrude action must include params.layer. The immediately following action must be entity.setLayer with selector.scope=lastResult and the identical layer. When several profiles were created, use exact handles or createdGeometryHandleIndexes; never use lastResult alone for the whole set. Prefer one atomic extrusion action for profiles that belong together."},
        {"preflightValidation", "Before user confirmation, Qt calls BRX actions.validate for the whole proposal. During internal batch execution Qt also calls BRX actions.validate for the current single action before sending it. If validation rejects a proposal, correct params/tool or ask_user for missing data; never repeat the same invalid proposal."},
        {"nativeCommandPolicy", "The agent may choose command.execute when a native BricsCAD command is the better fit. command.execute must contain exactly one complete command line from commands.list, no semicolon or newline, and is always validated by BRX actions.validate before user confirmation."},
        {"databaseWritePolicy", "Direct BricsCAD DB writes are forbidden. Proposals must use only tools[].name; never suggest AcDb, LayerTable, EntityTable, database mutation, or DB batch write operations."},
    });
    envelope.insert("effectiveTools", tools);
    envelope.insert("pendingProposal", m_pendingAgentProposal);
    envelope.insert("pendingDraft", m_pendingAgentDraft);
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("selectedWorkflow", isChatWorkspace() ? selectedWorkflowSummary() : QJsonObject{});
    envelope.insert("workflowCapsules", isChatWorkspace() ? workflowCapsules : QJsonArray{});
    envelope.insert("workflowSelectionPolicy", QJsonObject{
        {"source", isChatWorkspace() ? QStringLiteral("general workflow overlay") : QStringLiteral("brx-learning.json")},
        {"selectedWorkflowCount", isChatWorkspace() ? workflowCapsules.size() : 0},
        {"policy", isChatWorkspace()
            ? QStringLiteral("Wenn workflowCapsules vorhanden sind, nutze sie als allgemeinen Chat-Kontext.")
            : QStringLiteral("BricsCAD nutzt keine agent/workflows mehr. Nutze brxLearningContext als kompaktes Erfahrungswissen und antworte mit action_proposal statt workflow_run_proposal.")},
        {"workflowRunProposalRequiredWhenFit", false},
    });
    envelope.insert("conversationMode", "unified-agent-envelope");
    envelope.insert("expectedResponse", (m_chatMode == QStringLiteral("general") && !routeAllowsCadActions(normalizedRoute))
        ? QStringLiteral("barebone-agent-json-message-with-session-title")
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
    m_agentConversation = {};
    m_pendingAgentProposal = {};
    m_pendingAgentDraft = {};
    m_lastAgentToolResult = {};
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
    m_lastFocusedConversationContext = {};

    if (m_agentSessions.contains(m_agentSessionId)) {
        const AgentSessionState state = m_agentSessions.value(m_agentSessionId);
        m_agentConversation = state.conversation;
        m_pendingAgentProposal = state.pendingProposal;
        m_pendingAgentDraft = state.pendingDraft;
        m_lastAgentToolResult = state.lastToolResult;
        m_lastDocumentContext = {};
        m_lastFocusedConversationContext = {};
    } else {
        m_agentConversation = conversationFromWebHistory(history);
        m_pendingAgentProposal = {};
        m_pendingAgentDraft = {};
        m_lastAgentToolResult = {};
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

