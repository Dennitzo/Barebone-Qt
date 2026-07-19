# Barebone-Qt

Qt6 application shell.

## Build on Windows

```powershell
.\windows\check-environment.ps1
```
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\build-app.ps1 -Configuration Release
```

This builds both the Windows app and the BricsCAD BRX plugin.

The Windows app build output is standardized to:

```text
build\windows-6.11.1-msvc\Barebone-Qt.exe
```

To create a zip artifact:

```powershell
.\windows\package-artifact.ps1 -Configuration Release
```

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

## Optional BricsCAD BRX plugin

The BricsCAD BRX V26 SDK is expected at:

```text
C:\Program Files\Bricsys\BRXSDK\BRX26.1.05.0
```

The same `build-app.ps1` command also configures and builds the optional BRX target. To build only the BRX target manually from an x64 Visual Studio Developer Prompt:

```powershell
cmake -S . -B build\brx-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64 -DBAREBONE_BUILD_BRX_PLUGIN=ON
cmake --build build\brx-msvc --target BareboneBrx
```

The BRX build output is standardized to:

```text
build\brx-msvc\bricscad\BareboneBrx.brx
```

Load the resulting `BareboneBrx.brx` in BricsCAD with `APPLOAD`. The diagnostic commands are `BBPING` and `BBINFO`.

Barebone-Qt starts the local bridge server on `127.0.0.1:47626` and writes a per-run auth token to `%TEMP%\BareboneQtBridge.token`. The BRX plugin connects to that server as a reconnecting client and sends only the authenticated `hello` message. Incoming JSON requests are answered with a minimal “not active” error.

The BricsCAD tab is currently a fresh second chat shell named “BricsCAD”. It contains no CAD action pipeline, no workflow runtime, no capability catalog, no SDK fallback tools, and no automatic drawing access. The plugin is intentionally reduced to the connection skeleton so the BricsCAD integration can be rebuilt from a clean base.
