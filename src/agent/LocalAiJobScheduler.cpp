#include "LocalAiJobScheduler.h"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <limits>

LocalAiJobScheduler::LocalAiJobScheduler(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void LocalAiJobScheduler::setMaxConcurrentJobs(int value)
{
    const int bounded = std::clamp(value, 1, 4);
    if (m_maxConcurrentJobs == bounded) {
        return;
    }
    m_maxConcurrentJobs = bounded;
    drainQueue();
    emitStatus();
}

int LocalAiJobScheduler::maxConcurrentJobs() const
{
    return m_maxConcurrentJobs;
}

int LocalAiJobScheduler::activeCount() const
{
    return m_active.size();
}

int LocalAiJobScheduler::activeForegroundCount() const
{
    int count = 0;
    for (const ActiveJob& job : m_active) {
        if (!job.job.background) {
            ++count;
        }
    }
    return count;
}

int LocalAiJobScheduler::activeBackgroundCount() const
{
    int count = 0;
    for (const ActiveJob& job : m_active) {
        if (job.job.background) {
            ++count;
        }
    }
    return count;
}

int LocalAiJobScheduler::queuedCount() const
{
    return m_queue.size();
}

QString LocalAiJobScheduler::enqueue(Job job)
{
    if (job.id.trimmed().isEmpty()) {
        job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (job.kind.trimmed().isEmpty()) {
        job.kind = job.background ? QStringLiteral("Background") : QStringLiteral("Foreground");
    }

    if (!job.dedupeKey.trimmed().isEmpty()) {
        for (int i = m_queue.size() - 1; i >= 0; --i) {
            if (m_queue.at(i).job.dedupeKey == job.dedupeKey) {
                if (m_queue.at(i).job.background) {
                    ++m_throttledBackgroundJobs;
                }
                m_queue.removeAt(i);
            }
        }
        if (job.background) {
            abortActiveJobByDedupeKey(job.dedupeKey);
        }
    }

    if (!job.background && m_active.size() >= m_maxConcurrentJobs) {
        abortOldestCancellableBackground(QStringLiteral("foreground-priority"));
    }

    m_queue.append(QueuedJob{job, m_nextSequence++});
    sortQueue();
    drainQueue();
    emitStatus();
    return job.id;
}

void LocalAiJobScheduler::abortAll()
{
    m_queue.clear();
    for (ActiveJob& active : m_active) {
        active.aborting = true;
        if (active.reply && active.reply->isRunning()) {
            active.reply->abort();
        }
    }
    emitStatus();
}

QJsonObject LocalAiJobScheduler::status() const
{
    QJsonArray activeJobs;
    for (const ActiveJob& active : m_active) {
        activeJobs.append(QJsonObject{
            {QStringLiteral("id"), active.job.id},
            {QStringLiteral("kind"), active.job.kind},
            {QStringLiteral("background"), active.job.background},
            {QStringLiteral("cancellable"), active.job.cancellable},
            {QStringLiteral("dedupeKey"), active.job.dedupeKey},
        });
    }

    QJsonArray queuedJobs;
    for (const QueuedJob& queued : m_queue) {
        queuedJobs.append(QJsonObject{
            {QStringLiteral("id"), queued.job.id},
            {QStringLiteral("kind"), queued.job.kind},
            {QStringLiteral("background"), queued.job.background},
            {QStringLiteral("priority"), queued.job.priority},
            {QStringLiteral("dedupeKey"), queued.job.dedupeKey},
        });
    }

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.local-ai-job-scheduler.status.v1")},
        {QStringLiteral("maxConcurrent"), m_maxConcurrentJobs},
        {QStringLiteral("active"), m_active.size()},
        {QStringLiteral("activeForeground"), activeForegroundCount()},
        {QStringLiteral("activeBackground"), activeBackgroundCount()},
        {QStringLiteral("queued"), m_queue.size()},
        {QStringLiteral("abortedBackgroundJobs"), m_abortedBackgroundJobs},
        {QStringLiteral("throttledBackgroundJobs"), m_throttledBackgroundJobs},
        {QStringLiteral("activeJobs"), activeJobs},
        {QStringLiteral("queuedJobs"), queuedJobs},
    };
}

void LocalAiJobScheduler::drainQueue()
{
    sortQueue();
    while (!m_queue.isEmpty() && m_active.size() < m_maxConcurrentJobs) {
        const QueuedJob next = m_queue.takeFirst();
        startJob(next);
    }
}

void LocalAiJobScheduler::startJob(const QueuedJob& queued)
{
    QNetworkReply* reply = m_network->post(queued.job.request, queued.job.body);
    m_active.append(ActiveJob{queued.job, reply, queued.sequence, false});
    Q_EMIT jobStarted(queued.job.id, queued.job.kind, queued.job.background);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        finishReply(reply);
    });
}

void LocalAiJobScheduler::finishReply(QNetworkReply* reply)
{
    int index = -1;
    for (int i = 0; i < m_active.size(); ++i) {
        if (m_active.at(i).reply == reply) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        reply->deleteLater();
        return;
    }

    const ActiveJob active = m_active.takeAt(index);
    Result result;
    result.id = active.job.id;
    result.kind = active.job.kind;
    result.dedupeKey = active.job.dedupeKey;
    result.background = active.job.background;
    result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.networkError = reply->error();
    result.errorString = reply->errorString();
    result.body = reply->readAll();
    result.aborted = active.aborting || result.networkError == QNetworkReply::OperationCanceledError;

    Q_EMIT jobFinished(result.id, result.kind, result.background, result.aborted);
    if (active.job.callback) {
        active.job.callback(result);
    }
    reply->deleteLater();
    drainQueue();
    emitStatus();
}

void LocalAiJobScheduler::sortQueue()
{
    std::stable_sort(m_queue.begin(), m_queue.end(), [](const QueuedJob& left, const QueuedJob& right) {
        if (left.job.priority != right.job.priority) {
            return left.job.priority > right.job.priority;
        }
        if (left.job.background != right.job.background) {
            return !left.job.background;
        }
        return left.sequence < right.sequence;
    });
}

bool LocalAiJobScheduler::abortOldestCancellableBackground(const QString& reason)
{
    Q_UNUSED(reason);
    int index = -1;
    qint64 sequence = std::numeric_limits<qint64>::max();
    for (int i = 0; i < m_active.size(); ++i) {
        const ActiveJob& active = m_active.at(i);
        if (active.job.background && active.job.cancellable && !active.aborting && active.sequence < sequence) {
            index = i;
            sequence = active.sequence;
        }
    }
    if (index < 0) {
        return false;
    }
    ++m_abortedBackgroundJobs;
    m_active[index].aborting = true;
    if (m_active[index].reply && m_active[index].reply->isRunning()) {
        m_active[index].reply->abort();
    }
    emitStatus();
    return true;
}

bool LocalAiJobScheduler::abortActiveJobByDedupeKey(const QString& dedupeKey)
{
    bool aborted = false;
    for (ActiveJob& active : m_active) {
        if (active.job.background
            && active.job.cancellable
            && active.job.dedupeKey == dedupeKey
            && !active.aborting) {
            ++m_abortedBackgroundJobs;
            active.aborting = true;
            if (active.reply && active.reply->isRunning()) {
                active.reply->abort();
            }
            aborted = true;
        }
    }
    if (aborted) {
        emitStatus();
    }
    return aborted;
}

void LocalAiJobScheduler::emitStatus()
{
    Q_EMIT statusChanged(status());
}
