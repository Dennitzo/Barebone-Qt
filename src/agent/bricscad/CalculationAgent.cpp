#include "CalculationAgent.h"

#include "BricsCadAgentUtils.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <cmath>
#include <algorithm>

namespace {

constexpr const char* kFloorPlanWorkflowId = "grundriss_und_aussenwaende_einzeichnen";

QJsonObject point(double x, double y, double z);

bool isFloorPlanWorkflow(const QJsonObject& workflow)
{
    return BricsCadAgentUtils::workflowSlug(workflow.value(QStringLiteral("id")).toString())
        == QString::fromLatin1(kFloorPlanWorkflowId);
}

QJsonObject findFloorPlanWorkflow(
    const CalculationAgent::Request& request,
    const CalculationAgent::WorkflowLoader& loader)
{
    for (const QJsonValue& value : request.selectedWorkflows) {
        const QJsonObject workflow = value.toObject();
        if (isFloorPlanWorkflow(workflow)) {
            return workflow;
        }
    }
    for (const QString& id : BricsCadAgentUtils::routeWorkflowIds(request.route, 3)) {
        if (!loader) {
            continue;
        }
        const QJsonObject workflow = loader(id);
        if (isFloorPlanWorkflow(workflow)) {
            return workflow;
        }
    }
    const QStringList localIds = BricsCadAgentUtils::localWorkflowSelection(request.workflowIndex, request.prompt, 1);
    for (const QString& id : localIds) {
        if (!loader) {
            continue;
        }
        const QJsonObject workflow = loader(id);
        if (isFloorPlanWorkflow(workflow)) {
            return workflow;
        }
    }
    return {};
}

double numberFromMatch(const QRegularExpressionMatch& match)
{
    return match.captured(1).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
}

QString normalizedCalculationText(QString text)
{
    text.replace(QChar(0x00B2), QStringLiteral("2"));
    text.replace(QStringLiteral("Â²"), QStringLiteral("2"));
    text.replace(QStringLiteral("Ã‚Â²"), QStringLiteral("2"));
    text.replace(QStringLiteral("Ãƒâ€šÃ‚Â²"), QStringLiteral("2"));
    text.replace(QStringLiteral("ÃƒÆ’Ã‚Â¤"), QStringLiteral("ae"));
    text.replace(QStringLiteral("ÃƒÆ’Ã‚Â¶"), QStringLiteral("oe"));
    text.replace(QStringLiteral("ÃƒÆ’Ã‚Â¼"), QStringLiteral("ue"));
    text.replace(QStringLiteral("ÃƒÆ’Ã…Â¸"), QStringLiteral("ss"));
    text.replace(',', '.');
    return BricsCadAgentUtils::normalizedSearchText(text);
}

QStringList recentUserTexts(const QJsonArray& conversation, int maxMessages)
{
    QStringList messages;
    for (int i = conversation.size() - 1; i >= 0 && messages.size() < maxMessages; --i) {
        const QJsonObject item = conversation.at(i).toObject();
        if (item.value(QStringLiteral("role")).toString() != QStringLiteral("user")) {
            continue;
        }
        QString content = item.value(QStringLiteral("content")).toString().trimmed();
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject object = doc.object();
            content = object.value(QStringLiteral("userPrompt")).toString(
                object.value(QStringLiteral("message")).toString(content)).trimmed();
        }
        if (!content.isEmpty()) {
            messages.prepend(content);
        }
    }
    return messages;
}

bool promptLooksLikeCorrectionOrContinuation(const QString& prompt)
{
    const QString normalized = normalizedCalculationText(prompt);
    return BricsCadAgentUtils::textMentionsAny(normalized, {
        QStringLiteral("noch"),
        QStringLiteral("statt"),
        QStringLiteral("anstatt"),
        QStringLiteral("aender"),
        QStringLiteral("ander"),
        QStringLiteral("korrig"),
        QStringLiteral("soll"),
        QStringLiteral("sollte"),
        QStringLiteral("nicht"),
        QStringLiteral("nun"),
        QStringLiteral("jetzt"),
        QStringLiteral("dafuer"),
        QStringLiteral("uebernehmen"),
        QStringLiteral("verwende"),
        QStringLiteral("nutze"),
    });
}

