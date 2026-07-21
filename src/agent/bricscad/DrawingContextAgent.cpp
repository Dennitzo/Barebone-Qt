#include "DrawingContextAgent.h"

#include "BricsCadAgentUtils.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSharedPointer>

QJsonArray DrawingContextAgent::plannedReadOnlyRequests(const QString& prompt, const QJsonObject& capabilities)
{
    QJsonArray requests;
    auto addRequest = [&requests, &capabilities](const QString& method, const QJsonObject& params) {
        if (BricsCadAgentUtils::capabilitiesContainMethod(capabilities, method)) {
            requests.append(QJsonObject{
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), params},
            });
        }
    };

    addRequest(QStringLiteral("layers.list"), QJsonObject{});
    addRequest(QStringLiteral("geometry.query"), QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("currentSpace")}}},
        {QStringLiteral("include"), QJsonArray{
            QStringLiteral("metrics"),
            QStringLiteral("geometry"),
            QStringLiteral("dimensions"),
        }},
        {QStringLiteral("limit"), 80},
    });

    const QString normalizedPrompt = BricsCadAgentUtils::normalizedSearchText(prompt);
    QStringList explicitHandles;
    const QRegularExpression handlePattern(
        QStringLiteral(R"((?:handle|object\s*id)\s*[=:]?\s*([0-9a-f]+))"),
        QRegularExpression::CaseInsensitiveOption);
    auto handleMatches = handlePattern.globalMatch(prompt);
    while (handleMatches.hasNext()) {
        const QString handle = handleMatches.next().captured(1).trimmed().toUpper();
        if (!handle.isEmpty() && !explicitHandles.contains(handle)) {
            explicitHandles.append(handle);
        }
    }
    if (!explicitHandles.isEmpty()) {
        QJsonArray handles;
        for (const QString& handle : explicitHandles) handles.append(handle);
        addRequest(QStringLiteral("entity.describe"), QJsonObject{
            {QStringLiteral("handles"), handles},
            {QStringLiteral("include"), QJsonArray{
                QStringLiteral("core"), QStringLiteral("geometry"), QStringLiteral("metrics"), QStringLiteral("properties")}},
            {QStringLiteral("limit"), explicitHandles.size()},
        });
    }
    if (BricsCadAgentUtils::textMentionsAny(normalizedPrompt, {
            QStringLiteral("bim"),
            QStringLiteral("klassifiz"),
            QStringLiteral("fenster"),
            QStringLiteral("tuer"),
            QStringLiteral("guid"),
        })) {
        addRequest(QStringLiteral("bim.objects.query"), QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("currentSpace")}}},
            {QStringLiteral("include"), QJsonArray{QStringLiteral("core"), QStringLiteral("geometry")}},
            {QStringLiteral("offset"), 0},
            {QStringLiteral("limit"), 60},
        });
    }

    if (explicitHandles.isEmpty() && BricsCadAgentUtils::shouldPrefetchSelectionDescription(prompt)) {
        addRequest(QStringLiteral("selection.describe"), QJsonObject{
            {QStringLiteral("include"), QJsonArray{
                QStringLiteral("geometry"),
                QStringLiteral("metrics"),
                QStringLiteral("dimensions"),
            }},
            {QStringLiteral("limit"), 100},
        });
    }

    return requests;
}

