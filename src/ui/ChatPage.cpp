#include "ChatPage.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPalette>
#include <QPointer>
#include <QSharedPointer>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineSettings>

#include <algorithm>
#include <utility>

namespace {

constexpr int kChatRequestTimeoutMs = 600000;

template <typename Callback>
void emitToWebAsync(AiWebBridge* bridge, Callback callback)
{
    if (!bridge) {
        return;
    }
    QPointer<AiWebBridge> guard(bridge);
    QTimer::singleShot(0, bridge, [guard, callback = std::move(callback)]() mutable {
        if (guard) {
            callback(guard);
        }
    });
}

QString nowTime()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
}

QString logTime()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
}

QString effectiveWebTheme(const QString& theme)
{
    const QString normalized = theme.trimmed().toLower();
    if (normalized == QStringLiteral("dark") || normalized == QStringLiteral("light")) {
        return normalized;
    }
    return QApplication::palette().color(QPalette::Window).lightness() < 128
        ? QStringLiteral("dark")
        : QStringLiteral("light");
}

QString effectiveUiLanguage(const QString& language)
{
    return language.trimmed().compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("en")
        : QStringLiteral("de");
}

QUrl chatCompletionsUrl(QString baseUrl)
{
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith(QLatin1Char('/'))) {
        baseUrl.chop(1);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"))) {
        return QUrl(baseUrl);
    }
    return QUrl(baseUrl + QStringLiteral("/chat/completions"));
}

QUrl modelsUrl(QString baseUrl)
{
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith(QLatin1Char('/'))) {
        baseUrl.chop(1);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"))) {
        baseUrl.chop(QStringLiteral("/chat/completions").size());
    }
    return QUrl(baseUrl + QStringLiteral("/models"));
}

QJsonObject chatMessageFromResponse(const QJsonObject& response)
{
    const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return {};
    }
    const QJsonObject first = choices.first().toObject();
    return first.value(QStringLiteral("message")).toObject();
}

QString chatContentFromResponse(const QJsonObject& response)
{
    const QJsonObject message = chatMessageFromResponse(response);
    if (message.isEmpty()) {
        return {};
    }
    QString content = message.value(QStringLiteral("content")).toString().trimmed();
    if (content.isEmpty()) {
        const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            content = choices.first().toObject().value(QStringLiteral("text")).toString().trimmed();
        }
    }
    return content;
}

QString compactJsonValue(const QJsonValue& value)
{
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return value.toVariant().toString();
}

QString toolCallName(const QJsonObject& toolCall)
{
    return toolCall.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString().trimmed();
}

QString toolCallArgumentsText(const QJsonObject& toolCall)
{
    const QJsonValue arguments = toolCall.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments"));
    if (arguments.isString()) {
        return arguments.toString().trimmed();
    }
    return compactJsonValue(arguments);
}

QString revitToolDescription(const QString& name)
{
    if (name == QStringLiteral("revit_status")) {
        return QStringLiteral("Revit-Version, aktives Dokument und aktive Ansicht lesen");
    }
    if (name == QStringLiteral("revit_document_summary")) {
        return QStringLiteral("Dokumentzusammenfassung und Elementzaehler lesen");
    }
    if (name == QStringLiteral("revit_selection_describe")) {
        return QStringLiteral("Aktuelle Auswahl in Revit lesen");
    }
    if (name == QStringLiteral("revit_levels_list")) {
        return QStringLiteral("Ebenen des aktiven Dokuments lesen");
    }
    if (name == QStringLiteral("revit_views_list")) {
        return QStringLiteral("Ansichten des aktiven Dokuments lesen");
    }
    if (name == QStringLiteral("revit_categories_summary")) {
        return QStringLiteral("Elemente nach Kategorie zaehlen");
    }
    if (name == QStringLiteral("revit_elements_list")) {
        return QStringLiteral("Revit-Elemente mit ID, Kategorie, Klasse, Name, Familie, Typ, Ebene und Ansicht lesen");
    }
    return QStringLiteral("Revit-Read-only-Tool ausfuehren");
}

QJsonObject toolCallArgumentsObject(const QJsonObject& toolCall, QString* errorMessage)
{
    const QJsonValue arguments = toolCall.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments"));
    if (arguments.isUndefined() || arguments.isNull()) {
        return {};
    }
    if (arguments.isObject()) {
        return arguments.toObject();
    }
    if (!arguments.isString()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Tool-Argumente sind weder JSON-Objekt noch JSON-String.");
        }
        return {};
    }

    const QString text = arguments.toString().trimmed();
    if (text.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Tool-Argumente sind kein gueltiges JSON-Objekt: %1").arg(parseError.errorString());
        }
        return {};
    }
    return document.object();
}

QJsonObject toolResponseError(const QString& message)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("response")},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), message},
    };
}

