const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const cmakeSource = fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8");
const cpp = fs.readFileSync(path.join(root, "src", "ui", "BricsCadPage.cpp"), "utf8");
const pageHeader = fs.readFileSync(path.join(root, "src", "ui", "BricsCadPage.h"), "utf8");
const bridgeHeader = fs.readFileSync(path.join(root, "src", "ui", "AiWebBridge.h"), "utf8");
const indexHtml = fs.readFileSync(path.join(root, "index.html"), "utf8");
const brxSource = fs.readFileSync(path.join(root, "src", "bricscad", "BareboneBrxPlugin.cpp"), "utf8");
const drawingStoreSource = fs.readFileSync(path.join(root, "src", "agent", "DrawingContextStore.cpp"), "utf8");
const schedulerSource = fs.readFileSync(path.join(root, "src", "agent", "LocalAiJobScheduler.cpp"), "utf8");
const learningSource = fs.readFileSync(path.join(root, "src", "agent", "BricsCadLearningAgent.cpp"), "utf8");
const brxAgentSource = fs.readFileSync(path.join(root, "src", "agent", "BrxAgent.cpp"), "utf8");
const learningJson = fs.readFileSync(path.join(root, "agent", "bricscad-learning", "brx-learning.json"), "utf8");
const removedBrokerName = "BrxCapability" + "Broker";

function sourceSection(startMarker, endMarker) {
  const start = cpp.indexOf(startMarker);
  const end = cpp.indexOf(endMarker, start + startMarker.length);
  assert.ok(start >= 0 && end > start, `Could not isolate C++ section: ${startMarker}`);
  return cpp.slice(start, end);
}

function appendUnique(parts, value) {
  const text = typeof value === "string" ? value.trim() : "";
  if (text && !parts.includes(text)) {
    parts.push(text);
  }
}

// Behavioral reference for the provider boundary in parseModelOutput().  Fixtures
// enter as raw HTTP response bodies so JSON decoding and backslash survival are
// exercised together, rather than testing an already-decoded JavaScript object.
function parseProviderBody(rawBody, responseKind = "visible") {
  const response = JSON.parse(rawBody);
  const reasoningParts = [];
  const finalParts = [];

  const choices = Array.isArray(response.choices) ? response.choices : [];
  if (choices.length) {
    const firstChoice = choices[0] || {};
    const message = firstChoice.message || {};
    appendUnique(reasoningParts, message.reasoning);
    appendUnique(reasoningParts, message.reasoning_content);
    if (message.reasoning && typeof message.reasoning === "object") {
      appendUnique(reasoningParts, message.reasoning.content || message.reasoning.text);
    }
    appendUnique(finalParts, message.content || firstChoice.text);
  }

  if (!finalParts.length) {
    for (const item of Array.isArray(response.output) ? response.output : []) {
      const itemType = String(item.type || "").toLowerCase();
      const itemIsReasoning = itemType.includes("reasoning");
      appendUnique(itemIsReasoning ? reasoningParts : finalParts, item.text);

      for (const summary of Array.isArray(item.summary) ? item.summary : []) {
        appendUnique(
          reasoningParts,
          typeof summary === "string" ? summary : summary.text || summary.summary_text
        );
      }

      for (const content of Array.isArray(item.content) ? item.content : []) {
        const contentType = String(content.type || "").toLowerCase();
        const contentIsReasoning = itemIsReasoning
          || contentType.includes("reasoning")
          || contentType.includes("summary");
        appendUnique(
          contentIsReasoning ? reasoningParts : finalParts,
          content.text || content.output_text
        );
      }
    }
  }

  const finalText = (finalParts.length
    ? finalParts.join("\n")
    : String(response.output_text || "")).trim();
  let structuredValue = {};
  if (responseKind === "structured" && finalText) {
    try {
      const parsed = JSON.parse(finalText);
      if (parsed && !Array.isArray(parsed) && typeof parsed === "object") {
        structuredValue = parsed;
      }
    } catch (_) {
      structuredValue = {};
    }
  }
  return {
    finalText,
    reasoning: reasoningParts.join("\n\n").trim(),
    structuredValue
  };
}

function normalizedHistory(rawHistory) {
  const history = [];
  let userSeen = false;
  for (const stored of rawHistory) {
    const role = String(stored.role || "").trim().toLowerCase();
    if (role !== "user" && role !== "assistant") continue;
    if (role === "assistant" && !userSeen) continue;

    let content = String(stored.content || "").trim();
    if (!content) continue;
    if (role === "assistant") {
      try {
        const legacy = JSON.parse(content);
        if (legacy.schema === "barebone.agent.response.v2"
            && legacy.type === "message"
            && String(legacy.message || "").trim()) {
          content = String(legacy.message).trim();
        }
      } catch (_) {
        // Ordinary Markdown, including formulas, remains byte-for-byte semantic text.
      }
    } else {
      userSeen = true;
    }

    // This is also the provider-facing shape: persistence-only metadata is stripped.
    history.push({ role, content });
  }
  return history;
}

function defaultEstimate(messages) {
  return messages.reduce((total, message) => total + message.role.length + message.content.length, 0);
}

function displayedContextPercent(usedTokens, maxTokens) {
  const rawPercent = maxTokens > 0
    ? Math.max(0, Math.min(100, Math.trunc((usedTokens / maxTokens) * 100)))
    : 0;
  return usedTokens > 0 && maxTokens > 0 ? Math.max(1, rawPercent) : rawPercent;
}

