#pragma once

#include "../core/ConfigManager.h"

#include "BricsCadPage.h"
#include "ChatPage.h"
#include "SettingsPage.h"
#include "TemplatePage.h"

#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(ConfigManager& config, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    void retranslateUi();
    QString lightStyle() const;
    QString darkStyle() const;

    ConfigManager& m_config;
    QPushButton* m_dashboardButton = nullptr;
    QPushButton* m_dropdownButton = nullptr;
    QPushButton* m_templateButton = nullptr;
    QPushButton* m_bricsCadButton = nullptr;
    QPushButton* m_settingsButton = nullptr;
    QWidget* m_dropdownContent = nullptr;
    QStackedWidget* m_pages = nullptr;
    ChatPage* m_dashboard = nullptr;
    TemplatePage* m_template = nullptr;
    BricsCadPage* m_bricsCad = nullptr;
    SettingsPage* m_settings = nullptr;
};
