#include "BricsCadPage.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTcpSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr const char* kBrxSdkRoot = "C:/Program Files/Bricsys/BRXSDK/BRX26.1.05.0";
constexpr const char* kBrxPluginName = "BareboneBrx.brx";
constexpr const char* kBridgeHost = "127.0.0.1";
constexpr quint16 kBridgePort = 47626;
constexpr quint16 kPluginBridgePort = 47627;

QString decodeProtocolValue(const QString& value)
{
    return QUrl::fromPercentEncoding(value.toUtf8());
}

QByteArray encodeProtocolValue(const QString& value)
{
    return QUrl::toPercentEncoding(value);
}

QString responseValue(const QStringList& parts, const QString& key)
{
    const qsizetype index = parts.indexOf(key);
    if (index >= 0 && index + 1 < parts.size()) {
        return parts.at(index + 1);
    }
    return {};
}

void appendProtocolDebugLines(QPlainTextEdit* log, const QStringList& lines)
{
    if (!log) {
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    for (const QString& line : lines) {
        if (line.startsWith("DEBUG\t")) {
            log->appendPlainText(QString("[%1] BRX Debug: %2")
                .arg(stamp, decodeProtocolValue(line.mid(6))));
        }
    }
}

QWidget* wrapCard(QWidget* content, QWidget* parent, int height = 154)
{
    auto* card = new QWidget(parent);
    card->setObjectName("metricCard");
    card->setFixedHeight(height);

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setColor(QColor(16, 24, 40, 24));
    shadow->setOffset(0, 10);
    card->setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(content);
    return card;
}

QLabel* cardHeader(const QString& title, const QString& value, QWidget* parent)
{
    auto* label = new QLabel(
        QString("<span style='color:#8a93a3;font-size:12px;font-weight:700;letter-spacing:0'>%1</span><br><b style='font-size:24px'>%2</b>")
            .arg(title.toHtmlEscaped(), value.toHtmlEscaped()),
        parent);
    label->setMinimumHeight(66);
    return label;
}

QWidget* createCommandCard(QWidget* parent)
{
    const QString loadCommand = QString("APPLOAD %1").arg(kBrxPluginName);

    auto* wrapper = new QWidget(parent);
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* headline = cardHeader("BricsCAD", "BRX laden", wrapper);
    auto* command = new QLineEdit(loadCommand, wrapper);
    command->setReadOnly(true);

    auto* row = new QHBoxLayout();
    auto* copy = new QPushButton("Befehl kopieren", wrapper);
    copy->setObjectName("primaryButton");
    auto* docs = new QPushButton("Doku oeffnen", wrapper);
    row->addWidget(copy);
    row->addWidget(docs);

    layout->addWidget(headline);
    layout->addWidget(command);
    layout->addLayout(row);

    QObject::connect(copy, &QPushButton::clicked, wrapper, [command]() {
        QApplication::clipboard()->setText(command->text());
    });
    QObject::connect(docs, &QPushButton::clicked, wrapper, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(kBrxSdkRoot).filePath("docs/BrxDevRef.chm")));
    });

    return wrapCard(wrapper, parent, 188);
}

} // namespace

