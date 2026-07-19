const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const read = (file) => fs.readFileSync(path.join(root, file), "utf8");

const cmake = read("CMakeLists.txt");
const qrc = read("resources/resources.qrc");
const page = read("src/ui/BricsCadPage.cpp");
const pageHeader = read("src/ui/BricsCadPage.h");
const brxAgent = read("src/agent/BrxAgent.cpp");
const brxPlugin = read("src/bricscad/BareboneBrxPlugin.cpp");
const scheduler = read("src/agent/LocalAiJobScheduler.cpp");
const runtime = read("src/agent/bricscad/LocalAiAgentRuntime.cpp");
const coordinator = read("src/agent/bricscad/BricsCadAgentCoordinator.cpp");
const historyAgent = read("src/agent/bricscad/ConversationHistoryAgent.cpp");
const drawingAgent = read("src/agent/bricscad/DrawingContextAgent.cpp");
const toolWorkflowAgent = read("src/agent/bricscad/ToolWorkflowAgent.cpp");
const calculationAgent = read("src/agent/bricscad/CalculationAgent.cpp");
const finalAgent = read("src/agent/bricscad/BricsCadFinalAgent.cpp");
const config = read("src/core/ConfigManager.cpp");
const html = read("index.html");
const contract = JSON.parse(read("agent/contracts/response-v2.json"));
const roomWorkflow = JSON.parse(read("agent/bricscad-workflows/grundriss_und_aussenwaende_einzeichnen.json"));

for (const file of [
    "src/agent/bricscad/BricsCadAgentCoordinator.cpp",
    "src/agent/bricscad/ConversationHistoryAgent.cpp",
    "src/agent/bricscad/DrawingContextAgent.cpp",
    "src/agent/bricscad/ToolWorkflowAgent.cpp",
    "src/agent/bricscad/CalculationAgent.cpp",
    "src/agent/bricscad/BricsCadFinalAgent.cpp",
    "src/agent/bricscad/LocalAiAgentRuntime.cpp",
]) {
    assert.ok(cmake.includes(file.replaceAll("\\", "/")), `CMake misses ${file}`);
}

assert.equal(cmake.includes("BricsCadLearningAgent"), false);
assert.equal(qrc.includes("bricscad-learning"), false);
assert.equal(page.includes("m_brxLearning"), false);
assert.equal(page.includes("qt.learning"), false);
assert.equal(page.includes("brxLearningContext"), false);
assert.equal(config.includes("toolSuccessWeights"), false);
assert.equal(page.includes("ToolSuggestions"), false);

assert.equal(page.includes("void BricsCadPage::requestBricsCadPreparationAiSlot"), false);
assert.equal(page.includes("void BricsCadPage::requestFocusedConversationContext"), false);
assert.equal(page.includes("QJsonObject BricsCadPage::deterministicWorkflowCalculationForPrompt"), false);
assert.equal(page.includes("void BricsCadPage::requestCalculationForPrompt"), false);
assert.equal(page.includes("sendUnifiedAgentRequestWithDrawingContext"), false);
assert.equal(page.includes("sendUnifiedAgentRequestWithPrefetchedContext"), false);
assert.equal(pageHeader.includes("m_aiScheduler"), false);
assert.equal(pageHeader.includes("DrawingContextStore m_drawingContextStore"), false);
assert.ok(page.includes("BricsCadAgentCoordinator::RunRequest request"));
assert.ok(page.includes("m_bricsCadCoordinator->start(request)"));
assert.ok(page.includes("BricsCadFinalAgent::buildEnvelope(finalInput)"));
assert.ok(page.includes("m_aiRuntime->enqueuePost("));

