#include "ConfigManager.h"


namespace {

constexpr const char* kDefaultAiModel = "openai/gpt-oss-20b";
constexpr const char* kDefaultOfficialAiModel = "gpt-5.5";
constexpr const char* kDefaultLocalAiBaseUrl = "http://192.168.0.67:1234/v1";
constexpr const char* kDefaultOfficialAiBaseUrl = "https://api.openai.com/v1";

QString normalizedReasoningEffort(QString effort)
{
    effort = effort.trimmed().toLower();
    if (effort == "none" || effort == "low" || effort == "medium" || effort == "high") {
        return effort;
    }
    return QStringLiteral("high");
}

} // namespace

ConfigManager::ConfigManager(QObject* parent)
    : QObject(parent),
      m_settings("Barebone-Qt", "Barebone-Qt")
{
}

QString ConfigManager::theme() const
{
    return m_settings.value("app/theme", "system").toString();
}

QString ConfigManager::language() const
{
    return m_settings.value("app/language", "de").toString();
}

QString ConfigManager::aiProvider() const
{
    const QString provider = m_settings.value("ai/provider", "local").toString().trimmed().toLower();
    return provider == "official" ? QStringLiteral("official") : QStringLiteral("local");
}

QString ConfigManager::aiBaseUrl() const
{
    const bool local = aiProvider() == "local";
    const QString fallback = aiProvider() == "local"
        ? QString::fromLatin1(kDefaultLocalAiBaseUrl)
        : QString::fromLatin1(kDefaultOfficialAiBaseUrl);
    const QString value = m_settings.value("ai/baseUrl", fallback).toString().trimmed();
    if (!local && value == QString::fromLatin1(kDefaultLocalAiBaseUrl)) {
        return QString::fromLatin1(kDefaultOfficialAiBaseUrl);
    }
    return value.isEmpty() ? fallback : value;
}

QString ConfigManager::aiModel() const
{
    const bool local = aiProvider() == "local";
    const QString fallback = local
        ? QString::fromLatin1(kDefaultAiModel)
        : QString::fromLatin1(kDefaultOfficialAiModel);
    const QString value = m_settings.value("ai/model", fallback).toString().trimmed();
    if (!local
        && (value == QString::fromLatin1(kDefaultAiModel)
            || value == QStringLiteral("openai/gpt-oss-120b")
            || value == QStringLiteral("google/gemma-4-26b-a4b-qat")
            || value == QStringLiteral("google/gemma-4-31b-qat"))) {
        return QString::fromLatin1(kDefaultOfficialAiModel);
    }
    return value.isEmpty() ? fallback : value;
}

QString ConfigManager::aiApiKey() const
{
    return m_settings.value("ai/apiKey").toString();
}

QString ConfigManager::aiReasoningEffort() const
{
    return normalizedReasoningEffort(m_settings.value("ai/reasoningEffort", "high").toString());
}

QString ConfigManager::unifiedAiAssistantState() const
{
    return m_settings.value("ai/unifiedAssistantState").toString();
}

bool ConfigManager::sidebarDropdownExpanded() const
{
    return m_settings.value("app/sidebarDropdownExpanded", true).toBool();
}

QByteArray ConfigManager::windowGeometry() const
{
    return m_settings.value("app/windowGeometry").toByteArray();
}

void ConfigManager::setTheme(const QString& theme)
{
    m_settings.setValue("app/theme", theme);
    Q_EMIT changed();
}

void ConfigManager::setLanguage(const QString& language)
{
    m_settings.setValue("app/language", language);
    Q_EMIT changed();
}

void ConfigManager::setAiProvider(const QString& provider)
{
    const QString value = provider.trimmed().compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("local")
        : QStringLiteral("official");
    m_settings.setValue("ai/provider", value);
    Q_EMIT changed();
}

void ConfigManager::setAiBaseUrl(const QString& baseUrl)
{
    const QString fallback = aiProvider() == "local"
        ? QString::fromLatin1(kDefaultLocalAiBaseUrl)
        : QString::fromLatin1(kDefaultOfficialAiBaseUrl);
    const QString value = baseUrl.trimmed().isEmpty()
        ? fallback
        : baseUrl.trimmed();
    m_settings.setValue("ai/baseUrl", value);
    Q_EMIT changed();
}

void ConfigManager::setAiModel(const QString& model)
{
    const QString fallback = aiProvider() == "local"
        ? QString::fromLatin1(kDefaultAiModel)
        : QString::fromLatin1(kDefaultOfficialAiModel);
    const QString value = model.trimmed().isEmpty()
        ? fallback
        : model.trimmed();
    m_settings.setValue("ai/model", value);
    Q_EMIT changed();
}

void ConfigManager::setAiApiKey(const QString& apiKey)
{
    m_settings.setValue("ai/apiKey", apiKey.trimmed());
    Q_EMIT changed();
}

void ConfigManager::setAiReasoningEffort(const QString& effort)
{
    m_settings.setValue("ai/reasoningEffort", normalizedReasoningEffort(effort));
    Q_EMIT changed();
}

void ConfigManager::setUnifiedAiAssistantState(const QString& stateJson)
{
    m_settings.setValue("ai/unifiedAssistantState", stateJson);
}

void ConfigManager::setSidebarDropdownExpanded(bool expanded)
{
    m_settings.setValue("app/sidebarDropdownExpanded", expanded);
}

void ConfigManager::setWindowGeometry(const QByteArray& geometry)
{
    m_settings.setValue("app/windowGeometry", geometry);
}
