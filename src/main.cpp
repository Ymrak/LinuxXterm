#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("LinuxXterm");
    app.setApplicationVersion("1.0.2");

    MainWindow window;
    window.show();

    return app.exec();
}
