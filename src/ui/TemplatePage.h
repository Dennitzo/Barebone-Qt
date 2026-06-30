#pragma once

#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVector>
#include <QWidget>

class TemplatePage final : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Dashboard,
        Vorlage,
    };

    explicit TemplatePage(Mode mode, QWidget* agentWidget = nullptr, QWidget* parent = nullptr);
    void setLanguage(const QString& language);

private:
    QWidget* createMetricCard(const QString& title, const QString& value, int progress = -1);
    QWidget* createGroup(const QString& title, QGridLayout** gridTarget);

    Mode m_mode = Mode::Vorlage;
    QString m_language = QStringLiteral("de");
    QLabel* m_titleLabel = nullptr;
    QLabel* m_groupLabel = nullptr;
    QVector<QLabel*> m_metricLabels;
};
