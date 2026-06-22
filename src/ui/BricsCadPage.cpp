#include "BricsCadPage.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QGridLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineSettings>

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace {

constexpr const char* kBrxSdkRoot = "C:/Program Files/Bricsys/BRXSDK/BRX26.1.05.0";
constexpr const char* kBrxPluginName = "BareboneBrx.brx";
constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47626;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";
constexpr bool kAgentActionToolsEnabled = true;
constexpr int kMaxAgentValidationRetries = 4;
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
        "Wenn der gewuenschte Seitenbereich oder Inhalt nicht enthalten ist, sage das klar und fordere einen engeren/anderen Bereich an.\n"
        "Dokumente:\n%2\n\n"
        "%3")
        .arg(prompt,
            metadataLines.isEmpty() ? QStringLiteral("- Angehängtes Dokument") : metadataLines.join('\n'),
            selectedText);
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

QStringList inferredProvidedFieldsFromAskMessage(const QString& prompt, const QString& askMessage, const QJsonObject& draft)
{
    const QString normalized = askMessage.toLower();
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

QString repairMojibakeText(QString text)
{
    if (!text.contains(QStringLiteral("Ã"))
        && !text.contains(QStringLiteral("Â"))
        && !text.contains(QStringLiteral("â"))) {
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
        {QStringLiteral("Â«"), QStringLiteral("«")},
        {QStringLiteral("Â»"), QStringLiteral("»")},
        {QStringLiteral("Â°"), QStringLiteral("°")},
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

BricsCadPage::BricsCadPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_aiNetwork(new QNetworkAccessManager(this))
    , m_bridgeToken(generateBridgeToken())
{
    m_reasoningEffort = normalizedReasoningEffort(m_config.aiReasoningEffort());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(20);

    auto* title = new QLabel("BricsCAD Schnittstelle", this);
    title->setObjectName("pageTitle");
    root->addWidget(title);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setObjectName("templateScroll");

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(20);

    auto* functionsGroup = new QWidget(content);
    functionsGroup->setObjectName("dashboardGroup");
    auto* functionsLayout = new QVBoxLayout(functionsGroup);
    functionsLayout->setContentsMargins(18, 18, 18, 18);
    functionsLayout->setSpacing(14);

    auto* functionsTitle = new QLabel("Funktionen", functionsGroup);
    functionsTitle->setObjectName("settingsSectionTitle");
    functionsLayout->addWidget(functionsTitle);

    auto* functionsGrid = new QGridLayout();
    functionsGrid->setContentsMargins(0, 0, 0, 0);
    functionsGrid->setHorizontalSpacing(14);
    functionsGrid->setVerticalSpacing(14);
    functionsGrid->setColumnStretch(0, 1);
    functionsGrid->setColumnStretch(1, 1);
    functionsLayout->addLayout(functionsGrid);

    functionsGrid->addWidget(createBrxLoadCard(functionsGroup), 0, 0, Qt::AlignTop);

    auto* statusWrapper = new QWidget(functionsGroup);
    auto* statusLayout = new QVBoxLayout(statusWrapper);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(10);
    statusLayout->addWidget(cardHeader("BRX", "Status", statusWrapper));
    m_pluginStatus = new QLabel("BRX Plugin nicht verbunden", statusWrapper);
    m_pluginStatus->setObjectName("cardBodyText");
    m_pluginStatus->setMinimumHeight(28);
    statusLayout->addWidget(m_pluginStatus);
    auto* statusText = new QLabel("Interaktionen laufen ueber den AI Assistenten und die Live-BRX-Kontextabfragen.", statusWrapper);
    statusText->setObjectName("cardBodyText");
    statusText->setWordWrap(true);
    statusLayout->addWidget(statusText);
    statusLayout->addStretch();
    functionsGrid->addWidget(wrapCard(statusWrapper, functionsGroup, 190), 0, 1, Qt::AlignTop);

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
    m_agentWebView->setUrl(QUrl(QStringLiteral("qrc:/web/ai-assistant.html")));

    agentLayout->addWidget(m_agentWebView, 1);

    auto* logGroup = new QWidget(content);
    logGroup->setObjectName("dashboardGroup");
    auto* logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(18, 18, 18, 18);
    logLayout->setSpacing(14);

    auto* logTitle = new QLabel("Log", logGroup);
    logTitle->setObjectName("settingsSectionTitle");
    logLayout->addWidget(logTitle);

    m_bridgeStatus = new QLabel("Server startet...", logGroup);
    m_bridgeStatus->setObjectName("cardBodyText");
    logLayout->addWidget(m_bridgeStatus);

    m_bridgeLog = new QPlainTextEdit(logGroup);
    m_bridgeLog->setObjectName("logView");
    m_bridgeLog->setReadOnly(true);
    m_bridgeLog->setMinimumHeight(240);
    logLayout->addWidget(m_bridgeLog);

    contentLayout->addWidget(functionsGroup);
    contentLayout->addWidget(logGroup);
    contentLayout->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    startBridgeServer();

    QObject::connect(m_agentBridge, &AiWebBridge::promptSubmitted, this, [this](const QString& prompt) {
        sendAgentPrompt(prompt);
    });
    QObject::connect(m_agentBridge, &AiWebBridge::promptSubmittedWithContext, this, [this](const QString& prompt, const QVariantMap& context) {
        sendAgentPrompt(prompt, QJsonObject::fromVariantMap(context));
    });
    QObject::connect(m_agentBridge, &AiWebBridge::proposalConfirmed, this, [this]() {
        executeAgentProposal();
    });
    QObject::connect(m_agentBridge, &AiWebBridge::proposalClearedByUser, this, [this]() {
        clearAgentProposal();
        m_pendingAgentDraft = {};
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
    QObject::connect(m_agentBridge, &AiWebBridge::clientStateSaved, this, [this](const QString& stateJson) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(stateJson.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            m_config.setAiAssistantState(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
        }
    });
    QObject::connect(m_agentBridge, &AiWebBridge::uiReady, this, [this]() {
        Q_EMIT m_agentBridge->clientStateLoaded(m_config.aiAssistantState());
        Q_EMIT m_agentBridge->localAiStatusChanged(m_localAiStatusMessage, m_localAiReachable);
        refreshLocalContextWindow(false);
        emitContextBudget();
        setAgentBusy(m_agentBusy);
        if (!m_pendingAgentProposal.isEmpty()) {
            setAgentProposal(m_pendingAgentProposal);
        } else {
            clearAgentProposal();
        }
        setPluginStatus(m_brxAuthenticated ? QStringLiteral("BRX Plugin verbunden") : QStringLiteral("BRX Plugin nicht verbunden"), m_brxAuthenticated);
        emitCapabilitiesStatusToWeb();
    });
    QObject::connect(&m_config, &ConfigManager::changed, this, [this]() {
        refreshLocalContextWindow(true);
    });
    refreshLocalContextWindow(true);
}

QWidget* BricsCadPage::agentWidget() const
{
    return m_agentWidget;
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
        return;
    }

    m_contextWindowRequestInFlight = true;
    setLocalAiStatus(QStringLiteral("Lokale AI wird geprüft"), false);
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
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            appendBridgeLog(QString("LM Studio Kontext: ungueltige JSON Antwort: %1").arg(parseError.errorString()));
            setLocalAiStatus(QStringLiteral("Lokale AI Antwort ungültig"), false);
            emitContextBudget(-1, false, QStringLiteral("Kontextlaenge nicht abrufbar"));
            reply->deleteLater();
            return;
        }

        handleLocalContextWindowResponse(document.object());
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
    const int loadedContext = positiveIntValue(instanceConfig, {"context_length", "loaded_context_length", "n_ctx"});
    const int legacyLoadedContext = positiveIntValue(selectedLoadedInstance, {"context_length", "loaded_context_length", "n_ctx"});
    const int modelContext = positiveIntValue(selectedModel, {"context_length", "loaded_context_length", "max_context_length", "n_ctx"});
    const int maxContext = positiveIntValue(selectedModel, {"max_context_length", "context_length", "n_ctx"});
    const int contextLength = loadedContext > 0 ? loadedContext : (legacyLoadedContext > 0 ? legacyLoadedContext : modelContext);

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
    if (m_config.aiProvider() != QStringLiteral("local")) {
        return requestedOutputTokens;
    }

    const int contextTokens = effectiveContextWindowTokens();
    const int dynamicLimit = std::max(384, contextTokens / 4);
    return std::clamp(requestedOutputTokens, 256, dynamicLimit);
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
        "Du bist ein allgemeiner AI Chat-Assistent in Barebone-Qt. "
        "Antworte direkt, hilfreich und auf Deutsch, sofern der Nutzer keine andere Sprache wuenscht. "
        "Du hast in diesem Modus keine BricsCAD-Tools und sollst keine Aktionen in BricsCAD behaupten. "
        "Wenn die Nutzeranfrage einen Dokumentkontext enthaelt, nutze diesen als primaere Quelle und verweise bei PDFs nach Moeglichkeit auf Seiten. "
        "Wenn der Nutzer nach Rechtschreibfehlern, Tippfehlern oder Korrektur fragt, liste gefundene Stellen mit Original und Korrektur; wenn du keine offensichtlichen Fehler findest, sage das ausdruecklich.";

    const int maxHistoryMessages = std::min<qsizetype>(20, m_agentConversation.size());
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

    Q_EMIT m_agentBridge->contextBudgetChanged(QVariantMap{
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
    });
}

void BricsCadPage::setReasoningEffort(const QString& effort)
{
    const QString normalized = normalizedReasoningEffort(effort);
    if (m_reasoningEffort == normalized) {
        m_config.setAiReasoningEffort(normalized);
        Q_EMIT m_agentBridge->reasoningEffortApplied(m_reasoningEffort);
        return;
    }
    m_reasoningEffort = normalized;
    m_config.setAiReasoningEffort(m_reasoningEffort);
    Q_EMIT m_agentBridge->reasoningEffortApplied(m_reasoningEffort);
    appendBridgeLog(QString("AI Agent: Reasoning %1").arg(normalized));
}

void BricsCadPage::setChatMode(const QString& mode)
{
    const QString normalized = mode.trimmed().compare(QStringLiteral("general"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("general")
        : QStringLiteral("bricscad");
    if (m_chatMode == normalized) {
        return;
    }

    m_chatMode = normalized;
    resetAgentConversation();
    appendBridgeLog(QString("AI Agent: Modus %1").arg(m_chatMode));
}

bool BricsCadPage::isBricsCadMode() const
{
    return m_chatMode == QStringLiteral("bricscad");
}


void BricsCadPage::sendAgentPrompt(const QString& promptText, const QJsonObject& documentContext)
{
    if (m_agentBusy) {
        return;
    }

    const QString prompt = promptText.trimmed();
    if (prompt.isEmpty()) {
        return;
    }

    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    appendAgentChat("Du", prompt);

    if (!isBricsCadMode()) {
        m_agentValidationRetries = 0;
        m_lastAgentUserPrompt = prompt;
        m_lastDocumentContext = sanitizedContext;
        clearAgentProposal();
        m_pendingAgentDraft = {};
        sendGeneralChatRequest(prompt, sanitizedContext);
        return;
    }

    if (isAgentConfirmation(prompt) && !m_pendingAgentProposal.isEmpty()) {
        executeAgentProposal();
        return;
    }

    m_agentValidationRetries = 0;
    m_lastAgentUserPrompt = prompt;
    m_lastDocumentContext = sanitizedContext;

    if (!m_pendingAgentProposal.isEmpty()) {
        clearAgentProposal();
        appendBridgeLog("AI Agent: alter Vorschlag verworfen, neuer Nutzerprompt ist massgebend");
    }

    if (!m_pendingAgentDraft.isEmpty()) {
        appendBridgeLog("AI Agent: offener Plan/Draft wird als Kontext an die AI weitergegeben");
    }

    if (!ensureBridgeCapabilitiesForPrompt(prompt)) {
        return;
    }

    if (shouldPrefetchSelectionDescription(prompt)) {
        sendAgentRequestWithPrefetchedContext(
            prompt,
            "selection.describe",
            QJsonObject{
                {"include", QJsonArray{"geometry"}},
                {"limit", 100},
            },
            sanitizedContext);
        return;
    }

    sendAgentRequest(prompt, sanitizedContext);
}

void BricsCadPage::sendAgentRequest(const QString& prompt, const QJsonObject& documentContext)
{
    sendAgentEnvelope(agentRequestEnvelope(prompt, documentContext), prompt, true, "prompt");
}

void BricsCadPage::sendGeneralChatRequest(const QString& prompt, const QJsonObject& documentContext, bool forceNoReasoning)
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

    const int requestedOutputTokens = useResponsesApi ? 2048 : 1200;
    const int outputTokens = adjustedOutputTokenLimit(requestedOutputTokens);
    const ContextBuildResult contextBuild = buildGeneralMessagesForBudget(prompt, documentContext, outputTokens);
    const QJsonArray messages = contextBuild.messages;
    emitContextBudget(
        contextBuild.estimatedTokens,
        contextBuild.minimized,
        contextBuild.minimized ? QStringLiteral("Kontext automatisch minimiert") : QString());

    QJsonObject payload;
    payload.insert("model", model);
    if (useResponsesApi) {
        payload.insert("input", messages);
        payload.insert("max_output_tokens", outputTokens);
        const QString reasoningEffort = normalizedReasoningEffort(m_reasoningEffort);
        if (!forceNoReasoning && reasoningEffort != "none") {
            payload.insert("reasoning", QJsonObject{{"effort", reasoningEffort}});
        }
        if (!officialProvider) {
            payload.insert("temperature", 0.3);
        }
    } else {
        payload.insert("messages", messages);
        payload.insert("temperature", 0.3);
        payload.insert("max_tokens", outputTokens);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (officialProvider) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_config.aiApiKey().trimmed()).toUtf8());
    }
    request.setTransferTimeout(45000);

    setAgentBusy(true);
    const QString reasoningLog = useResponsesApi
        ? QString(" reasoning=%1").arg(forceNoReasoning ? QStringLiteral("none") : normalizedReasoningEffort(m_reasoningEffort))
        : QString();
    appendBridgeLog(QString("Qt -> AI Chat: provider=%1 endpoint=%2 model=%3%4")
        .arg(provider, url.toString(), model, reasoningLog));
    if (contextBuild.minimized) {
        appendBridgeLog(QString("AI Kontext: automatisch minimiert used=%1/%2 tokens history=%3")
            .arg(contextBuild.estimatedTokens)
            .arg(effectiveContextWindowTokens())
            .arg(contextBuild.historyMessages));
    }

    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, documentContext, useResponsesApi, forceNoReasoning]() {
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
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText)).trimmed();
        if (content.isEmpty()) {
            if (useResponsesApi && !forceNoReasoning) {
                appendBridgeLog("AI Chat: leere finale Antwort erhalten, wiederhole ohne Reasoning");
                reply->deleteLater();
                sendGeneralChatRequest(prompt, documentContext, true);
                return;
            }
            if (!reasoningText.trimmed().isEmpty()) {
                appendBridgeLog(QString("AI Reasoning ohne finale Antwort: %1").arg(reasoningText.left(800)));
            } else {
                appendBridgeLog(QString("AI Body ohne auswertbaren Inhalt: %1").arg(QString::fromUtf8(body).left(800)));
            }
            appendAgentChat("AI", "Leere Antwort erhalten.");
            reply->deleteLater();
            return;
        }

        m_agentConversation.append(QJsonObject{{"role", "user"}, {"content", prompt}});
        m_agentConversation.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        emitContextBudget();
        appendBridgeLog(QString("AI Chat: %1").arg(content.left(1600)));
        appendAgentChat("AI", content);
        reply->deleteLater();
    });
}

