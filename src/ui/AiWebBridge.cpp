#include "AiWebBridge.h"

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

void AiWebBridge::confirmProposal()
{
    Q_EMIT proposalConfirmed();
}

void AiWebBridge::clearProposal()
{
    Q_EMIT proposalClearedByUser();
}

void AiWebBridge::newChat()
{
    Q_EMIT newChatRequested();
}

void AiWebBridge::openSession(const QString& sessionId, const QVariantList& history)
{
    Q_EMIT sessionOpened(sessionId, history);
}
