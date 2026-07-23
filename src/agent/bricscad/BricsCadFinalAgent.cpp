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
            QStringLiteral("message"),
            QStringLiteral("ask_user"),
            QStringLiteral("context_request"),
            QStringLiteral("action_proposal"),
            QStringLiteral("plan"),
        }},
        {QStringLiteral("message"), QJsonObject{
            {QStringLiteral("required"), QJsonArray{QStringLiteral("message")}},
            {QStringLiteral("messagePolicy"), QStringLiteral(
                "For type=message, message must contain the complete visible answer. "
                "Never return an empty message when drawingContext already contains the requested fact.")},
        }},
        {QStringLiteral("actionProposal"), QJsonObject{
            {QStringLiteral("required"), QJsonArray{
                QStringLiteral("requiresConfirmation"),
                QStringLiteral("actions"),
                QStringLiteral("summary"),
            }},
            {QStringLiteral("toolSource"), QStringLiteral("effectiveTools[].name")},
            {QStringLiteral("paramsSource"), QStringLiteral("effectiveTools[].inputSchema")},
            {QStringLiteral("batch"), QStringLiteral("Use proposal.actions[] for known multi-step CAD work.")},
            {QStringLiteral("planningMetadata"), QJsonObject{
                {QStringLiteral("required"), QJsonArray{
                    QStringLiteral("proposalId"),
                    QStringLiteral("intentSummary"),
                    QStringLiteral("contextEvidence"),
                    QStringLiteral("workflowUsage"),
                    QStringLiteral("assumptions"),
                }},
                {QStringLiteral("policy"), QStringLiteral("Provide concise decision evidence, never hidden chain-of-thought.")},
            }},
            {QStringLiteral("userFacingDescription"), QStringLiteral(
                "Top-level message and proposal.summary must describe the planned execution in concrete user-facing German text. "
                "Mention target objects/names, tool intent and important parameters. "
                "Use proposal.details for the step list when more than one action is planned. "
                "Never use generic text such as 'Der Agent hat eine BricsCAD-Aktion vorbereitet'.")},
        }},
        {QStringLiteral("askUser"), QJsonObject{
            {QStringLiteral("required"), QJsonArray{
                QStringLiteral("message"),
                QStringLiteral("missing"),
            }},
            {QStringLiteral("messagePolicy"), QStringLiteral(
                "Write a short, natural German question that names every missing datum and explains the expected format. "
                "The message is shown directly in the chat card, so never omit it and never use generic waiting text.")},
        }},
        {QStringLiteral("contextRequest"), QJsonObject{
            {QStringLiteral("required"), QJsonArray{
                QStringLiteral("method"),
                QStringLiteral("params"),
            }},
            {QStringLiteral("methodSource"), QStringLiteral("readOnlyMethods[].name")},
            {QStringLiteral("example"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("context_request")},
                {QStringLiteral("method"), QStringLiteral("selection.describe")},
                {QStringLiteral("params"), QJsonObject{
                    {QStringLiteral("include"), QJsonArray{QStringLiteral("geometry"), QStringLiteral("metrics")}},
                    {QStringLiteral("limit"), 100},
                }},
            }},
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
    const QJsonObject selection = source.value(QStringLiteral("selection")).toObject();
    if (!selection.isEmpty()) {
        QJsonObject compactSelection{
            {QStringLiteral("queried"), selection.value(QStringLiteral("queried"))},
            {QStringLiteral("available"), selection.value(QStringLiteral("available"))},
            {QStringLiteral("count"), selection.value(QStringLiteral("count"))},
            {QStringLiteral("objects"), limitedArray(selection.value(QStringLiteral("objects")).toArray(), 20)},
        };
        if (selection.contains(QStringLiteral("error"))) {
            compactSelection.insert(QStringLiteral("error"), selection.value(QStringLiteral("error")));
        }
        compact.insert(QStringLiteral("selection"), compactSelection);
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
    if (focusedConversation.value(QStringLiteral("conversation")).isArray()) {
        compact.insert(QStringLiteral("conversation"), focusedConversation.value(QStringLiteral("conversation")));
    }
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
    QJsonArray workflowHintCapsules;
    for (int i = 0; i < input.workflowHints.size() && i < 3; ++i) {
        const QJsonObject capsule = compactWorkflowCapsule(input.workflowHints.at(i).toObject());
        if (!capsule.isEmpty()) {
            workflowHintCapsules.append(capsule);
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
        {QStringLiteral("originalUserPrompt"), input.prompt},
        {QStringLiteral("promptPriority"), QStringLiteral("highest")},
        {QStringLiteral("route"), compactRoute},
        {QStringLiteral("drawingContext"), compactDrawingContext(preparedDrawingContext, input.drawingContext)},
        {QStringLiteral("effectiveTools"), input.effectiveTools},
        {QStringLiteral("workflowCapsules"), workflowCapsules},
        {QStringLiteral("activeWorkflow"), workflowCapsules.isEmpty()
            ? QJsonObject{}
            : workflowCapsules.first().toObject()},
        {QStringLiteral("workflowHints"), workflowHintCapsules},
        {QStringLiteral("workflowHintPolicy"), QStringLiteral(
            "workflowHints sind unverbindliche Strategiebausteine. Pruefe aktiv, welche Teile zum aktuellen Prompt und Verlauf passen. "
            "Du darfst passende Teile verwenden, anpassen oder verwerfen; kopiere keinen Workflow blind.")},
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
                "Beruecksichtige erkennbare Geometriekonflikte/Ueberlappungen. Keine nativen Commands und keine lokalen Tool-Fallbacks. "
                "Der aktuelle userPrompt/originalUserPrompt ist die primaere Aufgabe. Workflow- und Toolkontext darf den Prompt nicht ersetzen. "
                "Bei einer reinen Zeichnungsfrage, die drawingContext bereits beantwortet, antworte mit type=message und einer nicht leeren sichtbaren Antwort. "
                "Waehle und kombiniere effectiveTools selbst. Pruefe vor Ausgabe, ob jede Aktion fuer Prompt und relevanten Verlauf notwendig ist. "
                "Wenn der Nutzer Auswahl, Selektion, selektiert oder scope=selection sagt, ist dies bereits ein gueltiger Selector und keine Rueckfrage wert. "
                "Bei 'Liste alle BIM Objekte als Tabelle auf' nutze direkt das read-only Tool bim.objects.query mit selector.scope=currentSpace, include core/geometry und positivem limit. Fordere dafuer keinen zusaetzlichen Kontext-Request an. "
                "qt.brx.context.fetch ist kein gueltiges readOnly-Verfahren und darf niemals als context_request verwendet werden. "
                "Nutze drawingContext.selection als harte BRX-Tatsache. Bei verfuegbaren selection.objects bevorzuge deren stabile Handles im Action-Selector; "
                "wenn die Auswahl noch nicht abgefragt wurde, fordere method=selection.describe als context_request an. "
                "Frage niemals, welches BRX-Tool oder welche Extrusionsmethode verwendet werden soll. Ermittle dies selbst aus effectiveTools, BRX-Capabilities, SDK-Beschreibungen und Workflows. "
                "Bei action_proposal immer eine konkrete sichtbare Ausfuehrungsbeschreibung in message und proposal.summary mitschicken; "
                "proposal.details enthaelt bei Batches die geplante Schrittfolge. Liefere ausserdem proposalId, intentSummary, "
                "contextEvidence, workflowUsage und assumptions als knappe pruefbare Entscheidungsdaten.")},
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
                ? QStringLiteral("Kein verbindlicher Workflow. Nutze Workflow-Hints nur, soweit sie Prompt und Verlauf nach eigener Pruefung sinnvoll unterstuetzen.")
                : QStringLiteral(
                    "Der erste Workflow in workflowCapsules ist manuell ausgewaehlt und verbindlich. Erhalte seine fachlichen Schritte und Reihenfolge. "
                    "Explizite Nutzerwerte duerfen passende Workflowwerte ersetzen und abhaengige Werte muessen neu berechnet werden. "
                    "Wenn aktueller Prompt, Verlauf oder Zeichnungslage dem Workflow widersprechen, liefere ask_user statt Schritte still zu entfernen, "
                    "eine andere Aufgabe auszufuehren oder den Workflow blind zu kopieren.")},
        }},
    };

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
