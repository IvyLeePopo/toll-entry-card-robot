#ifndef SIMSERVER_H
#define SIMSERVER_H

#include <QByteArray>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

class QTimer;

// ============================================================================
//  入口发卡机器人模拟器 (A 阶段: 4 工位)
//
//  现场机型: 上左/上右/下左/下右 四个工位, 每个工位一个卡箱, 上/下各一台读卡器
//  行为对齐真实项目:
//    * 上电/自检后 4 个工位各自备卡, 送卡后自动补备
//    * 备卡失败自动切到配对工位 (上左<->上右, 下左<->下右) 重试一次
//    * 下工位送卡前先把伸缩机构伸出 20cm, 取卡后自动收回
//    * 取卡、缺卡、恢复都是主动上报 (B3)
// ============================================================================

class SimDevice : public QObject
{
    Q_OBJECT

public:
    explicit SimDevice(QTcpSocket *sock, QObject *parent = 0);
    ~SimDevice();

    // 脏点: 参数放文件内全局变量, 多实例共享
    static void setConfig(int binPerPos, double writeFailRate,
                          double jamRate, double prepareFailRate, int speedPercent);

    void simulateCarArrived();
    void simulateTakeCard();
    void simulateCarLeft(bool tookCard);
    void setBin(int n);
    bool hasCar() const { return m_carPresent; }
    void printState();

private slots:
    void onReadyRead();
    void onDisconnected();
    void onHeartbeat();

private:
    struct PosSlot
    {
        PosSlot()
            : state(0), bin(0), written(false), failCount(0), needRecoverNotify(false) {}
        int state;              // PS_EMPTY / PS_READY / PS_FAILED / PS_SENT / PS_FAULT
        int bin;                // 该工位卡箱余量
        QString cardNo;
        bool written;
        int failCount;
        bool needRecoverNotify; // 失败/回收后重新备好, 要给车道补一个事件
    };

    void handle(int cmd, const QByteArray &p);
    void send(const QByteArray &f);
    void sendResult(int cmd, int rc);
    void pushState();
    void pushEvent(int ev, int pos);

    void prepareAll();
    void prepareLater(int pos, int delayMs);
    void onPrepared(int pos);
    int partnerOf(int pos);
    int overallState();
    int findPosByState(int st);
    void stretchIn();

    QTcpSocket *m_sock;
    QByteArray m_buf;

    PosSlot m_pos[5];           // 下标 1..4
    int m_devState;             // 0 空闲 1 自检 9 故障
    int m_stretch;              // STRETCH_IN / STRETCH_OUT
    int m_heartSeq;
    QTimer *m_heartTimer;

    bool m_carPresent;
    QString m_plate;
    int m_kind;
    int m_recycled;
    int m_inited;
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