for (const slot of ["history", "drawing", "tools-workflows", "calculation"]) {
    assert.ok(coordinator.includes(`QStringLiteral("${slot}")`), `Coordinator misses ${slot} slot`);
}
assert.ok(coordinator.includes("state->completed.size() != 4 || state->finalStarted"));
assert.ok(coordinator.includes("state->finalStarted = true"));
assert.ok(coordinator.includes("parallelPreparationComplete"));
assert.ok(coordinator.includes("m_finalRouteHandler"));
assert.ok(coordinator.includes("DrawingContextStore m_drawingContextStore") || read("src/agent/bricscad/BricsCadAgentCoordinator.h").includes("DrawingContextStore m_drawingContextStore"));

assert.ok(runtime.includes("setMaxConcurrentJobs("));
assert.ok(runtime.includes("? 4 : 1"));
assert.ok(runtime.includes("QSharedPointer<std::function<void(const JsonResult&)>>"));
assert.equal(runtime.includes("[callback = std::move(callback)]"), false);
assert.ok(scheduler.includes("std::clamp(value, 1, 4)"));
assert.ok(scheduler.includes("return background ? QStringLiteral(\"background\") : QStringLiteral(\"foreground\");"));
assert.equal(scheduler.includes("return QStringLiteral(\"ai\");"), false);

assert.ok(historyAgent.includes("barebone.agent.history-slot.request.v1"));
assert.ok(historyAgent.includes("relevantMessageIndexes"));
assert.ok(historyAgent.includes("if (indexes.isEmpty() && !summary.isEmpty()"));
assert.ok(historyAgent.includes("deterministic-empty-history"));
assert.ok(drawingAgent.includes("plannedReadOnlyRequests"));
assert.ok(drawingAgent.includes("capabilitiesContainMethod"));
assert.ok(drawingAgent.includes("DrawingContextStore& store"));
assert.ok(toolWorkflowAgent.includes("BrxAgent::compactToolIndex"));
assert.ok(toolWorkflowAgent.includes("BrxAgent::toolsByNames"));
assert.ok(toolWorkflowAgent.includes("BrxAgent::selectEffectiveTools"));
assert.ok(toolWorkflowAgent.includes("deterministic-catalog-logic"));
assert.ok(toolWorkflowAgent.includes("!effective.isEmpty() || !workflowIds.isEmpty()"));
assert.ok(calculationAgent.includes("deterministicFloorPlanCalculation"));
assert.ok(calculationAgent.includes("std::sqrt(requestedAreaMm2)"));
assert.ok(calculationAgent.includes("QStringLiteral(\"wallBoxes\")"));
assert.ok(calculationAgent.includes("recentUserTexts"));
assert.ok(calculationAgent.includes("promptLooksLikeCorrectionOrContinuation"));
assert.ok(calculationAgent.includes("usedConversationForMissingDimensions"));
assert.ok(calculationAgent.includes("QStringLiteral(\"wandstaerke\")"));
assert.ok(calculationAgent.includes("QStringLiteral(\"wandhoehe\")"));
assert.ok(calculationAgent.includes("QStringLiteral(\"wallThicknessMm\")}, {QStringLiteral(\"value\"), thickness"));
assert.ok(calculationAgent.includes("QStringLiteral(\"wallHeightMm\")}, {QStringLiteral(\"value\"), height"));
assert.ok(coordinator.includes("calculationRequest.conversation = request.conversation"));

const calculationInputBlock = calculationAgent.slice(calculationAgent.indexOf("jsonRequest.input"));
assert.equal(calculationInputBlock.includes("documentContext"), false);
assert.equal(calculationInputBlock.includes("drawingContext"), false);
assert.equal(calculationInputBlock.includes("effectiveTools"), false);
assert.equal(calculationInputBlock.includes("focusedConversationContext"), false);