double numberToWorkflowUnit(const QString& slot, double value, const QString& unit)
{
    const QString normalizedUnit = normalizedCalculationText(unit);
    if (slot.endsWith(QStringLiteral("Mm"))) {
        if (normalizedUnit == QStringLiteral("m") || normalizedUnit == QStringLiteral("meter")) {
            return value * 1000.0;
        }
        if (normalizedUnit == QStringLiteral("cm")) {
            return value * 10.0;
        }
        if (normalizedUnit.isEmpty()) {
            if (slot == QStringLiteral("wallThicknessMm")) {
                if (value <= 2.0) {
                    return value * 1000.0;
                }
                if (value <= 100.0) {
                    return value * 10.0;
                }
                return value;
            }
            if (value <= 50.0) {
                return value * 1000.0;
            }
        }
        return value;
    }

    if (slot == QStringLiteral("roomAreaM2")) {
        if (normalizedUnit == QStringLiteral("mm2")) {
            return value / 1000000.0;
        }
        if (normalizedUnit == QStringLiteral("cm2")) {
            return value / 10000.0;
        }
        return value;
    }

    return value;
}

bool extractNumberForAliases(
    const QString& prompt,
    const QStringList& aliases,
    const QString& slot,
    double* value)
{
    if (!value) {
        return false;
    }
    const QString normalized = normalizedCalculationText(prompt);
    for (const QString& alias : aliases) {
        const QString key = QRegularExpression::escape(normalizedCalculationText(alias));
        if (key.isEmpty()) {
            continue;
        }

        const QString unitPattern = QStringLiteral("(mm2|cm2|m2|mm|cm|m|qm|quadratmeter|meter)?");
        QRegularExpressionMatch match;
        if (key.size() == 1) {
            const QRegularExpression keyedShortPattern(
                QStringLiteral(R"((?:^|[^a-z0-9_])%1\s*[:=]\s*(-?\d+(?:\.\d+)?)\s*%2)")
                    .arg(key, unitPattern),
                QRegularExpression::CaseInsensitiveOption);
            match = keyedShortPattern.match(normalized);
        } else {
            const QRegularExpression keyedAfterPattern(
                QStringLiteral(R"((?:^|[^a-z0-9_])%1(?:\b|[^a-z0-9_])[^0-9-]{0,48}(-?\d+(?:\.\d+)?)\s*%2)")
                    .arg(key, unitPattern),
                QRegularExpression::CaseInsensitiveOption);
            match = keyedAfterPattern.match(normalized);
        }

        if (match.hasMatch()) {
            bool ok = false;
            const double parsed = match.captured(1).toDouble(&ok);
            if (ok && parsed > 0.0) {
                *value = numberToWorkflowUnit(slot, parsed, match.captured(2));
                return true;
            }
        }

        if (key.size() == 1) {
            continue;
        }

        const QRegularExpression keyedBeforePattern(
            QStringLiteral(R"((-?\d+(?:\.\d+)?)\s*%1[^\n.;:]{0,64}(?:^|[^a-z0-9_])%2(?:\b|[^a-z0-9_]))")
                .arg(unitPattern, key),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator matches = keyedBeforePattern.globalMatch(normalized);
        QRegularExpressionMatch lastMatch;
        while (matches.hasNext()) {
            lastMatch = matches.next();
        }
        if (!lastMatch.hasMatch()) {
            continue;
        }
        bool ok = false;
        const double parsed = lastMatch.captured(1).toDouble(&ok);
        if (ok && parsed > 0.0) {
            *value = numberToWorkflowUnit(slot, parsed, lastMatch.captured(2));
            return true;
        }
    }
    return false;
}

bool extractAreaM2(const QString& prompt, double* value)
{
    const QString normalized = normalizedCalculationText(prompt);
    const QList<QRegularExpression> patterns{
        QRegularExpression(QStringLiteral("(?:innenflaeche|flaeche|area)\\s*[:=]?\\s*([0-9]+(?:[\\.,][0-9]+)?)\\s*(?:qm|m2|m\\s*2|quadratmeter)")),
        QRegularExpression(QStringLiteral("([0-9]+(?:[\\.,][0-9]+)?)\\s*(?:qm|m2|m\\s*2|quadratmeter)")),
    };
    for (const QRegularExpression& pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(normalized);
        if (match.hasMatch()) {
            *value = numberFromMatch(match);
            return true;
        }
    }
    return extractNumberForAliases(prompt, {
        QStringLiteral("innenflaeche"),
        QStringLiteral("flaeche"),
        QStringLiteral("wohnflaeche"),
        QStringLiteral("grundflaeche"),
        QStringLiteral("area"),
        QStringLiteral("qm"),
        QStringLiteral("m2"),
        QStringLiteral("quadratmeter"),
    }, QStringLiteral("roomAreaM2"), value);
}

bool extractLayerName(const QString& prompt, QString* layerName)
{
    if (!layerName) {
        return false;
    }
    const QRegularExpression pattern(
        QStringLiteral(R"((?:layer|layername|ebene)\s*[:=]\s*["']?([^"',;\n\r]{1,48})["']?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(prompt);
    if (!match.hasMatch()) {
        return false;
    }
    const QString value = match.captured(1).trimmed();
    if (value.isEmpty()) {
        return false;
    }
    *layerName = value;
    return true;
}

bool extractOriginPoint(const QString& prompt, QJsonObject* origin)
{
    if (!origin) {
        return false;
    }
    const QString normalized = normalizedCalculationText(prompt);
    if (!BricsCadAgentUtils::textMentionsAny(normalized, {
            QStringLiteral("origin"),
            QStringLiteral("ursprung"),
            QStringLiteral("startpunkt"),
            QStringLiteral("wcs"),
        })) {
        return false;
    }

    auto axisValue = [&normalized](const QString& axis, double* out) {
        const QRegularExpression pattern(
            QStringLiteral(R"((?:^|[^a-z0-9_])%1\s*[:=]\s*(-?\d+(?:\.\d+)?)\s*(mm|cm|m|meter)?)")
                .arg(axis),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = pattern.match(normalized);
        if (!match.hasMatch()) {
            return false;
        }
        bool ok = false;
        const double parsed = match.captured(1).toDouble(&ok);
        if (!ok) {
            return false;
        }
        *out = numberToWorkflowUnit(QStringLiteral("roomLengthMm"), parsed, match.captured(2));
        return true;
    };

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    const bool hasX = axisValue(QStringLiteral("x"), &x);
    const bool hasY = axisValue(QStringLiteral("y"), &y);
    const bool hasZ = axisValue(QStringLiteral("z"), &z);
    if (!hasX && !hasY && !hasZ) {
        return false;
    }
    *origin = point(hasX ? x : 0.0, hasY ? y : 0.0, hasZ ? z : 0.0);
    return true;
}

struct FloorPlanOverrides {
    bool area = false;
    double areaM2 = 0.0;
    bool width = false;
    double widthMm = 0.0;
    bool length = false;
    double lengthMm = 0.0;
    bool thickness = false;
    double thicknessMm = 0.0;
    bool height = false;
    double heightMm = 0.0;
    bool layer = false;
    QString layerName;
    bool origin = false;
    QJsonObject originPoint;
};

FloorPlanOverrides extractFloorPlanOverrides(const QString& prompt)
{
    FloorPlanOverrides overrides;
    overrides.area = extractAreaM2(prompt, &overrides.areaM2);
    overrides.width = extractNumberForAliases(prompt, {
        QStringLiteral("innenbreite"),
        QStringLiteral("raumbreite"),
        QStringLiteral("hausbreite"),
        QStringLiteral("gebaeudebreite"),
        QStringLiteral("breite"),
        QStringLiteral("roomwidth"),
        QStringLiteral("interiorwidth"),
        QStringLiteral("b"),
    }, QStringLiteral("roomWidthMm"), &overrides.widthMm);
    overrides.length = extractNumberForAliases(prompt, {
        QStringLiteral("innenlaenge"),
        QStringLiteral("raumlaenge"),
        QStringLiteral("hauslaenge"),
        QStringLiteral("gebaeudelaenge"),
        QStringLiteral("laenge"),
        QStringLiteral("lange"),
        QStringLiteral("roomlength"),
        QStringLiteral("interiorlength"),
        QStringLiteral("l"),
    }, QStringLiteral("roomLengthMm"), &overrides.lengthMm);
    overrides.thickness = extractNumberForAliases(prompt, {
        QStringLiteral("wandstaerke"),
        QStringLiteral("wanddicke"),
        QStringLiteral("mauerstaerke"),
        QStringLiteral("mauerdicke"),
        QStringLiteral("wallthickness"),
        QStringLiteral("wall thickness"),
        QStringLiteral("thickness"),
        QStringLiteral("dicke"),
        QStringLiteral("staerke"),
    }, QStringLiteral("wallThicknessMm"), &overrides.thicknessMm);
    overrides.height = extractNumberForAliases(prompt, {
        QStringLiteral("wandhoehe"),
        QStringLiteral("wallheight"),
        QStringLiteral("wall height"),
        QStringLiteral("hoehe"),
        QStringLiteral("hohe"),
        QStringLiteral("height"),
    }, QStringLiteral("wallHeightMm"), &overrides.heightMm);
    overrides.layer = extractLayerName(prompt, &overrides.layerName);
    overrides.origin = extractOriginPoint(prompt, &overrides.originPoint);
    return overrides;
}

bool hasPrimaryFloorPlanDimensions(const FloorPlanOverrides& overrides)
{
    return overrides.area || overrides.width || overrides.length;
}

QJsonObject point(double x, double y, double z)
{
    return QJsonObject{
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("z"), z},
    };
}

QJsonObject wallBox(
    const QString& id,
    double x,
    double y,
    double z,
    double width,
    double depth,
    double height,
    const QString& layer)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("geometry"), QStringLiteral("box")},
        {QStringLiteral("origin"), point(x, y, z)},
        {QStringLiteral("width"), width},
        {QStringLiteral("depth"), depth},
        {QStringLiteral("height"), height},
        {QStringLiteral("layer"), layer},
    };
}