function contextBudgetUsedTokens({ estimatedTokens, conversationTokens, lastRequestTokens, busy }) {
  const usedTokens = estimatedTokens >= 0 ? estimatedTokens : conversationTokens;
  return busy && estimatedTokens < 0 && lastRequestTokens > 0
    ? lastRequestTokens
    : usedTokens;
}

// Behavioral reference for buildGeneralMessagesForBudget().  The estimator is
// injectable so boundary cases can be deterministic without depending on a model's
// tokenizer; selection semantics are whole-turn and whole-message only.
function selectWholeMessageContext({
  instruction,
  currentUserContent,
  rawHistory = [],
  budget,
  prompt = currentUserContent,
  completeRequired = false,
  estimate = defaultEstimate
}) {
  const history = normalizedHistory(rawHistory);
  const messagesForIndexes = (selectedIndexes) => [
    instruction,
    ...history.filter((_, index) => selectedIndexes.has(index)),
    { role: "user", content: currentUserContent }
  ];

  let selectedIndexes = new Set();
  let messages = messagesForIndexes(selectedIndexes);
  let estimated = estimate(messages);
  if (estimated > budget || !history.length) {
    return { messages, estimated, historyMessages: 0, omittedMessages: history.length };
  }

  const allIndexes = new Set(history.map((_, index) => index));
  const allMessages = messagesForIndexes(allIndexes);
  const fullEstimate = estimate(allMessages);
  if (fullEstimate <= budget || completeRequired) {
    return {
      messages: allMessages,
      estimated: fullEstimate,
      historyMessages: history.length,
      omittedMessages: 0
    };
  }

  const turns = [];
  history.forEach((message, index) => {
    if (message.role === "user" || !turns.length) {
      turns.push({ indexes: [], searchText: "", lastIndex: -1 });
    }
    const turn = turns[turns.length - 1];
    turn.indexes.push(index);
    turn.searchText += ` ${message.content}`;
    turn.lastIndex = index;
  });

  const wordsForText = (text) => new Set(
    String(text).toLowerCase().match(/[\p{L}\p{N}_]{3,}/gu) || []
  );
  const promptWords = wordsForText(prompt);
  const turnScore = (turn) => {
    const turnWords = wordsForText(turn.searchText);
    let overlap = 0;
    for (const word of promptWords) {
      if (turnWords.has(word)) overlap += 1;
    }
    return overlap * 100000 + turn.lastIndex;
  };

  const tryAddTurn = (turn) => {
    const candidate = new Set(selectedIndexes);
    turn.indexes.forEach((index) => candidate.add(index));
    const candidateMessages = messagesForIndexes(candidate);
    const candidateEstimate = estimate(candidateMessages);
    if (candidateEstimate > budget) return false;
    selectedIndexes = candidate;
    messages = candidateMessages;
    estimated = candidateEstimate;
    return true;
  };

  const recentTurnStart = Math.max(0, turns.length - 3);
  for (let index = turns.length - 1; index >= recentTurnStart; index -= 1) {
    tryAddTurn(turns[index]);
  }
  const olderIndexes = Array.from({ length: recentTurnStart }, (_, index) => index)
    .sort((left, right) => turnScore(turns[right]) - turnScore(turns[left]));
  olderIndexes.forEach((index) => tryAddTurn(turns[index]));

  return {
    messages,
    estimated,
    historyMessages: selectedIndexes.size,
    omittedMessages: history.length - selectedIndexes.size
  };
}

function assertApiMessageShape(messages) {
  for (const message of messages) {
    assert.deepEqual(
      Object.keys(message).sort(),
      ["content", "role"],
      "Provider messages may contain only role and content"
    );
    assert.equal(Object.hasOwn(message, "messageId"), false);
  }
}

function modelFamily(model) {
  const name = String(model).toLowerCase();
  if (name.includes("gpt-oss")) return "gpt-oss";
  if (name.includes("gemma-4")) return "gemma-4";
  return "generic";
}

function reasoningEffortForModel(model, configuredEffort) {
  const normalized = String(configuredEffort || "high").trim().toLowerCase();
  if (modelFamily(model) === "gpt-oss" && normalized === "none") return "low";
  return normalized;
}

function useResponsesApiForProvider(provider, model, responseKind = "visible") {
  return String(provider).toLowerCase() === "official"
    || (responseKind === "visible" && modelFamily(model) === "gpt-oss");
}

// Provider fixtures: reasoning is separated from the visible channel and all
// backslashes survive exactly one outer JSON decode.
const mathMarkdown = String.raw`\[F=G\cdot\frac{m_1m_2}{r^2}+\rho+\theta+\beta+\nu\]`;
const gemmaBody = JSON.stringify({
  choices: [{
    finish_reason: "stop",
    message: {
      role: "assistant",
      reasoning_content: "Interne Gemma-Überlegung, nicht anzeigen.",
      content: `## Ergebnis\n\n${mathMarkdown}`
    }
  }]
});
assert.ok(gemmaBody.includes(String.raw`\\frac`), "Provider JSON must escape LaTeX backslashes");
const gemma = parseProviderBody(gemmaBody);
assert.equal(gemma.finalText, `## Ergebnis\n\n${mathMarkdown}`);
assert.equal(gemma.reasoning, "Interne Gemma-Überlegung, nicht anzeigen.");
assert.equal(gemma.finalText.includes(gemma.reasoning), false);