void DrawingContextAgent::run(
    const Request& request,
    LocalAiAgentRuntime& runtime,
    DrawingContextStore& store,
    BridgeRequest bridgeRequest,
    SelectionCallback selectionCallback,
    Callback callback) const
{
    const QJsonArray requests = plannedReadOnlyRequests(request.prompt, request.capabilities);

    auto summarize = [&runtime, callback = std::move(callback), request](const QJsonArray& results) mutable {
        QJsonObject selectionFacts{
            {QStringLiteral("queried"), false},
            {QStringLiteral("available"), false},
            {QStringLiteral("count"), 0},
            {QStringLiteral("objects"), QJsonArray{}},
        };
        for (const QJsonValue& value : results) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("method")).toString() != QStringLiteral("selection.describe")) {
                continue;
            }
            const QJsonObject result = entry.value(QStringLiteral("result")).toObject();
            const QJsonArray objects = result.value(QStringLiteral("objects")).toArray();
            selectionFacts = QJsonObject{
                {QStringLiteral("queried"), true},
                {QStringLiteral("available"), entry.value(QStringLiteral("ok")).toBool(false) && !objects.isEmpty()},
                {QStringLiteral("count"), result.value(QStringLiteral("count")).toInt(objects.size())},
                {QStringLiteral("objects"), objects},
            };
            if (entry.contains(QStringLiteral("error"))) {
                selectionFacts.insert(QStringLiteral("error"), entry.value(QStringLiteral("error")));
            }
            break;
        }
        const QJsonObject snapshot{
            {QStringLiteral("schema"), QStringLiteral("barebone.brx.parallel-drawing-snapshot.v1")},
            {QStringLiteral("requests"), results},
            {QStringLiteral("selection"), selectionFacts},
        };

        if (results.isEmpty()) {
            callback(QJsonObject{
                {QStringLiteral("schema"), QStringLiteral("barebone.agent.drawing-focus.v1")},
                {QStringLiteral("ready"), false},
                {QStringLiteral("summary"), request.brxAuthenticated
                    ? QStringLiteral("Keine read-only BRX-Kontextabfrage verfuegbar.")
                    : QStringLiteral("BRX ist nicht verbunden; kein Live-Zeichnungskontext verfuegbar.")},
                {QStringLiteral("relevantObjects"), QJsonArray{}},
                {QStringLiteral("relevantLayers"), QJsonArray{}},
                {QStringLiteral("measurements"), QJsonArray{}},
                {QStringLiteral("uncertainties"), QJsonArray{}},
            });
            return;
        }

        LocalAiAgentRuntime::JsonRequest jsonRequest;
        jsonRequest.slot = QStringLiteral("drawing");
        jsonRequest.kind = QStringLiteral("BricsCadPreparation-drawing");
        jsonRequest.systemInstruction = QStringLiteral(
            "Du bist der Zeichnungskontext-Agent fuer BricsCAD. "
            "Komprimiere nur den gelieferten read-only Snapshot: Handles, Layer, Typen, Abmessungen, Bounds, Auswahlbezug. "
            "Erfinde keine Zeichnungsdaten und schlage keine Aktion vor. Antworte nur als kompaktes JSON nach outputShape.");
        jsonRequest.input = QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.drawing-focus.request.v1")},
            {QStringLiteral("userPrompt"), request.prompt},
            {QStringLiteral("snapshot"), snapshot},
            {QStringLiteral("outputShape"), QJsonObject{
                {QStringLiteral("schema"), QStringLiteral("barebone.agent.drawing-focus.v1")},
                {QStringLiteral("summary"), QString()},
                {QStringLiteral("relevantObjects"), QJsonArray{}},
                {QStringLiteral("relevantLayers"), QJsonArray{}},
                {QStringLiteral("measurements"), QJsonArray{}},
                {QStringLiteral("uncertainties"), QJsonArray{}},
            }},
        };
        jsonRequest.requiredFields = {
            QStringLiteral("schema"),
            QStringLiteral("summary"),
            QStringLiteral("relevantObjects"),
            QStringLiteral("relevantLayers"),
            QStringLiteral("measurements"),
            QStringLiteral("uncertainties"),
        };
        jsonRequest.maxOutputTokens = 1536;
        jsonRequest.priority = 90;
        jsonRequest.cancellable = true;
        jsonRequest.dedupeKey = QStringLiteral("bricscad-preparation:%1:%2:drawing")
            .arg(request.sessionId)
            .arg(request.operationGeneration);
        jsonRequest.operationGeneration = request.operationGeneration;
        jsonRequest.bricsCadMode = request.bricsCadMode;

        runtime.submitJson(jsonRequest, [callback = std::move(callback), results, selectionFacts](const LocalAiAgentRuntime::JsonResult& result) mutable {
            QJsonObject object;
            if (result.ok) {
                object = result.object;
                object.insert(QStringLiteral("ready"), true);
            } else {
                object = QJsonObject{
                    {QStringLiteral("schema"), QStringLiteral("barebone.agent.drawing-focus.v1")},
                    {QStringLiteral("ready"), false},
                    {QStringLiteral("summary"), QStringLiteral("Zeichnungskontext erfasst, AI-Komprimierung fehlgeschlagen.")},
                    {QStringLiteral("snapshot"), QJsonObject{
                        {QStringLiteral("schema"), QStringLiteral("barebone.brx.parallel-drawing-snapshot.v1")},
                        {QStringLiteral("requests"), results},
                    }},
                    {QStringLiteral("relevantObjects"), QJsonArray{}},
                    {QStringLiteral("relevantLayers"), QJsonArray{}},
                    {QStringLiteral("measurements"), QJsonArray{}},
                    {QStringLiteral("uncertainties"), QJsonArray{result.error}},
                };
            }
            if (object.value(QStringLiteral("summary")).toString().trimmed().isEmpty()) {
                object.insert(QStringLiteral("summary"), QStringLiteral("Zeichnungskontext kompakt erfasst."));
            }
            // Selection is authoritative BRX data and must never depend on the
            // LLM summary preserving it.
            object.insert(QStringLiteral("selection"), selectionFacts);
            callback(object);
        });
    };

    if (!request.brxAuthenticated || requests.isEmpty() || !bridgeRequest) {
        summarize(QJsonArray{});
        return;
    }

    QSharedPointer<QJsonArray> results(new QJsonArray);
    QSharedPointer<int> pending(new int(requests.size()));
    QSharedPointer<bool> completed(new bool(false));

    for (const QJsonValue& value : requests) {
        const QJsonObject item = value.toObject();
        const QString method = item.value(QStringLiteral("method")).toString();
        const QJsonObject params = item.value(QStringLiteral("params")).toObject();
        const bool queued = bridgeRequest(
            method,
            params,
            15000,
            [&store, method, params, results, pending, completed, summarize, selectionCallback](const QJsonObject& response) mutable {
                QJsonObject entry{
                    {QStringLiteral("method"), method},
                    {QStringLiteral("ok"), response.value(QStringLiteral("ok")).toBool(false)},
                };
                if (response.value(QStringLiteral("ok")).toBool(false)) {
                    store.updateFromContextResponse(method, params, response);
                    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
                    if (method == QStringLiteral("selection.describe") && selectionCallback) {
                        selectionCallback(result.value(QStringLiteral("objects")).toArray());
                    }
                    entry.insert(QStringLiteral("result"), BricsCadAgentUtils::compactBrxResponseForAgent(result));
                } else {
                    entry.insert(QStringLiteral("error"),
                        BricsCadAgentUtils::bridgeErrorMessage(response, QStringLiteral("Kontextabfrage fehlgeschlagen")));
                }
                results->append(entry);
                --(*pending);
                if (*pending == 0 && !*completed) {
                    *completed = true;
                    summarize(*results);
                }
            });
        if (!queued) {
            results->append(QJsonObject{
                {QStringLiteral("method"), method},
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("BRX Kontextabfrage konnte nicht gesendet werden.")},
            });
            --(*pending);
        }
    }
    if (*pending == 0 && !*completed) {
        *completed = true;
        summarize(*results);
    }
}
