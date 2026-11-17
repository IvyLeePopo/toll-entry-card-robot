#include "simserver.h"
#include "protocol_dup.h"

#include <QDebug>
#include <QHostAddress>
#include <QTimer>
#include <QtGlobal>

// 脏点: 配置用文件内全局变量, 多实例共享, 也没做范围校验
static int g_binInit = 150;             // 现场每个工位 150 张
static double g_writeFailRate = 0.02;
static double g_jamRate = 0.01;
static double g_prepareFailRate = 0.02;

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

void SimDevice::setConfig(int binPerPos, double writeFailRate,
                          double jamRate, double prepareFailRate)
{
    g_binInit = binPerPos;
    g_writeFailRate = writeFailRate;
    g_jamRate = jamRate;
    g_prepareFailRate = prepareFailRate;
}

SimDevice::SimDevice(QTcpSocket *sock, QObject *parent)
    : QObject(parent)
{
    m_sock = sock;
    m_devState = 0;
    m_stretch = STRETCH_IN;
    m_heartSeq = 0;
    m_carPresent = false;
    m_kind = 0;
    m_recycled = 0;
    m_inited = 0;

    for (int i = 1; i <= POS_COUNT; ++i)
        m_pos[i].bin = g_binInit;

    connect(m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()));

    // 脏点: 心跳周期写死 20 秒, 和上位机的判定阈值是两份独立的数字
    m_heartTimer = new QTimer(this);
    connect(m_heartTimer, SIGNAL(timeout()), this, SLOT(onHeartbeat()));
    m_heartTimer->start(20 * 1000);

    qDebug() << "[DEV] powered on, bin per pos =" << g_binInit;

    // 真实机器人: 上电就自己把四个工位备好, 不等车道发命令
    QTimer::singleShot(600, this, [this]() { prepareAll(); });
    pushState();
}

SimDevice::~SimDevice()
{
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
    qDebug() << "[DEV] host disconnected, stretch =" << m_stretch;
}

void SimDevice::onHeartbeat()
{
    m_heartSeq = (m_heartSeq + 1) & 0xFF;
    QByteArray p;
    p.append((char)m_heartSeq);
    send(buildFrame(CMD_HEARTBEAT, p));
}

// ---------------------------------------------------------------------------

int SimDevice::partnerOf(int pos)
{
    // 上组 1<->2, 下组 3<->4
    if (pos == POS_UP_LEFT)
        return POS_UP_RIGHT;
    if (pos == POS_UP_RIGHT)
        return POS_UP_LEFT;
    if (pos == POS_DOWN_LEFT)
        return POS_DOWN_RIGHT;
    if (pos == POS_DOWN_RIGHT)
        return POS_DOWN_LEFT;
    return 0;
}

int SimDevice::overallState()
{
    // 脏点: 用"数几个"来算整体状态, 上组坏了但下组好也算 PARTIAL, 语义很粗
    int bad = 0;
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        if (m_pos[i].state == PS_FAILED || m_pos[i].state == PS_FAULT)
            bad++;
    }
    if (bad >= POS_COUNT)
        return DS_ALLDOWN;
    if (bad > 0)
        return DS_PARTIAL;
    return DS_NORMAL;
}

int SimDevice::findPosByState(int st)
{
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        if (m_pos[i].state == st)
            return i;
    }
    return 0;
}

void SimDevice::prepareAll()
{
    int delay = 300;
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        if (m_pos[i].state == PS_EMPTY)
        {
            prepareLater(i, delay);
            delay += 200;          // 四个工位不要同时动作, 模拟机械排队
        }
    }
}

void SimDevice::prepareLater(int pos, int delayMs)
{
    if (pos < 1 || pos > POS_COUNT)
        return;

    QTimer::singleShot(delayMs, this, [this, pos]() { onPrepared(pos); });
}

