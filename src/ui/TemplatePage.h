#pragma once

#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QWidget>

class TemplatePage final : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Dashboard,
        Vorlage,
    };

    explicit TemplatePage(Mode mode, QWidget* agentWidget = nullptr, QWidget* parent = nullptr);

private:
    QWidget* createMetricCard(const QString& title, const QString& value, int progress = -1);
    QWidget* createGroup(const QString& title, QGridLayout** gridTarget);
};
