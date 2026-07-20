param(
    [string]$QtRoot = 'C:\Qt',
    [string]$QtKitPath,
    [string]$CMakePath = 'C:\Program Files\CMake\bin\cmake.exe'
)

. (Join-Path $PSScriptRoot 'common.ps1')

$cmake = Resolve-CMakeExecutable -CMakePath $CMakePath
$qtKit = Resolve-QtKit -QtRoot $QtRoot -QtKitPath $QtKitPath
Initialize-BuildEnvironment -QtKit $qtKit
if ($qtKit.Name -like 'msvc*') {
    Import-VisualStudioEnvironment
}
$ninja = Resolve-NinjaExecutable

Write-Step "Barebone-Qt Windows build environment"
Write-Host "CMake: $cmake"
Write-Host "Qt kit: $($qtKit.Path)"
if ($ninja) {
    Write-Host "Ninja: $ninja"
} else {
    Write-Host "Ninja: not found"
}

if (!$ninja) {
    Write-Host "Ninja is optional. build-app.ps1 will use Visual Studio 17 2022 as fallback."
}

Write-Step "Revit bridge environment"
$revitRoot = 'C:\Program Files\Autodesk\Revit 2026'
$revitApi = Join-Path $revitRoot 'RevitAPI.dll'
$revitApiUi = Join-Path $revitRoot 'RevitAPIUI.dll'
Write-Host "Revit 2026: $revitRoot"
Write-Host "RevitAPI.dll: $(if (Test-Path -LiteralPath $revitApi) { $revitApi } else { 'missing' })"
Write-Host "RevitAPIUI.dll: $(if (Test-Path -LiteralPath $revitApiUi) { $revitApiUi } else { 'missing' })"
Invoke-Tool dotnet '--list-sdks'
Invoke-Tool dotnet '--list-runtimes'
