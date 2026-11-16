#include <QApplication>

#include "lanewindow.h"
#include "logger.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    Logger::inst()->init();

    LaneWindow w;
    w.setWindowTitle(QString::fromUtf8("高速公路入口自动发卡车道  v0  (故意写脏的基线)"));
    w.resize(1240, 860);
    w.show();

    return a.exec();
}
