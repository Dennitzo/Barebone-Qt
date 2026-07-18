[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$script:Failures = @()
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Add-Failure {
    param([Parameter(Mandatory = $true)][string]$Message)
    $script:Failures += $Message
}

function Assert-Contract {
    param([Parameter(Mandatory = $true)][bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { Add-Failure $Message }
}

function Read-Text {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-Failure "Missing required file: $RelativePath"
        return ""
    }
    return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

function Missing-File {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    return -not (Test-Path -LiteralPath (Join-Path $repoRoot $RelativePath) -PathType Leaf)
}

function Items {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return @() }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return @() }
    return @($property.Value)
}

function Has-Text {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $false }
    $property = $Object.PSObject.Properties[$Name]
    return $null -ne $property -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)
}

$learningText = Read-Text "agent/bricscad-learning/brx-learning.json"
$learning = $null
if ($learningText) {
    try { $learning = $learningText | ConvertFrom-Json }
    catch { Add-Failure "brx-learning.json is not valid JSON: $($_.Exception.Message)" }
}

$qtText = Read-Text "src/ui/BricsCadPage.cpp"
$pluginText = Read-Text "src/bricscad/BareboneBrxPlugin.cpp"
$bimText = Read-Text "src/bricscad/BrxBimSdk.cpp"
$brxAgentText = Read-Text "src/agent/BrxAgent.cpp"
$cmakeText = Read-Text "CMakeLists.txt"
$removedBimStem = "Bim" + "Tools"

