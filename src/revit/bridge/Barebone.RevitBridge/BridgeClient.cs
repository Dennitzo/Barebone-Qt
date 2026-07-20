using Autodesk.Revit.UI;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace Barebone.RevitBridge;

internal sealed class BridgeClient : IDisposable
{
    private const string Host = "127.0.0.1";
    private const int Port = 47627;
    private const string TokenFileName = "BareboneRevitBridge.token";
    private const string BridgeBuild = "revit-json-bridge-v1";

    private readonly BridgeExternalEventHandler _handler;
    private readonly ExternalEvent _externalEvent;
    private readonly string _revitVersion;
    private readonly object _writerLock = new();
    private readonly CancellationTokenSource _cancellation = new();
    private readonly Thread _thread;
    private StreamWriter? _writer;
    private TcpClient? _tcpClient;
    private string _lastLoopMessage = "";

    public BridgeClient(BridgeExternalEventHandler handler, ExternalEvent externalEvent, string revitVersion)
    {
        _handler = handler;
        _externalEvent = externalEvent;
        _revitVersion = revitVersion;
        _thread = new Thread(Run) {
            IsBackground = true,
            Name = "Barebone Revit Bridge"
        };
    }

    public void Start()
    {
        BridgeLog.Write("Bridge client thread starting");
        _thread.Start();
    }

    public void SendResponse(int id, bool ok, object? result = null, string? error = null)
    {
        var payload = JsonSerializer.Serialize(new {
            type = "response",
            id,
            ok,
            result,
            error
        });
        SendLine(payload);
    }

    public void SendDebug(string message)
    {
        var payload = JsonSerializer.Serialize(new {
            type = "debug",
            message
        });
        SendLine(payload);
    }

    public void Dispose()
    {
        _cancellation.Cancel();
        try {
            _tcpClient?.Close();
        } catch {
        }
        if (_thread.IsAlive) {
            _thread.Join(TimeSpan.FromSeconds(2));
        }
        _externalEvent.Dispose();
        _cancellation.Dispose();
    }

    private void Run()
    {
        while (!_cancellation.IsCancellationRequested) {
            try {
                var tokenPath = BridgePaths.SharedTempFile(TokenFileName);
                var token = ReadToken(tokenPath);
                if (string.IsNullOrWhiteSpace(token)) {
                    LogLoopMessage($"Token file missing or empty at {tokenPath}");
                    Thread.Sleep(1000);
                    continue;
                }

                using var tcpClient = new TcpClient();
                LogLoopMessage($"Connecting to {Host}:{Port} tokenLength={token.Length}");
                tcpClient.Connect(Host, Port);
                _tcpClient = tcpClient;
                LogLoopMessage("TCP connected");
                using var stream = tcpClient.GetStream();
                using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
                using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true) {
                    AutoFlush = true,
                    NewLine = "\n"
                };
                lock (_writerLock) {
                    _writer = writer;
                }

                SendHello(token);
                BridgeLog.Write("Hello sent");
                ReadLoop(reader);
                LogLoopMessage("TCP read loop ended");
            } catch (OperationCanceledException) {
                BridgeLog.Write("Bridge client cancelled");
                return;
            } catch (Exception ex) {
                LogLoopMessage($"Connect/read failed: {ex.GetType().Name}: {ex.Message}");
                Thread.Sleep(1000);
            } finally {
                lock (_writerLock) {
                    _writer = null;
                }
                _tcpClient = null;
            }
        }
    }

    private void LogLoopMessage(string message)
    {
        if (message == _lastLoopMessage) {
            return;
        }
        _lastLoopMessage = message;
        BridgeLog.Write(message);
    }

    private void ReadLoop(StreamReader reader)
    {
        while (!_cancellation.IsCancellationRequested) {
            var line = reader.ReadLine();
            if (line is null) {
                return;
            }
            if (string.IsNullOrWhiteSpace(line)) {
                continue;
            }
            HandleLine(line);
        }
    }

    private void HandleLine(string line)
    {
        try {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("type", out var typeElement) || typeElement.GetString() != "request") {
                return;
            }

            var id = root.TryGetProperty("id", out var idElement) ? idElement.GetInt32() : 0;
            var method = root.TryGetProperty("method", out var methodElement) ? methodElement.GetString() ?? "" : "";
            var parameters = root.TryGetProperty("params", out var paramsElement)
                ? paramsElement.Clone()
                : JsonDocument.Parse("{}").RootElement.Clone();

            if (id <= 0 || string.IsNullOrWhiteSpace(method)) {
                SendResponse(id, ok: false, error: "Invalid request.");
                return;
            }

            _handler.Enqueue(new BridgeRequest(id, method, parameters));
            var raiseResult = _externalEvent.Raise();
            if (raiseResult is not ExternalEventRequest.Accepted and not ExternalEventRequest.Pending) {
                SendResponse(id, ok: false, error: $"ExternalEvent could not be raised: {raiseResult}");
            }
        } catch (Exception ex) {
            SendDebug($"Request parse failed: {ex.Message}");
        }
    }

    private void SendHello(string token)
    {
        var hello = JsonSerializer.Serialize(new {
            type = "hello",
            plugin = "BareboneRevit",
            protocol = "bridge-json",
            bridgeBuild = BridgeBuild,
            revitVersion = _revitVersion,
            activeDocument = (object?)null,
            token
        });
        SendLine(hello);
    }

    private void SendLine(string payload)
    {
        lock (_writerLock) {
            if (_writer is null) {
                BridgeLog.Write("SendLine skipped because writer is not connected");
                return;
            }
            _writer?.WriteLine(payload);
        }
    }

    private static string ReadToken(string path)
    {
        return File.Exists(path) ? File.ReadLines(path).FirstOrDefault()?.Trim() ?? "" : "";
    }
}

internal sealed record BridgeRequest(int Id, string Method, JsonElement Params);
