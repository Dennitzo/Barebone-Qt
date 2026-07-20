#include "RevitAgent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QTextStream>
#include <QUuid>

namespace {

constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47627;
constexpr const char* kBridgeTokenFileName = "BareboneRevitBridge.token";
constexpr int kBridgeRequestTimeoutMs = 30000;
constexpr int kMaxLogLines = 200;

QString logTime()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
}

QString generateBridgeToken()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
}

QString compactJson(const QJsonObject& value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QJsonObject bridgeError(const QString& message)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("response")},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), message},
    };
}

QJsonArray revitCapabilities()
{
    return QJsonArray{
        QStringLiteral("revit.status"),
        QStringLiteral("revit.document.summary"),
        QStringLiteral("revit.selection.describe"),
        QStringLiteral("revit.levels.list"),
        QStringLiteral("revit.views.list"),
        QStringLiteral("revit.categories.summary"),
        QStringLiteral("revit.elements.list"),
        QStringLiteral("revit.selection.set"),
        QStringLiteral("revit.textNote.create"),
        QStringLiteral("revit.parameter.setStringOnSelection"),
    };
}

QString bridgeMethodForTool(const QString& name)
{
    if (name == QStringLiteral("revit_status")) {
        return QStringLiteral("revit.status");
    }
    if (name == QStringLiteral("revit_document_summary")) {
        return QStringLiteral("revit.document.summary");
    }
    if (name == QStringLiteral("revit_selection_describe")) {
        return QStringLiteral("revit.selection.describe");
    }
    if (name == QStringLiteral("revit_levels_list")) {
        return QStringLiteral("revit.levels.list");
    }
    if (name == QStringLiteral("revit_views_list")) {
        return QStringLiteral("revit.views.list");
    }
    if (name == QStringLiteral("revit_categories_summary")) {
        return QStringLiteral("revit.categories.summary");
    }
    if (name == QStringLiteral("revit_elements_list")) {
        return QStringLiteral("revit.elements.list");
    }
    return {};
}

QJsonObject objectSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{}},
        {QStringLiteral("additionalProperties"), false},
    };
}

QJsonObject functionTool(const QString& name, const QString& description, const QJsonObject& parameters)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("parameters"), parameters},
        }},
    };
}

} // namespace

RevitAgent::RevitAgent(QObject* parent)
    : QObject(parent)
    , m_bridgeToken(generateBridgeToken())
{
    writeBridgeToken();
    startBridgeServer();
}

QString RevitAgent::statusMessage() const
{
    return m_statusMessage;
}

bool RevitAgent::isConnected() const
{
    return m_connected;
}

QStringList RevitAgent::recentLogLines() const
{
    return m_logLines;
}

void RevitAgent::requestPromptContext(std::function<void(const QJsonObject&)> callback)
{
    QJsonObject context{
        {QStringLiteral("connected"), m_connected},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("capabilities"), revitCapabilities()},
        {QStringLiteral("confirmationRequired"), true},
    };
    callback(context);
}

QJsonArray RevitAgent::toolDefinitions() const
{
    const QJsonObject empty = objectSchema();
    const QJsonObject limitSchema{
        {QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 1},
        {QStringLiteral("maximum"), 10000},
        {QStringLiteral("default"), 10000},
        {QStringLiteral("description"), QStringLiteral("Maximale Anzahl Datensaetze in der Antwort.")},
    };
    const QJsonObject scopeSchema{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("document"), QStringLiteral("activeView")}},
        {QStringLiteral("description"), QStringLiteral("document liest das gesamte aktive Dokument, activeView nur sichtbare Elemente der aktuellen Ansicht.")},
    };
    const QJsonObject elementsParameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), scopeSchema},
            {QStringLiteral("limit"), limitSchema},
            {QStringLiteral("includeElementTypes"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("description"), QStringLiteral("Wenn true, werden Elementtypen zusaetzlich zu Instanzen ausgegeben.")},
            }},
        }},
        {QStringLiteral("additionalProperties"), false},
    };
    const QJsonObject viewsParameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("limit"), limitSchema},
        }},
        {QStringLiteral("additionalProperties"), false},
    };

    return QJsonArray{
        functionTool(QStringLiteral("revit_status"),
            QStringLiteral("Liest Revit-Version, aktives Dokument und aktive Ansicht."), empty),
        functionTool(QStringLiteral("revit_document_summary"),
            QStringLiteral("Liest eine kompakte Zusammenfassung des aktiven Revit-Dokuments."), empty),
        functionTool(QStringLiteral("revit_selection_describe"),
            QStringLiteral("Liest die aktuelle Revit-Auswahl mit IDs, Namen, Kategorien und Klassen."), empty),
        functionTool(QStringLiteral("revit_levels_list"),
            QStringLiteral("Listet alle Ebenen des aktiven Revit-Dokuments."), empty),
        functionTool(QStringLiteral("revit_views_list"),
            QStringLiteral("Listet Ansichten des aktiven Revit-Dokuments."), viewsParameters),
        functionTool(QStringLiteral("revit_categories_summary"),
            QStringLiteral("Zaehlt Revit-Elemente nach Kategorie fuer einen schnellen Dokumentueberblick."), empty),
        functionTool(QStringLiteral("revit_elements_list"),
            QStringLiteral("Listet Revit-Elemente tabellarisch verwertbar mit ID, Kategorie, Klasse, Name, Familie, Typ, Ebene und Ansicht. Fuer Anfragen wie 'Liste alle Objekte als Tabelle auf' dieses Tool verwenden."),
            elementsParameters),
    };
}

