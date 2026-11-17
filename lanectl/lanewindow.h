#ifndef LANEWINDOW_H
#define LANEWINDOW_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QTcpSocket>
#include <QWidget>

#include "vehicle.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTimer;
class StatusPoller;

// 车道流程状态 (上位机自己的)
enum
{
    LS_IDLE = 0,        // 无车, 等来车
    LS_ARRIVED,         // 来车, 已选好工位
    LS_WRITING,         // 已下发写卡
    LS_PRESENTING,      // 已下发送卡
    LS_WAIT_TAKE,       // 卡已送出, 等司机取
    LS_TAKEN,           // 司机已取卡
    LS_WAIT_LEAVE,      // 等车辆驶离
    LS_RECOVERING,      // 回收
    LS_NOPOS,           // 没有可用工位
    LS_FAULT            // 故障
};

// ============================================================================
//  车道控制主窗口 —— v0 脏代码基线 (A 阶段: 4 工位)
//
//  真实车道上, 上位机只做三件事: 选工位 -> 写卡 -> 送卡。
//  备卡是机器人自己维持的(送完自动补备, 失败切配对工位), 车道不参与。
//
//  脏的地方:
//    * 界面 / 通信 / 协议 / 车道流程 / 流水 全在一个类里
//    * 工位选择、结果分发都是 if/else 链
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
    void onHeartCheck();

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
    void onRecoverFault();

    // ---- 模拟现场 ----
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
    void updatePosTable();
    void appendFlowRow(const VehicleRecord &rec);

    // ---- 通信 ----
    void sendCmd(int cmd, const QByteArray &payload);
    void sendFire(int cmd, const QByteArray &payload);
    void sendDebug(int action);
    void onResult(int cmd, int rc);
    void onState(const QByteArray &payload);
    void onEvent(int ev, int pos);

    // ---- 车道流程 ----
    int pickPosition();
    void startWrite();
    void startPresent();
    void doRecycle(int reason);
    void finishVehicle(int result, const QString &remark);
    void enterFault(int rc, const QString &what);
    void finishRecover();
    void showScreen(const QString &text);
    void playVoice(int id);
    void setState(int st);

    // ---- 通信 ----
    QTcpSocket *m_sock;
    QTimer *m_cmdTimer;        // 命令超时 8 秒 (脏: 所有命令共用一个)
    QTimer *m_takeTimer;       // 取卡超时 15 秒
    QTimer *m_heartTimer;      // 心跳检查 5 秒
    StatusPoller *m_poller;
    QByteArray m_buf;
    char *m_txBuf;             // 手工 new 的发送缓冲
    QDateTime m_lastHeart;

    // ---- 界面 ----
    QLineEdit *m_stationEdit;
    QLineEdit *m_laneEdit;
    QLineEdit *m_ipEdit;
    QLineEdit *m_portEdit;
    QPushButton *m_connBtn;
    QPushButton *m_discBtn;

    QLabel *m_devLbl;
    QLabel *m_heartLbl;
    QLabel *m_barrierLbl;
    QLabel *m_alarmLbl;

    QLabel *m_coilA;
    QLabel *m_coilB;
    QLabel *m_slotIcon;
    QLabel *m_barrierIcon;
    QLabel *m_curPosLbl;
    QLabel *m_screen;

    QTableWidget *m_posTable;   // 4 工位状态表

    QLabel *m_seqLbl;
    QLabel *m_plateLbl;
    QLabel *m_kindLbl;
    QLabel *m_timeLbl;
    QLabel *m_cardLbl;
    QLabel *m_stateLbl;

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
    QPushButton *m_bRecover;
    QPushButton *m_bSimCar;
    QPushButton *m_bSimTake;
    QPushButton *m_bSimLeave;

    // ---- 业务 ----
    VehicleRecord m_cur;
    QList<VehicleRecord> m_flows;
    int m_state;
    int m_seq;
    int m_retry;
    int m_voiceCount;
    bool m_carPresent;
    bool m_barrierUp;
    bool m_recovering;

    // ---- 设备侧 ----
    int m_devState;            // 整体状态 DS_*
    int m_posState[5];         // 1..4 工位状态 PS_*
    int m_posBin[5];           // 1..4 卡数
    QString m_posCard[5];      // 1..4 当前卡号 (B4 上报)
    int m_curPos;              // 本单车在用哪个工位
    int m_stretch;             // 伸缩机构

    QString m_vehPlate;
    int m_vehKind;
};

#endif // LANEWINDOW_H
