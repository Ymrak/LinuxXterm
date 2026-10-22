#include <QApplication>
#include <QCoreApplication>
#include "mainwindow.h"
#include "sshpassproxy.h"
#include <cstring>

int main(int argc, char *argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--sshpass-proxy") == 0) {
        QCoreApplication application(argc, argv);
        return runSshpassProxy(argc, argv);
    }

    QApplication app(argc, argv);
    app.setApplicationName("LinuxXterm");
    app.setApplicationVersion("1.0.3");

    MainWindow window;
    window.show();

    return app.exec();
}
