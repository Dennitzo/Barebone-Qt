param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$QtRoot = 'C:\Qt',
    [string]$QtKitPath,
    [string]$CMakePath = 'C:\Program Files\CMake\bin\cmake.exe',
    [switch]$Clean
)

. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-ProjectRoot
& (Join-Path $PSScriptRoot 'build-app.ps1') -Configuration $Configuration -QtRoot $QtRoot -QtKitPath $QtKitPath -CMakePath $CMakePath -Clean:$Clean
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$exe = Get-ChildItem -LiteralPath (Join-Path $root 'build') -Recurse -Filter 'Barebone-Qt.exe' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (!$exe) {
    throw "Barebone-Qt.exe not found."
}

$artifactRoot = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null
$zipPath = Join-Path $artifactRoot "Barebone-Qt-$Configuration.zip"
$stageDir = Join-Path $artifactRoot "Barebone-Qt-$Configuration"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Remove-SafeDirectory $stageDir
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

$appStageDir = Join-Path $stageDir 'app'
Copy-Item -LiteralPath $exe.DirectoryName -Destination $appStageDir -Recurse -Force

$bridgeOutput = Join-Path $root 'build\revit-bridge'
if (Test-Path -LiteralPath $bridgeOutput) {
    Copy-Item -LiteralPath $bridgeOutput -Destination (Join-Path $stageDir 'revit-bridge') -Recurse -Force
}

Copy-Item -LiteralPath (Join-Path $root 'windows\install-revit-addin.ps1') -Destination $stageDir -Force

Write-Step "Packaging Barebone-Qt"
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath
Write-Host "Artifact: $zipPath"
