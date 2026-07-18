#include "BrxAgent.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <initializer_list>

namespace {

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

void appendStringUnique(QJsonArray& array, const QString& value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) {
        return;
    }
    for (const QJsonValue& existing : array) {
        if (existing.toString() == text) {
            return;
        }
    }
    array.append(text);
}

void mergeStringArray(QJsonObject& target, const QString& key, const QJsonArray& values)
{
    QJsonArray merged = target.value(key).toArray();
    for (const QJsonValue& value : values) {
        appendStringUnique(merged, value.toString());
    }
    target.insert(key, merged);
}

bool textContainsAny(const QString& text, std::initializer_list<QString> needles)
{
    for (const QString& needle : needles) {
        if (!needle.isEmpty() && text.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool routeAllowsCadActions(const QJsonObject& route)
{
    const QString name = route.value(QStringLiteral("route")).toString();
    return name == QStringLiteral("bricscad_action")
        || name == QStringLiteral("validation_retry");
}

QStringList jsonStringArrayToList(const QJsonArray& values)
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

bool promptRequestsCadDataQuery(const QString& normalized)
{
    return textContainsAny(normalized, {
        QStringLiteral("liste"), QStringLiteral("tabelle"), QStringLiteral("zeige"),
        QStringLiteral("welche"), QStringLiteral("was ist"), QStringLiteral("abfrage"),
        QStringLiteral("query"), QStringLiteral("messen"), QStringLiteral("mess"),
        QStringLiteral("bbox"), QStringLiteral("bounding"), QStringLiteral("laenge"),
        QStringLiteral("länge"), QStringLiteral("flaeche"), QStringLiteral("fläche"),
        QStringLiteral("volumen"), QStringLiteral("handle"), QStringLiteral("eigenschaft"),
        QStringLiteral("properties"), QStringLiteral("property")});
}

bool promptTargetsBimObjects(const QString& normalized)
{
    return textContainsAny(normalized, {
        QStringLiteral("bim"), QStringLiteral("door"), QStringLiteral("window"),
        QStringLiteral("wall"), QStringLiteral("tuer"), QStringLiteral("tür"),
        QStringLiteral("fenster"), QStringLiteral("wand"), QStringLiteral("component type"),
        QStringLiteral("guid")});
}

bool promptRequestsEntityRename(const QString& normalized)
{
    return textContainsAny(normalized, {
        QStringLiteral("benenne"), QStringLiteral("umbenennen"), QStringLiteral("name"),
        QStringLiteral("setname"), QStringLiteral("entity.setname")})
        && !textContainsAny(normalized, {QStringLiteral("layer"), QStringLiteral("ebene")});
}

bool promptAllowsBimWallClassification(const QString& normalized)
{
    return textContainsAny(normalized, {
        QStringLiteral("klassifiz"), QStringLiteral("klassifizi"), QStringLiteral("bimwand"),
        QStringLiteral("bim wall"), QStringLiteral("als bim"), QStringLiteral("classification")});
}

void appendSdkAliases(QStringList& names, const QString& publicTool)
{
    auto append = [&names](const QString& name) {
        if (!name.isEmpty() && !names.contains(name)) {
            names << name;
        }
    };
    if (publicTool == QStringLiteral("geometry.move")) {
        append(QStringLiteral("brx.sdk.entity.transformBy"));
        append(QStringLiteral("brx.sdk.blockReference.setPosition"));
        append(QStringLiteral("brx.sdk.assoc.evaluate"));
    } else if (publicTool == QStringLiteral("geometry.copy")) {
        append(QStringLiteral("brx.sdk.entity.copy"));
    } else if (publicTool == QStringLiteral("geometry.rotate")) {
        append(QStringLiteral("brx.sdk.entity.rotateBy"));
    } else if (publicTool == QStringLiteral("geometry.scale")) {
        append(QStringLiteral("brx.sdk.entity.scaleBy"));
    } else if (publicTool == QStringLiteral("geometry.delete")) {
        append(QStringLiteral("brx.sdk.entity.erase"));
    } else if (publicTool == QStringLiteral("entity.setLayer")) {
        append(QStringLiteral("brx.sdk.entity.setLayer"));
    } else if (publicTool == QStringLiteral("entity.setName")) {
        append(QStringLiteral("brx.sdk.entity.setName"));
    } else if (publicTool == QStringLiteral("selection.set")) {
        append(QStringLiteral("brx.sdk.selection.setPickfirst"));
    } else if (publicTool == QStringLiteral("bim.classify")) {
        append(QStringLiteral("brx.sdk.bim.classification.set"));
    }
}

void appendSdkAliasesForSelectedPublicTools(QStringList& names)
{
    const QStringList current = names;
    for (const QString& name : current) {
        appendSdkAliases(names, name);
    }
}

void enrichRuntimeTool(QJsonObject& tool)
{
    const QString name = tool.value(QStringLiteral("name")).toString();
    const QJsonObject inputSchema = tool.value(QStringLiteral("inputSchema")).toObject();
    const QJsonObject properties = inputSchema.value(QStringLiteral("properties")).toObject();
    const QJsonObject apiDoc = tool.value(QStringLiteral("apiDoc")).toObject();

    tool.insert(QStringLiteral("source"), tool.value(QStringLiteral("virtual")).toBool(false)
        ? QStringLiteral("qt")
        : QStringLiteral("brx"));
    tool.insert(QStringLiteral("effectiveContractSource"), tool.value(QStringLiteral("virtual")).toBool(false)
        ? QStringLiteral("BrxAgent-virtual-tool")
        : QStringLiteral("BRX-runtime-capability"));

    QJsonObject capabilities = tool.value(QStringLiteral("capabilities")).toObject();
    capabilities.insert(QStringLiteral("requiresConfirmation"), tool.value(QStringLiteral("confirmationRequired")).toBool(true));
    capabilities.insert(QStringLiteral("risk"), tool.value(QStringLiteral("risk")).toString(QStringLiteral("modifiesDrawing")));
    capabilities.insert(QStringLiteral("bridgeMethod"), tool.value(QStringLiteral("bridgeMethod")).toString(name));
    capabilities.insert(QStringLiteral("supportsSelector"), properties.contains(QStringLiteral("selector")));
    capabilities.insert(QStringLiteral("supportsTarget"), properties.contains(QStringLiteral("target")));
    capabilities.insert(QStringLiteral("supportsHandles"), properties.contains(QStringLiteral("handles")));
    capabilities.insert(QStringLiteral("supportsSaveBefore"), properties.contains(QStringLiteral("saveBefore")));
    capabilities.insert(QStringLiteral("supportsReason"), properties.contains(QStringLiteral("reason")));
    capabilities.insert(QStringLiteral("hasExamples"), apiDoc.value(QStringLiteral("post")).toObject().value(QStringLiteral("examples")).isArray()
        || apiDoc.value(QStringLiteral("examples")).isArray());
    tool.insert(QStringLiteral("capabilities"), capabilities);

    if (!tool.contains(QStringLiteral("summary"))) {
        tool.insert(QStringLiteral("summary"), tool.value(QStringLiteral("description")).toString());
    }
    if (!tool.contains(QStringLiteral("domain"))) {
        tool.insert(QStringLiteral("domain"), tool.value(QStringLiteral("category")).toString(QStringLiteral("bridge")));
    }
    if (!tool.contains(QStringLiteral("examples"))) {
        const QJsonArray postExamples = apiDoc.value(QStringLiteral("post")).toObject().value(QStringLiteral("examples")).toArray();
        const QJsonArray rootExamples = apiDoc.value(QStringLiteral("examples")).toArray();
        if (!postExamples.isEmpty()) {
            tool.insert(QStringLiteral("examples"), postExamples);
        } else if (!rootExamples.isEmpty()) {
            tool.insert(QStringLiteral("examples"), rootExamples);
        }
    }

    mergeStringArray(tool, QStringLiteral("agentHints"), QJsonArray{
        QStringLiteral("Nutze dieses Tool nur, wenn die Nutzerabsicht exakt zur Toolbeschreibung und zum inputSchema passt."),
        QStringLiteral("Alle Pflichtfelder aus inputSchema/apiDoc.post.required muessen in params enthalten sein."),
        QStringLiteral("Qt validiert Toolname und Parameter lokal; BRX prueft den Vorschlag danach per actions.validate."),
    });
    mergeStringArray(tool, QStringLiteral("semanticConstraints"), QJsonArray{
        QStringLiteral("Erfinde keine zusaetzlichen Parameter ausserhalb des Schemas."),
        QStringLiteral("Mutierende BricsCAD-Datenbankzugriffe laufen nur ueber validierte BrxAgent/BRX-Tools."),
    });
    if (!tool.contains(QStringLiteral("unsupportedOperations"))) {
        tool.insert(QStringLiteral("unsupportedOperations"), QJsonArray{});
    }
}

QJsonObject layersEnsureManyTool()
{
    QJsonObject tool{
        {QStringLiteral("name"), QStringLiteral("layers.ensureMany")},
        {QStringLiteral("title"), QStringLiteral("Mehrere Layer sicher anlegen")},
        {QStringLiteral("description"), QStringLiteral("Qt-internes virtuelles Tool fuer kompakte Layer-Batches. Barebone-Qt expandiert layers[] vor Preflight und Ausfuehrung in einzelne layers.create-Aktionen.")},
        {QStringLiteral("summary"), QStringLiteral("Legt mehrere Layer kompakt an, wird vor der Ausfuehrung in layers.create[] expandiert.")},
        {QStringLiteral("bridgeMethod"), QStringLiteral("layers.create")},
        {QStringLiteral("kind"), QStringLiteral("action")},
        {QStringLiteral("risk"), QStringLiteral("modifiesDrawing")},
        {QStringLiteral("category"), QStringLiteral("layer")},
        {QStringLiteral("domain"), QStringLiteral("layer")},
        {QStringLiteral("confirmationRequired"), true},
        {QStringLiteral("virtual"), true},
        {QStringLiteral("expandsTo"), QStringLiteral("layers.create[]")},
        {QStringLiteral("agentHints"), QJsonArray{
            QStringLiteral("Preserve explicit layer names exactly, including a one-word name such as 'Test'."),
            QStringLiteral("Never rename 'Test' to 'Test Layer' or invent a two-word structure."),
            QStringLiteral("Any non-empty layer name without unsupported special characters is valid."),
        }},
        {QStringLiteral("inputSchema"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("required"), QJsonArray{QStringLiteral("layers")}},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("layers"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("items"), QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("object")},
                        {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}},
                        {QStringLiteral("properties"), QJsonObject{
                            {QStringLiteral("name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                            {QStringLiteral("colorIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 1}}},
                        }},
                    }},
                }},
                {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            }},
        }},
        {QStringLiteral("apiDoc"), QJsonObject{
            {QStringLiteral("method"), QStringLiteral("layers.ensureMany")},
            {QStringLiteral("virtual"), true},
            {QStringLiteral("required"), QJsonArray{QStringLiteral("layers")}},
            {QStringLiteral("bodySchema"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), QJsonObject{
                    {QStringLiteral("layers"), QStringLiteral("array of {name, optional colorIndex}; max batch count applies")},
                    {QStringLiteral("reason"), QStringLiteral("optional reason shown in proposal")},
                }},
            }},
            {QStringLiteral("examples"), QJsonArray{
                QJsonObject{{QStringLiteral("layers"), QJsonArray{
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("Heizung")}, {QStringLiteral("colorIndex"), 1}},
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("Sanitaer")}, {QStringLiteral("colorIndex"), 5}},
                }}},
            }},
        }},
    };
    enrichRuntimeTool(tool);
    return tool;
}

