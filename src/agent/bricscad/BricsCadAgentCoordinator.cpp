#include "BricsCadAgentCoordinator.h"

#include "BricsCadAgentUtils.h"

#include <QSharedPointer>
#include <QStringList>
#include <QUuid>

BricsCadAgentCoordinator::BricsCadAgentCoordinator(LocalAiAgentRuntime& runtime, QObject* parent)
    : QObject(parent)
    , m_runtime(runtime)
{
}

DrawingContextStore& BricsCadAgentCoordinator::drawingContextStore()
{
    return m_drawingContextStore;
}

const DrawingContextStore& BricsCadAgentCoordinator::drawingContextStore() const
{
    return m_drawingContextStore;
}

void BricsCadAgentCoordinator::setCurrentGenerationProvider(std::function<int()> provider)
{
    m_currentGenerationProvider = std::move(provider);
}

void BricsCadAgentCoordinator::setBridgeRequestHandler(DrawingContextAgent::BridgeRequest handler)
{
    m_bridgeRequestHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setWorkflowLoader(ToolWorkflowAgent::WorkflowLoader loader)
{
    m_workflowLoader = std::move(loader);
}

void BricsCadAgentCoordinator::setLogHandler(std::function<void(const QString&)> handler)
{
    m_logHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setBusyHandler(std::function<void(bool)> handler)
{
    m_busyHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setParallelActiveHandler(std::function<void(bool)> handler)
{
    m_parallelActiveHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setActiveReasoningRunHandler(std::function<void(const QString&)> handler)
{
    m_activeReasoningRunHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setReasoningProgressHandler(std::function<void(const QVariantMap&)> handler)
{
    m_reasoningProgressHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setFocusedConversationHandler(std::function<void(const QJsonObject&)> handler)
{
    m_focusedConversationHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setSelectionHandler(std::function<void(const QJsonArray&)> handler)
{
    m_selectionHandler = std::move(handler);
}

void BricsCadAgentCoordinator::setFinalRouteHandler(std::function<void(const QString&, const QJsonObject&, const QJsonObject&)> handler)
{
    m_finalRouteHandler = std::move(handler);
}

bool BricsCadAgentCoordinator::isActive() const
{
    return m_active;
}

void BricsCadAgentCoordinator::cancel()
{
    m_active = false;
    if (m_parallelActiveHandler) {
        m_parallelActiveHandler(false);
    }
}

void BricsCadAgentCoordinator::appendLog(const QString& message) const
{
    if (m_logHandler) {
        m_logHandler(message);
    }
}

bool BricsCadAgentCoordinator::currentGenerationMatches(int generation) const
{
    return !m_currentGenerationProvider || m_currentGenerationProvider() == generation;
}

QString BricsCadAgentCoordinator::slotLabel(const QString& slot) const
{
    if (slot == QStringLiteral("history")) {
        return QStringLiteral("Nachrichtenverlauf");
    }
    if (slot == QStringLiteral("drawing")) {
        return QStringLiteral("Zeichnungskontext");
    }
    if (slot == QStringLiteral("tools-workflows")) {
        return QStringLiteral("Tools und Workflows");
    }
    if (slot == QStringLiteral("calculation")) {
        return QStringLiteral("Berechnungen");
    }
    return slot;
}

QString BricsCadAgentCoordinator::slotDetail(const QString& slot, const QJsonObject& result) const
{
    if (slot == QStringLiteral("history")) {
        return QStringLiteral("%1 relevante Nachrichten")
            .arg(result.value(QStringLiteral("relevantMessageIndexes")).toArray().size());
    }
    if (slot == QStringLiteral("drawing")) {
        return result.value(QStringLiteral("summary")).toString().left(120);
    }
    if (slot == QStringLiteral("tools-workflows")) {
        return QStringLiteral("%1 Tools, %2 Workflows")
            .arg(result.value(QStringLiteral("toolNames")).toArray().size())
            .arg(result.value(QStringLiteral("workflowIds")).toArray().size());
    }
    return result.value(QStringLiteral("readyForExecution")).toBool(false)
        ? QStringLiteral("Berechnungen bereit")
        : QStringLiteral("Keine oder unvollstaendige Berechnung");
}

void BricsCadAgentCoordinator::emitSlotStatus(
    const QString& runId,
    const QString& slot,
    const QString& state,
    int revision,
    const QString& message)
{
    if (!m_reasoningProgressHandler) {
        return;
    }
    m_reasoningProgressHandler(QVariantMap{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.reasoning-progress.v1")},
        {QStringLiteral("runId"), runId},
        {QStringLiteral("stageId"), slot == QStringLiteral("final-run")
            ? QStringLiteral("final-run")
            : QStringLiteral("parallel-%1").arg(slot)},
        {QStringLiteral("state"), state},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("label"), slot == QStringLiteral("final-run")
            ? QStringLiteral("Finale Auswertung")
            : slotLabel(slot)},
        {QStringLiteral("message"), message},
    });
}

QJsonArray BricsCadAgentCoordinator::selectedWorkflowObjectsForRequest(const RunRequest& request) const
{
    QJsonArray workflows;
    QSet<QString> seen;
    auto appendWorkflow = [&workflows, &seen](const QJsonObject& workflow) {
        const QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
        const QString slug = BricsCadAgentUtils::workflowSlug(id);
        if (slug.isEmpty() || seen.contains(slug)) {
            return;
        }
        workflows.append(workflow);
        seen.insert(slug);
    };
    if (!request.selectedWorkflow.isEmpty()) {
        appendWorkflow(request.selectedWorkflow);
    }
    QStringList ids = BricsCadAgentUtils::routeWorkflowIds(request.route, 3);
    ids.append(BricsCadAgentUtils::localWorkflowSelection(request.workflowIndex, request.prompt, 3));
    for (const QString& id : ids) {
        if (m_workflowLoader) {
            appendWorkflow(m_workflowLoader(id));
        }
        if (workflows.size() >= 3) {
            break;
        }
    }
    return workflows;
}

void BricsCadAgentCoordinator::start(const RunRequest& request)
{
    if (!currentGenerationMatches(request.generation)) {
        return;
    }
    const auto state = QSharedPointer<PreparationState>::create();
    state->generation = request.generation;
    state->runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state->prompt = request.prompt;
    state->documentContext = request.documentContext;
    state->route = request.route;

    m_active = true;
    if (m_parallelActiveHandler) {
        m_parallelActiveHandler(true);
    }
    if (m_busyHandler) {
        m_busyHandler(true);
    }
    if (m_activeReasoningRunHandler) {
        m_activeReasoningRunHandler(state->runId);
    }

    for (const QString& slot : {
             QStringLiteral("history"),
             QStringLiteral("drawing"),
             QStringLiteral("tools-workflows"),
             QStringLiteral("calculation"),
         }) {
        appendLog(QStringLiteral("AI Parallel Slot: slot=%1 status=running detail=Vorbereitung gestartet").arg(slot));
        emitSlotStatus(state->runId, slot, QStringLiteral("running"), 1, QStringLiteral("laeuft parallel"));
    }
    emitSlotStatus(state->runId, QStringLiteral("final-run"), QStringLiteral("queued"), 1, QStringLiteral("wartet auf vier Slots"));

    ConversationHistoryAgent::Request historyRequest;
    historyRequest.prompt = request.prompt;
    historyRequest.conversation = request.conversation;
    historyRequest.sessionId = request.sessionId;
    historyRequest.operationGeneration = request.generation;
    m_historyAgent.run(historyRequest, m_runtime, [this, state](QJsonObject result) mutable {
        finishSlot(state, QStringLiteral("history"), result);
    });

    DrawingContextAgent::Request drawingRequest;
    drawingRequest.prompt = request.prompt;
    drawingRequest.sessionId = request.sessionId;
    drawingRequest.capabilities = request.capabilities;
    drawingRequest.brxAuthenticated = request.brxAuthenticated;
    drawingRequest.operationGeneration = request.generation;
    m_drawingAgent.run(
        drawingRequest,
        m_runtime,
        m_drawingContextStore,
        m_bridgeRequestHandler,
        m_selectionHandler,
        [this, state](QJsonObject result) mutable {
            finishSlot(state, QStringLiteral("drawing"), result);
        });

    ToolWorkflowAgent::Request toolRequest;
    toolRequest.prompt = request.prompt;
    toolRequest.sessionId = request.sessionId;
    toolRequest.route = request.route;
    toolRequest.catalog = request.catalog;
    toolRequest.workflowIndex = request.workflowIndex;
    toolRequest.manualWorkflowId = request.manualWorkflowId;
    toolRequest.selectedWorkflow = request.selectedWorkflow;
    toolRequest.operationGeneration = request.generation;
    m_toolWorkflowAgent.run(
        toolRequest,
        m_runtime,
        m_workflowLoader,
        [this, state](QJsonObject result) mutable {
            finishSlot(state, QStringLiteral("tools-workflows"), result);
        });

    CalculationAgent::Request calculationRequest;
    calculationRequest.prompt = request.prompt;
    calculationRequest.sessionId = request.sessionId;
    calculationRequest.route = request.route;
    calculationRequest.conversation = request.conversation;
    calculationRequest.selectedWorkflows = selectedWorkflowObjectsForRequest(request);
    calculationRequest.workflowIndex = request.workflowIndex;
    calculationRequest.selectedWorkflowSlotValues = request.selectedWorkflowSlotValues;
    calculationRequest.operationGeneration = request.generation;
    m_calculationAgent.run(
        calculationRequest,
        m_runtime,
        m_workflowLoader,
        [this, state](QJsonObject result) mutable {
            finishSlot(state, QStringLiteral("calculation"), result);
        });
}

void BricsCadAgentCoordinator::finishSlot(
    const QSharedPointer<PreparationState>& state,
    const QString& slot,
    QJsonObject result)
{
    if (!currentGenerationMatches(state->generation) || state->completed.contains(slot)) {
        return;
    }
    state->completed.insert(slot);
    state->results.insert(slot, result);

    const bool ready = result.value(QStringLiteral("ready")).toBool(!result.contains(QStringLiteral("error")));
    const QString detail = slotDetail(slot, result);
    appendLog(QStringLiteral("AI Parallel Slot: slot=%1 status=%2 detail=%3")
        .arg(slot, ready ? QStringLiteral("ready") : QStringLiteral("fallback"), detail));
    emitSlotStatus(
        state->runId,
        slot,
        ready ? QStringLiteral("succeeded") : QStringLiteral("failed"),
        2,
        detail);

    if (state->completed.size() != 4 || state->finalStarted) {
        return;
    }
    state->finalStarted = true;

    QJsonObject finalRoute = state->route;
    finalRoute.insert(QStringLiteral("parallelPreparationComplete"), true);
    finalRoute.insert(QStringLiteral("conversationFocusAttempted"), true);
    finalRoute.insert(QStringLiteral("drawingContextPrefetchAttempted"), true);
    finalRoute.insert(QStringLiteral("calculationAttempted"), true);

    const QJsonObject history = state->results.value(QStringLiteral("history")).toObject();
    if (m_focusedConversationHandler) {
        m_focusedConversationHandler(history);
    }
    if (!history.isEmpty()) {
        finalRoute.insert(QStringLiteral("focusedConversationContext"), history);
    }

    const QJsonObject toolSelection = state->results.value(QStringLiteral("tools-workflows")).toObject();
    finalRoute.insert(QStringLiteral("effectiveToolsSelected"), true);
    finalRoute.insert(QStringLiteral("effectiveTools"), toolSelection.value(QStringLiteral("effectiveTools")).toArray());
    finalRoute.insert(QStringLiteral("effectiveToolNames"), toolSelection.value(QStringLiteral("toolNames")));
    if (!toolSelection.value(QStringLiteral("workflowIds")).toArray().isEmpty()
        && finalRoute.value(QStringLiteral("selectedWorkflows")).toArray().isEmpty()) {
        finalRoute.insert(QStringLiteral("selectedWorkflows"), toolSelection.value(QStringLiteral("workflowIds")));
    }

    finalRoute.insert(QStringLiteral("preparedDrawingContext"), state->results.value(QStringLiteral("drawing")));
    finalRoute.insert(QStringLiteral("calculationResult"), state->results.value(QStringLiteral("calculation")));
    finalRoute.insert(QStringLiteral("parallelPreparation"), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.parallel-preparation.v1")},
        {QStringLiteral("completedSlots"), QJsonArray{
            QStringLiteral("history"),
            QStringLiteral("drawing"),
            QStringLiteral("tools-workflows"),
            QStringLiteral("calculation"),
        }},
        {QStringLiteral("toolWorkflowSummary"), toolSelection.value(QStringLiteral("summary"))},
    });

    m_active = false;
    if (m_parallelActiveHandler) {
        m_parallelActiveHandler(false);
    }
    appendLog(QStringLiteral("AI Parallel Final: status=running detail=Vier Slot-Ergebnisse werden konsolidiert"));
    emitSlotStatus(state->runId, QStringLiteral("final-run"), QStringLiteral("running"), 2,
        QStringLiteral("konsolidiert die vier kompakten Ergebnisse"));

    if (m_finalRouteHandler) {
        m_finalRouteHandler(state->prompt, state->documentContext, finalRoute);
    }
}
