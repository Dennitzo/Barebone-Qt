#include "SettingsPage.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QSignalBlocker>
#include <QVBoxLayout>

SettingsPage::SettingsPage(ConfigManager& config, QWidget* parent)
    : QWidget(parent),
      m_config(config)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(20);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName("pageTitle");
    root->addWidget(m_titleLabel);

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

    m_aiProvider = new QComboBox(section);
    m_aiProvider->addItem("Lokale AI", "local");
    m_aiProvider->addItem("Offizielle ChatGPT API", "official");

    m_aiBaseUrl = new QLineEdit(m_config.aiBaseUrl(), section);

    m_aiModel = new QComboBox(section);
    m_aiModel->setEditable(true);
    m_aiModel->setInsertPolicy(QComboBox::NoInsert);
    m_aiModel->addItem("openai/gpt-oss-20b", "openai/gpt-oss-20b");
    m_aiModel->addItem("openai/gpt-oss-120b", "openai/gpt-oss-120b");
    m_aiModel->addItem("google/gemma-4-26b-a4b-qat", "google/gemma-4-26b-a4b-qat");

    const auto installComboPopup = [](QComboBox* combo) {
        auto* view = new QListView(combo);
        view->setObjectName("settingsComboPopup");
        view->setUniformItemSizes(true);
        combo->setView(view);
    };
    installComboPopup(m_theme);
    installComboPopup(m_language);
    installComboPopup(m_aiProvider);
    installComboPopup(m_aiModel);

    m_aiApiKey = new QLineEdit(section);
    m_aiApiKey->setEchoMode(QLineEdit::Password);
    m_aiApiKey->setPlaceholderText("sk-...");

    m_themeLabel = new QLabel(section);
    m_themeLabel->setObjectName("settingsFieldLabel");
    m_languageLabel = new QLabel(section);
    m_languageLabel->setObjectName("settingsFieldLabel");
    m_aiProviderLabel = new QLabel(section);
    m_aiProviderLabel->setObjectName("settingsFieldLabel");
    m_aiBaseUrlLabel = new QLabel(section);
    m_aiBaseUrlLabel->setObjectName("settingsFieldLabel");
    m_aiModelLabel = new QLabel(section);
    m_aiModelLabel->setObjectName("settingsFieldLabel");
    m_aiApiKeyLabel = new QLabel("Secret Key", section);
    m_aiApiKeyLabel->setObjectName("settingsFieldLabel");
    form->addRow(m_themeLabel, m_theme);
    form->addRow(m_languageLabel, m_language);
    form->addRow(m_aiProviderLabel, m_aiProvider);
    form->addRow(m_aiBaseUrlLabel, m_aiBaseUrl);
    form->addRow(m_aiModelLabel, m_aiModel);
    form->addRow(m_aiApiKeyLabel, m_aiApiKey);

    m_saveButton = new QPushButton("Speichern", section);
    m_saveButton->setObjectName("serviceActionButton");
    m_saveStatus = new QLabel(section);
    m_saveStatus->setObjectName("cardBodyText");

    auto* actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 8, 0, 0);
    actionRow->setSpacing(14);
    actionRow->addWidget(m_saveButton);
    actionRow->addWidget(m_saveStatus, 1);
    form->addRow(QString(), actionRow);

    root->addWidget(section);
    root->addStretch();
    retranslateUi();
    loadSettings();

    QObject::connect(m_saveButton, &QPushButton::clicked, this, [this]() {
        saveSettings();
    });
    QObject::connect(m_aiProvider, &QComboBox::currentIndexChanged, this, [this]() {
        updateAiProviderUi();
    });
}

void SettingsPage::loadSettings()
{
    const QSignalBlocker themeBlocker(m_theme);
    const QSignalBlocker languageBlocker(m_language);
    const QSignalBlocker providerBlocker(m_aiProvider);
    const QSignalBlocker baseUrlBlocker(m_aiBaseUrl);
    const QSignalBlocker modelBlocker(m_aiModel);
    const QSignalBlocker apiKeyBlocker(m_aiApiKey);

    const int themeIndex = m_theme->findData(m_config.theme());
    m_theme->setCurrentIndex(themeIndex >= 0 ? themeIndex : 0);

    const int languageIndex = m_language->findData(m_config.language());
    m_language->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);

    const int providerIndex = m_aiProvider->findData(m_config.aiProvider());
    m_aiProvider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    updateAiProviderUi();

    m_aiBaseUrl->setText(m_config.aiBaseUrl());
    const int modelIndex = m_aiModel->findData(m_config.aiModel());
    if (modelIndex >= 0) {
        m_aiModel->setCurrentIndex(modelIndex);
    } else {
        m_aiModel->setCurrentText(m_config.aiModel());
    }
    m_aiApiKey->setText(m_config.aiApiKey());
}

void SettingsPage::saveSettings()
{
    m_config.setTheme(m_theme->currentData().toString());
    m_config.setLanguage(m_language->currentData().toString());
    m_config.setAiProvider(m_aiProvider->currentData().toString());
    m_config.setAiBaseUrl(m_aiBaseUrl->text());
    m_config.setAiModel(m_aiModel->currentText());
    if (m_aiProvider->currentData().toString() == "official") {
        m_config.setAiApiKey(m_aiApiKey->text());
    }
    loadSettings();

    if (m_saveStatus) {
        m_saveStatus->setText((isEnglish() ? QStringLiteral("Saved: %1") : QStringLiteral("Gespeichert: %1")).arg(m_config.aiModel()));
    }
}

