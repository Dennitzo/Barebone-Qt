#include "TemplatePage.h"

#include <QColor>
#include <QGraphicsDropShadowEffect>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QLabel* metricLabel(const QString& title, const QString& value, QWidget* parent)
{
    auto* label = new QLabel(QString("<span style='color:#8a93a3;font-size:12px;font-weight:700;letter-spacing:0'>%1</span><br><b style='font-size:26px'>%2</b>").arg(title, value), parent);
    label->setMinimumHeight(72);
    return label;
}

QWidget* wrapCard(QWidget* content, QWidget* parent, int height = 138)
{
    auto* card = new QWidget(parent);
    card->setObjectName("metricCard");
    card->setFixedHeight(height);
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setColor(QColor(16, 24, 40, 24));
    shadow->setOffset(0, 10);
    card->setGraphicsEffect(shadow);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(content);
    return card;
}

}

TemplatePage::TemplatePage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(20);

    auto* title = new QLabel("Dashboard", this);
    title->setObjectName("pageTitle");
    root->addWidget(title);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setObjectName("templateScroll");
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(20);

    QGridLayout* metricsGrid = nullptr;
    auto* metricsGroup = createGroup("Kachelgruppe", &metricsGrid);
    metricsGrid->addWidget(createMetricCard("Status", "Online", 9100), 0, 0);
    metricsGrid->addWidget(createMetricCard("Fortschritt", "84.20 %", 8420), 0, 1);
    metricsGrid->addWidget(createMetricCard("Auslastung", "61.75 %", 6175), 0, 2);
    metricsGrid->addWidget(createMetricCard("Speicher", "248 GB", 5100), 0, 3);
    metricsGrid->addWidget(createMetricCard("Eintraege", "9", -1), 0, 4);
    metricsGrid->setColumnStretch(0, 1);
    metricsGrid->setColumnStretch(1, 1);
    metricsGrid->setColumnStretch(2, 1);
    metricsGrid->setColumnStretch(3, 2);
    metricsGrid->setColumnStretch(4, 1);
    contentLayout->addWidget(metricsGroup);

    contentLayout->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

QWidget* TemplatePage::createMetricCard(const QString& title, const QString& value, int progress)
{
    auto* wrapper = new QWidget(this);
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(metricLabel(title, value, wrapper));
    if (progress >= 0) {
        auto* bar = new QProgressBar(wrapper);
        bar->setRange(0, 10000);
        bar->setValue(progress);
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        layout->addWidget(bar);
    }
    return wrapCard(wrapper, this);
}

QWidget* TemplatePage::createGroup(const QString& title, QGridLayout** gridTarget)
{
    auto* group = new QWidget(this);
    group->setObjectName("dashboardGroup");
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* label = new QLabel(title, group);
    label->setObjectName("settingsSectionTitle");
    layout->addWidget(label);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(18);
    *gridTarget = grid;
    layout->addLayout(grid);
    return group;
}
