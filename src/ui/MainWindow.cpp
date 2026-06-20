#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QVBoxLayout>

namespace {

bool effectiveDarkTheme(const QString& theme)
{
    const QString normalized = theme.toLower();
    if (normalized == "dark") {
        return true;
    }
    if (normalized == "light") {
        return false;
    }
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

}

MainWindow::MainWindow(ConfigManager& config, QWidget* parent)
    : QMainWindow(parent),
      m_config(config)
{
    buildUi();
    applyStyle();
    QObject::connect(&m_config, &ConfigManager::changed, this, &MainWindow::applyStyle);
}

void MainWindow::buildUi()
{
    setWindowTitle("Barebone-Qt");
    resize(1320, 860);
    setMinimumSize(1040, 700);
    if (!m_config.windowGeometry().isEmpty()) {
        restoreGeometry(m_config.windowGeometry());
    }

    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* sidebarFrame = new QWidget(central);
    sidebarFrame->setFixedWidth(232);
    sidebarFrame->setObjectName("sidebar");
    auto* sidebarLayout = new QVBoxLayout(sidebarFrame);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    auto* brand = new QWidget(sidebarFrame);
    brand->setObjectName("brand");
    auto* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(22, 24, 22, 16);
    brandLayout->setSpacing(10);

    auto* brandIcon = new QLabel(brand);
    brandIcon->setObjectName("brandIcon");
    brandIcon->setFixedSize(34, 34);
    brandIcon->setPixmap(QPixmap(":/icons/Bitcoin.png").scaled(34, 34, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    auto* brandText = new QLabel("Barebone-Qt", brand);
    brandText->setObjectName("brandText");
    brandLayout->addWidget(brandIcon);
    brandLayout->addWidget(brandText, 1);

    auto* nav = new QWidget(sidebarFrame);
    nav->setObjectName("sidebarNav");
    auto* navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(16, 10, 16, 0);
    navLayout->setSpacing(8);

    m_dashboardButton = new QPushButton("Dashboard", nav);
    m_dashboardButton->setObjectName("sidebarNavButton");
    m_dashboardButton->setCheckable(true);

    m_dropdownButton = new QPushButton("Dropdown", nav);
    m_dropdownButton->setObjectName("sidebarNavButton");
    m_dropdownButton->setCheckable(false);

    m_dropdownContent = new QWidget(nav);
    m_dropdownContent->setObjectName("sidebarDropdownContent");
    auto* dropdownLayout = new QVBoxLayout(m_dropdownContent);
    dropdownLayout->setContentsMargins(0, 0, 0, 0);
    dropdownLayout->setSpacing(8);
    m_bricsCadButton = new QPushButton("BricsCAD", m_dropdownContent);
    m_bricsCadButton->setObjectName("sidebarChildNavButton");
    m_bricsCadButton->setCheckable(true);
    dropdownLayout->addWidget(m_bricsCadButton);
    m_dropdownContent->setVisible(false);

    navLayout->addWidget(m_dashboardButton);
    navLayout->addWidget(m_dropdownButton);
    navLayout->addWidget(m_dropdownContent);
    navLayout->addStretch();

    m_settingsButton = new QPushButton("Einstellungen", sidebarFrame);
    m_settingsButton->setObjectName("sidebarSettingsButton");
    m_settingsButton->setCheckable(true);

    sidebarLayout->addWidget(brand);
    sidebarLayout->addWidget(nav, 1);
    sidebarLayout->addWidget(m_settingsButton);

    m_pages = new QStackedWidget(central);
    m_template = new TemplatePage(m_pages);
    m_bricsCad = new BricsCadPage(m_pages);
    m_settings = new SettingsPage(m_config, m_pages);
    m_pages->addWidget(m_template);
    m_pages->addWidget(m_bricsCad);
    m_pages->addWidget(m_settings);

    root->addWidget(sidebarFrame);
    root->addWidget(m_pages, 1);
    setCentralWidget(central);

    const auto selectDashboard = [this]() {
        m_pages->setCurrentWidget(m_template);
        m_dashboardButton->setChecked(true);
        m_bricsCadButton->setChecked(false);
        m_settingsButton->setChecked(false);
    };
    const auto selectBricsCad = [this]() {
        m_pages->setCurrentWidget(m_bricsCad);
        m_dashboardButton->setChecked(false);
        m_bricsCadButton->setChecked(true);
        m_settingsButton->setChecked(false);
    };
    const auto selectSettings = [this]() {
        m_pages->setCurrentWidget(m_settings);
        m_dashboardButton->setChecked(false);
        m_bricsCadButton->setChecked(false);
        m_settingsButton->setChecked(true);
    };

    QObject::connect(m_dashboardButton, &QPushButton::clicked, this, selectDashboard);
    QObject::connect(m_dropdownButton, &QPushButton::clicked, this, [this]() {
        m_dropdownContent->setVisible(!m_dropdownContent->isVisible());
    });
    QObject::connect(m_bricsCadButton, &QPushButton::clicked, this, selectBricsCad);
    QObject::connect(m_settingsButton, &QPushButton::clicked, this, selectSettings);
    selectDashboard();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_config.setWindowGeometry(saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::applyStyle()
{
    setStyleSheet(effectiveDarkTheme(m_config.theme()) ? darkStyle() : lightStyle());
}

QString MainWindow::lightStyle() const
{
    return R"QSS(
        QMainWindow, QWidget {
            background: #f6f7fb;
            color: #1d1d1f;
            font-family: "Helvetica Neue", Arial;
            font-size: 14px;
        }
        QLabel#pageTitle {
            font-size: 30px;
            font-weight: 700;
            color: #1d1d1f;
        }
        QWidget#sidebar {
            background: #ffffff;
            border: none;
        }
        QWidget#brand, QWidget#brand QLabel {
            background: #ffffff;
        }
        QLabel#brandText {
            font-size: 22px;
            font-weight: 800;
            color: #1d1d1f;
        }
        QLabel#brandIcon {
            background: transparent;
        }
        QWidget#sidebarNav, QWidget#sidebarDropdownContent {
            background: #ffffff;
        }
        QPushButton#sidebarNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: none;
            border-radius: 12px;
            background: transparent;
            color: #5f6673;
            font-weight: 700;
        }
        QPushButton#sidebarNavButton:hover, QPushButton#sidebarChildNavButton:hover {
            background: #e5eaf2;
        }
        QPushButton#sidebarNavButton:checked {
            background: #111827;
            color: #ffffff;
        }
        QPushButton#sidebarChildNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 28px;
            border: none;
            border-radius: 12px;
            background: transparent;
            color: #5f6673;
            font-weight: 700;
        }
        QPushButton#sidebarChildNavButton:checked {
            background: #111827;
            color: #ffffff;
        }
        QPushButton#sidebarSettingsButton {
            margin: 14px 16px 18px 16px;
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: none;
            border-radius: 12px;
            background: #f0f3f8;
            color: #303746;
            font-weight: 700;
        }
        QPushButton#sidebarSettingsButton:checked {
            background: #111827;
            color: #ffffff;
        }
        QWidget#metricCard {
            background: #ffffff;
            border: none;
            border-radius: 18px;
        }
        QWidget#metricCard QLabel, QWidget#metricCard QWidget {
            background: transparent;
        }
        QLabel#cardBodyText {
            color: #687386;
            line-height: 130%;
        }
        QLabel#settingsSectionTitle {
            color: #1d1d1f;
            font-size: 16px;
            font-weight: 700;
        }
        QWidget#dashboardGroup {
            background: transparent;
            border: 1px solid #dde4ef;
            border-radius: 18px;
        }
        QWidget#settingsSection {
            background: #ffffff;
            border: none;
            border-radius: 18px;
        }
        QWidget#settingsSection QLabel, QWidget#settingsSection QWidget {
            background: transparent;
        }
        QLabel#settingsFieldLabel {
            color: #687386;
            font-weight: 600;
        }
        QPushButton {
            min-height: 36px;
            border-radius: 11px;
            border: none;
            background: #eef1f6;
            color: #1d1d1f;
            padding: 0 16px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: #e5eaf2;
        }
        QPushButton#primaryButton {
            border: none;
            background: #111827;
            color: #ffffff;
        }
        QPushButton#secondaryButton:disabled {
            background: #f4f6fa;
            border: 1px solid #e1e6ef;
            color: #a4adbb;
        }
        QLineEdit, QComboBox {
            min-height: 36px;
            border: 1px solid #e2e7ef;
            border-radius: 11px;
            background: #ffffff;
            padding: 0 11px;
            selection-background-color: #111827;
        }
        QPlainTextEdit#logView {
            background: #ffffff;
            border: 1px solid #e2e7ef;
            border-radius: 11px;
            padding: 10px;
            font-family: Consolas, monospace;
            font-size: 12px;
        }
        QTableWidget#templateTable {
            background: #ffffff;
            border: 1px solid #e2e7ef;
            border-radius: 11px;
            gridline-color: #e2e7ef;
            selection-background-color: #111827;
            selection-color: #ffffff;
        }
        QHeaderView::section {
            background: #eef1f6;
            border: none;
            border-right: 1px solid #dde4ef;
            padding: 7px 8px;
            font-weight: 700;
            color: #303746;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QScrollArea QWidget {
            background: transparent;
        }
        QProgressBar {
            border: 1px solid #cfd7e3;
            border-radius: 6px;
            background: #e4e8f0;
        }
        QProgressBar::chunk {
            border-radius: 6px;
            background: #f7931a;
        }
        QSlider::groove:horizontal {
            height: 8px;
            border-radius: 4px;
            background: #e4e8f0;
        }
        QSlider::handle:horizontal {
            width: 18px;
            height: 18px;
            margin: -5px 0;
            border-radius: 9px;
            background: #111827;
        }
        QCheckBox {
            spacing: 9px;
        }
    )QSS";
}