QString markdownCell(const QJsonValue& value)
{
    QString text;
    if (value.isBool()) {
        text = value.toBool() ? QStringLiteral("ja") : QStringLiteral("nein");
    } else {
        text = value.toVariant().toString();
    }
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    text = text.simplified();
    if (text.size() > 90) {
        text = text.left(87).trimmed() + QStringLiteral("...");
    }
    return text.isEmpty() ? QStringLiteral("-") : text;
}

QString jsonString(const QJsonObject& object, const QString& key, const QString& fallback = {})
{
    const QString value = object.value(key).toVariant().toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

QString formatRevitElementsTable(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("ok")).toBool()) {
        return {};
    }

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const QJsonObject document = result.value(QStringLiteral("document")).toObject();
    const QJsonArray elements = result.value(QStringLiteral("elements")).toArray();
    if (elements.isEmpty()) {
        return QStringLiteral("Revit-Elemente: keine Elemente gefunden.");
    }

    const QString documentTitle = jsonString(document, QStringLiteral("title"), QStringLiteral("aktives Dokument"));
    const int totalCount = result.value(QStringLiteral("totalCount")).toInt();
    const int returnedCount = result.value(QStringLiteral("returnedCount")).toInt(elements.size());
    const bool truncated = result.value(QStringLiteral("truncated")).toBool();
    const QString scope = result.value(QStringLiteral("scope")).toString() == QStringLiteral("activeView")
        ? QStringLiteral("aktuelle Ansicht")
        : QStringLiteral("gesamtes Dokument");

    QStringList lines;
    lines << QStringLiteral("Revit-Elemente (%1) in `%2`: %3 gefunden, %4 ausgegeben%5.")
            .arg(scope)
            .arg(documentTitle)
            .arg(totalCount)
            .arg(returnedCount)
            .arg(truncated ? QStringLiteral(" (Ausgabe gekuerzt)") : QString());
    lines << QString();
    lines << QStringLiteral("| ID | Kategorie | Klasse | Name | Familie | Typ | Ebene | Ansicht |");
    lines << QStringLiteral("|---:|---|---|---|---|---|---|---|");
    for (const QJsonValue& value : elements) {
        const QJsonObject element = value.toObject();
        lines << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 |")
                .arg(markdownCell(element.value(QStringLiteral("id"))))
                .arg(markdownCell(element.value(QStringLiteral("category"))))
                .arg(markdownCell(element.value(QStringLiteral("className"))))
                .arg(markdownCell(element.value(QStringLiteral("name"))))
                .arg(markdownCell(element.value(QStringLiteral("familyName"))))
                .arg(markdownCell(element.value(QStringLiteral("typeName"))))
                .arg(markdownCell(element.value(QStringLiteral("levelName"))))
                .arg(markdownCell(element.value(QStringLiteral("ownerViewName"))));
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonObject compactElementForModel(const QJsonObject& element)
{
    return QJsonObject{
        {QStringLiteral("id"), element.value(QStringLiteral("id"))},
        {QStringLiteral("category"), element.value(QStringLiteral("category"))},
        {QStringLiteral("className"), element.value(QStringLiteral("className"))},
        {QStringLiteral("name"), element.value(QStringLiteral("name"))},
        {QStringLiteral("familyName"), element.value(QStringLiteral("familyName"))},
        {QStringLiteral("typeName"), element.value(QStringLiteral("typeName"))},
        {QStringLiteral("levelName"), element.value(QStringLiteral("levelName"))},
        {QStringLiteral("ownerViewName"), element.value(QStringLiteral("ownerViewName"))},
    };
}

QJsonArray elementPreviewForModel(const QJsonArray& elements)
{
    QJsonArray preview;
    const qsizetype count = std::min<qsizetype>(elements.size(), 80);
    for (qsizetype i = 0; i < count; ++i) {
        preview.append(compactElementForModel(elements.at(i).toObject()));
    }
    return preview;
}

QJsonObject modelSafeToolResponse(const QString& toolName, const QJsonObject& response)
{
    if (toolName == QStringLiteral("revit_elements_list") && response.value(QStringLiteral("ok")).toBool()) {
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonArray elements = result.value(QStringLiteral("elements")).toArray();
        const QJsonArray previewElements = elementPreviewForModel(elements);
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("ok"), true},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("document"), result.value(QStringLiteral("document"))},
                {QStringLiteral("scope"), result.value(QStringLiteral("scope"))},
                {QStringLiteral("includeElementTypes"), result.value(QStringLiteral("includeElementTypes"))},
                {QStringLiteral("totalCount"), result.value(QStringLiteral("totalCount"))},
                {QStringLiteral("returnedCount"), result.value(QStringLiteral("returnedCount"))},
                {QStringLiteral("truncated"), result.value(QStringLiteral("truncated"))},
                {QStringLiteral("displayedFullTableInChat"), true},
                {QStringLiteral("modelPreviewCount"), previewElements.size()},
                {QStringLiteral("previewElements"), previewElements},
                {QStringLiteral("instruction"), QStringLiteral("Die vollstaendige Tabelle wurde bereits direkt im Barebone-Qt Chat angezeigt. Fasse nur kurz zusammen und wiederhole nicht alle Zeilen.")},
            }},
        };
    }

    const QByteArray compact = QJsonDocument(response).toJson(QJsonDocument::Compact);
    constexpr qsizetype kMaxToolContentBytes = 60000;
    if (compact.size() <= kMaxToolContentBytes) {
        return response;
    }

    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("response")},
        {QStringLiteral("ok"), response.value(QStringLiteral("ok")).toBool()},
        {QStringLiteral("truncatedForModel"), true},
        {QStringLiteral("originalBytes"), compact.size()},
        {QStringLiteral("previewJson"), QString::fromUtf8(compact.left(kMaxToolContentBytes))},
    };
}

