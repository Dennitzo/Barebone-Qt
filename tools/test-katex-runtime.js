const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const html = fs.readFileSync(path.join(root, "index.html"), "utf8");
const katex = require(path.join(
  root,
  "resources",
  "vendor",
  "katex",
  "0.16.10",
  "katex.min.js"
));

function extractedFunction(name, nextName) {
  const start = html.indexOf(`function ${name}`);
  const end = html.indexOf(`function ${nextName}`, start);
  assert.ok(start >= 0 && end > start, `Could not extract ${name} from index.html`);
  return new Function(`${html.slice(start, end)}; return ${name};`)();
}

function extractedFunctions(firstName, nextName, names, dependencies = {}) {
  const start = html.indexOf(`function ${firstName}`);
  const end = html.indexOf(`function ${nextName}`, start);
  assert.ok(start >= 0 && end > start, `Could not extract ${firstName} block from index.html`);
  const dependencyNames = Object.keys(dependencies);
  return new Function(
    ...dependencyNames,
    `${html.slice(start, end)}; return { ${names.join(", ")} };`
  )(...dependencyNames.map((name) => dependencies[name]));
}

const katexStrictMode = extractedFunction("katexStrictMode", "normalizedMathParts");
const normalizedMathParts = extractedFunction("normalizedMathParts", "cachedKatexHtml");

assert.equal(katexStrictMode("unicodeTextInMathMode"), "ignore");
assert.equal(katexStrictMode("unknownSymbol"), "error");

[
  [String.raw`\(\rho+\theta+\beta+\nu\)`, false, String.raw`\rho+\theta+\beta+\nu`],
  [String.raw`\[\frac{a}{b}\]`, true, String.raw`\frac{a}{b}`],
  ["$F=ma$", false, "F=ma"],
  ["$$F=ma$$", true, "F=ma"]
].forEach(([source, display, tex]) => {
  const parsed = normalizedMathParts(source);
  assert.equal(parsed.source, source);
  assert.equal(parsed.display, display);
  assert.equal(parsed.tex, tex);
  assert.doesNotThrow(() => katex.renderToString(parsed.tex, {
    displayMode: parsed.display,
    throwOnError: true,
    strict: katexStrictMode,
    trust: false
  }));
});

const gptOssWorkFormulaSources = [
  `\\(\\boxed{1.65\\times10^{3}\\;\\text{J\u202fm}^{-3}}\\)`,
  `\\(1.65\\times10^{3}\\;\\text{J\u202fm}^{-3}\\)`
];
for (const source of gptOssWorkFormulaSources) {
  const parsed = normalizedMathParts(source);
  assert.equal(parsed.source, source, "Unicode-space normalization must not rewrite stored math source");
  assert.equal(parsed.tex.includes("\u202f"), false);
  assert.match(parsed.tex, /\\text\{J\\,m\}/);
  assert.doesNotThrow(() => katex.renderToString(parsed.tex, {
    displayMode: parsed.display,
    throwOnError: true,
    strict: katexStrictMode,
    trust: false
  }));
}

const normalizedWhitespaceWithInvalidCommand = normalizedMathParts(
  `\\(\\text{J\u202fm}\\notARealKatexCommand{x}\\)`
);
assert.equal(normalizedWhitespaceWithInvalidCommand.tex.includes("\u202f"), false);
assert.throws(() => katex.renderToString(normalizedWhitespaceWithInvalidCommand.tex, {
  throwOnError: true,
  strict: katexStrictMode,
  trust: false
}), /Undefined control sequence/);

const unsupportedZeroWidthSpace = normalizedMathParts(`\\(\\text{J\u200bm}\\)`);
assert.equal(unsupportedZeroWidthSpace.tex.includes("\u200b"), true);
assert.throws(() => katex.renderToString(unsupportedZeroWidthSpace.tex, {
  throwOnError: true,
  strict: katexStrictMode,
  trust: false
}), /unknownSymbol/);

