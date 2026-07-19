#pragma once

#include "LocalAiAgentRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class ConversationHistoryAgent final {
public:
    struct Request {
        QString prompt;
        QJsonArray conversation;
        QString sessionId;
        int operationGeneration = 0;
        bool bricsCadMode = true;
    };

    using Callback = std::function<void(QJsonObject)>;

    void run(const Request& request, LocalAiAgentRuntime& runtime, Callback callback) const;

    static QJsonObject fallback(const QString& prompt);
    static QJsonObject normalize(QJsonObject object, const QString& prompt, const QJsonArray& compactConversation);
    static QJsonArray compactConversation(const QJsonArray& conversation, int maxMessages = 32, int maxChars = 700);
};