QStringList generalWorkflowResourcePaths()
{
    return {
        QStringLiteral(":/agent/general-workflows/bemessung_der_trinkwasserinstallation_nach_din_1988_300.json"),
        QStringLiteral(":/agent/general-workflows/heizlastberechnung_nach_din_en_12831.json"),
    };
}

QString workflowIdFromObject(const QJsonObject& workflow, const QString& resourcePath)
{
    QString id = workflow.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty()) {
        id = workflow.value(QStringLiteral("workflowId")).toString().trimmed();
    }
    if (id.isEmpty()) {
        id = QFileInfo(resourcePath).baseName();
    }
    return id;
}

QString compactWorkflowSummary(const QVariantMap& workflow)
{
    QStringList parts;
    const QString title = workflow.value(QStringLiteral("title")).toString().trimmed();
    const QString description = workflow.value(QStringLiteral("description")).toString().trimmed();
    const QString contextSummary = workflow.value(QStringLiteral("contextSummary")).toString().trimmed();
    if (!title.isEmpty()) {
        parts << QStringLiteral("Titel: %1").arg(title);
    }
    if (!description.isEmpty()) {
        parts << QStringLiteral("Beschreibung: %1").arg(description.left(1200));
    } else if (!contextSummary.isEmpty()) {
        parts << QStringLiteral("Zusammenfassung: %1").arg(contextSummary.left(1200));
    }
    return parts.join(QStringLiteral("\n"));
}

ChatPage::Workspace workspaceFromName(const QString& value)
{
    return value.trimmed().compare(QStringLiteral("revit"), Qt::CaseInsensitive) == 0
        ? ChatPage::Workspace::Revit
        : ChatPage::Workspace::Chat;
}

QString revitActionInstruction()
{
    return QStringLiteral(
        "\nRevit-Modus ist aktiv. Nutze den bereitgestellten Revit-Kontext fuer Antworten."
        "\nFuer Read-only-Revit-Daten nutze die bereitgestellten Tools statt zu raten. Bei Anfragen wie 'Liste alle Objekte als Tabelle auf' verwende revit_elements_list mit scope=document, includeElementTypes=false und limit=10000."
        "\nToolaufrufe und Modellaktionen werden in Barebone-Qt immer zuerst dem Nutzer zur Bestaetigung angezeigt. Behaupte nicht, dass ein Revit-Zugriff bereits ausgefuehrt wurde, solange nur ein Toolaufruf angefordert ist."
        "\nDu darfst Revit nicht direkt veraendern. Wenn eine Modellaktion sinnvoll ist, beschreibe sie kurz und fuege am Ende genau einen JSON-Codeblock mit Sprache barebone_revit_action an."
        "\nErlaubtes Schema:"
        "\n```barebone_revit_action"
        "\n{\"type\":\"revit.proposal\",\"title\":\"Kurzer Titel\",\"summary\":\"Was wird geaendert\",\"details\":\"Risiken/Voraussetzungen\",\"method\":\"revit.textNote.create\",\"params\":{\"text\":\"Notiztext\"}}"
        "\n```"
        "\nErlaubte method-Werte: revit.selection.set, revit.textNote.create, revit.parameter.setStringOnSelection."
        "\nNutze eine Aktion nur, wenn der Nutzer eine Revit-Aenderung verlangt; sonst antworte normal.");
}

} // namespace