for (const [space, expected] of [["\u00a0", "~"], ["\u2009", String.raw`\,`]]) {
  const source = `\\(\\text{J${space}m}\\)`;
  const parsed = normalizedMathParts(source);
  assert.equal(parsed.source, source);
  assert.equal(parsed.tex, `\\text{J${expected}m}`);
  assert.doesNotThrow(() => katex.renderToString(parsed.tex, {
    throwOnError: true,
    strict: katexStrictMode,
    trust: false
  }));
}

const gemmaFormulaTex = String.raw`Zähler = (6,674 \times 10^{-11}\text{ m}^3 \cdot \text{kg}^{-1} \cdot \text{s}^{-2}) \cdot (5,972 \times 10^{24}\text{ kg}) \cdot (7,348 \times 10^{22}\text{ kg})`;
const gemmaResultTex = String.raw`Zähler \approx 2,928 \times 10^{37}\text{ kg} \cdot \text{m}^3 \cdot \text{s}^{-2}`;
for (const tex of [gemmaFormulaTex, gemmaResultTex]) {
  assert.throws(() => katex.renderToString(tex, {
    displayMode: true,
    throwOnError: true,
    strict: "error",
    trust: false
  }), /unicodeTextInMathMode/);
  assert.doesNotThrow(() => katex.renderToString(tex, {
    displayMode: true,
    throwOnError: true,
    strict: katexStrictMode,
    trust: false
  }));
}

const jsonRoundTrip = String.raw`\frac{m_1m_2}{r^2}+\rho+\theta+\beta+\nu`;
assert.equal(JSON.parse(JSON.stringify({ content: jsonRoundTrip })).content, jsonRoundTrip);

assert.throws(() => katex.renderToString(String.raw`\notARealKatexCommand{x}`, {
  throwOnError: true,
  strict: "error",
  trust: false
}));
assert.throws(() => katex.renderToString("Q \\", {
  displayMode: true,
  throwOnError: true,
  strict: "error",
  trust: false
}));

assert.match(html, /\(\^\|\[\^\\\\\$\]\)\\\$\\\$/);
assert.match(html, /function isInlineDollarMath/);
assert.match(html, /message-inline-code/);
assert.match(html, /math-source-text\.fallback/);

const isInlineDollarMath = extractedFunction("isInlineDollarMath", "appendInlineMarkdown");
const normalizeMathTextForMarkdown = extractedFunction(
  "normalizeMathTextForMarkdown",
  "appendMarkdownContent"
);
const { normalizeLooseMarkdownTables } = extractedFunctions(
  "splitMarkdownTableRow",
  "normalizeMarkdownHeading",
  ["normalizeLooseMarkdownTables"],
  {
    protectMarkdownSegmentsForStructure: (value) => ({
      text: String(value || ""),
      segments: []
    }),
    restoreMarkdownSegmentsAfterStructure: (value) => String(value || "")
  }
);
const structure = extractedFunctions(
  "isEscapedMarkdownDelimiter",
  "createMarkdownOrderedList",
  [
    "protectMarkdownSegmentsForStructure",
    "restoreMarkdownSegmentsAfterStructure",
    "normalizeMarkdownStructureText"
  ],
  {
    isInlineDollarMath,
    normalizeMathTextForMarkdown,
    normalizeLooseMarkdownTables
  }
);

function fakeElement(kind) {
  const classes = new Set();
  return {
    kind,
    children: [],
    className: "",
    classList: {
      add(...values) {
        values.forEach((value) => classes.add(value));
      },
      contains(value) {
        return classes.has(value);
      }
    },
    dataset: {},
    textContent: "",
    innerHTML: "",
    addEventListener() {},
    appendChild(child) {
      this.children.push(child);
      return child;
    }
  };
}

const selectableMathFactory = extractedFunctions(
  "createSelectableMathNode",
  "appendPlainText",
  ["createSelectableMathNode"],
  {
    normalizedMathParts,
    cachedKatexHtml: () => "",
    document: { createElement: fakeElement },
    copyToClipboard: () => Promise.resolve(),
    window: { getSelection: () => "", setTimeout: () => 0 }
  }
).createSelectableMathNode;
const invalidFallback = selectableMathFactory(String.raw`\(\notARealKatexCommand{x}\)`);
assert.equal(invalidFallback.children.length, 1, "Invalid source must exist only once in the DOM");
assert.equal(invalidFallback.children[0].textContent, String.raw`\(\notARealKatexCommand{x}\)`);
assert.equal(invalidFallback.children[0].classList.contains("fallback"), true);

