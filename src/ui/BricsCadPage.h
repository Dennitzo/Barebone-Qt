#pragma once

#include "../core/ConfigManager.h"
#include "ChatPage.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWidget>

class BricsCadPage final : public QWidget {
    Q_OBJECT

public:
    explicit BricsCadPage(ConfigManager& config, QWidget* parent = nullptr);

private:
    void startBridgeServer();
    void handleBridgeSocket(QTcpSocket* socket);
    void handleBridgeLine(const QByteArray& line);
    void handleBridgeMessage(const QJsonObject& message);
    void sendBridgeError(int id, const QString& message);
    void setBridgeStatus(const QString& message, bool connected);
    void appendBridgeLog(const QString& message);
    void writeBridgeToken() const;
    QString bridgeTokenPath() const;

    ChatPage* m_chatPage = nullptr;
    QTcpServer* m_bridgeServer = nullptr;
    QTcpSocket* m_brxSocket = nullptr;
    QByteArray m_brxReadBuffer;
    QString m_bridgeToken;
    bool m_brxAuthenticated = false;
};