void SimDevice::onPrepared(int pos)
{
    PosSlot &p = m_pos[pos];

    if (p.state == PS_SENT || p.state == PS_READY)
        return;                         // 已经有卡了, 别重复备

    if (p.bin <= 0)
    {
        p.state = PS_FAILED;
        qDebug() << "[DEV]" << posText(pos) << "bin empty";
        pushEvent(EV_POS_NO_CARD, pos);
        pushState();
        return;
    }

    // 脏点: 分卡失败直接切配对工位, 但没有区分"卡箱空"和"机构卡滞",
    //       现场这两种情况处理方式完全不同 (缺卡要人补, 卡滞要人拆)
    if (rand01() < g_prepareFailRate || rand01() < g_jamRate)
    {
        p.state = PS_FAILED;
        p.failCount++;
        p.needRecoverNotify = true;
        qDebug() << "[DEV]" << posText(pos) << "prepare FAILED, try partner";

        int partner = partnerOf(pos);
        if (partner > 0 && m_pos[partner].state == PS_EMPTY && m_pos[partner].bin > 0)
            prepareLater(partner, 400);

        pushState();
        return;
    }

    p.bin--;
    p.state = PS_READY;
    p.written = false;
    p.cardNo = makeCardNo();
    p.failCount = 0;

    QByteArray d;
    d.append((char)pos);
    d.append(p.cardNo.toLocal8Bit());
    send(buildFrame(CMD_CARD_DATA, d));

    qDebug() << "[DEV]" << posText(pos) << "card ready:" << p.cardNo << "bin left =" << p.bin;

    if (p.needRecoverNotify)
    {
        p.needRecoverNotify = false;
        pushEvent(EV_PREPARE_RECOVER, pos);   // 现场 event=4
    }

    pushState();

    if (p.bin <= 30)
        pushEvent(EV_POS_LOW, pos);
}

void SimDevice::stretchIn()
{
    m_stretch = STRETCH_IN;
}

// ---------------------------------------------------------------------------