QJsonObject compactWorkflowCapsule(const QJsonObject& workflow)
{
    return BricsCadAgentUtils::workflowCapsule(workflow, false);
}

} // namespace

QJsonObject CalculationAgent::deterministicFloorPlanCalculation(
    const Request& request,
    WorkflowLoader workflowLoader)
{
    const QJsonObject workflow = findFloorPlanWorkflow(request, workflowLoader);
    if (workflow.isEmpty()) {
        return {};
    }

    QJsonObject values = workflow.value(QStringLiteral("knownSlotValues")).toObject();
    for (auto it = request.selectedWorkflowSlotValues.constBegin();
         it != request.selectedWorkflowSlotValues.constEnd();
         ++it) {
        values.insert(it.key(), it.value());
    }

    bool explicitArea = false;
    bool explicitWidth = false;
    bool explicitLength = false;
    bool explicitThickness = false;
    bool explicitHeight = false;
    bool explicitLayer = false;
    bool explicitOrigin = false;

    auto applyOverrides = [&](const FloorPlanOverrides& overrides) {
        if (overrides.area) {
            values.insert(QStringLiteral("roomAreaM2"), overrides.areaM2);
            explicitArea = true;
        }
        if (overrides.width) {
            values.insert(QStringLiteral("roomWidthMm"), overrides.widthMm);
            explicitWidth = true;
        }
        if (overrides.length) {
            values.insert(QStringLiteral("roomLengthMm"), overrides.lengthMm);
            explicitLength = true;
        }
        if (overrides.thickness) {
            values.insert(QStringLiteral("wallThicknessMm"), overrides.thicknessMm);
            explicitThickness = true;
        }
        if (overrides.height) {
            values.insert(QStringLiteral("wallHeightMm"), overrides.heightMm);
            explicitHeight = true;
        }
        if (overrides.layer) {
            values.insert(QStringLiteral("targetLayer"), overrides.layerName);
            values.insert(QStringLiteral("layerName"), overrides.layerName);
            explicitLayer = true;
        }
        if (overrides.origin) {
            values.insert(QStringLiteral("origin"), overrides.originPoint);
            explicitOrigin = true;
        }
    };

    const FloorPlanOverrides promptOverrides = extractFloorPlanOverrides(request.prompt);
    const bool useConversationForMissingDimensions =
        !request.conversation.isEmpty()
        && (!hasPrimaryFloorPlanDimensions(promptOverrides)
            || promptLooksLikeCorrectionOrContinuation(request.prompt));
    if (useConversationForMissingDimensions) {
        const QString historyText = recentUserTexts(request.conversation, 6).join(QStringLiteral("\n"));
        applyOverrides(extractFloorPlanOverrides(historyText));
    }
    applyOverrides(promptOverrides);

    double areaM2 = values.value(QStringLiteral("roomAreaM2")).toDouble(100.0);
    double width = values.value(QStringLiteral("roomWidthMm")).toDouble(10000.0);
    double length = values.value(QStringLiteral("roomLengthMm")).toDouble(10000.0);
    const double requestedAreaMm2 = areaM2 * 1000000.0;

    if (explicitArea && !explicitWidth && !explicitLength) {
        width = std::sqrt(requestedAreaMm2);
        length = width;
    } else if (explicitArea && explicitWidth && !explicitLength && width > 0.0) {
        length = requestedAreaMm2 / width;
    } else if (explicitArea && !explicitWidth && explicitLength && length > 0.0) {
        width = requestedAreaMm2 / length;
    } else if (!explicitArea && (explicitWidth || explicitLength)) {
        areaM2 = width * length / 1000000.0;
    }

    if (explicitArea && explicitWidth && explicitLength
        && std::abs(width * length - requestedAreaMm2) > 10000.0) {
        return QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.calculation.v1")},
            {QStringLiteral("workflowIdUsed"), workflow.value(QStringLiteral("id")).toString()},
            {QStringLiteral("readyForExecution"), false},
            {QStringLiteral("error"), QStringLiteral("Explizite Innenflaeche, Breite und Laenge widersprechen sich.")},
            {QStringLiteral("missing"), QJsonArray{QStringLiteral("konsistente Innenflaeche oder Abmessungen")}},
        };
    }

    const double thickness = values.value(QStringLiteral("wallThicknessMm")).toDouble(300.0);
    const double height = values.value(QStringLiteral("wallHeightMm")).toDouble(2500.0);
    const QString layer = values.value(QStringLiteral("targetLayer")).toString(
        values.value(QStringLiteral("layerName")).toString(QStringLiteral("0")));
    const QJsonObject origin = values.value(QStringLiteral("origin")).toObject();
    const double ox = origin.value(QStringLiteral("x")).toDouble(0.0);
    const double oy = origin.value(QStringLiteral("y")).toDouble(0.0);
    const double oz = origin.value(QStringLiteral("z")).toDouble(0.0);

    const QJsonArray wallBoxes{
        wallBox(QStringLiteral("south"), ox - thickness, oy - thickness, oz, width + 2.0 * thickness, thickness, height, layer),
        wallBox(QStringLiteral("north"), ox - thickness, oy + length, oz, width + 2.0 * thickness, thickness, height, layer),
        wallBox(QStringLiteral("west"), ox - thickness, oy, oz, thickness, length, height, layer),
        wallBox(QStringLiteral("east"), ox + width, oy, oz, thickness, length, height, layer),
    };

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.calculation.v1")},
        {QStringLiteral("workflowIdUsed"), workflow.value(QStringLiteral("id")).toString()},
        {QStringLiteral("readyForExecution"), true},
        {QStringLiteral("values"), QJsonArray{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("interiorAreaM2")}, {QStringLiteral("value"), width * length / 1000000.0}, {QStringLiteral("unit"), QStringLiteral("m2")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("interiorWidthMm")}, {QStringLiteral("value"), width}, {QStringLiteral("unit"), QStringLiteral("mm")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("interiorLengthMm")}, {QStringLiteral("value"), length}, {QStringLiteral("unit"), QStringLiteral("mm")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("wallThicknessMm")}, {QStringLiteral("value"), thickness}, {QStringLiteral("unit"), QStringLiteral("mm")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("wallHeightMm")}, {QStringLiteral("value"), height}, {QStringLiteral("unit"), QStringLiteral("mm")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("expectedAreaMm2")}, {QStringLiteral("value"), width * length}, {QStringLiteral("unit"), QStringLiteral("mm2")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("outerWidthMm")}, {QStringLiteral("value"), width + 2.0 * thickness}, {QStringLiteral("unit"), QStringLiteral("mm")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("outerLengthMm")}, {QStringLiteral("value"), length + 2.0 * thickness}, {QStringLiteral("unit"), QStringLiteral("mm")}},
        }},
        {QStringLiteral("explicitInputs"), QJsonObject{
            {QStringLiteral("area"), explicitArea},
            {QStringLiteral("width"), explicitWidth},
            {QStringLiteral("length"), explicitLength},
            {QStringLiteral("wallThickness"), explicitThickness},
            {QStringLiteral("wallHeight"), explicitHeight},
            {QStringLiteral("layer"), explicitLayer},
            {QStringLiteral("origin"), explicitOrigin},
            {QStringLiteral("usedConversationForMissingDimensions"), useConversationForMissingDimensions},
        }},
        {QStringLiteral("contour"), QJsonObject{
            {QStringLiteral("origin"), point(ox, oy, oz)},
            {QStringLiteral("width"), width},
            {QStringLiteral("depth"), length},
            {QStringLiteral("layer"), layer},
        }},
        {QStringLiteral("wallBoxes"), wallBoxes},
        {QStringLiteral("steps"), QJsonArray{
            QStringLiteral("Innenmasse aus Nutzerwerten und Workflow-Defaults aufgeloest."),
            QStringLiteral("Vier nicht ueberlappende Aussenwandkoerper ausserhalb der Innenkontur berechnet."),
        }},
        {QStringLiteral("missing"), QJsonArray{}},
        {QStringLiteral("uncertainties"), QJsonArray{}},
    };
}

