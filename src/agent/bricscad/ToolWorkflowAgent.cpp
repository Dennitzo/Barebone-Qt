#include "ToolWorkflowAgent.h"

#include "BricsCadAgentUtils.h"
#include "../BrxAgent.h"

#include <QJsonDocument>
#include <QSet>

namespace {

QJsonObject workflowFromIndex(const QJsonArray& workflowIndex, const QString& id)
{
    const QString slug = BricsCadAgentUtils::workflowSlug(id);
    for (const QJsonValue& value : workflowIndex) {
        const QJsonObject workflow = value.toObject();
        if (BricsCadAgentUtils::workflowSlug(workflow.value(QStringLiteral("id")).toString()) == slug) {
            return workflow;
        }
    }
    return {};
}

QJsonArray workflowObjectsForIds(
    const QStringList& ids,
    const QJsonObject& selectedWorkflow,
    const ToolWorkflowAgent::WorkflowLoader& loader)
{
    QJsonArray workflows;
    QSet<QString> seen;
    const QString selectedSlug = BricsCadAgentUtils::workflowSlug(
        selectedWorkflow.value(QStringLiteral("id")).toString());
    for (const QString& id : ids) {
        const QString slug = BricsCadAgentUtils::workflowSlug(id);
        if (slug.isEmpty() || seen.contains(slug)) {
            continue;
        }
        QJsonObject workflow;
        if (!selectedWorkflow.isEmpty() && selectedSlug == slug) {
            workflow = selectedWorkflow;
        } else if (loader) {
            workflow = loader(id);
        }
        if (!workflow.isEmpty()) {
            workflows.append(workflow);
            seen.insert(slug);
        }
    }
    return workflows;
}

QString workflowRankingText(const QJsonArray& workflows)
{
    QString text;
    for (const QJsonValue& value : workflows) {
        const QJsonObject workflow = value.toObject();
        text.append(QStringLiteral("\n\n[BricsCAD-Workflow]\n"));
        text.append(workflow.value(QStringLiteral("title")).toString().left(180));
        text.append(QLatin1Char('\n'));
        text.append(workflow.value(QStringLiteral("description")).toString().left(1200));
        const QJsonArray steps = BricsCadAgentUtils::workflowStepSummary(workflow, 12);
        if (!steps.isEmpty()) {
            text.append(QStringLiteral("\nSchritte: "));
            text.append(QString::fromUtf8(QJsonDocument(steps).toJson(QJsonDocument::Compact)).left(1800));
        }
    }
    return text;
}

} // namespace

QJsonArray ToolWorkflowAgent::effectiveTools(
    const QJsonArray& catalog,
    const QJsonObject& route,
    const QString& prompt,
    const QJsonArray& selectedWorkflowObjects,
    const QString& pendingSourcePrompt)
{
    const QJsonArray preselectedTools = route.value(QStringLiteral("effectiveTools")).toArray();
    if (!preselectedTools.isEmpty()) {
        return preselectedTools;
    }

    const int cap = route.value(QStringLiteral("route")).toString() == QStringLiteral("validation_retry") ? 8 : 6;
    QSet<QString> workflowToolNames;
    if (route.value(QStringLiteral("route")).toString() != QStringLiteral("validation_retry")) {
        for (const QJsonValue& workflowValue : selectedWorkflowObjects) {
            for (const QString& name : BricsCadAgentUtils::workflowToolNames(workflowValue.toObject(), 24)) {
                workflowToolNames.insert(name);
            }
        }
    }

    if (!workflowToolNames.isEmpty()) {
        QJsonArray exactWorkflowTools;
        for (const QJsonValue& toolValue : catalog) {
            const QJsonObject tool = toolValue.toObject();
            const QString name = tool.value(QStringLiteral("name")).toString();
            if (workflowToolNames.contains(name)) {
                exactWorkflowTools.append(tool);
            }
            if (exactWorkflowTools.size() >= cap) {
                break;
            }
        }
        if (!exactWorkflowTools.isEmpty()) {
            return exactWorkflowTools;
        }
    }

    QString rankingPrompt = prompt;
    if (!pendingSourcePrompt.trimmed().isEmpty() && !rankingPrompt.contains(pendingSourcePrompt)) {
        rankingPrompt = QStringLiteral("%1\n%2").arg(pendingSourcePrompt, rankingPrompt);
    }
    rankingPrompt.append(workflowRankingText(selectedWorkflowObjects));
    const QJsonObject focusedContext = route.value(QStringLiteral("focusedConversationContext")).toObject();
    const QString focusedSummary = focusedContext.value(QStringLiteral("relevantSummary")).toString().trimmed();
    if (!focusedSummary.isEmpty()) {
        rankingPrompt.append(QStringLiteral("\n\n[Komprimierter relevanter Nachrichtenverlauf]\n"));
        rankingPrompt.append(focusedSummary.left(2200));
    }

    return BrxAgent::selectEffectiveTools(catalog, route, rankingPrompt, cap);
}