QJsonArray firstStrings(const QJsonArray& values, int limit, int maxChars)
{
    QJsonArray result;
    for (int i = 0; i < values.size() && result.size() < limit; ++i) {
        const QString text = values.at(i).toString().trimmed();
        if (!text.isEmpty()) {
            result.append(text.left(maxChars));
        }
    }
    return result;
}

QString toolSearchText(const QJsonObject& tool)
{
    QStringList parts{
        tool.value(QStringLiteral("name")).toString(),
        tool.value(QStringLiteral("title")).toString(),
        tool.value(QStringLiteral("domain")).toString(),
        tool.value(QStringLiteral("category")).toString(),
        tool.value(QStringLiteral("kind")).toString(),
        tool.value(QStringLiteral("risk")).toString(),
        tool.value(QStringLiteral("summary")).toString(),
        tool.value(QStringLiteral("description")).toString(),
        tool.value(QStringLiteral("policy")).toString(),
    };
    for (const QJsonValue& value : tool.value(QStringLiteral("keywords")).toArray()) {
        parts << value.toString();
    }
    for (const QJsonValue& value : tool.value(QStringLiteral("agentHints")).toArray()) {
        parts << value.toString();
    }
    return parts.join(QLatin1Char(' ')).toLower();
}

QStringList stringsFromArray(const QJsonArray& values)
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

QJsonObject pointSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("x"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("y"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("z"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        }},
    };
}

QJsonObject selectorSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("handles"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
            {QStringLiteral("names"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
            {QStringLiteral("layer"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };
}

QJsonObject sdkTool(
    const QString& name,
    const QString& title,
    const QString& summary,
    const QString& bridgeMethod,
    const QJsonObject& inputSchema,
    const QJsonArray& compatibleClasses,
    const QJsonArray& sdkFunctions,
    const QJsonArray& preconditions,
    const QJsonArray& readback)
{
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("title"), title},
        {QStringLiteral("domain"), QStringLiteral("brx-sdk")},
        {QStringLiteral("category"), QStringLiteral("brx.sdk")},
        {QStringLiteral("kind"), QStringLiteral("action")},
        {QStringLiteral("risk"), QStringLiteral("modifiesDrawing")},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("description"), summary},
        {QStringLiteral("bridgeMethod"), bridgeMethod},
        {QStringLiteral("confirmationRequired"), true},
        {QStringLiteral("sdkBacked"), true},
        {QStringLiteral("inputSchema"), inputSchema},
        {QStringLiteral("compatibleEntityClasses"), compatibleClasses},
        {QStringLiteral("compatibleGeometry"), compatibleClasses},
        {QStringLiteral("sdkFunctions"), sdkFunctions},
        {QStringLiteral("preconditions"), preconditions},
        {QStringLiteral("readback"), readback},
        {QStringLiteral("agentHints"), QJsonArray{
            QStringLiteral("Use qt.brx.db.inspect or qt.brx.db.compatibility before proposing this tool for unknown targets."),
            QStringLiteral("Never ask the user which SDK tool to use; inspect, compare candidates and propose the best validated action."),
            QStringLiteral("Every mutation is still confirmed by the user and validated by BRX actions.validate."),
        }},
    };
}

