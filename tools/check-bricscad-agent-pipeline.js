const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");
const page = read("src/ui/BricsCadPage.cpp");
const brxAgent = read("src/agent/BrxAgent.cpp");
const toolWorkflow = read("src/agent/bricscad/ToolWorkflowAgent.cpp");
const finalAgent = read("src/agent/bricscad/BricsCadFinalAgent.cpp");
const drawingAgent = read("src/agent/bricscad/DrawingContextAgent.cpp");
const calculationAgent = read("src/agent/bricscad/CalculationAgent.cpp");
const coordinator = read("src/agent/bricscad/BricsCadAgentCoordinator.cpp");
const webUi = read("index.html");

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const actionBranchStart = page.indexOf('if (type == "action_proposal")');
const contextBranchStart = page.indexOf('if (type == "context_request")', actionBranchStart);
const actionBranch = page.slice(actionBranchStart, contextBranchStart);

assert(actionBranchStart >= 0 && contextBranchStart > actionBranchStart,
  "BricsCAD action_proposal branch not found");
assert(actionBranch.includes("processAgentProposal(content, reply, proposal"),
  "action_proposal must enter structural and capability validation before execution");
assert(!actionBranch.includes("normalizedRectangularRoomWallProposal("),
  "active proposal path must not call the rectangular room normalizer");
assert(!actionBranch.includes("floorPlanBatchProposalFromCalculationResult("),
  "active proposal path must not replace the AI proposal with a deterministic floor-plan batch");
assert(!actionBranch.includes("promptProposalConflictMessage("),
  "active proposal path must not use prompt keyword conflict rules");
assert(page.includes("void BricsCadPage::processAgentProposal("),
  "proposals need an explicit processing path");
assert(!toolWorkflow.includes("BricsCadAgentUtils::localWorkflowSelection("),
  "ToolWorkflowAgent must not replace AI selection with local workflow keyword ranking");
assert(toolWorkflow.includes('QStringLiteral("fullCapabilityFallback")'),
  "tool selection fallback must expose capabilities instead of a deterministic answer");
assert(!finalAgent.includes('envelope.insert(QStringLiteral("deterministicBatchDraft")'),
  "FinalAgent must not inject deterministic workflow batches");
assert(brxAgent.includes('dimensionSemantics'),
  "geometry tools must publish dimension-axis semantics");
assert(brxAgent.includes('Massangaben x/y/z bedeuten Breite/Laenge/Hoehe'),
  "geometry.create must distinguish dimensions from coordinate containers");
assert(page.includes('moveDimensionAlias(QStringLiteral("widthMm"), QStringLiteral("width"))'),
  "geometry.create must normalize the width unit alias");
assert(page.includes('moveDimensionAlias(QStringLiteral("x"), QStringLiteral("width"))'),
  "top-level x dimensions must map to width");
assert(page.includes('x/y/z innerhalb von origin, point, position, coordinates, center oder vector'),
  "repair policy must preserve coordinate semantics");
assert(coordinator.includes('QStringLiteral("AI-Berechnung wird erfasst und aufbereitet")'),
  "calculation slot must use the user-facing preparation status text");
assert(coordinator.includes('const int completionRevision = slot == QStringLiteral("calculation") ? 3 : 2;'),
  "calculation completion must advance its reasoning revision so the UI can render the checkmark");
assert(calculationAgent.includes('object.insert(QStringLiteral("numericFacts"), promptNumericFacts);'),
  "calculation results must preserve numeric facts recognized before the AI run");
assert(coordinator.includes('QStringLiteral("%1 erkannt und aufbereitet")'),
  "calculation thinking status must expose recognized numeric values");
assert(finalAgent.includes('For type=message, message must contain the complete visible answer.'),
  "final BricsCAD messages must not be allowed to complete without visible content");
assert(page.includes('type=message enthaelt keine sichtbare message.'),
  "empty final messages must enter the repair path instead of silently completing");
