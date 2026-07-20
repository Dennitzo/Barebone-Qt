#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace BricsCadAgentUtils {

QString normalizedSearchText(QString text);
QString workflowSlug(QString text);
bool textMentionsAny(const QString& normalizedText, const QStringList& needles);
QStringList stringsFromJsonArray(const QJsonArray& values, int maxCount = 0);
QJsonArray stringsToJsonArray(const QStringList& values);
QStringList toolNamesForLog(const QJsonArray& tools, int maxCount = 24);
QStringList routeWorkflowIds(const QJsonObject& route, int maxCount = 3);
bool promptExplicitlyRequestsWorkflowExecution(const QString& prompt);

bool capabilityMethodAvailable(const QJsonObject& method);
bool capabilitiesContainMethod(const QJsonObject& capabilities, const QString& methodName);
bool shouldPrefetchSelectionDescription(const QString& prompt);
QString bridgeErrorMessage(const QJsonObject& response, const QString& fallback);
QJsonObject compactBrxResponseForAgent(const QJsonObject& response);

QJsonArray workflowDisplaySteps(const QJsonObject& workflow);
QStringList workflowToolNames(const QJsonObject& workflow, int maxCount = 12);
QJsonArray workflowStepSummary(const QJsonObject& workflow, int maxCount = 8);
QJsonObject workflowCapsule(const QJsonObject& workflow, bool detailed);
QStringList localWorkflowSelection(const QJsonArray& compactWorkflows, const QString& prompt, int maxCount = 3);

} // namespace BricsCadAgentUtils