ChatPage::ChatPage(ConfigManager& config, Workspace workspace, RevitAgent* revitAgent, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_workspace(workspace)
    , m_revitAgent(revitAgent)
    , m_network(new QNetworkAccessManager(this))
{
    m_workflows = loadWorkflowList();
    if (m_revitAgent) {
        m_bridgeStatusMessage = m_revitAgent->statusMessage();
        m_bridgeConnected = m_revitAgent->isConnected();
        for (const QString& line : m_revitAgent->recentLogLines()) {
            m_bridgeLogLines << line;
        }
    } else if (m_workspace == Workspace::Revit) {
        m_bridgeStatusMessage = QStringLiteral("Revit Agent nicht initialisiert");
    }

    buildUi();
    checkLocalAiStatus();

    if (m_revitAgent) {
        QObject::connect(m_revitAgent, &RevitAgent::bridgeStatusChanged, this, &ChatPage::setBridgeStatus);
        QObject::connect(m_revitAgent, &RevitAgent::bridgeLogAdded, this, &ChatPage::appendBridgeLog);
    }

    QObject::connect(&m_config, &ConfigManager::changed, this, [this]() {
        emitThemeAndLanguage();
        checkLocalAiStatus();
    });
}

QString ChatPage::workspaceName() const
{
    return m_workspace == Workspace::Revit ? QStringLiteral("revit") : QStringLiteral("chat");
}

void ChatPage::setBridgeStatus(const QString& message, bool connected)
{
    m_bridgeStatusMessage = message;
    m_bridgeConnected = connected;
    emitToWebAsync(m_bridge, [message, connected](AiWebBridge* target) {
        Q_EMIT target->bridgeStatusChanged(message, connected);
    });
}

void ChatPage::appendBridgeLog(const QString& message)
{
    const QString line = message.startsWith(QLatin1Char('['))
        ? message
        : QStringLiteral("[%1] %2").arg(logTime(), message);
    m_bridgeLogLines << line;
    while (m_bridgeLogLines.size() > 200) {
        m_bridgeLogLines.removeFirst();
    }
    emitToWebAsync(m_bridge, [line](AiWebBridge* target) {
        Q_EMIT target->bridgeLogAdded(line);
    });
}

void ChatPage::buildUi()
{
    setObjectName(QStringLiteral("agentPanel"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_webView = new QWebEngineView(this);
    m_webView->setObjectName(QStringLiteral("agentWebView"));
    m_webView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_webView->setMinimumSize(720, 520);
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

    m_bridge = new AiWebBridge(this);
    m_channel = new QWebChannel(m_webView);
    m_channel->registerObject(QStringLiteral("bareboneBridge"), m_bridge);
    m_webView->page()->setWebChannel(m_channel);
    m_webView->setUrl(QUrl(QStringLiteral("qrc:/web/ai-assistant.html?profile=unified&theme=%1&lang=%2&workspace=%3")
        .arg(effectiveWebTheme(m_config.theme()), effectiveUiLanguage(m_config.language()), workspaceName())));

    layout->addWidget(m_webView, 1);

    QObject::connect(m_bridge, &AiWebBridge::uiReady, this, [this]() {
        emitShellState();
    });
    QObject::connect(m_bridge, &AiWebBridge::promptSubmitted, this, [this](const QString& prompt) {
        sendChatPrompt(prompt);
    });
    QObject::connect(m_bridge, &AiWebBridge::promptSubmittedWithContext, this, [this](const QString& prompt, const QVariantMap& context) {
        sendChatPrompt(prompt, QJsonObject::fromVariantMap(context));
    });
    QObject::connect(m_bridge, &AiWebBridge::localAiStatusCheckRequested, this, [this]() {
        checkLocalAiStatus();
    });
    QObject::connect(m_bridge, &AiWebBridge::assistantWorkspaceChanged, this, [this](const QString& workspace) {
        applyWorkspace(workspace);
        emitToWebAsync(m_bridge, [workspace = workspaceName()](AiWebBridge* target) {
            Q_EMIT target->assistantWorkspaceApplied(workspace);
        });
    });
    QObject::connect(m_bridge, &AiWebBridge::clientStateSaved, this, [this](const QString& stateJson) {
        m_config.setUnifiedAiAssistantState(stateJson);
    });
    QObject::connect(m_bridge, &AiWebBridge::newChatRequested, this, [this]() {
        resetChatSession();
    });
    QObject::connect(m_bridge, &AiWebBridge::sessionOpened, this, [this](const QString&, const QVariantList&) {
        m_conversation = {};
    });
    QObject::connect(m_bridge, &AiWebBridge::reasoningEffortChanged, this, [this](const QString& effort) {
        m_config.setAiReasoningEffort(effort);
    });
    QObject::connect(m_bridge, &AiWebBridge::workflowListRequested, this, [this]() {
        emitWorkflowState();
    });
    QObject::connect(m_bridge, &AiWebBridge::workflowSelected, this, [this](const QString& workflowId) {
        if (m_workspace != Workspace::Chat) {
            m_selectedWorkflow = {};
            emitWorkflowState();
            return;
        }
        m_selectedWorkflow = workflowById(workflowId);
        emitWorkflowState();
    });
    QObject::connect(m_bridge, &AiWebBridge::workflowDeleteRequested, this, [this](const QString&) {
        if (m_workspace == Workspace::Chat) {
            appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Gebündelte Workflows sind in dieser frischen Vorlage schreibgeschützt."));
        }
    });
    QObject::connect(m_bridge, &AiWebBridge::workflowSelectionCleared, this, [this]() {
        m_selectedWorkflow = {};
        emitWorkflowState();
    });
    QObject::connect(m_bridge, &AiWebBridge::workflowTestRequested, this, [this](const QString&) {
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Workflow-Testausführung ist in dieser frischen Chat-Hülle nicht aktiv."));
    });
    QObject::connect(m_bridge, &AiWebBridge::proposalConfirmed, this, [this]() {
        if (m_workspace != Workspace::Revit || !m_revitAgent) {
            appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Diese Chat-Huelle fuehrt keine Aktionsvorschlaege aus."));
            return;
        }

        if (!m_pendingRevitToolCalls.isEmpty()) {
            executePendingRevitToolCalls();
            return;
        }

        setAgentBusy(true);
        m_revitAgent->executePendingProposal([this](const QJsonObject& response) {
            const bool ok = response.value(QStringLiteral("ok")).toBool();
            const QJsonObject result = response.value(QStringLiteral("result")).toObject();
            const QString message = ok
                ? result.value(QStringLiteral("message")).toString(QStringLiteral("Revit-Aktion ausgefuehrt."))
                : response.value(QStringLiteral("error")).toString(QStringLiteral("Revit-Aktion fehlgeschlagen."));
            appendAgentMessage(ok ? QStringLiteral("Revit") : QStringLiteral("Barebone-Qt"), message);
            if (ok) {
                emitToWebAsync(m_bridge, [](AiWebBridge* target) {
                    Q_EMIT target->proposalCleared();
                });
            }
            setAgentBusy(false);
        });
    });
    QObject::connect(m_bridge, &AiWebBridge::proposalClearedByUser, this, [this]() {
        clearPendingRevitToolCalls();
        if (m_revitAgent) {
            m_revitAgent->clearPendingProposal();
        }
    });
    QObject::connect(m_bridge, &AiWebBridge::operationCancelledByUser, this, [this]() {
        setAgentBusy(false);
    });
}

void ChatPage::sendChatPrompt(const QString& prompt, const QJsonObject& context)
{
    const QString cleanPrompt = prompt.trimmed();
    if (cleanPrompt.isEmpty() || m_busy) {
        return;
    }

    appendAgentMessage(QStringLiteral("Du"), cleanPrompt);
    setAgentBusy(true);
    if (m_revitAgent) {
        m_revitAgent->clearPendingProposal();
    }
    clearPendingRevitToolCalls();
    emitToWebAsync(m_bridge, [](AiWebBridge* target) {
        Q_EMIT target->proposalCleared();
    });

    if (m_workspace == Workspace::Revit && m_revitAgent) {
        appendBridgeLog(QStringLiteral("Qt -> AI Chat: Revit Tool-Kontext fuer Prompt wird vorbereitet"));
        m_revitAgent->requestPromptContext([this, cleanPrompt, context](const QJsonObject& revitContext) {
            if (!m_busy) {
                return;
            }
            postChatPrompt(cleanPrompt, context, revitContext);
        });
        return;
    }

    postChatPrompt(cleanPrompt, context, {});
}

void ChatPage::postChatPrompt(const QString& cleanPrompt, const QJsonObject& uiContext, const QJsonObject& revitContext)
{
    QJsonArray messages;
    QString systemPrompt = effectiveUiLanguage(m_config.language()) == QStringLiteral("en")
        ? QStringLiteral("You are Barebone-Qt. Answer plainly and concisely.")
        : QStringLiteral("Du bist Barebone-Qt. Antworte knapp, direkt und auf Deutsch.");

    if (m_workspace == Workspace::Revit) {
        systemPrompt += revitActionInstruction();
        if (!revitContext.isEmpty()) {
            systemPrompt += QStringLiteral("\nAktueller Revit-Kontext: ")
                + QString::fromUtf8(QJsonDocument(revitContext).toJson(QJsonDocument::Compact)).left(5000);
        }
    } else if (!m_selectedWorkflow.isEmpty()) {
        const QString workflowSummary = compactWorkflowSummary(m_selectedWorkflow);
        if (!workflowSummary.isEmpty()) {
            systemPrompt += QStringLiteral("\nAktiver allgemeiner Workflow-Kontext:\n") + workflowSummary;
        }
    }

    if (!uiContext.isEmpty()) {
        systemPrompt += QStringLiteral("\nKompakter UI-Kontext: ")
            + QString::fromUtf8(QJsonDocument(uiContext).toJson(QJsonDocument::Compact)).left(2000);
    }

    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemPrompt}});

    const qsizetype start = std::max<qsizetype>(0, m_conversation.size() - 20);
    for (qsizetype i = start; i < m_conversation.size(); ++i) {
        messages.append(m_conversation.at(i));
    }
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), cleanPrompt}});

    m_conversation.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), cleanPrompt}});
    postChatMessages(messages, m_workspace == Workspace::Revit && m_revitAgent && m_revitAgent->isConnected());
}