QJsonArray sdkTools()
{
    const QJsonObject transformSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("vector"), pointSchema()},
            {QStringLiteral("offset"), pointSchema()},
            {QStringLiteral("fromPoint"), pointSchema()},
            {QStringLiteral("toPoint"), pointSchema()},
            {QStringLiteral("units"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("mm"), QStringLiteral("drawing")}}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
        {QStringLiteral("oneOfRequired"), QJsonArray{QJsonArray{QStringLiteral("vector")}, QJsonArray{QStringLiteral("offset")}, QJsonArray{QStringLiteral("fromPoint"), QStringLiteral("toPoint")}}},
    };

    const QJsonObject copySchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("vector"), pointSchema()},
            {QStringLiteral("offset"), pointSchema()},
            {QStringLiteral("spacing"), pointSchema()},
            {QStringLiteral("count"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("units"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("mm"), QStringLiteral("drawing")}}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
        {QStringLiteral("oneOfRequired"), QJsonArray{QJsonArray{QStringLiteral("vector")}, QJsonArray{QStringLiteral("offset")}, QJsonArray{QStringLiteral("spacing")}}},
    };

    const QJsonObject rotateSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("basePoint"), pointSchema()},
            {QStringLiteral("basePointMode"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("axis"), pointSchema()},
            {QStringLiteral("angleDeg"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("angleRad"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
        {QStringLiteral("oneOfRequired"), QJsonArray{QJsonArray{QStringLiteral("angleDeg")}, QJsonArray{QStringLiteral("angleRad")}}},
    };

    const QJsonObject scaleSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector"), QStringLiteral("basePoint"), QStringLiteral("factor")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("basePoint"), pointSchema()},
            {QStringLiteral("factor"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject deleteSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector"), QStringLiteral("confirm")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("confirm"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject setLayerSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector"), QStringLiteral("layer")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("layer"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("createIfMissing"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject setNameSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector"), QStringLiteral("name")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject selectionSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("selector")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject bimClassifySchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("classification")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("classification"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    const QJsonObject assocSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("selector"), selectorSchema()},
            {QStringLiteral("saveBefore"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        }},
    };

    return QJsonArray{
        sdkTool(
            QStringLiteral("brx.sdk.entity.transformBy"),
            QStringLiteral("BRX SDK Entity transformBy"),
            QStringLiteral("Transforms complete database entities through AcDbEntity::transformBy-compatible semantics. Use for whole-entity translation when compatibility permits."),
            QStringLiteral("geometry.move"),
            transformSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::transformBy"), QStringLiteral("AcGeMatrix3d::translation")},
            QJsonArray{QStringLiteral("target handle resolves uniquely"), QStringLiteral("target is not from XRef"), QStringLiteral("target layer is not locked")},
            QJsonArray{QStringLiteral("reference point translated by vector"), QStringLiteral("handle/layer/class preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.blockReference.setPosition"),
            QStringLiteral("BRX SDK BlockReference setPosition"),
            QStringLiteral("Moves AcDbBlockReference targets by changing their insertion position. Candidate for ordinary blocks and BIM Door/Window block references after inspection."),
            QStringLiteral("geometry.move"),
            transformSchema,
            QJsonArray{QStringLiteral("AcDbBlockReference"), QStringLiteral("BIM_DOOR"), QStringLiteral("BIM_WINDOW")},
            QJsonArray{QStringLiteral("AcDbBlockReference::setPosition"), QStringLiteral("AcDbBlockReference::position")},
            QJsonArray{QStringLiteral("target is AcDbBlockReference"), QStringLiteral("target handle resolves uniquely"), QStringLiteral("target is writable")},
            QJsonArray{QStringLiteral("block insertion point translated by vector"), QStringLiteral("block transform basis preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.copy"),
            QStringLiteral("BRX SDK Entity clone/copy"),
            QStringLiteral("Copies writable database entities with AcDbEntity clone/append semantics. Use when the target class is copyable and a concrete offset/vector is known."),
            QStringLiteral("geometry.copy"),
            copySchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::clone"), QStringLiteral("AcDbBlockTableRecord::appendAcDbEntity"), QStringLiteral("AcGeMatrix3d::translation")},
            QJsonArray{QStringLiteral("selector resolves at least one writable entity"), QStringLiteral("target layer is not locked"), QStringLiteral("copy vector/offset/spacing is concrete")},
            QJsonArray{QStringLiteral("new handles are returned"), QStringLiteral("source handles remain unchanged")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.rotateBy"),
            QStringLiteral("BRX SDK Entity rotate"),
            QStringLiteral("Rotates complete database entities through AcDbEntity::transformBy-compatible matrix semantics around a concrete base point/axis."),
            QStringLiteral("geometry.rotate"),
            rotateSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::transformBy"), QStringLiteral("AcGeMatrix3d::rotation")},
            QJsonArray{QStringLiteral("target handle resolves uniquely or selector is concrete"), QStringLiteral("rotation angle is explicitly provided"), QStringLiteral("target layer is not locked")},
            QJsonArray{QStringLiteral("entity extents/orientation changed by angle"), QStringLiteral("handle/layer/class preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.scaleBy"),
            QStringLiteral("BRX SDK Entity scale"),
            QStringLiteral("Uniformly scales complete database entities through AcDbEntity::transformBy-compatible matrix semantics around a concrete base point."),
            QStringLiteral("geometry.scale"),
            scaleSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::transformBy"), QStringLiteral("AcGeMatrix3d::scaling")},
            QJsonArray{QStringLiteral("uniform factor > 0"), QStringLiteral("base point is concrete"), QStringLiteral("target layer is not locked")},
            QJsonArray{QStringLiteral("bounds scale uniformly"), QStringLiteral("handle/layer/class preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.erase"),
            QStringLiteral("BRX SDK Entity erase"),
            QStringLiteral("Erases writable database entities through AcDbEntity::erase after explicit confirmation and selector validation."),
            QStringLiteral("geometry.delete"),
            deleteSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::erase")},
            QJsonArray{QStringLiteral("confirm=true"), QStringLiteral("selector resolves writable entities"), QStringLiteral("targets are not XRef or locked-layer objects")},
            QJsonArray{QStringLiteral("affected handles reported erased"), QStringLiteral("no unrelated handles changed")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.setLayer"),
            QStringLiteral("BRX SDK Entity setLayer"),
            QStringLiteral("Assigns writable entities to a target layer through AcDbEntity layer APIs. Can create the layer first when createIfMissing is true."),
            QStringLiteral("entity.setLayer"),
            setLayerSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbEntity::setLayer"), QStringLiteral("AcDbLayerTableRecord")},
            QJsonArray{QStringLiteral("target layer exists or createIfMissing=true"), QStringLiteral("source entity is writable"), QStringLiteral("source layer is not locked")},
            QJsonArray{QStringLiteral("entity layer equals requested layer"), QStringLiteral("handle/class preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.entity.setName"),
            QStringLiteral("BRX SDK Entity name metadata"),
            QStringLiteral("Stores Barebone entity-name metadata on concrete entities via registered XData. Use for entity/BIM naming, not layer renaming."),
            QStringLiteral("entity.setName"),
            setNameSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("AcDb3dSolid"), QStringLiteral("AcDbPolyline"), QStringLiteral("AcDbCircle"), QStringLiteral("AcDbBlockReference")},
            QJsonArray{QStringLiteral("AcDbRegAppTable"), QStringLiteral("AcDbEntity::xData"), QStringLiteral("resbuf XData write")},
            QJsonArray{QStringLiteral("selector resolves writable entities"), QStringLiteral("name is non-empty"), QStringLiteral("target is not XRef")},
            QJsonArray{QStringLiteral("name readback matches requested name"), QStringLiteral("handle/class preserved")}),
        sdkTool(
            QStringLiteral("brx.sdk.selection.setPickfirst"),
            QStringLiteral("BRX SDK Pickfirst selection set"),
            QStringLiteral("Sets the editor pickfirst selection from concrete handles/selector facts. Use before operations that intentionally work on current selection."),
            QStringLiteral("selection.set"),
            selectionSchema,
            QJsonArray{QStringLiteral("AcDbEntity"), QStringLiteral("SelectionSet"), QStringLiteral("PickfirstSelection")},
            QJsonArray{QStringLiteral("acedSSSetFirst"), QStringLiteral("acedSSGet"), QStringLiteral("AcDbObjectId selection resolution")},
            QJsonArray{QStringLiteral("selector resolves at least one selectable entity"), QStringLiteral("selection-only changes do not create drawings")},
            QJsonArray{QStringLiteral("selection.describe returns requested handles")}),
        sdkTool(
            QStringLiteral("brx.sdk.bim.classification.set"),
            QStringLiteral("BRX SDK BIM classify"),
            QStringLiteral("Classifies selected or recently created 3D solids through the BIM SDK bridge. Use only when BIM classification is the user's explicit intent or required by a workflow."),
            QStringLiteral("bim.classify"),
            bimClassifySchema,
            QJsonArray{QStringLiteral("AcDb3dSolid"), QStringLiteral("BIMWall")},
            QJsonArray{QStringLiteral("BrxBimSdk classification bridge"), QStringLiteral("BIM object fingerprint readback")},
            QJsonArray{QStringLiteral("classification is supported"), QStringLiteral("target resolves to 3D solids"), QStringLiteral("BIM SDK is available")},
            QJsonArray{QStringLiteral("BIM class readback matches requested classification"), QStringLiteral("classified handles returned")}),
        sdkTool(
            QStringLiteral("brx.sdk.assoc.evaluate"),
            QStringLiteral("BRX SDK Assoc network evaluate"),
            QStringLiteral("Evaluates the top-level associative network after a mutation or as a repair/refresh step when BricsCAD relationships need recalculation."),
            QStringLiteral("brx.sdk.assoc.evaluate"),
            assocSchema,
            QJsonArray{QStringLiteral("AcDbDatabase"), QStringLiteral("AssociativeNetwork")},
            QJsonArray{QStringLiteral("AcDbAssocManager::evaluateTopLevelNetwork")},
            QJsonArray{QStringLiteral("active document database is available")},
            QJsonArray{QStringLiteral("evaluation status returned"), QStringLiteral("display update requested")}),
    };
}

bool sameToolName(const QJsonObject& lhs, const QJsonObject& rhs)
{
    return lhs.value(QStringLiteral("name")).toString() == rhs.value(QStringLiteral("name")).toString();
}

QJsonArray mergeTools(QJsonArray base, const QJsonArray& extra)
{
    for (const QJsonValue& value : extra) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            continue;
        }
        bool exists = false;
        for (const QJsonValue& existing : base) {
            if (existing.toObject().value(QStringLiteral("name")).toString() == name) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            base.append(tool);
        }
    }
    return base;
}

bool toolCatalogContains(const QJsonArray& tools, const QString& name)
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        return false;
    }
    for (const QJsonValue& value : tools) {
        if (value.toObject().value(QStringLiteral("name")).toString() == wanted) {
            return true;
        }
    }
    return false;
}

void appendToolNameIfAvailable(QStringList& names, const QJsonArray& tools, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (!trimmed.isEmpty()
        && !names.contains(trimmed)
        && toolCatalogContains(tools, trimmed)) {
        names << trimmed;
    }
}

QJsonObject toolCardForDescribe(QJsonObject tool, bool includeSchemas)
{
    if (includeSchemas) {
        return tool;
    }

    QJsonArray parameterNames;
    const QJsonObject properties = tool.value(QStringLiteral("inputSchema")).toObject()
        .value(QStringLiteral("properties")).toObject();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        parameterNames.append(it.key());
    }
    tool.insert(QStringLiteral("schemaOmitted"), true);
    tool.insert(QStringLiteral("parameters"), parameterNames);
    tool.remove(QStringLiteral("inputSchema"));
    tool.remove(QStringLiteral("resultSchema"));
    tool.remove(QStringLiteral("apiDoc"));
    return tool;
}

QJsonArray compactIndexForToolNames(const QJsonArray& tools, const QStringList& names)
{
    return BrxAgent::compactToolIndex(BrxAgent::toolsByNames(tools, names));
}

QJsonArray stringArray(std::initializer_list<QString> values)
{
    QJsonArray result;
    for (const QString& value : values) {
        result.append(value);
    }
    return result;
}

} // namespace

QJsonArray BrxAgent::buildToolCatalog(const QJsonObject& capabilities)
{
    QJsonArray tools;
    const QJsonArray methods = capabilities.value(QStringLiteral("methods")).toArray();
    for (const QJsonValue& value : methods) {
        const QJsonObject method = value.toObject();
        const QString name = method.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || !bridgeCapabilityMethodAvailable(method)) {
            continue;
        }

        const QString risk = method.value(QStringLiteral("risk")).toString(QStringLiteral("modifiesDrawing"));
        QJsonObject tool{
            {QStringLiteral("name"), name},
            {QStringLiteral("title"), method.value(QStringLiteral("title")).toString(name)},
            {QStringLiteral("description"), method.value(QStringLiteral("description")).toString()},
            {QStringLiteral("bridgeMethod"), name},
            {QStringLiteral("kind"), method.value(QStringLiteral("kind")).toString(risk == QStringLiteral("readOnly") ? QStringLiteral("query") : QStringLiteral("action"))},
            {QStringLiteral("risk"), risk},
            {QStringLiteral("category"), method.value(QStringLiteral("category")).toString(QStringLiteral("bridge"))},
            {QStringLiteral("resultSchema"), method.value(QStringLiteral("resultSchema")).toString()},
            {QStringLiteral("confirmationRequired"), risk != QStringLiteral("readOnly")},
            {QStringLiteral("inputSchema"), method.value(QStringLiteral("paramsSchema")).toObject(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), QJsonObject{}},
            })},
        };
        if (method.contains(QStringLiteral("apiDoc"))) {
            tool.insert(QStringLiteral("apiDoc"), method.value(QStringLiteral("apiDoc")).toObject());
        }
        enrichRuntimeTool(tool);
        tools.append(tool);
    }

    tools.append(layersEnsureManyTool());
    return runtimeToolsWithSdkTools(tools);
}

