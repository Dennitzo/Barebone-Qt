#pragma once

#include "LocalAiAgentRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class CalculationAgent final {
public:
    struct Request {
        QString prompt;
        QString sessionId;
        QJsonObject route;
        QJsonArray conversation;
        QJsonObject conversationContext;
        QJsonArray selectedWorkflows;
        QJsonArray workflowIndex;
        QJsonObject selectedWorkflowSlotValues;
        int operationGeneration = 0;
        bool bricsCadMode = true;
    };

    using WorkflowLoader = std::function<QJsonObject(const QString&)>;
    using Callback = std::function<void(QJsonObject)>;

    void run(
        const Request& request,
        LocalAiAgentRuntime& runtime,
        WorkflowLoader workflowLoader,
        Callback callback) const;

    static QJsonObject deterministicFloorPlanCalculation(const Request& request, WorkflowLoader workflowLoader);
};
