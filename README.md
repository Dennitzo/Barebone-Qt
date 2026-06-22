# Barebone-Qt

Windows-only Qt6 application shell.

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

Load the resulting `BareboneBrx.brx` in BricsCAD with `APPLOAD`. The diagnostic commands are `BBPING`, `BBINFO`, `BBLOG`, and `BBTRACK`; `BBLOG` opens the BricsCAD-owned bridge log window, and `BBTRACK` enables selection tracking for log/debug events.

Barebone-Qt starts the local bridge server on `127.0.0.1:47626` and writes a per-run auth token to `%TEMP%\BareboneQtBridge.token`. The BRX plugin connects to that server as a reconnecting client and keeps one newline-delimited JSON connection open. The BricsCAD tab uses this bridge to fetch the current drawing layers and to extrude closed rectangular polylines on the selected layer by the configured height in millimeters.

The bridge protocol uses request IDs and versioned result payloads. `rectangles.extrude` supports the detail levels `summary`, `element`, and `geometry`; Barebone-Qt currently requests `element`, which returns compact BIM-ready element data without full profile geometry:

```json
{"id":2,"type":"request","method":"rectangles.extrude","params":{"layer":"0","heightMm":3000,"detail":"element","saveBefore":true}}
{"id":2,"type":"response","ok":true,"result":{"schema":"barebone.bricscad.rectangles.extrude.result.v1","detail":"element","units":"mm","coordinateSystem":"WCS","layer":"0","heightMm":3000,"saveBefore":true,"savedBefore":true,"found":1,"extruded":1,"skipped":0,"errors":0,"elements":[{"schema":"barebone.bricscad.element.v1","operation":"extruded","sourceHandle":"A5","solidHandle":"A7","sourceEntityKind":"polyline","entityKind":"3dSolid","bimClass":null,"layer":"0","heightMm":3000,"lengthMm":6300,"thicknessMm":390,"boundingBoxStatus":"actual","boundingBox":{"min":[0,0,0],"max":[6300,390,3000]},"sourceProfile":{"boundingBox":{"min":[0,0,0],"max":[6300,390,0]}}}]}}
```

For AI-assisted workflows, Barebone-Qt now uses live BRX capabilities instead of a generated tool registry. Read-only context methods such as `geometry.query`, `selection.describe`, and `entity.describe` can fetch current drawing data on demand; drawing-changing actions still require a user-confirmed tool proposal before Barebone-Qt forwards them to BRX.
