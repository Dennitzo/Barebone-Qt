#pragma once

#include "LocalAiAgentRuntime.h"
#include "../DrawingContextStore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class DrawingContextAgent final {
public:
    using BridgeCallback = std::function<void(const QJsonObject&)>;
    using BridgeRequest = std::function<bool(const QString&, const QJsonObject&, int, BridgeCallback)>;
    using SelectionCallback = std::function<void(const QJsonArray&)>;
    using Callback = std::function<void(QJsonObject)>;

    struct Request {
        QString prompt;
        QString sessionId;
        QJsonObject capabilities;
        bool brxAuthenticated = false;
        int operationGeneration = 0;
        bool bricsCadMode = true;
    };

    void run(
        const Request& request,
        LocalAiAgentRuntime& runtime,
        DrawingContextStore& store,
        BridgeRequest bridgeRequest,
        SelectionCallback selectionCallback,
        Callback callback) const;

    static QJsonArray plannedReadOnlyRequests(const QString& prompt, const QJsonObject& capabilities);
};
