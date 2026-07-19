#pragma once

#include "../core/ConfigManager.h"
#include "AiWebBridge.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QWebChannel>
#include <QWebEngineView>
#include <QWidget>

class ChatPage final : public QWidget {
    Q_OBJECT

public:
    enum class Workspace {
        Chat,
        BricsCad,
    };

    explicit ChatPage(ConfigManager& config, Workspace workspace, QWidget* parent = nullptr);

    QString workspaceName() const;
    void setBridgeStatus(const QString& message, bool connected);
    void appendBridgeLog(const QString& message);

private:
    void buildUi();
    void sendChatPrompt(const QString& prompt, const QJsonObject& context = {});
    void handleChatResponse(const QJsonObject& response);
    void checkLocalAiStatus();
    void appendAgentMessage(const QString& speaker, const QString& message, const QVariantMap& extra = {});
    void setAgentBusy(bool busy);
    void emitShellState() const;
    void emitThemeAndLanguage() const;
    void resetChatSession();

    QVariantList loadWorkflowList() const;
    QVariantMap workflowById(const QString& workflowId) const;
    void emitWorkflowState() const;

    ConfigManager& m_config;
    Workspace m_workspace = Workspace::Chat;

    QWebEngineView* m_webView = nullptr;
    AiWebBridge* m_bridge = nullptr;
    QWebChannel* m_channel = nullptr;
    QNetworkAccessManager* m_network = nullptr;

    QJsonArray m_conversation;
    bool m_busy = false;
    bool m_localAiReachable = false;
    QString m_localAiStatusMessage = QStringLiteral("Lokale AI nicht geprueft");

    QVariantList m_workflows;
    QVariantMap m_selectedWorkflow;

    QString m_bridgeStatusMessage = QStringLiteral("Keine BRX Verbindung aktiv");
    bool m_bridgeConnected = false;
    QStringList m_bridgeLogLines;
};
