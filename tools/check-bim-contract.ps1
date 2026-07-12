[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$script:Failures = @()
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Add-ContractFailure {
    param([Parameter(Mandatory = $true)][string]$Message)
    $script:Failures += $Message
}

function Assert-Contract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        Add-ContractFailure $Message
    }
}

function Read-RepositoryText {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-ContractFailure "Missing required file: $RelativePath"
        return ""
    }
    try {
        return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
    }
    catch {
        Add-ContractFailure "Cannot read ${RelativePath}: $($_.Exception.Message)"
        return ""
    }
}

function Has-TextProperty {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) {
        return $false
    }
    $property = $Object.PSObject.Properties[$Name]
    return $null -ne $property -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)
}

function Property-Items {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) {
        return @()
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return @()
    }
    return @($property.Value)
}

function Same-StringSet {
    param([object[]]$Actual, [object[]]$Expected)
    $actualValues = @($Actual | ForEach-Object { [string]$_ } | Where-Object { $_ } | Sort-Object -Unique)
    $expectedValues = @($Expected | ForEach-Object { [string]$_ } | Where-Object { $_ } | Sort-Object -Unique)
    return ($actualValues -join "`n") -ceq ($expectedValues -join "`n")
}

$learningJson = Read-RepositoryText "agent/bricscad-learning/brx-learning.json"
$learning = $null
if ($learningJson) {
    try {
        $learning = $learningJson | ConvertFrom-Json
    }
    catch {
        Add-ContractFailure "agent/bricscad-learning/brx-learning.json is not valid JSON: $($_.Exception.Message)"
    }
}

$expectedTools = @(
    "bim.objects.query",
    "bim.selection.set",
    "bim.move",
    "bim.rotate"
)

