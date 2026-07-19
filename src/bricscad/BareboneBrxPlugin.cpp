#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "arxHeaders.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr const char* kQtBridgeHost = "127.0.0.1";
constexpr unsigned short kQtBridgePort = 47626;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";
constexpr const char* kBridgeBuildId = "bridge-json-minimal-v1";

std::atomic_bool g_bridgeClientRunning{false};
std::atomic_bool g_bridgeClientConnected{false};
std::thread g_bridgeClientThread;
std::mutex g_bridgeClientSocketMutex;
std::mutex g_bridgeClientSendMutex;
SOCKET g_bridgeClientSocket = INVALID_SOCKET;

std::string trim(const std::string& value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u00";
                static constexpr char hex[] = "0123456789abcdef";
                out << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string tempFilePath(const char* fileName)
{
    char tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        return fileName;
    }
    return std::string(tempPath) + fileName;
}

std::string bridgeTokenFilePath()
{
    return tempFilePath(kBridgeTokenFileName);
}

std::string brxDebugLogPath()
{
    return tempFilePath("BareboneBrxBridge.log");
}

void writeBrxDebugLog(const std::string& line)
{
    std::ofstream file(brxDebugLogPath(), std::ios::app);
    if (file) {
        file << line << '\n';
    }
    OutputDebugStringA((line + "\n").c_str());
}

std::string readBridgeToken()
{
    std::ifstream tokenFile(bridgeTokenFilePath());
    std::string token;
    std::getline(tokenFile, token);
    return trim(token);
}