BricsCadPage::BricsCadPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(20);

    auto* title = new QLabel("BricsCAD Schnittstelle", this);
    title->setObjectName("pageTitle");
    root->addWidget(title);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setObjectName("templateScroll");

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(20);

    auto* group = new QWidget(content);
    group->setObjectName("dashboardGroup");
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(18, 18, 18, 18);
    groupLayout->setSpacing(14);

    auto* groupTitle = new QLabel("BRX laden", group);
    groupTitle->setObjectName("settingsSectionTitle");
    groupLayout->addWidget(groupTitle);
    groupLayout->addWidget(createCommandCard(group));

    auto* bridgeGroup = new QWidget(content);
    bridgeGroup->setObjectName("dashboardGroup");
    auto* bridgeLayout = new QVBoxLayout(bridgeGroup);
    bridgeLayout->setContentsMargins(18, 18, 18, 18);
    bridgeLayout->setSpacing(14);

    auto* bridgeTitle = new QLabel("Direkte Kommunikation", bridgeGroup);
    bridgeTitle->setObjectName("settingsSectionTitle");
    bridgeLayout->addWidget(bridgeTitle);

    m_pluginStatus = new QLabel("BRX Plugin nicht verbunden", bridgeGroup);
    m_pluginStatus->setObjectName("cardBodyText");
    bridgeLayout->addWidget(m_pluginStatus);

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);

    m_layerCombo = new QComboBox(bridgeGroup);
    m_layerCombo->setMinimumHeight(36);
    m_layerCombo->setEnabled(false);
    form->addRow("Layer", m_layerCombo);

    m_heightInput = new QDoubleSpinBox(bridgeGroup);
    m_heightInput->setRange(0.1, 1000000.0);
    m_heightInput->setDecimals(2);
    m_heightInput->setSingleStep(100.0);
    m_heightInput->setValue(3000.0);
    m_heightInput->setSuffix(" mm");
    form->addRow("Hoehe", m_heightInput);

    bridgeLayout->addLayout(form);

    auto* actionRow = new QHBoxLayout();
    m_refreshLayersButton = new QPushButton("Layer aktualisieren", bridgeGroup);
    m_extrudeButton = new QPushButton("Rechtecke extrudieren", bridgeGroup);
    m_extrudeButton->setObjectName("primaryButton");
    m_extrudeButton->setEnabled(false);
    actionRow->addWidget(m_refreshLayersButton);
    actionRow->addWidget(m_extrudeButton);
    bridgeLayout->addLayout(actionRow);

    m_bridgeStatus = new QLabel("Server startet...", bridgeGroup);
    m_bridgeStatus->setObjectName("cardBodyText");
    bridgeLayout->addWidget(m_bridgeStatus);

    m_bridgeLog = new QPlainTextEdit(bridgeGroup);
    m_bridgeLog->setObjectName("logView");
    m_bridgeLog->setReadOnly(true);
    m_bridgeLog->setMinimumHeight(180);
    bridgeLayout->addWidget(m_bridgeLog);

    contentLayout->addWidget(group);
    contentLayout->addWidget(bridgeGroup);
    contentLayout->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    startBridgeServer();

    QObject::connect(m_refreshLayersButton, &QPushButton::clicked, this, [this]() {
        refreshLayers();
    });
    QObject::connect(m_extrudeButton, &QPushButton::clicked, this, [this]() {
        extrudeSelectedLayer();
    });

    QTimer::singleShot(500, this, [this]() {
        refreshLayers();
    });
}

void BricsCadPage::refreshLayers()
{
    if (!m_refreshLayersButton || !m_layerCombo) {
        return;
    }

    m_refreshLayersButton->setEnabled(false);
    m_extrudeButton->setEnabled(false);
    setPluginStatus("Layer werden vom BRX Plugin geladen...", false);
    appendBridgeLog("Qt -> BRX: LAYERS");

    const QByteArray response = sendPluginRequest("LAYERS\n", 10000);
    const QString responseText = QString::fromUtf8(response).trimmed();
    const QStringList responseLines = responseText.split('\n', Qt::SkipEmptyParts);
    appendProtocolDebugLines(m_bridgeLog, responseLines);

    if (responseText.startsWith("ERROR", Qt::CaseInsensitive)) {
        const QString message = responseLines.value(0).section('\t', 1);
        setPluginStatus(message.isEmpty() ? "BRX Plugin nicht erreichbar" : message, false);
        appendBridgeLog(QString("BRX -> Qt: %1").arg(responseLines.value(0, responseText)));
        m_refreshLayersButton->setEnabled(true);
        return;
    }

    QStringList layers;
    for (const QString& line : responseLines) {
        if (line.startsWith("LAYER\t")) {
            layers.append(decodeProtocolValue(line.mid(6)));
        }
    }

    {
        const QSignalBlocker blocker(m_layerCombo);
        m_layerCombo->clear();
        m_layerCombo->addItems(layers);
    }

    const bool hasLayers = !layers.isEmpty();
    m_layerCombo->setEnabled(hasLayers);
    m_extrudeButton->setEnabled(hasLayers);
    m_refreshLayersButton->setEnabled(true);
    setPluginStatus(
        hasLayers
            ? QString("%1 Layer geladen").arg(layers.size())
            : QString("Keine Layer in der aktiven Zeichnung gefunden"),
        true);
    appendBridgeLog(QString("BRX -> Qt: %1 Layer").arg(layers.size()));
}

