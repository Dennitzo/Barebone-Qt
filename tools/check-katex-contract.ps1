$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$index = Get-Content -LiteralPath (Join-Path $root "index.html") -Raw
$cpp = Get-Content -LiteralPath (Join-Path $root "src/ui/ChatPage.cpp") -Raw
$pageHeader = Get-Content -LiteralPath (Join-Path $root "src/ui/ChatPage.h") -Raw
$bricsCpp = Get-Content -LiteralPath (Join-Path $root "src/ui/BricsCadPage.cpp") -Raw
$bricsHeader = Get-Content -LiteralPath (Join-Path $root "src/ui/BricsCadPage.h") -Raw
$bridgeCpp = Get-Content -LiteralPath (Join-Path $root "src/ui/AiWebBridge.cpp") -Raw
$bridgeHeader = Get-Content -LiteralPath (Join-Path $root "src/ui/AiWebBridge.h") -Raw
$resources = Get-Content -LiteralPath (Join-Path $root "resources/resources.qrc") -Raw

function Assert-ContainsAny {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string[]]$Fragments,
        [Parameter(Mandatory = $true)][string]$Description
    )

    foreach ($fragment in $Fragments) {
        if ($Text.Contains($fragment)) {
            return
        }
    }
    throw "KaTeX contract missing: $Description"
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Fragment,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($Text.IndexOf($Fragment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "KaTeX contract violation ($Description): $Fragment"
    }
}

# The renderer must recognize the four common model-native delimiter pairs.
# Limit the search to the Markdown/math renderer so unrelated UI strings cannot
# accidentally satisfy the contract.
$rendererStart = $index.IndexOf("function katexStrictMode", [System.StringComparison]::Ordinal)
if ($rendererStart -lt 0) {
    throw "Markdown/math renderer entry point 'katexStrictMode' is missing from index.html."
}
$rendererEnd = $index.IndexOf("function scrollMessagesToBottomAfterLayout", $rendererStart, [System.StringComparison]::Ordinal)
if ($rendererEnd -lt 0) {
    throw "Could not determine the end of the Markdown/math renderer section in index.html."
}
$renderer = $index.Substring($rendererStart, $rendererEnd - $rendererStart)

Assert-ContainsAny $renderer @('"\\["', "'\\['") "display delimiter \[...\]"
Assert-ContainsAny $renderer @('"\\]"', "'\\]'") "display delimiter closing token \]"
Assert-ContainsAny $renderer @('"\\("', "'\\('") "inline delimiter \(...\)"
Assert-ContainsAny $renderer @('"\\)"', "'\\)'") "inline delimiter closing token \)"
Assert-ContainsAny $renderer @('"$$"', "'$$'") "display delimiter `$`$...`$`$"
Assert-ContainsAny $renderer @('"$"', "'$'") "inline delimiter `$...`$"

$requiredStrictRenderer = @(
    "throwOnError: true",
    "strict: katexStrictMode",
    "trust: false"
)
foreach ($fragment in $requiredStrictRenderer) {
    if (-not $index.Contains($fragment)) {
        throw "Strict KaTeX renderer contract missing: $fragment"
    }
}

$forbiddenPermissiveRenderer = @(
    "throwOnError: false",
    'strict: "ignore"',
    "trust: true"
)
foreach ($fragment in $forbiddenPermissiveRenderer) {
    Assert-NotContains $index $fragment "permissive rendering"
}

$strictPolicyStart = $renderer.IndexOf("function katexStrictMode", [System.StringComparison]::Ordinal)
$strictPolicyEnd = $renderer.IndexOf("function normalizedMathParts", $strictPolicyStart, [System.StringComparison]::Ordinal)
if ($strictPolicyStart -lt 0 -or $strictPolicyEnd -le $strictPolicyStart) {
    throw "Selective KaTeX strict-mode policy is missing."
}
$strictPolicy = $renderer.Substring($strictPolicyStart, $strictPolicyEnd - $strictPolicyStart)
Assert-ContainsAny $strictPolicy @('errorCode === "unicodeTextInMathMode"') "model-native accented labels"
Assert-ContainsAny $strictPolicy @('? "ignore" : "error"') "fail-closed strict-mode default"

