#include "SettingsPage.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

SettingsPage::SettingsPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent),
      m_config(config)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(20);

    auto* title = new QLabel("Einstellungen", this);
    title->setObjectName("pageTitle");
    root->addWidget(title);

    auto* section = new QWidget(this);
    section->setObjectName("settingsSection");
    auto* form = new QFormLayout(section);
    form->setContentsMargins(22, 20, 22, 20);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(14);

    m_theme = new QComboBox(section);
    m_theme->addItem("System", "system");
    m_theme->addItem("Hell", "light");
    m_theme->addItem("Dunkel", "dark");

    m_language = new QComboBox(section);
    m_language->addItem("Deutsch", "de");
    m_language->addItem("English", "en");

    m_aiBaseUrl = new QLineEdit(m_config.aiBaseUrl(), section);
    m_aiBaseUrl->setPlaceholderText("http://192.168.0.67:1234/v1");

    m_aiModel = new QComboBox(section);
    m_aiModel->addItem("openai/gpt-oss-20b", "openai/gpt-oss-20b");
    m_aiModel->addItem("openai/gpt-oss-120b", "openai/gpt-oss-120b");
    m_aiModel->addItem("google/gemma-4-26b-a4b-qat", "google/gemma-4-26b-a4b-qat");

    auto* themeLabel = new QLabel("Theme", section);
    themeLabel->setObjectName("settingsFieldLabel");
    auto* languageLabel = new QLabel("Sprache", section);
    languageLabel->setObjectName("settingsFieldLabel");
    auto* aiBaseUrlLabel = new QLabel("AI Server URL", section);
    aiBaseUrlLabel->setObjectName("settingsFieldLabel");
    auto* aiModelLabel = new QLabel("AI Modell", section);
    aiModelLabel->setObjectName("settingsFieldLabel");
    form->addRow(themeLabel, m_theme);
    form->addRow(languageLabel, m_language);
    form->addRow(aiBaseUrlLabel, m_aiBaseUrl);
    form->addRow(aiModelLabel, m_aiModel);

    m_saveButton = new QPushButton("Speichern", section);
    m_saveButton->setObjectName("serviceActionButton");
    m_saveStatus = new QLabel(section);
    m_saveStatus->setObjectName("cardBodyText");

    auto* actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 8, 0, 0);
    actionRow->addWidget(m_saveButton);
    actionRow->addWidget(m_saveStatus, 1);
    form->addRow(QString(), actionRow);

    root->addWidget(section);
    root->addStretch();
    loadSettings();

    QObject::connect(m_saveButton, &QPushButton::clicked, this, [this]() {
        saveSettings();
    });
}

void SettingsPage::loadSettings()
{
    const QSignalBlocker themeBlocker(m_theme);
    const QSignalBlocker languageBlocker(m_language);
    const QSignalBlocker baseUrlBlocker(m_aiBaseUrl);
    const QSignalBlocker modelBlocker(m_aiModel);

    const int themeIndex = m_theme->findData(m_config.theme());
    m_theme->setCurrentIndex(themeIndex >= 0 ? themeIndex : 0);

    const int languageIndex = m_language->findData(m_config.language());
    m_language->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);

    m_aiBaseUrl->setText(m_config.aiBaseUrl());
    const int modelIndex = m_aiModel->findData(m_config.aiModel());
    m_aiModel->setCurrentIndex(modelIndex >= 0 ? modelIndex : m_aiModel->findData("openai/gpt-oss-20b"));
}

void SettingsPage::saveSettings()
{
    m_config.setTheme(m_theme->currentData().toString());
    m_config.setLanguage(m_language->currentData().toString());
    m_config.setAiBaseUrl(m_aiBaseUrl->text());
    m_config.setAiModel(m_aiModel->currentData().toString());
    loadSettings();

    if (m_saveStatus) {
        m_saveStatus->setText(QString("Gespeichert: %1").arg(m_config.aiModel()));
    }
}
