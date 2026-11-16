#ifndef SIMSERVER_H
#define SIMSERVER_H

#include <QByteArray>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

// ============================================================================
//  车道设备模拟器
//
//  模拟现场这些设备:
//    触发线圈 A / 存在线圈 B  (车检器)
//    发卡机机构: 卡箱 / 分卡 / 卡槽伸缩    (备卡 900ms 伸卡 600ms 缩卡 500ms)
//    CPC 读写天线              (写卡 700ms)
//    回收箱
//  语音和 LED 屏只做日志, 不做界面
// ============================================================================

class SimDevice : public QObject
{
    Q_OBJECT

public:
    explicit SimDevice(QTcpSocket *sock, QObject *parent = 0);

    // 脏点: 参数放文件内全局变量, 多实例共享
    static void setConfig(int binCount, double writeFailRate, double jamRate);

    // 模拟现场动作 (控制台敲命令驱动)
    void simulateCarArrived();
    void simulateTakeCard();
    void simulateCarLeft(bool tookCard);
    void setBin(int n);
    bool hasCar() const { return m_carPresent; }
    void printState();

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    void handle(int cmd, const QByteArray &p);
    void send(const QByteArray &f);
    void ack(int rc);
    void pushEvent(int ev, int param);
    void pushStatus();
    bool roll(double rate);

    QTcpSocket *m_sock;
    QByteArray m_buf;

    int m_devState;
    int m_bin;
    int m_err;
    int m_slot;

    bool m_cardInSlot;
    bool m_cardWritten;
    QString m_cardNo;
    QByteArray m_entry;
    int m_recycled;

    bool m_carPresent;
    QString m_plate;
    int m_kind;
};

class SimServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit SimServer(QObject *parent = 0);

    void setAutoInterval(int sec);
    void setNoTake(bool on) { m_noTake = on; }
    SimDevice *device() const { return m_dev; }

protected:
    // 脏点: 只认一个上位机, 第二个连进来会把第一个顶掉, 也不提示
    void incomingConnection(qintptr fd);

private slots:
    void onAutoTick();

private:
    SimDevice *m_dev;
    bool m_autoBusy;
    bool m_noTake;
    int m_autoSec;
};

#endif // SIMSERVER_H