if ($learning) {
    $lessons = @(Items $learning "lessons")
    Assert-Contract ($lessons.Count -eq 11) "JSON contract: BIM move workflow must be removed and selector reference workflow must exist; expected 11 lessons."
    Assert-Contract ([int]$learning.metadata.lessonCount -eq $lessons.Count) "JSON metadata: lessonCount must match actual lessons."
    Assert-Contract ([int]$learning.metadata.activeCount -eq @($lessons | Where-Object { $_.active -ne $false }).Count) "JSON metadata: activeCount must match active lessons."
    Assert-Contract ([int]$learning.metadata.protectedWorkflowCount -eq @($lessons | Where-Object { $_.updateProtected -eq $true }).Count) "JSON metadata: protectedWorkflowCount must match protected lessons."
    Assert-Contract ([int]$learning.metadata.aiOwnedWorkflowCount -eq @($lessons | Where-Object { [string]$_.source -ceq "ai_runtime" }).Count) "JSON metadata: aiOwnedWorkflowCount must match AI-owned lessons."
    Assert-Contract ([int]$learning.metadata.validationExampleCount -eq ($lessons | ForEach-Object { @(Items $_ "validationExamples").Count } | Measure-Object -Sum).Sum) "JSON metadata: validationExampleCount must match examples."
    Assert-Contract ([int]$learning.metadata.repairRuleCount -eq @(Items $learning "repairRules").Count) "JSON metadata: repairRuleCount must match repairRules."
    Assert-Contract (-not $learningText.Contains('"toolProfiles"')) "JSON contract: toolProfiles must not be stored in brx-learning.json."
    Assert-Contract (-not $learningText.Contains('"toolProfileCount"')) "JSON contract: toolProfileCount metadata must not exist."

    Assert-Contract (@($lessons | Where-Object { [string]$_.id -ceq "workflow_bim_move" }).Count -eq 0) "JSON contract: workflow_bim_move must not exist."
    Assert-Contract (@($lessons | Where-Object { [string]$_.source -ceq "drawing_ai_runtime" }).Count -eq 0) "JSON contract: drawing_ai_runtime lessons must not exist."

    foreach ($lessonId in @("workflow_bim_objects_query", "workflow_bim_selection")) {
        $matches = @($lessons | Where-Object { [string]$_.id -ceq $lessonId })
        Assert-Contract ($matches.Count -eq 1) "JSON contract: $lessonId must exist exactly once."
        if ($matches.Count -eq 1) {
            $lesson = $matches[0]
            Assert-Contract ([string]$lesson.source -ceq "canonical_bim_workflow") "JSON contract: $lessonId must stay canonical."
            Assert-Contract ($lesson.updateProtected -eq $true) "JSON contract: $lessonId must stay updateProtected."
            Assert-Contract (Has-Text $lesson "title") "JSON contract: $lessonId needs title."
            Assert-Contract (Has-Text $lesson "intent") "JSON contract: $lessonId needs intent."
            Assert-Contract (@(Items $lesson "executionBatches").Count -gt 0) "JSON contract: $lessonId needs executionBatches."
        }
    }

    $selectorReference = @($lessons | Where-Object { [string]$_.id -ceq "workflow_brx_selector_reference" })
    Assert-Contract ($selectorReference.Count -eq 1) "JSON contract: workflow_brx_selector_reference must exist exactly once."
    if ($selectorReference.Count -eq 1) {
        $lesson = $selectorReference[0]
        Assert-Contract ([string]$lesson.source -ceq "canonical_brx_workflow") "JSON contract: workflow_brx_selector_reference must stay canonical BRX."
        Assert-Contract ($lesson.updateProtected -eq $true) "JSON contract: workflow_brx_selector_reference must stay updateProtected."
        Assert-Contract (Has-Text $lesson "title") "JSON contract: workflow_brx_selector_reference needs title."
        Assert-Contract ($learningText.Contains("scope`": `"handles")) "JSON contract: selector reference must document handle selectors."
        Assert-Contract ($learningText.Contains("scope`": `"selection")) "JSON contract: selector reference must document selection selectors."
        Assert-Contract ($learningText.Contains("scope`": `"lastResult")) "JSON contract: selector reference must document lastResult selectors."
    }

    Assert-Contract (-not $learningText.Contains("MANIP_MOVE")) "JSON contract: learning data must not hardcode MANIP_MOVE."
    Assert-Contract (-not $learningText.Contains("workflow_bim_move")) "JSON contract: learning data must not reference workflow_bim_move."
}

if ($cmakeText) {
    Assert-Contract ($cmakeText.Contains("src/agent/BrxAgent.cpp")) "Build contract: CMake must compile BrxAgent.cpp."
    Assert-Contract ($cmakeText.Contains("src/bricscad/BrxBimSdk.cpp")) "Build contract: CMake must compile BrxBimSdk.cpp."
    Assert-Contract (-not $cmakeText.Contains("src/bricscad/$removedBimStem.cpp")) "Build contract: removed $removedBimStem.cpp must not be compiled."
    Assert-Contract (-not $cmakeText.Contains("src/bricscad/$removedBimStem.h")) "Build contract: removed $removedBimStem.h must not be compiled."
}

Assert-Contract (Missing-File "src/bricscad/$removedBimStem.cpp") "Build contract: removed $removedBimStem.cpp file still exists."
Assert-Contract (Missing-File "src/bricscad/$removedBimStem.h") "Build contract: removed $removedBimStem.h file still exists."

if ($brxAgentText) {
    Assert-Contract ($brxAgentText.Contains("buildToolCatalog")) "BrxAgent contract: BrxAgent must build the AI-facing tool catalog."
    Assert-Contract ($brxAgentText.Contains("selectToolsForRoute")) "BrxAgent contract: BrxAgent must own route-specific tool selection."
    foreach ($token in @(
        "qt.brx.tools.describe",
        "qt.brx.db.schema",
        "qt.brx.db.query",
        "qt.brx.db.inspect",
        "qt.brx.db.compatibility",
        "qt.brx.workflow.testPlan",
        "qt.brx.workflow.repairHints",
        "brx.sdk.entity.transformBy",
        "brx.sdk.blockReference.setPosition",
        "brx.sdk.entity.copy",
        "brx.sdk.entity.rotateBy",
        "brx.sdk.entity.scaleBy",
        "brx.sdk.entity.erase",
        "brx.sdk.entity.setLayer",
        "brx.sdk.entity.setName",
        "brx.sdk.selection.setPickfirst",
        "brx.sdk.bim.classification.set",
        "brx.sdk.assoc.evaluate"
    )) {
        Assert-Contract ($brxAgentText.Contains($token)) "BrxAgent contract: missing $token."
    }
}

if ($qtText) {
    Assert-Contract ($qtText.Contains("#include `"../agent/BrxAgent.h`"")) "Qt contract: BricsCadPage must include BrxAgent."
    Assert-Contract ($qtText.Contains("BrxAgent::buildToolCatalog")) "Qt contract: available tools must be built by BrxAgent."
    Assert-Contract ($qtText.Contains("BrxAgent::selectToolsForRoute")) "Qt contract: route tool selection must be delegated to BrxAgent."
    Assert-Contract (-not $qtText.Contains("toolProfile(")) "Qt contract: BricsCadPage must not enrich tools from learning toolProfiles."
    Assert-Contract ($qtText.Contains("qt.brx.db.compatibility")) "Qt contract: context broker must expose BRX DB compatibility."
    Assert-Contract (-not $qtText.Contains("geometry.move.status")) "Qt contract: private BIM move polling endpoint must be removed."
    Assert-Contract (-not $qtText.Contains("BBGEOMETRYMOVE")) "Qt contract: BBGEOMETRYMOVE must not be referenced."
}

if ($pluginText) {
    Assert-Contract ($pluginText.Contains("namespace BrxToolRegistry")) "BRX contract: bridge descriptors must live in BrxToolRegistry."
    Assert-Contract (-not $pluginText.Contains("jsonCapabilitiesResponse(")) "BRX contract: old static capabilities response must be removed."
    foreach ($token in @(
        '"brx.sdk.assoc.evaluate"',
        "BRXASSOCEVALUATE",
        "AcDbAssocManager::evaluateTopLevelNetwork",
        "moveEntityWithBrxSdk",
        "validateGeometryMoveReadback"
    )) {
        Assert-Contract ($pluginText.Contains($token)) "BRX contract: missing $token."
    }
    foreach ($removed in @(
        "geometry.move.status",
        "BBGEOMETRYMOVE",
        "runNativeManipMoveCommand",
        "queueGeometryMoveJob",
        "GeometryMoveJob",
        "g_geometryMoveJobs",
        "hostSurfaceHint",
        "appendHostSurfacesJson",
        "appendAnchorSearchJson"
    )) {
        Assert-Contract (-not $pluginText.Contains($removed)) "BRX contract: removed fixed BIM move symbol still exists: $removed."
    }
}

if ($bimText) {
    foreach ($token in @(
        "BimApi::isBimAvailable",
        "BimClassification::getAllClassified",
        "BimClassification::getClassification",
        "BimClassification::getName",
        "BimClassification::getGUID",
        "BrxDbProperties::listAll",
        "getAcDbObjectId",
        "isAnchoredBlockRef",
        "getAnchorFace",
        "getAnchoredBlockReferences"
    )) {
        Assert-Contract ($bimText.Contains($token)) "BIM contract: BrxBimSdk.cpp must keep SDK support for $token."
    }
}

if ($script:Failures.Count -gt 0) {
    Write-Error ("BIM contract failed:`n - " + ($script:Failures -join "`n - "))
    exit 1
}

Write-Host "BIM contract OK"
