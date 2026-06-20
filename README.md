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
