# Barebone-Qt

Qt6 application shell with a local Revit 2026 bridge.

## Build on Windows

```powershell
.\windows\check-environment.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\build-app.ps1 -Configuration Release
```

This builds the Windows Qt app and the Revit C# bridge.

The Windows app build output is standardized to:

```text
build\windows-6.11.1-msvc-v143\Barebone-Qt.exe
```

The Revit bridge build output is standardized to:

```text
build\revit-bridge\Barebone.RevitBridge.dll
```

To install the Revit add-in manifest for the current user:

```powershell
.\windows\install-revit-addin.ps1 -BuildFirst -Configuration Release
```

The manifest is written to:

```text
%APPDATA%\Autodesk\Revit\Addins\2026\BareboneRevit.addin
```

After installing the manifest, restart Revit 2026 or load the add-in through Revit's Add-In Manager.

Barebone-Qt starts the local Revit bridge server on `127.0.0.1:47627` and writes a per-run auth token to `%TEMP%\BareboneRevitBridge.token`. The Revit add-in connects to that server as a reconnecting client. Qt keeps the LMStudio/OpenAI-compatible AI call local to the app; the C# add-in only reads Revit context and executes confirmed Revit actions.

## Build on macOS

```bash
./macos/check-environment.sh
```

To build the `.app` bundle:

```bash
./macos/build-app.sh --configuration Release
```

The macOS app build output is standardized to:

```text
build/macos-6.11.1-arm64/Barebone-Qt.app
```

To build, deploy Qt with `macdeployqt`, and create a zip artifact in one step:

```bash
./macos/package-artifact.sh --configuration Release
```

The Revit bridge is Windows-only because Autodesk Revit 2026 is Windows-only.