void SimDevice::handle(int cmd, const QByteArray &p)
{
    // 脏点: 没有统一的参数校验层, 每条命令自己 at(0)/at(1), 越界就是 UB
    qDebug() << "[DEV] rx cmd = 0x" + QString::number(cmd, 16) << "len =" << p.size();

    if (cmd == CMD_GET_STATUS)
    {
        pushState();
        return;
    }

    if (cmd == CMD_VOICE)
    {
        int id = p.size() > 0 ? (unsigned char)p.at(0) : 0;
        sendResult(cmd, RC_OK);
        qDebug() << "[SPK]" << voiceText(id);
        return;
    }

    if (cmd == CMD_DISPLAY)
    {
        sendResult(cmd, RC_OK);
        qDebug() << "[LED] line" << (p.size() > 0 ? (int)(unsigned char)p.at(0) : 0)
                 << QString::fromLocal8Bit(p.mid(1));
        return;
    }

    if (cmd == CMD_BARRIER)
    {
        bool up = p.size() > 0 && ((unsigned char)p.at(0) != 0);
        sendResult(cmd, RC_OK);
        qDebug() << "[GATE]" << (up ? "UP" : "DOWN");
        return;
    }

    if (cmd == CMD_TIME_SYNC)
    {
        // 脏点: 只打了个日志, 根本没改系统时间; 真机上这里要调 settimeofday
        qDebug() << "[DEV] time sync:" << QString::fromLocal8Bit(p).left(14);
        sendResult(cmd, RC_OK);
        return;
    }

    if (cmd == CMD_DEBUG)
    {
        int act = p.size() > 0 ? (unsigned char)p.at(0) : 0;
        sendResult(cmd, RC_OK);

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
            // 故障恢复: 强制造一个"车道已清空 + 机构已复位"的现场
            qDebug() << "[SIM] force clear lane";
            m_carPresent = false;
            stretchIn();
            for (int i = 1; i <= POS_COUNT; ++i)
            {
                m_pos[i].state = PS_EMPTY;
                m_pos[i].written = false;
            }
            pushState();
            prepareAll();
        }
        else
            qDebug() << "[SIM] unknown debug action" << act;
        return;
    }

    if (cmd == CMD_SELFTEST)
    {
        m_devState = 1;
        QTimer::singleShot(1000, this, [this, cmd]() {
            m_devState = 0;
            m_inited = 1;
            qDebug() << "[DEV] selftest ok";
            prepareAll();
            sendResult(cmd, RC_OK);
            pushState();
        });
        return;
    }

    if (cmd == CMD_RESET)
    {
        QTimer::singleShot(500, this, [this, cmd]() {
            stretchIn();
            m_devState = 0;
            m_inited = 1;
            for (int i = 1; i <= POS_COUNT; ++i)
            {
                m_pos[i].state = PS_EMPTY;
                m_pos[i].written = false;
            }
            qDebug() << "[DEV] reset done";
            sendResult(cmd, RC_OK);
            pushState();
            prepareAll();
        });
        return;
    }

    // ---------------- 以下命令都带工位号 ----------------
    int pos = p.size() > 0 ? (unsigned char)p.at(0) : 0;

    if (pos < 1 || pos > POS_COUNT)
    {
        sendResult(cmd, RC_NO_POS);
        return;
    }

    PosSlot &P = m_pos[pos];

    // ---------------- 手动备卡 ----------------
    if (cmd == CMD_PREPARE_CARD)
    {
        if (P.state == PS_READY || P.state == PS_SENT)
        {
            sendResult(cmd, RC_BAD_STATE);
            return;
        }
        P.state = PS_EMPTY;
        prepareLater(pos, 900);
        sendResult(cmd, RC_OK);
        return;
    }

    // ---------------- 写卡 ----------------
    if (cmd == CMD_WRITE_CARD)
    {
        if (P.state != PS_READY)
        {
            sendResult(cmd, RC_NOT_READY);
            return;
        }
        if (p.size() < 1 + ENTRY_DATA_LEN)
        {
            sendResult(cmd, RC_BAD_STATE);
            return;
        }

        QByteArray entry = p.mid(1);
        int wpos = pos;
        bool fail = rand01() < g_writeFailRate;

        QTimer::singleShot(700, this, [this, cmd, entry, wpos, fail]() {
            if (fail)
            {
                qDebug() << "[DEV] inject write failure";
                sendResult(cmd, RC_WRITE_FAIL);
                return;
            }

            QString st, ln, pl, tm, cn;
            int kd = 0;
            unpackEntryData(entry, &st, &ln, &kd, &pl, &tm, &cn);

            // 脏点: 写卡成功只改了 written 标志, 没校验写进去的卡号和实际卡片是否一致
            //       (现场这一步是靠读卡器回读 EF01 来确认的)
            m_pos[wpos].written = true;
            qDebug() << "[DEV]" << posText(wpos) << "written:" << st << ln << kd << pl << tm << cn;

            sendResult(cmd, RC_OK);
            pushState();
        });
        return;
    }

    // ---------------- 送卡 ----------------
    if (cmd == CMD_PRESENT_CARD)
    {
        if (P.state != PS_READY)
        {
            sendResult(cmd, RC_NOT_READY);
            return;
        }
        if (!P.written)
        {
            sendResult(cmd, RC_BAD_STATE);
            return;
        }

        // 下工位: 先把伸缩机构伸出去, 再把卡送出来 (现场 20cm)
        int delay = 600;
        if (pos == POS_DOWN_LEFT || pos == POS_DOWN_RIGHT)
        {
            m_stretch = STRETCH_OUT;
            delay = 800;
            qDebug() << "[DEV] stretch out 20cm for" << posText(pos);
        }

        QTimer::singleShot(delay, this, [this, cmd, pos]() {
            m_pos[pos].state = PS_SENT;
            qDebug() << "[DEV]" << posText(pos) << "card presented, waiting take";
            sendResult(cmd, RC_OK);
            pushState();
        });
        return;
    }

    // ---------------- 缩卡 ----------------
    if (cmd == CMD_RETRACT_CARD)
    {
        QTimer::singleShot(500, this, [this, cmd]() {
            stretchIn();
            qDebug() << "[DEV] stretch in";
            sendResult(cmd, RC_OK);
            pushState();
        });
        return;
    }

    // ---------------- 回收 ----------------
    if (cmd == CMD_RECYCLE_CARD)
    {
        if (P.state != PS_READY && P.state != PS_SENT)
        {
            sendResult(cmd, RC_BAD_STATE);
            return;
        }

        int reason = p.size() > 1 ? (unsigned char)p.at(1) : 0;

        QTimer::singleShot(800, this, [this, cmd, pos, reason]() {
            m_pos[pos].state = PS_EMPTY;
            m_pos[pos].written = false;
            m_pos[pos].needRecoverNotify = true;
            m_recycled++;
            stretchIn();
            qDebug() << "[DEV]" << posText(pos) << "recycled, reason =" << reason
                     << ", total =" << m_recycled;
            sendResult(cmd, RC_OK);
            pushState();
            prepareLater(pos, 300);
        });
        return;
    }

    sendResult(cmd, RC_UNKNOWN_CMD);
    qDebug() << "[DEV] unknown cmd";
}

