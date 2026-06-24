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
    void confirmProposal();
    void clearProposal();
    void newChat();
    void openSession(const QString& sessionId, const QVariantList& history);
    void setReasoningEffort(const QString& effort);
    void setChatMode(const QString& mode);
    void setTrainingMode(bool enabled);
    void saveClientState(const QString& stateJson);
    bool copyText(const QString& text);

Q_SIGNALS:
    void uiReady();
    void promptSubmitted(const QString& prompt);
    void promptSubmittedWithContext(const QString& prompt, const QVariantMap& context);
    void proposalConfirmed();
    void proposalClearedByUser();
    void newChatRequested();
    void sessionOpened(const QString& sessionId, const QVariantList& history);
    void reasoningEffortChanged(const QString& effort);
    void chatModeChanged(const QString& mode);
    void trainingModeChanged(bool enabled);
    void clientStateSaved(const QString& stateJson);

    void messageAdded(const QVariantMap& message);
    void statusChanged(const QString& status);
    void proposalChanged(const QVariantMap& proposal);
    void proposalCleared();
    void reasoningEffortApplied(const QString& effort);
    void trainingModeApplied(bool enabled);
    void clientStateLoaded(const QString& stateJson);
    void contextBudgetChanged(const QVariantMap& budget);
    void localAiStatusChanged(const QString& message, bool connected);
    void bridgeStatusChanged(const QString& message, bool connected);
    void bridgeLogAdded(const QString& message);
};