void BricsCadPage::sendAgentRequestWithPrefetchedContext(
    const QString& prompt,
    const QString& method,
    const QJsonObject& params,
    const QJsonObject& documentContext)
{
    if (!m_brxAuthenticated) {
        sendAgentRequest(prompt, documentContext);
        return;
    }

    appendBridgeLog(QString("Qt -> BRX Prefetch: %1 %2")
        .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
    setAgentBusy(true);

    const bool queued = sendBridgeRequest(
        method,
        params,
        15000,
        [this, prompt, method, params, documentContext](const QJsonObject& response) {
            setAgentBusy(false);
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            QJsonObject envelope = agentRequestEnvelope(prompt, documentContext);
            envelope.insert("prefetchedContext", QJsonObject{
                {"request", QJsonObject{{"method", method}, {"params", params}}},
                {"response", response},
            });
            sendAgentEnvelope(envelope, prompt, true, "prompt+context");
        });

    if (!queued) {
        setAgentBusy(false);
        sendAgentRequest(prompt, documentContext);
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

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content",
            "Du bist der AI Assistent fuer Barebone-Qt und BricsCAD. "
            "Du sprichst immer auf Deutsch mit dem Nutzer; nur JSON-Felder, Toolnamen und technische Parameter bleiben wie in der API vorgegeben. "
            "Du entscheidest selbst, ob du antwortest, Kontext brauchst, nachfragst oder eine ausfuehrbare BricsCAD-Aktion vorschlaegst. "
            "Barebone-Qt bleibt die Kontrollinstanz: riskante Aktionen werden validiert und erst nach Nutzerbestaetigung an BRX gesendet. "
            "Antworte ausschliesslich als gueltiges JSON-Objekt ohne Markdown. "
            "Diese Anwendung ist BricsCAD-fokussiert: nutze den gelieferten BricsCAD-Kontext, pendingDrafts und Capabilities auch bei kurzen Folgeantworten wie '100mm' oder 'x=100 y=200'. "
            "Nutze den Envelope mit userPrompt, conversation, context, documentContext, capabilities, readOnlyMethods, tools, pendingProposal, pendingDraft und lastToolResult. "
            "Wenn documentContext vorhanden ist und der Nutzer nach PDF-, Word- oder Textdatei-Inhalten fragt, antworte als type=message aus diesem Kontext; verweise bei PDFs auf Seiten und frage nach einem engeren Seitenbereich, wenn der noetige Text nicht enthalten ist. "
            "Wenn Zeichnungskontext fehlt, nutze context_request mit exakt einer Methode aus readOnlyMethods. "
            "Wenn du ausfuehren willst, nutze action_proposal mit tool exakt aus tools[].name und params passend zu inputSchema/apiDoc.post. "
            "Bei action_proposal muessen alle required-Felder aus inputSchema/apiDoc.post.required in params enthalten sein; nutze Beispiele nur als Formathilfe und lasse keine Pflichtfelder weg. "
            "Neue Grundgeometrien wie Kreis, Rechteck, Linie, Polyline, Punkt, Bogen oder Box werden mit geometry.create erstellt; erfinde keine Toolnamen wie draw.circle. "
            "Fuer geometry.create rectangle ist die zweite 2D-Abmessung als depth oder length zu senden, nicht als height; Angaben wie y, b, Tiefe oder Rechteck-Hoehe bedeuten dabei depth/length. "
            "Eine Wand mit Laenge, Wandstaerke und Hoehe kann als geometry.create geometry=box erstellt werden; Laenge=width, Wandstaerke=depth, Hoehe=height, danach optional bim.classify mit target=lastExtruded. "
            "Nutze geometry.scale nur fuer uniforme Skalierung mit factor. Nutze geometry.scale niemals fuer 'verlaengern', 'in X/Y/Z-Richtung strecken' oder einseitige Laengenaenderungen. "
            "Face-/Subentity-Bearbeitung ist nur erlaubt, wenn dafuer ein ausdrueckliches Tool in tools[] vorhanden ist. Erfinde keine Face-Tools und nutze geometry.move nicht fuer ein einzelnes Solid-Face, weil geometry.move ganze Entities verschiebt. "
            "Wenn ein Solid oder eine BIMWall verlaengert werden soll und kein direktes Extend-Tool vorhanden ist, hole zuerst measurement.bbox und schlage danach einen sicheren Ersatz-Workflow vor: neue Box mit geaenderten Abmessungen erstellen, alte Geometrie nach Bestaetigung loeschen und bei Bedarf wieder als BIMWall klassifizieren. "
            "Wenn der Nutzer Ursprung sagt, verwende {\"x\":0,\"y\":0,\"z\":0}. "
            "Frage fuer geometry.create nicht nach allgemeinem Zeichnungskontext; Einheiten sind mm und fehlender Layer kann als Layer 0 angenommen werden. "
            "Bei Extrusionen bedeuten z, hoehe, height, heightMm oder eine alleinstehende mm-Angabe immer params.heightMm. "
            "Bei Verschieben, Verlaengern oder Face-Bewegung bedeutet eine alleinstehende mm-Angabe die Distanz/Offset-Angabe; frage nicht erneut nach derselben Distanz, wenn userPrompt sie enthaelt. "
            "Wenn der Nutzer von Auswahl, ausgewaehlt, selektiert oder selection spricht, verwende fuer passende Tools params.selector.scope=\"selection\" statt nach Handles zu fragen. "
            "Wenn Angaben fehlen, frage natuerlich nach; speichere dabei einen draft mit deiner beabsichtigten Aktion. "
            "Wenn mehrere Schritte noetig sind, schlage genau den ersten sicheren Schritt vor und setze continueAfterSuccess=true plus nextIntent als kurze Beschreibung fuer den Folgeschritt. "
            "Akzeptierte Antworttypen: "
            "{\"type\":\"message\",\"message\":\"...\"}, "
            "{\"type\":\"ask_user\",\"message\":\"...\",\"missing\":[\"...\"],\"draft\":{...}}, "
            "{\"type\":\"context_request\",\"message\":\"...\",\"method\":\"selection.describe\",\"params\":{},\"reason\":\"...\"}, "
            "{\"type\":\"action_proposal\",\"message\":\"...\",\"tool\":\"name-aus-tools\",\"params\":{},\"requiresConfirmation\":true,\"continueAfterSuccess\":false,\"nextIntent\":\"...\"}, "
            "{\"type\":\"plan\",\"message\":\"...\",\"steps\":[...],\"neededContext\":[...],\"missingCapabilities\":[...]}. "
            "Nutze plan nur, wenn keine ausfuehrbare CAD-Aktion vorgeschlagen werden kann. "
            "Alte Typnamen assistant_message, tool_proposal und operation_plan sind nur Kompatibilitaet; bevorzuge message und action_proposal. "
            "Behaupte niemals, dass eine Aktion bereits ausgefuehrt wurde, bevor Barebone-Qt ein BRX-Resultat geliefert hat."},
    });

    const QJsonObject systemMessage = messages.first().toObject();
    const bool includeConversationHistory = envelope.value("includeConversationHistory").toBool(true);
    const int requestedOutputTokens = useResponsesApi ? 2048 : 700;
    const int outputTokens = adjustedOutputTokenLimit(requestedOutputTokens);
    const int budget = inputBudgetTokens(outputTokens);
    const int maxHistoryMessages = includeConversationHistory
        ? std::min<qsizetype>(10, m_agentConversation.size())
        : 0;

    auto buildAgentMessages = [&](int historyMessages, const QJsonObject& sourceEnvelope) {
        ContextBuildResult result;
        result.envelope = sourceEnvelope;
        result.historyMessages = historyMessages;

        QJsonArray builtMessages;
        builtMessages.append(systemMessage);

        QJsonArray recentConversation;
        const qsizetype historyStart = std::max<qsizetype>(0, m_agentConversation.size() - historyMessages);
        for (qsizetype i = historyStart; i < m_agentConversation.size(); ++i) {
            const QJsonObject item = m_agentConversation.at(i).toObject();
            builtMessages.append(item);
            recentConversation.append(item);
        }

        QJsonObject outboundEnvelope = sourceEnvelope;
        outboundEnvelope.insert("conversation", recentConversation);
        result.envelope = outboundEnvelope;
        builtMessages.append(QJsonObject{
            {"role", "user"},
            {"content", QString::fromUtf8(QJsonDocument(outboundEnvelope).toJson(QJsonDocument::Compact))},
        });
        result.messages = builtMessages;
        result.estimatedTokens = estimateTokensForMessages(builtMessages);
        return result;
    };

    ContextBuildResult contextBuild;
    for (int historyMessages = maxHistoryMessages; historyMessages >= 0; --historyMessages) {
        contextBuild = buildAgentMessages(historyMessages, envelope);
        if (contextBuild.estimatedTokens <= budget) {
            contextBuild.minimized = historyMessages < maxHistoryMessages;
            break;
        }
    }

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
        contextBuild = buildAgentMessages(0, minimizedEnvelope);
        contextBuild.minimized = true;
    }

    const QJsonArray outboundMessages = contextBuild.messages;
    emitContextBudget(
        contextBuild.estimatedTokens,
        contextBuild.minimized,
        contextBuild.minimized ? QStringLiteral("Kontext automatisch minimiert") : QString());

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
    request.setTransferTimeout(45000);

    setAgentBusy(true);
    const QString reasoningLog = useResponsesApi
        ? QString(" reasoning=%1").arg(normalizedReasoningEffort(m_reasoningEffort))
        : QString();
    appendBridgeLog(QString("Qt -> AI Agent: %1 provider=%2 endpoint=%3 model=%4%5")
        .arg(logLabel,
            provider,
            url.toString(),
            model,
            reasoningLog));
    if (contextBuild.minimized) {
        appendBridgeLog(QString("AI Kontext: automatisch minimiert used=%1/%2 tokens history=%3")
            .arg(contextBuild.estimatedTokens)
            .arg(effectiveContextWindowTokens())
            .arg(contextBuild.historyMessages));
    }

    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, userHistoryContent, storeUserMessage]() {
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
        const QString content = removeReasoningLeak(aiChatCompletionContent(responseDocument.object(), &reasoningText));
        if (content.trimmed().isEmpty()) {
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
        appendBridgeLog(QString("AI JSON: %1").arg(content.left(1600)));

        if (storeUserMessage) {
            m_agentConversation.append(QJsonObject{{"role", "user"}, {"content", userHistoryContent}});
        }
        m_agentConversation.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        emitContextBudget();
        handleAgentReply(content);
        reply->deleteLater();
    });
}

