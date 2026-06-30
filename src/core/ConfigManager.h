#pragma once

#include <QObject>
#include <QByteArray>
#include <QSettings>
#include <QString>

class ConfigManager final : public QObject {
    Q_OBJECT

public:
    explicit ConfigManager(QObject* parent = nullptr);

    QString theme() const;
    QString language() const;
    QString aiProvider() const;
    QString aiBaseUrl() const;
    QString aiModel() const;
    QString aiApiKey() const;
    QString aiReasoningEffort() const;
    QString unifiedAiAssistantState() const;
    bool sidebarDropdownExpanded() const;
    QByteArray windowGeometry() const;

    void setTheme(const QString& theme);
    void setLanguage(const QString& language);
    void setAiProvider(const QString& provider);
    void setAiBaseUrl(const QString& baseUrl);
    void setAiModel(const QString& model);
    void setAiApiKey(const QString& apiKey);
    void setAiReasoningEffort(const QString& effort);
    void setUnifiedAiAssistantState(const QString& stateJson);
    void setSidebarDropdownExpanded(bool expanded);
    void setWindowGeometry(const QByteArray& geometry);

Q_SIGNALS:
    void changed();

private:
    QSettings m_settings;
};