# KaTeX must be bundled with the application. Other unrelated web libraries may
# still use their own loading policy and are intentionally outside this check.
$katexAssetLines = @($index -split "\r?\n" | Where-Object {
    $_ -match "(?i)katex" -and $_ -match "(?i)(?:src|href)\s*="
})
if ($katexAssetLines.Count -eq 0) {
    throw "Local KaTeX script and stylesheet references are missing from index.html."
}
$katexAssetReferences = [string]::Join("`n", $katexAssetLines)
if ($katexAssetReferences -notmatch "(?i)katex(?:\.min)?\.css") {
    throw "Local KaTeX stylesheet reference is missing from index.html."
}
if ($katexAssetReferences -notmatch "(?i)katex(?:\.min)?\.js") {
    throw "Local KaTeX JavaScript reference is missing from index.html."
}
foreach ($line in $katexAssetLines) {
    if ($line -match "(?i)(?:https?:)?//") {
        throw "KaTeX must not be loaded from a network URL: $($line.Trim())"
    }
}

$requiredEmbeddedAssets = @(
    "katex.min.css",
    "katex.min.js",
    "LICENSE"
)
foreach ($asset in $requiredEmbeddedAssets) {
    if ($resources.IndexOf($asset, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "Qt resource contract missing embedded KaTeX asset: $asset"
    }
}
$embeddedWoff2Count = [regex]::Matches($resources, "(?i)\.woff2").Count
if ($embeddedWoff2Count -lt 20) {
    throw "Qt resource contract embeds only $embeddedWoff2Count KaTeX WOFF2 fonts; expected the complete 0.16.10 font set (at least 20)."
}

# No diagnostic, retry, AI-repair, persistence or WebChannel repair wiring may
# survive. The strings are checked across both JavaScript and C++ runtime code.
$runtimeSources = @(
    @{ Name = "index.html"; Text = $index },
    @{ Name = "ChatPage.cpp"; Text = $cpp },
    @{ Name = "ChatPage.h"; Text = $pageHeader },
    @{ Name = "BricsCadPage.cpp"; Text = $bricsCpp },
    @{ Name = "BricsCadPage.h"; Text = $bricsHeader },
    @{ Name = "AiWebBridge.cpp"; Text = $bridgeCpp },
    @{ Name = "AiWebBridge.h"; Text = $bridgeHeader }
)
$forbiddenRepairFragments = @(
    "mathRepair",
    "MathFormattingRepair",
    "barebone.katex.repair",
    "katexDiagnostics",
    "katex-repair",
    "diagnoseMarkdownKatex",
    "safelyNormalizeMathMarkdown",
    "scheduleMathRepairQueue",
    "KaTeX-Reparatur",
    "Formatierungsreparaturdienst"
)
foreach ($source in $runtimeSources) {
    foreach ($fragment in $forbiddenRepairFragments) {
        Assert-NotContains $source.Text $fragment "obsolete repair code in $($source.Name)"
    }
}

# Model prompts must not contain a KaTeX syntax contract. Mathematical output is
# model-native; Qt alone is responsible for recognizing and rendering delimiters.
$forbiddenCppPromptFragments = @(
    "katexFormattingInstructionText",
    "katexFormattingContract",
    "KaTeX-Ausgabe-Regeln",
    "Inline-Mathematik nur mit",
    "Escape deshalb JEDEN LaTeX-Backslash",
    "Nie `$...`$, `$`$...`$`$"
)
foreach ($fragment in $forbiddenCppPromptFragments) {
    Assert-NotContains $cpp $fragment "KaTeX model-prompt contract"
}

# Inspect only the plain ChatPage system prompt. BricsCAD now uses the same
# chat shell and no longer has a CAD/tool envelope.
$generalPromptStart = $cpp.IndexOf("QString systemPrompt", [System.StringComparison]::Ordinal)
if ($generalPromptStart -lt 0) {
    throw "ChatPage system prompt declaration is missing."
}
$generalPromptEnd = $cpp.IndexOf("if (m_workspace == Workspace::BricsCad)", $generalPromptStart, [System.StringComparison]::Ordinal)
if ($generalPromptEnd -lt 0) {
    throw "Could not determine the end of the ChatPage general prompt."
}
$generalPrompt = $cpp.Substring($generalPromptStart, $generalPromptEnd - $generalPromptStart)
$forbiddenGeneralContractFragments = @(
    "barebone.agent.response",
    "responseContract",
    "context_request",
    "JSON-Envelope",
    "JSON-Objekt",
    "sessionTitle"
)
foreach ($fragment in $forbiddenGeneralContractFragments) {
    Assert-NotContains $generalPrompt $fragment "structured Barebone contract in general chat"
}

Assert-ContainsAny $generalPrompt @("Antworte", "Answer") "retained plain chat instruction"

Write-Host "Model-native offline KaTeX contract checks passed."