bool RevitAgent::executeReadOnlyToolCall(const QString& name, const QJsonObject& arguments, std::function<void(const QJsonObject&)> callback)
{
    const QString method = bridgeMethodForTool(name.trimmed());
    if (method.isEmpty()) {
        if (callback) {
            callback(bridgeError(QStringLiteral("Unbekanntes oder nicht erlaubtes Revit-Tool: %1").arg(name)));
        }
        return false;
    }

    return sendRequest(method, arguments, std::move(callback));
}

bool RevitAgent::extractProposal(QString& assistantContent, QVariantMap* uiProposal)
{
    static const QRegularExpression fencedProposal(
        QStringLiteral("```(?:barebone_revit_action|json)\\s*([\\s\\S]*?\"(?:revit\\.proposal|method)\"[\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch match = fencedProposal.match(assistantContent);
    QString jsonText;
    if (match.hasMatch()) {
        jsonText = match.captured(1).trimmed();
    } else {
        const int firstBrace = assistantContent.indexOf(QLatin1Char('{'));
        const int lastBrace = assistantContent.lastIndexOf(QLatin1Char('}'));
        if (firstBrace >= 0 && lastBrace > firstBrace) {
            const QString candidate = assistantContent.mid(firstBrace, lastBrace - firstBrace + 1);
            if (candidate.contains(QStringLiteral("revit.proposal")) || candidate.contains(QStringLiteral("\"method\""))) {
                jsonText = candidate.trimmed();
            }
        }
    }

    if (jsonText.isEmpty()) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        appendBridgeLog(QStringLiteral("Revit Proposal JSON Fehler: %1").arg(parseError.errorString()));
        return false;
    }

    QJsonObject proposal = document.object();
    const QString method = proposal.value(QStringLiteral("method")).toString().trimmed();
    if (!isAllowedProposal(method)) {
        appendBridgeLog(QStringLiteral("Revit Proposal verworfen: Methode nicht erlaubt (%1)").arg(method));
        return false;
    }

    if (proposal.value(QStringLiteral("type")).toString().isEmpty()) {
        proposal.insert(QStringLiteral("type"), QStringLiteral("revit.proposal"));
    }
    m_pendingProposal = proposal;

    if (match.hasMatch()) {
        assistantContent.remove(match.capturedStart(), match.capturedLength());
    } else {
        const int firstBrace = assistantContent.indexOf(QLatin1Char('{'));
        const int lastBrace = assistantContent.lastIndexOf(QLatin1Char('}'));
        if (firstBrace >= 0 && lastBrace > firstBrace) {
            assistantContent.remove(firstBrace, lastBrace - firstBrace + 1);
        }
    }
    assistantContent = assistantContent.trimmed();

    if (uiProposal) {
        *uiProposal = proposalForUi(proposal);
    }
    return true;
}

void RevitAgent::executePendingProposal(std::function<void(const QJsonObject&)> callback)
{
    if (m_pendingProposal.isEmpty()) {
        callback(bridgeError(QStringLiteral("Keine Revit-Aktion wartet auf Bestaetigung.")));
        return;
    }

    const QString method = m_pendingProposal.value(QStringLiteral("method")).toString();
    const QJsonObject params = m_pendingProposal.value(QStringLiteral("params")).toObject();
    sendRequest(method, params, [this, callback = std::move(callback)](const QJsonObject& response) mutable {
        if (response.value(QStringLiteral("ok")).toBool()) {
            m_pendingProposal = {};
        }
        callback(response);
    });
}

void RevitAgent::clearPendingProposal()
{
    m_pendingProposal = {};
}

void RevitAgent::startBridgeServer()
{
    m_bridgeServer = new QTcpServer(this);
    QObject::connect(m_bridgeServer, &QTcpServer::newConnection, this, [this]() {
        while (m_bridgeServer->hasPendingConnections()) {
            handleBridgeSocket(m_bridgeServer->nextPendingConnection());
        }
    });

    if (!m_bridgeServer->listen(QHostAddress(QString::fromLatin1(kBridgeHost)), kBridgePort)) {
        setBridgeStatus(QStringLiteral("Revit Bridge nicht erreichbar"), false);
        appendBridgeLog(QStringLiteral("Server konnte nicht starten: %1").arg(m_bridgeServer->errorString()));
        return;
    }

    setBridgeStatus(QStringLiteral("Revit Bridge nicht erreichbar"), false);
    appendBridgeLog(QStringLiteral("Server bereit auf %1:%2, Token: %3")
        .arg(QString::fromLatin1(kBridgeHost))
        .arg(kBridgePort)
        .arg(bridgeTokenPath()));
}

void RevitAgent::handleBridgeSocket(QTcpSocket* socket)
{
    if (m_revitSocket && m_revitSocket != socket) {
        appendBridgeLog(QStringLiteral("Revit Verbindung ersetzt"));
        m_revitSocket->disconnectFromHost();
        m_revitSocket->deleteLater();
    }

    m_revitSocket = socket;
    m_revitReadBuffer.clear();
    m_revitAuthenticated = false;
    setBridgeStatus(QStringLiteral("Revit Bridge nicht erreichbar"), false);
    appendBridgeLog(QStringLiteral("Revit -> Qt: TCP verbunden von %1:%2")
        .arg(socket->peerAddress().toString())
        .arg(socket->peerPort()));

    QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (socket != m_revitSocket) {
            return;
        }
        m_revitReadBuffer.append(socket->readAll());
        while (true) {
            const int newline = m_revitReadBuffer.indexOf('\n');
            if (newline < 0) {
                break;
            }
            const QByteArray line = m_revitReadBuffer.left(newline).trimmed();
            m_revitReadBuffer.remove(0, newline + 1);
            if (!line.isEmpty()) {
                handleBridgeLine(line);
            }
        }
    });

    QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (socket == m_revitSocket) {
            for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end(); ++it) {
                if (it->timer) {
                    it->timer->stop();
                    it->timer->deleteLater();
                }
                if (it->callback) {
                    it->callback(bridgeError(QStringLiteral("Revit Bridge wurde getrennt.")));
                }
            }
            m_pendingRequests.clear();
            m_revitSocket = nullptr;
            m_revitAuthenticated = false;
            setBridgeStatus(QStringLiteral("Revit Bridge nicht erreichbar"), false);
            appendBridgeLog(QStringLiteral("Revit Verbindung getrennt"));
        }
        socket->deleteLater();
    });
}

