#ifndef PROTOCOL_DUP_H
#define PROTOCOL_DUP_H

#include <QByteArray>
#include <QString>

// ============================================================================
//  注意: 这个文件是 lanectl/protocol.h 的**拷贝**
//
//  脏点: 上位机一套组帧/解帧, 模拟器又抄了一套, 已经分叉:
//        - rcText 文案两边不一样, 出问题对日志时对不上
//        - 端口的默认值、超时时长两边各写各的
//   -> W4 抽成真正共用的 protocol 库, 两边链接同一个
// ============================================================================

#define FRAME_HEAD1       0xAA
#define FRAME_HEAD2       0x55
#define FRAME_MIN_LEN     6

#define POS_UP_LEFT       1
#define POS_UP_RIGHT      2
#define POS_DOWN_LEFT     3
#define POS_DOWN_RIGHT    4
#define POS_COUNT         4

#define PS_EMPTY          0
#define PS_READY          1
#define PS_FAILED         2
#define PS_SENT           3
#define PS_FAULT          4

#define DS_NORMAL         0
#define DS_PARTIAL        1
#define DS_ALLDOWN        2

#define CMD_SELFTEST      0x01
#define CMD_GET_STATUS    0x02
#define CMD_PREPARE_CARD  0x03
#define CMD_WRITE_CARD    0x04
#define CMD_PRESENT_CARD  0x05
#define CMD_RETRACT_CARD  0x06
#define CMD_RECYCLE_CARD  0x07
#define CMD_VOICE         0x08
#define CMD_DISPLAY       0x09
#define CMD_BARRIER       0x0A
#define CMD_RESET         0x0B
#define CMD_DEBUG         0x0C
#define CMD_TIME_SYNC     0x0D

#define CMD_RESULT        0xB1
#define CMD_STATE         0xB2
#define CMD_EVENT         0xB3
#define CMD_CARD_DATA     0xB4
#define CMD_VEHICLE_DATA  0xB5
#define CMD_HEARTBEAT     0xA0

#define RC_OK             0
#define RC_NO_CARD        1
#define RC_JAM            2
#define RC_WRITE_FAIL     3
#define RC_VERIFY_FAIL    4
#define RC_TIMEOUT        5
#define RC_BAD_STATE      6
#define RC_UNKNOWN_CMD    7
#define RC_NO_POS         8
#define RC_NOT_READY      9
#define RC_ABORT          10

#define EV_CAR_ARRIVED    1
#define EV_CAR_LEFT       2
#define EV_CARD_TAKEN     3
#define EV_PREPARE_RECOVER 4
#define EV_LEFT_NO_CARD   5
#define EV_POS_NO_CARD    6
#define EV_POS_LOW        7
#define EV_POS_FAULT      8

#define STRETCH_IN        0
#define STRETCH_OUT       1

#define RECYCLE_WRITE_FAIL 1
#define RECYCLE_TIMEOUT    2
#define RECYCLE_MANUAL     3

#define DBG_CAR_ARRIVE    1
#define DBG_CAR_TAKE      2
#define DBG_CAR_LEAVE     3
#define DBG_CAR_LEAVE_NO  4
#define DBG_LANE_CLEAR    5

#define VOICE_PLEASE_TAKE 1
#define VOICE_GOOD_LUCK   2
#define VOICE_WAIT        3
#define VOICE_NO_CARD     4
#define VOICE_RECYCLED    5

#define ENTRY_DATA_LEN     40

QByteArray buildFrame(int cmd, const QByteArray &payload);
int parseFrame(const QByteArray &buf, int *cmd, QByteArray *payload);

QString rcText(int rc);
QString posText(int pos);
QString posStateText(int st);
QString devSummaryText(int st);
QString eventText(int ev);
QString voiceText(int id);

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo);
bool unpackEntryData(const QByteArray &p, QString *station, QString *lane, int *kind,
                     QString *plate, QString *timeStr, QString *cardNo);

#endif // PROTOCOL_DUP_H
