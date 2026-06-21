function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Get-ProjectRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Add-PathEntry {
    param([string]$Path)
    if ($Path -and (Test-Path -LiteralPath $Path) -and (($env:Path -split ';') -notcontains $Path)) {
        $env:Path = "$Path;$env:Path"
    }
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE"
    }
}

function Remove-SafeDirectory {
    param([string]$Path)
    if (!$Path) {
        return
    }
    $resolvedParent = Resolve-Path -LiteralPath (Split-Path -Parent $Path) -ErrorAction SilentlyContinue
    if (!$resolvedParent) {
        return
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath((Get-ProjectRoot))
    if (!$fullPath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside project root: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

function Resolve-CMakeExecutable {
    param([string]$CMakePath)
    if ($CMakePath -and (Test-Path -LiteralPath $CMakePath)) {
        return $CMakePath
    }
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmake) {
        return $cmake.Source
    }
    throw "cmake.exe not found. Install CMake or pass -CMakePath."
}

function Resolve-NinjaExecutable {
    $ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($ninja) {
        return $ninja.Source
    }
    $wingetPackages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path -LiteralPath $wingetPackages) {
        $wingetNinja = Get-ChildItem -LiteralPath $wingetPackages -Recurse -Filter 'ninja.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*Ninja-build.Ninja*' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($wingetNinja) {
            return $wingetNinja.FullName
        }
    }
    return $null
}

function Resolve-QtKit {
    param(
        [string]$QtRoot = 'C:\Qt',
        [string]$QtKitPath
    )

    if ($QtKitPath) {
        $path = Resolve-Path -LiteralPath $QtKitPath -ErrorAction Stop
        return [PSCustomObject]@{
            Path = $path.Path
            Version = Split-Path -Leaf (Split-Path -Parent $path.Path)
            Name = Split-Path -Leaf $path.Path
        }
    }

    $kits = Get-ChildItem -LiteralPath $QtRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^6\.' } |
        ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'lib\cmake\Qt6\Qt6Config.cmake') } |
                ForEach-Object {
                    [PSCustomObject]@{
                        Path = $_.FullName
                        Version = Split-Path -Leaf (Split-Path -Parent $_.FullName)
                        Name = $_.Name
                    }
                }
        } |
        Sort-Object Version, Name -Descending

    $kit = $kits | Select-Object -First 1
    if (!$kit) {
        throw "No Qt6 kit found below $QtRoot. Pass -QtKitPath to a Qt kit directory."
    }
    return $kit
}

function Initialize-BuildEnvironment {
    param($QtKit)
    Add-PathEntry (Join-Path $QtKit.Path 'bin')
}

function Import-VisualStudioEnvironment {
    if ($env:VCINSTALLDIR -and $env:WindowsSdkDir) {
        return
    }

    $vsDevCmdCandidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat'
    )

    $vsDevCmd = $vsDevCmdCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$vsDevCmd) {
        throw "Visual Studio Developer Command Prompt was not found. Install Visual Studio C++ tools or run from an x64 Developer PowerShell."
    }

    $tempScript = Join-Path ([System.IO.Path]::GetTempPath()) "barebone-vsdevcmd-$PID.cmd"
    @(
        '@echo off',
        "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul",
        'set'
    ) | Set-Content -LiteralPath $tempScript -Encoding ASCII
    try {
        $environment = & cmd.exe /d /c "`"$tempScript`""
    } finally {
        Remove-Item -LiteralPath $tempScript -Force -ErrorAction SilentlyContinue
    }
    foreach ($line in $environment) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