const inlineRenderer = extractedFunctions(
  "appendInlineMarkdown",
  "normalizeMathTextForMarkdown",
  ["appendInlineMarkdown"],
  {
    protectMarkdownSegmentsForStructure: structure.protectMarkdownSegmentsForStructure,
    restoreMarkdownSegmentsAfterStructure: structure.restoreMarkdownSegmentsAfterStructure,
    createSelectableMathNode: (source) => ({ kind: "math", source }),
    appendPlainText: (parent, source) => parent.appendChild({ kind: "text", source }),
    normalizeMarkdownHeading: (source) => source,
    document: { createElement: fakeElement }
  }
).appendInlineMarkdown;

const displayParsing = extractedFunctions(
  "displayMathLine",
  "protectMarkdownSegmentsForStructure",
  ["displayMathLine", "collectDisplayMathBlock", "closingMarkdownDelimiterIndex"]
);

for (const adjacent of [
  "$$A$$$$B$$",
  String.raw`\[A\]\[B\]`,
  `$$${gemmaFormulaTex}$$$$${gemmaFormulaTex}$$`,
  `$$${gemmaResultTex}$$$$${gemmaResultTex}$$`
]) {
  assert.equal(displayParsing.displayMathLine(adjacent), "");
  assert.equal(displayParsing.collectDisplayMathBlock([adjacent], 0), null);
  const rendered = fakeElement("root");
  inlineRenderer(rendered, adjacent);
  assert.deepEqual(
    rendered.children.filter((child) => child.kind === "math").map((child) => child.source),
    structure.protectMarkdownSegmentsForStructure(adjacent)
      .segments
      .filter((segment) => segment.kind === "math")
      .map((segment) => segment.source)
  );
  const mathSegments = structure.protectMarkdownSegmentsForStructure(adjacent)
    .segments
    .filter((segment) => segment.kind === "math");
  assert.equal(mathSegments.length, 2, `Adjacent display math was not split: ${adjacent}`);
  for (const segment of mathSegments) {
    const parsed = normalizedMathParts(segment.source);
    assert.equal(parsed.tex.includes("$$$$"), false);
    assert.doesNotThrow(() => katex.renderToString(parsed.tex, {
      displayMode: parsed.display,
      throwOnError: true,
      strict: katexStrictMode,
      trust: false
    }));
  }
}

const multilineDisplay = ["$$", "A+B", "$$"].join("\n");
assert.deepEqual(
  displayParsing.collectDisplayMathBlock(multilineDisplay.split("\n"), 0),
  { math: multilineDisplay, nextIndex: 3 }
);

const protectedFormulaCases = [
  String.raw`\[(x + 1) y\]`,
  String.raw`\(x + 2) y\)`,
  String.raw`$$x + 3) y$$`,
  String.raw`$x + 4) y$`
];
protectedFormulaCases.forEach((source) => {
  assert.equal(
    structure.normalizeMarkdownStructureText(source),
    source,
    `Markdown structure normalization changed math source: ${source}`
  );
  const protectedSource = structure.protectMarkdownSegmentsForStructure(source);
  assert.equal(protectedSource.segments.length, 1);
  assert.equal(protectedSource.segments[0].kind, "math");
  assert.equal(
    structure.restoreMarkdownSegmentsAfterStructure(
      protectedSource.text,
      protectedSource.segments
    ),
    source
  );
});

const multilineTableLikeFormula = [
  String.raw`\[`,
  "A  B",
  "C  D",
  String.raw`\]`
].join("\n");
assert.equal(
  structure.normalizeMarkdownStructureText(multilineTableLikeFormula),
  multilineTableLikeFormula,
  "Loose-table normalization changed a display-math segment"
);

