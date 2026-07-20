#pragma once

#include "../agent/bricscad/BricsCadAgentCoordinator.h"
#include "../agent/bricscad/BricsCadFinalAgent.h"
#include "../agent/bricscad/LocalAiAgentRuntime.h"
#include "../core/ConfigManager.h"
#include "AiWebBridge.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
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
        int operationGeneration = 0;
        std::function<void(const QJsonObject&)> handler;
    };

    struct AgentSessionState {
        QJsonArray conversation;
        QJsonObject pendingProposal;
        QJsonObject pendingDraft;
        QJsonObject lastToolResult;
        QJsonObject lastBricsCadExecution;
    };

    struct ContextBuildResult {
        QJsonArray messages;
        QJsonObject envelope;
        int estimatedTokens = 0;
        int historyMessages = 0;
        bool minimized = false;
        int compressedHistoryMessages = 0;
    };

    struct DeferredAgentPrompt {
        QString prompt;
        QJsonObject documentContext;
    };

    void startBridgeServer();
    void appendBridgeLog(const QString& message);
    void emitCapabilitiesStatusToWeb() const;
    void sendAgentPrompt(const QString& prompt, const QJsonObject& documentContext = {});
    void routeUnifiedAgentPrompt(const QString& prompt, const QJsonObject& documentContext);
    void continueUnifiedAgentRequest(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route);
    void startBricsCadParallelPreparation(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route);
    void sendUnifiedAgentRequest(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route);
    QString enqueueAiPost(
        QNetworkRequest request,
        const QJsonObject& payload,
        const QString& kind,
        int priority,
        bool background,
        bool cancellable,
        const QString& dedupeKey,
        std::function<void(const LocalAiJobScheduler::Result&)> callback);
    void emitAgentRuntimeStatus(const QJsonObject& status) const;
    QJsonObject sanitizedPromptContext(const QJsonObject& context) const;
    void sendAgentEnvelope(const QJsonObject& envelope, const QString& userHistoryContent, bool storeUserMessage, const QString& logLabel);
    void sendWorkflowTrainingPrompt(const QString& prompt, bool compactContext = false);
    void saveGeneralWorkflowFromTraining(
        const QString& userInstruction = QString(),
        int retryCount = 0,
        const QString& validationError = {},
        const QString& rejectedContent = {},
        const QString& selectedMessageText = QString(),
        const QString& sessionTitle = QString());
    void saveGeneralWorkflowFromMessage(const QString& messageId, const QString& messageText, const QString& sessionTitle);
    void saveBricsCadWorkflowFromSession(const QString& sessionTitle, int retryCount = 0, const QString& validationError = {}, const QString& rejectedContent = {});
    void saveBricsCadWorkflowFromExecution(const QString& messageId, const QString& sessionTitle);
    void saveBricsCadWorkflowFromLastExecution(const QString& sessionTitle);
    QJsonObject workflowSaveCompressedTitleContext(const QString& selectedMessageText, const QString& sessionTitle) const;
    bool saveGeneralWorkflowFinalDraft(QString* savedPath = nullptr, QString* errorMessage = nullptr);
    void handleWorkflowTrainingReply(const QString& content);
    QJsonObject workflowTrainingEnvelope(const QString& prompt, bool compactContext = false) const;
    QJsonObject workflowTrainingState() const;
    void mergeWorkflowTrainingContext(const QJsonObject& context);
    void mergeWorkflowTrainingSlotValues(const QJsonObject& slots);
    QJsonArray unresolvedWorkflowTrainingMissing(const QJsonArray& missing) const;
    QJsonArray workflowTrainingIndex() const;
    QString generalWorkflowsDirectoryPath() const;
    QString bricsCadWorkflowsDirectoryPath() const;
    QJsonArray generalWorkflowIndex() const;
    QJsonArray bricsCadWorkflowIndex() const;
    QJsonObject loadGeneralWorkflowById(const QString& workflowId, QString* fileName = nullptr, QString* errorMessage = nullptr) const;
    QJsonObject loadBricsCadWorkflowById(const QString& workflowId, QString* fileName = nullptr, QString* errorMessage = nullptr) const;
    bool saveGeneralWorkflowFromObject(const QJsonObject& workflow, QString* savedPath = nullptr, QString* errorMessage = nullptr) const;
    bool deleteGeneralWorkflowById(const QString& workflowId, QString* deletedPath = nullptr, QString* errorMessage = nullptr);
    bool deleteBricsCadWorkflowById(const QString& workflowId, QString* deletedPath = nullptr, QString* errorMessage = nullptr);
    bool deleteWorkflowById(const QString& workflowId, QString* deletedPath = nullptr, QString* errorMessage = nullptr);
    void emitWorkflowListToWeb() const;
    QJsonObject loadWorkflowById(const QString& workflowId, QString* fileName = nullptr, QString* errorMessage = nullptr) const;
    void selectWorkflowForChat(const QString& workflowId);
    void clearSelectedWorkflowForChat();
    void exportAgentMessageToPdf(const QString& messageId, const QString& suggestedTitle);
    QJsonObject selectedWorkflowSummary() const;
    bool saveWorkflowFromTraining(const QJsonObject& workflow, QString* savedPath, QString* errorMessage) const;
    bool validateWorkflowForTraining(QJsonObject& workflow, QString& errorMessage) const;
    bool validateWorkflowStepForTraining(const QJsonObject& step, int index, QString& errorMessage, const QString& pathPrefix = QString()) const;
    bool retryWorkflowTrainingAfterValidationFailure(const QString& rejectedContent, const QJsonObject& rejectedObject, const QString& errorMessage);
    bool retryGeneralWorkflowSaveAfterValidationFailure(
        const QJsonObject& saveContext,
        const QString& rejectedContent,
        const QString& errorMessage);
    void startWorkflowTrainingSaveReview(const QString& rejectedContent, const QJsonObject& reply, const QJsonObject& workflow);
    void confirmWorkflowTrainingSaveReview();
    void confirmWorkflowTrainingFinalSave();
    void emitWorkflowTrainingFinalSavePrompt();
    void clearWorkflowTrainingPrompts();
    void clearWorkflowTrainingSavePrompt();
    void validateWorkflowWithBrxAndSave(const QString& rejectedContent, const QJsonObject& reply, const QJsonObject& workflow);
    void finishWorkflowTrainingSave(const QJsonObject& reply, const QJsonObject& workflow);
    void scheduleLocalAiStatusPoll();
    void refreshLocalContextWindow(bool force = false);
    void handleLocalContextWindowResponse(const QJsonObject& response);
    void emitContextBudget(int estimatedTokens = -1, bool minimized = false, const QString& detail = QString());
    void emitSessionTitleSuggestion(const QString& title) const;
    int effectiveContextWindowTokens() const;
    int inputBudgetTokens(int requestedOutputTokens) const;
    int adjustedOutputTokenLimit(int requestedOutputTokens) const;
    int dynamicOutputTokenTarget(int minimumTokens, int maximumTokens, int contextDivisor) const;
    int adjustedOutputTokenLimitForMessages(const QJsonArray& messages, int requestedOutputTokens) const;
    int estimateTokensForText(const QString& text) const;
    int estimateTokensForMessages(const QJsonArray& messages) const;
    QJsonObject documentContextWithTokenBudget(const QJsonObject& context, int tokenBudget, bool* minimized = nullptr) const;
    ContextBuildResult buildGeneralMessagesForBudget(
        const QJsonObject& envelope,
        const QJsonObject& instructionMessage,
        int requestedOutputTokens) const;
    QJsonObject fallbackFocusedConversationContext(const QString& prompt) const;
    QJsonObject normalizedFocusedConversationContext(const QJsonObject& object, const QString& prompt) const;
    QJsonObject conversationAccessForFocusedContext(const QJsonObject& focusedContext) const;
    QJsonObject compactConversationHistoryMessage(int index, bool fullText, int maxChars = 2200) const;
    QJsonArray conversationHistoryMessagesRange(int start, int count, bool fullText) const;
    void handleAgentReply(const QString& content);
    void processAgentProposal(
        const QString& rejectedContent,
        const QJsonObject& rejectedReply,
        QJsonObject proposal,
        const QString& sessionTitleSuggestion);
    bool retryAgentAfterValidationFailure(const QString& rejectedContent, const QJsonObject& rejectedObject, const QString& errorMessage);
    void discardLastAssistantConversation(const QString& content);
    QJsonObject normalizedAgentProposal(const QJsonObject& proposal) const;
    QJsonObject floorPlanBatchProposalFromCalculationResult(const QJsonObject& baseProposal) const;
    QJsonObject normalizedAgentAction(const QJsonObject& action) const;
    QJsonArray agentProposalActions(const QJsonObject& proposal) const;
    QJsonArray expandedAgentActions(const QJsonObject& action) const;
    bool proposalRequiresUserConfirmation(const QJsonObject& proposal) const;
    QJsonObject agentPreflightParams(const QJsonObject& proposal) const;
    void preflightAgentProposal(const QString& rejectedContent, const QJsonObject& rejectedObject, const QJsonObject& proposal);
    void handleAgentContextRequest(const QJsonObject& request);
    void continueAgentWithContextResult(const QJsonObject& contextRequest, const QJsonObject& contextResponse);
    bool selectedWorkflowVerificationPassed(const QJsonObject& response, QString& errorMessage) const;
    bool floorPlanBatchVerificationGatePassed(const QJsonObject& proposal, const QJsonArray& actions, int completedActionIndex, const QJsonArray& results, QString& errorMessage) const;
    void continueAgentAfterToolResult(const QJsonObject& proposal, const QJsonObject& response);
    void continueAgentAfterToolFailure(const QJsonObject& proposal, const QJsonObject& response, const QString& errorMessage);
    void executeAgentProposal();
    void executeAgentActionBatch(const QJsonObject& proposal, const QJsonArray& actions, int index, QJsonArray results);
    void clearLastBricsCadExecution();
    void recordLastBricsCadExecution(const QJsonObject& proposal, const QJsonArray& plannedActions, const QJsonArray& results, const QJsonObject& batchResult);
    QJsonObject bricsCadExecutionForMessage(const QString& messageId) const;
    QJsonObject latestBricsCadExecutionFromConversation() const;
    QVariantMap bricsCadExecutionMessageExtra() const;
    QJsonObject workflowFromBricsCadExecution(const QJsonObject& execution, const QString& sessionTitle, QString* errorMessage = nullptr) const;
    QJsonObject workflowFromLastBricsCadExecution(const QString& sessionTitle, QString* errorMessage = nullptr) const;
    void requestAgentExecutionSummary(const QJsonObject& proposal, const QJsonArray& actions, const QJsonArray& results, const QJsonObject& batchResult, const QString& fallbackSummary);
    void appendAgentChat(const QString& speaker, const QString& message, const QVariantMap& extra = {});
    void clearAgentProposal();
    void setAgentWaitingForUser(const QJsonObject& reply);
    void setAgentProposal(const QJsonObject& proposal);
    void setAgentBusy(bool busy);
    void processDeferredAgentPrompt();
    void cancelCurrentOperation();
    bool isAgentConfirmation(const QString& prompt) const;
    bool validateAgentProposal(const QJsonObject& proposal, QString& errorMessage) const;
    bool validateAgentAction(const QJsonObject& action, QString& errorMessage) const;
    bool validateLayersEnsureManyParams(const QJsonObject& params, QString& errorMessage) const;
    bool validateToolParams(const QJsonObject& params, const QJsonObject& inputSchema, QString& errorMessage) const;
    bool validateSchemaValue(const QJsonValue& value, const QJsonObject& schema, const QString& path, QString& errorMessage) const;
    QString aiChatCompletionContent(const QJsonObject& response, QString* reasoningText = nullptr) const;
    QJsonArray availableAgentTools() const;
    QJsonArray availableAgentToolsForRoute(const QJsonObject& route, const QString& prompt) const;
    QJsonArray compactWorkflowSelectorList() const;
    QJsonArray selectedWorkflowObjectsForRoute(const QJsonObject& route) const;
    QJsonArray workflowHintObjectsForRoute(const QJsonObject& route) const;
    QJsonArray readOnlyMethodsForRoute(const QJsonObject& route) const;
    QJsonObject toolDefinition(const QString& name) const;
    QString bridgeMethodForTool(const QString& name) const;
    bool isAllowedContextMethod(const QString& method) const;
    QJsonObject currentAgentContext() const;
    QJsonObject compactAgentStateSummary(const QString& prompt, const QJsonObject& documentContext, const QJsonObject& route) const;
    QJsonObject agentRequestEnvelope(const QString& prompt, const QJsonObject& documentContext = {}, const QJsonObject& route = {}) const;
    DrawingContextStore& drawingContextStore();
    const DrawingContextStore& drawingContextStore() const;
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
    bool sendBridgeActionRequest(
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
    void setAssistantWorkspace(const QString& workspace);
    void setChatMode(const QString& mode);
    void setTrainingMode(bool enabled);
    void emitUiThemeToWeb() const;
    void emitUiLanguageToWeb() const;
    bool isBricsCadMode() const;
    bool isChatWorkspace() const;
    QString unifiedAssistantState() const;
    void setUnifiedAssistantState(const QString& stateJson);

    ConfigManager& m_config;
    QTcpServer* m_bridgeServer = nullptr;
    QTcpSocket* m_brxSocket = nullptr;
    QNetworkAccessManager* m_aiNetwork = nullptr;
    LocalAiAgentRuntime* m_aiRuntime = nullptr;
    BricsCadAgentCoordinator* m_bricsCadCoordinator = nullptr;
    QTimer* m_localAiPollTimer = nullptr;
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
    bool m_trainingMode = false;
    QString m_chatMode = QStringLiteral("bricscad");
    QString m_assistantWorkspace = QStringLiteral("bricscad");
    QString m_reasoningEffort = QStringLiteral("high");
    QString m_localAiStatusMessage = QStringLiteral("Lokale AI nicht erreichbar");
    QString m_contextWindowModel;
    int m_contextWindowTokens = 0;
    int m_contextWindowMaxTokens = 0;
    int m_lastContextBudgetUsedTokens = 0;
    int m_agentValidationRetries = 0;
    int m_repeatedAgentContextRequestCount = 0;
    QString m_lastAgentContextRequestSignature;
    bool m_agentContextLoopBlocked = false;
    bool m_parallelPreparationActive = false;
    bool m_agentPreActionSaveCompleted = false;
    bool m_forceAgentReasoningOffNextRequest = false;
    int m_trainingValidationRetries = 0;
    int m_nextRequestId = 1;
    int m_operationGeneration = 1;
    QHash<int, PendingBridgeRequest> m_pendingRequests;
    QString m_queuedAgentPrompt;
    QJsonObject m_queuedAgentRoute;
    QString m_agentSessionId = QStringLiteral("session-default");
    QString m_lastAgentUserPrompt;
    QString m_activeReasoningRunId;
    QHash<QString, AgentSessionState> m_agentSessions;
    QJsonArray m_agentConversation;
    QJsonObject m_pendingAgentProposal;
    QJsonObject m_pendingAgentDraft;
    QJsonObject m_lastAgentToolResult;
    QJsonObject m_lastBricsCadExecution;
    QStringList m_agentRejectedResponseSignatures;
    QVector<DeferredAgentPrompt> m_deferredAgentPrompts;
    QJsonArray m_trainingConversation;
    QStringList m_trainingRejectedResponseSignatures;
    QJsonObject m_trainingWorkflowContext;
    QJsonObject m_trainingSlotValues;
    QJsonArray m_trainingMissing;
    int m_generalWorkflowSaveRetries = 0;
    QStringList m_generalWorkflowSaveRejectedSignatures;
    QJsonObject m_lastGeneralWorkflowSaveContext;
    QString m_trainingPhase = QStringLiteral("idle");
    bool m_trainingSaveReviewPending = false;
    bool m_trainingReviewConfirmed = false;
    QString m_trainingPendingReviewContent;
    QJsonObject m_trainingPendingReviewReply;
    QJsonObject m_trainingPendingReviewWorkflow;
    bool m_trainingFinalSavePending = false;
    QJsonObject m_trainingFinalSaveWorkflow;
    QJsonArray m_trainingFinalSaveActions;
    QString m_trainingPromptSignature;
    QString m_selectedWorkflowId;
    QJsonObject m_selectedWorkflow;
    QJsonObject m_selectedWorkflowSlotValues;
    QJsonObject m_workflowTestWorkflow;
    QJsonArray m_workflowTestActions;
    QJsonObject m_workflowTestDefaults;
    qint64 m_workflowTestStartedMs = 0;
    int m_workflowTestGeneration = 0;
    QJsonObject m_lastDocumentContext;
    QJsonObject m_lastAgentRoute;
    QJsonObject m_lastFocusedConversationContext;
    bool m_nextAgentMessageContinuationAvailable = false;
    QString m_nextAgentMessageContinuationReason;
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