assert.ok(finalAgent.includes("barebone.agent.bricscad.request.v2"));
assert.ok(finalAgent.includes("bricscad-final-minimal-v1"));
assert.ok(finalAgent.includes("minimalResponseContract"));
assert.ok(finalAgent.includes("deterministicBatchDraft"));
assert.ok(finalAgent.includes("{QStringLiteral(\"effectiveTools\"), input.effectiveTools}"));
assert.ok(finalAgent.includes("{QStringLiteral(\"workflowCapsules\"), workflowCapsules}"));
assert.equal(finalAgent.includes("input.responseContract"), false);
assert.equal(finalAgent.includes("envelope.insert(QStringLiteral(\"preparedDrawingContext\")"), false);
assert.ok(finalAgent.includes("parallel:effectiveToolsAndWorkflows"));
assert.ok(finalAgent.includes("Fuehre den Workflow als direkten Batch aus"));
assert.ok(finalAgent.includes("proposal.continueAfterSuccess=false"));
assert.ok(finalAgent.includes("Beende nicht nach der Kontur oder Pruefung"));
assert.ok(finalAgent.includes("geometry.query mit autoCreatedGeometryHandlesFromBatch"));
assert.ok(page.includes("floorPlanBatchProposalFromCalculationResult"));
assert.ok(page.includes("floor_plan_interior_verification_and_walls_v1"));
assert.ok(page.includes("floorPlanBatchVerificationGatePassed"));
assert.ok(page.includes("Qt Workflow-Batch: Grundriss/Aussenwaende aus Berechnungsergebnis aufgebaut"));
assert.ok(page.includes("autoCreatedGeometryHandlesFromBatch"));
assert.ok(page.includes("createdGeometryBatch"));
assert.ok(page.includes("Erzeugte Geometrien geprueft"));
assert.ok(page.includes("Alle erzeugten Geometrien nach der Erstellung pruefen"));
assert.ok(page.includes("return QStringLiteral(\"-\");"));
assert.ok(page.includes("response.value(QStringLiteral(\"calculation\")).toObject()"));
assert.ok(page.includes("m_lastAgentRoute.value(QStringLiteral(\"calculationResult\")).toObject()"));
assert.ok(page.includes("actions.validate %1/%2 direkt pruefbare Aktion(en)"));
assert.ok(html.includes("direkt pruefbare Aktion"));
assert.ok(page.includes("const QString corePrompt = bricsCadStructuredResponse"));
assert.ok(page.includes("const QString bricsCadMinimalPrompt"));
assert.ok(page.includes("requestedOutputTokens = bricsCadStructuredResponse"));
assert.ok(page.includes("? 2048"));
assert.ok(page.includes("bricscad_finalization_retry"));
assert.ok(page.includes("barebone.agent.repair-context.compact.v1"));
assert.ok(page.includes("boundedRepairCandidateNames.size() > 4"));
assert.equal(page.includes("finalInput.responseContract"), false);
assert.equal(pageHeader.includes("responseContract;"), false);
assert.ok(page.includes("params.value(QStringLiteral(\"autoRoomHandlesFromLastQuery\")).toBool(false)"));
assert.ok(page.includes("tool == QStringLiteral(\"measurement.area\")"));
assert.ok(page.includes("tool == QStringLiteral(\"measurement.bbox\")"));

assert.equal(page.includes("nativeCommands"), false);
assert.equal(page.includes("nativeCommand"), false);
assert.equal(page.includes("command.execute"), false);
assert.equal(page.includes("commands.list"), false);
assert.equal(brxAgent.includes("command.execute"), false);
assert.equal(brxAgent.includes("commands.list"), false);
assert.equal(brxPlugin.includes("command.execute"), false);
assert.equal(brxPlugin.includes("commands.list"), false);
assert.equal(brxPlugin.includes("kBridgeCommands"), false);
assert.equal(brxPlugin.includes("POSTCOMMAND"), false);
assert.equal(brxPlugin.includes("runNativeSelectionSetCommand"), false);

assert.ok(brxAgent.includes('capabilities.value(QStringLiteral("methods"))'));
assert.ok(brxAgent.includes("runtimeToolsWithSdkTools"));
assert.equal(brxAgent.includes("tools.append(layersEnsureManyTool"), false);
assert.equal(brxAgent.includes("tools.append(bimCreateTool"), false);
assert.ok(brxAgent.includes("const int routeCap = routeName == QStringLiteral(\"validation_retry\") ? 8 : 6;"));
assert.equal(brxAgent.includes("allToolIndex"), false);
assert.equal(brxAgent.includes("allToolNames"), false);
assert.equal(brxAgent.includes("describeAllTools"), false);
assert.equal(page.includes("repairMode.allToolIndex"), false);
assert.equal(page.includes("qt.brx.tools.describe"), false);
assert.equal(page.includes("qt.brx.db.compatibility"), false);
assert.ok(page.includes("constexpr int kMaxAgentRepairRetries = 3;"));

