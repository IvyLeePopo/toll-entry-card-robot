#include "simserver.h"
#include "protocol_dup.h"

#include <QDebug>
#include <QHostAddress>
#include <QTimer>
#include <QtGlobal>

static int g_binInit = 500;
static double g_writeFailRate = 0.02;
static double g_jamRate = 0.01;

static double rand01()
{
    // 脏点: 老的 qrand, 没种子; 真实项目用 QRandomGenerator
    return (double)qrand() / (double)RAND_MAX;
}

static QString makePlate()
{
    static const char *prov[] = {"\xcb\xd5", "\xd5\xe3", "\xbb\xa6", "\xcd\xee"};   // 苏 浙 沪 皖 (GBK)
    QString p = QString::fromLocal8Bit(prov[qrand() % 4]);
    p += QChar('A' + qrand() % 20);
    p += QString("%1").arg(10000 + qrand() % 89999);
    return p;                 // 苏A12345 -> GBK 正好 8 字节
}

static QString makeCardNo()
{
    return QString("%1").arg(10000000 + qrand() % 89999999);
}

void SimDevice::setConfig(int binCount, double writeFailRate, double jamRate)
{
    g_binInit = binCount;
    g_writeFailRate = writeFailRate;
    g_jamRate = jamRate;
}

SimDevice::SimDevice(QTcpSocket *sock, QObject *parent)
    : QObject(parent)
{
    m_sock = sock;
    m_devState = DEV_IDLE;
    m_bin = g_binInit;
    m_err = 0;
    m_slot = SLOT_IN;
    m_cardInSlot = false;
    m_cardWritten = false;
    m_recycled = 0;
    m_carPresent = false;
    m_kind = 0;

    connect(m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()));

    qDebug() << "[DEV] powered on, bin =" << m_bin;
    pushStatus();
}

void SimDevice::onReadyRead()
{
    m_buf.append(m_sock->readAll());

    while (true)
    {
        int cmd = 0;
        QByteArray p;
        int used = parseFrame(m_buf, &cmd, &p);

        if (used == 0)
            return;
        if (used < 0)
        {
            qDebug() << "[DEV] bad frame, drop buffer";
            m_buf.clear();
            return;
        }

        m_buf = m_buf.mid(used);
        handle(cmd, p);
    }
}

void SimDevice::onDisconnected()
{
    // 脏点: 设备对象和内部的 QTimer 都不回收, 重连一次泄漏一个
    qDebug() << "[DEV] host disconnected. cardInSlot =" << m_cardInSlot << "slot =" << m_slot;
}

// ---------------------------------------------------------------------------

