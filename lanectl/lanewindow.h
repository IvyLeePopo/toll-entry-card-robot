#ifndef LANEWINDOW_H
#define LANEWINDOW_H

#include <QByteArray>
#include <QList>
#include <QTcpSocket>
#include <QWidget>

#include "vehicle.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTimer;
class StatusPoller;

// 车道流程状态 (上位机自己的, 和设备状态是两套)
enum
{
    LS_IDLE = 0,        // 无车, 等来车
    LS_ARRIVED,         // 来车, 已收到车辆信息
    LS_PREPARING,       // 已下发备卡
    LS_WRITING,         // 已下发写卡
    LS_PRESENTING,      // 已下发伸卡
    LS_WAIT_TAKE,       // 卡槽已伸出, 等司机取卡
    LS_TAKEN,           // 司机已取卡, 播报中
    LS_RETRACTING,      // 已下发缩卡
    LS_WAIT_LEAVE,      // 等车辆驶离 (存在线圈释放)
    LS_RECOVERING,      // 回收卡片
    LS_NOCARD,          // 卡箱无卡
    LS_FAULT            // 故障
};

// ============================================================================
//  车道控制主窗口 —— v0 脏代码基线
//
//  真实车道上, 上位机是**事件驱动**的: 车压地感 -> 备卡 -> 写卡 -> 伸卡
//  -> 等取卡 -> 缩卡 -> 放行。人只负责处理异常, 不参与日常发卡。
//
//  脏的地方:
//    * 界面 / 通信 / 协议 / 车道流程 / 流水 全在一个类里
//    * 流程推进靠 onEvent() 里的 if/else 链 + m_state 数字
//    * 事件到达时不校验"当前状态是否合法"
//    * 手动按钮直接下发命令, 绕过状态机
//    * 裸指针、写死超时、只在 Windows 上能跑
// ============================================================================
class LaneWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LaneWindow(QWidget *parent = 0);
    ~LaneWindow();

private slots:
    // ---- 连接 ----
    void onConnect();
    void onDisconnect();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSockError(QAbstractSocket::SocketError e);
    void onPollTick();

    // ---- 定时器 ----
    void onCmdTimeout();
    void onTakeTimeout();

    // ---- 手动调试 ----
    void onManualSelftest();
    void onManualPrepare();
    void onManualWrite();
    void onManualPresent();
    void onManualRetract();
    void onManualRecycle();
    void onManualVoice();
    void onManualBarrierUp();
    void onManualBarrierDown();
    void onResetDev();
    void onClearFlow();

    // ---- 模拟现场 (只对 LaneSim 有效) ----
    void onSimCarArrive();
    void onSimTakeCard();
    void onSimCarLeave();

private:
    // ---- 界面 ----
    void buildUi();
    void log(const QString &s);
    void updateHead();
    void updateVehiclePanel();
    void updateLaneView();
    void appendFlowRow(const VehicleRecord &rec);

    // ---- 通信 ----
    void sendCmd(int cmd, const QByteArray &payload);   // 需要等 EVENT
    void sendFire(int cmd, const QByteArray &payload);  // 只回 ACK, 不等
    void sendDebug(int action);
    void onEvent(int ev, int param);

    // ---- 车道流程 ----
    void startPrepare();
    void startWrite();
    void startPresent();
    void startRetract();
    void doRecycle(int reason);
    void finishVehicle(int result, const QString &remark);
    void enterFault(int rc, const QString &what);
    void showScreen(const QString &text);
    void playVoice(int id);
    void setState(int st);

    // ---- 通信 ----
    QTcpSocket *m_sock;
    QTimer *m_cmdTimer;        // 命令超时 8 秒 (脏: 所有命令共用一个)
    QTimer *m_takeTimer;       // 取卡超时 15 秒
    StatusPoller *m_poller;
    QByteArray m_buf;
    char *m_txBuf;             // 手工 new 的发送缓冲

    // ---- 界面 ----
    QLineEdit *m_stationEdit;
    QLineEdit *m_laneEdit;
    QLineEdit *m_ipEdit;
    QLineEdit *m_portEdit;
    QPushButton *m_connBtn;
    QPushButton *m_discBtn;

    QLabel *m_devLbl;
    QLabel *m_binLbl;
    QLabel *m_slotLbl;
    QLabel *m_barrierLbl;
    QLabel *m_alarmLbl;

    QLabel *m_coilA;
    QLabel *m_coilB;
    QLabel *m_slotIcon;
    QLabel *m_barrierIcon;
    QProgressBar *m_binBar;

    QLabel *m_seqLbl;
    QLabel *m_plateLbl;
    QLabel *m_kindLbl;
    QLabel *m_timeLbl;
    QLabel *m_cardLbl;
    QLabel *m_stateLbl;
    QLabel *m_screen;

    QTableWidget *m_table;
    QTextEdit *m_log;

    QPushButton *m_bSelfTest;
    QPushButton *m_bPrepare;
    QPushButton *m_bWrite;
    QPushButton *m_bPresent;
    QPushButton *m_bRetract;
    QPushButton *m_bRecycle;
    QPushButton *m_bVoice;
    QPushButton *m_bUp;
    QPushButton *m_bDown;
    QPushButton *m_bReset;
    QPushButton *m_bClear;
    QPushButton *m_bSimCar;
    QPushButton *m_bSimTake;
    QPushButton *m_bSimLeave;

    // ---- 业务 ----
    VehicleRecord m_cur;       // 当前这辆车
    QList<VehicleRecord> m_flows;   // 流水 (值拷贝, 脏: 全量重绘)
    int m_state;
    int m_seq;                 // 流水号
    int m_retry;               // 写卡重试次数
    int m_voiceCount;          // 已播报"请取卡"次数
    bool m_carPresent;
    bool m_barrierUp;

    int m_devState;
    int m_binLeft;
    int m_errCode;
    int m_slotPos;

    QString m_vehPlate;        // 最近一次车辆信息帧
    int m_vehKind;
    QString m_lastCardNo;      // 最近一次卡号帧
    QString m_screenText;
};

#endif // LANEWINDOW_H
