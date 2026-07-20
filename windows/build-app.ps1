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
$cmake = Resolve-CMakeExecutable -CMakePath $CMakePath
$qtKit = Resolve-QtKit -QtRoot $QtRoot -QtKitPath $QtKitPath
Initialize-BuildEnvironment -QtKit $qtKit
$msvcToolsetVersion = $null
if ($qtKit.Name -like 'msvc*') {
    $msvcToolsetVersion = Resolve-V143ToolsetVersion
    Import-VisualStudioEnvironment -ToolsetVersion $msvcToolsetVersion
}

$ninja = Resolve-NinjaExecutable

$compilerName = $qtKit.Name
if ($compilerName -like 'msvc*') {
    $compilerName = 'msvc-v143'
}
$buildDirName = "windows-$($qtKit.Version)-$compilerName"
$buildDir = Join-Path $root "build\$buildDirName"
$revitBuildDir = Join-Path $root 'build\revit-bridge'
if ($Clean) {
    Remove-SafeDirectory $buildDir
    Remove-SafeDirectory $revitBuildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $revitBuildDir | Out-Null

Write-Step "Configuring Barebone-Qt with Qt $($qtKit.Version) $($qtKit.Name)"
if ($ninja) {
    Add-PathEntry (Split-Path -Parent $ninja)
    Invoke-Tool $cmake `
        '-S' $root `
        '-B' $buildDir `
        '-G' 'Ninja' `
        "-DCMAKE_BUILD_TYPE=$Configuration" `
        "-DCMAKE_PREFIX_PATH=$($qtKit.Path)" `
        "-DCMAKE_MAKE_PROGRAM=$ninja"
} else {
    Invoke-Tool $cmake `
        '-S' $root `
        '-B' $buildDir `
        '-G' 'Visual Studio 17 2022' `
        '-A' 'x64' `
        "-DCMAKE_PREFIX_PATH=$($qtKit.Path)"
}

Write-Step "Building Barebone-Qt ($Configuration)"
Invoke-Tool $cmake '--build' $buildDir '--config' $Configuration '--parallel'

$exe = Get-ChildItem -LiteralPath $buildDir -Recurse -Filter 'Barebone-Qt.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName |
    Select-Object -First 1
if ($exe) {
    Write-Host "Windows executable: $($exe.FullName)"
} else {
    Write-Warning "Build completed but Barebone-Qt.exe was not found below $buildDir"
}

Write-Step "Building Barebone Revit bridge"
$dotnetConfiguration = if ($Configuration -eq 'Debug') { 'Debug' } else { 'Release' }
$revitProject = Join-Path $root 'src\revit\bridge\Barebone.RevitBridge\Barebone.RevitBridge.csproj'
Invoke-Tool dotnet 'build' $revitProject '-c' $dotnetConfiguration '-o' $revitBuildDir

$revitBridge = Join-Path $revitBuildDir 'Barebone.RevitBridge.dll'
if (Test-Path -LiteralPath $revitBridge) {
    Write-Host "Revit bridge: $revitBridge"
} else {
    Write-Warning "Build completed but Barebone.RevitBridge.dll was not found below $revitBuildDir"
}