void SimDevice::handle(int cmd, const QByteArray &p)
{
    // 脏点: 和上位机各写一套状态判断, 两边认为的"合法命令"并不一致
    qDebug() << "[DEV] rx cmd = 0x" + QString::number(cmd, 16) << "len =" << p.size();

    if (cmd == CMD_GET_STATUS)
    {
        ack(RC_OK);
        pushStatus();
        return;
    }

    if (cmd == CMD_RESET)
    {
        ack(RC_OK);
        m_devState = DEV_SELFTEST;      // 脏点: 复位借用自检状态
        QTimer::singleShot(500, this, [this]() {
            m_devState = DEV_IDLE;
            m_err = 0;
            m_slot = SLOT_IN;
            qDebug() << "[DEV] reset done";
            pushEvent(EV_RESET_DONE, 0);
        });
        return;
    }

    // 语音 / 屏显 / 栏杆: 只回 ACK, 不回 EVENT
    // 脏点: 上位机必须知道"哪些命令要等事件, 哪些不用", 否则一定超时
    if (cmd == CMD_VOICE)
    {
        int id = p.size() > 0 ? (unsigned char)p.at(0) : 0;
        ack(RC_OK);
        qDebug() << "[SPK]" << voiceText(id);
        return;
    }

    if (cmd == CMD_DISPLAY)
    {
        ack(RC_OK);
        qDebug() << "[LED] line" << (p.size() > 0 ? (int)(unsigned char)p.at(0) : 0)
                 << QString::fromLocal8Bit(p.mid(1));
        return;
    }

    if (cmd == CMD_BARRIER)
    {
        bool up = p.size() > 0 && ((unsigned char)p.at(0) != 0);
        ack(RC_OK);
        qDebug() << "[GATE]" << (up ? "UP" : "DOWN");
        return;
    }

    // 调试命令: 让模拟器自己造现场, 相当于有人去压线圈 / 伸手拿卡
    // 脏点: 生产协议里混进调试指令, 上真机只能靠"设备不认这条命令"兜底
    if (cmd == CMD_DEBUG)
    {
        int act = p.size() > 0 ? (unsigned char)p.at(0) : 0;
        ack(RC_OK);
        if (act == DBG_CAR_ARRIVE)
            simulateCarArrived();
        else if (act == DBG_CAR_TAKE)
            simulateTakeCard();
        else if (act == DBG_CAR_LEAVE)
            simulateCarLeft(true);
        else if (act == DBG_CAR_LEAVE_NO)
            simulateCarLeft(false);
        else if (act == DBG_LANE_CLEAR)
        {
            // 故障恢复用: 强制造一个"车道已清空"的现场
            // 脏点: 真机上这一步是人工确认后按的"车道复位", 这里一并把卡也丢了
            qDebug() << "[SIM] force clear lane";
            m_carPresent = false;
            m_cardInSlot = false;
            m_cardWritten = false;
            m_slot = SLOT_IN;
            m_devState = DEV_IDLE;
            m_err = 0;
            pushStatus();
        }
        else
            qDebug() << "[SIM] unknown debug action" << act;
        return;
    }

    if (m_devState != DEV_IDLE && m_devState != DEV_WAIT_TAKE)
    {
        qDebug() << "[DEV] busy:" << devStateText(m_devState) << ", reject cmd";
        ack(RC_BAD_STATE);
        return;
    }
    // 脏点: 这里放行 WAIT_TAKE 是"补丁式"改法 —— 等取卡时允许缩卡/回收,
    //       但同样的状态判断在缩卡/回收/备卡的函数体里又各写了一遍

    // ---------------- 自检 ----------------
    if (cmd == CMD_SELFTEST)
    {
        ack(RC_OK);
        m_devState = DEV_SELFTEST;
        QTimer::singleShot(1000, this, [this]() {
            m_devState = DEV_IDLE;
            qDebug() << "[DEV] selftest ok";
            pushEvent(EV_SELFTEST_DONE, 0);
        });
        return;
    }

    // ---------------- 备卡 ----------------
    if (cmd == CMD_PREPARE_CARD)
    {
        if (m_cardInSlot)
        {
            ack(RC_BAD_STATE);              // 上一张卡还在机构里
            return;
        }

        if (m_bin <= 0)
        {
            m_err = RC_BIN_EMPTY;
            m_devState = DEV_NO_CARD;
            ack(RC_BIN_EMPTY);
            QTimer::singleShot(100, this, [this]() {
                qDebug() << "[DEV] bin empty";
                pushEvent(EV_BIN_EMPTY, 0);
            });
            return;
        }

        ack(RC_OK);
        m_devState = DEV_PREPARING;

        if (roll(g_jamRate))
        {
            // 分卡卡滞
            QTimer::singleShot(600, this, [this]() {
                m_devState = DEV_FAULT;
                m_err = RC_JAM;
                qDebug() << "[DEV] JAM in card feeder";
                pushEvent(EV_FAULT, RC_JAM);
            });
            return;
        }

        QTimer::singleShot(900, this, [this]() {
            m_bin--;
            m_cardInSlot = true;
            m_cardWritten = false;
            m_cardNo = makeCardNo();
            m_devState = DEV_IDLE;

            // 真实设备: 备卡后先把卡号读上来
            send(buildFrame(CMD_CARD_DATA, m_cardNo.toLocal8Bit()));
            qDebug() << "[DEV] card ready:" << m_cardNo << "bin left =" << m_bin;

            pushEvent(EV_CARD_READY, 0);

            if (m_bin <= 30)
                pushEvent(EV_BIN_LOW, m_bin);
        });
        return;
    }

    // ---------------- 写卡 ----------------
    if (cmd == CMD_WRITE_CARD)
    {
        if (!m_cardInSlot || m_slot != SLOT_IN)
        {
            ack(RC_BAD_STATE);
            return;
        }
        if (p.size() < ENTRY_DATA_LEN)
        {
            ack(RC_BAD_STATE);
            return;
        }

        ack(RC_OK);
        m_devState = DEV_WRITING;

        bool fail = roll(g_writeFailRate);

        QTimer::singleShot(700, this, [this, p, fail]() {
            m_devState = DEV_IDLE;
            if (fail)
            {
                m_err = RC_WRITE_FAIL;
                qDebug() << "[DEV] inject write failure";
                pushEvent(EV_WRITE_DONE, RC_WRITE_FAIL);
                return;
            }

            // 真实设备写完要回读校验
            QString st, ln, pl, tm, cn;
            int kd = 0;
            unpackEntryData(p, &st, &ln, &kd, &pl, &tm, &cn);
            m_entry = p;
            m_cardWritten = true;

            qDebug() << "[DEV] written:" << st << ln << kd << pl << tm << cn;
            pushEvent(EV_WRITE_DONE, RC_OK);
        });
        return;
    }

    // ---------------- 伸卡 ----------------
    if (cmd == CMD_PRESENT_CARD)
    {
        if (!m_cardInSlot || !m_cardWritten || m_slot != SLOT_IN)
        {
            ack(RC_BAD_STATE);
            return;
        }

        ack(RC_OK);
        m_devState = DEV_PRESENTING;

        QTimer::singleShot(600, this, [this]() {
            m_slot = SLOT_OUT;
            m_devState = DEV_WAIT_TAKE;
            qDebug() << "[DEV] slot out";
            pushEvent(EV_SLOT_OUT, 0);
        });
        return;
    }

    // ---------------- 缩卡 ----------------
    if (cmd == CMD_RETRACT_CARD)
    {
        if (m_slot != SLOT_OUT)
        {
            ack(RC_BAD_STATE);
            return;
        }

        ack(RC_OK);
        m_devState = DEV_RETRACTING;

        QTimer::singleShot(500, this, [this]() {
            m_slot = SLOT_IN;
            m_devState = DEV_IDLE;
            qDebug() << "[DEV] slot in";
            pushEvent(EV_SLOT_IN, 0);
        });
        return;
    }

    // ---------------- 回收 ----------------
    if (cmd == CMD_RECYCLE_CARD)
    {
        if (!m_cardInSlot)
        {
            ack(RC_BAD_STATE);
            return;
        }

        int reason = p.size() > 0 ? (unsigned char)p.at(0) : 0;

        ack(RC_OK);
        m_devState = DEV_RECYCLING;

        QTimer::singleShot(800, this, [this, reason]() {
            m_cardInSlot = false;
            m_cardWritten = false;
            m_slot = SLOT_IN;
            m_recycled++;
            m_devState = DEV_IDLE;
            qDebug() << "[DEV] card recycled, reason =" << reason << ", total =" << m_recycled;
            pushEvent(EV_RECYCLED, 0);
        });
        return;
    }

    ack(RC_UNKNOWN_CMD);
    qDebug() << "[DEV] unknown cmd";
}

