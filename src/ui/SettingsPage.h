#pragma once

#include "../core/ConfigManager.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class SettingsPage final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(ConfigManager& config, QWidget* parent = nullptr);

private:
    void loadSettings();
    void saveSettings();

    ConfigManager& m_config;
    QComboBox* m_theme = nullptr;
    QComboBox* m_language = nullptr;
    QLineEdit* m_aiBaseUrl = nullptr;
    QComboBox* m_aiModel = nullptr;
    QPushButton* m_saveButton = nullptr;
    QLabel* m_saveStatus = nullptr;
};
