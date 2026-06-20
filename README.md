# Barebone-Qt

Windows-only Qt6 application shell based on the visual structure of Bitcoin-Qt.

The template intentionally contains no Bitcoin Core, Electrs, Mempool, Public Pool, Node.js, npm, MariaDB, runtime staging, macOS, or Linux build scripts.

## Build on Windows

```powershell
.\windows\check-environment.ps1
.\windows\build-app.ps1 -Configuration Release
```

To create a zip artifact:

```powershell
.\windows\package-artifact.ps1 -Configuration Release
```