if ($null -ne $learning) {
    $profiles = @(Property-Items $learning "toolProfiles")
    $lessons = @(Property-Items $learning "lessons")
    Assert-Contract ($profiles.Count -gt 0) "JSON contract: toolProfiles must be a non-empty array."
    Assert-Contract ($lessons.Count -gt 0) "JSON contract: lessons must be a non-empty array."

    $newProfiles = @($profiles | Where-Object { $expectedTools -ccontains [string]$_.name })
    Assert-Contract ($newProfiles.Count -eq 4) "JSON contract: expected exactly four new BIM tool profiles; found $($newProfiles.Count)."

    foreach ($toolName in $expectedTools) {
        $matches = @($profiles | Where-Object { [string]$_.name -ceq $toolName })
        Assert-Contract ($matches.Count -eq 1) "JSON contract: tool profile '$toolName' must exist exactly once; found $($matches.Count)."
        if ($matches.Count -ne 1) {
            continue
        }

        $profile = $matches[0]
        Assert-Contract ([string]$profile.domain -ceq "bim") "JSON contract: '$toolName'.domain must be 'bim'."
        foreach ($textField in @("summary", "policy")) {
            Assert-Contract (Has-TextProperty $profile $textField) "JSON contract: '$toolName'.$textField must be non-empty."
        }
        Assert-Contract ([string]$profile.createdAt -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "JSON contract: '$toolName'.createdAt must use UTC ISO-8601 milliseconds."
        Assert-Contract ([string]$profile.updatedAt -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "JSON contract: '$toolName'.updatedAt must use UTC ISO-8601 milliseconds."
        foreach ($arrayField in @("keywords", "agentHints", "semanticConstraints", "unsupportedOperations", "examples")) {
            Assert-Contract (@(Property-Items $profile $arrayField).Count -gt 0) "JSON contract: '$toolName'.$arrayField must be non-empty."
        }
    }

    $lessonContracts = [ordered]@{
        "workflow_bim_objects_query" = @{
            FinalTool = "bim.objects.query"
            RequiredSlots = @()
            ExpectedTools = @("bim.objects.query")
        }
        "workflow_bim_selection" = @{
            FinalTool = "bim.selection.set"
            RequiredSlots = @("bimSelector")
            ExpectedTools = @("bim.objects.query", "bim.selection.set")
        }
        "workflow_bim_move" = @{
            FinalTool = "bim.move"
            RequiredSlots = @("bimSelector", "moveVectorMm")
            ExpectedTools = @("bim.objects.query", "bim.move")
        }
        "workflow_bim_rotate" = @{
            FinalTool = "bim.rotate"
            RequiredSlots = @("bimSelector", "angleDeg", "basePointMode")
            ExpectedTools = @("bim.objects.query", "bim.rotate")
        }
    }

    $bimLessons = @($lessons | Where-Object { [string]$_.id -like "workflow_bim_*" })
    Assert-Contract ($bimLessons.Count -eq 4) "JSON contract: expected exactly four workflow_bim_* lessons; found $($bimLessons.Count)."

    foreach ($lessonId in $lessonContracts.Keys) {
        $contract = $lessonContracts[$lessonId]
        $matches = @($lessons | Where-Object { [string]$_.id -ceq $lessonId })
        Assert-Contract ($matches.Count -eq 1) "JSON contract: lesson '$lessonId' must exist exactly once; found $($matches.Count)."
        if ($matches.Count -ne 1) {
            continue
        }

        $lesson = $matches[0]
        Assert-Contract ([string]$lesson.status -ceq "active") "JSON contract: '$lessonId'.status must be 'active'."
        Assert-Contract ([string]$lesson.source -ceq "canonical_bim_workflow") "JSON contract: '$lessonId'.source must be 'canonical_bim_workflow'."
        Assert-Contract ($lesson.updateProtected -eq $true) "JSON contract: '$lessonId'.updateProtected must be true."
        Assert-Contract ([string]$lesson.createdAt -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "JSON contract: '$lessonId'.createdAt must be UTC ISO-8601 with milliseconds."
        Assert-Contract ([string]$lesson.updatedAt -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "JSON contract: '$lessonId'.updatedAt must be UTC ISO-8601 with milliseconds."
        foreach ($textField in @("title", "intent", "description")) {
            Assert-Contract (Has-TextProperty $lesson $textField) "JSON contract: '$lessonId'.$textField must be non-empty."
        }

        $recommendedTools = @(Property-Items $lesson "recommendedTools")
        Assert-Contract (Same-StringSet $recommendedTools $contract.ExpectedTools) "JSON contract: '$lessonId'.recommendedTools must be exactly [$($contract.ExpectedTools -join ', ')]."

        $requiredSlotNames = @(Property-Items $lesson "requiredSlots" | ForEach-Object { [string]$_.name })
        Assert-Contract (Same-StringSet $requiredSlotNames $contract.RequiredSlots) "JSON contract: '$lessonId' required slots must be exactly [$($contract.RequiredSlots -join ', ')]."
        $confirmationSlots = @(Property-Items $lesson "requiresUserConfirmationInAgentMode")
        Assert-Contract (Same-StringSet $confirmationSlots $contract.RequiredSlots) "JSON contract: '$lessonId' confirmation slots must match its required slots."

        $batches = @(Property-Items $lesson "executionBatches")
        Assert-Contract ($batches.Count -gt 0) "JSON contract: '$lessonId' must define at least one execution batch."
        $steps = @()
        foreach ($batch in $batches) {
            Assert-Contract ([string]$batch.mode -ceq "sequential") "JSON contract: '$lessonId' batches must use mode=sequential."
            Assert-Contract ($batch.stopOnFailure -eq $true) "JSON contract: '$lessonId' batches must set stopOnFailure=true."
            $steps += @(Property-Items $batch "steps")
        }
        $stepTools = @($steps | ForEach-Object { [string]$_.tool })
        Assert-Contract (($stepTools -join "`n") -ceq ($contract.ExpectedTools -join "`n")) "JSON contract: '$lessonId' execution tools must be ordered as [$($contract.ExpectedTools -join ', ')]."

        $actionShapes = @(Property-Items $lesson "validActionShapes")
        $shapeTools = @($actionShapes | ForEach-Object { [string]$_.tool })
        Assert-Contract (($shapeTools -join "`n") -ceq ($contract.ExpectedTools -join "`n")) "JSON contract: '$lessonId' validActionShapes must mirror its ordered tools."

        $queryStep = @($steps | Where-Object { [string]$_.tool -ceq "bim.objects.query" }) | Select-Object -First 1
        if ($null -eq $queryStep) {
            Add-ContractFailure "JSON contract: '$lessonId' must resolve targets through bim.objects.query."
        }
        else {
            Assert-Contract ([int]$queryStep.paramsTemplate.limit -eq 100) "JSON contract: '$lessonId' query limit must be 100."
            Assert-Contract (@(Property-Items $queryStep.paramsTemplate "include").Count -gt 0) "JSON contract: '$lessonId' query must declare include fields."
        }

        $finalStep = @($steps | Where-Object { [string]$_.tool -ceq [string]$contract.FinalTool }) | Select-Object -Last 1
        if ($null -eq $finalStep) {
            Add-ContractFailure "JSON contract: '$lessonId' is missing final tool '$($contract.FinalTool)'."
        }
        elseif ($contract.FinalTool -cne "bim.objects.query") {
            Assert-Contract ($finalStep.paramsTemplate.autoBimHandlesFromLastQuery -eq $true) "JSON contract: '$lessonId' must bind exact handles via autoBimHandlesFromLastQuery=true."
        }

        if ($contract.FinalTool -ceq "bim.move") {
            Assert-Contract ([string]$finalStep.paramsTemplate.units -ceq "mm") "JSON contract: '$lessonId' move units must default to mm."
            Assert-Contract ([string]$finalStep.paramsTemplate.vector -ceq "{{moveVectorMm}}") "JSON contract: '$lessonId' must bind vector from {{moveVectorMm}}."
            Assert-Contract ($finalStep.paramsTemplate.saveBefore -eq $true) "JSON contract: '$lessonId' must set saveBefore=true."
        }
        elseif ($contract.FinalTool -ceq "bim.rotate") {
            Assert-Contract ([string]$finalStep.paramsTemplate.angleDeg -ceq "{{angleDeg}}") "JSON contract: '$lessonId' must bind angleDeg from {{angleDeg}}."
            Assert-Contract ([string]$finalStep.paramsTemplate.basePointMode -ceq "{{basePointMode}}") "JSON contract: '$lessonId' must bind basePointMode from {{basePointMode}}."
            Assert-Contract ($finalStep.paramsTemplate.saveBefore -eq $true) "JSON contract: '$lessonId' must set saveBefore=true."
        }

        $serializedLesson = $lesson | ConvertTo-Json -Depth 30 -Compress
        Assert-Contract ($serializedLesson -cnotmatch '"handles"\s*:') "JSON contract: '$lessonId' must not persist production handles."
    }

    foreach ($lesson in $lessons) {
        foreach ($example in @(Property-Items $lesson "validationExamples")) {
            Assert-Contract ([string]$example.title -cne "Aus erfolgreicher BRX-Ausfuehrung") "Learning contract: runtime example '$([string]$example.id)' needs a specific execution title."
        }
    }
}

$indexHtml = Read-RepositoryText "index.html"
if ($indexHtml) {
    Assert-Contract ($indexHtml -cnotmatch 'workflow-group-title') "Overlay contract: workflow-group-title must not exist."
    $renderStart = $indexHtml.IndexOf("function renderWorkflowList()", [System.StringComparison]::Ordinal)
    $renderEnd = if ($renderStart -ge 0) {
        $indexHtml.IndexOf("function createWorkflowSection", $renderStart, [System.StringComparison]::Ordinal)
    } else {
        -1
    }
    Assert-Contract ($renderStart -ge 0 -and $renderEnd -gt $renderStart) "Overlay contract: renderWorkflowList() could not be isolated."
    if ($renderStart -ge 0 -and $renderEnd -gt $renderStart) {
        $renderBody = $indexHtml.Substring($renderStart, $renderEnd - $renderStart)
        Assert-Contract ($renderBody -cmatch 'const\s+visible\s*=\s*filteredWorkflows\(\)') "Overlay contract: renderWorkflowList() must consume filteredWorkflows()."
        Assert-Contract ($renderBody -cmatch 'visible\.forEach\s*\(') "Overlay contract: renderWorkflowList() must render the filtered order directly."
        Assert-Contract ($renderBody -cnotmatch '\.sort\s*\(') "Overlay contract: renderWorkflowList() must not apply a second sort."
        Assert-Contract ($renderBody -cnotmatch 'discipline') "Overlay contract: renderWorkflowList() must not group or reorder by discipline."
    }
    Assert-Contract ($indexHtml -cmatch 'workflowLearningKind\(workflowPreview\)\s*===\s*"lesson"') "Overlay contract: the execution CTA must only enable lessons."
    Assert-Contract ($indexHtml -cmatch 'workflowLearningKind\(workflowPreview\)\s*!==\s*"lesson"') "Overlay contract: activation must guard non-lesson information entries."
}

$cmakeText = Read-RepositoryText "CMakeLists.txt"
$pluginText = Read-RepositoryText "src/bricscad/BareboneBrxPlugin.cpp"
$bimHeaderText = Read-RepositoryText "src/bricscad/BimTools.h"
$bimSourceText = Read-RepositoryText "src/bricscad/BimTools.cpp"
$qtSourceText = Read-RepositoryText "src/ui/BricsCadPage.cpp"
$learningAgentText = Read-RepositoryText "src/agent/BricsCadLearningAgent.cpp"
$backendText = $pluginText + "`n" + $bimHeaderText + "`n" + $bimSourceText

if ($cmakeText) {
    Assert-Contract ($cmakeText -cmatch 'src/bricscad/BimTools\.cpp') "Build contract: CMakeLists.txt must compile src/bricscad/BimTools.cpp."
    Assert-Contract ($cmakeText -cnotmatch '(?i)Ice\.lib') "Build contract: Ice.lib must not be linked."
    Assert-Contract ($cmakeText -cmatch 'MSVC_VERSION\s+LESS\s+1930\s+OR\s+MSVC_VERSION\s+GREATER_EQUAL\s+1950') "Build contract: CMake must pin the BRX target to the v143 MSVC_VERSION range 1930..1949."
}

$riskContracts = [ordered]@{
    "bim.objects.query" = @("query", "readOnly")
    "bim.selection.set" = @("action", "modifiesEditorState")
    "bim.move" = @("action", "modifiesDrawing")
    "bim.rotate" = @("action", "modifiesDrawing")
}

foreach ($toolName in $expectedTools) {
    Assert-Contract ($backendText.Contains($toolName)) "C++ contract: backend sources must reference '$toolName'."
    Assert-Contract ($qtSourceText.Contains($toolName)) "C++ contract: Qt agent integration must reference '$toolName'."
    if ($pluginText) {
        $kind = [regex]::Escape([string]$riskContracts[$toolName][0])
        $risk = [regex]::Escape([string]$riskContracts[$toolName][1])
        $descriptorPattern = '"' + [regex]::Escape($toolName) + '"\s*,\s*"' + $kind + '"\s*,\s*"' + $risk + '"'
        Assert-Contract ($pluginText -cmatch $descriptorPattern) "C++ contract: '$toolName' descriptor must declare kind=$kind and risk=$risk."
        $dispatchPattern = 'method\s*==\s*"' + [regex]::Escape($toolName) + '"'
        Assert-Contract ($pluginText -cmatch $dispatchPattern) "C++ contract: BRX dispatch must handle '$toolName'."
    }
}

if ($pluginText) {
    Assert-Contract ($pluginText -cmatch 'kCommandContextRequests[\s\S]*?"BIMSELECTIONSET"') "C++ contract: bim.selection.set must run in BricsCAD Command Context for acedSSSetFirst."
    Assert-Contract ($pluginText -cmatch 'latestBimQueryHandles[\s\S]*?autoBimHandlesFromLastQuery') "C++ contract: actions.validate must bind auto BIM mutations to the preceding query page."
    Assert-Contract ($pluginText -cmatch 'latestBimQuerySelector[\s\S]*?ResolvePurpose::SelectionMutation[\s\S]*?ResolvePurpose::DrawingMutation') "C++ contract: auto BIM binding must preserve name ambiguity semantics for selection and drawing mutations."
    Assert-Contract ($pluginText -cmatch 'begin\s*=\s*std::min[\s\S]*?offset[\s\S]*?end\s*=\s*std::min[\s\S]*?limit') "C++ contract: BIM preflight binding must honor query offset and limit."
    Assert-Contract ($pluginText -cmatch 'return\s+"_Wall"' -and $pluginText -cmatch 'return\s+"_Column"') "C++ contract: native BIMCLASSIFY options must be locale-neutral global keywords."
}

if ($bimSourceText) {
    foreach ($requiredToken in @(
        "BimApi::isBimAvailable",
        "isLicenseAvailable",
        "BricsCAD::eBim",
        "BimClassification::getAllClassified",
        "BimClassification::getClassification",
        "BimClassification::getName",
        "BimClassification::getGUID",
        "BrxDbProperties::listAll",
        "BrxDbProperties::getValue",
        "getAcDbObjectId",
        "acedSSSetFirst",
        "transformBy"
    )) {
        Assert-Contract ($bimSourceText.Contains($requiredToken)) "C++ contract: BimTools.cpp must use '$requiredToken'."
    }
    Assert-Contract ($bimSourceText.Contains("BrxGenericPropertiesAccess.h")) "C++ contract: BimTools.cpp must include BrxGenericPropertiesAccess.h."
    Assert-Contract ($bimSourceText.Contains("AnchorFeature.h")) "C++ contract: BimTools.cpp must guard anchored BIM relationships via AnchorFeature.h."
    Assert-Contract ($bimSourceText -cmatch 'purpose\s*==\s*ResolvePurpose::DrawingMutation[\s\S]*?isEntityFromXref[\s\S]*?isEntityOnLockedLayer[\s\S]*?isAnchoredOrAnchorHost') "C++ contract: actions.validate and execution must share the DrawingMutation XRef/layer/anchor preflight."
    Assert-Contract ($bimSourceText -cmatch 'before->name\s*!=\s*after\.name') "C++ contract: BIM transforms must verify that the BIM name remains unchanged."
    Assert-Contract ($bimSourceText -cmatch '\"classification\"') "C++ contract: target fingerprints must expose the canonical classification."
    Assert-Contract ($bimSourceText -cnotmatch 'value\.get\(date\)') "C++ contract: do not call the non-exported BRX AcValue::get(SYSTEMTIME&) overload."
    Assert-Contract ($bimSourceText -cnotmatch 'value\.get\(buffer\s*,') "C++ contract: do not call the non-exported BRX AcValue::get(void*&,DWORD&) overload."
}

if ($qtSourceText) {
    $prefetchStart = $qtSourceText.IndexOf("void BricsCadPage::sendUnifiedAgentRequestWithDrawingContext", [System.StringComparison]::Ordinal)
    $prefetchEnd = if ($prefetchStart -ge 0) {
        $qtSourceText.IndexOf("`nvoid BricsCadPage::", $prefetchStart + 10, [System.StringComparison]::Ordinal)
    } else {
        -1
    }
    Assert-Contract ($prefetchStart -ge 0 -and $prefetchEnd -gt $prefetchStart) "Prefetch contract: sendUnifiedAgentRequestWithDrawingContext() could not be isolated."
    if ($prefetchStart -ge 0 -and $prefetchEnd -gt $prefetchStart) {
        $prefetchBody = $qtSourceText.Substring($prefetchStart, $prefetchEnd - $prefetchStart)
        Assert-Contract ($prefetchBody -cmatch 'bridgeCapabilitiesContainMethod\([^\r\n]*"bim\.objects\.query"') "Prefetch contract: BIM query must be capability-gated."
        Assert-Contract ($prefetchBody -cmatch '\{"method",\s*"bim\.objects\.query"\}') "Prefetch contract: standard drawing context must request bim.objects.query."
        Assert-Contract ($prefetchBody -cmatch '\{"selector",\s*QJsonObject\{\{"scope",\s*"currentSpace"\}\}\}') "Prefetch contract: BIM snapshot selector must use currentSpace."
        Assert-Contract ($prefetchBody -cmatch '\{"include",\s*QJsonArray\{"core",\s*"geometry"\}\}') "Prefetch contract: BIM snapshot must include exactly core and geometry."
        Assert-Contract ($prefetchBody -cmatch '\{"offset",\s*0\}') "Prefetch contract: BIM snapshot offset must be 0."
        Assert-Contract ($prefetchBody -cmatch '\{"limit",\s*100\}') "Prefetch contract: BIM snapshot limit must be 100."
        Assert-Contract ($prefetchBody -cmatch 'barebone\.brx\.prefetched-drawing-context\.v1') "Prefetch contract: drawing context schema v1 must be emitted."
    }
    Assert-Contract ($qtSourceText -cmatch 'compactGeometryObjectForAgent[\s\S]*?QStringLiteral\("bim"\)') "Qt contract: compactGeometryObjectForAgent() must retain the BIM object."
    Assert-Contract ($qtSourceText.Contains('QStringLiteral("objectTable")')) "Qt contract: BIM query results must expose an object table."
    Assert-Contract ($qtSourceText.Contains('QStringLiteral("propertyTable")')) "Qt contract: BIM property queries must expose a property table."
    Assert-Contract ($qtSourceText.Contains('QStringLiteral("autoBimHandlesFromLastQuery")')) "Qt contract: workflow runtime must support autoBimHandlesFromLastQuery."
    Assert-Contract ($qtSourceText -cmatch 'agentPreflightParams[\s\S]*?if\s*\(autoBimHandles\)[\s\S]*?actionParams[\s\S]*?clientActionIndex') "Qt contract: dependency-aware BRX preflight must receive auto BIM mutations with a stable action index."
    Assert-Contract ($qtSourceText -cmatch 'proposalWithBimValidationTargets[\s\S]*?clientActionIndex[\s\S]*?paramsWithBimValidationTargets') "Qt contract: BIM validation targets must map back by stable action index."
    Assert-Contract ($qtSourceText -cmatch 'handlesFromLatestBimQuery[\s\S]*?The newest BIM query is authoritative[\s\S]*?return handles;') "Qt contract: an empty newest BIM query must not fall through to stale handles."
    Assert-Contract ($qtSourceText -cmatch 'finishBrxWorkflowTest[\s\S]*?brxGeometryResultTablesMarkdown\(results\)[\s\S]*?tablesMarkdown') "Qt contract: direct BIM query lessons must render their result tables."
}

if ($learningAgentText) {
    Assert-Contract ($learningAgentText -cmatch 'conciseLearningTitle') "Learning contract: runtime lessons must normalize a concise specific title."
    Assert-Contract ($learningAgentText -cmatch 'conciseExecutionDescription') "Learning contract: runtime lessons must store a concise execution description."
    Assert-Contract ($learningAgentText -cmatch 'compactLesson[\s\S]*?QStringLiteral\("description"\)[\s\S]*?lesson\.value\(QStringLiteral\("description"\)\)') "Learning contract: workflow cards must expose the stored short description instead of duplicating the long intent."
}

if ($script:Failures.Count -gt 0) {
    Write-Host "BIM contract check FAILED ($($script:Failures.Count) problem(s)):" -ForegroundColor Red
    foreach ($failure in $script:Failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "BIM contract check passed: 4 tool profiles, 4 lessons, flat overlay, BRX wiring and 100-object prefetch." -ForegroundColor Green
exit 0
