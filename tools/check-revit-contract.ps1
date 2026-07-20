$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

function Read-RepoFile {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    return Get-Content -LiteralPath (Join-Path $root $RelativePath) -Raw
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Fragment,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if ($Text.IndexOf($Fragment, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "Revit contract missing ($Description): $Fragment"
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Fragment,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if ($Text.IndexOf($Fragment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Revit contract violation ($Description): $Fragment"
    }
}

$agentCpp = Read-RepoFile "src/revit/RevitAgent.cpp"
$agentHeader = Read-RepoFile "src/revit/RevitAgent.h"
$chatCpp = Read-RepoFile "src/ui/ChatPage.cpp"
$chatHeader = Read-RepoFile "src/ui/ChatPage.h"
$mainHeader = Read-RepoFile "src/ui/MainWindow.h"
$bridgeProject = Read-RepoFile "src/revit/bridge/Barebone.RevitBridge/Barebone.RevitBridge.csproj"
$bridgeClient = Read-RepoFile "src/revit/bridge/Barebone.RevitBridge/BridgeClient.cs"
$bridgeHandler = Read-RepoFile "src/revit/bridge/Barebone.RevitBridge/BridgeExternalEventHandler.cs"
$installScript = Read-RepoFile "windows/install-revit-addin.ps1"
$buildScript = Read-RepoFile "windows/build-app.ps1"
$cmake = Read-RepoFile "CMakeLists.txt"

Assert-Contains $agentCpp "constexpr quint16 kBridgePort = 47627" "Qt Revit bridge port"
Assert-Contains $agentCpp "BareboneRevitBridge.token" "Qt Revit token file"
Assert-Contains $agentCpp "Revit Bridge erreichbar" "Revit bridge reachable status after plugin handshake"
Assert-Contains $agentCpp "Revit Bridge nicht erreichbar" "Revit bridge unreachable status without plugin"
Assert-Contains $agentCpp "revit.elements.list" "read-only Revit element list capability"
Assert-Contains $agentCpp "executeReadOnlyToolCall" "confirmed read-only tool execution entrypoint"
Assert-Contains $agentCpp "confirmationRequired" "Revit context requires confirmation before API reads"
Assert-Contains $agentHeader "Revit Bridge nicht erreichbar" "initial Revit bridge status is unreachable"
Assert-Contains $agentHeader "toolDefinitions" "LMStudio Revit tool definitions"
Assert-Contains $bridgeClient "private const int Port = 47627" "C# Revit bridge port"
Assert-Contains $bridgeClient "BareboneRevitBridge.token" "C# Revit token file"
Assert-Contains $bridgeProject "<TargetFramework>net8.0-windows</TargetFramework>" "Revit bridge target framework"
Assert-Contains $bridgeProject "C:\Program Files\Autodesk\Revit 2026\RevitAPI.dll" "RevitAPI reference"
Assert-Contains $bridgeProject "C:\Program Files\Autodesk\Revit 2026\RevitAPIUI.dll" "RevitAPIUI reference"
Assert-Contains $bridgeHandler "IExternalEventHandler" "ExternalEvent handler"
Assert-Contains $bridgeHandler "new Transaction" "confirmed write actions use transactions"
Assert-Contains $bridgeHandler '"revit.elements.list" => ElementsList' "read-only Revit elements list endpoint"
Assert-Contains $bridgeHandler "WhereElementIsNotElementType" "read-only element list defaults to instances"
Assert-Contains $chatCpp "tool_calls" "LMStudio tool call handling"
Assert-Contains $chatCpp "Revit API Zugriff bestaetigen" "chat confirmation for Revit tool calls"
Assert-Contains $chatCpp "modelSafeToolResponse" "large Revit tool responses are compacted before LMStudio follow-up"
Assert-Contains $chatCpp "displayedFullTableInChat" "full Revit element table is rendered locally instead of sent fully to LMStudio"
Assert-Contains $chatCpp "postChatMessages(finalMessages, false)" "tool result follow-up does not execute additional tools automatically"
Assert-Contains $installScript "<ClientId>" "Revit 2026 manifest ClientId"
Assert-Contains $installScript "<UseRevitContext>False</UseRevitContext>" "Revit 2026 dependency isolation"
Assert-Contains $buildScript "dotnet 'build'" "Windows build builds Revit bridge"
Assert-Contains $cmake "src/revit/RevitAgent.cpp" "Qt build includes RevitAgent"
Assert-NotContains $cmake ("BAREBONE_BUILD_" + "BR" + "X_PLUGIN") "old CMake option"

$indexHtml = Read-RepoFile "index.html"
Assert-Contains $indexHtml "Revit Bridge erreichbar" "header reachable label"
Assert-Contains $indexHtml "Revit Bridge nicht erreichbar" "header unreachable label"
Assert-Contains $indexHtml "AI kann Fehler machen. Revit Aktionen werden erst nach Bestätigung ausgeführt." "Revit footer confirmation wording"
Assert-NotContains $indexHtml "Revit Bridge bereit" "old server-ready header label"
Assert-NotContains $indexHtml "Revit ist aktuell eine Chat-Hülle ohne CAD-Ausführung" "old Revit footer wording"
Assert-NotContains $indexHtml "Revit is currently a chat shell without CAD execution" "old English Revit footer wording"

$activeSources = @(
    @{ Name = "index.html"; Text = $indexHtml },
    @{ Name = "ChatPage.cpp"; Text = $chatCpp },
    @{ Name = "ChatPage.h"; Text = $chatHeader },
    @{ Name = "MainWindow.h"; Text = $mainHeader },
    @{ Name = "RevitAgent.cpp"; Text = $agentCpp },
    @{ Name = "RevitAgent.h"; Text = $agentHeader },
    @{ Name = "README.md"; Text = Read-RepoFile "README.md" },
    @{ Name = "windows/build-app.ps1"; Text = $buildScript }
)

foreach ($source in $activeSources) {
    $legacyFragments = @(
        "Bri" + "csCAD",
        "Bri" + "csCad",
        "bri" + "cscad",
        "Barebone" + "Br" + "x",
        "BR" + "X",
        "476" + "26",
        "BareboneQt" + "Bridge.token"
    )
    foreach ($fragment in $legacyFragments) {
        Assert-NotContains $source.Text $fragment "legacy CAD string in $($source.Name)"
    }
}

Write-Host "Revit bridge contract checks passed."
