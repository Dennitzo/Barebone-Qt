#pragma once

#include "../core/ConfigManager.h"

#include <QComboBox>
#include <QWidget>

class SettingsPage final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(ConfigManager& config, QWidget* parent = nullptr);

private:
    ConfigManager& m_config;
    QComboBox* m_theme = nullptr;
    QComboBox* m_language = nullptr;
};
