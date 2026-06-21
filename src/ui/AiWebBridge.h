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
    void confirmProposal();
    void clearProposal();
    void newChat();
    void openSession(const QString& sessionId, const QVariantList& history);

Q_SIGNALS:
    void uiReady();
    void promptSubmitted(const QString& prompt);
    void proposalConfirmed();
    void proposalClearedByUser();
    void newChatRequested();
    void sessionOpened(const QString& sessionId, const QVariantList& history);

    void messageAdded(const QVariantMap& message);
    void statusChanged(const QString& status);
    void proposalChanged(const QVariantMap& proposal);
    void proposalCleared();
    void bridgeStatusChanged(const QString& message, bool connected);
    void bridgeLogAdded(const QString& message);
};