void RevitAgent::handleBridgeLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        appendBridgeLog(QStringLiteral("Revit -> Qt: ungueltiges JSON (%1)").arg(parseError.errorString()));
        return;
    }

    handleBridgeMessage(document.object());
}

void RevitAgent::handleBridgeMessage(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("hello")) {
        if (message.value(QStringLiteral("token")).toString() != m_bridgeToken) {
            appendBridgeLog(QStringLiteral("Revit -> Qt: hello mit ungueltigem Token"));
            if (m_revitSocket) {
                m_revitSocket->disconnectFromHost();
            }
            return;
        }

        m_revitAuthenticated = true;
        setBridgeStatus(QStringLiteral("Revit Bridge erreichbar"), true);
        appendBridgeLog(QStringLiteral("Revit -> Qt: hello %1 (%2)")
            .arg(message.value(QStringLiteral("plugin")).toString(QStringLiteral("BareboneRevit")),
                message.value(QStringLiteral("bridgeBuild")).toString(QStringLiteral("unknown"))));
        return;
    }

    if (!m_revitAuthenticated) {
        appendBridgeLog(QStringLiteral("Revit -> Qt: Nachricht vor Authentifizierung verworfen"));
        return;
    }

    if (type == QStringLiteral("response")) {
        completePendingRequest(message.value(QStringLiteral("id")).toInt(), message);
        return;
    }

    if (type == QStringLiteral("event") || type == QStringLiteral("debug")) {
        appendBridgeLog(QStringLiteral("Revit -> Qt: %1").arg(compactJson(message)));
        return;
    }

    appendBridgeLog(QStringLiteral("Revit -> Qt: Nachricht %1").arg(compactJson(message)));
}

