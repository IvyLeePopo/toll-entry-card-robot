#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QStringList>

#include "console.h"
#include "simserver.h"

static void printHelp()
{
    qDebug() << "";
    qDebug() << " commands:";
    qDebug() << "   car  | c        a car arrives (trigger loop A)";
    qDebug() << "   take | t        driver takes the card";
    qDebug() << "   leave| l        car leaves the lane (loop B released)";
    qDebug() << "   nocard| n       car leaves WITHOUT taking the card";
    qDebug() << "   bin N           set card bin count";
    qDebug() << "   auto N          auto car every N seconds (0 = off)";
    qDebug() << "   status | s      print device state";
    qDebug() << "   help   | h      this help";
    qDebug() << "";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int port = 6000;
    int bin = 150;
    double fail = 0.02;
    double jam = 0.01;
    double prep = 0.02;
    int autoSec = 0;
    int speed = 100;
    bool noTake = false;

    // 脏点: 手写参数解析, 敲错了不报错, 静默用默认值
    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i)
    {
        QString a = args.at(i);
        if (a == "--port" && i + 1 < args.size())
            port = args.at(++i).toInt();
        else if (a == "--bin" && i + 1 < args.size())
            bin = args.at(++i).toInt();
        else if (a == "--fail" && i + 1 < args.size())
            fail = args.at(++i).toDouble();
        else if (a == "--jam" && i + 1 < args.size())
            jam = args.at(++i).toDouble();
        else if (a == "--prep" && i + 1 < args.size())
            prep = args.at(++i).toDouble();
        else if (a == "--auto" && i + 1 < args.size())
            autoSec = args.at(++i).toInt();
        else if (a == "--speed" && i + 1 < args.size())
            speed = args.at(++i).toInt();
        else if (a == "--notake")
            noTake = true;
        else if (a == "-h" || a == "--help")
        {
            qDebug() << "usage: LaneSim [--port 6000] [--bin 150] [--fail 0.02] [--jam 0.01] [--prep 0.02] [--auto 10] [--speed 100] [--notake]";
            return 0;
        }
    }

    SimDevice::setConfig(bin, fail, jam, prep, speed);

    SimServer server;
    if (!server.listen(QHostAddress::Any, port))
    {
        qDebug() << "listen failed on port" << port;
        return 1;
    }

    qDebug() << "=============== LaneSim v0 (toll entrance card dispenser) ===============";
    qDebug() << " port      :" << port;
    qDebug() << " card bin  :" << bin << "per position";
    qDebug() << " write fail:" << fail;
    qDebug() << " jam rate  :" << jam;
    qDebug() << " prep fail :" << prep;
    qDebug() << " speed     :" << QString::number(speed) + "%";
    qDebug() << " auto car  :" << (autoSec > 0 ? QString::number(autoSec) + "s" : QString("off"));
    qDebug() << "------------------------------------------------------------------------";
    qDebug() << " waiting for LaneCtl to connect, then type commands below";
    printHelp();

    ConsoleInput console;
    console.start();

    QObject::connect(&console, &ConsoleInput::command, [&server](const QString &line) {
        QStringList t = line.split(" ", QString::SkipEmptyParts);
        if (t.isEmpty())
            return;

        QString c = t.at(0).toLower();
        SimDevice *dev = server.device();

        if (!dev && c != "help" && c != "h")
        {
            qDebug() << "[SIM] lane controller not connected yet";
            return;
        }

        if (c == "car" || c == "c")
            dev->simulateCarArrived();
        else if (c == "take" || c == "t")
            dev->simulateTakeCard();
        else if (c == "leave" || c == "l")
            dev->simulateCarLeft(true);
        else if (c == "nocard" || c == "n")
            dev->simulateCarLeft(false);
        else if (c == "bin" && t.size() > 1)
            dev->setBin(t.at(1).toInt());
        else if (c == "auto" && t.size() > 1)
            server.setAutoInterval(t.at(1).toInt());
        else if (c == "status" || c == "s")
            dev->printState();
        else if (c == "help" || c == "h" || c == "?")
            printHelp();
        else
            qDebug() << "[SIM] unknown command:" << c << ", type help";
    });

    if (autoSec > 0)
        server.setAutoInterval(autoSec);
    server.setNoTake(noTake);

    return app.exec();
}