QJsonArray BrxAgent::runtimeToolsWithSdkTools(const QJsonArray& runtimeTools)
{
    return mergeTools(runtimeTools, sdkTools());
}

QJsonArray BrxAgent::selectToolsForRoute(
    const QJsonArray& catalog,
    const QJsonObject& route,
    const QString& prompt,
    const QJsonObject& workflowContext)
{
    if (!routeAllowsCadActions(route)) {
        return {};
    }

    const QJsonArray allTools = runtimeToolsWithSdkTools(catalog);
    if (allTools.isEmpty()) {
        return {};
    }

    const QString normalized = prompt.toLower();
    const bool chatWorkspace = workflowContext.value(QStringLiteral("chatWorkspace")).toBool(false);
    const bool bimObjectIntent = promptTargetsBimObjects(normalized);
    const bool circleExtrudeIntentHint = textContainsAny(normalized, {
            QStringLiteral("kreis"), QStringLiteral("circle"),
            QStringLiteral("zylinder"), QStringLiteral("cylinder")})
        && textContainsAny(normalized, {
            QStringLiteral("extrudi"), QStringLiteral("hoehe"), QStringLiteral("hÃ¶he"),
            QStringLiteral("3d"), QStringLiteral("solid"), QStringLiteral("z=")});
    const bool bimSelectionIntent = bimObjectIntent && textContainsAny(normalized, {
        QStringLiteral("auswahl"), QStringLiteral("selekt"), QStringLiteral("waehle"),
        QStringLiteral("wähle"), QStringLiteral("select")});

    QStringList selectedToolNames = jsonStringArrayToList(route.value(QStringLiteral("selectedTools")).toArray());
    if (!selectedToolNames.isEmpty()) {
        if (circleExtrudeIntentHint) {
            selectedToolNames.removeAll(QStringLiteral("rectangles.extrude"));
            if (!selectedToolNames.contains(QStringLiteral("circle.extrude"))) {
                selectedToolNames.prepend(QStringLiteral("circle.extrude"));
            }
            if (!selectedToolNames.contains(QStringLiteral("profile.extrude"))) {
                selectedToolNames << QStringLiteral("profile.extrude");
            }
        }
        if ((selectedToolNames.contains(QStringLiteral("rectangles.extrude"))
                || selectedToolNames.contains(QStringLiteral("circle.extrude"))
                || selectedToolNames.contains(QStringLiteral("profile.extrude")))
            && !selectedToolNames.contains(QStringLiteral("entity.setLayer"))) {
            selectedToolNames << QStringLiteral("entity.setLayer");
        }
        if (!chatWorkspace && promptRequestsCadDataQuery(normalized)) {
            for (const QString& toolName : {
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
        if (bimObjectIntent) {
            for (const QString& toolName : {
                     QStringLiteral("bim.objects.query"),
                     bimSelectionIntent ? QStringLiteral("bim.selection.set") : QString()}) {
                if (!toolName.isEmpty() && !selectedToolNames.contains(toolName)) {
                    selectedToolNames << toolName;
                }
            }
        }
        appendSdkAliasesForSelectedPublicTools(selectedToolNames);
        const QJsonArray selectedToolDetails = toolsByNames(allTools, selectedToolNames);
        if (!selectedToolDetails.isEmpty()) {
            return selectedToolDetails;
        }
    }

    if (chatWorkspace) {
        return {};
    }

    QStringList workflowTools = jsonStringArrayToList(workflowContext.value(QStringLiteral("workflowTools")).toArray());
    for (const QJsonValue& value : workflowContext.value(QStringLiteral("recommendedTools")).toArray()) {
        const QString tool = value.toString().trimmed();
        if (!tool.isEmpty() && !workflowTools.contains(tool)) {
            workflowTools << tool;
        }
    }
    appendSdkAliasesForSelectedPublicTools(workflowTools);

    const bool layerIntent = textContainsAny(normalized, {
        QStringLiteral("layer"), QStringLiteral("ebene"), QStringLiteral("tga"),
        QStringLiteral("heizung"), QStringLiteral("sanit"), QStringLiteral("lueft"),
        QStringLiteral("lüft"), QStringLiteral("elektro")});
    const bool handleReferenceIntent = QRegularExpression(
        QStringLiteral(R"(\b[a-f][0-9a-f]{1,7}\b)"),
        QRegularExpression::CaseInsensitiveOption).match(normalized).hasMatch();
    const bool dataQueryIntent = promptRequestsCadDataQuery(normalized);
    const bool geometryIntent = handleReferenceIntent || dataQueryIntent || bimObjectIntent || textContainsAny(normalized, {
        QStringLiteral("geometrie"), QStringLiteral("kreis"), QStringLiteral("rechteck"),
        QStringLiteral("linie"), QStringLiteral("polyline"), QStringLiteral("wand"),
        QStringLiteral("objekt"), QStringLiteral("object"), QStringLiteral("entity"),
        QStringLiteral("handle"), QStringLiteral("solid"), QStringLiteral("bim"),
        QStringLiteral("klassifiz"), QStringLiteral("klassifizi"), QStringLiteral("box"),
        QStringLiteral("extrusion"), QStringLiteral("extrudi"), QStringLiteral("verschiebe"),
        QStringLiteral("kopiere"), QStringLiteral("rotiere"), QStringLiteral("skaliere"),
        QStringLiteral("weise"), QStringLiteral("zuweis"), QStringLiteral("zuordn"),
        QStringLiteral("setze"), QStringLiteral("loesche"), QStringLiteral("lösche"),
        QStringLiteral("auswahl"), QStringLiteral("selekt"), QStringLiteral("waehle"),
        QStringLiteral("wähle"), QStringLiteral("select")});
    const bool entityRenameIntent = promptRequestsEntityRename(normalized);
    const bool createIntent = geometryIntent && textContainsAny(normalized, {
        QStringLiteral("zeichne"), QStringLiteral("zeichnen"), QStringLiteral("erstelle"),
        QStringLiteral("erzeuge"), QStringLiteral("anlegen"), QStringLiteral("neu"),
        QStringLiteral("wand"), QStringLiteral("waende"), QStringLiteral("wände"),
        QStringLiteral("rechteck"), QStringLiteral("linie"), QStringLiteral("kreis"),
        QStringLiteral("box"), QStringLiteral("raum"), QStringLiteral("grundriss")});
    const bool extrudeIntent = geometryIntent && textContainsAny(normalized, {
        QStringLiteral("extrudi"), QStringLiteral("hoehe"), QStringLiteral("höhe"),
        QStringLiteral("3d"), QStringLiteral("solid"), QStringLiteral("wandhoehe"),
        QStringLiteral("wandhöhe")});
    const bool circleExtrudeIntent = extrudeIntent && textContainsAny(normalized, {
        QStringLiteral("kreis"), QStringLiteral("circle"),
        QStringLiteral("zylinder"), QStringLiteral("cylinder")});
    const bool rectangleExtrudeIntent = extrudeIntent && textContainsAny(normalized, {
        QStringLiteral("rechteck"), QStringLiteral("rectangle"),
        QStringLiteral("wand"), QStringLiteral("waende"), QStringLiteral("wÃ¤nde")});
    const bool classifyIntent = geometryIntent && promptAllowsBimWallClassification(normalized);
    const bool selectionIntent = geometryIntent && textContainsAny(normalized, {
        QStringLiteral("auswahl"), QStringLiteral("selekt"), QStringLiteral("waehle"),
        QStringLiteral("wähle"), QStringLiteral("select"), QStringLiteral("alle"),
        QStringLiteral("layer")});
    const bool entityLayerIntent = layerIntent && geometryIntent && textContainsAny(normalized, {
        QStringLiteral("weise"), QStringLiteral("zuweis"), QStringLiteral("zuordn"),
        QStringLiteral("setze"), QStringLiteral("assign"), QStringLiteral("auf layer"),
        QStringLiteral("in layer"), QStringLiteral("layer zu")});
    const bool moveIntent = geometryIntent && textContainsAny(normalized, {QStringLiteral("verschieb"), QStringLiteral("move")});
    const bool copyIntent = geometryIntent && textContainsAny(normalized, {QStringLiteral("kopiere"), QStringLiteral("copy")});
    const bool rotateIntent = geometryIntent && textContainsAny(normalized, {QStringLiteral("rotier"), QStringLiteral("dreh"), QStringLiteral("rotate")});
    const bool scaleIntent = geometryIntent && textContainsAny(normalized, {QStringLiteral("skaliere"), QStringLiteral("scale")});
    const bool deleteIntent = geometryIntent && textContainsAny(normalized, {QStringLiteral("loesche"), QStringLiteral("lösche"), QStringLiteral("delete")});
    const bool saveIntent = textContainsAny(normalized, {QStringLiteral("speicher"), QStringLiteral("save")});

    if (!layerIntent && !geometryIntent && workflowTools.isEmpty()) {
        return {};
    }

    const bool needsReadOnlyContext = dataQueryIntent || handleReferenceIntent || selectionIntent || bimObjectIntent;
    QJsonArray filtered;
    for (const QJsonValue& value : allTools) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        const bool readOnlyTool = tool.value(QStringLiteral("risk")).toString() == QStringLiteral("readOnly")
            || tool.value(QStringLiteral("kind")).toString() == QStringLiteral("query");
        bool include = false;
        if (layerIntent && !entityRenameIntent) {
            include = name.startsWith(QStringLiteral("layers."))
                || name == QStringLiteral("layers.ensureMany")
                || name == QStringLiteral("layers.list")
                || name == QStringLiteral("command.execute");
        }
        if (geometryIntent) {
            include = include
                || (needsReadOnlyContext && readOnlyTool)
                || (createIntent && name == QStringLiteral("geometry.create"))
                || (moveIntent && (name == QStringLiteral("geometry.move")
                    || name == QStringLiteral("brx.sdk.entity.transformBy")
                    || name == QStringLiteral("brx.sdk.blockReference.setPosition")
                    || name == QStringLiteral("brx.sdk.assoc.evaluate")))
                || (copyIntent && (name == QStringLiteral("geometry.copy") || name == QStringLiteral("brx.sdk.entity.copy")))
                || (rotateIntent && (name == QStringLiteral("geometry.rotate") || name == QStringLiteral("brx.sdk.entity.rotateBy")))
                || (scaleIntent && (name == QStringLiteral("geometry.scale") || name == QStringLiteral("brx.sdk.entity.scaleBy")))
                || (deleteIntent && (name == QStringLiteral("geometry.delete") || name == QStringLiteral("brx.sdk.entity.erase")))
                || (circleExtrudeIntent && (name == QStringLiteral("circle.extrude") || name == QStringLiteral("profile.extrude")))
                || (extrudeIntent && !circleExtrudeIntent && rectangleExtrudeIntent && name == QStringLiteral("rectangles.extrude"))
                || (extrudeIntent && !circleExtrudeIntent && name == QStringLiteral("profile.extrude"))
                || (classifyIntent && (name == QStringLiteral("bim.classify") || name == QStringLiteral("brx.sdk.bim.classification.set")))
                || (bimObjectIntent && name == QStringLiteral("bim.objects.query"))
                || (bimObjectIntent && selectionIntent && name == QStringLiteral("bim.selection.set"))
                || (entityLayerIntent && (name == QStringLiteral("entity.setLayer") || name == QStringLiteral("brx.sdk.entity.setLayer")))
                || (entityRenameIntent && (name == QStringLiteral("entity.setName") || name == QStringLiteral("brx.sdk.entity.setName")))
                || (selectionIntent && (name.startsWith(QStringLiteral("selection.")) || name == QStringLiteral("brx.sdk.selection.setPickfirst")))
                || (!entityRenameIntent && name == QStringLiteral("command.execute"));
        }
        if (saveIntent && name == QStringLiteral("document.save")) {
            include = true;
        }
        if (workflowTools.contains(name)) {
            include = true;
        }
        if (include) {
            filtered.append(tool);
        }
    }
    return filtered;
}

QJsonArray BrxAgent::compactToolIndex(const QJsonArray& tools)
{
    QJsonArray compactTools;
    for (const QJsonValue& value : runtimeToolsWithSdkTools(tools)) {
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
            {QStringLiteral("name"), name},
            {QStringLiteral("title"), tool.value(QStringLiteral("title")).toString()},
            {QStringLiteral("domain"), tool.value(QStringLiteral("domain")).toString(tool.value(QStringLiteral("category")).toString())},
            {QStringLiteral("kind"), tool.value(QStringLiteral("kind")).toString()},
            {QStringLiteral("risk"), tool.value(QStringLiteral("risk")).toString()},
            {QStringLiteral("summary"), tool.value(QStringLiteral("summary")).toString(
                tool.value(QStringLiteral("policy")).toString(tool.value(QStringLiteral("description")).toString())).left(220)},
            {QStringLiteral("keywords"), firstStrings(tool.value(QStringLiteral("keywords")).toArray(), 10, 60)},
            {QStringLiteral("parameters"), parameterNames},
        };

        const QJsonArray hints = tool.value(QStringLiteral("agentHints")).toArray();
        if (!hints.isEmpty()) {
            compact.insert(QStringLiteral("hints"), firstStrings(hints, 2, 120));
        }
        QJsonValue compatibleGeometry = tool.value(QStringLiteral("inputSchema")).toObject()
            .value(QStringLiteral("compatibleGeometry"));
        if (compatibleGeometry.isUndefined()) {
            compatibleGeometry = tool.value(QStringLiteral("compatibleGeometry"));
        }
        if (!compatibleGeometry.isUndefined()) {
            compact.insert(QStringLiteral("compatibleGeometry"), compatibleGeometry);
        }
        compactTools.append(compact);
    }
    return compactTools;
}

QJsonArray BrxAgent::toolsByNames(const QJsonArray& tools, const QStringList& names)
{
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
    for (const QJsonValue& value : runtimeToolsWithSdkTools(tools)) {
        const QJsonObject tool = value.toObject();
        if (wanted.contains(tool.value(QStringLiteral("name")).toString())) {
            selected.append(tool);
        }
    }
    return selected;
}

QStringList BrxAgent::toolNames(const QJsonArray& tools)
{
    QStringList names;
    for (const QJsonValue& value : runtimeToolsWithSdkTools(tools)) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty() && !names.contains(name)) {
            names << name;
        }
    }
    return names;
}

