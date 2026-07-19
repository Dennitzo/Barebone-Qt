#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

class LocalAiJobScheduler final : public QObject {
    Q_OBJECT

public:
    struct Result {
        QString id;
        QString kind;
        QString lane;
        QString dedupeKey;
        bool background = false;
        bool aborted = false;
        int httpStatus = 0;
        QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
        QString errorString;
        QByteArray body;
    };

    struct Job {
        QString id;
        QString kind;
        QString lane;
        int priority = 0;
        bool background = false;
        bool cancellable = true;
        QString dedupeKey;
        QNetworkRequest request;
        QByteArray body;
        int operationGeneration = 0;
        std::function<void(const Result&)> callback;
    };

    explicit LocalAiJobScheduler(QObject* parent = nullptr);

    void setMaxConcurrentJobs(int value);
    int maxConcurrentJobs() const;
    int activeCount() const;
    int activeForegroundCount() const;
    int activeBackgroundCount() const;
    int queuedCount() const;

    QString enqueue(Job job);
    void abortAll();
    QJsonObject status() const;

Q_SIGNALS:
    void statusChanged(const QJsonObject& status);
    void jobStarted(const QString& id, const QString& kind, bool background);
    void jobFinished(const QString& id, const QString& kind, bool background, bool aborted);

private:
    struct QueuedJob {
        Job job;
        qint64 sequence = 0;
    };

    struct ActiveJob {
        Job job;
        QNetworkReply* reply = nullptr;
        qint64 sequence = 0;
        bool aborting = false;
    };

    void drainQueue();
    void startJob(const QueuedJob& queued);
    void finishReply(QNetworkReply* reply);
    void sortQueue();
    bool abortOldestCancellableBackground(const QString& reason);
    bool abortActiveJobByDedupeKey(const QString& dedupeKey);
    void emitStatus();

    QNetworkAccessManager* m_network = nullptr;
    QVector<QueuedJob> m_queue;
    QVector<ActiveJob> m_active;
    int m_maxConcurrentJobs = 1;
    qint64 m_nextSequence = 1;
    int m_abortedBackgroundJobs = 0;
    int m_throttledBackgroundJobs = 0;
};
