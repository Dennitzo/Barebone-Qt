#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class DrawingContextStore {
public:
    void clear();
    void markDirty(const QString& reason);
    void ingestCapabilityResponse(const QString& method, const QJsonObject& response);
    void ingestReadResponse(const QString& method, const QJsonObject& params, const QJsonObject& response);
    void ingestValidationResult(const QJsonObject& proposal, const QJsonObject& result);
    void ingestExecutionResult(const QJsonObject& batchResult);
    void updateFromContextResponse(const QString& method, const QJsonObject& params, const QJsonObject& response);
    void updateFromPrefetchedContext(const QJsonObject& prefetchedDrawingContext);
    void recordActionBatchResult(const QJsonObject& batchResult);

    bool hasSnapshot() const;
    QJsonObject manifest() const;
    QJsonObject contextManifest() const;
    QJsonObject fetch(const QJsonObject& params) const;
    QJsonObject query(const QJsonObject& params) const;
    QJsonObject inspect(const QJsonObject& params) const;
    QJsonObject fullScan(const QJsonObject& params) const;
    QJsonObject fullContext(const QJsonObject& params) const;
    QJsonObject executionHistory(const QJsonObject& params) const;
    QJsonObject agentContext() const;

private:
    QJsonArray limitedArray(const QJsonArray& values, int limit) const;
    void ingestResult(const QString& method, const QJsonObject& params, const QJsonObject& result, bool ok, const QString& error = {});
    void ingestFactsFromResult(const QString& method, const QJsonObject& result);
    void upsertFact(const QJsonObject& fact);
    QJsonArray factsMatching(const QJsonObject& filters, int limit) const;
    QJsonArray pagedArray(const QJsonArray& values, int offset, int limit, QJsonObject* pageInfo = nullptr) const;
    QJsonArray filteredFacts(const QJsonArray& source, const QJsonObject& filters, int limit) const;

    int m_revision = 0;
    QString m_updatedAtUtc;
    QJsonArray m_documentBlocks;
    QJsonArray m_capabilityBlocks;
    QJsonArray m_layers;
    QJsonArray m_entities;
    QJsonArray m_bimObjects;
    QJsonArray m_selection;
    QJsonArray m_normalizedFacts;
    QJsonArray m_rawBlocks;
    QJsonArray m_dirtyRegions;
    QJsonArray m_actionSummaries;
    QJsonArray m_validationHistory;
    QJsonArray m_executionHistory;
};