void ToolWorkflowAgent::run(
    const Request& request,
    LocalAiAgentRuntime& runtime,
    WorkflowLoader workflowLoader,
    Callback callback) const
{
    QJsonObject input{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.tool-workflow-selection.request.v1")},
        {QStringLiteral("userPrompt"), request.prompt},
        {QStringLiteral("route"), request.route.value(QStringLiteral("route")).toString()},
        {QStringLiteral("availableTools"), BrxAgent::compactToolIndex(request.catalog)},
        {QStringLiteral("availableWorkflows"), request.workflowIndex},
        {QStringLiteral("manuallySelectedWorkflowId"), request.manualWorkflowId},
        {QStringLiteral("limits"), QJsonObject{{QStringLiteral("tools"), 6}, {QStringLiteral("workflows"), 3}}},
        {QStringLiteral("outputShape"), QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.tool-workflow-selection.v1")},
            {QStringLiteral("toolNames"), QJsonArray{}},
            {QStringLiteral("workflowIds"), QJsonArray{}},
            {QStringLiteral("confidence"), 0.0},
            {QStringLiteral("summary"), QString()},
        }},
    };

    auto finalize = [
        request,
        callback = std::move(callback),
        workflowLoader = std::move(workflowLoader)
    ](QJsonObject selection, bool aiSelectionReady) mutable {
        QStringList workflowIds;
        if (!request.manualWorkflowId.trimmed().isEmpty()) {
            workflowIds << request.manualWorkflowId.trimmed();
        }

        for (const QString& id : BricsCadAgentUtils::stringsFromJsonArray(selection.value(QStringLiteral("workflowIds")).toArray())) {
            if (workflowIds.size() >= 3) {
                break;
            }
            if (workflowFromIndex(request.workflowIndex, id).isEmpty()) {
                continue;
            }
            if (!workflowIds.contains(id)) {
                workflowIds << id;
            }
        }
        if (workflowIds.isEmpty()) {
            workflowIds = BricsCadAgentUtils::localWorkflowSelection(request.workflowIndex, request.prompt, 3);
        }
        if (workflowIds.isEmpty()) {
            workflowIds = BricsCadAgentUtils::routeWorkflowIds(request.route, 3);
        }

        const QJsonArray workflows = workflowObjectsForIds(
            workflowIds,
            request.selectedWorkflow,
            workflowLoader);
        QJsonObject scopedRoute = request.route;
        if (!workflowIds.isEmpty()) {
            scopedRoute.insert(QStringLiteral("selectedWorkflows"),
                QJsonArray::fromStringList(workflowIds));
        }

        QJsonArray effective = BrxAgent::toolsByNames(
            request.catalog,
            BricsCadAgentUtils::stringsFromJsonArray(selection.value(QStringLiteral("toolNames")).toArray()));
        QSet<QString> selectedNames;
        for (const QJsonValue& value : effective) {
            selectedNames.insert(value.toObject().value(QStringLiteral("name")).toString());
        }
        const QJsonArray workflowEffective = effectiveTools(request.catalog, scopedRoute, request.prompt, workflows);
        for (const QJsonValue& value : workflowEffective) {
            const QString name = value.toObject().value(QStringLiteral("name")).toString();
            if (!name.isEmpty() && !selectedNames.contains(name) && effective.size() < 6) {
                effective.append(value);
                selectedNames.insert(name);
            }
        }
        if (effective.isEmpty()) {
            effective = workflowEffective;
        }
        while (effective.size() > 6) {
            effective.removeAt(effective.size() - 1);
        }

        selection.insert(QStringLiteral("schema"), QStringLiteral("barebone.agent.tool-workflow-selection.v1"));
        selection.insert(QStringLiteral("toolNames"),
            QJsonArray::fromStringList(BricsCadAgentUtils::toolNamesForLog(effective, 6)));
        selection.insert(QStringLiteral("workflowIds"), QJsonArray::fromStringList(workflowIds));
        selection.insert(QStringLiteral("effectiveTools"), effective);
        selection.insert(QStringLiteral("selectionSource"),
            aiSelectionReady ? QStringLiteral("ai") : QStringLiteral("deterministic-catalog-logic"));
        selection.insert(QStringLiteral("ready"), !effective.isEmpty() || !workflowIds.isEmpty());
        callback(selection);
    };

    LocalAiAgentRuntime::JsonRequest jsonRequest;
    jsonRequest.slot = QStringLiteral("tools-workflows");
    jsonRequest.kind = QStringLiteral("BricsCadPreparation-tools-workflows");
    jsonRequest.systemInstruction = QStringLiteral(
        "Du bist der Tool-und-Workflow-Agent fuer BricsCAD. "
        "Waehle nur Namen aus den gelieferten Listen. Eine manuelle Workflow-Auswahl hat Vorrang. "
        "Hoestens sechs Tools und drei Workflows. Keine CAD-Aktion, keine Erklaerung ausser summary.");
    jsonRequest.input = input;
    jsonRequest.requiredFields = {
        QStringLiteral("schema"),
        QStringLiteral("toolNames"),
        QStringLiteral("workflowIds"),
        QStringLiteral("confidence"),
        QStringLiteral("summary"),
    };
    jsonRequest.maxOutputTokens = 768;
    jsonRequest.priority = 90;
    jsonRequest.cancellable = true;
    jsonRequest.dedupeKey = QStringLiteral("bricscad-preparation:%1:%2:tools-workflows")
        .arg(request.sessionId)
        .arg(request.operationGeneration);
    jsonRequest.operationGeneration = request.operationGeneration;
    jsonRequest.bricsCadMode = request.bricsCadMode;

    runtime.submitJson(jsonRequest, [finalize = std::move(finalize)](const LocalAiAgentRuntime::JsonResult& result) mutable {
        QJsonObject selection = result.ok ? result.object : QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.tool-workflow-selection.v1")},
            {QStringLiteral("toolNames"), QJsonArray{}},
            {QStringLiteral("workflowIds"), QJsonArray{}},
            {QStringLiteral("confidence"), 0.0},
            {QStringLiteral("summary"), result.error},
        };
        finalize(selection, result.ok);
    });
}