assert.ok(html.includes("const showWorkflowButton = isChatWorkspace() || isBricsCadWorkspace();"));
assert.ok(html.includes("workflowDelete.hidden = false;"));
assert.ok(html.includes("function openBricsCadWorkflowSession(workflow)"));
assert.ok(html.includes("function createWorkflowStepParametersSection(workflow)"));
assert.ok(html.includes('table.className = "workflow-parameter-table"'));
assert.ok(html.includes('card.className = "workflow-step-card"'));
assert.equal(html.includes("function workflowStepParameterLines(workflow)"), false);

assert.equal(Object.hasOwn(contract, "learningUpdate"), false);
assert.equal(contract.routeRules.bricscad_action.allowedTypes.includes("message"), false);
assert.equal(contract.routeRules.validation_retry.allowedTypes.includes("message"), false);
assert.equal(contract.routeRules.bricscad_action.allowedTypes.includes("plan"), false);
assert.equal(contract.routeRules.validation_retry.allowedTypes.includes("plan"), false);
assert.match(contract.defaultsPolicy, /delegatedValueChoice/);
assert.equal(contract.actionProposal.actionShape.toolSource, "effectiveTools[].name");
assert.equal(contract.actionProposal.actionShape.paramsSource, "effectiveTools[].inputSchema");

assert.deepEqual(roomWorkflow.preferredTools, [
    "geometry.create",
    "geometry.query",
    "measurement.area",
    "measurement.bbox",
]);
assert.equal(roomWorkflow.steps, undefined);
assert.equal(roomWorkflow.executionBatches.length, 2);
const [verificationBatch, wallBatch] = roomWorkflow.executionBatches;
assert.equal(verificationBatch.id, "interior_contour_and_verification");
assert.deepEqual(verificationBatch.steps.map((step) => step.tool), [
    "geometry.create",
    "geometry.query",
    "measurement.area",
    "measurement.bbox",
]);
assert.equal(verificationBatch.steps[0].paramsTemplate.geometry, "rectangle");
assert.equal(verificationBatch.steps[0].paramsTemplate.saveBefore, true);
assert.equal(verificationBatch.steps[2].paramsTemplate.autoRoomHandlesFromLastQuery, true);
assert.equal(verificationBatch.steps[3].paramsTemplate.autoRoomHandlesFromLastQuery, true);
assert.equal(verificationBatch.verificationContract.expectedAreaMm2, 100000000);
assert.equal(verificationBatch.verificationContract.areaToleranceMm2, 10000);
assert.deepEqual(verificationBatch.verificationContract.expectedDimensionsMm, {
    widthX: 10000,
    depthY: 10000,
    heightZ: 0,
});
assert.equal(wallBatch.dependsOnBatch, verificationBatch.id);
assert.equal(wallBatch.requiresVerificationPassed, true);
const wallParams = wallBatch.steps.map((step) => step.paramsTemplate);
assert.deepEqual(wallParams.map((params) => params.origin), [
    { x: -300, y: -300, z: 0 },
    { x: -300, y: 10000, z: 0 },
    { x: -300, y: 0, z: 0 },
    { x: 10000, y: 0, z: 0 },
]);
assert.deepEqual(wallParams.map(({ width, depth }) => ({ width, depth })), [
    { width: 10600, depth: 300 },
    { width: 10600, depth: 300 },
    { width: 300, depth: 10000 },
    { width: 300, depth: 10000 },
]);
assert.ok(wallParams.every((params) => params.saveBefore === false));

console.log("AI native-context behavior tests passed.");
