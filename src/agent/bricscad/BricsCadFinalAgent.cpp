#include "BricsCadFinalAgent.h"

#include "BricsCadAgentUtils.h"

namespace {

QJsonArray limitedArray(const QJsonArray& source, int limit)
{
    QJsonArray out;
    for (int i = 0; i < source.size() && i < limit; ++i) {
        out.append(source.at(i));
    }
    return out;
}

QJsonObject minimalResponseContract()
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.response.v2.minimal")},
        {QStringLiteral("requiredTopLevel"), QJsonArray{
            QStringLiteral("schema"),
            QStringLiteral("type"),
        }},
        {QStringLiteral("allowedTypes"), QJsonArray{
            QStringLiteral("ask_user"),
            QStringLiteral("context_request"),
            QStringLiteral("action_proposal"),
        }},
        {QStringLiteral("actionProposal"), QJsonObject{
            {QStringLiteral("required"), QJsonArray{
                QStringLiteral("requiresConfirmation"),
                QStringLiteral("actions"),
            }},
            {QStringLiteral("toolSource"), QStringLiteral("effectiveTools[].name")},
            {QStringLiteral("paramsSource"), QStringLiteral("effectiveTools[].inputSchema")},
            {QStringLiteral("batch"), QStringLiteral("Use proposal.actions[] for known multi-step CAD work.")},
        }},
    };
}

QJsonObject compactWorkflowCapsule(const QJsonObject& workflow)
{
    if (workflow.isEmpty()) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("id"), workflow.value(QStringLiteral("id")).toString()},
        {QStringLiteral("title"), workflow.value(QStringLiteral("title")).toString()},
        {QStringLiteral("description"), workflow.value(QStringLiteral("description")).toString().left(360)},
        {QStringLiteral("preferredTools"), BricsCadAgentUtils::stringsToJsonArray(
            BricsCadAgentUtils::workflowToolNames(workflow, 6))},
        {QStringLiteral("knownSlotValues"), workflow.value(QStringLiteral("knownSlotValues")).toObject()},
        {QStringLiteral("stepSummary"), BricsCadAgentUtils::workflowStepSummary(workflow, 8)},
    };
}

QJsonObject compactDrawingContext(const QJsonObject& preparedContext, const QJsonObject& currentContext)
{
    const QJsonObject source = preparedContext.isEmpty() ? currentContext : preparedContext;
    QJsonObject compact{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.drawing-focus.compact.v1")},
        {QStringLiteral("summary"), source.value(QStringLiteral("summary")).toString(
            QStringLiteral("Zeichnungskontext nicht verfuegbar.")).left(700)},
        {QStringLiteral("ready"), source.value(QStringLiteral("ready")).toBool(false)},
    };
    const QJsonArray objects = source.value(QStringLiteral("relevantObjects")).toArray();
    if (!objects.isEmpty()) {
        compact.insert(QStringLiteral("relevantObjects"), limitedArray(objects, 12));
    }
    const QJsonArray layers = source.value(QStringLiteral("relevantLayers")).toArray();
    if (!layers.isEmpty()) {
        compact.insert(QStringLiteral("relevantLayers"), limitedArray(layers, 12));
    }
    const QJsonArray measurements = source.value(QStringLiteral("measurements")).toArray();
    if (!measurements.isEmpty()) {
        compact.insert(QStringLiteral("measurements"), limitedArray(measurements, 12));
    }
    const QJsonArray uncertainties = source.value(QStringLiteral("uncertainties")).toArray();
    if (!uncertainties.isEmpty()) {
        compact.insert(QStringLiteral("uncertainties"), limitedArray(uncertainties, 6));
    }
    compact.insert(QStringLiteral("contextSource"),
        preparedContext.isEmpty() ? QStringLiteral("current compact context") : QStringLiteral("parallel preparedDrawingContext"));
    return compact;
}

QJsonObject compactFocusedConversation(const QJsonObject& focusedConversation)
{
    if (focusedConversation.isEmpty()) {
        return {};
    }
    QJsonObject compact{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.focused-conversation-context.compact.v1")},
        {QStringLiteral("topic"), focusedConversation.value(QStringLiteral("topic")).toString().left(120)},
        {QStringLiteral("relevantSummary"), focusedConversation.value(QStringLiteral("relevantSummary")).toString().left(1500)},
    };
    const QJsonArray indexes = focusedConversation.value(QStringLiteral("relevantMessageIndexes")).toArray();
    compact.insert(QStringLiteral("relevantMessageCount"), indexes.size());
    return compact;
}

