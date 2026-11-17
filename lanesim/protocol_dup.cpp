#include "protocol_dup.h"

#include <QDebug>
#include <string.h>

// 脏点: 又一个全局序号
static int g_seq = 0;

struct EntryDataSt
{
    char station[4];
    char lane[2];
    char kind;
    char plate[8];
    char time[14];
    char cardNo[8];
    char pad[3];
};

QByteArray buildFrame(int cmd, const QByteArray &payload)
{
    QByteArray f;
    f.append((char)FRAME_HEAD1);
    f.append((char)FRAME_HEAD2);
    f.append((char)(payload.size() & 0xFF));
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
        return -1;

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
    // 脏点: 和上位机那份文案已经不一样了
    if (rc == RC_OK)
        return QString::fromUtf8("OK");
    if (rc == RC_NO_CARD)
        return QString::fromUtf8("NO_CARD");
    if (rc == RC_JAM)
        return QString::fromUtf8("JAM");
    if (rc == RC_WRITE_FAIL)
        return QString::fromUtf8("WRITE_FAIL");
    if (rc == RC_BAD_STATE)
        return QString::fromUtf8("BAD_STATE");
    if (rc == RC_NO_POS)
        return QString::fromUtf8("NO_POS");
    if (rc == RC_NOT_READY)
        return QString::fromUtf8("NOT_READY");
    return QString::fromUtf8("RC_%1").arg(rc);
}

QString posText(int pos)
{
    if (pos == POS_UP_LEFT)
        return QString::fromUtf8("UP-L");
    if (pos == POS_UP_RIGHT)
        return QString::fromUtf8("UP-R");
    if (pos == POS_DOWN_LEFT)
        return QString::fromUtf8("DN-L");
    if (pos == POS_DOWN_RIGHT)
        return QString::fromUtf8("DN-R");
    return QString::fromUtf8("POS%1").arg(pos);
}

QString posStateText(int st)
{
    if (st == PS_EMPTY)
        return QString::fromUtf8("EMPTY");
    if (st == PS_READY)
        return QString::fromUtf8("READY");
    if (st == PS_FAILED)
        return QString::fromUtf8("FAILED");
    if (st == PS_SENT)
        return QString::fromUtf8("SENT");
    if (st == PS_FAULT)
        return QString::fromUtf8("FAULT");
    return QString::fromUtf8("ST_%1").arg(st);
}

QString devSummaryText(int st)
{
    if (st == DS_NORMAL)
        return QString::fromUtf8("NORMAL");
    if (st == DS_PARTIAL)
        return QString::fromUtf8("PARTIAL");
    if (st == DS_ALLDOWN)
        return QString::fromUtf8("ALLDOWN");
    return QString::fromUtf8("DS_%1").arg(st);
}

QString eventText(int ev)
{
    if (ev == EV_CAR_ARRIVED)
        return QString::fromUtf8("car arrived");
    if (ev == EV_CAR_LEFT)
        return QString::fromUtf8("car left");
    if (ev == EV_CARD_TAKEN)
        return QString::fromUtf8("card taken");
    if (ev == EV_PREPARE_RECOVER)
        return QString::fromUtf8("prepare recovered");
    if (ev == EV_LEFT_NO_CARD)
        return QString::fromUtf8("left without card");
    if (ev == EV_POS_NO_CARD)
        return QString::fromUtf8("pos no card");
    if (ev == EV_POS_LOW)
        return QString::fromUtf8("pos low");
    if (ev == EV_POS_FAULT)
        return QString::fromUtf8("pos fault");
    return QString::fromUtf8("ev %1").arg(ev);
}

QString voiceText(int id)
{
    if (id == VOICE_PLEASE_TAKE)
        return QString::fromUtf8("please take card");
    if (id == VOICE_GOOD_LUCK)
        return QString::fromUtf8("have a safe trip");
    if (id == VOICE_WAIT)
        return QString::fromUtf8("please wait");
    if (id == VOICE_NO_CARD)
        return QString::fromUtf8("no card, use manual lane");
    if (id == VOICE_RECYCLED)
        return QString::fromUtf8("card recycled");
    return QString::fromUtf8("voice %1").arg(id);
}

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo)
{
    EntryDataSt d;
    memset(&d, 0, sizeof(d));

    QByteArray st = station.left(4).toLocal8Bit();
    QByteArray ln = lane.left(2).toLocal8Bit();
    QByteArray pl = plate.left(8).toLocal8Bit();
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
