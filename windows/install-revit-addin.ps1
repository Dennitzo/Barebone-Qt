param(
    [string]$AssemblyPath,
    [switch]$BuildFirst,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('User', 'Machine', 'Both')]
    [string]$Scope = 'Machine'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-ProjectRoot
$project = Join-Path $root 'src\revit\bridge\Barebone.RevitBridge\Barebone.RevitBridge.csproj'
$outputDir = Join-Path $root 'build\revit-bridge'

if ($BuildFirst) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    Invoke-Tool dotnet 'build' $project '-c' $Configuration '-o' $outputDir
}

if (!$AssemblyPath) {
    $AssemblyPath = Join-Path $outputDir 'Barebone.RevitBridge.dll'
}

$assembly = Resolve-Path -LiteralPath $AssemblyPath -ErrorAction Stop
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<RevitAddIns>
  <AddIn Type="Application">
    <Name>Barebone Revit Bridge</Name>
    <Assembly>$($assembly.Path)</Assembly>
    <ClientId>2d8b1f23-5d71-4d5d-b59f-6db059c81f01</ClientId>
    <FullClassName>Barebone.RevitBridge.App</FullClassName>
    <VendorId>BBQT</VendorId>
    <VendorDescription>Barebone-Qt local Revit bridge</VendorDescription>
  </AddIn>
  <ManifestSettings>
    <UseRevitContext>False</UseRevitContext>
  </ManifestSettings>
</RevitAddIns>
"@

function Write-Manifest {
    param([Parameter(Mandatory = $true)][string]$Directory)
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $manifestPath = Join-Path $Directory 'BareboneRevit.addin'
    [System.IO.File]::WriteAllText($manifestPath, $manifest, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Revit add-in manifest: $manifestPath"
}

if ($Scope -eq 'User' -or $Scope -eq 'Both') {
    Write-Manifest (Join-Path $env:APPDATA 'Autodesk\Revit\Addins\2026')
}
if ($Scope -eq 'Machine' -or $Scope -eq 'Both') {
    Write-Manifest 'C:\ProgramData\Autodesk\Revit\Addins\2026'
    if ($Scope -eq 'Machine') {
        $userManifest = Join-Path $env:APPDATA 'Autodesk\Revit\Addins\2026\BareboneRevit.addin'
        if (Test-Path -LiteralPath $userManifest) {
            Remove-Item -LiteralPath $userManifest -Force
            Write-Host "Removed duplicate user manifest: $userManifest"
        }
    }
}
Write-Host "Assembly: $($assembly.Path)"
