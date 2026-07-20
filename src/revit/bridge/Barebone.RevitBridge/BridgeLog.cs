namespace Barebone.RevitBridge;

internal static class BridgeLog
{
    private const string LogFileName = "BareboneRevitBridge.log";

    public static void Write(string message)
    {
        var line = $"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}";
        var assemblyDirectory = Path.GetDirectoryName(typeof(BridgeLog).Assembly.Location);
        var paths = new[] {
            BridgePaths.SharedTempFile(LogFileName),
            Path.Combine(Path.GetTempPath(), LogFileName),
            string.IsNullOrWhiteSpace(assemblyDirectory) ? "" : Path.Combine(assemblyDirectory, LogFileName),
            Path.Combine(AppContext.BaseDirectory, LogFileName)
        };

        foreach (var path in paths.Where(path => !string.IsNullOrWhiteSpace(path)).Distinct(StringComparer.OrdinalIgnoreCase)) {
            WriteToPath(path, line);
        }
    }

    private static void WriteToPath(string path, string line)
    {
        try {
            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(directory)) {
                Directory.CreateDirectory(directory);
            }
            File.AppendAllText(path, line);
        } catch {
        }
    }
}
