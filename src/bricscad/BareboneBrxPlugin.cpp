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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kQtBridgeHost = "127.0.0.1";
constexpr unsigned short kQtBridgePort = 47626;
constexpr unsigned short kPluginBridgePort = 47627;
constexpr double kRectangleTolerance = 1.0e-6;

std::atomic_bool g_pluginServerRunning{false};
std::thread g_pluginServerThread;
std::mutex g_pluginServerSocketMutex;
SOCKET g_pluginServerSocket = INVALID_SOCKET;

struct BridgeJob {
    explicit BridgeJob(std::string value)
        : request(std::move(value))
    {
    }

    std::string request;
    std::string response;
    std::mutex mutex;
    std::condition_variable doneEvent;
    bool done = false;
};

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string toUpperAscii(std::string value)
{
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::vector<std::string> splitTabs(const std::string& value)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\t', start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

int hexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string percentEncode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(CP_UTF8, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const wchar_t* value)
{
    if (value == nullptr || value[0] == L'\0') {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring asciiToWide(const std::string& value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char c : value) {
        result.push_back(c < 128 ? static_cast<wchar_t>(c) : L'?');
    }
    return result;
}

std::string acharToUtf8(const ACHAR* value)
{
#ifdef UNICODE
    return wideToUtf8(value);
#else
    return value == nullptr ? std::string() : std::string(value);
#endif
}

std::basic_string<ACHAR> utf8ToAchar(const std::string& value)
{
#ifdef UNICODE
    return utf8ToWide(value);
#else
    return value;
#endif
}

std::string errorStatusText(Acad::ErrorStatus status)
{
    std::ostringstream result;
    result << acharToUtf8(acadErrorStatusText(status)) << " (" << static_cast<int>(status) << ")";
    return result.str();
}

std::string objectHandleText(const AcDbObjectId& objectId)
{
    if (objectId.isNull()) {
        return "<null>";
    }

    ACHAR handleBuffer[AcDbHandle::kStrSiz]{};
    if (!objectId.handle().getIntoAsciiBuffer(handleBuffer)) {
        return "<handle unavailable>";
    }
    return acharToUtf8(handleBuffer);
}

void printBrxDebug(const std::string& message)
{
#ifdef UNICODE
    const std::wstring wideMessage = utf8ToWide(message);
    acutPrintf(_T("\nBarebone-Qt BRX Debug: %ls"), wideMessage.c_str());
#else
    acutPrintf("\nBarebone-Qt BRX Debug: %s", message.c_str());
#endif
}

std::string brxDebugLogPath()
{
    char tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        return "BareboneQtBrx.log";
    }
    return std::string(tempPath) + "BareboneQtBrx.log";
}

void resetBrxDebugLog()
{
    std::ofstream logFile(brxDebugLogPath(), std::ios::out | std::ios::trunc);
    if (logFile) {
        logFile << "Barebone-Qt BRX Debug Log\n";
    }
}

void writeBrxDebugLog(const std::string& message)
{
    std::ofstream logFile(brxDebugLogPath(), std::ios::out | std::ios::app);
    if (logFile) {
        logFile << message << '\n';
    }
}

void appendDebug(std::vector<std::string>& debugLines, const std::string& message)
{
    debugLines.push_back(message);
    printBrxDebug(message);
    writeBrxDebugLog(message);
}

void appendDebugResponse(std::ostringstream& response, const std::vector<std::string>& debugLines)
{
    for (const std::string& line : debugLines) {
        response << "DEBUG\t" << percentEncode(line) << "\n";
    }
}

void finishBridgeJob(BridgeJob* job, std::string response)
{
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->response = std::move(response);
        job->done = true;
    }
    job->doneEvent.notify_one();
}

bool requiresCommandContext(const std::string& request)
{
    const std::vector<std::string> parts = splitTabs(request);
    return !parts.empty() && toUpperAscii(parts.front()) == "EXTRUDE";
}

