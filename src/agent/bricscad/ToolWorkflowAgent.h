#pragma once

#include "LocalAiAgentRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class ToolWorkflowAgent final {
public:
    using WorkflowLoader = std::function<QJsonObject(const QString&)>;
    using Callback = std::function<void(QJsonObject)>;

    struct Request {
        QString prompt;
        QString sessionId;
        QJsonObject route;
        QJsonArray catalog;
        QJsonArray workflowIndex;
        QString manualWorkflowId;
        QJsonObject selectedWorkflow;
        int operationGeneration = 0;
        bool bricsCadMode = true;
    };

    void run(
        const Request& request,
        LocalAiAgentRuntime& runtime,
        WorkflowLoader workflowLoader,
        Callback callback) const;

    static QJsonArray effectiveTools(
        const QJsonArray& catalog,
        const QJsonObject& route,
        const QString& prompt,
        const QJsonArray& selectedWorkflowObjects,
        const QString& pendingSourcePrompt = {});
};
