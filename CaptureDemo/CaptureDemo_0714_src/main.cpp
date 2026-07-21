#include "mainwindow.h"
#include <QApplication>
#include <QFile>
// #include <QtCore/QTextCodec>
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

   // #if (QT_VERSION <= QT_VERSION_CHECK(5,0,0))
   // #if _MSC_VER
   // QTextCodec *codec = QTextCodec::codecForName("GBK");
   // #else
   // QTextCodec *codec = QTextCodec::codecForName("UTF-8");
   // #endif
   // QTextCodec::setCodecForLocale(codec);
   // QTextCodec::setCodecForCStrings(codec);
   // QTextCodec::setCodecForTr(codec);
   // #else
   // QTextCodec *codec = QTextCodec::codecForName("UTF-8");
   // QTextCodec::setCodecForLocale(codec);
   // #endif
    // QThread::Sleep(1);
    qDebug() << "start";
    QFile file(":/res/qss/default.qss");
    if (file.open(QFile::ReadOnly))
    {
        QString qss = QString(file.readAll());
        qApp->setStyleSheet(qss);
        file.close();
        MainWindow w;
        w.init();
        w.show();
        return a.exec();
    }
    else
    {
        return -1;
    }
}