const gptOssBody = JSON.stringify({
  status: "completed",
  output: [
    {
      type: "reasoning",
      summary: [{ type: "summary_text", text: "Interne Harmony-Zusammenfassung." }],
      content: [{ type: "reasoning_text", text: "Verdeckte Rechenschritte." }]
    },
    {
      type: "message",
      role: "assistant",
      content: [{ type: "output_text", text: `Direkte Antwort\n\n${mathMarkdown}` }]
    }
  ]
});
const gptOss = parseProviderBody(gptOssBody);
assert.equal(gptOss.finalText, `Direkte Antwort\n\n${mathMarkdown}`);
assert.match(gptOss.reasoning, /Interne Harmony-Zusammenfassung/);
assert.match(gptOss.reasoning, /Verdeckte Rechenschritte/);
assert.doesNotMatch(gptOss.finalText, /Harmony|Rechenschritte/);

const structured = parseProviderBody(JSON.stringify({
  choices: [{ message: { content: JSON.stringify({ schema: "fixture.v1", type: "ok" }) } }]
}), "structured");
assert.deepEqual(structured.structuredValue, { schema: "fixture.v1", type: "ok" });

for (const parsed of [gemma, gptOss]) {
  assert.ok(parsed.finalText.includes(String.raw`\frac`));
  assert.ok(parsed.finalText.includes(String.raw`\rho`));
  assert.ok(parsed.finalText.includes(String.raw`\theta`));
  assert.ok(parsed.finalText.includes(String.raw`\beta`));
  assert.ok(parsed.finalText.includes(String.raw`\nu`));
  assert.equal(/[\t\r\f\v]/u.test(parsed.finalText), false, "LaTeX must not turn into controls");
}

// Model-specific reasoning behavior expected by the LM Studio adapters.
assert.equal(reasoningEffortForModel("google/gemma-4-31b-qat", "low"), "low");
assert.equal(reasoningEffortForModel("google/gemma-4-31b-qat", "medium"), "medium");
assert.equal(reasoningEffortForModel("openai/gpt-oss-20b", "none"), "low");
assert.equal(reasoningEffortForModel("openai/gpt-oss-20b", "high"), "high");
assert.equal(useResponsesApiForProvider("local", "openai/gpt-oss-20b", "visible"), true);
assert.equal(useResponsesApiForProvider("local", "openai/gpt-oss-20b", "structured"), false);
assert.equal(useResponsesApiForProvider("local", "google/gemma-4-31b-qat", "visible"), false);
assert.equal(useResponsesApiForProvider("official", "any-model", "structured"), true);

const instruction = { role: "system", content: "Antworte direkt als Markdown." };

// One prompt: no history search/focus surrogate and no synthetic history entry.
const onePrompt = selectWholeMessageContext({
  instruction,
  currentUserContent: "Berechne die Gravitation.",
  rawHistory: [{ role: "assistant", content: "Worüber möchtest du sprechen?", messageId: "greeting" }],
  budget: 10000
});
assert.deepEqual(onePrompt.messages, [
  instruction,
  { role: "user", content: "Berechne die Gravitation." }
]);
assert.equal(onePrompt.historyMessages, 0);
assertApiMessageShape(onePrompt.messages);

// Short sessions are forwarded completely. Technical roles and persistence-only
// message IDs never reach either provider API.
const formulaTurn = String.raw`\[Q=U\cdot A\cdot\Delta T\]`;
const shortSession = selectWholeMessageContext({
  instruction,
  currentUserContent: "Überprüfe alles.",
  rawHistory: [
    { role: "assistant", content: "Begrüßung", messageId: "greeting" },
    { role: "user", content: "Hier ist die Formel.", messageId: "u1" },
    { role: "assistant", content: formulaTurn, messageId: "a1" },
    { role: "thinking", content: "Technischer Eintrag", messageId: "t1" },
    { role: "user", content: "Und diese Tabelle?", messageId: "u2" },
    { role: "assistant", content: "| a | b |\n|---|---|\n| 1 | 2 |", messageId: "a2" }
  ],
  budget: 10000
});
assert.equal(shortSession.historyMessages, 4);
assert.equal(shortSession.messages[2].content, formulaTurn);
assert.equal(shortSession.messages[4].content, "| a | b |\n|---|---|\n| 1 | 2 |");
assertApiMessageShape(shortSession.messages);

const legacyMessage = "Historische Antwort als sichtbares Markdown.";
const legacySession = selectWholeMessageContext({
  instruction,
  currentUserContent: "Weiter.",
  rawHistory: [
    { role: "user", content: "Altfrage", messageId: "legacy-u" },
    {
      role: "assistant",
      content: JSON.stringify({
        schema: "barebone.agent.response.v2",
        type: "message",
        message: legacyMessage,
        sessionTitle: "Alt"
      }),
      messageId: "legacy-a"
    }
  ],
  budget: 10000
});
assert.equal(legacySession.messages[2].content, legacyMessage);
assertApiMessageShape(legacySession.messages);