QJsonObject deterministicBatchDraftFromCalculation(const QJsonObject& calculation)
{
    if (!calculation.value(QStringLiteral("readyForExecution")).toBool(false)
        || calculation.value(QStringLiteral("contour")).toObject().isEmpty()
        || calculation.value(QStringLiteral("wallBoxes")).toArray().isEmpty()) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.deterministic-batch-draft.v1")},
        {QStringLiteral("source"), QStringLiteral("calculationResult")},
        {QStringLiteral("intendedSequence"), QJsonArray{
            QStringLiteral("geometry.create rectangle"),
            QStringLiteral("geometry.query lastResult rectangle"),
            QStringLiteral("measurement.area autoRoomHandlesFromLastQuery"),
            QStringLiteral("measurement.bbox autoRoomHandlesFromLastQuery"),
            QStringLiteral("geometry.create four exterior wall boxes"),
            QStringLiteral("geometry.query createdGeometryBatch handles"),
        }},
        {QStringLiteral("contour"), calculation.value(QStringLiteral("contour")).toObject()},
        {QStringLiteral("wallBoxes"), limitedArray(calculation.value(QStringLiteral("wallBoxes")).toArray(), 4)},
        {QStringLiteral("verification"), QStringLiteral("Qt stops the batch before wall boxes if area or bbox does not match calculationResult.")},
    };
}

} // namespace

