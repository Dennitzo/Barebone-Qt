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

QString chatContentFromResponse(const QJsonObject& response)
{
    const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return {};
    }
    const QJsonObject first = choices.first().toObject();
    const QJsonObject message = first.value(QStringLiteral("message")).toObject();
    QString content = message.value(QStringLiteral("content")).toString().trimmed();
    if (content.isEmpty()) {
        content = first.value(QStringLiteral("text")).toString().trimmed();
    }
    return content;
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

} // namespace

ChatPage::ChatPage(ConfigManager& config, Workspace workspace, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_workspace(workspace)
    , m_network(new QNetworkAccessManager(this))
{
    if (m_workspace == Workspace::BricsCad) {
        m_bridgeStatusMessage = QStringLiteral("BRX Plugin nicht verbunden");
    } else {
        m_workflows = loadWorkflowList();
    }

    buildUi();
    checkLocalAiStatus();

    QObject::connect(&m_config, &ConfigManager::changed, this, [this]() {
        emitThemeAndLanguage();
        checkLocalAiStatus();
    });
}

QString ChatPage::workspaceName() const
{
    return m_workspace == Workspace::BricsCad ? QStringLiteral("bricscad") : QStringLiteral("chat");
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
    QObject::connect(m_bridge, &AiWebBridge::assistantWorkspaceChanged, this, [this](const QString&) {
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
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("Diese Chat-Hülle führt keine Aktionsvorschläge aus."));
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

    QJsonArray messages;
    QString systemPrompt = effectiveUiLanguage(m_config.language()) == QStringLiteral("en")
        ? QStringLiteral("You are Barebone-Qt. Answer plainly and concisely.")
        : QStringLiteral("Du bist Barebone-Qt. Antworte knapp, direkt und auf Deutsch.");

    if (m_workspace == Workspace::BricsCad) {
        systemPrompt += QStringLiteral("\nBricsCAD ist aktuell eine frische Chat-Huelle. Es gibt keine CAD-Aktionsausfuehrung, keine BricsCAD-Tools, keine Workflows und keine automatische Zeichnungsbearbeitung. Die BRX-Plugin-Verbindung ist nur ein minimales Grundgeruest.");
    } else if (!m_selectedWorkflow.isEmpty()) {
        const QString workflowSummary = compactWorkflowSummary(m_selectedWorkflow);
        if (!workflowSummary.isEmpty()) {
            systemPrompt += QStringLiteral("\nAktiver allgemeiner Workflow-Kontext:\n") + workflowSummary;
        }
    }

    if (!context.isEmpty()) {
        systemPrompt += QStringLiteral("\nKompakter UI-Kontext: ")
            + QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact)).left(2000);
    }

    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemPrompt}});

    const qsizetype start = std::max<qsizetype>(0, m_conversation.size() - 20);
    for (qsizetype i = start; i < m_conversation.size(); ++i) {
        messages.append(m_conversation.at(i));
    }
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), cleanPrompt}});

    m_conversation.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), cleanPrompt}});

    QNetworkRequest request(chatCompletionsUrl(m_config.aiBaseUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(kChatRequestTimeoutMs);
    const QString apiKey = m_config.aiApiKey().trimmed();
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    }

    const QJsonObject payload{
        {QStringLiteral("model"), m_config.aiModel()},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("max_tokens"), 2048},
    };

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
    const QString content = chatContentFromResponse(response);
    if (content.isEmpty()) {
        appendAgentMessage(QStringLiteral("Barebone-Qt"), QStringLiteral("AI-Antwort enthielt keinen Text."));
        return;
    }
    m_conversation.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), content}});
    appendAgentMessage(QStringLiteral("AI"), content);
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
            {QStringLiteral("detail"), QStringLiteral("Chat-Huelle ohne BricsCAD-Aktionspipeline")},
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
