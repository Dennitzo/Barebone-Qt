#include "DrawingContextStore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>

namespace {

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

int resultCount(const QJsonObject& result)
{
    return result.value(QStringLiteral("count")).toInt(
        result.value(QStringLiteral("objects")).toArray().size()
        + result.value(QStringLiteral("entities")).toArray().size()
        + result.value(QStringLiteral("layers")).toArray().size()
        + result.value(QStringLiteral("commands")).toArray().size()
        + result.value(QStringLiteral("methods")).toArray().size()
        + result.value(QStringLiteral("actions")).toArray().size());
}

QJsonArray firstArray(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        const QJsonArray values = object.value(key).toArray();
        if (!values.isEmpty()) {
            return values;
        }
    }
    return {};
}

QString objectFieldText(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        const QString value = object.value(key).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

bool objectMatchesFilters(const QJsonObject& object, const QJsonObject& filters)
{
    const QJsonArray handles = filters.value(QStringLiteral("handles")).toArray();
    if (!handles.isEmpty()) {
        const QString handle = objectFieldText(object, {
            QStringLiteral("handle"),
            QStringLiteral("id"),
            QStringLiteral("objectId"),
        });
        bool matched = false;
        for (const QJsonValue& value : handles) {
            if (handle.compare(value.toString().trimmed(), Qt::CaseInsensitive) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    const QJsonArray names = filters.value(QStringLiteral("names")).toArray();
    if (!names.isEmpty()) {
        const QString name = objectFieldText(object, {
            QStringLiteral("name"),
            QStringLiteral("blockName"),
            QStringLiteral("displayName"),
        });
        bool matched = false;
        for (const QJsonValue& value : names) {
            if (name.compare(value.toString().trimmed(), Qt::CaseInsensitive) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    const QString guid = filters.value(QStringLiteral("guid")).toString().trimmed();
    if (!guid.isEmpty()
        && objectFieldText(object, {QStringLiteral("guid")}).compare(guid, Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QString layer = filters.value(QStringLiteral("layer")).toString().trimmed();
    if (!layer.isEmpty()
        && objectFieldText(object, {QStringLiteral("layer"), QStringLiteral("layerName")})
            .compare(layer, Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QString type = filters.value(QStringLiteral("type")).toString().trimmed();
    if (!type.isEmpty()) {
        const QString objectType = objectFieldText(object, {
            QStringLiteral("type"),
            QStringLiteral("kind"),
            QStringLiteral("entityType"),
            QStringLiteral("bimType"),
            QStringLiteral("shape"),
        });
        if (!objectType.contains(type, Qt::CaseInsensitive)) {
            return false;
        }
    }

    const QJsonArray types = filters.value(QStringLiteral("types")).toArray();
    if (!types.isEmpty()) {
        const QString objectType = objectFieldText(object, {
            QStringLiteral("type"),
            QStringLiteral("kind"),
            QStringLiteral("entityType"),
            QStringLiteral("bimType"),
            QStringLiteral("shape"),
        });
        bool matched = false;
        for (const QJsonValue& value : types) {
            if (objectType.contains(value.toString(), Qt::CaseInsensitive)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    const QString bimType = filters.value(QStringLiteral("bimType")).toString(
        filters.value(QStringLiteral("classification")).toString()).trimmed();
    if (!bimType.isEmpty()) {
        const QString objectBimType = objectFieldText(object, {
            QStringLiteral("bimType"),
            QStringLiteral("classification"),
        });
        if (!objectBimType.contains(bimType, Qt::CaseInsensitive)) {
            return false;
        }
    }

    if (filters.value(QStringLiteral("selectionOnly")).toBool(false)
        && !object.value(QStringLiteral("selectionState")).toObject().value(QStringLiteral("selected")).toBool(false)) {
        return false;
    }

    return true;
}

QJsonObject compactObjectFact(const QString& method, const QJsonObject& object, bool selected)
{
    const QString handle = objectFieldText(object, {
        QStringLiteral("handle"),
        QStringLiteral("id"),
        QStringLiteral("objectId"),
    });
    QJsonObject bim = object.value(QStringLiteral("bim")).toObject();
    const QString bimType = objectFieldText(object, {
        QStringLiteral("bimType"),
        QStringLiteral("classification"),
        QStringLiteral("type"),
    });
    if (!bimType.isEmpty() && !bim.contains(QStringLiteral("type"))) {
        bim.insert(QStringLiteral("type"), bimType);
    }

    QJsonObject fact{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.fact.v1")},
        {QStringLiteral("handle"), handle},
        {QStringLiteral("sourceMethods"), QJsonArray{method}},
        {QStringLiteral("entityType"), objectFieldText(object, {
            QStringLiteral("entityType"),
            QStringLiteral("type"),
            QStringLiteral("kind"),
            QStringLiteral("shape"),
        })},
        {QStringLiteral("layer"), objectFieldText(object, {QStringLiteral("layer"), QStringLiteral("layerName")})},
        {QStringLiteral("bounds"), object.value(QStringLiteral("bounds"))},
        {QStringLiteral("dimensions"), object.value(QStringLiteral("dimensions"))},
        {QStringLiteral("geometry"), object.value(QStringLiteral("geometry"))},
        {QStringLiteral("bim"), bim},
        {QStringLiteral("properties"), object.value(QStringLiteral("properties")).toArray()},
        {QStringLiteral("selectionState"), QJsonObject{{QStringLiteral("selected"), selected}}},
        {QStringLiteral("lastReadAtUtc"), nowUtc()},
        {QStringLiteral("staleness"), QStringLiteral("fresh")},
        {QStringLiteral("confidence"), handle.isEmpty() ? 0.65 : 0.95},
        {QStringLiteral("raw"), object},
    };

    const QString name = objectFieldText(object, {
        QStringLiteral("name"),
        QStringLiteral("blockName"),
        QStringLiteral("displayName"),
    });
    if (!name.isEmpty()) {
        fact.insert(QStringLiteral("name"), name);
    }
    const QString guid = objectFieldText(object, {QStringLiteral("guid")});
    if (!guid.isEmpty()) {
        fact.insert(QStringLiteral("guid"), guid);
    }
    return fact;
}

} // namespace

void DrawingContextStore::clear()
{
    m_revision = 0;
    m_updatedAtUtc.clear();
    m_documentBlocks = {};
    m_capabilityBlocks = {};
    m_layers = {};
    m_entities = {};
    m_bimObjects = {};
    m_selection = {};
    m_normalizedFacts = {};
    m_rawBlocks = {};
    m_dirtyRegions = {};
    m_actionSummaries = {};
    m_validationHistory = {};
    m_executionHistory = {};
    m_workflowRepairHistory = {};
}

void DrawingContextStore::markDirty(const QString& reason)
{
    ++m_revision;
    m_updatedAtUtc = nowUtc();
    m_dirtyRegions.append(QJsonObject{
        {QStringLiteral("at"), m_updatedAtUtc},
        {QStringLiteral("reason"), reason.trimmed().isEmpty() ? QStringLiteral("drawing changed") : reason.trimmed()},
    });
    while (m_dirtyRegions.size() > 20) {
        m_dirtyRegions.removeFirst();
    }
}

void DrawingContextStore::updateFromContextResponse(const QString& method, const QJsonObject& params, const QJsonObject& response)
{
    if (method == QStringLiteral("capabilities.list")
        || method == QStringLiteral("actions.list")
        || method == QStringLiteral("commands.list")) {
        ingestCapabilityResponse(method, response);
        return;
    }
    ingestReadResponse(method, params, response);
}

void DrawingContextStore::ingestCapabilityResponse(const QString& method, const QJsonObject& response)
{
    const bool ok = response.value(QStringLiteral("ok")).toBool(false);
    const QJsonObject result = response.value(QStringLiteral("result")).toObject(response);
    ingestResult(method, {}, result, ok, response.value(QStringLiteral("error")).toString());
}

void DrawingContextStore::ingestReadResponse(const QString& method, const QJsonObject& params, const QJsonObject& response)
{
    const bool ok = response.value(QStringLiteral("ok")).toBool(false);
    const QJsonObject result = response.value(QStringLiteral("result")).toObject(response);
    ingestResult(method, params, result, ok, response.value(QStringLiteral("error")).toString());
}

void DrawingContextStore::ingestValidationResult(const QJsonObject& proposal, const QJsonObject& result)
{
    ++m_revision;
    m_updatedAtUtc = nowUtc();
    m_validationHistory.append(QJsonObject{
        {QStringLiteral("at"), m_updatedAtUtc},
        {QStringLiteral("proposal"), proposal},
        {QStringLiteral("result"), result},
    });
    while (m_validationHistory.size() > 100) {
        m_validationHistory.removeFirst();
    }
}

void DrawingContextStore::ingestExecutionResult(const QJsonObject& batchResult)
{
    recordActionBatchResult(batchResult);
}

void DrawingContextStore::updateFromPrefetchedContext(const QJsonObject& prefetchedDrawingContext)
{
    for (const QJsonValue& value : prefetchedDrawingContext.value(QStringLiteral("requests")).toArray()) {
        const QJsonObject entry = value.toObject();
        ingestResult(
            entry.value(QStringLiteral("method")).toString(),
            entry.value(QStringLiteral("params")).toObject(),
            entry.value(QStringLiteral("result")).toObject(),
            entry.value(QStringLiteral("ok")).toBool(false),
            entry.value(QStringLiteral("error")).toString());
    }
}

void DrawingContextStore::recordActionBatchResult(const QJsonObject& batchResult)
{
    ++m_revision;
    m_updatedAtUtc = nowUtc();
    const QString summary = batchResult.value(QStringLiteral("summary")).toString();
    m_actionSummaries.append(QJsonObject{
        {QStringLiteral("at"), m_updatedAtUtc},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("executionStats"), batchResult.value(QStringLiteral("executionStats")).toObject()},
    });
    while (m_actionSummaries.size() > 20) {
        m_actionSummaries.removeFirst();
    }
    m_dirtyRegions.append(QJsonObject{
        {QStringLiteral("at"), m_updatedAtUtc},
        {QStringLiteral("reason"), summary.isEmpty() ? QStringLiteral("BRX action batch executed") : summary},
    });
    while (m_dirtyRegions.size() > 20) {
        m_dirtyRegions.removeFirst();
    }

    m_executionHistory.append(QJsonObject{
        {QStringLiteral("at"), m_updatedAtUtc},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("status"), batchResult.value(QStringLiteral("failed")).toInt(0) > 0
            ? QStringLiteral("failed")
            : QStringLiteral("completed")},
        {QStringLiteral("actionsRequested"), batchResult.value(QStringLiteral("actionsRequested")).toInt()},
        {QStringLiteral("actionsCompleted"), batchResult.value(QStringLiteral("actionsCompleted")).toInt()},
        {QStringLiteral("failed"), batchResult.value(QStringLiteral("failed")).toInt()},
        {QStringLiteral("executionStats"), batchResult.value(QStringLiteral("executionStats")).toObject()},
        {QStringLiteral("results"), batchResult.value(QStringLiteral("results")).toArray()},
        {QStringLiteral("postconditions"), batchResult.value(QStringLiteral("postconditions")).toArray()},
        {QStringLiteral("rollback"), batchResult.value(QStringLiteral("rollback")).toObject()},
    });
    while (m_executionHistory.size() > 100) {
        m_executionHistory.removeFirst();
    }

    for (const QJsonValue& value : batchResult.value(QStringLiteral("results")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonObject response = entry.value(QStringLiteral("response")).toObject();
        const QString method = entry.value(QStringLiteral("tool")).toString();
        if (!response.isEmpty()) {
            ingestFactsFromResult(method, response.value(QStringLiteral("result")).toObject(response));
        }
    }
}

bool DrawingContextStore::hasSnapshot() const
{
    return !m_layers.isEmpty()
        || !m_entities.isEmpty()
        || !m_bimObjects.isEmpty()
        || !m_selection.isEmpty()
        || !m_rawBlocks.isEmpty();
}

QJsonObject DrawingContextStore::manifest() const
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.drawing.manifest.v1")},
        {QStringLiteral("revision"), m_revision},
        {QStringLiteral("updatedAtUtc"), m_updatedAtUtc},
        {QStringLiteral("snapshotAvailable"), hasSnapshot()},
        {QStringLiteral("layerCount"), m_layers.size()},
        {QStringLiteral("entityCount"), m_entities.size()},
        {QStringLiteral("bimObjectCount"), m_bimObjects.size()},
        {QStringLiteral("selectionCount"), m_selection.size()},
        {QStringLiteral("factCount"), m_normalizedFacts.size()},
        {QStringLiteral("capabilityBlockCount"), m_capabilityBlocks.size()},
        {QStringLiteral("executionCount"), m_executionHistory.size()},
        {QStringLiteral("validationCount"), m_validationHistory.size()},
        {QStringLiteral("rawBlockCount"), m_rawBlocks.size()},
        {QStringLiteral("dirty"), !m_dirtyRegions.isEmpty()},
        {QStringLiteral("dirtyRegions"), limitedArray(m_dirtyRegions, 8)},
        {QStringLiteral("recentActions"), limitedArray(m_actionSummaries, 6)},
        {QStringLiteral("policy"), QStringLiteral("Manifest ist lokal in Qt gespeichert. Fuer Details qt.drawing.query oder qt.drawing.fullScan verwenden.")},
    };
}

QJsonObject DrawingContextStore::contextManifest() const
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.context.manifest.v1")},
        {QStringLiteral("source"), QStringLiteral("DrawingContextStore")},
        {QStringLiteral("revision"), m_revision},
        {QStringLiteral("updatedAtUtc"), m_updatedAtUtc},
        {QStringLiteral("available"), QJsonObject{
            {QStringLiteral("document"), m_documentBlocks.size()},
            {QStringLiteral("capabilities"), m_capabilityBlocks.size()},
            {QStringLiteral("layers"), m_layers.size()},
            {QStringLiteral("entities"), m_entities.size()},
            {QStringLiteral("bimObjects"), m_bimObjects.size()},
            {QStringLiteral("selection"), m_selection.size()},
            {QStringLiteral("facts"), m_normalizedFacts.size()},
            {QStringLiteral("rawBlocks"), m_rawBlocks.size()},
            {QStringLiteral("validations"), m_validationHistory.size()},
            {QStringLiteral("executions"), m_executionHistory.size()},
            {QStringLiteral("workflowRepair"), m_workflowRepairHistory.size()},
        }},
        {QStringLiteral("fetchDomains"), QJsonArray{
            QStringLiteral("document"),
            QStringLiteral("capabilities"),
            QStringLiteral("layers"),
            QStringLiteral("entities"),
            QStringLiteral("bim"),
            QStringLiteral("selection"),
            QStringLiteral("facts"),
            QStringLiteral("raw"),
            QStringLiteral("validation"),
            QStringLiteral("execution"),
            QStringLiteral("workflowRepair"),
        }},
        {QStringLiteral("policy"), QStringLiteral("Voller BRX-Kontext liegt lokal vor und wird seitenweise per qt.brx.context.fetch oder qt.brx.db.fullContext abgerufen; nicht ungefiltert in den Initialprompt senden.")},
    };
}

QJsonObject DrawingContextStore::fetch(const QJsonObject& params) const
{
    const QString domain = params.value(QStringLiteral("domain")).toString(QStringLiteral("facts")).trimmed();
    const int offset = std::max(0, params.value(QStringLiteral("offset")).toInt(0));
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(50), 1, 500);
    QJsonArray source;
    if (domain == QStringLiteral("document")) source = m_documentBlocks;
    else if (domain == QStringLiteral("capabilities")) source = m_capabilityBlocks;
    else if (domain == QStringLiteral("layers")) source = m_layers;
    else if (domain == QStringLiteral("entities")) source = m_entities;
    else if (domain == QStringLiteral("bim")) source = m_bimObjects;
    else if (domain == QStringLiteral("selection")) source = m_selection;
    else if (domain == QStringLiteral("raw")) source = m_rawBlocks;
    else if (domain == QStringLiteral("validation")) source = m_validationHistory;
    else if (domain == QStringLiteral("execution")) source = m_executionHistory;
    else if (domain == QStringLiteral("workflowRepair")) source = m_workflowRepairHistory;
    else source = m_normalizedFacts;

    QJsonObject page;
    const QJsonArray items = pagedArray(source, offset, limit, &page);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.context.fetch.result.v1")},
        {QStringLiteral("domain"), domain},
        {QStringLiteral("page"), page},
        {QStringLiteral("items"), items},
    };
}

QJsonObject DrawingContextStore::query(const QJsonObject& params) const
{
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(100), 1, 500);
    const QJsonObject filters = params.value(QStringLiteral("filters")).toObject(params);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.drawing.query.result.v1")},
        {QStringLiteral("manifest"), manifest()},
        {QStringLiteral("params"), params},
        {QStringLiteral("layers"), limitedArray(m_layers, limit)},
        {QStringLiteral("entities"), filteredFacts(m_entities, filters, limit)},
        {QStringLiteral("bimObjects"), filteredFacts(m_bimObjects, filters, limit)},
        {QStringLiteral("selection"), filteredFacts(m_selection, filters, limit)},
        {QStringLiteral("facts"), factsMatching(filters, limit)},
        {QStringLiteral("complete"), m_rawBlocks.isEmpty() ? hasSnapshot() : true},
    };
}

QJsonObject DrawingContextStore::inspect(const QJsonObject& params) const
{
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(100), 1, 500);
    const QJsonObject filters = params.value(QStringLiteral("filters")).toObject(params);
    const QJsonArray facts = factsMatching(filters, limit);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.inspect.result.v1")},
        {QStringLiteral("params"), params},
        {QStringLiteral("manifest"), contextManifest()},
        {QStringLiteral("count"), facts.size()},
        {QStringLiteral("facts"), facts},
        {QStringLiteral("selection"), filteredFacts(m_selection, filters, limit)},
        {QStringLiteral("policy"), QStringLiteral("Diese Facts stammen aus dem promptlokalen Store. Wenn count=0 oder stale, gezielt BRX read-only nachladen.")},
    };
}

QJsonObject DrawingContextStore::fullScan(const QJsonObject& params) const
{
    const int blockLimit = std::clamp(params.value(QStringLiteral("blockLimit")).toInt(12), 1, 100);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.drawing.full-scan.result.v1")},
        {QStringLiteral("manifest"), manifest()},
        {QStringLiteral("blocks"), limitedArray(m_rawBlocks, blockLimit)},
        {QStringLiteral("layers"), limitedArray(m_layers, 500)},
        {QStringLiteral("entities"), limitedArray(m_entities, 500)},
        {QStringLiteral("bimObjects"), limitedArray(m_bimObjects, 500)},
        {QStringLiteral("selection"), limitedArray(m_selection, 200)},
        {QStringLiteral("complete"), hasSnapshot()},
        {QStringLiteral("policy"), QStringLiteral("Dies ist der aktuell lokal verfuegbare DrawingContextStore. Wenn complete=false, zuerst BRX read-only Kontext wie layers.list/geometry.query/bim.objects.query anfordern.")},
    };
}

QJsonObject DrawingContextStore::fullContext(const QJsonObject& params) const
{
    const int offset = std::max(0, params.value(QStringLiteral("offset")).toInt(0));
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(100), 1, 500);
    QJsonObject page;
    const QJsonArray facts = pagedArray(m_normalizedFacts, offset, limit, &page);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.db.full-context.result.v1")},
        {QStringLiteral("manifest"), contextManifest()},
        {QStringLiteral("page"), page},
        {QStringLiteral("facts"), facts},
        {QStringLiteral("capabilities"), limitedArray(m_capabilityBlocks, 20)},
        {QStringLiteral("recentExecution"), limitedArray(m_executionHistory, 20)},
        {QStringLiteral("policy"), QStringLiteral("Vollkontext wird seitenweise ausgeliefert; weitere Seiten ueber offset/limit abrufen.")},
    };
}

