#pragma once

#include "CalculationAgent.h"
#include "ConversationHistoryAgent.h"
#include "DrawingContextAgent.h"
#include "LocalAiAgentRuntime.h"
#include "ToolWorkflowAgent.h"
#include "../DrawingContextStore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVariantMap>

#include <functional>

class BricsCadAgentCoordinator final : public QObject {
    Q_OBJECT

public:
    struct RunRequest {
        QString prompt;
        QJsonObject documentContext;
        QJsonObject route;
        QJsonObject historyContext;
        QJsonArray conversation;
        QJsonObject capabilities;
        QJsonArray catalog;
        QJsonArray workflowIndex;
        QString sessionId;
        QString manualWorkflowId;
        QJsonObject selectedWorkflow;
        QJsonObject selectedWorkflowSlotValues;
        int generation = 0;
        bool brxAuthenticated = false;
    };

    explicit BricsCadAgentCoordinator(LocalAiAgentRuntime& runtime, QObject* parent = nullptr);

    DrawingContextStore& drawingContextStore();
    const DrawingContextStore& drawingContextStore() const;

    void setCurrentGenerationProvider(std::function<int()> provider);
    void setBridgeRequestHandler(DrawingContextAgent::BridgeRequest handler);
    void setWorkflowLoader(ToolWorkflowAgent::WorkflowLoader loader);
    void setLogHandler(std::function<void(const QString&)> handler);
    void setBusyHandler(std::function<void(bool)> handler);
    void setParallelActiveHandler(std::function<void(bool)> handler);
    void setActiveReasoningRunHandler(std::function<void(const QString&)> handler);
    void setReasoningProgressHandler(std::function<void(const QVariantMap&)> handler);
    void setFocusedConversationHandler(std::function<void(const QJsonObject&)> handler);
    void setSelectionHandler(std::function<void(const QJsonArray&)> handler);
    void setFinalRouteHandler(std::function<void(const QString&, const QJsonObject&, const QJsonObject&)> handler);

    bool isActive() const;
    void cancel();
    void start(const RunRequest& request);

private:
    struct PreparationState {
        int generation = 0;
        QString runId;
        QString prompt;
        QJsonObject documentContext;
        QJsonObject route;
        QJsonObject historyContext;
        QJsonObject results;
        QSet<QString> completed;
        bool finalStarted = false;
    };

    void emitSlotStatus(const QString& runId, const QString& slot, const QString& state, int revision, const QString& message);
    void appendLog(const QString& message) const;
    bool currentGenerationMatches(int generation) const;
    QString slotLabel(const QString& slot) const;
    QString slotDetail(const QString& slot, const QJsonObject& result) const;
    void finishSlot(const QSharedPointer<PreparationState>& state, const QString& slot, QJsonObject result);
    void startCalculation(const QSharedPointer<PreparationState>& state, const RunRequest& request, const QJsonObject& history);
    QJsonArray selectedWorkflowObjectsForRequest(const RunRequest& request) const;

    LocalAiAgentRuntime& m_runtime;
    DrawingContextStore m_drawingContextStore;
    ConversationHistoryAgent m_historyAgent;
    DrawingContextAgent m_drawingAgent;
    ToolWorkflowAgent m_toolWorkflowAgent;
    CalculationAgent m_calculationAgent;

    bool m_active = false;

    std::function<int()> m_currentGenerationProvider;
    DrawingContextAgent::BridgeRequest m_bridgeRequestHandler;
    ToolWorkflowAgent::WorkflowLoader m_workflowLoader;
    std::function<void(const QString&)> m_logHandler;
    std::function<void(bool)> m_busyHandler;
    std::function<void(bool)> m_parallelActiveHandler;
    std::function<void(const QString&)> m_activeReasoningRunHandler;
    std::function<void(const QVariantMap&)> m_reasoningProgressHandler;
    std::function<void(const QJsonObject&)> m_focusedConversationHandler;
    std::function<void(const QJsonArray&)> m_selectionHandler;
    std::function<void(const QString&, const QJsonObject&, const QJsonObject&)> m_finalRouteHandler;
};
