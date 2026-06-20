# Barebone-Qt

Windows-only Qt6 application shell.

## Build on Windows

```powershell
.\windows\check-environment.ps1
.\windows\build-app.ps1 -Configuration Release
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

Configure the optional BRX target from an x64 Visual Studio Developer Prompt:

```powershell
cmake -S . -B build\brx-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64 -DBAREBONE_BUILD_BRX_PLUGIN=ON
cmake --build build\brx-msvc --target BareboneBrx
```

Load the resulting `BareboneBrx.brx` in BricsCAD with `APPLOAD`. The diagnostic commands are `BBPING` and `BBINFO`.

Barebone-Qt starts a local diagnostic bridge on `127.0.0.1:47626`. The BRX plugin also starts a local command bridge on `127.0.0.1:47627`. The BricsCAD tab in Barebone-Qt uses that command bridge to fetch the current drawing layers and to extrude closed rectangular polylines on the selected layer by the configured height in millimeters.
