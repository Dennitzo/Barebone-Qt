#pragma once

#include "../core/ConfigManager.h"
#include "../revit/RevitAgent.h"

#include <QPlainTextEdit>
#include <QWidget>

class RevitPage final : public QWidget {
    Q_OBJECT

public:
    explicit RevitPage(ConfigManager& config, RevitAgent& agent, QWidget* parent = nullptr);

private:
    void appendBridgeLog(const QString& message);

    QPlainTextEdit* m_bridgeLog = nullptr;
};
