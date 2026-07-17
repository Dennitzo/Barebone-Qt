$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$cpp = Get-Content -LiteralPath (Join-Path $root "src/ui/BricsCadPage.cpp") -Raw -Encoding UTF8

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Fragment,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (-not $Text.Contains($Fragment)) {
        throw "AI adapter contract missing ($Description): $Fragment"
    }
}

function Function-Section {
    param(
        [Parameter(Mandatory = $true)][string]$Start,
        [Parameter(Mandatory = $true)][string]$End
    )
    $startIndex = $cpp.IndexOf($Start, [System.StringComparison]::Ordinal)
    $endIndex = $cpp.IndexOf($End, $startIndex + $Start.Length, [System.StringComparison]::Ordinal)
    if ($startIndex -lt 0 -or $endIndex -le $startIndex) {
        throw "Could not isolate C++ section: $Start"
    }
    return $cpp.Substring($startIndex, $endIndex - $startIndex)
}

foreach ($fragment in @(
    "enum class LocalModelFamily",
    "GptOss",
    "Gemma4",
    "enum class AgentResponseKind",
    "VisibleMarkdown",
    "StructuredJson",
    "struct ParsedModelOutput",
    "reasoning_content",
    "output_text"
)) {
    Assert-Contains $cpp $fragment "model-family output adapter"
}

$sendSection = Function-Section "void BricsCadPage::sendAgentEnvelope" "QString BricsCadPage::generalWorkflowsDirectoryPath"
foreach ($fragment in @(
    'QStringLiteral("general_chat")',
    'QStringLiteral("document_qa")',
    "buildGeneralMessagesForBudget(envelope, systemMessage",
    "AgentResponseKind::VisibleMarkdown",
    "appendAgentChat(QStringLiteral(`"AI`"), content)"
)) {
    Assert-Contains $sendSection $fragment "plain general-chat path"
}
foreach ($forbidden in @(
    "barebone.agent.response.v2",
    "context_request",
    "sessionTitle",
    "LaTeX-Backslashes"
)) {
    if ($sendSection.IndexOf($forbidden, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Plain general-chat send path still contains obsolete contract text: $forbidden"
    }
}

$historySection = Function-Section "BricsCadPage::ContextBuildResult BricsCadPage::buildGeneralMessagesForBudget" "QJsonObject BricsCadPage::fallbackFocusedConversationContext"
foreach ($fragment in @(
    "barebone.agent.response.v2",
    "completeConversationContext",
    "messagesForIndexes",
    "selectedWorkflow",
    "documentContext"
)) {
    Assert-Contains $historySection $fragment "deterministic whole-message context"
}
if ($historySection.Contains("compressedHistorySummary") -or $historySection.Contains(".left(") -or $historySection.Contains("messageId")) {
    throw "General-chat context selection must not summarize, character-truncate, or forward persistence metadata."
}

$continueSection = Function-Section "void BricsCadPage::continueUnifiedAgentRequest" "void BricsCadPage::selectToolsForUnifiedAgentRequest"
Assert-Contains $continueSection "if (isChatWorkspace())" "local chat-history selection"
Assert-Contains $continueSection "if (!isChatWorkspace()" "AI focus restricted to non-chat workspace"

foreach ($schemaName in @(
    "barebone_router",
    "barebone_tool_selection",
    "barebone_context_focus",
    "barebone_agent_response",
    "barebone_workflow_training",
    "barebone_general_workflow"
)) {
    Assert-Contains $cpp $schemaName "route-specific local structured output"
}

$behaviorTest = Join-Path $PSScriptRoot "test-ai-context-behavior.js"
& node $behaviorTest
if ($LASTEXITCODE -ne 0) {
    throw "AI adapter behavior tests failed with exit code $LASTEXITCODE."
}

Write-Host "Model adapter and deterministic context contract checks passed."