bool sendAll(SOCKET socketHandle, const std::string& response)
{
    const char* cursor = response.data();
    int remaining = static_cast<int>(response.size());
    while (remaining > 0) {
        const int sent = send(socketHandle, cursor, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

std::string readSocketRequest(SOCKET socketHandle)
{
    std::string request;
    char buffer[1024]{};
    while (request.size() < 65536) {
        const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        request.append(buffer, received);
        if (request.find('\n') != std::string::npos) {
            break;
        }
    }
    return trim(request);
}

std::string errorResponse(const std::string& message)
{
    return "ERROR\t" + message + "\n";
}

AcDbDatabase* workingDatabase()
{
    return acdbHostApplicationServices() == nullptr
        ? nullptr
        : acdbHostApplicationServices()->workingDatabase();
}

std::string listLayersInApplicationContext()
{
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    }

    AcAxDocLock lock(database);
    if (lock.lockStatus() != Acad::eOk) {
        return errorResponse("Aktive Zeichnung konnte nicht gesperrt werden");
    }

    AcDbLayerTable* layerTable = nullptr;
    if (database->getLayerTable(layerTable, AcDb::kForRead) != Acad::eOk || layerTable == nullptr) {
        return errorResponse("Layer-Tabelle konnte nicht gelesen werden");
    }

    AcDbLayerTableIterator* iterator = nullptr;
    if (layerTable->newIterator(iterator) != Acad::eOk || iterator == nullptr) {
        layerTable->close();
        return errorResponse("Layer-Iterator konnte nicht erstellt werden");
    }

    std::vector<std::string> layerNames;
    for (; !iterator->done(); iterator->step()) {
        AcDbLayerTableRecord* record = nullptr;
        if (iterator->getRecord(record, AcDb::kForRead) != Acad::eOk || record == nullptr) {
            continue;
        }

        const ACHAR* name = nullptr;
        if (record->getName(name) == Acad::eOk && name != nullptr) {
            layerNames.push_back(acharToUtf8(name));
        }
        record->close();
    }

    delete iterator;
    layerTable->close();

    std::sort(layerNames.begin(), layerNames.end());
    layerNames.erase(std::unique(layerNames.begin(), layerNames.end()), layerNames.end());

    std::ostringstream response;
    response << "OK\n";
    for (const std::string& layerName : layerNames) {
        response << "LAYER\t" << percentEncode(layerName) << "\n";
    }
    response << "END\n";
    return response.str();
}

bool isRectanglePolyline(const AcDbPolyline* polyline)
{
    if (polyline == nullptr
        || polyline->isClosed() == Adesk::kFalse
        || polyline->isOnlyLines() == Adesk::kFalse
        || polyline->numVerts() != 4) {
        return false;
    }

    AcGePoint3d points[4];
    for (unsigned int i = 0; i < 4; ++i) {
        if (polyline->getPointAt(i, points[i]) != Acad::eOk) {
            return false;
        }
    }

    AcGeVector3d edges[4] = {
        points[1] - points[0],
        points[2] - points[1],
        points[3] - points[2],
        points[0] - points[3],
    };

    for (const AcGeVector3d& edge : edges) {
        if (edge.length() <= kRectangleTolerance) {
            return false;
        }
    }

    const double len0 = edges[0].length();
    const double len1 = edges[1].length();
    const double len2 = edges[2].length();
    const double len3 = edges[3].length();
    const bool oppositeLengthsMatch = std::abs(len0 - len2) <= kRectangleTolerance
        && std::abs(len1 - len3) <= kRectangleTolerance;
    const bool rightAngles = std::abs(edges[0].dotProduct(edges[1])) <= kRectangleTolerance * len0 * len1
        && std::abs(edges[1].dotProduct(edges[2])) <= kRectangleTolerance * len1 * len2
        && std::abs(edges[2].dotProduct(edges[3])) <= kRectangleTolerance * len2 * len3
        && std::abs(edges[3].dotProduct(edges[0])) <= kRectangleTolerance * len3 * len0;

    return oppositeLengthsMatch && rightAngles;
}

bool createSelectionSet(const AcDbObjectIdArray& objectIds, ads_name selectionSet, std::vector<std::string>& debugLines)
{
    int status = acedSSAdd(nullptr, nullptr, selectionSet);
    appendDebug(debugLines, "acedSSAdd create selection set: " + std::to_string(status));
    if (status != RTNORM) {
        return false;
    }

    for (int i = 0; i < objectIds.length(); ++i) {
        ads_name entityName{};
        const Acad::ErrorStatus nameStatus = acdbGetAdsName(entityName, objectIds.at(i));
        appendDebug(debugLines, "acdbGetAdsName handle=" + objectHandleText(objectIds.at(i)) + ": " + errorStatusText(nameStatus));
        if (nameStatus != Acad::eOk) {
            continue;
        }

        status = acedSSAdd(entityName, selectionSet, selectionSet);
        appendDebug(debugLines, "acedSSAdd entity handle=" + objectHandleText(objectIds.at(i)) + ": " + std::to_string(status));
    }

    Adesk::Int32 selectionLength = 0;
    status = acedSSLength(selectionSet, &selectionLength);
    appendDebug(debugLines, "acedSSLength: status=" + std::to_string(status) + ", length=" + std::to_string(selectionLength));
    return status == RTNORM && selectionLength == objectIds.length();
}

int runNativeExtrudeCommand(const ads_name selectionSet, double heightMm, std::vector<std::string>& debugLines)
{
    {
        std::ostringstream line;
        line << "Calling acedCommand _.EXTRUDE heightMm=" << heightMm;
        appendDebug(debugLines, line.str());
    }

    const int commandStatus = acedCommand(
        RTSTR, _T("_.EXTRUDE"),
        RTPICKS, selectionSet,
        RTSTR, _T(""),
        RTREAL, heightMm,
        RTNONE);
    appendDebug(debugLines, "acedCommand _.EXTRUDE status=" + std::to_string(commandStatus));
    acedUpdateDisplay();
    return commandStatus;
}

std::string extrudeLayerRectanglesInApplicationContext(const std::string& layerNameUtf8, double heightMm)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    {
        std::ostringstream line;
        line << "EXTRUDE request layer='" << layerNameUtf8 << "' heightMm=" << heightMm;
        appendDebug(debugLines, line.str());
    }

    if (layerNameUtf8.empty()) {
        return fail("Layername ist leer");
    }
    if (!std::isfinite(heightMm) || heightMm <= 0.0) {
        return fail("Hoehe muss groesser als 0 mm sein");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }
    appendDebug(debugLines, "Working database gefunden");

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    appendDebug(debugLines, std::string("Document manager context: ") + (applicationContext ? "application" : "command"));

    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }

    const std::basic_string<ACHAR> layerName = utf8ToAchar(layerNameUtf8);
    AcDbObjectId layerId;
    AcDbLayerTable* layerTable = nullptr;
    const Acad::ErrorStatus layerTableStatus = database->getLayerTable(layerTable, AcDb::kForRead);
    appendDebug(debugLines, "getLayerTable: " + errorStatusText(layerTableStatus));
    if (layerTableStatus != Acad::eOk || layerTable == nullptr) {
        return fail("Layer-Tabelle konnte nicht gelesen werden");
    }

    const Acad::ErrorStatus layerStatus = layerTable->getAt(layerName.c_str(), layerId);
    layerTable->close();
    appendDebug(debugLines, "Layer lookup: " + errorStatusText(layerStatus));
    if (layerStatus != Acad::eOk || layerId.isNull()) {
        return fail("Layer wurde in der aktiven Zeichnung nicht gefunden");
    }
    appendDebug(debugLines, "Layer id handle=" + objectHandleText(layerId));

    AcDbBlockTableRecord* space = nullptr;
    const AcDbObjectId currentSpaceId = database->currentSpaceId();
    const Acad::ErrorStatus openSpaceReadStatus = acdbOpenObject(space, currentSpaceId, AcDb::kForRead);
    appendDebug(debugLines, "Open current space for read handle=" + objectHandleText(currentSpaceId) + ": " + errorStatusText(openSpaceReadStatus));
    if (openSpaceReadStatus != Acad::eOk || space == nullptr) {
        return fail("Aktueller Zeichenbereich konnte nicht gelesen werden");
    }

    AcDbBlockTableRecordIterator* iterator = nullptr;
    const Acad::ErrorStatus iteratorStatus = space->newIterator(iterator);
    appendDebug(debugLines, "Create block iterator: " + errorStatusText(iteratorStatus));
    if (iteratorStatus != Acad::eOk || iterator == nullptr) {
        space->close();
        return fail("Objekt-Iterator konnte nicht erstellt werden");
    }

    AcDbObjectIdArray rectangleIds;
    int scannedEntities = 0;
    int layerEntities = 0;
    for (; !iterator->done(); iterator->step()) {
        AcDbObjectId entityId;
        if (iterator->getEntityId(entityId) != Acad::eOk || entityId.isNull()) {
            continue;
        }
        ++scannedEntities;

        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openEntityStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
        if (openEntityStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "Skip entity handle=" + objectHandleText(entityId) + " open failed: " + errorStatusText(openEntityStatus));
            continue;
        }

        if (entity->layerId() == layerId) {
            ++layerEntities;
            const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
            if (isRectanglePolyline(polyline)) {
                rectangleIds.append(entityId);
                appendDebug(debugLines, "Rectangle candidate handle=" + objectHandleText(entityId));
            }
        }
        entity->close();
    }

    delete iterator;
    space->close();

    {
        std::ostringstream line;
        line << "Scan result scanned=" << scannedEntities
            << " onLayer=" << layerEntities
            << " rectangles=" << rectangleIds.length();
        appendDebug(debugLines, line.str());
    }

    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native EXTRUDE");
    }

    int extruded = 0;
    int errors = 0;

    if (!rectangleIds.isEmpty()) {
        ads_name selectionSet{};
        if (!createSelectionSet(rectangleIds, selectionSet, debugLines)) {
            errors = rectangleIds.length();
            appendDebug(debugLines, "Selection set creation failed; EXTRUDE not executed");
        } else {
            const int commandStatus = runNativeExtrudeCommand(selectionSet, heightMm, debugLines);
            if (commandStatus == RTNORM) {
                extruded = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE completed for selection");
            } else {
                errors = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE failed for selection");
            }

            const int freeStatus = acedSSFree(selectionSet);
            appendDebug(debugLines, "acedSSFree status=" + std::to_string(freeStatus));
        }
    }

    const int found = rectangleIds.length();
    const int skipped = std::max(0, found - extruded);
    std::ostringstream response;
    response << "OK"
        << "\tFOUND\t" << found
        << "\tEXTRUDED\t" << extruded
        << "\tSKIPPED\t" << skipped
        << "\tERRORS\t" << errors
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string handlePluginRequestInApplicationContext(const std::string& request)
{
    const std::vector<std::string> parts = splitTabs(request);
    if (parts.empty() || parts.front().empty()) {
        return errorResponse("Leerer BRX Request");
    }

    const std::string command = toUpperAscii(parts.front());
    if (command == "LAYERS") {
        return listLayersInApplicationContext();
    }

    if (command == "EXTRUDE") {
        if (parts.size() < 3) {
            return errorResponse("EXTRUDE erwartet Layer und Hoehe");
        }

        const std::string layerName = percentDecode(parts[1]);
        char* parseEnd = nullptr;
        const double height = std::strtod(parts[2].c_str(), &parseEnd);
        if (parseEnd == parts[2].c_str()) {
            return errorResponse("Hoehe konnte nicht gelesen werden");
        }

        return extrudeLayerRectanglesInApplicationContext(layerName, height);
    }

    return errorResponse("Unbekannter BRX Request");
}

