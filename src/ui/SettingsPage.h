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
    void retranslateUi();

private:
    void loadSettings();
    void saveSettings();
    void updateAiProviderUi();
    bool isEnglish() const;

    ConfigManager& m_config;
    QComboBox* m_theme = nullptr;
    QComboBox* m_language = nullptr;
    QComboBox* m_aiProvider = nullptr;
    QLineEdit* m_aiBaseUrl = nullptr;
    QComboBox* m_aiModel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_themeLabel = nullptr;
    QLabel* m_languageLabel = nullptr;
    QLabel* m_aiProviderLabel = nullptr;
    QLabel* m_aiBaseUrlLabel = nullptr;
    QLabel* m_aiModelLabel = nullptr;
    QLabel* m_aiApiKeyLabel = nullptr;
    QLineEdit* m_aiApiKey = nullptr;
    QPushButton* m_saveButton = nullptr;
    QLabel* m_saveStatus = nullptr;
};
