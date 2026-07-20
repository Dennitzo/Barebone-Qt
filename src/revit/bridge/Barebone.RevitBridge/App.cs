using Autodesk.Revit.UI;

namespace Barebone.RevitBridge;

public sealed class App : IExternalApplication
{
    private BridgeClient? _client;

    public Result OnStartup(UIControlledApplication application)
    {
        try {
            BridgeLog.Write($"OnStartup Revit {application.ControlledApplication.VersionNumber}");
            var handler = new BridgeExternalEventHandler();
            var externalEvent = ExternalEvent.Create(handler);
            _client = new BridgeClient(handler, externalEvent, application.ControlledApplication.VersionNumber);
            handler.AttachClient(_client);
            _client.Start();
            BridgeLog.Write("OnStartup completed");
            return Result.Succeeded;
        } catch (Exception ex) {
            BridgeLog.Write($"OnStartup failed: {ex}");
            throw;
        }
    }

    public Result OnShutdown(UIControlledApplication application)
    {
        BridgeLog.Write("OnShutdown");
        _client?.Dispose();
        _client = null;
        return Result.Succeeded;
    }
}