const { splitMarkdownTableRow } = extractedFunctions(
  "splitMarkdownTableRow",
  "normalizeMarkdownHeading",
  ["splitMarkdownTableRow"],
  {
    protectMarkdownSegmentsForStructure: structure.protectMarkdownSegmentsForStructure,
    restoreMarkdownSegmentsAfterStructure: structure.restoreMarkdownSegmentsAfterStructure
  }
);
assert.deepEqual(
  splitMarkdownTableRow(String.raw`| Formel | \(x | y\) |`),
  ["Formel", String.raw`\(x | y\)`]
);
assert.deepEqual(
  splitMarkdownTableRow(String.raw`| Betrag | $|x|$ |`),
  ["Betrag", String.raw`$|x|$`]
);

const mixedMath = String.raw`Gültig: \(x + 1) y\); ungültig: \(\notARealKatexCommand{x} + 2) y\).`;
assert.equal(structure.normalizeMarkdownStructureText(mixedMath), mixedMath);
const mixedSegments = structure.protectMarkdownSegmentsForStructure(mixedMath)
  .segments
  .filter((segment) => segment.kind === "math");
assert.equal(mixedSegments.length, 2);
assert.doesNotThrow(() => katex.renderToString(normalizedMathParts(mixedSegments[0].source).tex, {
  throwOnError: true,
  strict: "error",
  trust: false
}));
assert.throws(() => katex.renderToString(normalizedMathParts(mixedSegments[1].source).tex, {
  throwOnError: true,
  strict: "error",
  trust: false
}));

const gemmaNumericTableValues = [
  String.raw`$6,674 \times 10^{-11}$`,
  String.raw`$1,989 \times 10^{30}$`,
  String.raw`$5,972 \times 10^{24}$`,
  String.raw`$149,6 \times 10^6$`
];
for (const source of gemmaNumericTableValues) {
  const mathSegments = structure.protectMarkdownSegmentsForStructure(source)
    .segments
    .filter((segment) => segment.kind === "math");
  assert.deepEqual(
    mathSegments.map((segment) => segment.source),
    [source],
    `Digit-leading inline math was not detected: ${source}`
  );

  const rendered = fakeElement("root");
  inlineRenderer(rendered, source);
  assert.deepEqual(
    rendered.children.filter((child) => child.kind === "math").map((child) => child.source),
    [source],
    `Digit-leading inline math was not rendered: ${source}`
  );

  const parsed = normalizedMathParts(source);
  assert.doesNotThrow(() => katex.renderToString(parsed.tex, {
    displayMode: parsed.display,
    throwOnError: true,
    strict: katexStrictMode,
    trust: false
  }));
}

const gemmaParameterTable = [
  ["**Parameter**", "**Symbol**", "**Wert**", "**Einheit**"].join("\t"),
  [
    "Gravitationskonstante",
    "$G$",
    gemmaNumericTableValues[0],
    String.raw`$\text{m}^3 \cdot \text{kg}^{-1} \cdot \text{s}^{-2}$`
  ].join("\t"),
  ["Masse der Sonne", "$m_1$", gemmaNumericTableValues[1], String.raw`$\text{kg}$`].join("\t"),
  ["Masse der Erde", "$m_2$", gemmaNumericTableValues[2], String.raw`$\text{kg}$`].join("\t"),
  ["Mittlerer Abstand", "$r$", gemmaNumericTableValues[3], String.raw`$\text{km}$`].join("\t")
].join("\n");
const normalizedGemmaParameterTable = structure.normalizeMarkdownStructureText(gemmaParameterTable);
for (const source of gemmaNumericTableValues) {
  assert.ok(
    normalizedGemmaParameterTable.includes(source),
    `Table normalization changed numeric math source: ${source}`
  );
}
assert.match(normalizedGemmaParameterTable, /^\| \*\*Parameter\*\* \|/m);
assert.match(normalizedGemmaParameterTable, /^\| --- \| --- \| --- \| --- \|$/m);

const gptOssGalaxyTable = [
  "| Parameter | Wert | Einheit |",
  "| --- | --- | --- |",
  String.raw`| Masse der Milchstraße (M<sub>MW</sub>) | \(1,5 \times 10^{12}\) M\(_{\odot}\) | M\(_{\odot}\) |`,
  String.raw`| Masse von Andromeda (M<sub>M31</sub>) | \(1,6 \times 10^{12}\) M\(_{\odot}\) | M\(_{\odot}\) |`,
  String.raw`| Abstand zwischen den Galaxien (r) | \(780\) kpc | kpc |`
].join("\n");
assert.equal(
  structure.normalizeMarkdownStructureText(gptOssGalaxyTable),
  gptOssGalaxyTable,
  "Rendering preparation must not rewrite the stored gpt-oss table"
);