std::optional<int> jsonIntProperty(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    char* end = nullptr;
    const long value = std::strtol(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<std::string> jsonStringProperty(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t quote = json.find('"', colon + 1);
    if (quote == std::string::npos) {
        return std::nullopt;
    }
    std::ostringstream out;
    bool escaped = false;
    for (std::size_t i = quote + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            switch (ch) {
            case '"': out << '"'; break;
            case '\\': out << '\\'; break;
            case '/': out << '/'; break;
            case 'b': out << '\b'; break;
            case 'f': out << '\f'; break;
            case 'n': out << '\n'; break;
            case 'r': out << '\r'; break;
            case 't': out << '\t'; break;
            default: out << ch; break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return out.str();
        }
        out << ch;
    }
    return std::nullopt;
}

std::string minimalErrorResponse(int id, const std::string& method)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":false"
        << ",\"error\":\"Barebone BRX Minimal-Bridge: keine Tools oder Capabilities aktiv\""
        << ",\"method\":\"" << jsonEscape(method) << "\""
        << "}\n";
    return response.str();
}

bool sendRawLine(const std::string& payload)
{
    std::lock_guard<std::mutex> socketLock(g_bridgeClientSocketMutex);
    if (g_bridgeClientSocket == INVALID_SOCKET) {
        return false;
    }
    const char* data = payload.data();
    int remaining = static_cast<int>(payload.size());
    while (remaining > 0) {
        const int sent = send(g_bridgeClientSocket, data, remaining, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        remaining -= sent;
    }
    return true;
}

bool sendLine(const std::string& payload)
{
    std::lock_guard<std::mutex> sendLock(g_bridgeClientSendMutex);
    return sendRawLine(payload);
}

void closeBridgeSocket()
{
    std::lock_guard<std::mutex> socketLock(g_bridgeClientSocketMutex);
    if (g_bridgeClientSocket != INVALID_SOCKET) {
        closesocket(g_bridgeClientSocket);
        g_bridgeClientSocket = INVALID_SOCKET;
    }
    g_bridgeClientConnected = false;
}

std::string helloMessage(const std::string& token)
{
    std::ostringstream hello;
    hello << "{\"type\":\"hello\""
        << ",\"plugin\":\"BareboneBrx\""
        << ",\"protocol\":\"bridge-json\""
        << ",\"bridgeBuild\":\"" << kBridgeBuildId << "\""
        << ",\"token\":\"" << jsonEscape(token) << "\""
        << "}\n";
    return hello.str();
}

void processIncomingLine(const std::string& line)
{
    const std::string type = jsonStringProperty(line, "type").value_or("");
    if (type != "request") {
        return;
    }
    const int id = jsonIntProperty(line, "id").value_or(0);
    const std::string method = jsonStringProperty(line, "method").value_or("");
    writeBrxDebugLog("Qt -> BRX minimal request rejected: " + (method.empty() ? std::string("<empty>") : method));
    sendLine(minimalErrorResponse(id, method));
}

void bridgeClientLoop()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        writeBrxDebugLog("BRX minimal bridge: WSAStartup failed");
        return;
    }

    while (g_bridgeClientRunning) {
        const std::string token = readBridgeToken();
        if (token.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kQtBridgePort);
        inet_pton(AF_INET, kQtBridgeHost, &address.sin_addr);

        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(socketHandle);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        {
            std::lock_guard<std::mutex> socketLock(g_bridgeClientSocketMutex);
            g_bridgeClientSocket = socketHandle;
            g_bridgeClientConnected = true;
        }

        writeBrxDebugLog("BRX minimal bridge connected to Qt");
        sendLine(helloMessage(token));

        std::string buffer;
        char chunk[4096]{};
        while (g_bridgeClientRunning) {
            const int received = recv(socketHandle, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            while (true) {
                const std::size_t newline = buffer.find('\n');
                if (newline == std::string::npos) {
                    break;
                }
                const std::string line = trim(buffer.substr(0, newline));
                buffer.erase(0, newline + 1);
                if (!line.empty()) {
                    processIncomingLine(line);
                }
            }
        }

        closeBridgeSocket();
        if (g_bridgeClientRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }

    closeBridgeSocket();
    WSACleanup();
}

void startBridgeClient()
{
    if (g_bridgeClientRunning.exchange(true)) {
        return;
    }
    g_bridgeClientThread = std::thread(bridgeClientLoop);
}

void stopBridgeClient()
{
    if (!g_bridgeClientRunning.exchange(false)) {
        return;
    }
    closeBridgeSocket();
    if (g_bridgeClientThread.joinable()) {
        g_bridgeClientThread.join();
    }
}

void printToCommandLine(const wchar_t* message)
{
    acutPrintf(L"\n%s", message);
}

} // namespace

class BareboneBrxApp final : public AcRxArxApp {
public:
    BareboneBrxApp()
        : AcRxArxApp()
    {
    }

    void RegisterServerComponents() override
    {
    }

    AcRx::AppRetCode On_kInitAppMsg(void* packet) override
    {
        const AcRx::AppRetCode ret = AcRxArxApp::On_kInitAppMsg(packet);
        acrxUnlockApplication(packet);
        acrxRegisterAppMDIAware(packet);
        startBridgeClient();
        printToCommandLine(L"BareboneBrx minimal bridge loaded.");
        return ret;
    }

    AcRx::AppRetCode On_kUnloadAppMsg(void* packet) override
    {
        stopBridgeClient();
        printToCommandLine(L"BareboneBrx minimal bridge unloaded.");
        return AcRxArxApp::On_kUnloadAppMsg(packet);
    }

    static void BareboneBrx_BBPING()
    {
        printToCommandLine(g_bridgeClientConnected ? L"Barebone BRX bridge connected." : L"Barebone BRX bridge not connected.");
    }

    static void BareboneBrx_BBINFO()
    {
        printToCommandLine(L"Barebone BRX minimal bridge: connection only, no tools or capabilities.");
    }
};

IMPLEMENT_ARX_ENTRYPOINT(BareboneBrxApp)

ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneBrx, _BBPING, BBPING, ACRX_CMD_MODAL, nullptr)
ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneBrx, _BBINFO, BBINFO, ACRX_CMD_MODAL, nullptr)