// When the budget is tight, a relevant older turn is selected atomically. The
// three oversized recent turns do not cause a formula/table/code fragment to leak.
const relevantUser = "Prüfe Gravitation Mond Formeln.";
const relevantAssistant = `${mathMarkdown}\n\n| Wert | Einheit |\n|---|---|\n| F | N |`;
const huge = (label) => `${label}\n${"x".repeat(1200)}`;
const budgetHistory = [
  { role: "user", content: "Unabhängiges Gartenthema." },
  { role: "assistant", content: "Rasen und Hecke." },
  { role: "user", content: relevantUser },
  { role: "assistant", content: relevantAssistant },
  { role: "user", content: huge("Neuer Turn eins") },
  { role: "assistant", content: huge("Antwort eins") },
  { role: "user", content: huge("Neuer Turn zwei") },
  { role: "assistant", content: huge("Antwort zwei") },
  { role: "user", content: huge("Neuer Turn drei") },
  { role: "assistant", content: huge("Antwort drei") }
];
const relevantOnlyBudget = defaultEstimate([
  instruction,
  { role: "user", content: relevantUser },
  { role: "assistant", content: relevantAssistant },
  { role: "user", content: "Prüfe Gravitation und Mond vollständig." }
]);
const selected = selectWholeMessageContext({
  instruction,
  currentUserContent: "Prüfe Gravitation und Mond vollständig.",
  prompt: "Prüfe Gravitation und Mond vollständig.",
  rawHistory: budgetHistory,
  budget: relevantOnlyBudget
});
assert.deepEqual(selected.messages.slice(1, -1), [
  { role: "user", content: relevantUser },
  { role: "assistant", content: relevantAssistant }
]);
assert.equal(selected.historyMessages, 2);
assert.equal(selected.omittedMessages, 8);
assertApiMessageShape(selected.messages);

// A turn never enters partially, even if its user message alone would fit.
const atomicTurn = selectWholeMessageContext({
  instruction,
  currentUserContent: "Weiter.",
  rawHistory: [
    { role: "user", content: "Kurzer Prompt" },
    { role: "assistant", content: "A".repeat(1000) }
  ],
  budget: defaultEstimate([instruction, { role: "user", content: "Kurzer Prompt" }, { role: "user", content: "Weiter." }])
});
assert.deepEqual(atomicTurn.messages, [instruction, { role: "user", content: "Weiter." }]);

// Explicit complete-context requests return all whole messages even above budget;
// the caller can then report the size limit without silently shortening formulas.
const complete = selectWholeMessageContext({
  instruction,
  currentUserContent: "Prüfe den gesamten Workflow.",
  rawHistory: budgetHistory,
  budget: defaultEstimate([
    instruction,
    { role: "user", content: "Prüfe den gesamten Workflow." }
  ]),
  completeRequired: true
});
assert.equal(complete.historyMessages, budgetHistory.length);
assert.equal(complete.messages.length, budgetHistory.length + 2);
assert.ok(complete.estimated > defaultEstimate([
  instruction,
  { role: "user", content: "Prüfe den gesamten Workflow." }
]));

const mandatoryWorkflow = [
  "## Ausgewählter Workflow",
  String.raw`\[\dot V=30\,\mathrm{m^3/h}\]`,
  "| Schritt | Wert |",
  "|---|---|",
  "| 1 | 30 m³/h |",
  "```text",
  "vollständiger Codeblock",
  "```"
].join("\n");
const mandatory = selectWholeMessageContext({
  instruction,
  currentUserContent: mandatoryWorkflow,
  budget: 1
});
assert.equal(mandatory.messages.at(-1).content, mandatoryWorkflow);
assert.ok(mandatory.estimated > 1);

// Pin the executable behavior fixtures to the corresponding production branches.
const parserSource = sourceSection(
  "static ParsedModelOutput parseModelOutput(const QJsonObject& response",
  "QString BricsCadPage::aiChatCompletionContent"
);
for (const required of ["reasoning_content", "output_text", "reasoningParts", "finalParts"]) {
  assert.ok(parserSource.includes(required), `Production provider parser missing ${required}`);
}
assert.ok(parserSource.includes("itemIsReasoning ? reasoningParts : finalParts"));
assert.ok(parserSource.includes("contentIsReasoning ? reasoningParts : finalParts"));

const contextSource = sourceSection(
  "BricsCadPage::ContextBuildResult BricsCadPage::buildGeneralMessagesForBudget",
  "QJsonObject BricsCadPage::fallbackFocusedConversationContext"
);
for (const required of ["messagesForIndexes", "tryAddTurn", "completeConversationContext", "selectedIndexes"]) {
  assert.ok(contextSource.includes(required), `Production context selector missing ${required}`);
}
assert.equal(contextSource.includes("messageId"), false, "API history must strip messageId");
assert.equal(contextSource.includes("compressedHistorySummary"), false);
assert.equal(contextSource.includes(".left("), false, "Whole messages must never be sliced");

const sendSource = sourceSection(
  "void BricsCadPage::sendAgentEnvelope",
  "QString BricsCadPage::generalWorkflowsDirectoryPath"
);
assert.ok(sendSource.includes("general_chat"), "general_chat must use the plain path");
assert.ok(sendSource.includes("document_qa"), "document_qa must use the plain path");
assert.ok(sendSource.includes("AgentResponseKind::VisibleMarkdown"));
assert.match(
  sendSource,
  /plainGeneralResponse\s*=[\s\S]{0,400}general_chat[\s\S]{0,400}document_qa/,
  "Both general_chat and document_qa must select the same plain Markdown branch"
);

const reasoningSource = sourceSection(
  "QString reasoningEffortForModel",
  "void insertChatReasoningForModel"
);
assert.ok(reasoningSource.includes("LocalModelFamily::GptOss"));
assert.match(reasoningSource, /normalized\s*==\s*QStringLiteral\("none"\)[\s\S]*QStringLiteral\("low"\)/);
assert.equal(
  reasoningSource.includes("LocalModelFamily::Gemma4"),
  false,
  "Gemma low/medium must pass through without a forced family-specific override"
);