void CalculationAgent::run(
    const Request& request,
    LocalAiAgentRuntime& runtime,
    WorkflowLoader workflowLoader,
    Callback callback) const
{
    QJsonObject deterministic = deterministicFloorPlanCalculation(request, workflowLoader);
    if (!deterministic.isEmpty()) {
        deterministic.insert(QStringLiteral("ready"), !deterministic.contains(QStringLiteral("error")));
        deterministic.insert(QStringLiteral("source"), QStringLiteral("deterministic-workflow-logic"));
        callback(deterministic);
        return;
    }

    QJsonArray workflows;
    for (const QJsonValue& value : request.selectedWorkflows) {
        workflows.append(compactWorkflowCapsule(value.toObject()));
    }
    if (workflows.isEmpty()) {
        for (const QString& id : BricsCadAgentUtils::localWorkflowSelection(request.workflowIndex, request.prompt, 2)) {
            if (workflowLoader) {
                const QJsonObject workflow = workflowLoader(id);
                if (!workflow.isEmpty()) {
                    workflows.append(compactWorkflowCapsule(workflow));
                }
            }
        }
    }

    LocalAiAgentRuntime::JsonRequest jsonRequest;
    jsonRequest.slot = QStringLiteral("calculation");
    jsonRequest.kind = QStringLiteral("BricsCadPreparation-calculation");
    jsonRequest.systemInstruction = QStringLiteral(
        "Du bist der Berechnungs-Agent fuer BricsCAD. "
        "Berechne nur kompakte konkrete Werte aus Prompt und optionalen Workflowkapseln. Keine CAD-Aktion, keine Toolkarten, keine Policies.");
    jsonRequest.input = QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.calculation.input.v1")},
        {QStringLiteral("userPrompt"), request.prompt},
        {QStringLiteral("workflowCapsules"), workflows},
        {QStringLiteral("policy"), QStringLiteral("Fehlende frei waehlbare Beispielwerte plausibel setzen; Widersprueche in missing/error melden.")},
        {QStringLiteral("responseShape"), QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.calculation.v1")},
            {QStringLiteral("readyForExecution"), true},
            {QStringLiteral("workflowIdUsed"), QString()},
            {QStringLiteral("values"), QJsonArray{}},
            {QStringLiteral("steps"), QJsonArray{}},
            {QStringLiteral("missing"), QJsonArray{}},
            {QStringLiteral("uncertainties"), QJsonArray{}},
        }},
    };
    jsonRequest.requiredFields = {
        QStringLiteral("schema"),
        QStringLiteral("readyForExecution"),
        QStringLiteral("workflowIdUsed"),
        QStringLiteral("values"),
        QStringLiteral("steps"),
        QStringLiteral("missing"),
        QStringLiteral("uncertainties"),
    };
    jsonRequest.maxOutputTokens = 2048;
    jsonRequest.priority = 85;
    jsonRequest.cancellable = true;
    jsonRequest.dedupeKey = QStringLiteral("bricscad-preparation:%1:%2:calculation")
        .arg(request.sessionId)
        .arg(request.operationGeneration);
    jsonRequest.operationGeneration = request.operationGeneration;
    jsonRequest.bricsCadMode = request.bricsCadMode;

    runtime.submitJson(jsonRequest, [callback = std::move(callback)](const LocalAiAgentRuntime::JsonResult& result) mutable {
        QJsonObject object;
        if (result.ok) {
            object = result.object;
            object.insert(QStringLiteral("ready"), !object.contains(QStringLiteral("error")));
        } else {
            object = QJsonObject{
                {QStringLiteral("schema"), QStringLiteral("barebone.agent.calculation.v1")},
                {QStringLiteral("ready"), false},
                {QStringLiteral("readyForExecution"), false},
                {QStringLiteral("error"), result.error},
            };
        }
        callback(object);
    });
}
