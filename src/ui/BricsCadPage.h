#pragma once

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QWidget>

class BricsCadPage final : public QWidget {
    Q_OBJECT

public:
    explicit BricsCadPage(QWidget* parent = nullptr);

private:
    void startBridgeServer();
    void appendBridgeLog(const QString& message);
    QByteArray handleBridgeRequest(const QByteArray& request);
    void refreshLayers();
    void extrudeSelectedLayer();
    QByteArray sendPluginRequest(const QByteArray& request, int timeoutMs = 15000);
    void setPluginStatus(const QString& message, bool connected);

    QTcpServer* m_bridgeServer = nullptr;
    QLabel* m_pluginStatus = nullptr;
    QComboBox* m_layerCombo = nullptr;
    QDoubleSpinBox* m_heightInput = nullptr;
    QPushButton* m_refreshLayersButton = nullptr;
    QPushButton* m_extrudeButton = nullptr;
    QLabel* m_bridgeStatus = nullptr;
    QPlainTextEdit* m_bridgeLog = nullptr;
};
