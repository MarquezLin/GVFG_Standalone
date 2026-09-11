#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QWidget *window = createMainWindow();
    window->show();

    const int result = app.exec();
    delete window;
    return result;
}