void BricsCadPage::handleAgentReply(const QString& content)
{
    bool parsed = false;
    const QJsonObject reply = jsonObjectFromAiContent(content, &parsed);
    if (!parsed) {
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
    if (type == "assistant_message") {
        type = "message";
    } else if (type == "tool_proposal") {
        type = "action_proposal";
    } else if (type == "operation_plan") {
        type = "plan";
    }
    const QString message = reply.value("message").toString();

    if (type == "action_proposal") {
        QJsonObject proposal = normalizedAgentProposal(reply);
        if (!message.trimmed().isEmpty()) {
            proposal.insert("summary", message.trimmed());
        }
        QString errorMessage;
        if (!validateAgentProposal(proposal, errorMessage)) {
            clearAgentProposal();
            if (retryAgentAfterValidationFailure(
                    content,
                    reply,
                    QString("action_proposal ist nicht gueltig: %1").arg(errorMessage))) {
                return;
            }
            appendAgentChat("Barebone-Qt", QString("AI Vorschlag abgelehnt: %1").arg(errorMessage));
            return;
        }
        setAgentProposal(proposal);
        appendBridgeLog(QString("AI Vorschlag: %1 params=%2")
            .arg(proposal.value("tool").toString(),
                QString::fromUtf8(QJsonDocument(proposal.value("params").toObject()).toJson(QJsonDocument::Compact))));
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
        QStringList providedFields = providedMissingFields(m_lastAgentUserPrompt, missing, m_pendingAgentDraft);
        providedFields.append(inferredProvidedFieldsFromAskMessage(m_lastAgentUserPrompt, message, m_pendingAgentDraft));
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
        if (!m_pendingAgentDraft.isEmpty()) {
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
        m_agentValidationRetries = 0;
        return;
    }

    if (type == "plan") {
        if (retryAgentAfterValidationFailure(
                content,
                reply,
                "Antworttyp plan ist fuer eine ausfuehrbare CAD-Anfrage nicht final. Nutze keinen Plan mit Pseudo-Actions. Gib entweder eine echte Rueckfrage ask_user oder einen action_proposal mit tool exakt aus tools[].name und params gemaess inputSchema/apiDoc.post zurueck. Fuer mehrstufige Workflows schlage den ersten sicheren Schritt vor und setze continueAfterSuccess/nextIntent.")) {
            return;
        }
        m_pendingAgentDraft = reply;
        clearAgentProposal();
        appendBridgeLog(QString("AI Agent: Plan missing=%1")
            .arg(QString::fromUtf8(QJsonDocument(reply.value("missingCapabilities").toArray()).toJson(QJsonDocument::Compact))));
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
        m_lastDocumentContext);
    envelope.insert("type", "validation_error");
    envelope.insert("validationError", errorMessage);
    envelope.insert("rejectedContent", rejectedContent.left(4000));
    if (!rejectedObject.isEmpty()) {
        envelope.insert("rejectedObject", rejectedObject);
    }
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentValidationRetries},
        {"policy", "Your previous response was not shown to the user. Correct it using the available tools and schemas."},
    });
    envelope.insert("instruction",
        "Korrigiere deine letzte Antwort. Antworte ausschliesslich mit einem gueltigen JSON-Objekt. "
        "Nutze keinen freien Plan und keine Pseudo-Actions. Wenn die Aufgabe ausfuehrbar ist, liefere genau einen action_proposal fuer den naechsten sicheren Schritt. "
        "tool muss exakt einem tools[].name entsprechen. params muessen inputSchema/apiDoc.post erfuellen. "
        "Wenn echte Informationen fehlen, nutze ask_user mit missing und einem draft. "
        "Wenn du Zeichnungskontext brauchst, nutze context_request mit exakt einer readOnlyMethods[].name Methode. "
        "Wenn die Anfrage allgemein ist, nutze type=message.");

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, QString("agent_loop_%1").arg(m_agentValidationRetries));
    return true;
}