void ChatPage::postChatMessages(const QJsonArray& messages, bool allowRevitTools)
{
    m_lastChatMessages = messages;
    m_acceptRevitToolCalls = allowRevitTools;

    QNetworkRequest request(chatCompletionsUrl(m_config.aiBaseUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(kChatRequestTimeoutMs);
    const QString apiKey = m_config.aiApiKey().trimmed();
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    }

    QJsonObject payload{
        {QStringLiteral("model"), m_config.aiModel()},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("max_tokens"), 4096},
    };

    if (allowRevitTools && m_workspace == Workspace::Revit && m_revitAgent) {
        payload.insert(QStringLiteral("tools"), m_revitAgent->toolDefinitions());
        payload.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }

    appendBridgeLog(QStringLiteral("Qt -> AI Chat: %1 model=%2 workspace=%3")
        .arg(request.url().toString(), m_config.aiModel(), workspaceName()));

    QNetworkReply* reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("AI-Anfrage fehlgeschlagen: %1").arg(reply->errorString()));
            appendBridgeLog(QStringLiteral("AI Chat Fehler: %1").arg(reply->errorString()));
            setAgentBusy(false);
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("AI-Antwort war kein gueltiges JSON."));
            appendBridgeLog(QStringLiteral("AI Chat JSON Fehler: %1").arg(parseError.errorString()));
            setAgentBusy(false);
            reply->deleteLater();
            return;
        }

        handleChatResponse(document.object());
        setAgentBusy(false);
        reply->deleteLater();
    });
}