bool SettingsPage::isEnglish() const
{
    return m_config.language().trimmed().compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0;
}

void SettingsPage::retranslateUi()
{
    const bool en = isEnglish();
    if (m_titleLabel) {
        m_titleLabel->setText(en ? QStringLiteral("Settings") : QStringLiteral("Einstellungen"));
    }
    if (m_themeLabel) {
        m_themeLabel->setText(en ? QStringLiteral("Theme") : QStringLiteral("Theme"));
    }
    if (m_languageLabel) {
        m_languageLabel->setText(en ? QStringLiteral("Language") : QStringLiteral("Sprache"));
    }
    if (m_aiProviderLabel) {
        m_aiProviderLabel->setText(en ? QStringLiteral("AI provider") : QStringLiteral("AI Anbieter"));
    }
    if (m_aiBaseUrlLabel) {
        m_aiBaseUrlLabel->setText(en ? QStringLiteral("AI server URL") : QStringLiteral("AI Server URL"));
    }
    if (m_aiModelLabel) {
        m_aiModelLabel->setText(en ? QStringLiteral("AI model") : QStringLiteral("AI Modell"));
    }
    if (m_aiApiKeyLabel) {
        m_aiApiKeyLabel->setText(en ? QStringLiteral("Secret key") : QStringLiteral("Secret Key"));
    }
    if (m_saveButton) {
        m_saveButton->setText(en ? QStringLiteral("Save") : QStringLiteral("Speichern"));
    }
    const auto setTextForData = [](QComboBox* combo, const QString& data, const QString& text) {
        if (!combo) {
            return;
        }
        const int index = combo->findData(data);
        if (index >= 0) {
            combo->setItemText(index, text);
        }
    };
    setTextForData(m_theme, QStringLiteral("system"), en ? QStringLiteral("System") : QStringLiteral("System"));
    setTextForData(m_theme, QStringLiteral("light"), en ? QStringLiteral("Light") : QStringLiteral("Hell"));
    setTextForData(m_theme, QStringLiteral("dark"), en ? QStringLiteral("Dark") : QStringLiteral("Dunkel"));
    setTextForData(m_language, QStringLiteral("de"), en ? QStringLiteral("German") : QStringLiteral("Deutsch"));
    setTextForData(m_language, QStringLiteral("en"), en ? QStringLiteral("English") : QStringLiteral("Englisch"));
    setTextForData(m_aiProvider, QStringLiteral("local"), en ? QStringLiteral("Local AI") : QStringLiteral("Lokale AI"));
    setTextForData(m_aiProvider, QStringLiteral("official"), en ? QStringLiteral("Official ChatGPT API") : QStringLiteral("Offizielle ChatGPT API"));
    updateAiProviderUi();
}

void SettingsPage::updateAiProviderUi()
{
    const QString provider = m_aiProvider->currentData().toString();
    const bool official = provider != "local";
    const QString currentModel = m_aiModel->currentText().trimmed();
    const bool currentLooksLocal = currentModel == "openai/gpt-oss-20b"
        || currentModel == "openai/gpt-oss-120b"
        || currentModel == "google/gemma-4-26b-a4b-qat";
    const bool currentLooksOfficial = currentModel == "gpt-5.5"
        || currentModel == "gpt-5.4"
        || currentModel == "gpt-4.1";

    m_aiModel->clear();
    if (official) {
        m_aiModel->addItem("gpt-5.5", "gpt-5.5");
        m_aiModel->addItem("gpt-5.4", "gpt-5.4");
        m_aiModel->addItem("gpt-4.1", "gpt-4.1");
        m_aiBaseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
        if (m_aiBaseUrl->text().trimmed().isEmpty()
            || m_aiBaseUrl->text().trimmed() == "http://192.168.0.67:1234/v1") {
            m_aiBaseUrl->setText("https://api.openai.com/v1");
        }
        m_aiApiKeyLabel->setVisible(true);
        m_aiApiKey->setVisible(true);
        m_aiApiKey->setEnabled(true);
    } else {
        m_aiModel->addItem("openai/gpt-oss-20b", "openai/gpt-oss-20b");
        m_aiModel->addItem("openai/gpt-oss-120b", "openai/gpt-oss-120b");
        m_aiModel->addItem("google/gemma-4-26b-a4b-qat", "google/gemma-4-26b-a4b-qat");
        m_aiBaseUrl->setPlaceholderText(QStringLiteral("http://192.168.0.67:1234/v1"));
        if (m_aiBaseUrl->text().trimmed().isEmpty()
            || m_aiBaseUrl->text().trimmed() == "https://api.openai.com/v1") {
            m_aiBaseUrl->setText("http://192.168.0.67:1234/v1");
        }
        m_aiApiKeyLabel->setVisible(false);
        m_aiApiKey->setVisible(false);
        m_aiApiKey->setEnabled(false);
    }

    const int modelIndex = m_aiModel->findData(currentModel);
    if (modelIndex >= 0) {
        m_aiModel->setCurrentIndex(modelIndex);
    } else if (!currentModel.isEmpty() && !(official && currentLooksLocal) && !(!official && currentLooksOfficial)) {
        m_aiModel->setCurrentText(currentModel);
    }
}