QJsonObject BricsCadPage::normalizedAgentProposal(const QJsonObject& proposal) const
{
    QJsonObject normalized = proposal;
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

    normalized.insert("params", params);
    return normalized;
}

void BricsCadPage::handleAgentContextRequest(const QJsonObject& request)
{
    const QString method = request.value("method").toString().trimmed();
    const QJsonObject params = request.value("params").toObject();
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
    QJsonArray readOnlyMethods;
    for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
        const QJsonObject method = value.toObject();
        if (method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }

    QJsonObject envelope;
    envelope.insert("type", "context_result");
    envelope.insert("request", contextRequest);
    envelope.insert("response", contextResponse);
    envelope.insert("context", currentAgentContext());
    envelope.insert("capabilities", m_brxCapabilities);
    envelope.insert("readOnlyMethods", readOnlyMethods);
    envelope.insert("geometryDataModel", QJsonObject{
        {"sourceOfTruth", "BRX readOnlyMethods"},
        {"recommendedFlow", QJsonArray{"actions.list", "layers.list", "geometry.query", "selection.describe", "entity.describe", "measurement.bbox", "measurement.length", "measurement.area"}},
        {"fields", QJsonArray{"handle", "type", "kind", "shape", "layer", "bounds", "geometry.vertices", "metrics.length", "metrics.area", "metrics.height", "metrics.volume"}},
        {"policy", "Use fetched geometry data to classify entities. Do not assume an action exists just because the user asks for it."},
    });
    envelope.insert("operationLimits", QJsonObject{
        {"subentityFaceMove", QJsonObject{
            {"available", false},
            {"reason", "No confirmed action tool for selecting and transforming individual AcDb3dSolid faces is exposed yet."},
            {"policy", "Do not invent solid.face.move, face.move or use geometry.move for a single face. geometry.move moves whole entities. If the user wants to lengthen a wall face, use measurement.bbox first and then propose a safe replacement workflow only with existing tools, or return plan/missingCapabilities."},
        }},
        {"axisExtension", QJsonObject{
            {"preferredContext", "measurement.bbox"},
            {"forbiddenTools", QJsonArray{"geometry.scale"}},
            {"policy", "A standalone mm value after a pending extension or face-move request is the distance/offset value, not a new unrelated prompt."},
        }},
    });
    envelope.insert("actionToolsEnabled", kAgentActionToolsEnabled);
    envelope.insert("executionPolicy", QJsonObject{
        {"mode", kAgentActionToolsEnabled ? QStringLiteral("confirmed-actions") : QStringLiteral("context-only")},
        {"toolProposalAllowed", kAgentActionToolsEnabled && !availableAgentTools().isEmpty()},
        {"whenNoToolFits", "plan"},
    });
    envelope.insert("tools", availableAgentTools());
    envelope.insert("pendingProposal", m_pendingAgentProposal);
    envelope.insert("pendingDraft", m_pendingAgentDraft);
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("conversationMode", "direct-ai-with-confirmed-actions");
    envelope.insert("expectedResponse", "strict-json-object");

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

    const QString tool = m_pendingAgentProposal.value("tool").toString();
    const QString bridgeMethod = bridgeMethodForTool(tool);
    const QJsonObject params = m_pendingAgentProposal.value("params").toObject();
    const QJsonObject executedProposal = m_pendingAgentProposal;

    appendAgentChat("Barebone-Qt", QString("Bestaetigt. Fuehre %1 ueber %2 aus.").arg(tool, bridgeMethod));
    const bool queued = sendBridgeRequest(
        bridgeMethod,
        params,
        30000,
        [this, tool, bridgeMethod, executedProposal](const QJsonObject& response) {
            appendJsonDebugLines(m_bridgeLog, response.value("debug").toArray());
            if (!response.value("ok").toBool(false)) {
                const QString message = bridgeErrorMessage(response, "Tool-Ausfuehrung fehlgeschlagen");
                appendAgentChat("BRX", QString("%1 fehlgeschlagen: %2").arg(tool, message));
                appendBridgeLog(QString("BRX -> Qt: ERROR %1 %2").arg(bridgeMethod, message));
                clearAgentProposal();
                m_agentValidationRetries = 0;
                appendAgentChat("Barebone-Qt", "Automatische Folgeaktionen wurden nach dem BRX-Fehler gestoppt, damit BricsCAD nicht weiter belastet wird. Bitte pruefe, ob BricsCAD noch stabil laeuft, und sende die korrigierte Anweisung danach erneut.");
                return;
            }

            const QJsonObject result = response.value("result").toObject();
            m_lastAgentToolResult = result;
            m_pendingAgentDraft = {};
            m_agentValidationRetries = 0;

            const QString summary = result.value("summary").toString().trimmed();
            if (!summary.isEmpty()) {
                appendAgentChat("BRX", summary);
            } else if (result.contains("affected") || result.contains("failed")) {
                appendAgentChat("BRX", QString("%1 ausgefuehrt: %2 betroffen, %3 Fehler.")
                    .arg(tool)
                    .arg(result.value("affected").toInt())
                    .arg(result.value("failed").toInt(result.value("errors").toInt())));
            } else if (result.contains("handle")) {
                appendAgentChat("BRX", QString("%1 ausgefuehrt: Handle %2.")
                    .arg(tool, result.value("handle").toString()));
            } else {
                appendAgentChat("BRX", QString("%1 ausgefuehrt.").arg(tool));
            }

            appendBridgeLog(QString("BRX -> Qt: %1 ausgefuehrt result=%2")
                .arg(bridgeMethod, QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(1000)));
            const QString resultSummary = !summary.isEmpty()
                ? summary
                : QString("%1 wurde erfolgreich ausgefuehrt.").arg(tool);
            m_agentConversation.append(QJsonObject{
                {"role", "assistant"},
                {"content", QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "message"},
                    {"message", QString("BRX Ergebnis: %1 Aktion abgeschlossen; offene Rueckfragen sind erledigt.").arg(resultSummary)},
                    {"tool", tool},
                    {"status", "completed"},
                }).toJson(QJsonDocument::Compact))},
            });
            clearAgentProposal();
            continueAgentAfterToolResult(executedProposal, response);
        });
    if (!queued) {
        appendAgentChat("Barebone-Qt", QString("BRX Plugin ist nicht verbunden. %1 wurde nicht gesendet.").arg(tool));
    }
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

    QJsonArray readOnlyMethods;
    for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
        const QJsonObject method = value.toObject();
        if (method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }

    QJsonObject envelope;
    envelope.insert("type", "tool_result");
    envelope.insert("nextIntent", nextIntent);
    envelope.insert("completedProposal", proposal);
    envelope.insert("toolResponse", response);
    envelope.insert("context", currentAgentContext());
    envelope.insert("capabilities", m_brxCapabilities);
    envelope.insert("readOnlyMethods", readOnlyMethods);
    envelope.insert("tools", availableAgentTools());
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("expectedResponse", "strict-json-object");

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

    QJsonArray readOnlyMethods;
    for (const QJsonValue& value : m_brxCapabilities.value("methods").toArray()) {
        const QJsonObject method = value.toObject();
        if (method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }

    QJsonObject envelope = agentRequestEnvelope(m_lastAgentUserPrompt.isEmpty()
        ? QStringLiteral("Korrigiere die fehlgeschlagene BricsCAD-Aktion.")
        : m_lastAgentUserPrompt,
        m_lastDocumentContext);
    envelope.insert("type", "tool_error");
    envelope.insert("executionError", errorMessage);
    envelope.insert("failedProposal", proposal);
    envelope.insert("toolResponse", response);
    envelope.insert("context", currentAgentContext());
    envelope.insert("capabilities", m_brxCapabilities);
    envelope.insert("readOnlyMethods", readOnlyMethods);
    envelope.insert("tools", availableAgentTools());
    envelope.insert("expectedResponse", "strict-json-object");
    envelope.insert("agentLoop", QJsonObject{
        {"retry", m_agentValidationRetries},
        {"maxRetries", kMaxAgentValidationRetries},
        {"policy", "The user-confirmed tool call failed in BRX. Correct the next response instead of repeating the same proposal."},
    });
    envelope.insert("instruction",
        "Die bestaetigte Aktion wurde von BRX abgelehnt. Wiederhole nicht denselben Vorschlag. "
        "Nutze executionError, failedProposal, tools[].inputSchema und apiDoc.post, um params oder tool zu korrigieren. "
        "Wenn du eine korrigierte Aktion ausfuehren willst, antworte mit genau einem action_proposal. "
        "Wenn echte Informationen fehlen, nutze ask_user. Wenn Zeichnungskontext fehlt, nutze context_request. "
        "Wenn der urspruengliche Wunsch mehrere Schritte hat, setze continueAfterSuccess=true und nextIntent fuer den naechsten Schritt.");

    const QString compact = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    sendAgentEnvelope(envelope, compact, false, QString("tool_error_loop_%1").arg(m_agentValidationRetries));
}

