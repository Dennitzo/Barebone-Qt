#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QSize>
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

bool englishUi(const ConfigManager& config)
{
    return config.language().trimmed().compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0;
}

QPixmap highDpiLogoPixmap(const QSize& logicalSize)
{
    const QPixmap source(QStringLiteral(":/icons/AppLogo.png"));
    if (source.isNull() || logicalSize.isEmpty()) {
        return source;
    }

    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen ? qMax<qreal>(screen->devicePixelRatio(), 1.0) : 1.0;
    const QSize physicalSize = logicalSize * dpr;

    QPixmap pixmap = source.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

}

MainWindow::MainWindow(ConfigManager& config, QWidget* parent)
    : QMainWindow(parent),
      m_config(config)
{
    buildUi();
    applyStyle();
    retranslateUi();
    QObject::connect(&m_config, &ConfigManager::changed, this, &MainWindow::applyStyle);
    QObject::connect(&m_config, &ConfigManager::changed, this, &MainWindow::retranslateUi);
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
    brandIcon->setPixmap(highDpiLogoPixmap(QSize(34, 34)));

    auto* brandText = new QLabel("Barebone-Qt", brand);
    brandText->setObjectName("brandText");
    brandLayout->addWidget(brandIcon);
    brandLayout->addWidget(brandText, 1);

    auto* nav = new QWidget(sidebarFrame);
    nav->setObjectName("sidebarNav");
    auto* navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(16, 10, 16, 0);
    navLayout->setSpacing(8);

    m_dashboardButton = new QPushButton("AI Assistent", nav);
    m_dashboardButton->setObjectName("sidebarNavButton");
    m_dashboardButton->setCheckable(true);

    m_dropdownButton = new QPushButton("Dropdown", nav);
    m_dropdownButton->setObjectName("sidebarNavButton");
    m_dropdownButton->setCheckable(false);

    m_dropdownContent = new QWidget(nav);
    m_dropdownContent->setObjectName("sidebarDropdownContent");
    auto* dropdownLayout = new QVBoxLayout(m_dropdownContent);
    dropdownLayout->setContentsMargins(14, 0, 0, 0);
    dropdownLayout->setSpacing(8);
    m_templateButton = new QPushButton("Vorlage", m_dropdownContent);
    m_templateButton->setObjectName("sidebarChildNavButton");
    m_templateButton->setCheckable(true);
    m_bricsCadButton = new QPushButton("BricsCAD", m_dropdownContent);
    m_bricsCadButton->setObjectName("sidebarChildNavButton");
    m_bricsCadButton->setCheckable(true);
    dropdownLayout->addWidget(m_templateButton);
    dropdownLayout->addWidget(m_bricsCadButton);
    m_dropdownContent->setVisible(m_config.sidebarDropdownExpanded());

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
    m_dashboard = new ChatPage(m_config, ChatPage::Workspace::Chat, m_pages);
    m_bricsCad = new BricsCadPage(m_config, m_pages);
    m_template = new TemplatePage(TemplatePage::Mode::Vorlage, nullptr, m_pages);
    m_settings = new SettingsPage(m_config, m_pages);
    m_pages->addWidget(m_dashboard);
    m_pages->addWidget(m_template);
    m_pages->addWidget(m_bricsCad);
    m_pages->addWidget(m_settings);

    auto* sidebarSeparator = new QWidget(central);
    sidebarSeparator->setObjectName("sidebarSeparator");
    sidebarSeparator->setFixedWidth(1);

    root->addWidget(sidebarFrame);
    root->addWidget(sidebarSeparator);
    root->addWidget(m_pages, 1);
    setCentralWidget(central);

    const auto selectDashboard = [this]() {
        m_pages->setCurrentWidget(m_dashboard);
        m_dashboardButton->setChecked(true);
        m_templateButton->setChecked(false);
        m_bricsCadButton->setChecked(false);
        m_settingsButton->setChecked(false);
    };
    const auto selectTemplate = [this]() {
        m_pages->setCurrentWidget(m_template);
        m_dashboardButton->setChecked(false);
        m_templateButton->setChecked(true);
        m_bricsCadButton->setChecked(false);
        m_settingsButton->setChecked(false);
    };
    const auto selectBricsCad = [this]() {
        m_pages->setCurrentWidget(m_bricsCad);
        m_dashboardButton->setChecked(false);
        m_templateButton->setChecked(false);
        m_bricsCadButton->setChecked(true);
        m_settingsButton->setChecked(false);
    };
    const auto selectSettings = [this]() {
        m_pages->setCurrentWidget(m_settings);
        m_dashboardButton->setChecked(false);
        m_templateButton->setChecked(false);
        m_bricsCadButton->setChecked(false);
        m_settingsButton->setChecked(true);
    };

    QObject::connect(m_dashboardButton, &QPushButton::clicked, this, selectDashboard);
    QObject::connect(m_dropdownButton, &QPushButton::clicked, this, [this]() {
        const bool expanded = !m_dropdownContent->isVisible();
        m_dropdownContent->setVisible(expanded);
        m_config.setSidebarDropdownExpanded(expanded);
    });
    QObject::connect(m_templateButton, &QPushButton::clicked, this, selectTemplate);
    QObject::connect(m_bricsCadButton, &QPushButton::clicked, this, selectBricsCad);
    QObject::connect(m_settingsButton, &QPushButton::clicked, this, selectSettings);
    selectDashboard();
}