QJsonObject DrawingContextStore::executionHistory(const QJsonObject& params) const
{
    const int offset = std::max(0, params.value(QStringLiteral("offset")).toInt(0));
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(20), 1, 100);
    QJsonObject page;
    const QJsonArray executions = pagedArray(m_executionHistory, offset, limit, &page);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.execution.history.v1")},
        {QStringLiteral("page"), page},
        {QStringLiteral("validations"), limitedArray(m_validationHistory, limit)},
        {QStringLiteral("executions"), executions},
        {QStringLiteral("dirtyRegions"), limitedArray(m_dirtyRegions, limit)},
    };
}

QJsonObject DrawingContextStore::repairContext(const QJsonObject& params) const
{
    const int limit = std::clamp(params.value(QStringLiteral("limit")).toInt(50), 1, 200);
    const QJsonObject filters = params.value(QStringLiteral("filters")).toObject(params);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("barebone.qt.brx.workflow.repair-context.v1")},
        {QStringLiteral("params"), params},
        {QStringLiteral("manifest"), contextManifest()},
        {QStringLiteral("facts"), factsMatching(filters, limit)},
        {QStringLiteral("recentValidations"), limitedArray(m_validationHistory, 12)},
        {QStringLiteral("recentExecutions"), limitedArray(m_executionHistory, 12)},
        {QStringLiteral("recentRepair"), limitedArray(m_workflowRepairHistory, 12)},
        {QStringLiteral("policy"), QStringLiteral("Nutze diesen Kontext, um einen anderen validierbaren Toolpfad vorzuschlagen; geschuetzte Workflows nicht ueberschreiben.")},
    };
}