void BricsCadPage::appendAgentChat(const QString& speaker, const QString& message)
{
    if (!m_agentBridge) {
        return;
    }

    const QString visibleMessage = speaker == QStringLiteral("AI")
        ? removeReasoningLeak(message)
        : repairMojibakeText(message);

    Q_EMIT m_agentBridge->messageAdded(QVariantMap{
        {"speaker", speaker},
        {"message", visibleMessage.trimmed()},
        {"time", QDateTime::currentDateTime().toString("HH:mm 'Uhr'")},
    });
}

void BricsCadPage::clearAgentProposal()
{
    m_pendingAgentProposal = {};
    if (m_agentBridge) {
        Q_EMIT m_agentBridge->proposalCleared();
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

    Q_EMIT m_agentBridge->proposalChanged(QVariantMap{
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

    const QString tool = proposal.value("tool").toString();
    const QJsonObject params = proposal.value("params").toObject();
    const QJsonObject definition = toolDefinition(tool);
    const QString paramsText = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    const QString summary = proposal.value("summary").toString(
        proposal.value("message").toString()).trimmed();

    QStringList lines;
    lines << QString("Werkzeug: %1").arg(definition.value("title").toString(tool));
    lines << QString("Name: %1").arg(tool.isEmpty() ? QStringLiteral("<fehlt>") : tool);
    lines << QString("Kategorie: %1").arg(definition.value("category").toString("general"));
    lines << QString("Bridge: %1").arg(definition.value("bridgeMethod").toString(tool));
    lines << QString("Risiko: %1").arg(definition.value("risk").toString("modifiesDrawing"));
    lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
    lines << QString("Parameter: %1").arg(paramsText.isEmpty() ? QStringLiteral("{}") : paramsText);

    const QString reason = proposal.value("reason").toString().trimmed();
    if (!reason.isEmpty()) {
        lines << QString("Grund: %1").arg(reason);
    }
    const QString nextIntent = proposal.value("nextIntent").toString().trimmed();
    if (!nextIntent.isEmpty()) {
        lines << QString("Danach: %1").arg(nextIntent);
    }

    if (m_agentBridge) {
        Q_EMIT m_agentBridge->proposalChanged(QVariantMap{
            {"title", "AI Vorschlag bereit"},
            {"summary", summary.isEmpty() ? QStringLiteral("Der Agent hat eine BricsCAD-Aktion vorbereitet.") : summary},
            {"details", lines.join("\n")},
            {"canRun", true},
        });
    }
}

void BricsCadPage::setAgentBusy(bool busy)
{
    m_agentBusy = busy;
    if (m_agentBridge) {
        Q_EMIT m_agentBridge->statusChanged(busy ? QStringLiteral("thinking") : QStringLiteral("idle"));
    }
}

bool BricsCadPage::isAgentConfirmation(const QString& prompt) const
{
    const QString normalized = prompt.trimmed().toLower();
    return normalized == "ja"
        || normalized == "ok"
        || normalized == "ausfuehren"
        || normalized == "ausfÃ¼hren"
        || normalized == "mach"
        || normalized == "mach das"
        || normalized == "bestaetigen"
        || normalized == "bestÃ¤tigen";
}

bool BricsCadPage::validateAgentProposal(const QJsonObject& proposal, QString& errorMessage) const
{
    if (proposal.isEmpty()) {
        errorMessage = "kein offener Vorschlag";
        return false;
    }

    const QString tool = proposal.value("tool").toString().trimmed();
    if (tool.isEmpty()) {
        errorMessage = "tool fehlt";
        return false;
    }

    const QJsonValue paramsValue = proposal.value("params");
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

    if (!proposal.value("nextIntent").toString().trimmed().isEmpty()
        && !proposal.value("continueAfterSuccess").toBool(false)) {
        errorMessage = "nextIntent ist gesetzt, aber continueAfterSuccess ist false";
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
    const QString activePrompt = m_lastAgentUserPrompt.toLower();
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

    return validateToolParams(params, definition.value("inputSchema").toObject(), errorMessage);
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
        if (name == "command.execute") {
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
        tools.append(tool);
    }

    return tools;
}

QJsonObject BricsCadPage::toolDefinition(const QString& name) const
{
    for (const QJsonValue& value : availableAgentTools()) {
        const QJsonObject tool = value.toObject();
        if (tool.value("name").toString() == name) {
            return tool;
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

QJsonObject BricsCadPage::agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext) const
{
    QJsonArray readOnlyMethods;
    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value("risk").toString() == "readOnly") {
            readOnlyMethods.append(method);
        }
    }
    if (readOnlyMethods.isEmpty()) {
        for (const QString& method : QStringList{"actions.list", "layers.list", "geometry.query", "selection.describe", "entity.describe", "measurement.bbox", "measurement.length", "measurement.area"}) {
            readOnlyMethods.append(QJsonObject{{"name", method}, {"risk", "readOnly"}});
        }
    }

    QJsonObject envelope;
    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    envelope.insert("userPrompt", prompt);
    envelope.insert("cadIntent", true);
    envelope.insert("includeConversationHistory", true);
    envelope.insert("context", currentAgentContext());
    if (!sanitizedContext.isEmpty()) {
        envelope.insert("documentContext", sanitizedContext);
        envelope.insert("documentPolicy", QJsonObject{
            {"mode", "attached-document-context"},
            {"response", "If the user asks about attached PDFs, Word documents, or text files, answer from documentContext as type=message unless a BricsCAD action is explicitly requested."},
            {"limits", "documentContext may contain only requested pages or capped excerpts. Ask for a narrower page range if the answer cannot be derived from the excerpts."},
        });
    }
    envelope.insert("capabilities", m_brxCapabilities);
    envelope.insert("readOnlyMethods", readOnlyMethods);
    envelope.insert("geometryDataModel",
        QJsonObject{
            {"sourceOfTruth", "BRX readOnlyMethods"},
            {"recommendedFlow", QJsonArray{"actions.list", "layers.list", "geometry.query", "selection.describe", "entity.describe", "measurement.bbox", "measurement.length", "measurement.area"}},
            {"fields", QJsonArray{"handle", "type", "kind", "shape", "layer", "bounds", "geometry.vertices", "metrics.length", "metrics.area", "metrics.height", "metrics.volume"}},
            {"policy", "Use fetched geometry data to classify entities. Do not assume an action exists just because the user asks for it."},
        });
    envelope.insert("operationLimits", QJsonObject{
        {"subentityFaceMove", QJsonObject{
            {"available", false},
            {"reason", "No confirmed action tool for selecting and transforming individual AcDb3dSolid faces is exposed yet."},
            {"policy", "Do not invent solid.face.move, face.move or use geometry.move for a single face. geometry.move moves whole entities. If the user wants to lengthen a wall face, use measurement.bbox first and then propose a safe replacement workflow only with existing tools, or return plan/missingCapabilities."},
        }},
        {"axisExtension", QJsonObject{
            {"preferredContext", "measurement.bbox"},
            {"forbiddenTools", QJsonArray{"geometry.scale"}},
            {"policy", "A standalone mm value after a pending extension or face-move request is the distance/offset value, not a new unrelated prompt."},
        }},
    });
    envelope.insert("actionToolsEnabled", kAgentActionToolsEnabled);
    envelope.insert("reasoning", QJsonObject{{"effort", normalizedReasoningEffort(m_reasoningEffort)}});
    envelope.insert("executionPolicy", QJsonObject{
        {"mode", kAgentActionToolsEnabled ? QStringLiteral("confirmed-actions") : QStringLiteral("message-only")},
        {"toolProposalAllowed", kAgentActionToolsEnabled && !availableAgentTools().isEmpty()},
        {"whenNoToolFits", "plan"},
    });
    envelope.insert("tools", availableAgentTools());
    envelope.insert("pendingProposal", m_pendingAgentProposal);
    envelope.insert("pendingDraft", m_pendingAgentDraft);
    envelope.insert("lastToolResult", m_lastAgentToolResult);
    envelope.insert("conversationMode", "direct-ai-with-confirmed-actions");
    envelope.insert("expectedResponse", "strict-json-object");
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
    m_queuedAgentPrompt.clear();
    if (prompt.isEmpty()) {
        return;
    }

    if (!m_brxAuthenticated || availableAgentTools().isEmpty()) {
        setAgentBusy(false);
        appendAgentChat("Barebone-Qt", "BRX Toolliste konnte nicht geladen werden. Prompt wurde nicht an die AI gesendet.");
        appendBridgeLog("AI Agent: queued prompt verworfen, keine BRX Capabilities");
        return;
    }

    setAgentBusy(false);
    appendBridgeLog("AI Agent: queued prompt wird nach Capabilities-Laden fortgesetzt");
    if (shouldPrefetchSelectionDescription(prompt)) {
        sendAgentRequestWithPrefetchedContext(
            prompt,
            "selection.describe",
            QJsonObject{
                {"include", QJsonArray{"geometry"}},
                {"limit", 100},
            },
            m_lastDocumentContext);
        return;
    }
    sendAgentRequest(prompt, m_lastDocumentContext);
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
        requestBridgeCapabilities();
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
        Q_EMIT m_agentBridge->bridgeStatusChanged(message, connected);
    }
}

void BricsCadPage::setLocalAiStatus(const QString& message, bool connected)
{
    m_localAiStatusMessage = message;
    m_localAiReachable = connected;

    if (m_agentBridge) {
        Q_EMIT m_agentBridge->localAiStatusChanged(message, connected);
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
        Q_EMIT m_agentBridge->bridgeLogAdded(line);
    }
}

void BricsCadPage::emitCapabilitiesStatusToWeb() const
{
    if (!m_agentBridge || m_brxCapabilities.isEmpty()) {
        return;
    }

    const int methodCount = m_brxCapabilities.value("methods").toArray().size();
    const int commandCount = m_brxCapabilities.value("commands").toArray().size();
    int toolCount = 0;
    const QJsonArray methods = m_brxCapabilities.value("methods").toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value("kind").toString() == "action"
            && method.value("name").toString() != "command.execute") {
            ++toolCount;
        }
    }

    const QString line = QString("[%1] BRX -> Qt: %2 Capabilities, %3 Commands, %4 Action-Tools")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
        .arg(methodCount)
        .arg(commandCount)
        .arg(toolCount);
    Q_EMIT m_agentBridge->bridgeLogAdded(line);
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
