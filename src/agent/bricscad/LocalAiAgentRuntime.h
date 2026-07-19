#pragma once

#include "../LocalAiJobScheduler.h"
#include "../../core/ConfigManager.h"

#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class LocalAiAgentRuntime final : public QObject {
    Q_OBJECT

public:
    enum class ResponseKind {
        VisibleMarkdown,
        StructuredJson,
    };

    struct JsonRequest {
        QString slot;
        QString kind;
        QString systemInstruction;
        QJsonObject input;
        QStringList requiredFields;
        int maxOutputTokens = 1024;
        int priority = 80;
        bool background = false;
        bool cancellable = true;
        QString dedupeKey;
        int timeoutMs = 90000;
        int operationGeneration = 0;
        bool bricsCadMode = false;
    };

    struct JsonResult {
        bool ok = false;
        QString error;
        QJsonObject object;
        QString rawContent;
        QByteArray rawBody;
    };

    explicit LocalAiAgentRuntime(ConfigManager& config, QObject* parent = nullptr);

    QString enqueuePost(
        QNetworkRequest request,
        const QJsonObject& payload,
        const QString& kind,
        int priority,
        bool background,
        bool cancellable,
        const QString& dedupeKey,
        int operationGeneration,
        bool bricsCadMode,
        std::function<void(const LocalAiJobScheduler::Result&)> callback);

    void submitJson(const JsonRequest& request, std::function<void(const JsonResult&)> callback);
    void abortAll();
    QJsonObject status() const;

Q_SIGNALS:
    void statusChanged(const QJsonObject& status);
    void jobStarted(const QString& id, const QString& kind, bool background);
    void jobFinished(const QString& id, const QString& kind, bool background, bool aborted);

private:
    QString provider() const;
    QString model() const;
    QString baseUrl() const;
    bool officialProvider() const;
    bool useResponsesApi(ResponseKind responseKind, const QString& modelName) const;
    QString reasoningEffortForModel(const QString& modelName, const QString& configuredEffort) const;
    QJsonObject structuredResponseFormat(const QString& name, const QStringList& requiredFields) const;
    QString chatCompletionContent(const QJsonObject& response) const;
    QJsonObject objectFromModelContent(const QString& content, bool* ok) const;

    ConfigManager& m_config;
    LocalAiJobScheduler* m_scheduler = nullptr;
};
