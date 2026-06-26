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

void AiWebBridge::clearSelectedWorkflow()
{
    Q_EMIT workflowSelectionCleared();
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