void BricsCadPage::extrudeSelectedLayer()
{
    if (!m_layerCombo || m_layerCombo->currentText().isEmpty()) {
        setPluginStatus("Bitte zuerst einen Layer laden und auswaehlen", false);
        return;
    }

    const QString layerName = m_layerCombo->currentText();
    const double height = m_heightInput ? m_heightInput->value() : 3000.0;
    const QByteArray request = QByteArray("EXTRUDE\t")
        + encodeProtocolValue(layerName)
        + "\t"
        + QByteArray::number(height, 'f', 2)
        + "\n";

    m_refreshLayersButton->setEnabled(false);
    m_extrudeButton->setEnabled(false);
    setPluginStatus(QString("Extrudiere Rechtecke auf Layer \"%1\"...").arg(layerName), true);
    appendBridgeLog(QString("Qt -> BRX: EXTRUDE %1 %2 mm").arg(layerName).arg(height, 0, 'f', 2));

    const QByteArray response = sendPluginRequest(request, 30000);
    const QString responseText = QString::fromUtf8(response).trimmed();
    const QStringList responseLines = responseText.split('\n', Qt::SkipEmptyParts);
    const QString summaryLine = responseLines.value(0, responseText);
    appendBridgeLog(QString("BRX -> Qt: %1").arg(summaryLine));
    appendProtocolDebugLines(m_bridgeLog, responseLines);

    if (summaryLine.startsWith("ERROR", Qt::CaseInsensitive)) {
        const QString message = summaryLine.section('\t', 1);
        setPluginStatus(message.isEmpty() ? "Extrusion fehlgeschlagen" : message, false);
    } else {
        const QStringList parts = summaryLine.split('\t');
        const QString found = responseValue(parts, "FOUND");
        const QString extruded = responseValue(parts, "EXTRUDED");
        const QString skipped = responseValue(parts, "SKIPPED");
        const QString errors = responseValue(parts, "ERRORS");
        setPluginStatus(
            QString("%1 von %2 Rechtecken extrudiert, %3 uebersprungen, %4 Fehler")
                .arg(extruded.isEmpty() ? "0" : extruded,
                     found.isEmpty() ? "0" : found,
                     skipped.isEmpty() ? "0" : skipped,
                     errors.isEmpty() ? "0" : errors),
            true);
    }

    m_refreshLayersButton->setEnabled(true);
    m_extrudeButton->setEnabled(m_layerCombo->count() > 0);
}

QByteArray BricsCadPage::sendPluginRequest(const QByteArray& request, int timeoutMs)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, kPluginBridgePort);
    if (!socket.waitForConnected(2000)) {
        return QString("ERROR\tBRX Plugin nicht erreichbar auf %1:%2")
            .arg(kBridgeHost)
            .arg(kPluginBridgePort)
            .toUtf8();
    }

    socket.write(request);
    if (!socket.waitForBytesWritten(2000)) {
        return QString("ERROR\tSenden an BRX Plugin fehlgeschlagen: %1")
            .arg(socket.errorString())
            .toUtf8();
    }

    QByteArray response;
    if (!socket.waitForReadyRead(timeoutMs)) {
        return QString("ERROR\tZeitueberschreitung beim Warten auf BRX Antwort: %1")
            .arg(socket.errorString())
            .toUtf8();
    }

    response.append(socket.readAll());
    while (socket.waitForReadyRead(100)) {
        response.append(socket.readAll());
    }

    return response.isEmpty() ? QByteArray("ERROR\tLeere BRX Antwort") : response;
}

void BricsCadPage::setPluginStatus(const QString& message, bool connected)
{
    if (!m_pluginStatus) {
        return;
    }

    const QString color = connected ? "#1f7a3f" : "#9a3412";
    m_pluginStatus->setText(QString("<span style='color:%1;font-weight:700'>%2</span>")
        .arg(color, message.toHtmlEscaped()));
}

void BricsCadPage::startBridgeServer()
{
    m_bridgeServer = new QTcpServer(this);
    const bool listening = m_bridgeServer->listen(QHostAddress::LocalHost, kBridgePort);
    if (!listening) {
        m_bridgeStatus->setText(QString("127.0.0.1:%1 konnte nicht geoeffnet werden: %2")
            .arg(kBridgePort)
            .arg(m_bridgeServer->errorString()));
        appendBridgeLog("Serverstart fehlgeschlagen");
        return;
    }

    m_bridgeStatus->setText(QString("Bereit auf %1:%2").arg(kBridgeHost).arg(kBridgePort));
    appendBridgeLog("Server bereit");

    QObject::connect(m_bridgeServer, &QTcpServer::newConnection, this, [this]() {
        while (m_bridgeServer->hasPendingConnections()) {
            auto* socket = m_bridgeServer->nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                const QByteArray request = socket->readAll();
                const QByteArray response = handleBridgeRequest(request);
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });
}

void BricsCadPage::appendBridgeLog(const QString& message)
{
    if (!m_bridgeLog) {
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_bridgeLog->appendPlainText(QString("[%1] %2").arg(stamp, message));
}

QByteArray BricsCadPage::handleBridgeRequest(const QByteArray& request)
{
    const QString command = QString::fromUtf8(request).trimmed().toUpper();
    appendBridgeLog(QString("BricsCAD -> Qt: %1").arg(command.isEmpty() ? "<leer>" : command));

    if (command == "PING") {
        appendBridgeLog("Qt -> BricsCAD: PONG");
        return "PONG Barebone-Qt bridge is alive\n";
    }

    if (command == "INFO") {
        appendBridgeLog("Qt -> BricsCAD: INFO");
        return "Barebone-Qt bridge: SDK=BRX26.1.05, Plugin=BareboneBrx.brx, QtPort=47626, BrxPort=47627\n";
    }

    appendBridgeLog("Qt -> BricsCAD: ERROR");
    return "ERROR Unknown command\n";
}
