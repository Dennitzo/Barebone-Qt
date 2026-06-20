param(
    [string]$QtRoot = 'C:\Qt',
    [string]$QtKitPath,
    [string]$CMakePath = 'C:\Program Files\CMake\bin\cmake.exe'
)

. (Join-Path $PSScriptRoot 'common.ps1')

$cmake = Resolve-CMakeExecutable -CMakePath $CMakePath
$qtKit = Resolve-QtKit -QtRoot $QtRoot -QtKitPath $QtKitPath
Initialize-BuildEnvironment -QtKit $qtKit
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