void MainWindow::retranslateUi()
{
    const bool en = englishUi(m_config);
    setWindowTitle(QStringLiteral("Barebone-Qt"));
    if (m_dashboardButton) {
        m_dashboardButton->setText(en ? QStringLiteral("AI Assistant") : QStringLiteral("AI Assistent"));
    }
    if (m_dropdownButton) {
        m_dropdownButton->setText(en ? QStringLiteral("More") : QStringLiteral("Dropdown"));
    }
    if (m_templateButton) {
        m_templateButton->setText(en ? QStringLiteral("Template") : QStringLiteral("Vorlage"));
    }
    if (m_bricsCadButton) {
        m_bricsCadButton->setText(QStringLiteral("BricsCAD"));
    }
    if (m_settingsButton) {
        m_settingsButton->setText(en ? QStringLiteral("Settings") : QStringLiteral("Einstellungen"));
    }
    if (m_template) {
        m_template->setLanguage(en ? QStringLiteral("en") : QStringLiteral("de"));
    }
    if (m_settings) {
        m_settings->retranslateUi();
    }
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
            background: #f7f7f4;
            color: #242621;
            font-family: "Helvetica Neue", Arial;
            font-size: 14px;
        }
        QLabel#pageTitle {
            font-size: 30px;
            font-weight: 700;
            color: #242621;
        }
        QWidget#sidebar {
            background: #ffffff;
            border: none;
        }
        QWidget#sidebarSeparator {
            background: #d8d8d3;
        }
        QWidget#brand, QWidget#brand QLabel {
            background: #ffffff;
        }
        QLabel#brandText {
            font-size: 22px;
            font-weight: 800;
            color: #242621;
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
            border: 1px solid #d8d8d3;
            border-radius: 12px;
            background: transparent;
            color: #666b61;
            font-weight: 700;
        }
        QPushButton#sidebarNavButton:hover, QPushButton#sidebarChildNavButton:hover {
            background: #e8eddf;
            border: 1px solid #cbd3c0;
        }
        QPushButton#sidebarNavButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #ffffff;
        }
        QPushButton#sidebarChildNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 28px;
            border: 1px solid #d8d8d3;
            border-radius: 12px;
            background: transparent;
            color: #666b61;
            font-weight: 700;
        }
        QPushButton#sidebarChildNavButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #ffffff;
        }
        QPushButton#sidebarSettingsButton {
            margin: 14px 16px 18px 16px;
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: 1px solid #d8d8d3;
            border-radius: 12px;
            background: #eef1ea;
            color: #343a30;
            font-weight: 700;
        }
        QPushButton#sidebarSettingsButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #ffffff;
        }
        QWidget#metricCard {
            background: #ffffff;
            border: 1px solid #8fbd45;
            border-radius: 18px;
        }
        QWidget#metricCard QLabel, QWidget#metricCard QWidget {
            background: transparent;
        }
        QLabel#cardBodyText {
            color: #74786f;
            line-height: 130%;
        }
        QLabel#settingsSectionTitle {
            color: #242621;
            font-size: 16px;
            font-weight: 700;
        }
        QWidget#dashboardGroup {
            background: transparent;
            border: 1px solid #d8d8d3;
            border-radius: 18px;
        }
        QWidget#dashboardAgentSection {
            background: transparent;
        }
        QWidget#agentPanel {
            background: #fafaf7;
            border: 1px solid #d8d8d3;
            border-radius: 18px;
        }
        QWidget#agentPanel QLabel, QWidget#agentPanel QWidget {
            background: transparent;
        }
        QLabel#agentTitle {
            color: #242621;
            font-size: 18px;
            font-weight: 800;
        }
        QLabel#agentStatusPill {
            color: #74786f;
            background: #eef1ea;
            border: 1px solid #d0d4c8;
            border-radius: 10px;
            padding: 4px 10px;
            font-size: 12px;
            font-weight: 800;
        }
        QLabel#agentStatusPill[state="thinking"] {
            color: #4f7f1a;
            background: #edf6e4;
            border: 1px solid #73a22d;
        }
        QTextBrowser#agentChatView {
            background: #ffffff;
            border: 1px solid #dcdcd5;
            border-radius: 8px;
            padding: 14px;
        }
        QWidget#agentComposer {
            background: #ffffff;
            border: 1px solid #d0d4c8;
            border-radius: 14px;
        }
        QLineEdit#agentInput {
            border: none;
            background: transparent;
            min-height: 42px;
            padding: 0 4px;
        }
        QWidget#agentProposalPanel {
            background: #ffffff;
            border: 1px solid #dcdcd5;
            border-radius: 14px;
        }
        QLabel#agentProposalTitle {
            color: #242621;
            font-size: 14px;
            font-weight: 800;
        }
        QLabel#agentProposalDetails {
            color: #74786f;
            line-height: 130%;
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
            color: #74786f;
            font-weight: 600;
        }
        QPushButton {
            min-height: 36px;
            border-radius: 11px;
            border: none;
            background: #eef1ea;
            color: #242621;
            padding: 0 16px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: #e8eddf;
        }
        QPushButton#primaryButton {
            border: none;
            background: #8fbd45;
            color: #ffffff;
        }
        QPushButton#secondaryButton {
            background: #eef1ea;
            border: 1px solid #cbd3c0;
            color: #242621;
        }
        QPushButton#secondaryButton:disabled {
            background: #f2f3ef;
            border: 1px solid #ddddd8;
            color: #9a9998;
        }
        QPushButton#serviceActionButton {
            background: #edf6e4;
            border: 1px solid #8fbd45;
            color: #242621;
        }
        QPushButton#serviceActionButton:hover {
            background: #dfefd0;
            border: 1px solid #8fbd45;
        }
        QPushButton#serviceActionButton:disabled {
            background: #f2f3ef;
            border: 1px solid #a9c978;
            color: #9a9998;
        }
        QLineEdit, QComboBox, QDoubleSpinBox {
            min-height: 36px;
            border: 1px solid #dcdcd5;
            border-radius: 11px;
            background: #ffffff;
            color: #242621;
            padding: 0 11px;
            selection-background-color: #8fbd45;
            selection-color: #ffffff;
        }
        QComboBox QAbstractItemView {
            background: #ffffff;
            color: #242621;
            border: 1px solid #dcdcd5;
            selection-background-color: #8fbd45;
            selection-color: #ffffff;
            outline: 0;
        }
        QListView#settingsComboPopup {
            background: #ffffff;
            color: #242621;
            border: 1px solid #dcdcd5;
            outline: 0;
        }
        QListView#settingsComboPopup::item {
            min-height: 28px;
            padding: 6px 10px;
            background: #ffffff;
            color: #242621;
        }
        QListView#settingsComboPopup::item:hover {
            background: #edf6e4;
            color: #242621;
        }
        QListView#settingsComboPopup::item:selected {
            background: #8fbd45;
            color: #ffffff;
        }
        QPlainTextEdit#logView {
            background: #ffffff;
            border: 1px solid #dcdcd5;
            border-radius: 11px;
            padding: 10px;
            font-family: Consolas, monospace;
            font-size: 12px;
        }
        QTableWidget#templateTable {
            background: #ffffff;
            border: 1px solid #dcdcd5;
            border-radius: 11px;
            gridline-color: #dcdcd5;
            selection-background-color: #8fbd45;
            selection-color: #ffffff;
        }
        QHeaderView::section {
            background: #eef1ea;
            border: none;
            border-right: 1px solid #d8d8d3;
            padding: 7px 8px;
            font-weight: 700;
            color: #343a30;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QScrollArea QWidget {
            background: transparent;
        }
        QProgressBar {
            border: 1px solid #cbd3c0;
            border-radius: 6px;
            background: #dfe4d8;
        }
        QProgressBar::chunk {
            border-radius: 6px;
            background: #8fbd45;
        }
        QSlider::groove:horizontal {
            height: 8px;
            border-radius: 4px;
            background: #dfe4d8;
        }
        QSlider::handle:horizontal {
            width: 18px;
            height: 18px;
            margin: -5px 0;
            border-radius: 9px;
            background: #8fbd45;
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
            background: #171717;
            color: #f4f3f3;
            font-family: "Helvetica Neue", Arial;
            font-size: 14px;
        }
        QLabel#pageTitle {
            font-size: 30px;
            font-weight: 700;
            color: #f4f3f3;
        }
        QWidget#sidebar {
            background: #171717;
            border: none;
        }
        QWidget#sidebarSeparator {
            background: #565656;
        }
        QWidget#brand, QWidget#brand QLabel {
            background: #171717;
        }
        QLabel#brandText {
            font-size: 22px;
            font-weight: 800;
            color: #f4f3f3;
        }
        QLabel#brandIcon {
            background: transparent;
        }
        QWidget#sidebarNav, QWidget#sidebarDropdownContent {
            background: #171717;
        }
        QPushButton#sidebarNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: 1px solid #2f2f2f;
            border-radius: 12px;
            background: transparent;
            color: #b0b0b0;
            font-weight: 700;
        }
        QPushButton#sidebarNavButton:hover, QPushButton#sidebarChildNavButton:hover {
            background: #242424;
            border: 1px solid #3a3a3a;
        }
        QPushButton#sidebarNavButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #101410;
        }
        QPushButton#sidebarChildNavButton {
            min-height: 42px;
            text-align: left;
            padding-left: 28px;
            border: 1px solid #2f2f2f;
            border-radius: 12px;
            background: transparent;
            color: #b0b0b0;
            font-weight: 700;
        }
        QPushButton#sidebarChildNavButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #101410;
        }
        QPushButton#sidebarSettingsButton {
            margin: 14px 16px 18px 16px;
            min-height: 42px;
            text-align: left;
            padding-left: 14px;
            border: 1px solid #2f2f2f;
            border-radius: 12px;
            background: transparent;
            color: #dddddd;
            font-weight: 700;
        }
        QPushButton#sidebarSettingsButton:checked {
            background: #8fbd45;
            border: 1px solid #8fbd45;
            color: #101410;
        }
        QWidget#metricCard {
            background: #1e1e1e;
            border: 1px solid #8fbd45;
            border-radius: 18px;
        }
        QWidget#metricCard QLabel, QWidget#metricCard QWidget {
            background: transparent;
        }
        QLabel#cardBodyText {
            color: #b0b0b0;
            line-height: 130%;
        }
        QLabel#settingsSectionTitle {
            color: #f4f3f3;
            font-size: 16px;
            font-weight: 700;
        }
        QWidget#dashboardGroup {
            background: transparent;
            border: 1px solid #333333;
            border-radius: 18px;
        }
        QWidget#dashboardAgentSection {
            background: transparent;
        }
        QWidget#agentPanel {
            background: #171717;
            border: 1px solid #333333;
            border-radius: 18px;
        }
        QWidget#agentPanel QLabel, QWidget#agentPanel QWidget {
            background: transparent;
        }
        QLabel#agentTitle {
            color: #f4f3f3;
            font-size: 18px;
            font-weight: 800;
        }
        QLabel#agentStatusPill {
            color: #b0b0b0;
            background: #1f1f1f;
            border: 1px solid #333333;
            border-radius: 10px;
            padding: 4px 10px;
            font-size: 12px;
            font-weight: 800;
        }
        QLabel#agentStatusPill[state="thinking"] {
            color: #f4f3f3;
            background: #1f1f1f;
            border: 1px solid #8fbd45;
        }
        QTextBrowser#agentChatView {
            background: #171717;
            border: 1px solid #333333;
            border-radius: 8px;
            color: #f4f3f3;
            padding: 14px;
        }
        QWidget#agentComposer {
            background: #202020;
            border: 1px solid #3a3a3a;
            border-radius: 14px;
        }
        QLineEdit#agentInput {
            border: none;
            background: transparent;
            min-height: 42px;
            color: #f4f3f3;
            padding: 0 4px;
        }
        QWidget#agentProposalPanel {
            background: #202020;
            border: 1px solid #3a3a3a;
            border-radius: 14px;
        }
        QLabel#agentProposalTitle {
            color: #f4f3f3;
            font-size: 14px;
            font-weight: 800;
        }
        QLabel#agentProposalDetails {
            color: #b0b0b0;
            line-height: 130%;
        }
        QWidget#settingsSection {
            background: #202020;
            border: none;
            border-radius: 18px;
        }
        QWidget#settingsSection QLabel, QWidget#settingsSection QWidget {
            background: transparent;
        }
        QLabel#settingsFieldLabel {
            color: #b0b0b0;
            font-weight: 600;
        }
        QPushButton {
            min-height: 36px;
            border-radius: 11px;
            border: none;
            background: #2a2a2a;
            color: #f4f3f3;
            padding: 0 16px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: #333333;
        }
        QPushButton#primaryButton {
            background: #8fbd45;
            color: #101410;
        }
        QPushButton#secondaryButton {
            background: #2a2a2a;
            border: 1px solid #454545;
            color: #f4f3f3;
        }
        QPushButton#secondaryButton:disabled {
            background: #202020;
            border: 1px solid #333333;
            color: #777777;
        }
        QPushButton#serviceActionButton {
            background: #2a2a2a;
            border: 1px solid #8fbd45;
            color: #f4f3f3;
        }
        QPushButton#serviceActionButton:hover {
            background: #333333;
            border: 1px solid #8fbd45;
        }
        QPushButton#serviceActionButton:disabled {
            background: #202020;
            border: 1px solid #4f5f36;
            color: #777777;
        }
        QLineEdit, QComboBox, QDoubleSpinBox {
            min-height: 36px;
            border: 1px solid #333333;
            border-radius: 11px;
            background: #171717;
            color: #f4f3f3;
            padding: 0 11px;
            selection-background-color: #8fbd45;
            selection-color: #101410;
        }
        QComboBox QAbstractItemView {
            background: #171717;
            color: #f4f3f3;
            border: 1px solid #333333;
            selection-background-color: #8fbd45;
            selection-color: #101410;
            outline: 0;
        }
        QListView#settingsComboPopup {
            background: #171717;
            color: #f4f3f3;
            border: 1px solid #333333;
            outline: 0;
        }
        QListView#settingsComboPopup::item {
            min-height: 28px;
            padding: 6px 10px;
            background: #171717;
            color: #f4f3f3;
        }
        QListView#settingsComboPopup::item:hover {
            background: #2a2a2a;
            color: #f4f3f3;
        }
        QListView#settingsComboPopup::item:selected {
            background: #8fbd45;
            color: #101410;
        }
        QPlainTextEdit#logView {
            background: #171717;
            border: 1px solid #333333;
            border-radius: 11px;
            color: #f4f3f3;
            padding: 10px;
            font-family: Consolas, monospace;
            font-size: 12px;
        }
        QTableWidget#templateTable {
            background: #171717;
            border: 1px solid #333333;
            border-radius: 11px;
            color: #f4f3f3;
            gridline-color: #333333;
            selection-background-color: #8fbd45;
            selection-color: #101410;
        }
        QHeaderView::section {
            background: #2a2a2a;
            border: none;
            border-right: 1px solid #333333;
            padding: 7px 8px;
            font-weight: 700;
            color: #dddddd;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QScrollArea QWidget {
            background: transparent;
        }
        QProgressBar {
            border: 1px solid #4a4a4a;
            border-radius: 6px;
            background: #2a2a2a;
        }
        QProgressBar::chunk {
            border-radius: 6px;
            background: #8fbd45;
        }
        QSlider::groove:horizontal {
            height: 8px;
            border-radius: 4px;
            background: #2a2a2a;
        }
        QSlider::handle:horizontal {
            width: 18px;
            height: 18px;
            margin: -5px 0;
            border-radius: 9px;
            background: #8fbd45;
        }
        QCheckBox {
            spacing: 9px;
        }
    )QSS";
}