assert(page.includes('type=plan enthaelt keine sichtbare message.'),
  "empty plans must be repaired into a visible answer for drawing questions");
assert(page.includes('{QStringLiteral("revision"), 3 + m_agentValidationRetries}'),
  "final-answer repair updates must advance the shared thinking revision");
assert(page.includes('{QStringLiteral("revision"), 4 + m_agentValidationRetries}'),
  "proposal completion after a repair must advance beyond its running revision");
assert(page.includes('m_pendingAgentDraft.value(QStringLiteral("_questionMessage"))'),
  "discarding a question must remove its pending conversation context");
assert(webUi.includes('discard.textContent = uiLanguage === "en" ? "Discard" : "Verwerfen";'),
  "non-runnable question cards must provide a discard button");
assert(webUi.includes('typeof bridge.clearProposal === "function"'),
  "discarding a question must notify Qt to reset the pending draft");
const executionWorkflowStart = page.indexOf('QJsonObject BricsCadPage::workflowFromBricsCadExecution');
assert(page.indexOf('proposal.value(QStringLiteral("intentSummary"))', executionWorkflowStart)
    < page.indexOf('title = workflowTitleSeed(sourcePrompt);', executionWorkflowStart),
  "workflow save must prefer the AI intent title over the complete source prompt");
assert(page.includes('{QStringLiteral("circle"), QStringLiteral("Kreis-Geometrie erstellen")}'),
  "workflow identity must use a reusable geometry topic instead of concrete dimensions");
assert(page.includes('ask_user.message fehlt. Formuliere eine kurze direkte deutsche Rueckfrage'),
  "ask_user replies without a user-facing question must be repaired by the AI");
assert(page.includes('setAgentWaitingForUser(QJsonObject{{QStringLiteral("message"), question}'),
  "ask_user must render the legacy green question card");
assert(page.includes('discardLastAssistantConversation(content);\n        m_agentConversation.append(QJsonObject{'),
  "ask_user must replace raw response JSON with the visible question in conversation context");
assert(!page.includes('setAgentWaitingForUser(reply);'),
  "ask_user must not render the legacy green question card");
assert(page.includes('request.value(QStringLiteral("request")).toObject()'),
  "context requests must accept the response-v2 request wrapper");
assert(page.includes('method = QStringLiteral("selection.describe")'),
  "selection domain context requests must resolve to selection.describe");
assert(drawingAgent.includes('object.insert(QStringLiteral("selection"), selectionFacts);'),
  "drawing context must preserve authoritative selection facts outside AI compression");
assert(!page.includes('Der Agent wartet auf weitere Angaben.'),
  "generic hidden-detail waiting text must not be shown");
assert(webUi.includes('session.messages = session.messages.filter((message) => message?.kind !== "proposal_context")'),
  "green proposal cards must be removed from persisted session messages");
assert(webUi.includes('activeSession().proposal = storedProposal;\n      activeSession().activeProposalMessageId = "";\n      renderProposal(storedProposal);'),
  "green proposal cards must use the transient inline renderer");
assert(!webUi.includes('session.messages.push(proposalMessage);'),
  "green proposal cards must not be appended to chat history");
assert(webUi.includes('const liveThinkingIndicator = isBusy'),
  "session rendering must preserve the live thinking DOM node");
assert(webUi.includes('messages.appendChild(liveThinkingIndicator);'),
  "preserved thinking state must be reattached after an unavoidable render");
assert(webUi.includes('activeSession().proposal = storedProposal;\n      activeSession().activeProposalMessageId = "";\n      renderProposal(storedProposal);'),
  "terminal proposal rendering must preserve the shared thinking run before showing the transient card");
assert(webUi.includes('// Terminal Qt signals can arrive after a proposal/message was rendered.'),
  "idle must finalize an orphaned transient thinking indicator");
console.log("BricsCAD AI-planning pipeline checks OK");