const insertReasoningSource = sourceSection(
  "void insertChatReasoningForModel",
  "bool useResponsesApiForProvider"
);
assert.ok(insertReasoningSource.includes("LocalModelFamily::Gemma4"));
assert.ok(insertReasoningSource.includes("LocalModelFamily::GptOss"));

// A non-empty request must never look like an empty context merely because a
// large model window makes its integer percentage smaller than one.
assert.equal(displayedContextPercent(0, 262144), 0);
assert.equal(displayedContextPercent(1, 262144), 1);
assert.equal(displayedContextPercent(1000, 262144), 1);
assert.equal(displayedContextPercent(196608, 262144), 75);
assert.equal(contextBudgetUsedTokens({
  estimatedTokens: -1,
  conversationTokens: 0,
  lastRequestTokens: 1000,
  busy: true
}), 1000);
assert.equal(contextBudgetUsedTokens({
  estimatedTokens: -1,
  conversationTokens: 250,
  lastRequestTokens: 1000,
  busy: true
}), 1000);
assert.equal(contextBudgetUsedTokens({
  estimatedTokens: -1,
  conversationTokens: 250,
  lastRequestTokens: 1000,
  busy: false
}), 250);
const contextBudgetSource = sourceSection(
  "void BricsCadPage::emitContextBudget",
  "void BricsCadPage::setReasoningEffort"
);
assert.ok(contextBudgetSource.includes("const int rawPercent"));
assert.match(
  contextBudgetSource,
  /m_agentBusy[\s\S]*estimatedTokens\s*<\s*0[\s\S]*m_lastContextBudgetUsedTokens\s*>\s*0/,
  "Background context polls must preserve the active request usage while thinking"
);
assert.match(
  contextBudgetSource,
  /usedTokens\s*>\s*0\s*&&\s*maxTokens\s*>\s*0[\s\S]*std::max\(1, rawPercent\)/,
  "Production context meter must preserve a visible non-zero percentage for positive usage"
);

const agentBrokerSource = sourceSection(
  "void BricsCadPage::continueUnifiedAgentRequest",
  "void BricsCadPage::sendUnifiedAgentRequest"
);
assert.ok(
  agentBrokerSource.includes("initialDrawingContextNeeded"),
  "BricsCAD prompts must decide whether an initial drawing prefetch is actually needed"
);
assert.ok(
  agentBrokerSource.includes("heizlast") && agentBrokerSource.includes("dokument"),
  "Future calculation/documentation agents must be allowed to trigger drawing context"
);

