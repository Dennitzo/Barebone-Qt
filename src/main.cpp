#include "core/ConfigManager.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Barebone-Qt");
    QApplication::setApplicationVersion(APP_VERSION);
    QApplication::setOrganizationName("Barebone-Qt");
    QApplication::setWindowIcon(QIcon(":/icons/AppLogo.png"));

    ConfigManager config;
    MainWindow window(config);
    window.show();

    return app.exec();
}
