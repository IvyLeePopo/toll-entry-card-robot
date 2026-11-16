#include "protocol.h"

#include <QDebug>
#include <string.h>

// 脏点: 发送序号做成文件内全局变量, 多线程一定串号
static int g_seq = 0;

// 脏点: 定长结构体直接当协议用, 靠"两端都是小端"这种默契
struct EntryDataSt
{
    char station[4];
    char lane[2];
    char kind;
    char plate[8];          // 车牌 8 字节: "苏A12345" 刚好塞满, 新能源车牌就爆了
    char time[14];          // YYYYMMDDhhmmss
    char cardNo[8];
    char pad[3];            // 凑 40 字节, 谁也不知道为什么是 3
};

QByteArray buildFrame(int cmd, const QByteArray &payload)
{
    QByteArray f;
    f.append((char)FRAME_HEAD1);
    f.append((char)FRAME_HEAD2);
    f.append((char)(payload.size() & 0xFF));    // 脏点: 超 255 静默截断
    f.append((char)(g_seq++ & 0xFF));
    f.append((char)(cmd & 0xFF));
    f.append(payload);

    unsigned char chk = 0;
    for (int i = 2; i < f.size(); ++i)
        chk += (unsigned char)f.at(i);
    f.append((char)chk);
    return f;
}

int parseFrame(const QByteArray &buf, int *cmd, QByteArray *payload)
{
    if (buf.size() < FRAME_MIN_LEN)
        return 0;

    if ((unsigned char)buf.at(0) != FRAME_HEAD1 || (unsigned char)buf.at(1) != FRAME_HEAD2)
    {
        // 脏点: 帧头对不上就把整块缓冲丢掉, 不做滑动查找
        return -1;
    }

    int len = (unsigned char)buf.at(2);
    if (buf.size() < len + FRAME_MIN_LEN)
        return 0;

    unsigned char chk = 0;
    for (int i = 2; i < 5 + len; ++i)
        chk += (unsigned char)buf.at(i);

    if (chk != (unsigned char)buf.at(5 + len))
        return -2;

    if (cmd)
        *cmd = (unsigned char)buf.at(4);
    if (payload)
        *payload = buf.mid(5, len);

    return len + FRAME_MIN_LEN;
}

QString rcText(int rc)
{
    if (rc == RC_OK)
        return QString::fromUtf8("成功");
    if (rc == RC_BIN_EMPTY)
        return QString::fromUtf8("卡箱无卡");
    if (rc == RC_JAM)
        return QString::fromUtf8("卡片卡滞");
    if (rc == RC_WRITE_FAIL)
        return QString::fromUtf8("写卡失败");
    if (rc == RC_VERIFY_FAIL)
        return QString::fromUtf8("写后回读校验失败");
    if (rc == RC_TIMEOUT)
        return QString::fromUtf8("设备响应超时");
    if (rc == RC_BAD_STATE)
        return QString::fromUtf8("设备状态不允许");
    if (rc == RC_UNKNOWN_CMD)
        return QString::fromUtf8("不支持的命令");
    if (rc == RC_TAKE_TIMEOUT)
        return QString::fromUtf8("超时未取卡");
    if (rc == RC_ABORT)
        return QString::fromUtf8("流程中止");
    return QString::fromUtf8("未知结果(%1)").arg(rc);
}

QString devStateText(int st)
{
    if (st == DEV_IDLE)
        return QString::fromUtf8("待机");
    if (st == DEV_SELFTEST)
        return QString::fromUtf8("自检中");
    if (st == DEV_PREPARING)
        return QString::fromUtf8("备卡中");
    if (st == DEV_WRITING)
        return QString::fromUtf8("写卡中");
    if (st == DEV_PRESENTING)
        return QString::fromUtf8("伸卡中");
    if (st == DEV_WAIT_TAKE)
        return QString::fromUtf8("等待取卡");
    if (st == DEV_RETRACTING)
        return QString::fromUtf8("缩卡中");
    if (st == DEV_RECYCLING)
        return QString::fromUtf8("回收中");
    if (st == DEV_NO_CARD)
        return QString::fromUtf8("卡箱无卡");
    if (st == DEV_FAULT)
        return QString::fromUtf8("故障");
    return QString::fromUtf8("未知(%1)").arg(st);
}

