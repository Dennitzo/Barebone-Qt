#include "BricsCadPage.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
constexpr int kMaxAgentBatchActions = 25;
constexpr int kAgentBatchActionDelayMs = 1000;
constexpr int kAiModelResponseTimeoutMs = 20 * 60 * 1000;
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
    QStringList refs{QStringLiteral("bricscad-safety")};
    if (name.startsWith(QStringLiteral("layers."))
        || name == QStringLiteral("layers.ensureMany")) {
        refs << QStringLiteral("layers");
    }
    if (name.startsWith(QStringLiteral("geometry."))
        || name.startsWith(QStringLiteral("rectangles."))
        || name.startsWith(QStringLiteral("profile."))
        || name.startsWith(QStringLiteral("bim."))
        || name.startsWith(QStringLiteral("selection."))) {
        refs << QStringLiteral("geometry");
    }
    refs.removeDuplicates();
    return refs;
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

    if (name == QStringLiteral("geometry.create")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("createsPrimitiveGeometry", true);
        caps.insert("units", "mm");
        caps.insert("supportedGeometryIntent", QJsonArray{"point", "line", "rectangle", "polyline", "circle", "arc", "box"});
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "Neue Grundgeometrie bevorzugt mit geometry.create erzeugen.",
            "rectangle: zweite 2D-Abmessung als depth oder length senden, nicht als height.",
            "box kann fuer einfache Wandkoerper genutzt werden: width=Laenge, depth=Wandstaerke, height=Hoehe.",
            "Wenn der Nutzer Ursprung sagt, center/basePoint/start je nach Schema mit {\"x\":0,\"y\":0,\"z\":0} interpretieren."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "Einheiten sind mm.",
            "Fehlender Layer darf als Layer 0 angenommen werden, wenn das Schema Layer optional macht.",
            "Nicht fuer Face-/Subentity-Bearbeitung verwenden."
        });
        return;
    }

    if (name == QStringLiteral("rectangles.extrude")
        || name == QStringLiteral("profile.extrude")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("createsSolids", true);
        caps.insert("requiresClosedProfile", true);
        caps.insert("heightUnit", "mm");
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "Extrusionen verwenden eine positive Hoehe in mm.",
            "Eine alleinstehende mm-Angabe im Extrusionskontext bedeutet heightMm.",
            "Bei Auswahl/selektiert bevorzugt selector.scope=\"selection\" verwenden."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "Nur geschlossene Rechtecke/Profile extrudieren.",
            "heightMm muss groesser als 0 sein.",
            "Wenn das Profil unklar ist, zuerst readOnly-Kontext per selection.describe oder geometry.query abfragen."
        });
        return;
    }

    if (name == QStringLiteral("bim.classify")) {
        tool.insert("capabilities", QJsonObject{
            {"supportedClassifications", QJsonArray{"BIMWall"}},
            {"supportedTargets", QJsonArray{"selection", "handles", "lastExtruded"}},
            {"requiresSolidTarget", true},
            {"supportsCurrentSelection", true},
        });
        tool.insert("agentHints", QJsonArray{
            "Fuer 'als Wand klassifizieren' classification=BIMWall verwenden.",
            "Bei aktueller Auswahl bevorzugt params.selector={\"scope\":\"selection\",\"kind\":\"solid\"} senden.",
            "Bei soeben erzeugter/extrudierter Geometrie target=lastExtruded verwenden.",
            "Nicht behaupten, dass BIM-Klassifizierung fehlt, wenn dieses Tool in tools[] vorhanden ist.",
        });
        tool.insert("semanticConstraints", QJsonArray{
            "classification erlaubt aktuell nur BIMWall.",
            "Das Ziel muss ein 3D-Solid sein; bei Auswahl selector.kind=solid setzen.",
            "target und selector sind alternative Zielangaben; mindestens eine davon ist erforderlich.",
        });
        tool.insert("externalReferences", QJsonArray{
            QJsonObject{
                {"title", "BricsCAD BIMCLASSIFY command"},
                {"url", "https://help.bricsys.com/en-us/document/command-reference/b/bimclassify-command"},
                {"note", "Official command reference: BIMCLASSIFY classifies selected entities as BIM elements."}
            }
        });
        return;
    }

    if (name == QStringLiteral("geometry.move")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("movesWholeEntities", true);
        caps.insert("supportsFaceMove", false);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "geometry.move verschiebt ganze Entities ueber Selector.",
            "Bei Auswahl/selektiert selector.scope=\"selection\" verwenden.",
            "Fuer Face-/Subentity-Bewegung ist dieses Tool nicht geeignet."
        });
        mergeJsonStringArray(tool, "unsupportedOperations", QJsonArray{
            "single_face_move",
            "one_side_wall_lengthen"
        });
        return;
    }

    if (name == QStringLiteral("geometry.copy")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("copiesWholeEntities", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "geometry.copy kopiert ganze Entities ueber Selector.",
            "Bei Mehrfachkopien Kopienanzahl/Spacing nur verwenden, wenn im Schema vorhanden.",
            "Bei Auswahl/selektiert selector.scope=\"selection\" verwenden."
        });
        return;
    }

    if (name == QStringLiteral("geometry.rotate")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("rotatesWholeEntities", true);
        caps.insert("angleUnit", "degrees");
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "geometry.rotate rotiert ganze Entities um eine Basis/Rotationsachse gemaess Schema.",
            "Winkel als Zahl senden; falls der Nutzer Grad nennt, als Grad interpretieren.",
            "Bei Auswahl/selektiert selector.scope=\"selection\" verwenden."
        });
        return;
    }

    if (name == QStringLiteral("geometry.scale")) {
        tool.insert("capabilities", QJsonObject{
            {"scaleMode", "uniformOnly"},
            {"supportsUniformScale", true},
            {"supportsAxisScale", false},
            {"supportsStretch", false},
            {"supportsLengthen", false},
        });
        tool.insert("agentHints", QJsonArray{
            "Nur gleichfoermige Skalierung mit params.factor ist erlaubt.",
            "Nicht fuer Verlaengern, Strecken oder einseitige Achsenanpassung verwenden.",
            "Bei Verlaengern zuerst measurement.bbox abfragen und dann einen sicheren Ersatz-Workflow planen.",
        });
        tool.insert("semanticConstraints", QJsonArray{
            "params.factor muss > 0 sein.",
            "xFactor, yFactor, zFactor, scaleX, scaleY, scaleZ und targetLength sind ungueltig.",
            "basePoint ist optional; wenn der Nutzer Ursprung sagt, {\"x\":0,\"y\":0,\"z\":0} verwenden.",
        });
        tool.insert("unsupportedOperations", QJsonArray{
            QJsonObject{
                {"intent", "non_uniform_axis_scale"},
                {"reason", "BRX geometry.scale akzeptiert nur factor."},
                {"fallback", "Nur ein passendes anderes freigegebenes Tool oder validiertes command.execute verwenden; sonst plan/ask_user."},
            },
            QJsonObject{
                {"intent", "one_axis_lengthen_or_stretch"},
                {"reason", "geometry.scale veraendert alle Achsen gleichfoermig."},
                {"fallback", "measurement.bbox fuer Ist-Abmessungen abfragen und sicheren Ersatz-Workflow vorschlagen."},
            },
        });
        tool.insert("externalReferences", QJsonArray{
            QJsonObject{
                {"title", "BricsCAD SCALE command"},
                {"url", "https://help.bricsys.com/en-us/document/command-reference/s/scale-command"},
                {"note", "Official command reference: SCALE resizes a selection set relative to a base point using a scale factor."}
            }
        });
        return;
    }

    if (name == QStringLiteral("geometry.delete")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("deletesEntities", true);
        caps.insert("requiresConfirmTrue", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "geometry.delete loescht Entities ueber Selector.",
            "params.confirm muss true sein.",
            "Bei unklarem Ziel zuerst context_request nutzen statt breit zu loeschen."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "Loeschen immer eng auf Auswahl, Handles oder eindeutige Filter begrenzen.",
            "Nie ohne Nutzerabsicht loeschen."
        });
        return;
    }

    if (name == QStringLiteral("selection.set")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("modifiesDrawing", false);
        caps.insert("modifiesEditorSelection", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "selection.set aendert nur die aktive Auswahl im Editor.",
            "Nutze Handles oder eindeutige Selector-Kriterien.",
            "Bei unklarer Auswahl zuerst readOnly-Kontext abfragen."
        });
        return;
    }

    if (name == QStringLiteral("layers.ensureMany")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("virtualQtTool", true);
        caps.insert("expandsTo", "layers.create[]");
        caps.insert("internalBatch", true);
        caps.insert("maxBatchActions", kMaxAgentBatchActions);
        caps.insert("supportsAciColor", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "Fuer mehrere neue Layer mit bekannten Namen/Farben bevorzugt layers.ensureMany verwenden.",
            "params.layers enthaelt eine kompakte Liste aus {name, optional colorIndex}.",
            "Qt expandiert dieses Tool vor Preflight und Ausfuehrung in einzelne layers.create-Aktionen.",
            "TGA-Defaults: Heizung rot=1, Sanitaer blau/cyan=5 oder 4, Lueftung gruen=3, Elektro gelb=2."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "Nur fuer Layer-Neuanlage verwenden, nicht fuer Rename oder Farbwechsel vorhandener Layer.",
            "Jeder Layer braucht einen nicht-leeren eindeutigen Namen.",
            "colorIndex ist optional; wenn gesetzt, muss es eine ACI-Farbe 1..255 sein."
        });
        return;
    }

    if (name == QStringLiteral("layers.create")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("usesNativeLayerCommand", true);
        caps.insert("supportsAciColor", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "layers.create legt einen einzelnen Layer an.",
            "Fuer mehrere Layer bevorzugt layers.ensureMany verwenden, wenn verfuegbar.",
            "colorIndex ist ACI-Farbe und optional, wenn das Schema es erlaubt."
        });
        return;
    }

    if (name == QStringLiteral("layers.rename")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("usesNativeLayerCommand", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "layers.rename benennt genau einen vorhandenen Layer um.",
            "Alter und neuer Layername muessen eindeutig sein.",
            "Bei unklaren Layernamen zuerst layers.list als context_request verwenden."
        });
        return;
    }

    if (name == QStringLiteral("layers.setColor")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("usesNativeLayerCommand", true);
        caps.insert("supportsAciColor", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "layers.setColor setzt eine ACI-Farbe fuer einen vorhandenen Layer.",
            "ACI-Farben: 1 rot, 2 gelb, 3 gruen, 4 cyan, 5 blau, 6 magenta, 7 weiss/schwarz.",
            "Bei unklarem Layernamen zuerst layers.list als context_request verwenden."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "colorIndex muss eine gueltige ACI-Farbe sein.",
            "Layer muss existieren."
        });
        return;
    }

    if (name == QStringLiteral("command.execute")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("executesNativeCommand", true);
        caps.insert("requiresKnownCommand", true);
        caps.insert("singleCommandLineOnly", true);
        caps.insert("commandWhitelistSource", "commands.list");
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "command.execute nur verwenden, wenn ein nativer BricsCAD-Command besser passt als ein spezialisiertes Bridge-Tool.",
            "Die Kommandozeile muss vollstaendig und genau ein einzelner Command aus commands.list sein.",
            "Spezialisierte Bridge-Tools sind vorzuziehen, wenn sie die Absicht direkt abdecken."
        });
        mergeJsonStringArray(tool, "semanticConstraints", QJsonArray{
            "Keine Semikolons, keine Newlines, keine verketteten Commands.",
            "Keine direkten DB-Schreiboperationen.",
            "BRX actions.validate muss den Command vor Nutzerbestaetigung akzeptieren."
        });
        mergeJsonStringArray(tool, "unsupportedOperations", QJsonArray{
            "multi_command_macro",
            "unlisted_native_command",
            "script_or_lisp_execution"
        });
        return;
    }

    if (name == QStringLiteral("document.save")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("savesActiveDocument", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "document.save speichert die aktive Zeichnung explizit.",
            "Nur verwenden, wenn der Nutzer Speichern verlangt oder ein Workflow eine explizite Sicherung braucht."
        });
        return;
    }

    if (name == QStringLiteral("undo.last") || name == QStringLiteral("undo.redo")) {
        QJsonObject caps = tool.value("capabilities").toObject();
        caps.insert("changesUndoStackState", true);
        tool.insert("capabilities", caps);
        mergeJsonStringArray(tool, "agentHints", QJsonArray{
            "Undo/Redo nur bei ausdruecklicher Nutzerabsicht verwenden.",
            "Schritte begrenzen und keine neuen Zeichnungsobjekte behaupten."
        });
    }
}