QJsonObject DrawingContextStore::agentContext() const
{
    return QJsonObject{
        {QStringLiteral("manifest"), manifest()},
        {QStringLiteral("availableLocalMethods"), QJsonArray{
            QStringLiteral("qt.drawing.manifest"),
            QStringLiteral("qt.drawing.query"),
            QStringLiteral("qt.drawing.fullScan"),
            QStringLiteral("qt.brx.context.manifest"),
            QStringLiteral("qt.brx.context.fetch"),
            QStringLiteral("qt.brx.db.inspect"),
            QStringLiteral("qt.brx.db.fullContext"),
            QStringLiteral("qt.brx.execution.history"),
            QStringLiteral("qt.brx.workflow.repairContext"),
        }},
    };
}

QJsonArray DrawingContextStore::limitedArray(const QJsonArray& values, int limit) const
{
    QJsonArray result;
    for (int i = 0; i < values.size() && i < limit; ++i) {
        result.append(values.at(i));
    }
    if (values.size() > limit) {
        result.append(QJsonObject{
            {QStringLiteral("truncated"), true},
            {QStringLiteral("remaining"), values.size() - limit},
        });
    }
    return result;
}

QJsonArray DrawingContextStore::pagedArray(const QJsonArray& values, int offset, int limit, QJsonObject* pageInfo) const
{
    QJsonArray result;
    const int safeOffset = std::clamp(offset, 0, static_cast<int>(values.size()));
    const int safeLimit = std::clamp(limit, 1, 500);
    for (int i = safeOffset; i < values.size() && result.size() < safeLimit; ++i) {
        result.append(values.at(i));
    }
    if (pageInfo != nullptr) {
        *pageInfo = QJsonObject{
            {QStringLiteral("offset"), safeOffset},
            {QStringLiteral("limit"), safeLimit},
            {QStringLiteral("count"), result.size()},
            {QStringLiteral("total"), values.size()},
            {QStringLiteral("nextOffset"), safeOffset + result.size() < values.size()
                ? safeOffset + result.size()
                : -1},
            {QStringLiteral("complete"), safeOffset + result.size() >= values.size()},
        };
    }
    return result;
}