QJsonObject BrxAgent::describeTools(const QJsonArray& tools, const QJsonObject& params)
{
    QStringList requestedNames = stringsFromArray(params.value(QStringLiteral("names")).toArray());
    const QString singleName = params.value(QStringLiteral("name")).toString().trimmed();
    if (!singleName.isEmpty() && !requestedNames.contains(singleName)) {
        requestedNames << singleName;
    }
    const QString query = params.value(QStringLiteral("query")).toString().trimmed().toLower();
    const bool describeAll = params.value(QStringLiteral("all")).toBool(false);
    const bool includeSchemas = !params.contains(QStringLiteral("includeSchemas"))
        || params.value(QStringLiteral("includeSchemas")).toBool(true);
    bool cursorOk = false;
    int offset = params.value(QStringLiteral("cursor")).toString().trimmed().toInt(&cursorOk);
    if (!cursorOk) {
        offset = params.value(QStringLiteral("offset")).toInt(0);
    }
    offset = std::max(0, offset);
    const int defaultLimit = describeAll ? 32 : 8;
    const int maxLimit = describeAll ? 64 : 16;
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(defaultLimit), 1, maxLimit);

    QJsonArray matches;
    for (const QJsonValue& value : runtimeToolsWithSdkTools(tools)) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }

        const bool nameMatch = requestedNames.contains(name);
        const bool queryMatch = requestedNames.isEmpty()
            && !query.isEmpty()
            && toolSearchText(tool).contains(query);
        if (!describeAll && !nameMatch && !queryMatch) {
            continue;
        }

        matches.append(tool);
    }

    QJsonArray detailed;
    QJsonArray matchedIndex;
    for (int i = offset; i < matches.size() && detailed.size() < limit; ++i) {
        const QJsonObject tool = matches.at(i).toObject();
        detailed.append(toolCardForDescribe(tool, includeSchemas));
        QJsonArray single;
        single.append(tool);
        const QJsonArray compact = compactToolIndex(single);
        if (!compact.isEmpty()) {
            matchedIndex.append(compact.at(0));
        }
    }
    const int nextOffset = offset + detailed.size() < matches.size()
        ? offset + detailed.size()
        : -1;

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.tools.describe.result.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("requestedNames"), QJsonArray::fromStringList(requestedNames)},
        {QStringLiteral("query"), query},
        {QStringLiteral("all"), describeAll},
        {QStringLiteral("includeSchemas"), includeSchemas},
        {QStringLiteral("count"), detailed.size()},
        {QStringLiteral("totalMatches"), matches.size()},
        {QStringLiteral("page"), QJsonObject{
            {QStringLiteral("offset"), offset},
            {QStringLiteral("limit"), limit},
            {QStringLiteral("nextOffset"), nextOffset},
            {QStringLiteral("nextCursor"), nextOffset >= 0 ? QString::number(nextOffset) : QString()},
            {QStringLiteral("complete"), nextOffset < 0},
        }},
        {QStringLiteral("tools"), detailed},
        {QStringLiteral("index"), matchedIndex},
        {QStringLiteral("policy"), QStringLiteral(
            "Diese Tooldetails kommen aus dem zentralen BrxAgent. Die lokale AI darf Toolkandidaten selbst waehlen, muss mutierende Aktionen aber als action_proposal durch Qt/BRX validieren und bestaetigen lassen.")},
    };
}

