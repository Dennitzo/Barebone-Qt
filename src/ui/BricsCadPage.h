#pragma once

#include "../core/ConfigManager.h"
#include "AiWebBridge.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWebChannel>
#include <QWebEngineView>
#include <QWidget>

#include <functional>

class BricsCadPage final : public QWidget {
    Q_OBJECT

public:
    explicit BricsCadPage(ConfigManager& config, QWidget* parent = nullptr);
    QWidget* agentWidget() const;

private:
    struct PendingBridgeRequest {
        QString method;
        QTimer* timeout = nullptr;
        std::function<void(const QJsonObject&)> handler;
    };

    struct AgentSessionState {
        QJsonArray conversation;
        QJsonObject pendingProposal;
        QJsonObject pendingDraft;
        QJsonObject lastToolResult;
    };

    struct ContextBuildResult {
        QJsonArray messages;
        QJsonObject envelope;
        int estimatedTokens = 0;
        int historyMessages = 0;
        bool minimized = false;
    };

    void startBridgeServer();
    void appendBridgeLog(const QString& message);
    void emitCapabilitiesStatusToWeb() const;
    void sendAgentPrompt(const QString& prompt, const QJsonObject& documentContext = {});
    void sendAgentRequest(const QString& prompt, const QJsonObject& documentContext = {});
    void sendAgentRequestWithPrefetchedContext(const QString& prompt, const QString& method, const QJsonObject& params, const QJsonObject& documentContext = {});
    void sendAgentEnvelope(const QJsonObject& envelope, const QString& userHistoryContent, bool storeUserMessage, const QString& logLabel);
    void sendGeneralChatRequest(const QString& prompt, const QJsonObject& documentContext = {}, bool forceNoReasoning = false);
    void refreshLocalContextWindow(bool force = false);
    void handleLocalContextWindowResponse(const QJsonObject& response);
    void emitContextBudget(int estimatedTokens = -1, bool minimized = false, const QString& detail = QString()) const;
    int effectiveContextWindowTokens() const;
    int inputBudgetTokens(int requestedOutputTokens) const;
    int adjustedOutputTokenLimit(int requestedOutputTokens) const;
    int estimateTokensForText(const QString& text) const;
    int estimateTokensForMessages(const QJsonArray& messages) const;
    QJsonObject documentContextWithTokenBudget(const QJsonObject& context, int tokenBudget, bool* minimized = nullptr) const;
    ContextBuildResult buildGeneralMessagesForBudget(const QString& prompt, const QJsonObject& documentContext, int requestedOutputTokens) const;
    void handleAgentReply(const QString& content);
    bool retryAgentAfterValidationFailure(const QString& rejectedContent, const QJsonObject& rejectedObject, const QString& errorMessage);
    void discardLastAssistantConversation(const QString& content);
    QJsonObject normalizedAgentProposal(const QJsonObject& proposal) const;
    QJsonObject normalizedAgentAction(const QJsonObject& action) const;
    QJsonArray agentProposalActions(const QJsonObject& proposal) const;
    QJsonArray expandedAgentActions(const QJsonObject& action) const;
    QJsonObject agentPreflightParams(const QJsonObject& proposal) const;
    void preflightAgentProposal(const QString& rejectedContent, const QJsonObject& rejectedObject, const QJsonObject& proposal);
    void handleAgentContextRequest(const QJsonObject& request);
    void continueAgentWithContextResult(const QJsonObject& contextRequest, const QJsonObject& contextResponse);
    void continueAgentAfterToolResult(const QJsonObject& proposal, const QJsonObject& response);
    void continueAgentAfterToolFailure(const QJsonObject& proposal, const QJsonObject& response, const QString& errorMessage);
    void executeAgentProposal();
    void executeAgentActionBatch(const QJsonObject& proposal, const QJsonArray& actions, int index, QJsonArray results);
    void requestAgentExecutionSummary(const QJsonObject& proposal, const QJsonArray& actions, const QJsonArray& results, const QJsonObject& batchResult, const QString& fallbackSummary);
    void appendAgentChat(const QString& speaker, const QString& message);
    void clearAgentProposal();
    void setAgentWaitingForUser(const QJsonObject& reply);
    void setAgentProposal(const QJsonObject& proposal);
    void setAgentBusy(bool busy);
    bool isAgentConfirmation(const QString& prompt) const;
    bool validateAgentProposal(const QJsonObject& proposal, QString& errorMessage) const;
    bool validateAgentAction(const QJsonObject& action, QString& errorMessage) const;
    bool validateLayersEnsureManyParams(const QJsonObject& params, QString& errorMessage) const;
    bool validateToolParams(const QJsonObject& params, const QJsonObject& inputSchema, QString& errorMessage) const;
    bool validateSchemaValue(const QJsonValue& value, const QJsonObject& schema, const QString& path, QString& errorMessage) const;
    QString aiChatCompletionContent(const QJsonObject& response, QString* reasoningText = nullptr) const;
    QJsonArray availableAgentTools() const;
    QJsonObject layersEnsureManyToolDefinition() const;
    QJsonObject toolDefinition(const QString& name) const;
    QString bridgeMethodForTool(const QString& name) const;
    bool isAllowedContextMethod(const QString& method) const;
    QJsonObject currentAgentContext() const;
    QJsonObject agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext = {}) const;
    bool ensureBridgeCapabilitiesForPrompt(const QString& prompt);
    void continueQueuedAgentPrompt();
    void requestBridgeCapabilities();
    void handleBridgeSocket(QTcpSocket* socket);
    bool handleBridgeLine(const QByteArray& line, bool* incomplete = nullptr);
    void handleBridgeMessage(const QJsonObject& message);
    bool sendBridgeMessage(const QJsonObject& message);
    bool sendBridgeRequest(
        const QString& method,
        const QJsonObject& params,
        int timeoutMs,
        std::function<void(const QJsonObject&)> handler);
    void completeBridgeRequest(int id, const QJsonObject& message);
    void failPendingBridgeRequests(const QString& message);
    void forceBridgeReconnect(const QString& reason, bool preserveQueuedPrompt);
    void setPluginStatus(const QString& message, bool connected);
    void setLocalAiStatus(const QString& message, bool connected);
    void writeBridgeToken() const;
    void resetAgentConversation();
    void openAgentSession(const QString& sessionId, const QVariantList& history);
    void saveCurrentAgentSession();
    QJsonArray conversationFromWebHistory(const QVariantList& history) const;
    void setReasoningEffort(const QString& effort);
    void setChatMode(const QString& mode);
    bool isBricsCadMode() const;

    ConfigManager& m_config;
    QTcpServer* m_bridgeServer = nullptr;
    QTcpSocket* m_brxSocket = nullptr;
    QNetworkAccessManager* m_aiNetwork = nullptr;
    QByteArray m_brxReadBuffer;
    QByteArray m_brxJsonAccumulator;
    QString m_bridgeToken;
    bool m_brxAuthenticated = false;
    bool m_agentBusy = false;
    bool m_capabilitiesRequested = false;
    bool m_preserveQueuedAgentPromptOnDisconnect = false;
    bool m_contextWindowRequestInFlight = false;
    bool m_contextWindowAvailable = false;
    bool m_localAiReachable = false;
    QString m_chatMode = QStringLiteral("bricscad");
    QString m_reasoningEffort = QStringLiteral("high");
    QString m_localAiStatusMessage = QStringLiteral("Lokale AI wird geprüft");
    QString m_contextWindowModel;
    int m_contextWindowTokens = 0;
    int m_contextWindowMaxTokens = 0;
    int m_agentValidationRetries = 0;
    int m_nextRequestId = 1;
    QHash<int, PendingBridgeRequest> m_pendingRequests;
    QString m_queuedAgentPrompt;
    QString m_agentSessionId = QStringLiteral("session-default");
    QString m_lastAgentUserPrompt;
    QHash<QString, AgentSessionState> m_agentSessions;
    QJsonArray m_agentConversation;
    QJsonObject m_pendingAgentProposal;
    QJsonObject m_pendingAgentDraft;
    QJsonObject m_lastAgentToolResult;
    QJsonObject m_lastDocumentContext;
    QJsonObject m_brxCapabilities;
    QJsonArray m_currentSelection;
    QWidget* m_agentWidget = nullptr;
    QWebEngineView* m_agentWebView = nullptr;
    QWebChannel* m_agentChannel = nullptr;
    AiWebBridge* m_agentBridge = nullptr;
    QLabel* m_pluginStatus = nullptr;
    QLabel* m_bridgeStatus = nullptr;
    QPlainTextEdit* m_bridgeLog = nullptr;
};