void DrawingContextStore::ingestResult(const QString& method, const QJsonObject& params, const QJsonObject& result, bool ok, const QString& error)
{
    ++m_revision;
    m_updatedAtUtc = nowUtc();

    QJsonObject block{
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
        {QStringLiteral("ok"), ok},
        {QStringLiteral("count"), resultCount(result)},
        {QStringLiteral("at"), m_updatedAtUtc},
    };
    if (!ok && !error.isEmpty()) {
        block.insert(QStringLiteral("error"), error);
    }
    if (ok) {
        block.insert(QStringLiteral("result"), result);
    }

    if (method == QStringLiteral("capabilities.list")
        || method == QStringLiteral("actions.list")
        || method == QStringLiteral("commands.list")) {
        m_capabilityBlocks.append(block);
        while (m_capabilityBlocks.size() > 30) {
            m_capabilityBlocks.removeFirst();
        }
    } else if (method == QStringLiteral("document.status")
        || method == QStringLiteral("document.info")) {
        m_documentBlocks.append(block);
        while (m_documentBlocks.size() > 20) {
            m_documentBlocks.removeFirst();
        }
    }

    m_rawBlocks.append(block);
    while (m_rawBlocks.size() > 500) {
        m_rawBlocks.removeFirst();
    }

    if (!ok) {
        return;
    }

    if (method == QStringLiteral("layers.list")) {
        const QJsonArray layers = firstArray(result, {QStringLiteral("layers"), QStringLiteral("items")});
        if (!layers.isEmpty()) {
            m_layers = layers;
        }
        return;
    }

    if (method == QStringLiteral("geometry.query")
        || method == QStringLiteral("entity.describe")
        || method == QStringLiteral("measurement.bbox")
        || method == QStringLiteral("measurement.length")
        || method == QStringLiteral("measurement.area")) {
        const QJsonArray entities = firstArray(result, {
            QStringLiteral("objects"),
            QStringLiteral("entities"),
            QStringLiteral("items"),
            QStringLiteral("handles"),
        });
        if (!entities.isEmpty()) {
            m_entities = entities;
            ingestFactsFromResult(method, result);
        }
        return;
    }

    if (method == QStringLiteral("bim.objects.query")) {
        const QJsonArray objects = firstArray(result, {
            QStringLiteral("objects"),
            QStringLiteral("bimObjects"),
            QStringLiteral("items"),
        });
        if (!objects.isEmpty()) {
            m_bimObjects = objects;
            ingestFactsFromResult(method, result);
        }
        return;
    }

    if (method == QStringLiteral("selection.describe")
        || method == QStringLiteral("bim.selection.set")) {
        const QJsonArray selection = firstArray(result, {
            QStringLiteral("selection"),
            QStringLiteral("objects"),
            QStringLiteral("entities"),
            QStringLiteral("items"),
        });
        if (!selection.isEmpty()) {
            m_selection = selection;
            ingestFactsFromResult(method, result);
        }
    }
}