QJsonObject BrxAgent::repairToolContext(const QJsonArray& tools, const QJsonObject& params)
{
    const QJsonArray allTools = runtimeToolsWithSdkTools(tools);
    const QString failedTool = params.value(QStringLiteral("failedTool")).toString().trimmed();
    const QJsonObject failedParams = params.value(QStringLiteral("failedParams")).toObject();
    const QString error = params.value(QStringLiteral("error")).toString(
        params.value(QStringLiteral("validationError")).toString(
            params.value(QStringLiteral("executionError")).toString())).trimmed();
    const QString prompt = params.value(QStringLiteral("prompt")).toString(
        params.value(QStringLiteral("userPrompt")).toString()).trimmed();
    const QString searchable = QStringLiteral("%1 %2 %3 %4")
        .arg(failedTool, error, prompt,
             QString::fromUtf8(QJsonDocument(failedParams).toJson(QJsonDocument::Compact)))
        .toLower();

    QStringList candidateNames;
    QJsonArray candidateGroups;
    auto addGroup = [&](const QString& id, const QString& reason, std::initializer_list<QString> names) {
        QStringList groupNames;
        for (const QString& name : names) {
            appendToolNameIfAvailable(groupNames, allTools, name);
            appendToolNameIfAvailable(candidateNames, allTools, name);
        }
        if (groupNames.isEmpty()) {
            return;
        }
        candidateGroups.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("toolNames"), QJsonArray::fromStringList(groupNames)},
            {QStringLiteral("index"), compactIndexForToolNames(allTools, groupNames)},
        });
    };

    if (!failedTool.isEmpty()) {
        appendToolNameIfAvailable(candidateNames, allTools, failedTool);
    }

    const bool mentionsExtrude = failedTool.contains(QStringLiteral("extrude"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("extrud"), QStringLiteral("zylinder"), QStringLiteral("cylinder"), QStringLiteral("solid")});
    const bool mentionsCircle = failedTool == QStringLiteral("circle.extrude")
        || textContainsAny(searchable, {QStringLiteral("circle"), QStringLiteral("kreis"), QStringLiteral("zylinder"), QStringLiteral("cylinder"), QStringLiteral("acdbcircle")});
    const bool mentionsRectangle = failedTool == QStringLiteral("rectangles.extrude")
        || textContainsAny(searchable, {QStringLiteral("rectangle"), QStringLiteral("rechteck"), QStringLiteral("polyline"), QStringLiteral("acdbpolyline")});
    const bool mentionsMove = failedTool.contains(QStringLiteral("move"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("move"), QStringLiteral("verschieb"), QStringLiteral("translation")});
    const bool mentionsRotate = failedTool.contains(QStringLiteral("rotate"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("rotate"), QStringLiteral("rotier"), QStringLiteral("dreh")});
    const bool mentionsCopy = failedTool.contains(QStringLiteral("copy"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("copy"), QStringLiteral("kopier"), QStringLiteral("dupliz")});
    const bool mentionsScale = failedTool.contains(QStringLiteral("scale"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("scale"), QStringLiteral("skalier")});
    const bool mentionsDelete = failedTool.contains(QStringLiteral("delete"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("delete"), QStringLiteral("erase"), QStringLiteral("loesch"), QStringLiteral("lÃ¶sch")});
    const bool mentionsLayer = failedTool.contains(QStringLiteral("layer"), Qt::CaseInsensitive)
        || textContainsAny(searchable, {QStringLiteral("layer"), QStringLiteral("ebene")});
    const bool mentionsBim = textContainsAny(searchable, {
        QStringLiteral("bim"), QStringLiteral("door"), QStringLiteral("window"), QStringLiteral("wall"),
        QStringLiteral("tuer"), QStringLiteral("tÃ¼r"), QStringLiteral("fenster"), QStringLiteral("wand")});

    addGroup(QStringLiteral("read_current_drawing"),
        QStringLiteral("Read exact target facts before choosing another write path."),
        {QStringLiteral("geometry.query"), QStringLiteral("entity.describe"), QStringLiteral("selection.describe"), QStringLiteral("measurement.bbox")});

    if (mentionsBim) {
        addGroup(QStringLiteral("bim_read_and_selection"),
            QStringLiteral("BIM names/classes/GUIDs must be resolved before mutation."),
            {QStringLiteral("bim.objects.query"), QStringLiteral("bim.selection.set"), QStringLiteral("brx.sdk.selection.setPickfirst")});
    }

    if (mentionsExtrude) {
        addGroup(QStringLiteral("extrusion_alternatives"),
            mentionsCircle
                ? QStringLiteral("Circle/cylinder intent must consider circle.extrude before rectangle-only extrusion.")
                : QStringLiteral("Extrusion repair should compare profile-specific tools and read target geometry."),
            {QStringLiteral("circle.extrude"), QStringLiteral("profile.extrude"), QStringLiteral("rectangles.extrude"), QStringLiteral("entity.setLayer")});
        if (mentionsRectangle) {
            addGroup(QStringLiteral("rectangle_specific_repair"),
                QStringLiteral("Rectangle extrusion only applies to closed rectangle polylines; inspect geometry first."),
                {QStringLiteral("geometry.query"), QStringLiteral("rectangles.extrude"), QStringLiteral("profile.extrude")});
        }
    }

    if (mentionsMove) {
        addGroup(QStringLiteral("move_alternatives"),
            QStringLiteral("Compare high-level move with SDK entity/block-reference movement and association refresh."),
            {QStringLiteral("geometry.move"), QStringLiteral("brx.sdk.entity.transformBy"), QStringLiteral("brx.sdk.blockReference.setPosition"), QStringLiteral("brx.sdk.assoc.evaluate")});
    }
    if (mentionsRotate) {
        addGroup(QStringLiteral("rotate_alternatives"),
            QStringLiteral("Compare high-level rotate with SDK transform rotation."),
            {QStringLiteral("geometry.rotate"), QStringLiteral("brx.sdk.entity.rotateBy")});
    }
    if (mentionsCopy) {
        addGroup(QStringLiteral("copy_alternatives"),
            QStringLiteral("Compare high-level copy with SDK clone/copy semantics."),
            {QStringLiteral("geometry.copy"), QStringLiteral("brx.sdk.entity.copy")});
    }
    if (mentionsScale) {
        addGroup(QStringLiteral("scale_alternatives"),
            QStringLiteral("Compare high-level scale with SDK transform scaling."),
            {QStringLiteral("geometry.scale"), QStringLiteral("brx.sdk.entity.scaleBy")});
    }
    if (mentionsDelete) {
        addGroup(QStringLiteral("delete_alternatives"),
            QStringLiteral("Compare high-level delete with SDK erase only after explicit delete intent."),
            {QStringLiteral("geometry.delete"), QStringLiteral("brx.sdk.entity.erase")});
    }
    if (mentionsLayer) {
        addGroup(QStringLiteral("layer_alternatives"),
            QStringLiteral("Layer repair may need layer creation/listing and then entity assignment."),
            {QStringLiteral("layers.list"), QStringLiteral("layers.create"), QStringLiteral("layers.ensureMany"), QStringLiteral("entity.setLayer"), QStringLiteral("brx.sdk.entity.setLayer")});
    }

    addGroup(QStringLiteral("native_command_fallback"),
        QStringLiteral("Use native commands only when commands.list knows the command and typed BRX/SDK tools are not sufficient."),
        {QStringLiteral("commands.list"), QStringLiteral("command.execute")});

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.repair-tool-context.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("mode"), QStringLiteral("full_tool_space")},
        {QStringLiteral("phase"), params.value(QStringLiteral("phase")).toString()},
        {QStringLiteral("retry"), params.value(QStringLiteral("retry")).toInt()},
        {QStringLiteral("failedTool"), failedTool},
        {QStringLiteral("failedParams"), failedParams},
        {QStringLiteral("error"), error.left(4000)},
        {QStringLiteral("allToolCount"), allTools.size()},
        {QStringLiteral("allToolNames"), QJsonArray::fromStringList(toolNames(allTools))},
        {QStringLiteral("allToolIndex"), compactToolIndex(allTools)},
        {QStringLiteral("candidateToolNames"), QJsonArray::fromStringList(candidateNames)},
        {QStringLiteral("candidateGroups"), candidateGroups},
        {QStringLiteral("describeAllTools"), QJsonObject{
            {QStringLiteral("method"), QStringLiteral("qt.brx.tools.describe")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("all"), true},
                {QStringLiteral("offset"), 0},
                {QStringLiteral("limit"), 64},
                {QStringLiteral("includeSchemas"), true},
            }},
        }},
        {QStringLiteral("policy"), QStringLiteral(
            "Repairmodus: alle Tools sind im allToolIndex sichtbar. Wiederhole den abgelehnten Tool-/Param-Pfad nicht. "
            "Wenn candidateGroups nicht reichen, lade vollstaendige Toolkarten mit qt.brx.tools.describe all=true oder konkreten names nach; "
            "nutze qt.brx.db.compatibility fuer unklare Zielklassen, bevor du ask_user verwendest.")},
    };
}