void processBridgeJobInApplicationContext(void* data)
{
    auto* job = static_cast<BridgeJob*>(data);
    finishBridgeJob(job, handlePluginRequestInApplicationContext(job->request));
}

void processBridgeJobInCommandContext(void* data)
{
    auto* job = static_cast<BridgeJob*>(data);
    finishBridgeJob(job, handlePluginRequestInApplicationContext(job->request));
}

std::string dispatchPluginRequest(const std::string& rawRequest)
{
    const std::string request = trim(rawRequest);
    if (request.empty()) {
        return errorResponse("Leerer BRX Request");
    }

    if (toUpperAscii(request) == "PING") {
        return "OK\tBareboneBrx\tready\n";
    }

    if (acDocManager == nullptr) {
        return errorResponse("BricsCAD Document Manager ist nicht verfuegbar");
    }

    BridgeJob job(request);
    if (requiresCommandContext(request)) {
        const Acad::ErrorStatus scheduleStatus = acDocManager->beginExecuteInCommandContext(&processBridgeJobInCommandContext, &job);
        if (scheduleStatus != Acad::eOk) {
            return errorResponse("BricsCAD Command Context konnte nicht gestartet werden: " + errorStatusText(scheduleStatus));
        }
    } else {
        acDocManager->executeInApplicationContext(&processBridgeJobInApplicationContext, &job);
    }

    std::unique_lock<std::mutex> lock(job.mutex);
    job.doneEvent.wait(lock, [&job]() {
        return job.done;
    });
    return job.response.empty() ? errorResponse("Leere Antwort aus BricsCAD Application Context") : job.response;
}

