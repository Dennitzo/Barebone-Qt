#include "SettingsPage.h"

#include <QFormLayout>
#include <QLabel>
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
    m_theme->setCurrentIndex(m_theme->findData(m_config.theme()));

    m_language = new QComboBox(section);
    m_language->addItem("Deutsch", "de");
    m_language->addItem("English", "en");
    m_language->setCurrentIndex(m_language->findData(m_config.language()));

    auto* themeLabel = new QLabel("Theme", section);
    themeLabel->setObjectName("settingsFieldLabel");
    auto* languageLabel = new QLabel("Sprache", section);
    languageLabel->setObjectName("settingsFieldLabel");
    form->addRow(themeLabel, m_theme);
    form->addRow(languageLabel, m_language);

    root->addWidget(section);
    root->addStretch();

    QObject::connect(m_theme, &QComboBox::currentIndexChanged, this, [this]() {
        m_config.setTheme(m_theme->currentData().toString());
    });
    QObject::connect(m_language, &QComboBox::currentIndexChanged, this, [this]() {
        m_config.setLanguage(m_language->currentData().toString());
    });
}
