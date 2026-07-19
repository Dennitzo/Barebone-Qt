#include "BricsCadPage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPlainTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

#include <QUuid>

namespace {

constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47626;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";

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

} // namespace

BricsCadPage::BricsCadPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent)
    , m_bridgeToken(generateBridgeToken())
{
    Q_UNUSED(config)
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bridgeLog = new QPlainTextEdit(this);
    m_bridgeLog->setObjectName(QStringLiteral("logView"));
    m_bridgeLog->setReadOnly(true);
    layout->addWidget(m_bridgeLog, 1);

    writeBridgeToken();
    startBridgeServer();
}

void BricsCadPage::startBridgeServer()
{
    m_bridgeServer = new QTcpServer(this);
    QObject::connect(m_bridgeServer, &QTcpServer::newConnection, this, [this]() {
        while (m_bridgeServer->hasPendingConnections()) {
            handleBridgeSocket(m_bridgeServer->nextPendingConnection());
        }
    });

    if (!m_bridgeServer->listen(QHostAddress(QString::fromLatin1(kBridgeHost)), kBridgePort)) {
        setBridgeStatus(QStringLiteral("BRX Bridge nicht bereit"), false);
        appendBridgeLog(QStringLiteral("Server konnte nicht starten: %1").arg(m_bridgeServer->errorString()));
        return;
    }

    setBridgeStatus(QStringLiteral("BRX Bridge bereit"), false);
    appendBridgeLog(QStringLiteral("Server bereit, Token: %1").arg(bridgeTokenPath()));
}

void BricsCadPage::handleBridgeSocket(QTcpSocket* socket)
{
    if (m_brxSocket && m_brxSocket != socket) {
        appendBridgeLog(QStringLiteral("BRX Verbindung ersetzt"));
        m_brxSocket->disconnectFromHost();
        m_brxSocket->deleteLater();
    }

    m_brxSocket = socket;
    m_brxReadBuffer.clear();
    m_brxAuthenticated = false;
    setBridgeStatus(QStringLiteral("BRX Plugin verbunden, Authentifizierung laeuft..."), true);
    appendBridgeLog(QStringLiteral("BRX -> Qt: TCP verbunden von %1:%2")
        .arg(socket->peerAddress().toString())
        .arg(socket->peerPort()));

    QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        m_brxReadBuffer.append(socket->readAll());
        while (true) {
            const int newline = m_brxReadBuffer.indexOf('\n');
            if (newline < 0) {
                break;
            }
            const QByteArray line = m_brxReadBuffer.left(newline).trimmed();
            m_brxReadBuffer.remove(0, newline + 1);
            if (!line.isEmpty()) {
                handleBridgeLine(line);
            }
        }
    });

    QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (socket == m_brxSocket) {
            m_brxSocket = nullptr;
            m_brxAuthenticated = false;
            setBridgeStatus(QStringLiteral("BRX Plugin nicht verbunden"), false);
            appendBridgeLog(QStringLiteral("BRX Verbindung getrennt"));
        }
        socket->deleteLater();
    });
}

void BricsCadPage::handleBridgeLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        appendBridgeLog(QStringLiteral("BRX -> Qt: ungueltiges JSON (%1)").arg(parseError.errorString()));
        return;
    }

    handleBridgeMessage(document.object());
}

void BricsCadPage::handleBridgeMessage(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("hello")) {
        if (message.value(QStringLiteral("token")).toString() != m_bridgeToken) {
            appendBridgeLog(QStringLiteral("BRX -> Qt: hello mit ungueltigem Token"));
            if (m_brxSocket) {
                m_brxSocket->disconnectFromHost();
            }
            return;
        }

        m_brxAuthenticated = true;
        setBridgeStatus(QStringLiteral("BRX Plugin verbunden"), true);
        appendBridgeLog(QStringLiteral("BRX -> Qt: hello %1 (%2)")
            .arg(message.value(QStringLiteral("plugin")).toString(QStringLiteral("BareboneBrx")),
                message.value(QStringLiteral("bridgeBuild")).toString(QStringLiteral("minimal"))));
        return;
    }

    if (!m_brxAuthenticated) {
        appendBridgeLog(QStringLiteral("BRX -> Qt: Nachricht vor Authentifizierung verworfen"));
        return;
    }

    if (type == QStringLiteral("request")) {
        const int id = message.value(QStringLiteral("id")).toInt();
        const QString method = message.value(QStringLiteral("method")).toString();
        appendBridgeLog(QStringLiteral("BRX -> Qt: Request %1 abgelehnt (%2)")
            .arg(id)
            .arg(method.isEmpty() ? QStringLiteral("ohne Methode") : method));
        sendBridgeError(id, QStringLiteral("BricsCAD ist aktuell nur als frische Chat- und Verbindungs-Huelle aktiv."));
        return;
    }

    if (type == QStringLiteral("event") || type == QStringLiteral("debug")) {
        appendBridgeLog(QStringLiteral("BRX -> Qt: %1").arg(compactJson(message)));
        return;
    }

    appendBridgeLog(QStringLiteral("BRX -> Qt: Nachricht %1").arg(compactJson(message)));
}

void BricsCadPage::sendBridgeError(int id, const QString& message)
{
    if (!m_brxSocket || m_brxSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    const QJsonObject response{
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("response")},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), message},
    };
    QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    payload.append('\n');
    m_brxSocket->write(payload);
}

void BricsCadPage::setBridgeStatus(const QString& message, bool connected)
{
    Q_UNUSED(message)
    Q_UNUSED(connected)
}

void BricsCadPage::appendBridgeLog(const QString& message)
{
    const QString line = message.startsWith(QLatin1Char('['))
        ? message
        : QStringLiteral("[%1] %2").arg(logTime(), message);

    QFile logFile(QDir::temp().filePath(QStringLiteral("BareboneQtApp.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        logFile.write(line.toUtf8());
        logFile.write("\n");
    }

    if (m_bridgeLog) {
        m_bridgeLog->appendPlainText(line);
    }
}

void BricsCadPage::writeBridgeToken() const
{
    QFile file(bridgeTokenPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << m_bridgeToken << '\n';
}

QString BricsCadPage::bridgeTokenPath() const
{
    return QDir::temp().filePath(QString::fromLatin1(kBridgeTokenFileName));
}
