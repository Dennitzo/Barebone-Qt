#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class RevitAgent final : public QObject {
    Q_OBJECT

public:
    explicit RevitAgent(QObject* parent = nullptr);

    QString statusMessage() const;
    bool isConnected() const;
    QStringList recentLogLines() const;

    void requestPromptContext(std::function<void(const QJsonObject&)> callback);
    QJsonArray toolDefinitions() const;
    bool executeReadOnlyToolCall(const QString& name, const QJsonObject& arguments, std::function<void(const QJsonObject&)> callback);
    bool extractProposal(QString& assistantContent, QVariantMap* uiProposal);
    void executePendingProposal(std::function<void(const QJsonObject&)> callback);
    void clearPendingProposal();

Q_SIGNALS:
    void bridgeStatusChanged(const QString& message, bool connected);
    void bridgeLogAdded(const QString& message);

private:
    struct PendingRequest {
        QString method;
        std::function<void(const QJsonObject&)> callback;
        QTimer* timer = nullptr;
    };

    void startBridgeServer();
    void handleBridgeSocket(QTcpSocket* socket);
    void handleBridgeLine(const QByteArray& line);
    void handleBridgeMessage(const QJsonObject& message);
    bool sendRequest(const QString& method, const QJsonObject& params, std::function<void(const QJsonObject&)> callback);
    void completePendingRequest(int id, const QJsonObject& response);
    void setBridgeStatus(const QString& message, bool connected);
    void appendBridgeLog(const QString& message);
    void writeBridgeToken() const;
    QString bridgeTokenPath() const;
    QVariantMap proposalForUi(const QJsonObject& proposal) const;
    bool isAllowedProposal(const QString& method) const;

    QTcpServer* m_bridgeServer = nullptr;
    QTcpSocket* m_revitSocket = nullptr;
    QByteArray m_revitReadBuffer;
    QString m_bridgeToken;
    bool m_revitAuthenticated = false;
    QString m_statusMessage = QStringLiteral("Revit Bridge nicht erreichbar");
    bool m_connected = false;
    QStringList m_logLines;
    int m_nextRequestId = 1;
    QHash<int, PendingRequest> m_pendingRequests;
    QJsonObject m_pendingProposal;
};