QJsonObject BricsCadFinalAgent::buildEnvelope(const BuildInput& input)
{
    QJsonArray workflowCapsules;
    for (int i = 0; i < input.selectedWorkflows.size() && i < 2; ++i) {
        const QJsonObject capsule = compactWorkflowCapsule(input.selectedWorkflows.at(i).toObject());
        if (!capsule.isEmpty()) {
            workflowCapsules.append(capsule);
        }
    }

    QJsonObject compactRoute = input.route;
    compactRoute.remove(QStringLiteral("effectiveTools"));
    compactRoute.remove(QStringLiteral("effectiveToolNames"));
    compactRoute.remove(QStringLiteral("focusedConversationContext"));
    compactRoute.remove(QStringLiteral("calculationResult"));
    compactRoute.remove(QStringLiteral("preparedDrawingContext"));
    compactRoute.remove(QStringLiteral("parallelPreparation"));

    QJsonObject focusedConversation = input.route.value(QStringLiteral("focusedConversationContext")).toObject();
    const QJsonObject calculation = input.route.value(QStringLiteral("calculationResult")).toObject();
    const QJsonObject preparedDrawingContext = input.route.value(QStringLiteral("preparedDrawingContext")).toObject();

    QJsonObject envelope{
        {QStringLiteral("schema"), QStringLiteral("barebone.agent.bricscad.request.v2")},
        {QStringLiteral("profile"), QStringLiteral("bricscad-final-minimal-v1")},
        {QStringLiteral("userPrompt"), input.prompt},
        {QStringLiteral("route"), compactRoute},
        {QStringLiteral("drawingContext"), compactDrawingContext(preparedDrawingContext, input.drawingContext)},
        {QStringLiteral("effectiveTools"), input.effectiveTools},
        {QStringLiteral("workflowCapsules"), workflowCapsules},
        {QStringLiteral("pipeline"), QJsonObject{
            {QStringLiteral("order"), QJsonArray{
                QStringLiteral("parallel:compressedConversationHistory"),
                QStringLiteral("parallel:prefetchedDrawingContext"),
                QStringLiteral("parallel:effectiveToolsAndWorkflows"),
                QStringLiteral("parallel:calculation"),
                QStringLiteral("fanIn:finalReasoningAndExecution"),
            }},
            {QStringLiteral("conversationHistoryIncluded"), !focusedConversation.isEmpty()},
            {QStringLiteral("effectiveToolsLocked"), input.route.value(QStringLiteral("effectiveToolsSelected")).toBool(false)},
            {QStringLiteral("drawingContextPrefetched"), input.route.value(QStringLiteral("drawingContextPrefetchAttempted")).toBool(false)},
            {QStringLiteral("calculationAttempted"), input.route.value(QStringLiteral("calculationAttempted")).toBool(false)},
            {QStringLiteral("policy"), QStringLiteral(
                "Finaler Minimal-Run: pruefe Nutzerabsicht, kompakte Zeichnungslage, Berechnung und kleine Toolauswahl. "
                "Beruecksichtige erkennbare Geometriekonflikte/Ueberlappungen. Keine nativen Commands, keine langen Workflows, keine lokalen Tool-Fallbacks.")},
        }},
        {QStringLiteral("reasoning"), QJsonObject{{QStringLiteral("effort"), QStringLiteral("low")}}},
        {QStringLiteral("responseContract"), minimalResponseContract()},
        {QStringLiteral("execution"), QJsonObject{
            {QStringLiteral("confirmationRequiredForMutations"), true},
            {QStringLiteral("validateAgainstCapabilities"), true},
            {QStringLiteral("delegatedValueChoice"), input.delegatedValueChoice},
            {QStringLiteral("delegatedValuePolicy"), QStringLiteral(
                "Wenn delegatedValueChoice=true, plausible sichere Default-/Beispielwerte selbst einsetzen und nicht danach fragen. "
                "Eine ausfuehrbare CAD-Anweisung muss als action_proposal enden; blosse Beschreibung oder Ausfuehrungsbehauptung ist ungueltig.")},
            {QStringLiteral("workflowExecutionPolicy"), workflowCapsules.isEmpty()
                ? QStringLiteral("Kein Workflow-Kontext aktiv.")
                : QStringLiteral(
                    "Der erste Workflow in workflowCapsules ist die ausfuehrbare Vorlage. "
                    "Explizite Nutzerwerte ersetzen passende Workflowwerte; abhaengige Koordinaten und Abmessungen neu berechnen. "
                    "Fehlen Nutzerwerte, konkrete paramsTemplate- und knownSlotValues-Beispielwerte verwenden und nicht danach fragen. "
                    "Bei ausfuehrbarer Workflow-Anfrage bekannte Schrittfolgen direkt als proposal.actions[] Batch liefern; niemals mit message beenden.")},
        }},
    };

    const QJsonObject deterministicBatchDraft = deterministicBatchDraftFromCalculation(calculation);
    if (!deterministicBatchDraft.isEmpty()) {
        envelope.insert(QStringLiteral("deterministicBatchDraft"), deterministicBatchDraft);
    }

    if (!workflowCapsules.isEmpty()
        && workflowCapsules.first().toObject().value(QStringLiteral("id")).toString()
            == QStringLiteral("grundriss_und_aussenwaende_einzeichnen")) {
        QJsonObject execution = envelope.value(QStringLiteral("execution")).toObject();
        execution.insert(QStringLiteral("workflowExecutionPolicy"), QStringLiteral(
            "Fuehre den Workflow als direkten Batch aus: "
            "proposal.actions enthaelt genau diese Reihenfolge: "
            "(1) geometry.create rectangle fuer die Innenkontur, "
            "(2) geometry.query fuer selector.scope=lastResult, "
            "(3) measurement.area mit autoRoomHandlesFromLastQuery, "
            "(4) measurement.bbox mit autoRoomHandlesFromLastQuery, "
            "(5-8) vier geometry.create box Aktionen fuer die Aussenwaende, "
            "(9) geometry.query mit autoCreatedGeometryHandlesFromBatch fuer alle erzeugten Handles. "
            "Setze proposal.continueAfterSuccess=false. Qt stoppt den Batch nach der Pruefung deterministisch, falls Flaeche oder Innenmasse abweichen. "
            "Nutze calculationResult.contour und calculationResult.wallBoxes; explizite Nutzerwerte sind dort bereits eingerechnet. "
            "Beende nicht nach der Kontur oder Pruefung, solange die vier Waende und die finale Geometriepruefung im Batch fehlen."));
        envelope.insert(QStringLiteral("execution"), execution);
    }

    if (input.route.contains(QStringLiteral("calculationResult"))) {
        envelope.insert(QStringLiteral("calculationResult"), calculation);
    }
    if (input.route.contains(QStringLiteral("parallelPreparation"))) {
        envelope.insert(QStringLiteral("parallelPreparation"), QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("barebone.agent.parallel-preparation.compact.v1")},
            {QStringLiteral("completedSlots"), input.route.value(QStringLiteral("parallelPreparation")).toObject()
                .value(QStringLiteral("completedSlots")).toArray()},
        });
    }
    if (!focusedConversation.isEmpty()) {
        envelope.insert(QStringLiteral("focusedConversationContext"), compactFocusedConversation(focusedConversation));
    }

    return envelope;
}
