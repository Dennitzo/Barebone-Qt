[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$failures = [System.Collections.Generic.List[string]]::new()

function Read-Text([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not [System.IO.File]::Exists($path)) {
        $script:failures.Add("Missing file: $RelativePath")
        return ""
    }
    return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

function Require-Text([string]$Text, [string]$Needle, [string]$Message) {
    if (-not $Text.Contains($Needle)) { $script:failures.Add($Message) }
}

function Forbid-Text([string]$Text, [string]$Needle, [string]$Message) {
    if ($Text.Contains($Needle)) { $script:failures.Add($Message) }
}

function Require-MissingFile([string]$RelativePath, [string]$Message) {
    if ([System.IO.File]::Exists((Join-Path $root $RelativePath))) { $script:failures.Add($Message) }
}

$cmake = Read-Text "CMakeLists.txt"
$qt = Read-Text "src/ui/BricsCadPage.cpp"
$qtHeader = Read-Text "src/ui/BricsCadPage.h"
$brxAgent = Read-Text "src/agent/BrxAgent.cpp"
$drawingStore = Read-Text "src/agent/DrawingContextStore.cpp"
$scheduler = Read-Text "src/agent/LocalAiJobScheduler.cpp"
$learningAgent = Read-Text "src/agent/BricsCadLearningAgent.cpp"
$brx = Read-Text "src/bricscad/BareboneBrxPlugin.cpp"
$BrxBimSdk = Read-Text "src/bricscad/BrxBimSdk.cpp"
$learning = Read-Text "agent/bricscad-learning/brx-learning.json"
$index = Read-Text "index.html"
$removedBrokerName = "BrxCapability" + "Broker"
$removedBimStem = "Bim" + "Tools"

Require-Text $cmake 'src/agent/BrxAgent.cpp' 'CMake must compile BrxAgent.cpp.'
Require-MissingFile 'BRX_CAPABILITIES.md' 'Manual BRX_CAPABILITIES.md must be removed; BrxAgent/BRX registry are the runtime sources.'
Forbid-Text $cmake $removedBrokerName 'CMake must not compile the removed capability broker.'
Require-Text $cmake 'src/bricscad/BrxBimSdk.cpp' 'CMake must compile BrxBimSdk.cpp.'
Forbid-Text $cmake "src/bricscad/$removedBimStem.cpp" "CMake must not compile removed $removedBimStem.cpp."
Forbid-Text $cmake "src/bricscad/$removedBimStem.h" "CMake must not compile removed $removedBimStem.h."
Require-Text $qt '#include "../agent/BrxAgent.h"' 'BricsCadPage must use BrxAgent as the public BRX context/tool broker.'
Forbid-Text $qt ($removedBrokerName + '::') 'BricsCadPage must not call the removed capability broker directly.'
Require-Text $qt 'BrxAgent::buildToolCatalog' 'availableAgentTools() must build the AI-visible catalog through BrxAgent.'
Require-Text $qt 'BrxAgent::selectToolsForRoute' 'Route-specific tool selection must be delegated to BrxAgent.'
Require-Text $qt 'BrxAgent::localContextMethods' 'Read-only context methods must come from BrxAgent.'
Require-Text $qt 'qt.brx.db.compatibility' 'Qt must expose BrxAgent DB compatibility context.'
Require-Text $qt 'qt.brx.context.manifest' 'Qt must expose full BRX context manifest.'
Require-Text $qt 'qt.brx.context.fetch' 'Qt must expose paged BRX context fetch.'
Require-Text $qt 'qt.brx.db.fullContext' 'Qt must expose paged full DB context.'
Require-Text $qt 'qt.brx.execution.history' 'Qt must expose execution history context.'
Require-Text $qt 'qt.brx.workflow.repairContext' 'Qt must expose workflow repair context.'
Require-Text $qt 'qt.brx.workflow.repairHints' 'Qt must expose workflow repair hints to the local AI.'
Require-Text $brxAgent 'brx.sdk.entity.transformBy' 'BrxAgent must make SDK transform toolcards selectable.'
Require-Text $brxAgent 'brx.sdk.blockReference.setPosition' 'BrxAgent must make SDK BlockReference toolcards selectable.'
Require-Text $brxAgent 'brx.sdk.assoc.evaluate' 'BrxAgent must make SDK association-evaluate toolcards selectable.'
foreach ($sdkAlias in @(
    'brx.sdk.entity.copy',
    'brx.sdk.entity.rotateBy',
    'brx.sdk.entity.scaleBy',
    'brx.sdk.entity.erase',
    'brx.sdk.entity.setLayer',
    'brx.sdk.entity.setName',
    'brx.sdk.selection.setPickfirst',
    'brx.sdk.bim.classification.set'
)) {
    Require-Text $brxAgent $sdkAlias "BrxAgent must route SDK alias $sdkAlias."
}

Require-Text $qt 'isValidBricsCadLayerName(name)' 'Qt must allow valid one-word layer names without enforcing a two-word structure.'
Require-Text $qt 'field == "layername"' 'ask_user must recognize an explicitly supplied layerName as already provided.'
Require-Text $qt 'Extrusion deterministisch dem Ziellayer zuweisen' 'Qt must append required post-extrusion layer assignment.'
Require-Text $qt 'Neu erzeugte Geometrie wie vom Nutzer verlangt selektieren' 'Qt must materialize explicit selection request before extrusion.'
Require-Text $brx '"circle.extrude"' 'BRX capabilities must expose circle.extrude.'
Require-Text $brx 'countCircleProfiles' 'BRX actions.validate must validate circle profiles for circle.extrude.'
Require-Text $brx 'method == "circle.extrude"' 'BRX dispatcher must execute circle.extrude.'
Require-Text $brx 'selectorMatchTokensForEntity' 'BRX selector filters must use canonical entity match tokens.'
Require-Text $brx 'selectorAnyTokenMatches' 'BRX selector array filters must match canonical aliases.'
Require-Text $brx 'state.plannedLastResultKind = "CIRCLE"' 'BRX batch preflight must recognize geometry.create circle as planned lastResult.'
Require-Text $brx 'normalizedSelectorScope' 'BRX selector scope validation/resolution must normalize common aliases.'
Require-Text $brxAgent 'circle.extrude' 'BrxAgent must route circle.extrude.'
Require-Text $brxAgent 'selectedToolNames << QStringLiteral("profile.extrude")' 'Circle extrusion selectedTools routing must include profile.extrude fallback.'
Require-Text $brxAgent 'circleExtrudeIntent && (name == QStringLiteral("circle.extrude") || name == QStringLiteral("profile.extrude"))' 'Circle extrusion heuristic routing must include profile.extrude fallback.'
Require-Text $qt 'tool == QStringLiteral("circle.extrude")' 'Qt must treat circle.extrude as an extrusion action.'
Require-Text $qt 'profileCircleSelector' 'Qt proposal validation must allow profile.extrude circle fallback without forcing a target layer.'
Require-Text $qt 'Wenn circle.extrude nicht in effectiveTools vorhanden ist' 'AI policy must instruct profile.extrude fallback when circle.extrude is unavailable.'
Require-Text $qt 'params.contains(QStringLiteral("z"))' 'Qt must normalize z aliases to heightMm for extrusion prompts.'
Require-Text $brx 'jsonDoubleProperty(paramsJson, "z")' 'BRX extrusion validation/execution must accept z as a height alias.'
Require-Text $brxAgent 'return filtered;' 'BrxAgent CAD routes must send filtered detailed tools instead of an unbounded full tool dump.'
Require-Text $qtHeader 'DrawingContextStore m_drawingContextStore' 'Qt must keep prompt-driven drawing context store.'
Require-Text $qt 'm_drawingContextStore.clear()' 'Every BricsCAD prompt must discard stale drawing facts before explicit read-only queries.'
Require-Text $drawingStore 'method == QStringLiteral("selection.describe")' 'DrawingContextStore must ingest explicit selection.describe results.'
Require-Text $drawingStore 'contextManifest()' 'DrawingContextStore must expose a full BRX context manifest.'
Require-Text $drawingStore 'fetch(const QJsonObject& params)' 'DrawingContextStore must expose paged context fetch.'
Require-Text $drawingStore 'inspect(const QJsonObject& params)' 'DrawingContextStore must expose DB inspect.'
Require-Text $drawingStore 'fullContext(const QJsonObject& params)' 'DrawingContextStore must expose paged full DB context.'
Require-Text $drawingStore 'executionHistory(const QJsonObject& params)' 'DrawingContextStore must expose execution history.'
Require-Text $drawingStore 'repairContext(const QJsonObject& params)' 'DrawingContextStore must expose workflow repair context.'
Require-Text $drawingStore 'compactObjectFact' 'DrawingContextStore must normalize BRX objects into facts.'
Require-Text $drawingStore 'm_normalizedFacts' 'DrawingContextStore must keep a normalized fact index.'
Require-Text $drawingStore 'm_capabilityBlocks' 'DrawingContextStore must persist capability/action/command context blocks.'
Require-Text $scheduler 'std::clamp(value, 1, 4)' 'Local AI scheduler must keep bounded concurrency.'
Require-Text $scheduler 'activeForegroundCount()' 'Local AI scheduler must expose foreground accounting.'

Require-Text $brxAgent 'buildToolCatalog' 'BrxAgent must own runtime catalog construction.'
Require-Text $brxAgent 'selectToolsForRoute' 'BrxAgent must own route-specific tool selection.'
Require-Text $brxAgent 'runtimeToolsWithSdkTools' 'BrxAgent must own runtime tool enrichment.'
Require-Text $brxAgent 'qt.brx.tools.describe' 'BrxAgent must expose tool describe context.'
Require-Text $brxAgent 'qt.brx.db.schema' 'BrxAgent must expose DB schema context.'
Require-Text $brxAgent 'qt.brx.db.query' 'BrxAgent must expose DB query context.'
Require-Text $brxAgent 'qt.brx.db.inspect' 'BrxAgent must expose DB inspect context.'
Require-Text $brxAgent 'qt.brx.db.compatibility' 'BrxAgent must expose DB compatibility context.'
Require-Text $brxAgent 'qt.brx.context.manifest' 'BrxAgent must expose full context manifest.'
Require-Text $brxAgent 'qt.brx.context.fetch' 'BrxAgent must expose full context fetch.'
Require-Text $brxAgent 'qt.brx.db.fullContext' 'BrxAgent must expose full DB context.'
Require-Text $brxAgent 'qt.brx.execution.history' 'BrxAgent must expose execution history.'
Require-Text $brxAgent 'qt.brx.workflow.repairContext' 'BrxAgent must expose workflow repair context.'
Require-Text $brxAgent 'qt.brx.workflow.testPlan' 'BrxAgent must expose workflow test planning context.'
Require-Text $brxAgent 'qt.brx.workflow.repairHints' 'BrxAgent must expose workflow repair hints.'
Require-Text $brxAgent 'brx.sdk.entity.transformBy' 'BrxAgent must describe SDK entity transformation.'
Require-Text $brxAgent 'brx.sdk.blockReference.setPosition' 'BrxAgent must describe SDK BlockReference movement.'
Require-Text $brxAgent 'brx.sdk.assoc.evaluate' 'BrxAgent must describe SDK association evaluation.'
foreach ($sdkAlias in @(
    'brx.sdk.entity.copy',
    'brx.sdk.entity.rotateBy',
    'brx.sdk.entity.scaleBy',
    'brx.sdk.entity.erase',
    'brx.sdk.entity.setLayer',
    'brx.sdk.entity.setName',
    'brx.sdk.selection.setPickfirst',
    'brx.sdk.bim.classification.set'
)) {
    Require-Text $brxAgent $sdkAlias "BrxAgent must describe SDK alias $sdkAlias."
}
foreach ($validationMapping in @(
    'return QStringLiteral("geometry.copy");',
    'return QStringLiteral("geometry.rotate");',
    'return QStringLiteral("geometry.scale");',
    'return QStringLiteral("geometry.delete");',
    'return QStringLiteral("entity.setLayer");',
    'return QStringLiteral("entity.setName");',
    'return QStringLiteral("selection.set");',
    'return QStringLiteral("bim.classify");'
)) {
    Require-Text $qt $validationMapping "Qt must map SDK aliases through public BRX validation: $validationMapping"
}
Require-Text $brxAgent 'compatibleGeometry' 'BrxAgent compact selector contract must expose geometry compatibility.'
Require-Text $brxAgent 'repairToolContext' 'BrxAgent must build full repair tool context.'
Require-Text $brxAgent 'allToolIndex' 'Repair context must expose every tool in a compact index.'
Require-Text $brxAgent 'candidateGroups' 'Repair context must group alternative tool candidates.'
Require-Text $brxAgent 'describeAllTools' 'Repair context must advertise paged full-tool describe access.'
Require-Text $brxAgent 'params.value(QStringLiteral("all")).toBool(false)' 'qt.brx.tools.describe must support all=true.'
Require-Text $brxAgent 'params.value(QStringLiteral("cursor"))' 'qt.brx.tools.describe must support cursor paging.'
Require-Text $brxAgent 'params.value(QStringLiteral("includeSchemas"))' 'qt.brx.tools.describe must support schema inclusion control.'
Require-Text $qt 'BrxAgent::repairToolContext' 'BricsCadPage must inject BrxAgent repair context into retry envelopes.'
Require-Text $qt 'repairMode.allToolIndex' 'Repair retry instructions must tell the AI to use the full compact tool index.'
Require-Text $qt 'qt.brx.tools.describe names=[...] oder all=true' 'Repair retry instructions must expose full toolcard paging.'
Forbid-Text $brxAgent $removedBrokerName 'BrxAgent must contain the migrated broker logic directly.'
Forbid-Text $qt 'toolProfile(' 'BricsCadPage must not enrich tools from persistent learning toolProfiles.'
Forbid-Text $learning '"toolProfiles"' 'brx-learning.json must not store canonical tool profiles.'
Require-Text $learning '"id": "workflow_brx_selector_reference"' 'Learning JSON must contain the protected selector reference workflow.'
Require-Text $learning '"title": "BRX Selector Referenz"' 'Selector reference workflow must be visible by title.'
Require-Text $learning '"source": "canonical_brx_workflow"' 'Selector reference workflow must be marked as canonical BRX workflow.'
Require-Text $learning '"selectorScenarios"' 'Selector reference workflow must document selector scenarios.'
Require-Text $learning '"scenario": "explicit_handle"' 'Selector reference workflow must document explicit handle selectors.'
Require-Text $learning '"scenario": "current_selection"' 'Selector reference workflow must document current selection selectors.'
Require-Text $learning '"scenario": "created_circle_lastResult"' 'Selector reference workflow must document circle lastResult selectors.'
Require-Text $learning '"fallbackCommand": "profile.extrude"' 'Selector reference workflow must document profile.extrude fallback for circle extrusion.'
Require-Text $learning '"scenario": "post_extrusion_lastExtruded"' 'Selector reference workflow must document lastExtruded selectors.'
Require-Text $learning '"scenario": "current_space_read_context"' 'Selector reference workflow must document currentSpace read-context selectors.'
Require-Text $learningAgent 'selectorScenarios' 'Learning compact context must expose selectorScenarios to the AI.'
Forbid-Text $learning 'ai_circle.extrude_anwenden' 'Faulty selector runtime lesson must be removed from learning context.'
Require-Text $brx 'namespace BrxToolRegistry' 'BRX plugin must keep bridge method/command descriptors in BrxToolRegistry.'
Forbid-Text $brx 'jsonCapabilitiesResponse(' 'Old non-registry capabilities response must be removed.'
Forbid-Text $brx 'jsonActionsResponse(' 'Old non-registry actions response must be removed.'
Forbid-Text $brx 'jsonCommandsResponse(' 'Old non-registry commands response must be removed.'

foreach ($removedBrokerFile in @(
    ('src/agent/' + $removedBrokerName + '.cpp'),
    ('src/agent/' + $removedBrokerName + '.h')
)) {
    Require-MissingFile $removedBrokerFile "Removed capability broker file still exists: $removedBrokerFile"
}

foreach ($removedBimFile in @(
    "src/bricscad/$removedBimStem.cpp",
    "src/bricscad/$removedBimStem.h"
)) {
    Require-MissingFile $removedBimFile "Removed BIM helper file still exists: $removedBimFile"
}

Require-Text $learningAgent 'learningKey' 'Runtime learning upsert must preserve stable learning keys for update-in-place behavior.'
Require-Text $learningAgent 'stableLessonId' 'Runtime learning upsert must convert stable learning keys into stable lesson ids.'
Require-Text $learningAgent 'lessonKind' 'Runtime learnings must distinguish observations, command templates and workflows.'
Require-Text $learningAgent 'directTestEligible' 'Runtime learnings must explicitly mark direct workflow test eligibility.'
Require-Text $learningAgent 'observedCommands' 'Runtime command learnings must persist structured observed command metadata.'
Require-Text $learningAgent 'commandTemplatesFromActionShapes' 'Runtime command actions must be convertible into executable command templates.'

Require-Text $brx '"brx.sdk.assoc.evaluate"' 'BRX capabilities must expose the BrxAgent SDK association-evaluate method.'
Require-Text $brx 'if (command == "GEOMETRYCREATE")' 'BRX dispatcher must execute geometry.create via GEOMETRYCREATE.'
Require-Text $brx 'return createGeometryInApplicationContext(paramsJson);' 'GEOMETRYCREATE must call createGeometryInApplicationContext.'
Require-Text $brx 'BRXASSOCEVALUATE' 'BRX dispatch must route brx.sdk.assoc.evaluate.'
Require-Text $brx 'evaluateAssocNetworkInApplicationContext' 'BRX must implement association evaluation.'
Require-Text $brx 'AcDbAssocManager::evaluateTopLevelNetwork' 'BRX must evaluate association networks through the SDK.'
Require-Text $brx 'moveEntityWithBrxSdk' 'geometry.move must use BRX SDK entity/block-reference functions.'
Require-Text $brx 'AcDbBlockReference::setPosition' 'BlockReference moves must retain the BRX SDK position API.'
Require-Text $brx 'AcDbEntity::transformBy' 'Entity moves must retain the BRX SDK transform API.'
Require-Text $brx 'validateGeometryMoveReadback' 'geometry.move must verify transformation by readback.'
Require-Text $brx 'autoBimHandlesFromLastQuery' 'BIM follow-up actions must support exact handles from the preceding BIM query.'
Require-Text $brx 'hasBimValidationArtifacts' 'BIM mutations must require resolved handles and target fingerprints from actions.validate.'
Require-Text $brx 'validateMoveTargetDatabaseState' 'geometry.move preflight must reject stale, XRef or locked-layer targets.'
Require-Text $brx 'learnedCommandWhitelistContains' 'command.execute validation must accept Qt-authorized learned commands.'
Require-Text $brx 'COMMAND_NAME' 'command.execute results must include the normalized native command name.'
Require-Text $brx 'isQuiescent()' 'Prompt-driven BRX access must reject requests while the active document is not quiescent.'
Require-Text $brx 'activeDocumentReadyForPromptRequest(lifecycleError)' 'Prompt-driven bridge requests must pass the document lifecycle gate.'
Require-Text $brx 'Intentionally do not print debug noise into the BricsCAD command line.' 'BRX debug logging must stay out of the command line.'
Require-Text $brx 'Qt merges them into the bridge log' 'BRX debug lines must be available to Qt bridge log.'

foreach ($removed in @(
    'geometry.move.status',
    'BBGEOMETRYMOVE',
    'GeometryMoveJob',
    'g_geometryMoveJobs',
    'queueGeometryMoveJob',
    'runNativeManipMoveCommand',
    'ScopedShortSysVar',
    'hostSurfaceHint',
    'appendHostSurfacesJson',
    'appendAnchorSearchJson'
)) {
    Forbid-Text $qt $removed "Qt still contains removed fixed BIM move symbol: $removed"
    Forbid-Text $brx $removed "BRX still contains removed fixed BIM move symbol: $removed"
    Forbid-Text $learning $removed "Learning data still contains removed fixed BIM move symbol: $removed"
}

foreach ($removedFile in @(
    'src/agent/DrawingActivityTracker.cpp',
    'src/agent/DrawingActivityTracker.h',
    'src/agent/InteractiveDrawingLearningAgent.cpp',
    'src/agent/InteractiveDrawingLearningAgent.h'
)) {
    Require-MissingFile $removedFile "Removed realtime drawing-learning file still exists: $removedFile"
    Forbid-Text $cmake $removedFile "CMake still compiles removed realtime drawing-learning file: $removedFile"
}

foreach ($removedSource in @($cmake, $qt + "`n" + $qtHeader, $learningAgent, $learning, $index)) {
    Forbid-Text $removedSource 'drawing_ai_runtime' 'drawing_ai_runtime must be absent from build, Qt, learning code/data and overlay.'
}

foreach ($removedBrxSymbol in @(
    'AcEditorReactor',
    'AcDbDatabaseReactor',
    'AcApDocManagerReactor',
    'BridgeEditorReactor',
    'BridgeDatabaseReactor',
    'BridgeDocumentReactor',
    'sendCommandActivityEvent',
    'selection.changed',
    'command.started',
    'command.ended',
    'object.appended',
    'object.modified',
    'object.erased',
    'g_drawingLifecycleActive',
    'BBTRACK'
)) {
    Forbid-Text $brx $removedBrxSymbol "BRX still contains removed realtime command/database/selection tracking symbol: $removedBrxSymbol"
}

Require-Text $brxAgent "including a one-word name such as 'Test'" 'BrxAgent layer tool must preserve explicit one-word names.'
Require-Text $brxAgent "Never rename 'Test' to 'Test Layer'" 'BrxAgent layer tool must forbid replacing explicit names.'
Require-Text $brxAgent 'Any non-empty layer name without unsupported special characters is valid' 'BrxAgent layer tool must allow all valid non-empty names.'
Forbid-Text $learning 'workflow_bim_move' 'Learning data must not contain the removed fixed BIM move workflow.'
Forbid-Text $learning 'MANIP_MOVE' 'Learning data must not hardcode MANIP_MOVE.'
Forbid-Text $learning 'Unbekannter BRX Request' 'Learning data must not preserve false geometry.create unknown-request failures.'
Require-Text $index 'function normalizedProposalForStorage(proposal)' 'Proposal payloads must be normalized before session storage.'
Require-Text $index 'workspaces: workspaceStatesForStorage({ includeProposals: false })' 'Qt client-state persistence must exclude open proposals.'
Require-Text $index 'bridge.saveClientState(JSON.stringify(clientStateForQt()))' 'Qt client-state persistence must use proposal-free state snapshots.'

if ($qt.Contains("muss die Struktur 'Fachgebiet Thema'")) {
    $failures.Add('Qt still rejects one-word layer names with the obsolete two-word error.')
}
if ($learning.Contains("Every new layer name must use the structure 'Fachgebiet Thema'")) {
    $failures.Add('The obsolete mandatory two-word layer policy is still present.')
}
if ($qt.Contains('agentProposalActions(m_pendingAgentProposal)')) {
    $failures.Add('Tool routing must not inspect m_pendingAgentProposal through agentProposalActions; that path can recurse through toolDefinition.')
}

$printDebugStart = $brx.IndexOf('void printBrxDebug(const std::string& message)', [System.StringComparison]::Ordinal)
if ($printDebugStart -ge 0) {
    $printDebugEnd = $brx.IndexOf('std::string brxDebugLogPath()', $printDebugStart, [System.StringComparison]::Ordinal)
    if ($printDebugEnd -gt $printDebugStart) {
        $printDebugBody = $brx.Substring($printDebugStart, $printDebugEnd - $printDebugStart)
        if ($printDebugBody.Contains('acutPrintf')) {
            $failures.Add('printBrxDebug must not write into the BricsCAD command line.')
        }
    }
}

foreach ($token in @(
    "BimApi::isBimAvailable",
    "BimClassification::getAllClassified",
    "BimClassification::getClassification",
    "BimClassification::getName",
    "BimClassification::getGUID",
    "BrxDbProperties::listAll",
    "isAnchoredBlockRef",
    "getAnchorFace",
    "getAnchoredBlockReferences"
)) {
    Require-Text $BrxBimSdk $token "BrxBimSdk.cpp must keep SDK BIM support for $token."
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host 'BRX capability routing contract passed.'