QString hostStateText(int st)
{
    if (st == 0)
        return QString::fromUtf8("空闲(等车)");
    if (st == 1)
        return QString::fromUtf8("来车");
    if (st == 2)
        return QString::fromUtf8("备卡中");
    if (st == 3)
        return QString::fromUtf8("写卡中");
    if (st == 4)
        return QString::fromUtf8("伸卡中");
    if (st == 5)
        return QString::fromUtf8("等待司机取卡");
    if (st == 6)
        return QString::fromUtf8("已取卡");
    if (st == 7)
        return QString::fromUtf8("缩卡中");
    if (st == 8)
        return QString::fromUtf8("等待车辆驶离");
    if (st == 9)
        return QString::fromUtf8("回收卡片");
    if (st == 10)
        return QString::fromUtf8("卡箱无卡");
    if (st == 11)
        return QString::fromUtf8("故障");
    return QString::fromUtf8("未知(%1)").arg(st);
}

QString kindText(int kind)
{
    if (kind == 1)
        return QString::fromUtf8("一型客车");
    if (kind == 2)
        return QString::fromUtf8("二型客车");
    if (kind == 3)
        return QString::fromUtf8("一型货车");
    if (kind == 4)
        return QString::fromUtf8("二型货车");
    if (kind == 5)
        return QString::fromUtf8("三型货车");
    if (kind == 6)
        return QString::fromUtf8("专项作业车");
    return QString::fromUtf8("未知车型(%1)").arg(kind);
}

QString voiceText(int id)
{
    if (id == VOICE_PLEASE_TAKE)
        return QString::fromUtf8("请取卡");
    if (id == VOICE_GOOD_LUCK)
        return QString::fromUtf8("一路平安");
    if (id == VOICE_WAIT)
        return QString::fromUtf8("请稍候");
    if (id == VOICE_NO_CARD)
        return QString::fromUtf8("卡箱无卡, 请走人工车道");
    if (id == VOICE_RECYCLED)
        return QString::fromUtf8("卡片已回收");
    return QString::fromUtf8("语音%1").arg(id);
}

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo)
{
    EntryDataSt d;
    memset(&d, 0, sizeof(d));

    QByteArray st = station.left(4).toLocal8Bit();
    QByteArray ln = lane.left(2).toLocal8Bit();
    QByteArray pl = plate.left(8).toLocal8Bit();     // 脏点: 新能源车牌到这里就断了
    QByteArray tm = timeStr.left(14).toLocal8Bit();
    QByteArray cn = cardNo.left(8).toLocal8Bit();

    memcpy(d.station, st.constData(), qMin((int)sizeof(d.station), st.size()));
    memcpy(d.lane, ln.constData(), qMin((int)sizeof(d.lane), ln.size()));
    d.kind = (char)kind;
    memcpy(d.plate, pl.constData(), qMin((int)sizeof(d.plate), pl.size()));
    memcpy(d.time, tm.constData(), qMin((int)sizeof(d.time), tm.size()));
    memcpy(d.cardNo, cn.constData(), qMin((int)sizeof(d.cardNo), cn.size()));

    return QByteArray((const char *)&d, sizeof(d));
}

bool unpackEntryData(const QByteArray &p, QString *station, QString *lane, int *kind,
                     QString *plate, QString *timeStr, QString *cardNo)
{
    if (p.size() < (int)sizeof(EntryDataSt))
        return false;

    EntryDataSt d;
    memcpy(&d, p.constData(), sizeof(d));

    if (station)
        *station = QString::fromLocal8Bit(d.station, sizeof(d.station)).trimmed();
    if (lane)
        *lane = QString::fromLocal8Bit(d.lane, sizeof(d.lane)).trimmed();
    if (kind)
        *kind = (unsigned char)d.kind;
    if (plate)
        *plate = QString::fromLocal8Bit(d.plate, sizeof(d.plate)).trimmed();
    if (timeStr)
        *timeStr = QString::fromLocal8Bit(d.time, sizeof(d.time)).trimmed();
    if (cardNo)
        *cardNo = QString::fromLocal8Bit(d.cardNo, sizeof(d.cardNo)).trimmed();
    return true;
}
