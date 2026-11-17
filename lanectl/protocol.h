#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QByteArray>
#include <QString>

// ============================================================================
//  高速公路入口自动发卡机器人 通信协议 v0
//  (A 阶段: 对齐四川现场业务 —— 4 工位 / B1·B2·B3 三类上行 / 心跳 / 时间同步)
//
//  帧格式:  AA 55 | LEN | SEQ | CMD | PAYLOAD | CHK
//    LEN : payload 长度, 1 字节
//    SEQ : 发送序号, 0~255 循环 —— 发出去, 但本版本没人校验
//    CHK : LEN..PAYLOAD 逐字节累加取低 8 位 —— 不是 CRC16
//
//  脏点: 真实现场用的是 FF..FF + 转义 + BCC, 这里还是自制的 AA55 帧;
//        帧格式的替换留到 W4 协议层重构
// ============================================================================

#define FRAME_HEAD1       0xAA
#define FRAME_HEAD2       0x55
#define FRAME_MIN_LEN     6

// ------------------------- 工位 (对应现场 上左/上右/下左/下右) -------------------------
#define POS_UP_LEFT       1
#define POS_UP_RIGHT      2
#define POS_DOWN_LEFT     3
#define POS_DOWN_RIGHT    4
#define POS_COUNT         4

// 工位状态 (B2 状态帧里每个工位占一个字节)
#define PS_EMPTY          0     // 空, 没有卡
#define PS_READY          1     // 已备卡, 可以送
#define PS_FAILED         2     // 备卡失败 / 卡箱无卡
#define PS_SENT           3     // 已送卡, 等司机取
#define PS_FAULT          4     // 机构故障

// 卡机整体状态
#define DS_NORMAL         0     // 正常
#define DS_PARTIAL        1     // 部分工位不可用
#define DS_ALLDOWN        2     // 全部不可用 / 整机故障

// ------------------------- 上位机(车道) -> 下位机(机器人) -------------------------
#define CMD_SELFTEST      0x01  // 自检
#define CMD_GET_STATUS    0x02  // 查状态 -> 回 B2
#define CMD_PREPARE_CARD  0x03  // 手动备卡: [1B 工位]   (正常情况下机器人自己备卡)
#define CMD_WRITE_CARD    0x04  // 写卡: [1B 工位][40B 入口信息]
#define CMD_PRESENT_CARD  0x05  // 送卡: [1B 工位]        (下工位先伸伸缩机构 20cm)
#define CMD_RETRACT_CARD  0x06  // 缩卡: [1B 工位]        (取卡后设备会自己缩)
#define CMD_RECYCLE_CARD  0x07  // 回收: [1B 工位][1B 原因]
#define CMD_VOICE         0x08  // 语音: [1B 语音号]
#define CMD_DISPLAY       0x09  // 屏显: [1B 行号][N 文本]
#define CMD_BARRIER       0x0A  // 栏杆: [1B 0落 1抬]
#define CMD_RESET         0x0B  // 复位
#define CMD_DEBUG         0x0C  // 调试: [1B 动作] 让模拟器造现场
#define CMD_TIME_SYNC     0x0D  // 时间同步: [14B yyyyMMddHHmmss]  (对应现场 C1)

// ------------------------- 下位机(机器人) -> 上位机(车道) -------------------------
#define CMD_RESULT        0xB1  // 执行结果: [1B 命令字][1B 结果码]   (对应现场 B1)
#define CMD_STATE         0xB2  // 状态: [1B 整体][4 x (1B 工位状态,1B 卡数)] (对应现场 B2)
#define CMD_EVENT         0xB3  // 事件: [1B 事件码][1B 工位]          (对应现场 B3)
#define CMD_CARD_DATA     0xB4  // 备卡成功: [1B 工位][8B 卡号]
#define CMD_VEHICLE_DATA  0xB5  // 来车数据: [8B 车牌][1B 车型]
#define CMD_HEARTBEAT     0xA0  // 心跳: [1B 序号]                      (设备每 20s 主动发)

// ------------------------- 执行结果码 -------------------------
#define RC_OK             0
#define RC_NO_CARD        1     // 工位无卡 / 卡箱空
#define RC_JAM            2     // 卡片卡滞
#define RC_WRITE_FAIL     3     // 写卡失败
#define RC_VERIFY_FAIL    4     // 写后回读校验失败
#define RC_TIMEOUT        5
#define RC_BAD_STATE      6     // 状态不允许
#define RC_UNKNOWN_CMD    7
#define RC_NO_POS         8     // 工位号不存在
#define RC_NOT_READY      9     // 该工位还没备卡, 不能送

// ------------------------- 事件码 (B3) -------------------------
#define EV_CAR_ARRIVED    1     // 车辆压触发线圈
#define EV_CAR_LEFT       2     // 车辆驶离
#define EV_CARD_TAKEN     3     // 司机取卡 (参数=工位)  ← 现场 event=3
#define EV_PREPARE_RECOVER 4    // 备卡恢复 (回收/失败后重新备好, 通知车道可以继续)
#define EV_LEFT_NO_CARD   5     // 未取卡驶离
#define EV_POS_NO_CARD    6     // 工位卡箱无卡 (参数=工位)
#define EV_POS_LOW        7     // 工位余量不足 (参数=工位)
#define EV_POS_FAULT      8     // 工位机构故障 (参数=工位)

// ------------------------- 伸缩机构 -------------------------
#define STRETCH_IN        0     // 缩回
#define STRETCH_OUT       1     // 伸出 (20cm)

// ------------------------- 回收原因 -------------------------
#define RECYCLE_WRITE_FAIL 1
#define RECYCLE_TIMEOUT    2
#define RECYCLE_MANUAL     3

// ------------------------- 调试动作 -------------------------
#define DBG_CAR_ARRIVE    1
#define DBG_CAR_TAKE      2
#define DBG_CAR_LEAVE     3
#define DBG_CAR_LEAVE_NO  4
#define DBG_LANE_CLEAR    5

// ------------------------- 语音号 -------------------------
#define VOICE_PLEASE_TAKE 1
#define VOICE_GOOD_LUCK   2
#define VOICE_WAIT        3
#define VOICE_NO_CARD     4
#define VOICE_RECYCLED    5

// ---------------------------------------------------------------------------
QByteArray buildFrame(int cmd, const QByteArray &payload);
int parseFrame(const QByteArray &buf, int *cmd, QByteArray *payload);

QString rcText(int rc);
QString posText(int pos);
QString posStateText(int st);
QString devSummaryText(int st);
QString eventText(int ev);
QString hostStateText(int st);
QString kindText(int kind);
QString voiceText(int id);

// ---------------------------------------------------------------------------
// 入口信息 (写进 CPC 卡的内容) —— 定长 40 字节
// 脏点: 定长 + GBK, 新能源 8 位车牌存不下会被截断
// ---------------------------------------------------------------------------
#define ENTRY_DATA_LEN     40

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo);
bool unpackEntryData(const QByteArray &p, QString *station, QString *lane, int *kind,
                     QString *plate, QString *timeStr, QString *cardNo);

#endif // PROTOCOL_H
