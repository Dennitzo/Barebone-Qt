#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

class BrxAgent {
public:
    static QJsonArray buildToolCatalog(const QJsonObject& capabilities);
    static QJsonArray runtimeToolsWithSdkTools(const QJsonArray& runtimeTools);
    static QJsonArray selectToolsForRoute(
        const QJsonArray& catalog,
        const QJsonObject& route,
        const QString& prompt,
        const QJsonObject& workflowContext = {});
    static QJsonArray compactToolIndex(const QJsonArray& tools);
    static QJsonArray toolsByNames(const QJsonArray& tools, const QStringList& names);
    static QStringList toolNames(const QJsonArray& tools);
    static QJsonObject describeTools(const QJsonArray& tools, const QJsonObject& params);
    static QJsonObject repairToolContext(const QJsonArray& tools, const QJsonObject& params);
    static QJsonArray localContextMethods();

    static QJsonObject dbSchema();
    static QJsonObject sdkCatalog(const QJsonObject& params = {});
    static QJsonObject dbCompatibility(const QJsonObject& params);
    static QJsonObject workflowTestPlan(const QJsonObject& params);
    static QJsonObject workflowRepairHints(const QJsonObject& params);
    static QJsonObject workflowRepairHints(const QJsonObject& params, const QJsonObject& repairContext);
    static QJsonObject dbContextFromStoreResult(const QString& method, const QJsonObject& params, const QJsonObject& storeResult);
};
