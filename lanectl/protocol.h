#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QByteArray>
#include <QString>

// ============================================================================
//  高速公路入口自动发卡机 通信协议 v0
//  (上位机 = 车道控制软件, 下位机 = 发卡机 / 车检器 / 语音屏 / 栏杆)
//
//  帧格式:  AA 55 | LEN | SEQ | CMD | PAYLOAD | CHK
//    LEN : payload 长度, 1 字节
//    SEQ : 发送序号, 0~255 循环 —— 发出去, 但本版本没人校验
//    CHK : LEN..PAYLOAD 逐字节累加取低 8 位 —— 不是 CRC16
//
//  脏点: 命令和事件挤在一套 CMD 里, 没有"请求/响应"配对;
//        也没有心跳, 断了只能靠 socket 自己报错
// ============================================================================

#define FRAME_HEAD1       0xAA
#define FRAME_HEAD2       0x55
#define FRAME_MIN_LEN     6

// ------------------------- 上位机 -> 下位机 -------------------------
#define CMD_SELFTEST       0x01   // 自检
#define CMD_GET_STATUS     0x02   // 查状态
#define CMD_PREPARE_CARD   0x03   // 备卡: [1B 卡箱号]
#define CMD_WRITE_CARD     0x04   // 写卡: [40B 入口信息]
#define CMD_PRESENT_CARD   0x05   // 伸卡
#define CMD_RETRACT_CARD   0x06   // 缩卡
#define CMD_RECYCLE_CARD   0x07   // 回收: [1B 原因] 1=写卡失败 2=超时未取 3=人工
#define CMD_VOICE          0x08   // 语音: [1B 语音号]   (只回 ACK)
#define CMD_DISPLAY        0x09   // 屏显: [1B 行号][N 文本]  (只回 ACK)
#define CMD_BARRIER        0x0A   // 栏杆: [1B 0落 1抬]   (只回 ACK)
#define CMD_RESET          0x0B   // 复位
#define CMD_DEBUG          0x0C   // 调试: [1B 动作] 让模拟器造场景 (只回 ACK)

// 调试动作
#define DBG_CAR_ARRIVE     1      // 车压触发线圈
#define DBG_CAR_TAKE       2      // 司机取卡
#define DBG_CAR_LEAVE      3      // 车辆驶离
#define DBG_CAR_LEAVE_NO   4      // 未取卡驶离

// ------------------------- 下位机 -> 上位机 -------------------------
#define CMD_ACK            0x80   // [1B 结果码]
#define CMD_STATUS         0x81   // [1B 设备状态][1B 卡箱余量][1B 错误码][1B 卡槽位置]
#define CMD_EVENT          0x82   // [1B 事件码][1B 参数]  —— 真正的流程驱动靠它
#define CMD_CARD_DATA      0x83   // [8B 卡号]              —— 备卡成功后先发它, 再发事件
#define CMD_VEHICLE_DATA   0x84   // [8B 车牌][1B 车型]     —— 来车后先发它, 再发事件

// ------------------------- 执行结果 -------------------------
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

// ------------------------- 事件码 -------------------------
#define EV_CARD_READY      1     // 备卡到位 (卡已分到待发位)
#define EV_WRITE_DONE      2     // 写卡完成, 参数=结果码
#define EV_SLOT_OUT        3     // 卡槽伸出到位
#define EV_CARD_TAKEN      4     // 司机取走卡片
#define EV_SLOT_IN         5     // 卡槽缩回到位
#define EV_RECYCLED        6     // 卡片已回收
#define EV_CAR_ARRIVED     7     // 车辆压触发线圈 (来车)
#define EV_CAR_LEFT        8     // 车辆驶离 (存在线圈释放)
#define EV_LEFT_NO_CARD    9     // 未取卡就驶离
#define EV_BIN_EMPTY       10    // 卡箱无卡
#define EV_BIN_LOW         11    // 卡箱余量不足, 参数=剩余张数
#define EV_FAULT           12    // 设备故障, 参数=错误码
#define EV_SELFTEST_DONE   13    // 自检完成
#define EV_RESET_DONE      14    // 复位完成

// ------------------------- 卡槽位置 -------------------------
#define SLOT_IN            0
#define SLOT_OUT           1

// ------------------------- 回收原因 -------------------------
#define RECYCLE_WRITE_FAIL 1
#define RECYCLE_TIMEOUT    2
#define RECYCLE_MANUAL     3

// ------------------------- 设备状态 -------------------------
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

// ------------------------- 语音号 -------------------------
#define VOICE_PLEASE_TAKE  1     // 请取卡
#define VOICE_GOOD_LUCK    2     // 一路平安
#define VOICE_WAIT         3     // 请稍候
#define VOICE_NO_CARD      4     // 卡箱无卡, 请走人工车道
#define VOICE_RECYCLED     5     // 卡片已回收

// ---------------------------------------------------------------------------
// 组帧 / 解帧
// 脏点: 序号是文件内全局变量; 解帧用负数返回错误, 调用方到处 if (used < 0)
// ---------------------------------------------------------------------------
QByteArray buildFrame(int cmd, const QByteArray &payload);
int parseFrame(const QByteArray &buf, int *cmd, QByteArray *payload);

// ---------------------------------------------------------------------------
// 文案 (上位机和模拟器各写了一份, 已经不一致了)
// ---------------------------------------------------------------------------
QString rcText(int rc);
QString devStateText(int st);
QString hostStateText(int st);
QString kindText(int kind);
QString voiceText(int id);

// ---------------------------------------------------------------------------
// 入口信息 (写进 CPC 卡的内容) —— 定长 40 字节
// 脏点: 定长 + GBK, 新能源 8 位车牌存不下会被截断; 不判长度
// ---------------------------------------------------------------------------
#define ENTRY_DATA_LEN     40

QByteArray packEntryData(const QString &station, const QString &lane, int kind,
                         const QString &plate, const QString &timeStr, const QString &cardNo);
bool unpackEntryData(const QByteArray &p, QString *station, QString *lane, int *kind,
                     QString *plate, QString *timeStr, QString *cardNo);

#endif // PROTOCOL_H