const toolRoutingSource = sourceSection(
  "QJsonArray BricsCadPage::availableAgentToolsForRoute",
  "QJsonArray BricsCadPage::readOnlyMethodsForRoute"
);
assert.ok(cpp.includes("BrxAgent::buildToolCatalog"));
assert.ok(toolRoutingSource.includes("BrxAgent::selectToolsForRoute"));
assert.equal(cpp.includes(`${removedBrokerName}::`), false, "BricsCadPage must route through BrxAgent, not the removed broker");
assert.equal(brxAgentSource.includes(removedBrokerName), false, "BrxAgent must contain migrated broker logic directly");
assert.ok(cpp.includes("tableMarkdownPolicy"), "BricsCAD geometry table output must carry a Markdown table policy");
assert.ok(cpp.includes("Tabellenzeilen niemals als Einzeiler zusammenkleben"));
assert.ok(brxSource.includes('"circle.extrude"'), "BRX runtime capabilities must expose circle.extrude");
assert.ok(brxSource.includes("countCircleProfiles"), "BRX validation must have circle-specific extrusion checks");
assert.ok(brxSource.includes('method == "circle.extrude"'), "BRX dispatcher must route circle.extrude");
assert.ok(brxSource.includes("selectorMatchTokensForEntity"), "BRX selector filters must use normalized entity match tokens");
assert.ok(brxSource.includes("selectorAnyTokenMatches"), "BRX selector array filters must match canonical aliases");
assert.ok(brxSource.includes("AcDbCircle::cast(entity)"), "BRX selector filters must recognize AcDbCircle as circle");
assert.ok(brxSource.includes('state.plannedLastResultKind = "CIRCLE"'), "Batch preflight must recognize geometry.create circle as planned circle lastResult");
assert.ok(brxSource.includes("normalizedSelectorScope"), "BRX selector scopes must be normalized before validation/resolution");
assert.ok(brxAgentSource.includes("circle.extrude"), "BrxAgent must make circle.extrude selectable");
assert.ok(brxAgentSource.includes('selectedToolNames << QStringLiteral("profile.extrude")'), "Circle extrusion routing must include profile.extrude fallback when selectedTools are prefilled");
assert.ok(
  brxAgentSource.includes('circleExtrudeIntent && (name == QStringLiteral("circle.extrude") || name == QStringLiteral("profile.extrude"))'),
  "Circle extrusion routing must keep profile.extrude available as generic profile fallback"
);
assert.ok(cpp.includes('tool == QStringLiteral("circle.extrude")'), "Qt must treat circle.extrude as an extrusion action");
assert.ok(cpp.includes("profileCircleSelector"), "Qt proposal validation must allow profile.extrude circle fallback without forcing a target layer");
assert.ok(cpp.includes("Wenn circle.extrude nicht in effectiveTools vorhanden ist"), "AI policy must tell local models to use profile.extrude fallback instead of asking for manual extrusion");
assert.ok(cpp.includes('params.contains(QStringLiteral("z"))'), "Qt must normalize z to heightMm for extrusion actions");
assert.ok(brxSource.includes('jsonDoubleProperty(paramsJson, "z")'), "BRX extrusion validation/execution must accept z as height alias");
for (const removedBrokerFile of [
  path.join("src", "agent", `${removedBrokerName}.cpp`),
  path.join("src", "agent", `${removedBrokerName}.h`)
]) {
  assert.equal(fs.existsSync(path.join(root, removedBrokerFile)), false, `Removed broker file still exists: ${removedBrokerFile}`);
  assert.equal(cmakeSource.includes(removedBrokerFile.replaceAll(path.sep, "/")), false, `CMake still compiles removed broker file: ${removedBrokerFile}`);
}
assert.ok(brxAgentSource.includes("compatibleGeometry"));
assert.ok(brxAgentSource.includes("return filtered;"));
assert.ok(brxAgentSource.includes("repairToolContext"), "BrxAgent must expose a full repair tool context");
assert.ok(brxAgentSource.includes("allToolIndex"), "Repair mode must expose the full compact tool index");
assert.ok(brxAgentSource.includes("candidateGroups"), "Repair mode must group alternative tool candidates");
assert.ok(brxAgentSource.includes("describeAllTools"), "Repair mode must expose paged full-tool describe access");
assert.ok(brxAgentSource.includes('params.value(QStringLiteral("all")).toBool(false)'), "qt.brx.tools.describe must support all=true");
assert.ok(brxAgentSource.includes('params.value(QStringLiteral("cursor"))'), "qt.brx.tools.describe must support cursor paging");
assert.ok(brxAgentSource.includes('params.value(QStringLiteral("includeSchemas"))'), "qt.brx.tools.describe must allow schema-heavy output to be controlled");
assert.ok(cpp.includes("repairMode"), "Repair envelopes must include repairMode context");
assert.ok(cpp.includes("BrxAgent::repairToolContext"), "BricsCadPage must build repair context through BrxAgent");
assert.ok(cpp.includes("qt.brx.db.compatibility"), "Repair instructions must route ambiguous tool choice through db compatibility");
assert.ok(cpp.includes("qt.brx.tools.describe names=[...] oder all=true"), "Repair instructions must expose full toolcard paging to the AI");
assert.equal(
  toolRoutingSource.includes("Q_UNUSED(filtered)"),
  false,
  "Filtered tool details must not be discarded"
);
assert.equal(fs.existsSync(path.join(root, "BRX_CAPABILITIES.md")), false, "Manual BRX_CAPABILITIES.md must be removed");
assert.equal(cpp.includes("toolProfile("), false, "BricsCadPage must not enrich tools from learning toolProfiles");
assert.equal(learningJson.includes('"toolProfiles"'), false, "brx-learning.json must not store toolProfiles");
assert.ok(learningJson.includes('"id": "workflow_brx_selector_reference"'), "Learning JSON must contain protected BRX selector reference workflow");
assert.ok(learningJson.includes('"title": "BRX Selector Referenz"'), "Selector reference workflow must be visible by title");
assert.match(
  learningJson,
  /"id": "workflow_brx_selector_reference"[\s\S]*?"updateProtected": true/,
  "Selector reference workflow must be read-only"
);
assert.match(
  learningJson,
  /"id": "workflow_brx_selector_reference"[\s\S]*?"source": "canonical_brx_workflow"/,
  "Selector reference workflow must be a canonical BRX workflow"
);
assert.match(
  learningJson,
  /"selectorScenarios"[\s\S]*?"scenario": "explicit_handle"[\s\S]*?"scenario": "current_selection"[\s\S]*?"scenario": "created_circle_lastResult"[\s\S]*?"scenario": "post_extrusion_lastExtruded"[\s\S]*?"scenario": "current_space_read_context"/,
  "Selector reference workflow must document selector scenarios with matching commands"
);
const selectorReferenceLesson = JSON.parse(learningJson).lessons
  .find((lesson) => lesson.id === "workflow_brx_selector_reference");
const circleSelectorScenario = selectorReferenceLesson.selectorScenarios
  .find((scenario) => scenario.scenario === "created_circle_lastResult");
assert.equal(
  circleSelectorScenario.fallbackCommand,
  "profile.extrude",
  "Selector reference workflow must document profile.extrude fallback for circle extrusion"
);
assert.ok(
  !learningJson.includes("ai_circle.extrude_anwenden"),
  "Faulty runtime circle-extrude workflow must be removed from learning context"
);
assert.ok(learningSource.includes("selectorScenarios"), "Compact BRX lessons must expose selectorScenarios to the AI");
assert.ok(brxSource.includes("namespace BrxToolRegistry"), "BRX descriptors must live in BrxToolRegistry");
assert.equal(brxSource.includes("jsonCapabilitiesResponse("), false, "Old static capability response must be removed");

const localContextSource = sourceSection(
  "QJsonObject BricsCadPage::localAgentContextResponse",
  "void BricsCadPage::handleAgentContextRequest"
);
for (const method of [
  "qt.tools.describe",
  "qt.brx.tools.describe",
  "qt.brx.db.schema",
  "qt.brx.db.query",
  "qt.brx.db.inspect",
  "qt.brx.db.fullContext",
  "qt.brx.db.compatibility",
  "qt.brx.context.manifest",
  "qt.brx.context.fetch",
  "qt.brx.execution.history",
  "qt.brx.workflow.testPlan",
  "qt.brx.workflow.repairHints",
  "qt.brx.workflow.repairContext",
  "qt.learning.describe",
  "qt.documents.describe",
  "qt.drawing.manifest",
  "qt.drawing.query",
  "qt.drawing.fullScan"
]) {
  assert.ok(localContextSource.includes(method), `Missing local context method ${method}`);
}
assert.ok(cpp.includes("DrawingContextStore m_drawingContextStore")
  || fs.readFileSync(path.join(root, "src", "ui", "BricsCadPage.h"), "utf8").includes("DrawingContextStore m_drawingContextStore"));