void SimDevice::send(const QByteArray &f)
{
    if (!m_sock)
        return;
    m_sock->write(f);
    m_sock->flush();
}

void SimDevice::sendResult(int cmd, int rc)
{
    QByteArray p;
    p.append((char)(cmd & 0xFF));
    p.append((char)(rc & 0xFF));
    send(buildFrame(CMD_RESULT, p));
}

void SimDevice::pushEvent(int ev, int pos)
{
    QByteArray p;
    p.append((char)(ev & 0xFF));
    p.append((char)(pos & 0xFF));
    send(buildFrame(CMD_EVENT, p));
}

void SimDevice::pushState()
{
    // 对应现场 B2: [整体状态] + 4 x (工位状态, 卡数)
    QByteArray p;
    p.append((char)overallState());
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        p.append((char)m_pos[i].state);
        p.append((char)m_pos[i].bin);
    }
    send(buildFrame(CMD_STATE, p));
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

    int pos = findPosByState(PS_SENT);
    if (pos == 0)
    {
        qDebug() << "[SIM] no card presented, take ignored";
        return;
    }

    m_pos[pos].state = PS_EMPTY;
    m_pos[pos].written = false;
    stretchIn();

    qDebug() << "[SIM] driver took the card at" << posText(pos);
    pushEvent(EV_CARD_TAKEN, pos);
    pushState();

    // 现场: 取卡后立刻补备下一张
    prepareLater(pos, 500);
}

void SimDevice::simulateCarLeft(bool tookCard)
{
    if (!m_carPresent)
    {
        qDebug() << "[SIM] no car, leave ignored";
        return;
    }

    m_carPresent = false;

    int stillHolding = findPosByState(PS_SENT);
    if (!tookCard && stillHolding > 0)
    {
        qDebug() << "[SIM] car left WITHOUT taking the card at" << posText(stillHolding);
        pushEvent(EV_LEFT_NO_CARD, stillHolding);
    }

    qDebug() << "[SIM] car left the lane";
    pushEvent(EV_CAR_LEFT, 0);
}

void SimDevice::setBin(int n)
{
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        m_pos[i].bin = n;
        if (m_pos[i].state == PS_FAILED)
            m_pos[i].state = PS_EMPTY;
    }
    qDebug() << "[SIM] bin per pos set to" << n;
    pushState();
    prepareAll();
}

void SimDevice::printState()
{
    qDebug() << "-------- device state --------";
    qDebug() << " overall:" << devSummaryText(overallState())
             << " stretch:" << (m_stretch == STRETCH_OUT ? "OUT" : "IN")
             << " car:" << m_carPresent;
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        qDebug() << " " << posText(i)
                 << posStateText(m_pos[i].state)
                 << "bin:" << m_pos[i].bin
                 << "card:" << (m_pos[i].cardNo.isEmpty() ? QString("-") : m_pos[i].cardNo);
    }
    qDebug() << " bin empty total:" << m_recycled;
    qDebug() << "------------------------------";
}

// ----------------------------------------------------------------------------

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

    // 脏点: 自动来车用的是写死的时序 (6 秒取卡 / 4 秒离开), 跟真实司机没关系
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