void ChatPage::handleChatResponse(const QJsonObject& response)
{
    const QJsonObject assistantMessage = chatMessageFromResponse(response);
    if (m_acceptRevitToolCalls && preparePendingRevitToolCalls(assistantMessage)) {
        return;
    }

    QString content = chatContentFromResponse(response);
    if (content.isEmpty()) {
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("AI-Antwort enthielt keinen Text."));
        return;
    }

    QVariantMap proposal;
    const bool hasRevitProposal = m_workspace == Workspace::Revit
        && m_revitAgent
        && m_revitAgent->extractProposal(content, &proposal);
    if (content.isEmpty() && hasRevitProposal) {
        content = QStringLiteral("Ich habe einen bestaetigungspflichtigen Revit-Aktionsvorschlag vorbereitet.");
    }

    m_conversation.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), content}});
    appendAgentMessage(QStringLiteral("AI"), content);
    if (hasRevitProposal) {
        emitToWebAsync(m_bridge, [proposal](AiWebBridge* target) {
            Q_EMIT target->proposalChanged(proposal);
        });
    }
}

bool ChatPage::preparePendingRevitToolCalls(const QJsonObject& assistantMessage)
{
    if (m_workspace != Workspace::Revit || !m_revitAgent || assistantMessage.isEmpty()) {
        return false;
    }

    const QJsonArray toolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    if (toolCalls.isEmpty()) {
        return false;
    }

    QJsonObject storedAssistantMessage{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), assistantMessage.value(QStringLiteral("content")).toString()},
        {QStringLiteral("tool_calls"), toolCalls},
    };

    QStringList summaryLines;
    QStringList detailLines;
    int index = 1;
    for (const QJsonValue& value : toolCalls) {
        const QJsonObject toolCall = value.toObject();
        const QString name = toolCallName(toolCall);
        const QString arguments = toolCallArgumentsText(toolCall);
        summaryLines << QStringLiteral("%1. `%2`: %3").arg(index).arg(name, revitToolDescription(name));
        detailLines << QStringLiteral("%1. Tool: `%2`\n   Zweck: %3\n   Parameter: `%4`")
                .arg(index)
                .arg(name, revitToolDescription(name), arguments.isEmpty() ? QStringLiteral("{}") : arguments);
        ++index;
    }

    m_pendingRevitToolMessages = m_lastChatMessages;
    m_pendingRevitToolMessages.append(storedAssistantMessage);
    m_pendingRevitToolCalls = toolCalls;
    m_acceptRevitToolCalls = false;

    QVariantMap proposal{
        {QStringLiteral("title"), QStringLiteral("Revit API Zugriff bestaetigen")},
        {QStringLiteral("summary"), QStringLiteral("Die AI moechte folgende Revit-Daten lesen:\n%1").arg(summaryLines.join(QLatin1Char('\n')))},
        {QStringLiteral("details"), QStringLiteral("Ausgefuehrt wird erst nach Klick auf Ausfuehren.\n\n%1").arg(detailLines.join(QStringLiteral("\n\n")))},
        {QStringLiteral("canRun"), true},
    };
    emitToWebAsync(m_bridge, [proposal](AiWebBridge* target) {
        Q_EMIT target->proposalChanged(proposal);
    });
    appendBridgeLog(QStringLiteral("AI -> Qt: %1 Revit Toolcall(s) warten auf Bestaetigung").arg(toolCalls.size()));
    return true;
}