bool documentContextHasText(const QJsonObject& context)
{
    return !context.value("selectedText").toString().trimmed().isEmpty();
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
        && normalized.contains(QStringLiteral("bim"))
        && normalized.contains(QStringLiteral("wand"))
        && textMentionsAny(normalized, {
            QStringLiteral("als"),
            QStringLiteral("klassifiz"),
            QStringLiteral("klassifizi"),
            QStringLiteral("klassifizierung"),
            QStringLiteral("classification"),
            QStringLiteral("wandle"),
            QStringLiteral("umwandel"),
        });
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
    const QString normalized = prompt.toLower();
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
    if (routeAllowsCadActions(route)
        && textMentionsAny(normalized, {
            QStringLiteral("layer"),
            QStringLiteral("ebene"),
            QStringLiteral("tga"),
            QStringLiteral("heizung"),
            QStringLiteral("sanit"),
            QStringLiteral("lueft"),
            QStringLiteral("lüft"),
            QStringLiteral("elektro"),
        })) {
        refs << QStringLiteral("layers");
    }
    if (routeAllowsCadActions(route)
        && textMentionsAny(normalized, {
            QStringLiteral("geometrie"),
            QStringLiteral("kreis"),
            QStringLiteral("rechteck"),
            QStringLiteral("linie"),
            QStringLiteral("polyline"),
            QStringLiteral("wand"),
            QStringLiteral("solid"),
            QStringLiteral("box"),
            QStringLiteral("extrusion"),
            QStringLiteral("extrudi"),
            QStringLiteral("verschiebe"),
            QStringLiteral("skaliere"),
            QStringLiteral("bim"),
            QStringLiteral("klassifiz"),
            QStringLiteral("klassifizi"),
        })) {
        refs << QStringLiteral("geometry");
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
        QStringLiteral("geometrie"),
        QStringLiteral("extrud"),
        QStringLiteral("bim"),
        QStringLiteral("klassifiz"),
    });

    if (mentionsLayer && (toolName.startsWith(QStringLiteral("layers."))
        || toolName == QStringLiteral("layers.ensureMany"))) {
        return true;
    }
    if (mentionsGeometry) {
        return toolName == QStringLiteral("geometry.create")
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
            const QString text = repairMojibakeText(value.toString()).trimmed();
            if (!text.isEmpty()) {
                result << text;
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
        lines << QStringLiteral("**Berechnungen**");
        int count = 0;
        for (const QJsonValue& value : derivedValues) {
            if (count++ >= 6) {
                break;
            }
            const QJsonObject derived = value.toObject();
            const QString name = repairMojibakeText(derived.value("name").toString()).trimmed();
            const QString expression = repairMojibakeText(derived.value("expression").toString()).trimmed();
            const QString example = workflowJsonValueSummary(derived.value("example"));
            QString line = name.isEmpty() ? QStringLiteral("Berechnung") : name;
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
        {"legacyAcceptedOnlyForCompatibility", true},
        {"topLevel", QJsonObject{
            {"required", QJsonArray{"schema", "type", "message"}},
            {"schema", "barebone.agent.response.v2"},
            {"allowedTypes", QJsonArray{"message", "ask_user", "context_request", "action_proposal", "plan"}},
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
    if (canonicalSlot == QStringLiteral("classification")) {
        return {"bim", "bimwall", "klassifizierung", "classification"};
    }
    return {workflowTrainingSearchText(canonicalSlot)};
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

bool workflowActionCreatesRectangle(const QJsonObject& action, const QString& paramsKey)
{
    const QString tool = action.value("tool").toString().trimmed();
    const QJsonObject params = action.value(paramsKey).toObject();
    return tool == QStringLiteral("geometry.create")
        && params.value("geometry").toString().compare(QStringLiteral("rectangle"), Qt::CaseInsensitive) == 0;
}

bool workflowActionExtrudesProfile(const QJsonObject& action)
{
    const QString tool = action.value("tool").toString().trimmed();
    return tool == QStringLiteral("rectangles.extrude") || tool == QStringLiteral("profile.extrude");
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

    if (tool == QStringLiteral("bim.classify") && workflowActionExtrudesProfile(previousAction)) {
        QJsonObject selector = params.value("selector").toObject();
        if (selector.value("scope").toString().compare(QStringLiteral("selection"), Qt::CaseInsensitive) == 0) {
            selector.insert(QStringLiteral("scope"), QStringLiteral("lastExtruded"));
            selector.insert(QStringLiteral("kind"), QStringLiteral("solid"));
            params.insert(QStringLiteral("selector"), selector);
            action.insert(paramsKey, params);
            if (changed) {
                *changed = true;
            }
        } else if (params.value("target").toString().trimmed().isEmpty()
            && !params.value("selector").isObject()
            && !params.value("handles").isArray()) {
            params.insert(QStringLiteral("target"), QStringLiteral("lastExtruded"));
            action.insert(paramsKey, params);
            if (changed) {
                *changed = true;
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
    QObject::connect(m_agentBridge, &AiWebBridge::trainingModeChanged, this, [this](bool enabled) {
        setTrainingMode(enabled);
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
        "Du bist ein allgemeiner AI Chat-Assistent in Barebone-Qt. "
        "Antworte direkt, hilfreich und auf Deutsch, sofern der Nutzer keine andere Sprache wuenscht. "
        "Ohne freigegebene Tools sollst du keine Aktionen in BricsCAD behaupten. "
        "Wenn die Nutzeranfrage einen Dokumentkontext enthaelt, nutze diesen als primaere Quelle und verweise bei PDFs nach Moeglichkeit auf Seiten. "
        "Wenn der Nutzer nach Rechtschreibfehlern, Tippfehlern oder Korrektur fragt, liste gefundene Stellen mit Original und Korrektur; wenn du keine offensichtlichen Fehler findest, sage das ausdruecklich.";

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
    if (isBricsCadMode() && m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
        requestBridgeCapabilities();
    }
}

void BricsCadPage::setTrainingMode(bool enabled)
{
    if (m_trainingMode == enabled) {
        if (m_agentBridge) {
            Q_EMIT m_agentBridge->trainingModeApplied(m_trainingMode);
        }
        return;
    }

    m_trainingMode = enabled;
    if (m_trainingMode && m_chatMode != QStringLiteral("bricscad")) {
        m_chatMode = QStringLiteral("bricscad");
        appendBridgeLog("AI Agent: Trainingsmodus nutzt BricsCAD Modus");
    }
    clearAgentProposal();
    m_pendingAgentDraft = {};
    m_pendingAgentProposal = {};
    m_agentValidationRetries = 0;
    m_trainingValidationRetries = 0;
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
    if (m_trainingMode) {
        m_trainingConversation = {};
        m_trainingWorkflowContext = {};
        m_trainingSlotValues = {};
        m_trainingMissing = {};
        m_trainingPhase = QStringLiteral("collecting");
    } else {
        m_trainingPhase = QStringLiteral("idle");
    }

    if (m_agentBridge) {
        Q_EMIT m_agentBridge->trainingModeApplied(m_trainingMode);
    }

    if (m_trainingMode) {
        appendBridgeLog("Workflow Training: aktiviert");
        if (m_brxAuthenticated && m_brxCapabilities.isEmpty()) {
            requestBridgeCapabilities();
        }
        appendAgentChat("Barebone-Qt", "Trainingsmodus aktiv. Welchen Workflow wollen wir erstellen, erweitern oder bearbeiten?");
    } else {
        appendBridgeLog("Workflow Training: deaktiviert");
        appendAgentChat("Barebone-Qt", "Trainingsmodus deaktiviert. Normale Chat- und BricsCAD-Interaktion ist wieder aktiv.");
    }
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

    if (m_trainingMode) {
        m_agentValidationRetries = 0;
        m_lastAgentUserPrompt = prompt;
        m_lastDocumentContext = {};
        clearAgentProposal();
        const QJsonObject detectedSlots = workflowTrainingSlotValuesFromPrompt(prompt, m_trainingMissing, m_trainingWorkflowContext);
        if (!detectedSlots.isEmpty()) {
            mergeWorkflowTrainingSlotValues(detectedSlots);
            appendBridgeLog(QString("Workflow Training Slots: %1")
                .arg(QString::fromUtf8(QJsonDocument(detectedSlots).toJson(QJsonDocument::Compact))));
        }
        if (m_trainingSaveReviewPending) {
            if (isWorkflowTrainingReviewConfirmationText(prompt)) {
                appendBridgeLog("Workflow Training Review: Nutzer hat die Vor-Speicher-Review bestaetigt");
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
                return;
            }

            appendBridgeLog("Workflow Training Review: Nutzerantwort wird zur Ueberarbeitung an die AI gegeben");
            m_trainingSaveReviewPending = false;
            m_trainingReviewConfirmed = false;
            m_trainingPendingReviewContent.clear();
            m_trainingPendingReviewReply = {};
            m_trainingPendingReviewWorkflow = {};
            m_trainingPhase = QStringLiteral("reviewing");
            sendWorkflowTrainingPrompt(QString(
                "Der Nutzer hat auf die Vor-Speicher-Review geantwortet und moeglicherweise Korrekturen ergaenzt:\n%1\n\n"
                "Ueberarbeite den aktiven Workflow anhand dieser Antwort. Denke die Punkte erneut durch. "
                "Antworte mit type=workflow_draft und gehe die Zusammenfassung wieder punktweise durch: Titel, Beschreibung, Eingaben, Berechnungen, Batch-Ausfuehrungen, Validierungsbeispiel und offene Risiken. "
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

    if (isAgentConfirmation(prompt) && !m_pendingAgentProposal.isEmpty()) {
        executeAgentProposal();
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
        clearAgentProposal();
        appendBridgeLog("AI Agent: alter Vorschlag verworfen, neuer Nutzerprompt ist massgebend");
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

    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, documentContext, fallbackRoute, mode]() {
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

    const QString corePrompt = agentResourceText(QStringLiteral(":/agent/policies/core.md"));
    const QJsonObject systemMessage{
        {"role", "system"},
        {"content", corePrompt.isEmpty()
            ? QStringLiteral("Du bist der AI Assistent fuer Barebone-Qt. Antworte ausschliesslich mit einem gueltigen JSON-Objekt gemaess responseContract.")
            : corePrompt},
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
    const QJsonObject route = envelope.value("route").toObject();
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

QString BricsCadPage::workflowsDirectoryPath() const
{
    QDir root(bareboneProjectRootPath());
    return root.filePath(QStringLiteral("agent/workflows"));
}

QJsonArray BricsCadPage::workflowTrainingIndex() const
{
    QDir dir(workflowsDirectoryPath());
    QJsonArray workflows;
    if (!dir.exists()) {
        return workflows;
    }

    const QStringList files = dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fileName : files) {
        QFile file(dir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            workflows.append(QJsonObject{
                {"fileName", fileName},
                {"error", parseError.errorString()},
            });
            continue;
        }

        QJsonObject workflow = document.object();
        const QByteArray compact = QJsonDocument(workflow).toJson(QJsonDocument::Compact);
        if (compact.size() > 12000) {
            workflow = QJsonObject{
                {"schema", workflow.value("schema")},
                {"id", workflow.value("id")},
                {"title", workflow.value("title")},
                {"triggerExamples", workflow.value("triggerExamples")},
                {"requiredSlots", workflow.value("requiredSlots")},
                {"preferredTools", workflow.value("preferredTools")},
                {"note", "Workflow wurde fuer den Trainingskontext gekuerzt; Datei ist groesser als 12k."},
            };
        }

        workflows.append(QJsonObject{
            {"fileName", fileName},
            {"id", workflow.value("id").toString(QFileInfo(fileName).baseName())},
            {"title", workflow.value("title").toString(QFileInfo(fileName).baseName())},
            {"workflow", workflow},
        });
    }
    return workflows;
}

QJsonObject BricsCadPage::workflowTrainingState() const
{
    return QJsonObject{
        {"phase", m_trainingPhase},
        {"knownSlotValues", m_trainingSlotValues},
        {"lastMissing", m_trainingMissing},
        {"activeWorkflow", m_trainingWorkflowContext},
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
    m_trainingWorkflowContext = merged;
}

QJsonArray BricsCadPage::unresolvedWorkflowTrainingMissing(const QJsonArray& missing) const
{
    QJsonArray unresolved;
    for (const QJsonValue& value : missing) {
        const QString name = workflowSlotNameFromValue(value);
        const QString canonical = canonicalWorkflowSlot(name);
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

    return QJsonObject{
        {"schema", "barebone.workflow.training.request.v1"},
        {"mode", "workflow_training"},
        {"compactContext", compactContext},
        {"userPrompt", prompt},
        {"instruction",
            "Hilf dem Nutzer, versionierte agent/workflows/*.json Dateien zu erstellen oder zu bearbeiten. "
            "Erzeuge keine CAD-Aktion und keine normale Barebone-Agent-action_proposal. "
            "Qt verwaltet trainingState, knownSlotValues und activeWorkflow verbindlich. Frage bekannte Slots niemals erneut ab. "
            "Lange Erstprompts mit Titel, Beschreibung, Batch-Ausfuehrungen, Berechnungen oder komplexer Logik strukturierst du zuerst als type=workflow_draft. "
            "workflow_draft ist ein sichtbarer Entwurf und darf workflowDraft mit description, derivedValues, executionBatches, constructionStrategy, authoringNotes und offenen questions enthalten, aber er wird nicht gespeichert. "
            "Sammle fehlende Angaben mit kurzen type=ask_user Antworten, bestaetige neu erkannte Werte mit type=slot_update oder aktualisiere den sichtbaren Entwurf mit type=workflow_draft. "
            "Vor dem Speichern erzwingt Qt eine Review-Phase: Wenn trainingState.saveReviewPending=true ist, pruefe Titel, Beschreibung, Pflichtangaben, Beispielwerte, Berechnungen, Batch-Ausfuehrungen, Tool-Parameter, Validierungsbeispiel und Risiken punktweise und antworte mit type=workflow_draft plus questions. "
            "Waehrend saveReviewPending=true darfst du kein workflow_update ausgeben, ausser der Nutzer hat die Review ausdruecklich bestaetigt und Qt fordert final dazu auf. "
            "Wenn noch Angaben fehlen, antworte mit type=ask_user, missing und questions; gib dabei hoechstens workflowSeed mit id/title/requiredSlots aus, aber keinen vollstaendigen Workflow. "
            "Erzeuge type=workflow_update erst, wenn die benoetigten Pflichtslots fachlich geklaert oder als variable requiredSlots definiert sind. "
            "Wenn der Nutzer den Entwurf bestaetigt oder Speichern/Aktualisieren verlangt, wandle workflowDraft in ein vollstaendiges workflow_update mit steps und validationExamples um. "
            "Wenn der Nutzer einen bestehenden Workflow erweitern oder bearbeiten moechte, nutze existingWorkflows und gib einen vollstaendigen gemergten workflow zurueck. "
            "Wenn genug Daten vorhanden sind oder der Nutzer Speichern/Aktualisieren verlangt, antworte mit type=workflow_update und einem vollstaendigen workflow Objekt. "
            "Nutze fuer ausfuehrbare Werkzeugschritte workflow.steps[].tool und workflow.steps[].paramsTemplate exakt nach effectiveTools[].inputSchema/apiPost. "
            "Batch-Ausfuehrungen modellierst du in workflow.executionBatches mit mode=sequential, stopOnFailure=true und steps[].tool/paramsTemplate. "
            "Spiegele dieselben ausfuehrbaren Schritte zusaetzlich in der flachen workflow.steps Liste, sobald du workflow_update erzeugst. "
            "Berechnungen speicherst du als workflow.derivedValues mit name, expression, dependsOn, unit und example; Qt fuehrt keine Formeln aus, daher muessen validationExamples konkrete Beispielwerte enthalten. "
            "Wenn ein Schritt unmittelbar zuvor mit geometry.create ein Rechteck erzeugt, verwende im naechsten rectangles.extrude selector={\"scope\":\"lastResult\",\"kind\":\"rectangle\"}, nicht scope=selection. "
            "Wenn unmittelbar zuvor extrudiert wurde, klassifiziere mit bim.classify target=lastExtruded. "
            "Nutze keine Felder toolName/arguments in constructionStrategy fuer Werkzeugaufrufe."},
        {"responseContract", QJsonObject{
            {"schema", "barebone.workflow.training.response.v1"},
            {"allowedTypes", QJsonArray{"ask_user", "slot_update", "workflow_draft", "workflow_update", "message"}},
            {"askUserShape", QJsonObject{
                {"schema", "barebone.workflow.training.response.v1"},
                {"type", "ask_user"},
                {"message", "Gezielte Rueckfrage ohne Platzhaltertext"},
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
                    {"constructionStrategy", QJsonArray{"fachliche Strategie und Berechnungsannahmen"}},
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
            {"slotPolicy", "requiredSlots ist eine Liste aus strings oder Objekten mit name/type/description. optionalSlots ist immer ein Objekt nach slotName, kein Array."},
            {"draftPolicy", "Bei langen Erstprompts erst workflow_draft mit workflowDraft erzeugen. workflow_update nur nach Nutzerbestaetigung oder wenn der Nutzer explizit speichern/aktualisieren will."},
            {"calculationPolicy", "Berechnungen gehoeren in derivedValues als name/expression/dependsOn/unit/example. Schreibe konkrete Beispielwerte in knownSlotValues und validationExamples; Qt wertet expression nicht aus."},
            {"batchPolicy", "Komplexe Ablaufe gehoeren in executionBatches[].steps mit mode=sequential und stopOnFailure=true. Fuer workflow_update muss zusaetzlich workflow.steps die gleiche ausfuehrbare Sequenz flach enthalten."},
            {"validationPolicy", "workflow_update muss validationExamples[].actions mit konkreten, platzhalterfreien Beispielaktionen enthalten. Verkettete Beispiele duerfen lastResult/lastExtruded nutzen, aber nicht eine leere selection voraussetzen."},
        }},
        {"knownToolNames", toolNames},
        {"effectiveTools", effectiveTools},
        {"existingWorkflows", existingWorkflows},
        {"trainingState", workflowTrainingState()},
        {"activeWorkflow", m_trainingWorkflowContext},
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

    const QJsonArray requiredSlots = workflow.value("requiredSlots").toArray();
    if (requiredSlots.isEmpty()) {
        errorMessage = QStringLiteral("workflow.requiredSlots braucht mindestens einen Slot");
        return false;
    }

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
    QString savedPath;
    QString errorMessage;
    if (!saveWorkflowFromTraining(workflow, &savedPath, &errorMessage)) {
        appendAgentChat("Barebone-Qt", QString("Workflow konnte nicht gespeichert werden: %1").arg(errorMessage));
        return;
    }

    m_trainingValidationRetries = 0;
    m_trainingWorkflowContext = workflow;
    m_trainingMissing = {};
    m_trainingPhase = QStringLiteral("saved");
    m_trainingSaveReviewPending = false;
    m_trainingReviewConfirmed = false;
    m_trainingPendingReviewContent.clear();
    m_trainingPendingReviewReply = {};
    m_trainingPendingReviewWorkflow = {};
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
    sendWorkflowTrainingPrompt(QString(
        "Korrigiere deine letzte Workflow-Training-Antwort intern. "
        "Der Nutzer sieht diese Validierung nicht. Fehler: %1\n"
        "Vorherige Antwort: %2\n"
        "Antworte ausschliesslich mit barebone.workflow.training.response.v1 JSON. "
        "Wenn Daten fehlen, nutze ask_user mit kurzer message, missing und questions, aber ohne workflow.steps und ohne validationExamples. "
        "Wenn erst ein strukturierter Entwurf sinnvoll ist, nutze workflow_draft mit workflowDraft. "
        "Wenn nur neue Angaben erkannt wurden, nutze slot_update mit slots. "
        "Wenn der Workflow speicherbar ist, nutze workflow_update mit steps[].tool, steps[].paramsTemplate und validationExamples[].actions.")
        .arg(errorMessage, rejectedJson),
        true);
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

    appendBridgeLog("Workflow Training Review: Vor dem Speichern wird der Entwurf punktweise geprueft");
    sendWorkflowTrainingPrompt(QStringLiteral(
        "Bevor dieser Workflow gespeichert wird, pruefe den aktiven Workflow noch einmal fachlich und technisch. "
        "Denke die Zusammenfassung punktweise durch und antworte ausschliesslich mit type=workflow_draft. "
        "Gehe einzeln auf diese Punkte ein: Titel, Beschreibung, Pflichtangaben, bekannte Beispielwerte, Berechnungen, Batch-Ausfuehrungen, Tool-Parameter, Validierungsbeispiel und Risiken. "
        "Formuliere danach konkrete questions, ob diese Punkte korrekt sind oder ob noch etwas beruecksichtigt werden muss. "
        "Nutze workflowDraft mit dem vollstaendigen aktuellen Workflow-Entwurf. "
        "Antworte nicht mit workflow_update, nicht speichern und keine BRX-Aktion vorschlagen, bis der Nutzer die Review ausdruecklich bestaetigt."),
        true);
}

void BricsCadPage::validateWorkflowWithBrxAndSave(
    const QString& rejectedContent,
    const QJsonObject& reply,
    const QJsonObject& workflow)
{
    bool runtimeSelectorNormalized = false;
    QJsonObject normalizedWorkflow = normalizedWorkflowRuntimeSelectors(workflow, &runtimeSelectorNormalized);
    if (runtimeSelectorNormalized) {
        appendBridgeLog("Workflow Training: Runtime-Selektoren auf lastResult/lastExtruded normalisiert");
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
            "Du erstellst und bearbeitest ausschliesslich agent/workflows JSON-Dateien. "
            "Antworte ausschliesslich mit einem JSON-Objekt gemaess barebone.workflow.training.response.v1. "
            "Beim ersten langen fachlichen Prompt leitest du automatisch id, Titel, Beschreibung, Eingaben, Berechnungen und Batch-Struktur ab und antwortest mit type=workflow_draft. "
            "Stelle Rueckfragen erst dann mit type=ask_user, wenn nach dem Draft noch konkrete Pflichtangaben fehlen. "
            "Wenn der Nutzer Bearbeiten, Erweitern oder Aktualisieren sagt, suche in existingWorkflows nach dem passenden Workflow und erhalte dessen bestehende Inhalte. "
            "Nutze trainingState.knownSlotValues als verbindlichen Speicher; frage diese Werte nicht erneut ab. "
            "Bei neuen Einzelangaben darfst du type=slot_update mit slots verwenden, statt einen kompletten Workflow zu schreiben. "
            "Bei type=ask_user darfst du keine vollstaendigen steps, validationExamples oder langen Workflow-Entwuerfe ausgeben; frage nur fehlende Daten ab. "
            "Bei type=workflow_draft darfst du workflowDraft mit description, derivedValues, executionBatches, constructionStrategy, authoringNotes und questions liefern; dieser Draft wird noch nicht gespeichert. "
            "Bei type=workflow_update muss workflow.steps flach ausfuehrbar sein, auch wenn executionBatches zusaetzlich gespeichert werden. "
            "Speichere Berechnungen als derivedValues mit expression und example; schreibe konkrete Beispielwerte in validationExamples. "
            "Speichere keine starren Prompt-zu-Command-Regeln, sondern Slots, Defaults, Strategien, Constraints, Beispiele und bevorzugte Tools. "
            "Falls compactContext=true ist, konzentriere dich auf eine kurze gueltige JSON-Antwort und vermeide lange Erklaerungen."},
    });
    for (const QJsonValue& value : m_trainingConversation) {
        messages.append(value.toObject());
    }
    messages.append(QJsonObject{
        {"role", "user"},
        {"content", QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact))},
    });
    const int requestedOutputTokens = dynamicOutputTokenTarget(
        compactContext ? 2048 : 4096,
        compactContext ? kWorkflowTrainingCompactOutputTokens : kWorkflowTrainingOutputTokens,
        compactContext ? 12 : 8);
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

    QNetworkReply* reply = m_aiNetwork->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, prompt, compactContext]() {
        setAgentBusy(false);

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendAgentChat("AI", QString("Fehler bei der Workflow-Training-Anfrage: provider=%1 http=%2 %3")
                .arg(m_config.aiProvider())
                .arg(httpStatus)
                .arg(reply->errorString()));
            if (!body.isEmpty()) {
                appendBridgeLog(QString("AI Workflow Body: %1").arg(QString::fromUtf8(body).left(800)));
            }
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
        if (m_trainingSaveReviewPending
            && m_trainingWorkflowContext.value("steps").isArray()
            && m_trainingWorkflowContext.value("validationExamples").isArray()) {
            m_trainingPendingReviewWorkflow = m_trainingWorkflowContext;
            m_trainingPendingReviewReply = reply;
            m_trainingPendingReviewContent = content;
        }
        appendAgentChat("AI", workflowDraftMessageForChat(reply, displayDraft, m_trainingMissing));
        return;
    }

    if (type == QStringLiteral("ask_user")) {
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
            && !m_trainingSlotValues.isEmpty()) {
            appendBridgeLog("Workflow Training: Rueckfrage uebersprungen, weil alle fehlenden Slots bereits im Qt-Trainingszustand vorhanden sind");
            m_trainingPhase = QStringLiteral("drafting");
            sendWorkflowTrainingPrompt(
                QStringLiteral("Die zuletzt angefragten fehlenden Angaben sind in trainingState.knownSlotValues vorhanden. Erstelle jetzt den naechsten sinnvollen Trainingsschritt: frage nur wirklich unbekannte Daten ab, aktualisiere den Entwurf mit workflow_draft oder antworte bei Speicherwunsch mit workflow_update."),
                true);
            return;
        }

        QStringList lines;
        if (!message.isEmpty()) {
            lines << message;
        }
        const QJsonArray questions = reply.value("questions").toArray();
        for (const QJsonValue& value : questions) {
            const QString question = repairMojibakeText(value.toString()).trimmed();
            if (!question.isEmpty() && workflowTrainingQuestionNeedsAnswer(question, m_trainingSlotValues)) {
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
        if (!m_trainingReviewConfirmed) {
            startWorkflowTrainingSaveReview(content, reply, workflow);
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
        const QJsonArray actions = agentProposalActions(proposal);
        if (actions.size() > 1) {
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
        {"policy", "Your previous response was not shown to the user. Correct it using the available tools and schemas."},
    });
    envelope.insert("instruction",
        "Korrigiere deine letzte Antwort. Antworte ausschliesslich mit einem gueltigen JSON-Objekt. "
        "Nutze Barebone-Agent-JSON v2 mit schema=\"barebone.agent.response.v2\". "
        "Nutze keinen freien Plan und keine Pseudo-Actions. Wenn die Aufgabe ausfuehrbar ist, liefere genau einen action_proposal fuer den naechsten sicheren Schritt. "
        "Direkte BricsCAD-DB-Schreibvorgaenge, AcDb-/LayerTable-/EntityTable-Mutationen und Pseudo-Tools fuer DB-Writes sind verboten; nutze ausschliesslich tools[].name. "
        "Wenn mehrere unabhaengige Aktionen mit bekannten Parametern erforderlich sind, liefere ein action_proposal mit proposal.actions:[{\"tool\":\"...\",\"params\":{...}},...] und proposal.continueAfterSuccess=false. "
        "Fuer mehrere Layer mit Namen/Farben nutze bevorzugt layers.ensureMany mit params.layers. "
        "Nutze continueAfterSuccess nicht, um Batch-Aufgaben wie mehrere Layer oder mehrere gleichartige Objekte einzeln nachzufordern. "
        "tool muss exakt einem tools[].name entsprechen. params muessen inputSchema/apiDoc.post erfuellen. "
        "Wenn validationError mit BRX Preflight beginnt, wiederhole nicht denselben Vorschlag; nutze die dort genannten Fehler, fehlenden Daten und Hinweise verbindlich. "
        "Wenn echte Informationen fehlen, nutze ask_user mit missing und einem draft. "
        "Wenn du Zeichnungskontext brauchst, nutze context_request mit exakt einer readOnlyMethods[].name Methode. "
        "Wenn die Anfrage allgemein ist, nutze type=message.");

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
        const QString name = repairMojibakeText(layer.value("name").toString()).trimmed();
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
    int explicitFailures = 0;
    QJsonArray createdLayerNames;
    QJsonArray skippedLayerNames;

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
    return stats;
}

QString agentCompletionSummary(const QJsonArray& actions, const QJsonArray& results, const QString& fallbackSummary)
{
    const QJsonObject stats = executionStatsForActions(actions, results);
    const int layerCreateRequested = stats.value("layerCreatesRequested").toInt(0);
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

    if (actions.size() > 1) {
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
    QJsonObject params = action.value("params").toObject();
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
                    setAgentBusy(false);
                    m_agentValidationRetries = 0;
                    appendAgentChat("Barebone-Qt", QString("Batch-Ausfuehrung nach Aktion %1/%2 gestoppt, damit BricsCAD nicht weiter belastet wird. Bitte pruefe, ob BricsCAD noch stabil laeuft, und sende die korrigierte Anweisung danach erneut.")
                        .arg(index + 1)
                        .arg(actions.size()));
                    return;
                }

                const QJsonObject result = response.value("result").toObject();
                m_lastAgentToolResult = result;
                m_pendingAgentDraft = {};
                m_agentValidationRetries = 0;

                appendBridgeLog(QString("BRX -> Qt: %1 ausgefuehrt result=%2")
                    .arg(bridgeMethod, QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(1000)));

                results.append(QJsonObject{
                    {"index", index + 1},
                    {"tool", tool},
                    {"bridgeMethod", bridgeMethod},
                    {"params", params},
                    {"response", response},
                    {"result", result},
                });

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
        [this, actions, index, executeCurrentAction](const QJsonObject& response) mutable {
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
            appendAgentChat("Barebone-Qt", QString("Batch-Ausfuehrung vor Aktion %1/%2 gestoppt: BRX Preflight hat die Aktion abgelehnt: %3")
                .arg(index + 1)
                .arg(actions.size())
                .arg(message));
            setAgentBusy(false);
            m_agentValidationRetries = 0;
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
        "Erstelle aus completedProposal, executedActions, executionStats, toolResults und batchResult eine kurze Abschlussnachricht im ChatGPT-Stil. "
        "Antworte ausschliesslich mit genau einem JSON-Objekt: {\"type\":\"message\",\"message\":\"...\"}. "
        "Schreibe natuerlich auf Deutsch, maximal zwei kurze Saetze. "
        "Nutze executionStats fuer konkrete Zahlen, z.B. wie viele Layer neu angelegt, uebersprungen oder fehlgeschlagen sind. "
        "Erwaehne keine internen Qt-/BRX-Details, keine JSON-Daten, keine Validierung und keine Denkprozesse. "
        "Behaupte nur, was in den Ergebnissen erfolgreich abgeschlossen wurde. "
        "Falls die Ergebnisse unklar sind, nutze fallbackSummary als Grundlage.");

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
        m_lastDocumentContext,
        m_lastAgentRoute);
    envelope.insert("type", "tool_error");
    envelope.insert("executionError", errorMessage);
    envelope.insert("failedProposal", proposal);
    envelope.insert("toolResponse", response);
    envelope.insert("context", currentAgentContext());
    envelope.insert("capabilities", m_brxCapabilities);
    envelope.insert("readOnlyMethods", readOnlyMethods);
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

void BricsCadPage::appendAgentChat(const QString& speaker, const QString& message)
{
    if (!m_agentBridge) {
        return;
    }

    const QString visibleMessage = speaker == QStringLiteral("AI")
        ? repairMojibakeText(removeReasoningLeak(message))
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

    const QJsonArray actions = agentProposalActions(proposal);
    const QString summary = proposal.value("summary").toString(
        proposal.value("message").toString()).trimmed();

    QStringList lines;
    if (actions.size() > 1) {
        lines << QString("Batch: %1 Aktionen").arg(actions.size());
        lines << QString("Bestaetigung: %1").arg(proposal.value("requiresConfirmation").toBool(true) ? "Ja" : "Nein");
        lines << QString("Ausfuehrung: intern als Batch, zu BRX einzeln mit Einzel-Preflight");
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
        Q_EMIT m_agentBridge->proposalChanged(QVariantMap{
            {"title", actions.size() > 1 ? "AI Batch-Vorschlag bereit" : "AI Vorschlag bereit"},
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
        const QString name = repairMojibakeText(layer.value("name").toString()).trimmed();
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

QJsonArray BricsCadPage::availableAgentToolsForRoute(const QJsonObject& route, const QString& prompt) const
{
    if (!routeAllowsCadActions(route)) {
        return {};
    }

    const QJsonArray allTools = availableAgentTools();
    if (allTools.isEmpty()) {
        return {};
    }

    const QString normalized = prompt.toLower();
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

    if (!layerIntent && !geometryIntent) {
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
                || name == QStringLiteral("command.execute")
                || name == QStringLiteral("document.save");
        }
        if (geometryIntent) {
            include = include
                || name.startsWith(QStringLiteral("geometry."))
                || name.startsWith(QStringLiteral("rectangles."))
                || name.startsWith(QStringLiteral("profile."))
                || name.startsWith(QStringLiteral("bim."))
                || name.startsWith(QStringLiteral("selection."))
                || name == QStringLiteral("command.execute")
                || name == QStringLiteral("document.save");
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

QJsonObject BricsCadPage::agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route) const
{
    const QJsonObject sanitizedContext = sanitizedDocumentContext(documentContext);
    const QJsonObject normalizedRoute = normalizedAgentRouteForMode(route, prompt, sanitizedContext, m_chatMode);
    const QJsonArray tools = availableAgentToolsForRoute(normalizedRoute, prompt);
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
    envelope.insert("capabilities", (cadContextAllowed && m_brxAuthenticated) ? m_brxCapabilities : QJsonObject{});
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
    envelope.insert("conversationMode", "unified-agent-envelope");
    envelope.insert("expectedResponse", "barebone-agent-json-v2-strict-object");
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
