[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$qt = [System.IO.File]::ReadAllText((Join-Path $root "src/ui/BricsCadPage.cpp"), [System.Text.Encoding]::UTF8)
$brx = [System.IO.File]::ReadAllText((Join-Path $root "src/bricscad/BareboneBrxPlugin.cpp"), [System.Text.Encoding]::UTF8)
$learning = [System.IO.File]::ReadAllText((Join-Path $root "agent/bricscad-learning/brx-learning.json"), [System.Text.Encoding]::UTF8)
$index = [System.IO.File]::ReadAllText((Join-Path $root "index.html"), [System.Text.Encoding]::UTF8)
$failures = [System.Collections.Generic.List[string]]::new()

function Require-Text([string]$Text, [string]$Needle, [string]$Message) {
    if (-not $Text.Contains($Needle)) { $script:failures.Add($Message) }
}

Require-Text $qt 'isValidBricsCadLayerName(name)' 'Qt must allow every valid layer name without enforcing a two-word structure.'
Require-Text $qt 'field == "layername"' 'ask_user must recognize an explicitly supplied layerName as already provided.'
Require-Text $qt 'Extrusion deterministisch dem Ziellayer zuweisen' 'Qt must append the required post-extrusion layer assignment.'
Require-Text $qt 'Neu erzeugte Geometrie wie vom Nutzer verlangt selektieren' 'Qt must materialize an explicit selection request before extrusion.'
Require-Text $qt 'selectedTools is a relevance hint only' 'Tool selection must be a relevance hint, not an allow-list.'
Require-Text $qt 'return allTools;' 'CAD routes must retain all available BRX capabilities.'
Require-Text $qt 'toolName == QStringLiteral("profile.extrude")' 'Compact prompt relevance must include profile.extrude.'
Require-Text $qt 'compatibleGeometry' 'The compact selector contract must expose geometry compatibility.'
Require-Text $brx '"solidProfiles":["circle","ellipse","closedPolyline","closedSpline","rectangle"]' 'profile.extrude must advertise supported solid profiles.'
Require-Text $brx 'BRX accepts' 'BRX must accept Qt-approved one-word layer names.'
Require-Text $learning "including a one-word name such as 'Test'" 'The layer tool profile must preserve explicit one-word names.'
Require-Text $learning "never rename 'Test' to 'Test Layer'" 'The layer tool profile must forbid replacing an explicit name.'
Require-Text $learning 'Any non-empty layer name without unsupported special characters is valid' 'The layer tool profile must allow all valid non-empty layer names.'
Require-Text $index 'function normalizedProposalForStorage(proposal)' 'Proposal payloads must be normalized before session storage.'
Require-Text $index 'workspaces: workspaceStatesForStorage({ includeProposals: false })' 'Qt client-state persistence must exclude open proposals.'
Require-Text $index 'bridge.saveClientState(JSON.stringify(clientStateForQt()))' 'Qt client-state persistence must use the proposal-free state snapshot.'

if ($learning.Contains("Every new layer name must use the structure 'Fachgebiet Thema'")) {
    $failures.Add('The obsolete mandatory two-word layer policy is still present.')
}
if ($qt.Contains("muss die Struktur 'Fachgebiet Thema'")) {
    $failures.Add('Qt still rejects one-word layer names with the obsolete two-word error.')
}
if ($brx.Contains('AI-Walls')) {
    $failures.Add('BRX examples still contain hyphenated layer names.')
}
if ($learning.Contains("German umlauts and '-' are allowed")) {
    $failures.Add('Layer learning still allows hyphens as valid layer-name characters.')
}
if ($qt.Contains('agentProposalActions(m_pendingAgentProposal)')) {
    $failures.Add('Tool routing must not inspect m_pendingAgentProposal through agentProposalActions; that path can recurse through toolDefinition.')
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host 'BRX capability routing contract passed.'
