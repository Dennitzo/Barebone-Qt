#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class BricsCadFinalAgent final {
public:
    struct BuildInput {
        QString prompt;
        QJsonObject route;
        QJsonObject drawingContext;
        QJsonArray effectiveTools;
        QJsonArray selectedWorkflows;
        QJsonArray workflowHints;
        QJsonObject pendingProposal;
        QJsonObject pendingDraft;
        QJsonObject lastToolResult;
        QString reasoningEffort;
        bool delegatedValueChoice = false;
    };

    static QJsonObject buildEnvelope(const BuildInput& input);
};
