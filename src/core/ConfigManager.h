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
    QString aiBaseUrl() const;
    QString aiModel() const;
    QByteArray windowGeometry() const;

    void setTheme(const QString& theme);
    void setLanguage(const QString& language);
    void setAiBaseUrl(const QString& baseUrl);
    void setAiModel(const QString& model);
    void setWindowGeometry(const QByteArray& geometry);

Q_SIGNALS:
    void changed();

private:
    QSettings m_settings;
};