void SimDevice::send(const QByteArray &f)
{
    if (!m_sock)
        return;
    m_sock->write(f);
    m_sock->flush();
}

void SimDevice::ack(int rc)
{
    // 脏点: ACK 里不返回命令字, 上位机分不清是哪条被拒
    QByteArray p;
    p.append((char)(rc & 0xFF));
    send(buildFrame(CMD_ACK, p));
}

void SimDevice::pushEvent(int ev, int param)
{
    QByteArray p;
    p.append((char)(ev & 0xFF));
    p.append((char)(param & 0xFF));
    send(buildFrame(CMD_EVENT, p));
    pushStatus();
}

void SimDevice::pushStatus()
{
    QByteArray p;
    p.append((char)m_devState);
    p.append((char)m_bin);
    p.append((char)m_err);
    p.append((char)m_slot);      // 脏点: 第 4 字节协议文档里没有, 上位机却偷偷用了
    send(buildFrame(CMD_STATUS, p));
}

bool SimDevice::roll(double rate)
{
    if (rate <= 0.0)
        return false;
    return rand01() < rate;
}

// ---------------------------------------------------------------------------
//  模拟现场动作
// ---------------------------------------------------------------------------

void SimDevice::simulateCarArrived()
{
    if (m_carPresent)
    {
        qDebug() << "[SIM] car already in lane, ignore";
        return;
    }

    m_carPresent = true;
    m_kind = 1 + qrand() % 6;
    m_plate = makePlate();

    qDebug() << "[SIM] car arrived:" << m_plate << "kind =" << m_kind;

    // 真实车道: 车牌识别仪 / 车型识别 先给车辆信息, 车检器再报"来车"
    QByteArray v;
    v.append(m_plate.toLocal8Bit());
    v.append((char)m_kind);
    send(buildFrame(CMD_VEHICLE_DATA, v));

    pushEvent(EV_CAR_ARRIVED, 0);
}

