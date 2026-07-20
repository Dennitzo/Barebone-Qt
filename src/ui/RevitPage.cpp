#include "RevitPage.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

RevitPage::RevitPage(ConfigManager& config, RevitAgent& agent, QWidget* parent)
    : QWidget(parent)
{
    Q_UNUSED(config)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Logs"), this);
    title->setObjectName(QStringLiteral("settingsSectionTitle"));

    m_bridgeLog = new QPlainTextEdit(this);
    m_bridgeLog->setObjectName(QStringLiteral("logView"));
    m_bridgeLog->setReadOnly(true);

    layout->addWidget(title);
    layout->addWidget(m_bridgeLog, 1);

    for (const QString& line : agent.recentLogLines()) {
        appendBridgeLog(line);
    }

    QObject::connect(&agent, &RevitAgent::bridgeLogAdded, this, &RevitPage::appendBridgeLog);
}

void RevitPage::appendBridgeLog(const QString& message)
{
    if (m_bridgeLog) {
        m_bridgeLog->appendPlainText(message);
    }
}