// Drawing state is deliberately prompt-driven. A BricsCAD turn starts with an
// empty store/selection, then fills them only from explicit read-only requests.
const promptEntrySource = sourceSection(
  "void BricsCadPage::sendAgentPrompt",
  "void BricsCadPage::continueUnifiedAgentRequest"
);
assert.ok(promptEntrySource.includes("isBricsCadMode()"));
assert.ok(promptEntrySource.includes("m_drawingContextStore.clear()"));
assert.ok(promptEntrySource.includes("m_currentSelection = {}"));

const drawingPrefetchSource = sourceSection(
  "void BricsCadPage::sendUnifiedAgentRequestWithPrefetchedContext",
  "static ParsedModelOutput parseModelOutput"
);
assert.ok(drawingPrefetchSource.includes("selection.describe"));
assert.ok(drawingPrefetchSource.includes("m_drawingContextStore.updateFromContextResponse"));
assert.match(
  drawingPrefetchSource,
  /method\s*==\s*QStringLiteral\("selection\.describe"\)[\s\S]*?m_currentSelection\s*=\s*result\.value\(QStringLiteral\("objects"\)\)\.toArray\(\)/,
  "selection.describe must populate the turn-local selection from its current BRX response"
);
assert.match(
  drawingStoreSource,
  /method\s*==\s*QStringLiteral\("selection\.describe"\)[\s\S]*?m_selection\s*=\s*selection/,
  "DrawingContextStore must ingest explicit selection.describe results"
);
for (const token of [
  "contextManifest()",
  "fetch(const QJsonObject& params)",
  "inspect(const QJsonObject& params)",
  "fullContext(const QJsonObject& params)",
  "executionHistory(const QJsonObject& params)",
  "repairContext(const QJsonObject& params)",
  "compactObjectFact",
  "m_normalizedFacts",
  "m_capabilityBlocks"
]) {
  assert.ok(drawingStoreSource.includes(token), `DrawingContextStore missing full-context support token ${token}`);
}

assert.ok(schedulerSource.includes("std::clamp(value, 1, 4)"));
assert.ok(schedulerSource.includes("abortOldestCancellableBackground"));
assert.ok(schedulerSource.includes("activeForeground"));
assert.ok(cpp.includes("m_config.aiProvider() == QStringLiteral(\"local\") ? 4 : 1"));
assert.equal(cpp.includes("m_aiNetwork->post"), false, "AI POST requests must go through LocalAiJobScheduler");
assert.ok(bridgeHeader.includes("agentRuntimeStatusChanged"));
assert.ok(indexHtml.includes("function applyAgentRuntimeStatus"));
assert.ok(pageHeader.includes("QVector<DeferredAgentPrompt> m_deferredAgentPrompts"));
assert.ok(cpp.includes("AI Runtime: Foreground-Prompt eingereiht"));
assert.equal(cpp.includes("void BricsCadPage::resolvePendingGeometryMoveResponse"), false);
assert.equal(cpp.includes('QStringLiteral("geometry.move.status")'), false);
assert.ok(brxAgentSource.includes("brx.sdk.entity.transformBy"));
assert.ok(brxAgentSource.includes("brx.sdk.blockReference.setPosition"));
assert.ok(brxAgentSource.includes("brx.sdk.assoc.evaluate"));
for (const sdkAlias of [
  "brx.sdk.entity.copy",
  "brx.sdk.entity.rotateBy",
  "brx.sdk.entity.scaleBy",
  "brx.sdk.entity.erase",
  "brx.sdk.entity.setLayer",
  "brx.sdk.entity.setName",
  "brx.sdk.selection.setPickfirst",
  "brx.sdk.bim.classification.set"
]) {
  assert.ok(brxAgentSource.includes(sdkAlias), `BrxAgent missing SDK alias ${sdkAlias}`);
}
for (const validationMapping of [
  'return QStringLiteral("geometry.copy");',
  'return QStringLiteral("geometry.rotate");',
  'return QStringLiteral("geometry.scale");',
  'return QStringLiteral("geometry.delete");',
  'return QStringLiteral("entity.setLayer");',
  'return QStringLiteral("entity.setName");',
  'return QStringLiteral("selection.set");',
  'return QStringLiteral("bim.classify");'
]) {
  assert.ok(cpp.includes(validationMapping), `Qt missing SDK validation mapping ${validationMapping}`);
}

for (const removedFile of [
  path.join("src", "agent", "DrawingActivityTracker.cpp"),
  path.join("src", "agent", "DrawingActivityTracker.h"),
  path.join("src", "agent", "InteractiveDrawingLearningAgent.cpp"),
  path.join("src", "agent", "InteractiveDrawingLearningAgent.h")
]) {
  assert.equal(
    fs.existsSync(path.join(root, removedFile)),
    false,
    `Removed realtime drawing-learning file still exists: ${removedFile}`
  );
  assert.equal(
    cmakeSource.includes(removedFile.replaceAll(path.sep, "/")),
    false,
    `CMake still compiles removed realtime drawing-learning file: ${removedFile}`
  );
}

