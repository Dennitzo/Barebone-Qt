const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const cpp = fs.readFileSync(path.join(root, "src", "ui", "BricsCadPage.cpp"), "utf8");

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

console.log("AI provider and whole-message context behavior tests passed.");