void handlePluginClient(SOCKET clientSocket)
{
    const std::string request = readSocketRequest(clientSocket);
    const std::string response = dispatchPluginRequest(request);
    sendAll(clientSocket, response);
    shutdown(clientSocket, SD_BOTH);
    closesocket(clientSocket);
}

void pluginServerLoop()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPluginBridgePort);
    inet_pton(AF_INET, kQtBridgeHost, &address.sin_addr);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
        || listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_pluginServerSocketMutex);
        g_pluginServerSocket = listenSocket;
    }

    while (g_pluginServerRunning.load()) {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            break;
        }
        handlePluginClient(clientSocket);
    }

    bool closeListenSocket = true;
    {
        std::lock_guard<std::mutex> lock(g_pluginServerSocketMutex);
        if (g_pluginServerSocket == listenSocket) {
            g_pluginServerSocket = INVALID_SOCKET;
        } else {
            closeListenSocket = false;
        }
    }
    if (closeListenSocket) {
        closesocket(listenSocket);
    }
    WSACleanup();
}

void startPluginBridgeServer()
{
    bool expected = false;
    if (!g_pluginServerRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    g_pluginServerThread = std::thread(pluginServerLoop);
}

void stopPluginBridgeServer()
{
    bool expected = true;
    if (!g_pluginServerRunning.compare_exchange_strong(expected, false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_pluginServerSocketMutex);
        if (g_pluginServerSocket != INVALID_SOCKET) {
            shutdown(g_pluginServerSocket, SD_BOTH);
            closesocket(g_pluginServerSocket);
            g_pluginServerSocket = INVALID_SOCKET;
        }
    }

    if (g_pluginServerThread.joinable()) {
        g_pluginServerThread.join();
    }
}

std::string sendBridgeRequest(const char* request)
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return "ERROR WSAStartup failed";
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        return "ERROR socket creation failed";
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kQtBridgePort);
    if (inet_pton(AF_INET, kQtBridgeHost, &address.sin_addr) != 1) {
        closesocket(socketHandle);
        WSACleanup();
        return "ERROR invalid bridge address";
    }

    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socketHandle);
        WSACleanup();
        return "ERROR Barebone-Qt bridge not reachable on 127.0.0.1:47626";
    }

    const int requestLength = static_cast<int>(std::strlen(request));
    if (send(socketHandle, request, requestLength, 0) == SOCKET_ERROR) {
        closesocket(socketHandle);
        WSACleanup();
        return "ERROR sending bridge request failed";
    }

    shutdown(socketHandle, SD_SEND);

    std::string response;
    char buffer[1024]{};
    int received = 0;
    while ((received = recv(socketHandle, buffer, sizeof(buffer) - 1, 0)) > 0) {
        response.append(buffer, received);
    }

    closesocket(socketHandle);
    WSACleanup();

    if (response.empty()) {
        return "ERROR empty response from Barebone-Qt bridge";
    }

    return response;
}