QJsonArray BrxAgent::localContextMethods()
{
    QJsonArray methods{
        QStringLiteral("qt.tools.describe"),
        QStringLiteral("qt.learning.describe"),
        QStringLiteral("qt.documents.describe"),
        QStringLiteral("qt.drawing.manifest"),
        QStringLiteral("qt.drawing.query"),
        QStringLiteral("qt.drawing.fullScan"),
    };
    for (const QString& method : {
             QStringLiteral("qt.brx.tools.describe"),
             QStringLiteral("qt.brx.context.manifest"),
             QStringLiteral("qt.brx.context.fetch"),
             QStringLiteral("qt.brx.db.schema"),
             QStringLiteral("qt.brx.db.query"),
             QStringLiteral("qt.brx.db.inspect"),
             QStringLiteral("qt.brx.db.fullContext"),
             QStringLiteral("qt.brx.db.compatibility"),
             QStringLiteral("qt.brx.execution.history"),
             QStringLiteral("qt.brx.sdk.catalog"),
             QStringLiteral("qt.brx.workflow.testPlan"),
             QStringLiteral("qt.brx.workflow.repairHints"),
             QStringLiteral("qt.brx.workflow.repairContext"),
         }) {
        methods.append(method);
    }
    return methods;
}

QJsonObject BrxAgent::dbSchema()
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.schema.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("readDomains"), stringArray({
            QStringLiteral("documentStatus"), QStringLiteral("layers"), QStringLiteral("blocks"),
            QStringLiteral("spaces"), QStringLiteral("selection"), QStringLiteral("handles"),
            QStringLiteral("entityClasses"), QStringLiteral("properties"), QStringLiteral("xdata"),
            QStringLiteral("extensionDictionary"), QStringLiteral("bimFingerprints"),
            QStringLiteral("bounds"), QStringLiteral("geometry"), QStringLiteral("massProperties")})},
        {QStringLiteral("writeDomains"), stringArray({
            QStringLiteral("createEntity"), QStringLiteral("modifyEntity"), QStringLiteral("transformEntity"),
            QStringLiteral("copyEntity"), QStringLiteral("deleteEntity"), QStringLiteral("setLayer"),
            QStringLiteral("setName"), QStringLiteral("setSelection"), QStringLiteral("bimClassify"),
            QStringLiteral("assocEvaluate"), QStringLiteral("validatedNativeCommand")})},
        {QStringLiteral("mutationPolicy"), QStringLiteral("Write access is exposed only as validated BRX tools. No free C++ or unvalidated command execution is available to the model.")},
        {QStringLiteral("requiredMutationEnvelope"), QJsonObject{
            {QStringLiteral("preflight"), QStringLiteral("actions.validate")},
            {QStringLiteral("confirmation"), QStringLiteral("required for modifiesDrawing/modifiesEditorState")},
            {QStringLiteral("rollback"), QStringLiteral("undo/readback on failure")},
            {QStringLiteral("readback"), QStringLiteral("before/after result required")},
        }},
    };
}

QJsonObject BrxAgent::sdkCatalog(const QJsonObject& params)
{
    const QString query = params.value(QStringLiteral("query")).toString().trimmed().toLower();
    QJsonArray tools;
    for (const QJsonValue& value : sdkTools()) {
        const QJsonObject tool = value.toObject();
        const QString text = QString::fromUtf8(QJsonDocument(tool).toJson(QJsonDocument::Compact)).toLower();
        if (query.isEmpty() || text.contains(query)) {
            tools.append(tool);
        }
    }
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.sdk.catalog.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("query"), query},
        {QStringLiteral("count"), tools.size()},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("policy"), QStringLiteral("Kuratierter V1-Katalog. Weitere BRX-SDK-Funktionen werden als echte validierte Tools ergaenzt, nicht vom Modell erfunden.")},
    };
}