const qtRealtimeSources = `${cpp}\n${pageHeader}`;
for (const removedSymbol of [
  "DrawingActivityTracker",
  "InteractiveDrawingLearningAgent",
  "m_drawingContextPoll",
  "refreshDrawingContextSnapshot",
  "scheduleDrawingContextPoll",
  "enqueueDrawingLearningJob",
  "observeInteractiveDrawingLearning",
  "DrawingLearning",
  "drawing-learning:",
  "m_brxDrawingLifecycleActive"
]) {
  assert.equal(
    qtRealtimeSources.includes(removedSymbol),
    false,
    `Qt still contains removed realtime drawing-learning symbol: ${removedSymbol}`
  );
}

for (const [label, source] of [
  ["CMake", cmakeSource],
  ["Qt", qtRealtimeSources],
  ["learning agent", learningSource],
  ["learning JSON", learningJson],
  ["overlay", indexHtml]
]) {
  assert.equal(source.includes("drawing_ai_runtime"), false, `${label} still knows drawing_ai_runtime`);
}

for (const removedBrxSymbol of [
  "AcEditorReactor",
  "AcDbDatabaseReactor",
  "AcApDocManagerReactor",
  "BridgeEditorReactor",
  "BridgeDatabaseReactor",
  "BridgeDocumentReactor",
  "sendCommandActivityEvent",
  "selection.changed",
  "command.started",
  "command.ended",
  "object.appended",
  "object.modified",
  "object.erased",
  "g_drawingLifecycleActive",
  "detailsDeferred",
  "BBTRACK"
]) {
  assert.equal(
    brxSource.includes(removedBrxSymbol),
    false,
    `BRX still contains removed realtime command/database/selection tracking symbol: ${removedBrxSymbol}`
  );
}

assert.ok(brxSource.includes("isQuiescent()"), "Prompt-driven BRX requests must require a quiescent document");
assert.ok(brxSource.includes("activeDocumentReadyForPromptRequest(lifecycleError)"));
assert.ok(brxSource.includes("autoBimHandlesFromLastQuery"));
assert.ok(brxSource.includes("hasBimValidationArtifacts"));
assert.ok(brxSource.includes("validateMoveTargetDatabaseState"));
assert.equal(brxSource.includes("struct GeometryMoveJob"), false);
assert.equal(brxSource.includes("std::string geometryMoveStatusBridgeResponse"), false);
assert.equal(brxSource.includes('if (method == "geometry.move.status")'), false);
assert.equal(brxSource.includes('"method":"geometry.move.status"'), false);
assert.equal(brxSource.includes('_T("BBGEOMETRYMOVE ")'), false);
assert.equal(brxSource.includes("static void BareboneQtBBGEOMETRYMOVE()"), false);
assert.ok(brxSource.includes("validateGeometryMoveReadback"));
assert.ok(brxSource.includes("sameMoveFingerprint"));
assert.ok(brxSource.includes("AcDbAssocManager::evaluateTopLevelNetwork"));
assert.ok(brxSource.includes('"brx.sdk.assoc.evaluate"'));
assert.ok(brxSource.includes("BRXASSOCEVALUATE"));
assert.ok(brxSource.includes("evaluateAssocNetworkInApplicationContext"));
assert.ok(brxSource.includes("moveEntityWithBrxSdk"));
assert.ok(brxSource.includes("AcDbBlockReference::setPosition"));
assert.ok(brxSource.includes("AcDbEntity::transformBy"));
assert.ok(brxSource.includes('transformationVerified\\":true'));
for (const removedMoveSymbol of [
  "geometry.move.status",
  "BBGEOMETRYMOVE",
  "GeometryMoveJob",
  "g_geometryMoveJobs",
  "queueGeometryMoveJob",
  "runNativeManipMoveCommand",
  "ScopedShortSysVar",
  "hostSurfaceHint",
  "hostSurfaces",
  "anchorSearch",
  "appendHostSurfacesJson",
  "appendAnchorSearchJson",
  "hintedAnchorPointAfterValid",
  "doorWindowAnchorRepair",
  "expectedAnchorHostHandleAfter",
  "doorWindowAnchorCandidatePoints",
  "queryAnchorCandidateForDoorWindow",
  "BimApi::createAnchoredBlockReference",
  "BimApi::unAnchorBlockReference",
  "acceptedDoorWindowAnchorRepair"
]) {
  assert.equal(brxSource.includes(removedMoveSymbol), false, `BRX still contains removed BIM move symbol: ${removedMoveSymbol}`);
  assert.equal(cpp.includes(removedMoveSymbol), false, `Qt still contains removed BIM move symbol: ${removedMoveSymbol}`);
  assert.equal(learningJson.includes(removedMoveSymbol), false, `Learning JSON still contains removed BIM move symbol: ${removedMoveSymbol}`);
}
assert.match(
  brxSource,
  /result\.tool\s*==\s*"geometry\.move"[\s\S]{0,1800}?autoBimHandlesFromLastQuery[\s\S]{0,1800}?ResolvePurpose::DrawingMutation/,
  "geometry.move validation must bind auto BIM query handles through DrawingMutation resolution"
);
assert.equal(learningJson.includes("workflow_bim_move"), false, "Fixed BIM move workflow must be removed from learning JSON");
assert.equal(learningJson.includes("MANIP_MOVE"), false, "Learning JSON must not hardcode MANIP_MOVE");
assert.equal(cpp.includes("QJsonArray BricsCadPage::compactToolSelectorList"), false);
assert.equal(cpp.includes("QJsonArray BricsCadPage::agentToolsByNames"), false);

console.log("AI provider and whole-message context behavior tests passed.");