void ChatPage::executePendingRevitToolCalls()
{
    if (m_workspace != Workspace::Revit || !m_revitAgent) {
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Revit Tools koennen nur im Revit-Modus ausgefuehrt werden."));
        clearPendingRevitToolCalls();
        return;
    }
    if (m_pendingRevitToolCalls.isEmpty()) {
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Kein Revit Toolaufruf wartet auf Bestaetigung."));
        return;
    }

    setAgentBusy(true);
    emitToWebAsync(m_bridge, [](AiWebBridge* target) {
        Q_EMIT target->proposalCleared();
    });

    auto calls = QSharedPointer<QJsonArray>::create(m_pendingRevitToolCalls);
    auto messages = QSharedPointer<QJsonArray>::create(m_pendingRevitToolMessages);
    auto index = QSharedPointer<int>::create(0);
    auto runNext = QSharedPointer<std::function<void()>>::create();
    QPointer<ChatPage> guard(this);

    *runNext = [guard, calls, messages, index, runNext]() mutable {
        if (!guard) {
            return;
        }
        if (*index >= calls->size()) {
            const QJsonArray finalMessages = *messages;
            guard->clearPendingRevitToolCalls();
            guard->appendBridgeLog(QStringLiteral("Qt -> AI Chat: Revit Tool-Ergebnisse werden uebergeben"));
            guard->postChatMessages(finalMessages, false);
            return;
        }

        const QJsonObject toolCall = calls->at(*index).toObject();
        ++(*index);

        const QString id = toolCall.value(QStringLiteral("id")).toString(QStringLiteral("barebone_revit_tool"));
        const QString name = toolCallName(toolCall);
        QString argumentError;
        const QJsonObject arguments = toolCallArgumentsObject(toolCall, &argumentError);

        auto appendToolResult = [guard, messages, id, name, runNext](const QJsonObject& response) mutable {
            if (!guard) {
                return;
            }
            if (name == QStringLiteral("revit_elements_list") && response.value(QStringLiteral("ok")).toBool()) {
                const QString table = formatRevitElementsTable(response);
                if (!table.isEmpty()) {
                    guard->appendAgentMessage(QStringLiteral("Revit"), table);
                }
            }
            const QJsonObject safeResponse = modelSafeToolResponse(name, response);
            messages->append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("tool")},
                {QStringLiteral("tool_call_id"), id},
                {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(safeResponse).toJson(QJsonDocument::Compact))},
            });
            (*runNext)();
        };

        if (!argumentError.isEmpty()) {
            appendToolResult(toolResponseError(argumentError));
            return;
        }

        guard->appendBridgeLog(QStringLiteral("Qt -> Revit: bestaetigter Toolcall %1").arg(name));
        guard->m_revitAgent->executeReadOnlyToolCall(name, arguments, appendToolResult);
    };

    (*runNext)();
}

void ChatPage::clearPendingRevitToolCalls()
{
    m_pendingRevitToolMessages = {};
    m_pendingRevitToolCalls = {};
    m_acceptRevitToolCalls = false;
}

void ChatPage::applyWorkspace(const QString& workspace)
{
    const Workspace nextWorkspace = workspaceFromName(workspace);
    if (m_workspace == nextWorkspace) {
        return;
    }

    m_workspace = nextWorkspace;
    if (m_workspace == Workspace::Revit) {
        if (m_revitAgent) {
            setBridgeStatus(m_revitAgent->statusMessage(), m_revitAgent->isConnected());
        } else {
            setBridgeStatus(QStringLiteral("Revit Agent nicht initialisiert"), false);
        }
        m_selectedWorkflow = {};
    }
    emitWorkflowState();
}