bool RevitAgent::sendRequest(const QString& method, const QJsonObject& params, std::function<void(const QJsonObject&)> callback)
{
    if (!m_revitSocket || m_revitSocket->state() != QAbstractSocket::ConnectedState || !m_revitAuthenticated) {
        if (callback) {
            callback(bridgeError(QStringLiteral("Revit Bridge ist nicht erreichbar.")));
        }
        return false;
    }

    const int id = m_nextRequestId++;
    const QJsonObject request{
        {QStringLiteral("type"), QStringLiteral("request")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    PendingRequest pending{method, std::move(callback), timer};
    m_pendingRequests.insert(id, pending);
    QObject::connect(timer, &QTimer::timeout, this, [this, id]() {
        auto it = m_pendingRequests.find(id);
        if (it == m_pendingRequests.end()) {
            return;
        }
        const PendingRequest pending = it.value();
        m_pendingRequests.erase(it);
        if (pending.callback) {
            pending.callback(bridgeError(QStringLiteral("Revit Request Timeout: %1").arg(pending.method)));
        }
        if (pending.timer) {
            pending.timer->deleteLater();
        }
    });
    timer->start(kBridgeRequestTimeoutMs);

    appendBridgeLog(QStringLiteral("Qt -> Revit: Request %1 %2").arg(id).arg(method));
    m_revitSocket->write(payload);
    return true;
}

void RevitAgent::completePendingRequest(int id, const QJsonObject& response)
{
    auto it = m_pendingRequests.find(id);
    if (it == m_pendingRequests.end()) {
        appendBridgeLog(QStringLiteral("Revit -> Qt: Antwort ohne Pending Request %1").arg(id));
        return;
    }

    const PendingRequest pending = it.value();
    m_pendingRequests.erase(it);
    if (pending.timer) {
        pending.timer->stop();
        pending.timer->deleteLater();
    }
    appendBridgeLog(QStringLiteral("Revit -> Qt: Response %1 ok=%2").arg(id).arg(response.value(QStringLiteral("ok")).toBool()));
    if (pending.callback) {
        pending.callback(response);
    }
}

void RevitAgent::setBridgeStatus(const QString& message, bool connected)
{
    m_statusMessage = message;
    m_connected = connected && m_revitAuthenticated;
    Q_EMIT bridgeStatusChanged(m_statusMessage, m_connected);
}

void RevitAgent::appendBridgeLog(const QString& message)
{
    const QString line = message.startsWith(QLatin1Char('['))
        ? message
        : QStringLiteral("[%1] %2").arg(logTime(), message);

    QFile logFile(QDir::temp().filePath(QStringLiteral("BareboneQtApp.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        logFile.write(line.toUtf8());
        logFile.write("\n");
    }

    m_logLines << line;
    while (m_logLines.size() > kMaxLogLines) {
        m_logLines.removeFirst();
    }
    Q_EMIT bridgeLogAdded(line);
}

void RevitAgent::writeBridgeToken() const
{
    QFile file(bridgeTokenPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << m_bridgeToken << '\n';
}

QString RevitAgent::bridgeTokenPath() const
{
    return QDir::temp().filePath(QString::fromLatin1(kBridgeTokenFileName));
}

QVariantMap RevitAgent::proposalForUi(const QJsonObject& proposal) const
{
    return QVariantMap{
        {QStringLiteral("title"), proposal.value(QStringLiteral("title")).toString(QStringLiteral("Revit Aktion bereit"))},
        {QStringLiteral("summary"), proposal.value(QStringLiteral("summary")).toString()},
        {QStringLiteral("details"), proposal.value(QStringLiteral("details")).toString(compactJson(proposal.value(QStringLiteral("params")).toObject()))},
        {QStringLiteral("canRun"), true},
    };
}

bool RevitAgent::isAllowedProposal(const QString& method) const
{
    return method == QStringLiteral("revit.selection.set")
        || method == QStringLiteral("revit.textNote.create")
        || method == QStringLiteral("revit.parameter.setStringOnSelection");
}