const gptOssGalaxyRows = gptOssGalaxyTable.split("\n").map(splitMarkdownTableRow);
assert.deepEqual(gptOssGalaxyRows.map((row) => row.length), [3, 3, 3, 3, 3]);
for (const [rowIndex, expectedSubscript] of [[2, "MW"], [3, "M31"]]) {
  const renderedParameter = fakeElement("root");
  inlineRenderer(renderedParameter, gptOssGalaxyRows[rowIndex][0]);
  const subscripts = renderedParameter.children.filter((child) => child.kind === "sub");
  assert.equal(subscripts.length, 1);
  assert.equal(subscripts[0].textContent, expectedSubscript);
  assert.equal(subscripts[0].innerHTML, "", "Subscript content must never use innerHTML");

  const renderedValue = fakeElement("root");
  inlineRenderer(renderedValue, gptOssGalaxyRows[rowIndex][1]);
  assert.equal(
    renderedValue.children.filter((child) => child.kind === "math").length,
    2,
    "Existing value-column math must remain KaTeX input"
  );

  const renderedUnit = fakeElement("root");
  inlineRenderer(renderedUnit, gptOssGalaxyRows[rowIndex][2]);
  assert.deepEqual(
    renderedUnit.children.filter((child) => child.kind === "math").map((child) => child.source),
    [String.raw`\(_{\odot}\)`]
  );
}

const renderedDistanceParameter = fakeElement("root");
inlineRenderer(renderedDistanceParameter, gptOssGalaxyRows[4][0]);
assert.equal(
  renderedDistanceParameter.children.filter((child) => child.kind === "math").length,
  0,
  "Ordinary parentheses such as (r) must remain text"
);

const gptOssEinsteinTable = [
  "| **Anwendung** | **Formel (Einstein‑Notation)** | **Ergebnis (mit Einheit)** |",
  "| --- | --- | --- |",
  String.raw`| Skalarprodukt zweier Vektoren | \(a_i b_i\) | \(-3\;\text{J}\) |`,
  `| Spannung × Dehnung (Arbeit/Vol.) | \\(\\sigma_{ij}\\varepsilon_{ij}\\) | ${gptOssWorkFormulaSources[1]} |`
].join("\n");
assert.equal(
  structure.normalizeMarkdownStructureText(gptOssEinsteinTable),
  gptOssEinsteinTable,
  "Rendering preparation must preserve typographic spaces in stored table math"
);
const gptOssEinsteinRows = gptOssEinsteinTable.split("\n").map(splitMarkdownTableRow);
const renderedEinsteinResult = fakeElement("root");
inlineRenderer(renderedEinsteinResult, gptOssEinsteinRows[3][2]);
assert.deepEqual(
  renderedEinsteinResult.children.filter((child) => child.kind === "math").map((child) => child.source),
  [gptOssWorkFormulaSources[1]]
);
const normalizedEinsteinResult = normalizedMathParts(gptOssWorkFormulaSources[1]);
assert.doesNotThrow(() => katex.renderToString(normalizedEinsteinResult.tex, {
  throwOnError: true,
  strict: katexStrictMode,
  trust: false
}));

const renderedSuperscript = fakeElement("root");
inlineRenderer(renderedSuperscript, "Fläche m<sup>2</sup>");
assert.equal(renderedSuperscript.children.filter((child) => child.kind === "sup").length, 1);
assert.equal(renderedSuperscript.children.find((child) => child.kind === "sup").textContent, "2");