void DrawingContextStore::ingestFactsFromResult(const QString& method, const QJsonObject& result)
{
    const bool selected = method == QStringLiteral("selection.describe")
        || method == QStringLiteral("bim.selection.set");
    const QJsonArray values = firstArray(result, {
        QStringLiteral("objects"),
        QStringLiteral("bimObjects"),
        QStringLiteral("entities"),
        QStringLiteral("items"),
    });
    for (const QJsonValue& value : values) {
        const QJsonObject object = value.toObject();
        if (!object.isEmpty()) {
            upsertFact(compactObjectFact(method, object, selected));
        }
    }
}

void DrawingContextStore::upsertFact(const QJsonObject& fact)
{
    const QString handle = fact.value(QStringLiteral("handle")).toString().trimmed();
    if (handle.isEmpty()) {
        m_normalizedFacts.append(fact);
        while (m_normalizedFacts.size() > 5000) {
            m_normalizedFacts.removeFirst();
        }
        return;
    }

    for (int i = 0; i < m_normalizedFacts.size(); ++i) {
        QJsonObject existing = m_normalizedFacts.at(i).toObject();
        if (existing.value(QStringLiteral("handle")).toString().compare(handle, Qt::CaseInsensitive) != 0) {
            continue;
        }

        QJsonArray methods = existing.value(QStringLiteral("sourceMethods")).toArray();
        for (const QJsonValue& method : fact.value(QStringLiteral("sourceMethods")).toArray()) {
            bool seen = false;
            for (const QJsonValue& existingMethod : methods) {
                if (existingMethod.toString() == method.toString()) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                methods.append(method);
            }
        }
        existing = fact;
        existing.insert(QStringLiteral("sourceMethods"), methods);
        m_normalizedFacts.replace(i, existing);
        return;
    }

    m_normalizedFacts.append(fact);
    while (m_normalizedFacts.size() > 5000) {
        m_normalizedFacts.removeFirst();
    }
}

QJsonArray DrawingContextStore::factsMatching(const QJsonObject& filters, int limit) const
{
    return filteredFacts(m_normalizedFacts, filters, limit);
}

QJsonArray DrawingContextStore::filteredFacts(const QJsonArray& source, const QJsonObject& filters, int limit) const
{
    QJsonArray result;
    for (const QJsonValue& value : source) {
        if (result.size() >= limit) {
            break;
        }
        const QJsonObject object = value.toObject();
        if (object.isEmpty() || objectMatchesFilters(object, filters)) {
            result.append(value);
        }
    }
    if (source.size() > result.size() && result.size() >= limit) {
        result.append(QJsonObject{
            {QStringLiteral("truncated"), true},
            {QStringLiteral("sourceCount"), source.size()},
        });
    }
    return result;
}
