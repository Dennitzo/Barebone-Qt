$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Read-Text([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw -Encoding UTF8 }
$plugin = Read-Text 'src/bricscad/BareboneBrxPlugin.cpp'
$agent = Read-Text 'src/agent/BrxAgent.cpp'
$page = Read-Text 'src/ui/BricsCadPage.cpp'

if ($plugin -notmatch 'capabilities\.list') { throw 'BRX plugin does not expose capabilities.list.' }
if ($agent -notmatch 'runtimeToolsWithSdkTools') { throw 'BRX SDK functions are not retained.' }
if ($page -notmatch 'validateAgentAction') { throw 'Action validation is missing.' }
if ($page -notmatch 'actions\.validate') { throw 'BRX preflight validation is missing.' }
Write-Host 'Native BRX/BIM contract passed.'