QString MainWindow::darkStyle() const
{
    return R"QSS(
        QMainWindow, QWidget {
            background: #0f1117;
            color: #f5f7fb;
            font-family: "Helvetica Neue", Arial;
            font-size: 14px;
        }
        QLabel#pageTitle {
            font-size: 30px;
            font-weight: 700;
            color: #f5f7fb;
        }
        QWidget#sidebar {
            background: #151922;
            border: none;
        }
        QWidget#brand, QWidget#brand QLabel {
            background: #151922;
        }
        QLabel#brandText {
            font-size: 22px;
            font-weight: 800;
            color: #f5f7fb;
        }
        QLabel#brandIcon {
            background: transparent;
        }
        QWidget#sidebarNav, QWidget#sidebarDropdownContent {
            background: #151922;
        }
        QPushButton#sidebarNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: none;
            border-radius: 12px;
            background: transparent;
            color: #aab2c0;
            font-weight: 700;
        }
        QPushButton#sidebarNavButton:hover, QPushButton#sidebarChildNavButton:hover {
            background: #30394a;
        }
        QPushButton#sidebarNavButton:checked {
            background: #f5f7fb;
            color: #111827;
        }
        QPushButton#sidebarChildNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 28px;
            border: none;
            border-radius: 12px;
            background: transparent;
            color: #aab2c0;
            font-weight: 700;
        }
        QPushButton#sidebarChildNavButton:checked {
            background: #f5f7fb;
            color: #111827;
        }
        QPushButton#sidebarSettingsButton {
            margin: 14px 16px 18px 16px;
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: none;
            border-radius: 12px;
            background: #202633;
            color: #d7dce5;
            font-weight: 700;
        }
        QPushButton#sidebarSettingsButton:checked {
            background: #f5f7fb;
            color: #111827;
        }
        QWidget#metricCard {
            background: #191e29;
            border: none;
            border-radius: 18px;
        }
        QWidget#metricCard QLabel, QWidget#metricCard QWidget {
            background: transparent;
        }
        QLabel#cardBodyText {
            color: #aab2c0;
            line-height: 130%;
        }
        QLabel#settingsSectionTitle {
            color: #f5f7fb;
            font-size: 16px;
            font-weight: 700;
        }
        QWidget#dashboardGroup {
            background: transparent;
            border: 1px solid #303746;
            border-radius: 18px;
        }
        QWidget#settingsSection {
            background: #191e29;
            border: none;
            border-radius: 18px;
        }
        QWidget#settingsSection QLabel, QWidget#settingsSection QWidget {
            background: transparent;
        }
        QLabel#settingsFieldLabel {
            color: #aab2c0;
            font-weight: 600;
        }
        QPushButton {
            min-height: 36px;
            border-radius: 11px;
            border: none;
            background: #262d3a;
            color: #f5f7fb;
            padding: 0 16px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: #30394a;
        }
        QPushButton#primaryButton {
            background: #f5f7fb;
            color: #111827;
        }
        QPushButton#secondaryButton:disabled {
            background: #1d2330;
            border: 1px solid #2a3241;
            color: #687386;
        }
        QLineEdit, QComboBox {
            min-height: 36px;
            border: 1px solid #303746;
            border-radius: 11px;
            background: #11151e;
            color: #f5f7fb;
            padding: 0 11px;
            selection-background-color: #f5f7fb;
            selection-color: #111827;
        }
        QPlainTextEdit#logView {
            background: #11151e;
            border: 1px solid #303746;
            border-radius: 11px;
            color: #f5f7fb;
            padding: 10px;
            font-family: Consolas, monospace;
            font-size: 12px;
        }
        QTableWidget#templateTable {
            background: #11151e;
            border: 1px solid #303746;
            border-radius: 11px;
            color: #f5f7fb;
            gridline-color: #303746;
            selection-background-color: #f5f7fb;
            selection-color: #111827;
        }
        QHeaderView::section {
            background: #262d3a;
            border: none;
            border-right: 1px solid #303746;
            padding: 7px 8px;
            font-weight: 700;
            color: #d7dce5;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QScrollArea QWidget {
            background: transparent;
        }
        QProgressBar {
            border: 1px solid #4b5563;
            border-radius: 6px;
            background: #262d3a;
        }
        QProgressBar::chunk {
            border-radius: 6px;
            background: #f7931a;
        }
        QSlider::groove:horizontal {
            height: 8px;
            border-radius: 4px;
            background: #262d3a;
        }
        QSlider::handle:horizontal {
            width: 18px;
            height: 18px;
            margin: -5px 0;
            border-radius: 9px;
            background: #f5f7fb;
        }
        QCheckBox {
            spacing: 9px;
        }
    )QSS";
}