void SimDevice::simulateTakeCard()
{
    if (!m_carPresent)
    {
        qDebug() << "[SIM] no car, take ignored";
        return;
    }
    if (!m_cardInSlot || m_slot != SLOT_OUT)
    {
        qDebug() << "[SIM] no card presented, take ignored";
        return;
    }

    m_cardInSlot = false;
    qDebug() << "[SIM] driver took the card";
    pushEvent(EV_CARD_TAKEN, 0);
}

void SimDevice::simulateCarLeft(bool tookCard)
{
    if (!m_carPresent)
    {
        qDebug() << "[SIM] no car, leave ignored";
        return;
    }

    m_carPresent = false;

    if (!tookCard && m_cardInSlot)
    {
        qDebug() << "[SIM] car left WITHOUT taking the card!";
        pushEvent(EV_LEFT_NO_CARD, 0);
    }

    qDebug() << "[SIM] car left the lane";
    pushEvent(EV_CAR_LEFT, 0);
}

void SimDevice::setBin(int n)
{
    m_bin = n;
    if (m_bin > 0 && m_devState == DEV_NO_CARD)
        m_devState = DEV_IDLE;
    qDebug() << "[SIM] bin set to" << m_bin;
    pushStatus();
}

void SimDevice::printState()
{
    qDebug() << "----- device state -----";
    qDebug() << " state      :" << devStateText(m_devState);
    qDebug() << " bin        :" << m_bin;
    qDebug() << " slot       :" << (m_slot == SLOT_OUT ? "OUT" : "IN");
    qDebug() << " cardInSlot :" << m_cardInSlot << "written:" << m_cardWritten;
    qDebug() << " cardNo     :" << (m_cardNo.isEmpty() ? QString("-") : m_cardNo);
    qDebug() << " car        :" << m_carPresent << (m_carPresent ? m_plate : QString("-"));
    qDebug() << " recycled   :" << m_recycled;
    qDebug() << "------------------------";
}

// ---------------------------------------------------------------------------

SimServer::SimServer(QObject *parent)
    : QTcpServer(parent)
{
    m_dev = 0;
    m_autoBusy = false;
    m_noTake = false;
    m_autoSec = 0;
}

void SimServer::setAutoInterval(int sec)
{
    m_autoSec = sec;
    if (sec <= 0)
        return;

    QTimer *t = new QTimer(this);
    connect(t, SIGNAL(timeout()), this, SLOT(onAutoTick()));
    t->start(sec * 1000);
    qDebug() << "[SIM] auto car mode on, every" << sec << "s";
}

void SimServer::onAutoTick()
{
    if (!m_dev || m_autoBusy)
        return;
    if (m_dev->hasCar())
        return;

    // 脏点: 自动来车用的是写死的时序 (6 秒取卡 / 4 秒离开),
    //       跟真实司机的行为没关系, 也不管上位机有没有真的发到"请取卡"
    m_autoBusy = true;
    m_dev->simulateCarArrived();

    QTimer::singleShot(6000, this, [this]() {
        if (m_dev && !m_noTake)
            m_dev->simulateTakeCard();
        QTimer::singleShot(4000, this, [this]() {
            if (m_dev)
                m_dev->simulateCarLeft(!m_noTake);
            m_autoBusy = false;
        });
    });
}

void SimServer::incomingConnection(qintptr fd)
{
    QTcpSocket *s = new QTcpSocket(this);
    s->setSocketDescriptor(fd);
    qDebug() << "[SIM] lane controller connected from" << s->peerAddress().toString();
    m_dev = new SimDevice(s, this);
}
