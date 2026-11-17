#include <QApplication>
#include <QStringList>

#include "lanewindow.h"
#include "logger.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    Logger::inst()->init();

    // --connect <host> <port>  启动即连接 (自动化测试 / 演示用)
    QString host;
    int port = 6000;
    QStringList args = a.arguments();
    for (int i = 1; i < args.size(); ++i)
    {
        if (args.at(i) == "--connect" && i + 2 < args.size())
        {
            host = args.at(++i);
            port = args.at(++i).toInt();
        }
    }

    LaneWindow w;
    w.setWindowTitle(QString::fromUtf8("高速公路入口自动发卡车道  v1-w1  (并发重构后)"));
    w.resize(1240, 860);
    w.show();

    if (!host.isEmpty())
        w.autoConnect(host, port);

    int rc = a.exec();

    // 异步日志线程必须显式收尾, 否则退出时最后几行会丢
    Logger::inst()->flush();
    Logger::inst()->shutdown();

    return rc;
}
