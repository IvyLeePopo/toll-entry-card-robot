#ifndef PROTOCOL_DUP_H
#define PROTOCOL_DUP_H

#include <QByteArray>
#include <QString>

// ============================================================================
//  注意: 这个文件是 lanectl/protocol.h 的**拷贝**
//
//  脏点: 上位机一套组帧/解帧, 模拟器又抄了一套, 已经分叉:
//        - STATUS 上位机文档写 3 字节, 这里实际发 4 字节
//        - rcText 的文案两边不一样, 出问题对日志时对不上
//   -> W4 抽成真正共用的 protocol 库, 两边链接同一个
// ============================================================================

#define FRAME_HEAD1       0xAA
#define FRAME_HEAD2       0x55
#define FRAME_MIN_LEN     6

#define CMD_SELFTEST       0x01
#define CMD_GET_STATUS     0x02
#define CMD_PREPARE_CARD   0x03
#define CMD_WRITE_CARD     0x04
#define CMD_PRESENT_CARD   0x05
#define CMD_RETRACT_CARD   0x06
#define CMD_RECYCLE_CARD   0x07
#define CMD_VOICE          0x08
#define CMD_DISPLAY        0x09
#define CMD_BARRIER        0x0A
#define CMD_RESET          0x0B
#define CMD_DEBUG          0x0C

#define DBG_CAR_ARRIVE     1
#define DBG_CAR_TAKE       2
#define DBG_CAR_LEAVE      3
#define DBG_CAR_LEAVE_NO   4
#define DBG_LANE_CLEAR     5

#define CMD_ACK            0x80
#define CMD_STATUS         0x81
#define CMD_EVENT          0x82
#define CMD_CARD_DATA      0x83
#define CMD_VEHICLE_DATA   0x84

#define RC_OK              0
#define RC_BIN_EMPTY       1
#define RC_JAM             2
#define RC_WRITE_FAIL      3
#define RC_VERIFY_FAIL     4
#define RC_TIMEOUT         5
#define RC_BAD_STATE       6
#define RC_UNKNOWN_CMD     7
#define RC_TAKE_TIMEOUT    8
#define RC_ABORT           9

#define EV_CARD_READY      1
#define EV_WRITE_DONE      2
#define EV_SLOT_OUT        3
#define EV_CARD_TAKEN      4
#define EV_SLOT_IN         5
#define EV_RECYCLED        6
#define EV_CAR_ARRIVED     7
#define EV_CAR_LEFT        8
#define EV_LEFT_NO_CARD    9
#define EV_BIN_EMPTY       10
#define EV_BIN_LOW         11
#define EV_FAULT           12
#define EV_SELFTEST_DONE   13
#define EV_RESET_DONE      14

#define SLOT_IN            0
#define SLOT_OUT           1

#define RECYCLE_WRITE_FAIL 1
#define RECYCLE_TIMEOUT    2
#define RECYCLE_MANUAL     3

#define DEV_IDLE           0
#define DEV_SELFTEST       1
#define DEV_PREPARING      2
#define DEV_WRITING        3
#define DEV_PRESENTING     4
#define DEV_WAIT_TAKE      5
#define DEV_RETRACTING     6
#define DEV_RECYCLING      7
#define DEV_NO_CARD        8
#define DEV_FAULT          9

#define VOICE_PLEASE_TAKE  1
#define VOICE_GOOD_LUCK    2
#define VOICE_WAIT         3
#define VOICE_NO_CARD      4
#define VOICE_RECYCLED     5

#define ENTRY_DATA_LEN     40

QByteArray buildFrame(int cmd, const QByteArray &payload);
int parseFrame(const QByteArray &buf, int *cmd, QByteArray *payload);

QString rcText(int rc);
QString devStateText(int st);
QString voiceText(int id);

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo);
bool unpackEntryData(const QByteArray &p, QString *station, QString *lane, int *kind,
                     QString *plate, QString *timeStr, QString *cardNo);

#endif // PROTOCOL_DUP_H
