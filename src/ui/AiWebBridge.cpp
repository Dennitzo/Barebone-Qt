#include "AiWebBridge.h"

#include <QClipboard>
#include <QGuiApplication>

AiWebBridge::AiWebBridge(QObject* parent)
    : QObject(parent)
{
}

void AiWebBridge::ready()
{
    Q_EMIT uiReady();
}

void AiWebBridge::sendPrompt(const QString& prompt)
{
    Q_EMIT promptSubmitted(prompt);
}

void AiWebBridge::sendPromptWithContext(const QString& prompt, const QVariantMap& context)
{
    Q_EMIT promptSubmittedWithContext(prompt, context);
}

void AiWebBridge::checkLocalAiStatus()
{
    Q_EMIT localAiStatusCheckRequested();
}

void AiWebBridge::confirmProposal()
{
    Q_EMIT proposalConfirmed();
}

void AiWebBridge::clearProposal()
{
    Q_EMIT proposalClearedByUser();
}

void AiWebBridge::cancelCurrentOperation()
{
    Q_EMIT operationCancelledByUser();
}

void AiWebBridge::confirmWorkflowTrainingSave()
{
    Q_EMIT workflowTrainingSaveConfirmed();
}

void AiWebBridge::saveMessageAsWorkflow(const QString& messageId, const QString& messageText, const QString& sessionTitle)
{
    Q_EMIT messageWorkflowSaveRequested(messageId, messageText, sessionTitle);
}

void AiWebBridge::confirmWorkflowTrainingRun()
{
    Q_EMIT workflowTrainingRunConfirmed();
}

void AiWebBridge::confirmWorkflowTrainingFinalSave()
{
    Q_EMIT workflowTrainingFinalSaveConfirmed();
}

void AiWebBridge::newChat()
{
    Q_EMIT newChatRequested();
}

void AiWebBridge::openSession(const QString& sessionId, const QVariantList& history)
{
    Q_EMIT sessionOpened(sessionId, history);
}

void AiWebBridge::setReasoningEffort(const QString& effort)
{
    Q_EMIT reasoningEffortChanged(effort);
}

void AiWebBridge::setChatMode(const QString& mode)
{
    Q_EMIT chatModeChanged(mode);
}

void AiWebBridge::setTrainingMode(bool enabled)
{
    Q_EMIT trainingModeChanged(enabled);
}

void AiWebBridge::setAssistantWorkspace(const QString& workspace)
{
    Q_EMIT assistantWorkspaceChanged(workspace);
}

void AiWebBridge::saveClientState(const QString& stateJson)
{
    Q_EMIT clientStateSaved(stateJson);
}

void AiWebBridge::requestWorkflowList()
{
    Q_EMIT workflowListRequested();
}

void AiWebBridge::selectWorkflow(const QString& workflowId)
{
    Q_EMIT workflowSelected(workflowId);
}

void AiWebBridge::runWorkflowTest(const QString& workflowId)
{
    Q_EMIT workflowTestRequested(workflowId);
}

void AiWebBridge::deleteWorkflow(const QString& workflowId)
{
    Q_EMIT workflowDeleteRequested(workflowId);
}

void AiWebBridge::clearSelectedWorkflow()
{
    Q_EMIT workflowSelectionCleared();
}

void AiWebBridge::exportMessageToPdf(const QString& messageId, const QString& suggestedTitle)
{
    Q_EMIT messagePdfExportRequested(messageId, suggestedTitle);
}

void AiWebBridge::requestMathFormattingRepair(const QString& messageId, int revision, const QString& markdown, const QString& diagnosticsJson)
{
    Q_EMIT mathFormattingRepairRequested(messageId, revision, markdown, diagnosticsJson);
}

void AiWebBridge::acceptMathFormattingRepair(const QString& messageId, int revision, const QString& markdown)
{
    Q_EMIT mathFormattingRepairAccepted(messageId, revision, markdown);
}

bool AiWebBridge::copyText(const QString& text)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return false;
    }

    clipboard->setText(text, QClipboard::Clipboard);
    return true;
}
