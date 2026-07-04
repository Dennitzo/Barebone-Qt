#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

class AiWebBridge final : public QObject {
    Q_OBJECT

public:
    explicit AiWebBridge(QObject* parent = nullptr);

public Q_SLOTS:
    void ready();
    void sendPrompt(const QString& prompt);
    void sendPromptWithContext(const QString& prompt, const QVariantMap& context);
    void checkLocalAiStatus();
    void confirmProposal();
    void clearProposal();
    void cancelCurrentOperation();
    void confirmWorkflowTrainingSave();
    void saveMessageAsWorkflow(const QString& messageId, const QString& messageText, const QString& sessionTitle);
    void confirmWorkflowTrainingRun();
    void confirmWorkflowTrainingFinalSave();
    void newChat();
    void openSession(const QString& sessionId, const QVariantList& history);
    void setReasoningEffort(const QString& effort);
    void setChatMode(const QString& mode);
    void setTrainingMode(bool enabled);
    void setAssistantWorkspace(const QString& workspace);
    void saveClientState(const QString& stateJson);
    void requestWorkflowList();
    void selectWorkflow(const QString& workflowId);
    void deleteWorkflow(const QString& workflowId);
    void clearSelectedWorkflow();
    void exportMessageToPdf(const QString& messageId, const QString& suggestedTitle);
    void requestMathFormattingRepair(const QString& messageId, int revision, const QString& markdown, const QString& diagnosticsJson);
    void acceptMathFormattingRepair(const QString& messageId, int revision, const QString& markdown);
    bool copyText(const QString& text);

Q_SIGNALS:
    void uiReady();
    void promptSubmitted(const QString& prompt);
    void promptSubmittedWithContext(const QString& prompt, const QVariantMap& context);
    void localAiStatusCheckRequested();
    void proposalConfirmed();
    void proposalClearedByUser();
    void operationCancelledByUser();
    void workflowTrainingSaveConfirmed();
    void messageWorkflowSaveRequested(const QString& messageId, const QString& messageText, const QString& sessionTitle);
    void workflowTrainingRunConfirmed();
    void workflowTrainingFinalSaveConfirmed();
    void newChatRequested();
    void sessionOpened(const QString& sessionId, const QVariantList& history);
    void reasoningEffortChanged(const QString& effort);
    void chatModeChanged(const QString& mode);
    void trainingModeChanged(bool enabled);
    void assistantWorkspaceChanged(const QString& workspace);
    void clientStateSaved(const QString& stateJson);
    void workflowListRequested();
    void workflowSelected(const QString& workflowId);
    void workflowDeleteRequested(const QString& workflowId);
    void workflowSelectionCleared();
    void messagePdfExportRequested(const QString& messageId, const QString& suggestedTitle);
    void mathFormattingRepairRequested(const QString& messageId, int revision, const QString& markdown, const QString& diagnosticsJson);
    void mathFormattingRepairAccepted(const QString& messageId, int revision, const QString& markdown);

    void messageAdded(const QVariantMap& message);
    void statusChanged(const QString& status);
    void proposalChanged(const QVariantMap& proposal);
    void proposalCleared();
    void workflowTrainingSavePromptChanged(const QVariantMap& prompt);
    void workflowTrainingSavePromptCleared();
    void workflowTrainingRunPromptChanged(const QVariantMap& prompt);
    void workflowTrainingRunPromptCleared();
    void workflowTrainingFinalSavePromptChanged(const QVariantMap& prompt);
    void workflowTrainingFinalSavePromptCleared();
    void reasoningEffortApplied(const QString& effort);
    void trainingModeApplied(bool enabled);
    void assistantWorkspaceApplied(const QString& workspace);
    void clientStateLoaded(const QString& stateJson);
    void contextBudgetChanged(const QVariantMap& budget);
    void localAiStatusChanged(const QString& message, bool connected);
    void uiThemeChanged(const QString& theme);
    void uiLanguageChanged(const QString& language);
    void bridgeStatusChanged(const QString& message, bool connected);
    void bridgeLogAdded(const QString& message);
    void workflowListChanged(const QVariantList& workflows);
    void selectedWorkflowChanged(const QVariantMap& workflow);
    void workflowRunProgress(const QVariantMap& progress);
    void workflowRunFinished(const QVariantMap& result);
    void mathFormattingRepairCompleted(const QString& messageId, int revision, const QString& markdown);
    void mathFormattingRepairFailed(const QString& messageId, int revision, const QString& errorMessage);
    void sessionTitleSuggested(const QString& sessionId, const QString& title);
};