for (const unsafeOrMalformedHtml of [
  "M<sub>MW",
  "M<sub class=\"symbol\">MW</sub>",
  "M<sub><b>MW</b></sub>",
  "M<sub>MW</sup>",
  "<script>alert(1)</script>",
  "<img src=x onerror=alert(1)>"
]) {
  const rendered = fakeElement("root");
  inlineRenderer(rendered, unsafeOrMalformedHtml);
  assert.equal(
    rendered.children.some((child) => child.kind === "sub" || child.kind === "sup"),
    false,
    `Unsafe or malformed HTML was interpreted: ${unsafeOrMalformedHtml}`
  );
  assert.deepEqual(
    rendered.children.map((child) => child.source).filter(Boolean),
    [unsafeOrMalformedHtml],
    `Unsafe or malformed HTML was not preserved as visible source: ${unsafeOrMalformedHtml}`
  );
}

const inlineSubCode = "`M<sub>MW</sub>`";
const renderedInlineSubCode = fakeElement("root");
inlineRenderer(renderedInlineSubCode, inlineSubCode);
assert.deepEqual(renderedInlineSubCode.children.map((child) => child.kind), ["code"]);
assert.equal(renderedInlineSubCode.children[0].textContent, "M<sub>MW</sub>");
const appendInlineMarkdownSource = html.slice(
  html.indexOf("function appendInlineMarkdown"),
  html.indexOf("function normalizeMathTextForMarkdown")
);
assert.equal(
  appendInlineMarkdownSource.includes("innerHTML"),
  false,
  "Model-provided inline HTML must never be assigned through innerHTML"
);

[
  String.raw`Preis \$20 bleibt Text.`,
  "$20 bleibt Text.",
  String.raw`Ungeschlossen: \(x + 1) y`,
  String.raw`Ungeschlossen: $x + 1) y`,
  String.raw`Inline-Code: ` + "`" + String.raw`\(x + 1) y\)` + "`"
].forEach((source) => {
  assert.equal(structure.normalizeMarkdownStructureText(source), source);
});
assert.equal(
  structure.protectMarkdownSegmentsForStructure("$20 bleibt Text.")
    .segments
    .filter((segment) => segment.kind === "math")
    .length,
  0,
  "An unclosed currency amount must remain ordinary text"
);

const inlineCode = String.raw`Inline-Code: ` + "`" + String.raw`\(x + 1) y\)` + "`";
const inlineCodeSegments = structure.protectMarkdownSegmentsForStructure(inlineCode).segments;
assert.deepEqual(inlineCodeSegments.map((segment) => segment.kind), ["code"]);

const splitMarkdownCodeFenceSegments = extractedFunction(
  "splitMarkdownCodeFenceSegments",
  "renderMarkdownMessageText"
);
const fenced = [
  "Vorher",
  "```text",
  String.raw`\[(x + 1) y\]`,
  "$20",
  "M<sub>MW</sub>",
  "```",
  String.raw`Nachher \(z\)`
].join("\n");
const fencedSegments = splitMarkdownCodeFenceSegments(fenced);
assert.deepEqual(fencedSegments.map((segment) => segment.kind), [
  "markdown",
  "codeFence",
  "markdown"
]);
assert.match(fencedSegments[1].source, /\\\[\(x \+ 1\) y\\\]/);
assert.match(fencedSegments[1].source, /\$20/);
assert.match(fencedSegments[1].source, /M<sub>MW<\/sub>/);

const readableStoredMessageText = new Function(
  "germanizeText",
  `${html.slice(
    html.indexOf("function readableStoredMessageText"),
    html.indexOf("function normalizeMessage", html.indexOf("function readableStoredMessageText"))
  )}; return readableStoredMessageText;`
)((value) => String(value || ""));
const ordinaryJson = JSON.stringify({ message: "Vom Nutzer gewünschtes JSON", value: 42 });
assert.equal(readableStoredMessageText("AI", ordinaryJson), ordinaryJson);
const legacyJson = JSON.stringify({
  schema: "barebone.agent.response.v2",
  type: "message",
  message: "Historische sichtbare Antwort"
});
assert.equal(readableStoredMessageText("AI", legacyJson), "Historische sichtbare Antwort");
const ordinaryFinal = "The user asked for this exact visible sentence.";
assert.equal(readableStoredMessageText("AI", ordinaryFinal), ordinaryFinal);

console.log("KaTeX 0.16.10 runtime, delimiter, and renderer protection tests passed.");