void ChatPage::checkLocalAiStatus()
{
    QNetworkRequest request(modelsUrl(m_config.aiBaseUrl()));
    request.setTransferTimeout(5000);
    const QString apiKey = m_config.aiApiKey().trimmed();
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    }

    QNetworkReply* reply = m_network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_localAiReachable = reply->error() == QNetworkReply::NoError;
        m_localAiStatusMessage = m_localAiReachable
            ? QStringLiteral("Lokale AI erreichbar")
            : QStringLiteral("Lokale AI nicht erreichbar");
        emitToWebAsync(m_bridge, [message = m_localAiStatusMessage, reachable = m_localAiReachable](AiWebBridge* target) {
            Q_EMIT target->localAiStatusChanged(message, reachable);
        });
        appendBridgeLog(QStringLiteral("AI Status: %1").arg(m_localAiStatusMessage));
        reply->deleteLater();
    });
}

void ChatPage::appendAgentMessage(const QString& speaker, const QString& message, const QVariantMap& extra)
{
    if (!m_bridge) {
        return;
    }

    QVariantMap payload = extra;
    payload.insert(QStringLiteral("speaker"), speaker);
    payload.insert(QStringLiteral("message"), message);
    payload.insert(QStringLiteral("time"), nowTime());
    emitToWebAsync(m_bridge, [payload](AiWebBridge* target) {
        Q_EMIT target->messageAdded(payload);
    });
}

void ChatPage::setAgentBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emitToWebAsync(m_bridge, [busy](AiWebBridge* target) {
        Q_EMIT target->statusChanged(busy ? QStringLiteral("thinking") : QStringLiteral("idle"));
    });
}

void ChatPage::emitShellState() const
{
    emitToWebAsync(m_bridge, [this](AiWebBridge* target) {
        Q_EMIT target->clientStateLoaded(m_config.unifiedAiAssistantState());
        Q_EMIT target->assistantWorkspaceApplied(workspaceName());
        Q_EMIT target->trainingModeApplied(false);
        Q_EMIT target->reasoningEffortApplied(m_config.aiReasoningEffort());
        Q_EMIT target->uiThemeChanged(effectiveWebTheme(m_config.theme()));
        Q_EMIT target->uiLanguageChanged(effectiveUiLanguage(m_config.language()));
        Q_EMIT target->localAiStatusChanged(m_localAiStatusMessage, m_localAiReachable);
        Q_EMIT target->bridgeStatusChanged(m_bridgeStatusMessage, m_bridgeConnected);
        Q_EMIT target->workflowListChanged(m_workspace == Workspace::Chat ? m_workflows : QVariantList{});
        Q_EMIT target->selectedWorkflowChanged(m_workspace == Workspace::Chat ? m_selectedWorkflow : QVariantMap{});
        Q_EMIT target->proposalCleared();
        Q_EMIT target->statusChanged(m_busy ? QStringLiteral("thinking") : QStringLiteral("idle"));
        Q_EMIT target->contextBudgetChanged(QVariantMap{
            {QStringLiteral("usedTokens"), 0},
            {QStringLiteral("maxTokens"), 0},
            {QStringLiteral("detail"), QStringLiteral("Chat-Huelle ohne Revit-Aktionspipeline")},
        });
        for (const QString& line : m_bridgeLogLines) {
            Q_EMIT target->bridgeLogAdded(line);
        }
    });
}

void ChatPage::emitThemeAndLanguage() const
{
    emitToWebAsync(m_bridge, [this](AiWebBridge* target) {
        Q_EMIT target->uiThemeChanged(effectiveWebTheme(m_config.theme()));
        Q_EMIT target->uiLanguageChanged(effectiveUiLanguage(m_config.language()));
    });
}

void ChatPage::resetChatSession()
{
    m_conversation = {};
    setAgentBusy(false);
}

QVariantList ChatPage::loadWorkflowList() const
{
    QVariantList workflows;
    for (const QString& resourcePath : generalWorkflowResourcePaths()) {
        QFile file(resourcePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        QJsonObject workflow = document.object();
        const QString id = workflowIdFromObject(workflow, resourcePath);
        workflow.insert(QStringLiteral("id"), id);
        workflow.insert(QStringLiteral("fileName"), QFileInfo(resourcePath).fileName());
        workflow.insert(QStringLiteral("source"), QStringLiteral("bundled"));
        workflows.append(workflow.toVariantMap());
    }
    return workflows;
}

QVariantMap ChatPage::workflowById(const QString& workflowId) const
{
    const QString wanted = workflowId.trimmed();
    if (wanted.isEmpty()) {
        return {};
    }
    for (const QVariant& value : m_workflows) {
        const QVariantMap workflow = value.toMap();
        if (workflow.value(QStringLiteral("id")).toString() == wanted) {
            return workflow;
        }
    }
    return {};
}

void ChatPage::emitWorkflowState() const
{
    emitToWebAsync(m_bridge, [this](AiWebBridge* target) {
        Q_EMIT target->workflowListChanged(m_workspace == Workspace::Chat ? m_workflows : QVariantList{});
        Q_EMIT target->selectedWorkflowChanged(m_workspace == Workspace::Chat ? m_selectedWorkflow : QVariantMap{});
    });
}