QJsonObject BrxAgent::dbCompatibility(const QJsonObject& params)
{
    const QString intent = params.value(QStringLiteral("intent")).toString().trimmed().toLower();
    QString entityClass = params.value(QStringLiteral("entityClass")).toString(
        params.value(QStringLiteral("class")).toString()).trimmed();
    QString bimType = params.value(QStringLiteral("bimType")).toString(
        params.value(QStringLiteral("classification")).toString()).trimmed();
    const QJsonArray handles = params.value(QStringLiteral("handles")).toArray();
    const QJsonArray storeFacts = params.value(QStringLiteral("storeFacts")).toArray();
    if ((!entityClass.isEmpty() || !bimType.isEmpty()) == false && !storeFacts.isEmpty()) {
        const QJsonObject fact = storeFacts.first().toObject();
        entityClass = fact.value(QStringLiteral("entityType")).toString();
        bimType = fact.value(QStringLiteral("bim")).toObject().value(QStringLiteral("type")).toString(
            fact.value(QStringLiteral("bimType")).toString());
    }
    const bool moveIntent = intent.contains(QStringLiteral("move"))
        || intent.contains(QStringLiteral("verschieb"))
        || intent.contains(QStringLiteral("translation"));
    const bool copyIntent = intent.contains(QStringLiteral("copy"))
        || intent.contains(QStringLiteral("kopier"))
        || intent.contains(QStringLiteral("dupliz"));
    const bool rotateIntent = intent.contains(QStringLiteral("rotate"))
        || intent.contains(QStringLiteral("rotier"))
        || intent.contains(QStringLiteral("dreh"));
    const bool scaleIntent = intent.contains(QStringLiteral("scale"))
        || intent.contains(QStringLiteral("skalier"));
    const bool deleteIntent = intent.contains(QStringLiteral("delete"))
        || intent.contains(QStringLiteral("erase"))
        || intent.contains(QStringLiteral("loesch"))
        || intent.contains(QStringLiteral("lösch"));
    const bool setLayerIntent = intent.contains(QStringLiteral("setlayer"))
        || intent.contains(QStringLiteral("set layer"))
        || intent.contains(QStringLiteral("layer zu"))
        || intent.contains(QStringLiteral("zu layer"))
        || intent.contains(QStringLiteral("layer assign"))
        || intent.contains(QStringLiteral("layer zuweis"));
    const bool setNameIntent = intent.contains(QStringLiteral("setname"))
        || intent.contains(QStringLiteral("set name"))
        || intent.contains(QStringLiteral("benenn"))
        || intent.contains(QStringLiteral("name"));
    const bool selectionIntent = intent.contains(QStringLiteral("selection"))
        || intent.contains(QStringLiteral("select"))
        || intent.contains(QStringLiteral("auswahl"))
        || intent.contains(QStringLiteral("selekt"));
    const bool classifyIntent = intent.contains(QStringLiteral("classify"))
        || intent.contains(QStringLiteral("klassifiz"))
        || intent.contains(QStringLiteral("bim"));

    QJsonArray candidates;
    auto appendCandidate = [&candidates](const QString& tool, const QString& reason, const QJsonArray& requiredContext = {}) {
        candidates.append(QJsonObject{
            {QStringLiteral("tool"), tool},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("requiredContext"), requiredContext},
        });
    };

    if (moveIntent) {
        if (entityClass.contains(QStringLiteral("BlockReference"), Qt::CaseInsensitive)
            || bimType.contains(QStringLiteral("DOOR"), Qt::CaseInsensitive)
            || bimType.contains(QStringLiteral("WINDOW"), Qt::CaseInsensitive)) {
            appendCandidate(QStringLiteral("brx.sdk.blockReference.setPosition"),
                QStringLiteral("Target is or may be AcDbBlockReference; inspect current insertion point and move by vector."),
                QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("bim.objects.query if BIM name is used")});
        }
        appendCandidate(QStringLiteral("brx.sdk.entity.transformBy"),
            QStringLiteral("Generic whole-entity translation candidate for writable AcDbEntity targets."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect")});
        appendCandidate(QStringLiteral("geometry.move"),
            QStringLiteral("High-level whole-entity move remains available when selector and vector are concrete."),
            QJsonArray{QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("brx.sdk.assoc.evaluate"),
            QStringLiteral("Use after a successful relation-sensitive mutation if associative/BIM readback is stale."),
            QJsonArray{QStringLiteral("previous mutation result")});
    }

    if (copyIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.copy"),
            QStringLiteral("Generic copy candidate for writable AcDbEntity targets when vector/offset/spacing is concrete."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("geometry.copy"),
            QStringLiteral("High-level copy bridge remains available when selector and offset are concrete."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (rotateIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.rotateBy"),
            QStringLiteral("Generic rotate candidate for complete writable entities when angle and base point are explicit."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("geometry.rotate"),
            QStringLiteral("High-level rotate bridge remains available when selector, base point and angle are concrete."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (scaleIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.scaleBy"),
            QStringLiteral("Generic uniform scale candidate for complete writable entities when factor and base point are explicit."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("geometry.scale"),
            QStringLiteral("High-level uniform scale bridge remains available when selector, base point and factor are concrete."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (deleteIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.erase"),
            QStringLiteral("Generic erase candidate for concrete writable entities after explicit delete intent and confirm=true."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("geometry.delete"),
            QStringLiteral("High-level delete bridge remains available when selector is concrete and confirm=true."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (setLayerIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.setLayer"),
            QStringLiteral("SDK layer assignment candidate for writable entities when target layer is known or createIfMissing=true."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("layers.list"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("entity.setLayer"),
            QStringLiteral("High-level layer assignment bridge remains available with selector and target layer."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (setNameIntent && !setLayerIntent) {
        appendCandidate(QStringLiteral("brx.sdk.entity.setName"),
            QStringLiteral("SDK-backed entity metadata naming candidate. Use for entities/BIM objects, not layers."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("entity.setName"),
            QStringLiteral("High-level entity naming bridge remains available with selector and name."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (selectionIntent) {
        appendCandidate(QStringLiteral("brx.sdk.selection.setPickfirst"),
            QStringLiteral("SDK pickfirst selection candidate for concrete handles/selector facts before selection-dependent operations."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("selection.describe")});
        appendCandidate(QStringLiteral("selection.set"),
            QStringLiteral("High-level selection bridge remains available for handles/layer/current-space selectors."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (classifyIntent) {
        appendCandidate(QStringLiteral("brx.sdk.bim.classification.set"),
            QStringLiteral("SDK-backed BIM classification candidate when BIM classification is explicitly intended and target solids are concrete."),
            QJsonArray{QStringLiteral("qt.brx.db.inspect"), QStringLiteral("bim.objects.query"), QStringLiteral("actions.validate")});
        appendCandidate(QStringLiteral("bim.classify"),
            QStringLiteral("High-level BIM classify bridge remains available for selected/recent 3D solids."),
            QJsonArray{QStringLiteral("actions.validate")});
    }

    if (candidates.isEmpty()) {
        appendCandidate(QStringLiteral("qt.brx.tools.describe"),
            QStringLiteral("No direct compatibility rule matched; ask BrxAgent for tool details by query before proposing actions."));
    }

    QJsonArray storeFactPreview;
    for (int i = 0; i < storeFacts.size() && i < 12; ++i) {
        storeFactPreview.append(storeFacts.at(i));
    }

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.compatibility.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("intent"), intent},
        {QStringLiteral("entityClass"), entityClass},
        {QStringLiteral("bimType"), bimType},
        {QStringLiteral("handles"), handles},
        {QStringLiteral("storeFactCount"), storeFacts.size()},
        {QStringLiteral("storeFacts"), storeFactPreview},
        {QStringLiteral("candidates"), candidates},
        {QStringLiteral("policy"), QStringLiteral("Die AI waehlt selbst den besten Kandidaten. Mutierende Kandidaten muessen als action_proposal durch Qt/BRX validiert werden.")},
    };
}

QJsonObject BrxAgent::workflowTestPlan(const QJsonObject& params)
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.workflow.test-plan.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("workflowId"), params.value(QStringLiteral("workflowId")).toString()},
        {QStringLiteral("recommendedSteps"), QJsonArray{
            QStringLiteral("Load workflow definition and requiredRuntimeContext."),
            QStringLiteral("Inspect current drawing/selection through qt.brx.db.inspect or BRX read-only tools."),
            QStringLiteral("Ask qt.brx.db.compatibility for ambiguous SDK choices."),
            QStringLiteral("Produce one confirmed action_proposal with concrete params."),
            QStringLiteral("After execution, store testRuns/readbacks and update ai_runtime workflow if repair succeeded."),
        }},
        {QStringLiteral("policy"), QStringLiteral("Workflow ausfuehren is AI-guided. Do not run stale executionBatches directly when context or tools need analysis.")},
    };
}

QJsonObject BrxAgent::workflowRepairHints(const QJsonObject& params)
{
    const QString error = params.value(QStringLiteral("error")).toString();
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.workflow.repair-hints.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("error"), error},
        {QStringLiteral("hints"), QJsonArray{
            QStringLiteral("Do not repeat the same rejected tool/params."),
            QStringLiteral("Inspect target handles and entity classes before choosing another write tool."),
            QStringLiteral("Prefer BRX/SDK tools over command.execute when a typed tool exists."),
            QStringLiteral("Update only source=ai_runtime/updateProtected=false workflows with successful repaired steps."),
        }},
    };
}

QJsonObject BrxAgent::workflowRepairHints(const QJsonObject& params, const QJsonObject& repairContext)
{
    QJsonObject result = workflowRepairHints(params);
    result.insert(QStringLiteral("repairContext"), repairContext);
    result.insert(QStringLiteral("policy"), QStringLiteral("Repair-Hints sind mit lokalem Execution-/DB-Kontext angereichert. Nutze Facts und letzte Fehler fuer einen anderen validierbaren Toolpfad."));
    return result;
}

QJsonObject BrxAgent::dbContextFromStoreResult(const QString& method, const QJsonObject& params, const QJsonObject& storeResult)
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.context.v1")},
        {QStringLiteral("source"), QStringLiteral("BrxAgent")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
        {QStringLiteral("result"), storeResult},
        {QStringLiteral("policy"), QStringLiteral("This is local cached drawing context. If data is missing or stale, request BRX read-only methods such as entity.describe, selection.describe, geometry.query or bim.objects.query.")},
    };
}
