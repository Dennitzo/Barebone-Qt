#include "ConfigManager.h"

namespace {

constexpr const char* kDefaultAiModel = "openai/gpt-oss-20b";

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

QString ConfigManager::aiBaseUrl() const
{
    return m_settings.value("ai/baseUrl", "http://192.168.0.67:1234/v1").toString();
}

QString ConfigManager::aiModel() const
{
    return m_settings.value("ai/model", kDefaultAiModel).toString();
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

void ConfigManager::setAiBaseUrl(const QString& baseUrl)
{
    const QString value = baseUrl.trimmed().isEmpty()
        ? QStringLiteral("http://192.168.0.67:1234/v1")
        : baseUrl.trimmed();
    m_settings.setValue("ai/baseUrl", value);
    Q_EMIT changed();
}

void ConfigManager::setAiModel(const QString& model)
{
    const QString value = model.trimmed().isEmpty()
        ? QString::fromLatin1(kDefaultAiModel)
        : model.trimmed();
    m_settings.setValue("ai/model", value);
    Q_EMIT changed();
}

void ConfigManager::setWindowGeometry(const QByteArray& geometry)
{
    m_settings.setValue("app/windowGeometry", geometry);
}
