$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Read-Text([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw -Encoding UTF8 }
$cmake = Read-Text 'CMakeLists.txt'
$qrc = Read-Text 'resources/resources.qrc'
$agent = Read-Text 'src/agent/BrxAgent.cpp'
$page = Read-Text 'src/ui/BricsCadPage.cpp'
$index = Read-Text 'index.html'

if ($cmake -match 'BricsCadLearningAgent' -or $qrc -match 'bricscad-learning') { throw 'Learning resources are still registered.' }
if ($agent -notmatch 'capabilities\.value\(QStringLiteral\("methods"\)\)') { throw 'BRX tools are not built from runtime capabilities.' }
if ($agent -match 'tools\.append\(layersEnsureManyTool|tools\.append\(bimCreateTool') { throw 'Virtual hardcoded tools are still appended.' }
if ($page -match 'm_brxLearning|qt\.learning|brxLearningContext') { throw 'BricsCAD learning runtime is still referenced.' }
if ($index -notmatch 'const showWorkflowButton = isChatWorkspace\(\)') { throw 'Workflow overlay is not restricted to Chat.' }
Write-Host 'BRX capability routing contract passed.'