void printBridgeResponse(const std::string& response)
{
    const std::wstring wideResponse = asciiToWide(response);
    acutPrintf(_T("\nBarebone-Qt Bridge: %ls"), wideResponse.c_str());
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

    AcRx::AppRetCode On_kInitAppMsg(void* appData) override
    {
        const AcRx::AppRetCode result = AcRxArxApp::On_kInitAppMsg(appData);
        acrxRegisterAppMDIAware(appData);
        acrxUnlockApplication(appData);

        resetBrxDebugLog();
        startPluginBridgeServer();

        acutPrintf(_T("\nBarebone-Qt BRX Schnittstelle geladen."));
        acutPrintf(_T("\nDirekte Kommunikation: 127.0.0.1:%d"), kPluginBridgePort);
        const std::wstring debugLogPath = asciiToWide(brxDebugLogPath());
        acutPrintf(_T("\nDebug Log: %ls"), debugLogPath.c_str());
        acutPrintf(_T("\nVerfuegbare Befehle: BBPING, BBINFO\n"));
        return result;
    }

    AcRx::AppRetCode On_kUnloadAppMsg(void* appData) override
    {
        stopPluginBridgeServer();
        acutPrintf(_T("\nBarebone-Qt BRX Schnittstelle entladen.\n"));
        return AcRxArxApp::On_kUnloadAppMsg(appData);
    }

    static void BareboneQtBBPING()
    {
        printBridgeResponse(sendBridgeRequest("PING\n"));
    }

    static void BareboneQtBBINFO()
    {
        printBridgeResponse(sendBridgeRequest("INFO\n"));
    }
};

IMPLEMENT_ARX_ENTRYPOINT(BareboneBrxApp)

ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBPING, BBPING, ACRX_CMD_TRANSPARENT, NULL)
ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBINFO, BBINFO, ACRX_CMD_TRANSPARENT, NULL)
