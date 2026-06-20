#include "ConfigManager.h"

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

void ConfigManager::setWindowGeometry(const QByteArray& geometry)
{
    m_settings.setValue("app/windowGeometry", geometry);
}
