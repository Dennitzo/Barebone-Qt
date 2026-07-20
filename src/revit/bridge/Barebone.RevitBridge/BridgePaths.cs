namespace Barebone.RevitBridge;

internal static class BridgePaths
{
    public static string SharedTempFile(string fileName)
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!string.IsNullOrWhiteSpace(localAppData)) {
            return Path.Combine(localAppData, "Temp", fileName);
        }

        var tempEnvironment = Environment.GetEnvironmentVariable("TEMP");
        if (!string.IsNullOrWhiteSpace(tempEnvironment)) {
            return Path.Combine(tempEnvironment, fileName);
        }

        return Path.Combine(Path.GetTempPath(), fileName);
    }
}
