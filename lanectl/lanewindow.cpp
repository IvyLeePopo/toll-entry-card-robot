#include "lanewindow.h"
#include "logger.h"
#include "protocol.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <string.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// 静态小工具: 点灯
static void setLamp(QLabel *l, bool on)
{
    l->setStyleSheet(on ? "color:#d40000; font-size:20px;"
                        : "color:#c0c0c0; font-size:20px;");
}

// ============================================================================
//  状态轮询线程
//  脏点:
//    1. 为了每 2 秒查一次状态专门开一个线程, 一个 QTimer 就够了
//    2. volatile bool 当停止标志, 不是同步原语
//    3. 停线程要等 sleep 醒来
//   -> W1 用线程池 + std::atomic 换掉
// ============================================================================
class StatusPoller : public QThread
{
public:
    explicit StatusPoller(LaneWindow *w) : m_w(w), m_run(true) {}
    void stop() { m_run = false; }

protected:
    void run()
    {
        while (m_run)
        {
            QThread::msleep(2000);
            if (!m_run)
                break;
            QMetaObject::invokeMethod(m_w, "onPollTick", Qt::QueuedConnection);
        }
    }

private:
    LaneWindow *m_w;
    volatile bool m_run;
};

// ----------------------------------------------------------------------------

LaneWindow::LaneWindow(QWidget *parent)
    : QWidget(parent)
{
    // 脏点: 成员初始化散在构造函数 / buildUi / onConnected 三处
    m_sock = new QTcpSocket(this);

    m_cmdTimer = new QTimer(this);
    m_cmdTimer->setSingleShot(true);
    m_takeTimer = new QTimer(this);
    m_takeTimer->setSingleShot(true);

    m_txBuf = new char[512];          // 脏点: 手工缓冲, 长度写死
    memset(m_txBuf, 0, 512);

    m_poller = new StatusPoller(this);

    m_state = LS_IDLE;
    m_seq = 0;
    m_retry = 0;
    m_voiceCount = 0;
    m_carPresent = false;

    m_devState = -1;
    m_binLeft = -1;
    m_errCode = -1;
    m_slotPos = -1;
    m_vehKind = 0;
    m_barrierUp = false;
    m_recovering = false;

    buildUi();

    connect(m_sock, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(m_sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_sock, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onSockError(QAbstractSocket::SocketError)));

    connect(m_cmdTimer, SIGNAL(timeout()), this, SLOT(onCmdTimeout()));
    connect(m_takeTimer, SIGNAL(timeout()), this, SLOT(onTakeTimeout()));

    connect(m_connBtn, SIGNAL(clicked()), this, SLOT(onConnect()));
    connect(m_discBtn, SIGNAL(clicked()), this, SLOT(onDisconnect()));

    connect(m_bSelfTest, SIGNAL(clicked()), this, SLOT(onManualSelftest()));
    connect(m_bPrepare, SIGNAL(clicked()), this, SLOT(onManualPrepare()));
    connect(m_bWrite, SIGNAL(clicked()), this, SLOT(onManualWrite()));
    connect(m_bPresent, SIGNAL(clicked()), this, SLOT(onManualPresent()));
    connect(m_bRetract, SIGNAL(clicked()), this, SLOT(onManualRetract()));
    connect(m_bRecycle, SIGNAL(clicked()), this, SLOT(onManualRecycle()));
    connect(m_bVoice, SIGNAL(clicked()), this, SLOT(onManualVoice()));
    connect(m_bUp, SIGNAL(clicked()), this, SLOT(onManualBarrierUp()));
    connect(m_bDown, SIGNAL(clicked()), this, SLOT(onManualBarrierDown()));
    connect(m_bReset, SIGNAL(clicked()), this, SLOT(onResetDev()));
    connect(m_bClear, SIGNAL(clicked()), this, SLOT(onClearFlow()));
    connect(m_bRecover, SIGNAL(clicked()), this, SLOT(onRecoverFault()));

    connect(m_bSimCar, SIGNAL(clicked()), this, SLOT(onSimCarArrive()));
    connect(m_bSimTake, SIGNAL(clicked()), this, SLOT(onSimTakeCard()));
    connect(m_bSimLeave, SIGNAL(clicked()), this, SLOT(onSimCarLeave()));

    m_poller->start();

    showScreen(QString::fromUtf8("欢迎光临"));
    log(QString::fromUtf8("车道控制软件启动. 先启动 LaneSim, 再点[连接]"));
    log(QString::fromUtf8("提示: 来车要在 LaneSim 控制台里敲 car (或开自动来车)"));
}

LaneWindow::~LaneWindow()
{
    // 脏点: 关程序时设备可能还在动, 不回收卡也不落杆
    if (m_poller)
    {
        m_poller->stop();
        m_poller->wait(3000);
    }
    delete[] m_txBuf;
}

// ============================================================================
//  界面
// ============================================================================

void LaneWindow::buildUi()
{
    // 脏点: 界面纯手写, 一半是控件创建
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ---------------- 第一行: 车道参数 + 状态 ----------------
    QHBoxLayout *top = new QHBoxLayout;

    top->addWidget(new QLabel(QString::fromUtf8("站号:")));
    m_stationEdit = new QLineEdit(QString::fromUtf8("0512"));
    m_stationEdit->setFixedWidth(60);
    top->addWidget(m_stationEdit);

    top->addWidget(new QLabel(QString::fromUtf8("车道:")));
    m_laneEdit = new QLineEdit(QString::fromUtf8("03"));
    m_laneEdit->setFixedWidth(40);
    top->addWidget(m_laneEdit);

    top->addSpacing(8);
    top->addWidget(new QLabel(QString::fromUtf8("IP:")));
    m_ipEdit = new QLineEdit(QString::fromUtf8("127.0.0.1"));
    m_ipEdit->setFixedWidth(100);
    top->addWidget(m_ipEdit);

    top->addWidget(new QLabel(QString::fromUtf8("端口:")));
    m_portEdit = new QLineEdit(QString::fromUtf8("6000"));
    m_portEdit->setFixedWidth(55);
    top->addWidget(m_portEdit);

    m_connBtn = new QPushButton(QString::fromUtf8("连接"));
    m_discBtn = new QPushButton(QString::fromUtf8("断开"));
    m_discBtn->setEnabled(false);
    top->addWidget(m_connBtn);
    top->addWidget(m_discBtn);

    top->addSpacing(12);
    m_devLbl = new QLabel(QString::fromUtf8("设备: 未连接"));
    m_binLbl = new QLabel(QString::fromUtf8("卡箱: -"));
    m_slotLbl = new QLabel(QString::fromUtf8("卡槽: -"));
    m_barrierLbl = new QLabel(QString::fromUtf8("栏杆: -"));

    QFont bold = m_devLbl->font();
    bold.setBold(true);
    m_devLbl->setFont(bold);

    top->addWidget(m_devLbl);
    top->addWidget(m_binLbl);
    top->addWidget(m_slotLbl);
    top->addWidget(m_barrierLbl);
    top->addStretch();
    root->addLayout(top);

    // ---------------- 第二行: 三栏 ----------------
    QHBoxLayout *mid = new QHBoxLayout;

    // ---- 车道示意 ----
    QGroupBox *laneBox = new QGroupBox(QString::fromUtf8("车道示意"));
    QVBoxLayout *laneLay = new QVBoxLayout(laneBox);

    QGridLayout *lg = new QGridLayout;

    lg->addWidget(new QLabel(QString::fromUtf8("触发线圈 A")), 0, 0);
    m_coilA = new QLabel(QString::fromUtf8("●"));
    setLamp(m_coilA, false);
    lg->addWidget(m_coilA, 0, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("发卡机卡槽")), 1, 0);
    m_slotIcon = new QLabel(QString::fromUtf8("──  缩回"));
    m_slotIcon->setStyleSheet("font-size:16px; color:#555;");
    lg->addWidget(m_slotIcon, 1, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("栏杆")), 2, 0);
    m_barrierIcon = new QLabel(QString::fromUtf8("▬  落杆"));
    m_barrierIcon->setStyleSheet("font-size:16px; color:#555;");
    lg->addWidget(m_barrierIcon, 2, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("存在线圈 B")), 3, 0);
    m_coilB = new QLabel(QString::fromUtf8("●"));
    setLamp(m_coilB, false);
    lg->addWidget(m_coilB, 3, 1);

    laneLay->addLayout(lg);

    laneLay->addWidget(new QLabel(QString::fromUtf8("信息屏 (LED):")));
    m_screen = new QLabel(QString::fromUtf8("欢迎光临"));
    m_screen->setAlignment(Qt::AlignCenter);
    m_screen->setWordWrap(true);
    m_screen->setMinimumHeight(80);
    m_screen->setStyleSheet(
        "background:#101010; color:#33ff66; border:1px solid #333;"
        "font-family:'Microsoft YaHei'; font-size:20px; font-weight:bold;");
    laneLay->addWidget(m_screen);

    laneLay->addSpacing(6);
    laneLay->addWidget(new QLabel(QString::fromUtf8("卡箱余量:")));
    m_binBar = new QProgressBar;
    m_binBar->setRange(0, 500);
    m_binBar->setValue(0);
    m_binBar->setFormat(QString::fromUtf8("%v / 500"));
    laneLay->addWidget(m_binBar);
    laneLay->addStretch();

    mid->addWidget(laneBox, 3);

    // ---- 当前车辆 ----
    QGroupBox *vehBox = new QGroupBox(QString::fromUtf8("当前车辆"));
    QGridLayout *vg = new QGridLayout(vehBox);

    QFont big = font();
    big.setPointSize(14);
    big.setBold(true);

    vg->addWidget(new QLabel(QString::fromUtf8("流水号:")), 0, 0);
    m_seqLbl = new QLabel("-");
    vg->addWidget(m_seqLbl, 0, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("车牌:")), 1, 0);
    m_plateLbl = new QLabel("-");
    m_plateLbl->setFont(big);
    m_plateLbl->setStyleSheet("color:#0044aa;");
    vg->addWidget(m_plateLbl, 1, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("车型:")), 2, 0);
    m_kindLbl = new QLabel("-");
    vg->addWidget(m_kindLbl, 2, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("来车时间:")), 3, 0);
    m_timeLbl = new QLabel("-");
    vg->addWidget(m_timeLbl, 3, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("CPC 卡号:")), 4, 0);
    m_cardLbl = new QLabel("-");
    m_cardLbl->setFont(big);
    m_cardLbl->setStyleSheet("color:#660099;");
    vg->addWidget(m_cardLbl, 4, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("车道流程:")), 5, 0);
    m_stateLbl = new QLabel(hostStateText(LS_IDLE));
    m_stateLbl->setStyleSheet("color:#c07000; font-weight:bold;");
    vg->addWidget(m_stateLbl, 5, 1);

    vg->addWidget(new QLabel(QString::fromUtf8("报警:")), 6, 0);
    m_alarmLbl = new QLabel(QString::fromUtf8("无"));
    m_alarmLbl->setStyleSheet("color:#0a0;");
    m_alarmLbl->setWordWrap(true);
    vg->addWidget(m_alarmLbl, 6, 1);

    vg->setRowStretch(7, 1);

    mid->addWidget(vehBox, 3);

    // ---- 手动控制 ----
    QGroupBox *manBox = new QGroupBox(QString::fromUtf8("手动控制 (单步 / 排障)"));
    QGridLayout *mg = new QGridLayout(manBox);

    m_bSelfTest = new QPushButton(QString::fromUtf8("自检"));
    m_bPrepare = new QPushButton(QString::fromUtf8("备卡"));
    m_bWrite = new QPushButton(QString::fromUtf8("写卡"));
    m_bPresent = new QPushButton(QString::fromUtf8("伸卡"));
    m_bRetract = new QPushButton(QString::fromUtf8("缩卡"));
    m_bRecycle = new QPushButton(QString::fromUtf8("回收"));
    m_bVoice = new QPushButton(QString::fromUtf8("播报"));
    m_bUp = new QPushButton(QString::fromUtf8("抬杆"));
    m_bDown = new QPushButton(QString::fromUtf8("落杆"));
    m_bReset = new QPushButton(QString::fromUtf8("复位"));
    m_bClear = new QPushButton(QString::fromUtf8("清流水"));

    mg->addWidget(m_bSelfTest, 0, 0);
    mg->addWidget(m_bPrepare, 0, 1);
    mg->addWidget(m_bWrite, 1, 0);
    mg->addWidget(m_bPresent, 1, 1);
    mg->addWidget(m_bRetract, 2, 0);
    mg->addWidget(m_bRecycle, 2, 1);
    mg->addWidget(m_bVoice, 3, 0);
    mg->addWidget(m_bUp, 3, 1);
    mg->addWidget(m_bDown, 4, 0);
    mg->addWidget(m_bReset, 4, 1);
    mg->addWidget(m_bClear, 5, 0, 1, 2);

    m_bRecover = new QPushButton(QString::fromUtf8("故障恢复 / 跳过本车"));
    m_bRecover->setStyleSheet("font-weight:bold; color:#c00;");
    m_bRecover->setMinimumHeight(32);
    mg->addWidget(m_bRecover, 6, 0, 1, 2);

    mg->setRowStretch(7, 1);

    // ---- 模拟现场 (录 demo / 单机联调用) ----
    QGroupBox *simBox = new QGroupBox(QString::fromUtf8("模拟现场 (只对 LaneSim 有效)"));
    QGridLayout *sg = new QGridLayout(simBox);

    m_bSimCar = new QPushButton(QString::fromUtf8("模拟来车"));
    m_bSimTake = new QPushButton(QString::fromUtf8("模拟取卡"));
    m_bSimLeave = new QPushButton(QString::fromUtf8("模拟驶离"));

    m_bSimCar->setStyleSheet("font-weight:bold; color:#0044aa;");
    m_bSimCar->setMinimumHeight(34);

    sg->addWidget(m_bSimCar, 0, 0, 1, 2);
    sg->addWidget(m_bSimTake, 1, 0);
    sg->addWidget(m_bSimLeave, 1, 1);

    QWidget *rightCol = new QWidget;
    QVBoxLayout *rightLay = new QVBoxLayout(rightCol);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->addWidget(manBox, 3);
    rightLay->addWidget(simBox, 1);

    mid->addWidget(rightCol, 2);

    root->addLayout(mid, 3);

    // ---------------- 流水表 ----------------
    QGroupBox *flowBox = new QGroupBox(QString::fromUtf8("通行流水"));
    QVBoxLayout *fl = new QVBoxLayout(flowBox);

    m_table = new QTableWidget(0, 7);
    QStringList hdr;
    hdr << QString::fromUtf8("流水号")
        << QString::fromUtf8("来车时间")
        << QString::fromUtf8("车牌")
        << QString::fromUtf8("车型")
        << QString::fromUtf8("CPC 卡号")
        << QString::fromUtf8("取卡时间")
        << QString::fromUtf8("结果");
    m_table->setHorizontalHeaderLabels(hdr);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 70);
    m_table->setColumnWidth(1, 90);
    m_table->setColumnWidth(2, 100);
    m_table->setColumnWidth(3, 90);
    m_table->setColumnWidth(4, 100);
    m_table->setColumnWidth(5, 90);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    fl->addWidget(m_table);

    root->addWidget(flowBox, 2);

    // ---------------- 日志 ----------------
    m_log = new QTextEdit;
    m_log->setReadOnly(true);
    m_log->setStyleSheet("font-family:Consolas,'Courier New'; font-size:12px;");
    root->addWidget(m_log, 2);
}

// ============================================================================
//  界面刷新
// ============================================================================

void LaneWindow::log(const QString &s)
{
    // 脏点: 只追加不截断, 跑一天下来 QTextEdit 会吃掉几百兆
    m_log->append(QDateTime::currentDateTime().toString("HH:mm:ss ") + s);
    LOG(s);
}

void LaneWindow::updateHead()
{
    QString dev = QString::fromUtf8("设备: ") + devStateText(m_devState);
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        dev = QString::fromUtf8("设备: 未连接");
    m_devLbl->setText(dev);

    m_binLbl->setText(QString::fromUtf8("卡箱: %1")
                      .arg(m_binLeft < 0 ? QString("-") : QString::number(m_binLeft)));
    m_slotLbl->setText(QString::fromUtf8("卡槽: %1")
                       .arg(m_slotPos == SLOT_OUT ? QString::fromUtf8("伸出")
                            : m_slotPos == SLOT_IN ? QString::fromUtf8("缩回") : QString("-")));
    m_barrierLbl->setText(QString::fromUtf8("栏杆: %1")
                          .arg(m_barrierUp ? QString::fromUtf8("抬起") : QString::fromUtf8("落下")));

    // 脏点: 状态灯颜色靠重新 setStyleSheet, 每次刷新都触发一次样式重算
    if (m_devState == DEV_FAULT || m_devState == DEV_NO_CARD)
        m_devLbl->setStyleSheet("color:#c00; font-weight:bold;");
    else if (m_sock->state() != QAbstractSocket::ConnectedState)
        m_devLbl->setStyleSheet("color:#888; font-weight:bold;");
    else
        m_devLbl->setStyleSheet("color:#0a0; font-weight:bold;");

    if (m_binLeft >= 0)
        m_binBar->setValue(m_binLeft);
}

void LaneWindow::updateLaneView()
{
    setLamp(m_coilA, m_carPresent);
    setLamp(m_coilB, m_carPresent);

    if (m_slotPos == SLOT_OUT)
    {
        m_slotIcon->setText(QString::fromUtf8("═══> 伸出"));
        m_slotIcon->setStyleSheet("font-size:16px; color:#c07000; font-weight:bold;");
    }
    else
    {
        m_slotIcon->setText(QString::fromUtf8("──  缩回"));
        m_slotIcon->setStyleSheet("font-size:16px; color:#555;");
    }

    if (m_barrierUp)
    {
        m_barrierIcon->setText(QString::fromUtf8("╱  抬起"));
        m_barrierIcon->setStyleSheet("font-size:16px; color:#0a0; font-weight:bold;");
    }
    else
    {
        m_barrierIcon->setText(QString::fromUtf8("▬  落杆"));
        m_barrierIcon->setStyleSheet("font-size:16px; color:#555;");
    }
}

void LaneWindow::updateVehiclePanel()
{
    m_seqLbl->setText(m_cur.seq > 0 ? QString::number(m_cur.seq) : QString("-"));
    m_plateLbl->setText(m_cur.plate.isEmpty() ? QString("-") : m_cur.plate);
    m_kindLbl->setText(m_cur.kind > 0 ? kindText(m_cur.kind) : QString("-"));
    m_timeLbl->setText(m_cur.arriveTime.isValid()
                       ? m_cur.arriveTime.toString("HH:mm:ss") : QString("-"));
    m_cardLbl->setText(m_cur.cardNo.isEmpty() ? QString("-") : m_cur.cardNo);
    m_stateLbl->setText(hostStateText(m_state));
}

void LaneWindow::appendFlowRow(const VehicleRecord &rec)
{
    Q_UNUSED(rec);   // 脏点: 参数没人用, 真正的数据从 m_flows 里拿

    // 脏点: 来一辆车就整张表重画, 流水上千条以后会明显卡
    m_table->setRowCount(m_flows.size());

    for (int i = 0; i < m_flows.size(); ++i)
    {
        const VehicleRecord &r = m_flows.at(i);

        for (int c = 0; c < 7; ++c)
        {
            if (!m_table->item(i, c))
                m_table->setItem(i, c, new QTableWidgetItem(QString()));
        }

        m_table->item(i, 0)->setText(QString::number(r.seq));
        m_table->item(i, 1)->setText(r.arriveTime.toString("HH:mm:ss"));
        m_table->item(i, 2)->setText(r.plate);
        m_table->item(i, 3)->setText(kindText(r.kind));
        m_table->item(i, 4)->setText(r.cardNo);
        m_table->item(i, 5)->setText(r.takeTime.isValid()
                                     ? r.takeTime.toString("HH:mm:ss") : QString("-"));

        QString res = resText(r.result);
        if (!r.remark.isEmpty())
            res += QString::fromUtf8(" (") + r.remark + QString::fromUtf8(")");
        m_table->item(i, 6)->setText(res);

        QColor c(0, 0, 0);
        if (r.result == RES_ISSUED)
            c = QColor(0, 130, 0);
        else if (r.result == RES_RUNNING)
            c = QColor(180, 110, 0);
        else
            c = QColor(190, 0, 0);
        m_table->item(i, 6)->setForeground(QBrush(c));
    }
}

void LaneWindow::setState(int st)
{
    m_state = st;
    m_stateLbl->setText(hostStateText(st));
    log(QString::fromUtf8("[车道] -> ") + hostStateText(st));
}

// ============================================================================
//  连接
// ============================================================================

void LaneWindow::onConnect()
{
    if (m_sock->state() != QAbstractSocket::UnconnectedState)
        m_sock->abort();

    log(QString::fromUtf8("连接 %1:%2 ...").arg(m_ipEdit->text()).arg(m_portEdit->text()));
    m_sock->connectToHost(m_ipEdit->text(), m_portEdit->text().toInt());

    // 脏点: UI 线程阻塞等 2 秒
    if (!m_sock->waitForConnected(2000))
    {
        log(QString::fromUtf8("连接失败: ") + m_sock->errorString());
        return;
    }

    // 真实车道: 连上先自检
    sendCmd(CMD_SELFTEST, QByteArray());
}

void LaneWindow::onConnected()
{
    m_connBtn->setEnabled(false);
    m_discBtn->setEnabled(true);
    log(QString::fromUtf8("已连接车道设备"));
    sendFire(CMD_GET_STATUS, QByteArray());
    updateHead();
}

void LaneWindow::onDisconnect()
{
    m_cmdTimer->stop();
    m_takeTimer->stop();
    m_sock->disconnectFromHost();
    log(QString::fromUtf8("主动断开连接"));
}

void LaneWindow::onDisconnected()
{
    m_connBtn->setEnabled(true);
    m_discBtn->setEnabled(false);
    m_devState = -1;
    m_binLeft = -1;
    m_slotPos = -1;
    m_errCode = -1;
    m_buf.clear();
    m_cmdTimer->stop();
    m_takeTimer->stop();

    log(QString::fromUtf8("连接已断开"));
    if (m_state != LS_IDLE)
        log(QString::fromUtf8("注意: 断开时车道还在 [%1], 卡可能留在机器里")
            .arg(hostStateText(m_state)));
    updateHead();
}

void LaneWindow::onSockError(QAbstractSocket::SocketError e)
{
    // 脏点: 不区分错误类型, 不重连, 不提示处理办法
    log(QString::fromUtf8("socket 错误(%1): %2").arg((int)e).arg(m_sock->errorString()));
}

void LaneWindow::onPollTick()
{
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        return;
    // 脏点: 绕过 sendCmd 直接写, 免得打乱流程超时定时器; 同一动作两套发送路径
    m_sock->write(buildFrame(CMD_GET_STATUS, QByteArray()));
}

// ============================================================================
//  收包
// ============================================================================

void LaneWindow::onReadyRead()
{
    m_buf.append(m_sock->readAll());

    // 脏点: 组帧/校验/分发全在一个槽里, 没有独立的 FrameParser;
    //       坏帧直接清空缓冲, 会把后面已经到达的完整帧一起丢掉
    while (true)
    {
        int cmd = 0;
        QByteArray payload;
        int used = parseFrame(m_buf, &cmd, &payload);

        if (used == 0)
            return;
        if (used < 0)
        {
            log(QString::fromUtf8("坏帧(%1), 清空接收缓冲 %2 字节").arg(used).arg(m_buf.size()));
            m_buf.clear();
            return;
        }

        m_buf = m_buf.mid(used);

        if (cmd == CMD_ACK)
        {
            // 脏点: ACK 只打日志, 不参与流程; "到底该不该等 ACK" 没人写得出来
            int rc = payload.size() > 0 ? (unsigned char)payload.at(0) : -1;
            if (rc != RC_OK)
                log(QString::fromUtf8("设备拒收命令: ") + rcText(rc));
        }
        else if (cmd == CMD_STATUS)
        {
            if (payload.size() >= 3)
            {
                m_devState = (unsigned char)payload.at(0);
                m_binLeft = (unsigned char)payload.at(1);
                m_errCode = (unsigned char)payload.at(2);
                if (payload.size() >= 4)
                    m_slotPos = (unsigned char)payload.at(3);   // 脏: 第 4 字节文档里没有
            }
            updateHead();
            updateLaneView();
        }
        else if (cmd == CMD_CARD_DATA)
        {
            m_lastCardNo = QString::fromLocal8Bit(payload).trimmed();
            log(QString::fromUtf8("备卡成功, 卡号 ") + m_lastCardNo);
        }
        else if (cmd == CMD_VEHICLE_DATA)
        {
            if (payload.size() >= 9)
            {
                m_vehPlate = QString::fromLocal8Bit(payload.left(8)).trimmed();
                m_vehKind = (unsigned char)payload.at(8);
            }
            log(QString::fromUtf8("车辆信息: %1 %2").arg(m_vehPlate).arg(kindText(m_vehKind)));
        }
        else if (cmd == CMD_EVENT)
        {
            if (payload.size() < 2)
            {
                log(QString::fromUtf8("EVENT 长度不够, 丢弃"));
                continue;
            }
            int ev = (unsigned char)payload.at(0);
            int param = (unsigned char)payload.at(1);
            onEvent(ev, param);
        }
        else
        {
            log(QString::fromUtf8("未知命令 0x%1").arg(cmd, 2, 16, QChar('0')));
        }
    }
}

// ============================================================================
//  发送
// ============================================================================

void LaneWindow::sendCmd(int cmd, const QByteArray &payload)
{
    if (m_sock->state() != QAbstractSocket::ConnectedState)
    {
        log(QString::fromUtf8("未连接设备, 命令 0x%1 丢弃")
            .arg(cmd, 2, 16, QChar('0')));
        return;
    }

    QByteArray f = buildFrame(cmd, payload);

    // 脏点: 多余的一次拷贝, 长度还写死 512
    memset(m_txBuf, 0, 512);
    memcpy(m_txBuf, f.constData(), f.size() > 511 ? 511 : f.size());
    m_sock->write(m_txBuf, f.size());
    m_sock->flush();

    // 脏点: 备卡 0.9 秒、写卡 0.7 秒, 全用 8 秒超时, 没有按命令区分
    m_cmdTimer->start(8000);

    log(QString::fromUtf8(">> CMD=0x%1 len=%2").arg(cmd, 2, 16, QChar('0')).arg(f.size()));
}

void LaneWindow::sendFire(int cmd, const QByteArray &payload)
{
    // 脏点: 和 sendCmd 几乎一样, 只是不启定时器. 复制粘贴出来的第二套发送逻辑
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray f = buildFrame(cmd, payload);
    m_sock->write(f);
    m_sock->flush();
}

void LaneWindow::sendDebug(int action)
{
    // 只发给模拟器: 让设备侧自己去压线圈 / 伸手拿卡, 上位机不伪造事件
    // 脏点: 调试命令和生产命令混在同一套协议里, 上真机只能靠"设备不认识这条命令"兜底
    if (m_sock->state() != QAbstractSocket::ConnectedState)
    {
        log(QString::fromUtf8("未连接设备, 无法模拟现场 (先启动 LaneSim 并点[连接])"));
        return;
    }

    sendFire(CMD_DEBUG, QByteArray(1, (char)action));
}

// ============================================================================
//  车道流程 —— 全项目最脏的地方
// ============================================================================

void LaneWindow::onEvent(int ev, int param)
{
    // 脏点:
    //   1. 整个车道流程是一条 if/else 链, 加一个"打印通行券"就要插到中间
    //   2. 事件到达时从不校验"当前状态能不能接受这个事件",
    //      所以车还没走下一辆就压线圈时, 上一个流程会被静默覆盖
    //   3. 事件码用宏, 没有强类型, 传错一个数字编译期毫无提示
    log(QString::fromUtf8("[事件] %1(%2) 参数=%3")
        .arg(ev == EV_CARD_READY ? QString::fromUtf8("备卡到位")
             : ev == EV_WRITE_DONE ? QString::fromUtf8("写卡完成")
             : ev == EV_SLOT_OUT ? QString::fromUtf8("卡槽伸出")
             : ev == EV_CARD_TAKEN ? QString::fromUtf8("已取卡")
             : ev == EV_SLOT_IN ? QString::fromUtf8("卡槽缩回")
             : ev == EV_RECYCLED ? QString::fromUtf8("回收完成")
             : ev == EV_CAR_ARRIVED ? QString::fromUtf8("车辆到达")
             : ev == EV_CAR_LEFT ? QString::fromUtf8("车辆驶离")
             : ev == EV_LEFT_NO_CARD ? QString::fromUtf8("未取卡驶离")
             : ev == EV_BIN_EMPTY ? QString::fromUtf8("卡箱无卡")
             : ev == EV_BIN_LOW ? QString::fromUtf8("卡箱余量低")
             : ev == EV_FAULT ? QString::fromUtf8("设备故障")
             : ev == EV_SELFTEST_DONE ? QString::fromUtf8("自检完成")
             : ev == EV_RESET_DONE ? QString::fromUtf8("复位完成")
             : QString::fromUtf8("未知"))
        .arg(ev).arg(param));

    if (ev == EV_CAR_ARRIVED)
    {
        if (m_carPresent)
            log(QString::fromUtf8("!! 车辆滞留还没走又来车了, 这里没锁车道"));

        m_carPresent = true;
        m_seq++;
        m_cur = VehicleRecord();
        m_cur.seq = m_seq;
        m_cur.arriveTime = QDateTime::currentDateTime();
        m_cur.plate = m_vehPlate;
        m_cur.kind = m_vehKind;
        m_cur.result = RES_RUNNING;
        m_retry = 0;
        m_voiceCount = 0;
        m_alarmLbl->setText(QString::fromUtf8("无"));
        m_alarmLbl->setStyleSheet("color:#0a0;");

        setState(LS_ARRIVED);
        updateVehiclePanel();
        updateLaneView();

        // 真实车道: 压地感就直接备卡, 不等人工按任何按钮
        startPrepare();
    }
    else if (ev == EV_CARD_READY)
    {
        m_cur.cardNo = m_lastCardNo;
        updateVehiclePanel();
        startWrite();
    }
    else if (ev == EV_WRITE_DONE)
    {
        m_cmdTimer->stop();
        if (param == RC_OK)
        {
            startPresent();
        }
        else
        {
            log(QString::fromUtf8("写卡失败: ") + rcText(param));
            if (m_retry < 2)
            {
                m_retry++;
                log(QString::fromUtf8("第 %1 次重试: 先回收废卡").arg(m_retry));
                doRecycle(RECYCLE_WRITE_FAIL);
            }
            else
            {
                enterFault(param, QString::fromUtf8("写卡连续失败"));
            }
        }
    }
    else if (ev == EV_SLOT_OUT)
    {
        m_cmdTimer->stop();
        m_slotPos = SLOT_OUT;
        setState(LS_WAIT_TAKE);
        showScreen(QString::fromUtf8("请取卡"));
        playVoice(VOICE_PLEASE_TAKE);
        m_voiceCount = 1;
        // 真实车道一般 30 秒, 演示用 15 秒
        m_takeTimer->start(15000);
        updateLaneView();
        updateHead();
    }
    else if (ev == EV_CARD_TAKEN)
    {
        m_takeTimer->stop();
        m_cur.takeTime = QDateTime::currentDateTime();
        finishVehicle(RES_ISSUED, QString::fromUtf8("正常发卡"));
        setState(LS_TAKEN);

        showScreen(QString::fromUtf8("卡已被取走，一路平安"));
        playVoice(VOICE_GOOD_LUCK);

        // 取卡后放行
        sendFire(CMD_BARRIER, QByteArray(1, (char)1));
        m_barrierUp = true;

        startRetract();
    }
    else if (ev == EV_SLOT_IN)
    {
        m_cmdTimer->stop();
        m_slotPos = SLOT_IN;
        updateLaneView();
        updateHead();
        if (m_state == LS_RETRACTING)
            setState(LS_WAIT_LEAVE);
    }
    else if (ev == EV_RECYCLED)
    {
        m_cmdTimer->stop();

        if (m_recovering)
        {
            finishRecover();
            return;
        }

        if (m_retry > 0 && m_cur.result == RES_RUNNING)
        {
            log(QString::fromUtf8("废卡已回收, 重新备卡"));
            startPrepare();
        }
        else
        {
            log(QString::fromUtf8("卡片已回收到废卡箱"));
            setState(LS_WAIT_LEAVE);
        }
    }
    else if (ev == EV_BIN_EMPTY)
    {
        setState(LS_NOCARD);
        showScreen(QString::fromUtf8("卡箱无卡，请走人工车道"));
        playVoice(VOICE_NO_CARD);
        m_alarmLbl->setText(QString::fromUtf8("报警: 卡箱无卡, 需更换卡箱"));
        m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
        finishVehicle(RES_NO_CARD, QString::fromUtf8("卡箱无卡"));
    }
    else if (ev == EV_BIN_LOW)
    {
        m_alarmLbl->setText(QString::fromUtf8("预警: 卡箱仅剩 %1 张, 请补卡").arg(param));
        m_alarmLbl->setStyleSheet("color:#e08000; font-weight:bold;");
        log(QString::fromUtf8("卡箱余量不足: %1").arg(param));
    }
    else if (ev == EV_FAULT)
    {
        enterFault(param, QString::fromUtf8("设备上报故障"));
    }
    else if (ev == EV_LEFT_NO_CARD)
    {
        // 车没取卡就压过存在线圈走了, 卡还在卡槽里 -> 必须回收, 否则下一辆车就发错了
        log(QString::fromUtf8("!! 车辆未取卡驶离"));
        m_alarmLbl->setText(QString::fromUtf8("报警: 车辆未取卡驶离, 卡片已回收"));
        m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
        m_takeTimer->stop();
        finishVehicle(RES_RECYCLED, QString::fromUtf8("未取卡驶离"));
        doRecycle(RECYCLE_MANUAL);
    }
    else if (ev == EV_CAR_LEFT)
    {
        m_carPresent = false;
        if (m_barrierUp)
        {
            sendFire(CMD_BARRIER, QByteArray(1, (char)0));
            m_barrierUp = false;
        }
        if (m_state == LS_WAIT_LEAVE || m_state == LS_IDLE)
        {
            setState(LS_IDLE);
            showScreen(QString::fromUtf8("欢迎光临"));
        }
        updateLaneView();
        updateHead();
    }
    else if (ev == EV_SELFTEST_DONE)
    {
        m_cmdTimer->stop();
        log(QString::fromUtf8("自检完成, 设备就绪"));
        updateHead();
    }
    else if (ev == EV_RESET_DONE)
    {
        m_cmdTimer->stop();
        log(QString::fromUtf8("设备复位完成"));

        if (m_recovering)
        {
            // 第二步: 把可能夹在机构里的卡回收掉 (没卡时设备会拒收, 属正常)
            log(QString::fromUtf8("回收残留卡片..."));
            sendCmd(CMD_RECYCLE_CARD, QByteArray(1, (char)RECYCLE_MANUAL));
            // 脏点: 没有卡就没有 EV_RECYCLED, 只能靠固定延时兜底
            QTimer::singleShot(1500, this, [this]() { finishRecover(); });
            return;
        }

        setState(LS_IDLE);
        updateHead();
    }
    else
    {
        log(QString::fromUtf8("未处理的事件码 %1").arg(ev));
    }
}

void LaneWindow::startPrepare()
{
    // 脏点: 卡箱号写死 1; 双卡箱机型直接不能用
    setState(LS_PREPARING);
    sendCmd(CMD_PREPARE_CARD, QByteArray(1, (char)1));
}

void LaneWindow::startWrite()
{
    QString t = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");
    QByteArray data = packEntryData(m_stationEdit->text(), m_laneEdit->text(),
                                    m_cur.kind, m_cur.plate, t, m_cur.cardNo);

    setState(LS_WRITING);
    sendCmd(CMD_WRITE_CARD, data);

    log(QString::fromUtf8("写卡: 入口 %1 站 %2 车道 / 车型 %3 / 车牌 %4 / 卡号 %5")
        .arg(m_stationEdit->text())
        .arg(m_laneEdit->text())
        .arg(kindText(m_cur.kind))
        .arg(m_cur.plate)
        .arg(m_cur.cardNo));
}

void LaneWindow::startPresent()
{
    setState(LS_PRESENTING);
    sendCmd(CMD_PRESENT_CARD, QByteArray());
}

void LaneWindow::startRetract()
{
    setState(LS_RETRACTING);
    sendCmd(CMD_RETRACT_CARD, QByteArray());
}

void LaneWindow::doRecycle(int reason)
{
    // 脏点: 好几个地方调过来, 每次意图都不一样, 回收完该干嘛全靠外面判断
    setState(LS_RECOVERING);
    sendCmd(CMD_RECYCLE_CARD, QByteArray(1, (char)reason));
}

void LaneWindow::finishVehicle(int result, const QString &remark)
{
    if (m_cur.result != RES_RUNNING)
        return;                     // 脏: 用"结果是否已填"来防止重复关单

    m_cur.result = result;
    m_cur.remark = remark;
    m_flows.prepend(m_cur);
    appendFlowRow(m_cur);

    log(QString::fromUtf8("[流水] %1 %2 %3 卡号%4 -> %5")
        .arg(m_cur.seq)
        .arg(m_cur.plate)
        .arg(kindText(m_cur.kind))
        .arg(m_cur.cardNo)
        .arg(resText(result)));
}

void LaneWindow::enterFault(int rc, const QString &what)
{
    m_cmdTimer->stop();
    m_takeTimer->stop();

    setState(LS_FAULT);
    m_errCode = rc;
    m_alarmLbl->setText(QString::fromUtf8("故障: ") + what);
    m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
    showScreen(QString::fromUtf8("设备故障，请联系工作人员"));
    log(QString::fromUtf8("!! %1: %2").arg(what).arg(rcText(rc)));

    // 本车按"已中止"记一笔 (旧版记成"已回收", 其实卡还在机构里)
    finishVehicle(RES_ABORTED, what);
    log(QString::fromUtf8("处理完现场后点[故障恢复 / 跳过本车], 否则下一辆车发不出去"));
    updateHead();
}

void LaneWindow::finishRecover()
{
    if (!m_recovering)
        return;             // EV_RECYCLED 和兜底定时器只认第一个

    m_recovering = false;

    // 脏点: 车道清空靠一条调试命令, 真机上是人工确认后按"车道复位"
    if (m_sock->state() == QAbstractSocket::ConnectedState)
        sendFire(CMD_DEBUG, QByteArray(1, (char)DBG_LANE_CLEAR));

    m_cmdTimer->stop();
    m_takeTimer->stop();
    m_carPresent = false;
    m_retry = 0;
    m_voiceCount = 0;
    m_errCode = 0;
    m_devState = DEV_IDLE;
    m_slotPos = SLOT_IN;

    if (m_barrierUp)
    {
        sendFire(CMD_BARRIER, QByteArray(1, (char)0));
        m_barrierUp = false;
    }

    // 还在跑的车按"已中止"落一条流水, 不留半截
    finishVehicle(RES_ABORTED, QString::fromUtf8("故障恢复, 本车中止"));

    m_cur = VehicleRecord();
    setState(LS_IDLE);
    showScreen(QString::fromUtf8("欢迎光临"));
    m_alarmLbl->setText(QString::fromUtf8("已恢复, 可接待下一辆车"));
    m_alarmLbl->setStyleSheet("color:#0a0;");
    updateVehiclePanel();
    updateLaneView();
    updateHead();

    log(QString::fromUtf8("故障恢复完成, 车道已清空"));
}

void LaneWindow::showScreen(const QString &text)
{
    m_screen->setText(text);

    if (m_sock->state() != QAbstractSocket::ConnectedState)
        return;

    // 脏点: 本地显示和下发给 LED 屏是两件事, 混在一个函数里;
    //       断线时屏上还留着上一次的内容, 没人管
    QByteArray p;
    p.append((char)1);
    p.append(text.toLocal8Bit());
    sendFire(CMD_DISPLAY, p);
}

void LaneWindow::playVoice(int id)
{
    sendFire(CMD_VOICE, QByteArray(1, (char)id));
    log(QString::fromUtf8("语音: ") + voiceText(id));
}

// ============================================================================
//  超时
// ============================================================================

void LaneWindow::onCmdTimeout()
{
    // 脏点: 不知道是哪条命令超时, 也不重发, 直接判故障
    enterFault(RC_TIMEOUT, QString::fromUtf8("等待设备响应超时"));
}

void LaneWindow::onTakeTimeout()
{
    m_voiceCount++;

    if (m_voiceCount <= 2)
    {
        // 第一次超时: 再催一次
        showScreen(QString::fromUtf8("请取卡"));
        playVoice(VOICE_PLEASE_TAKE);
        log(QString::fromUtf8("超时未取卡, 第 %1 次重复提示").arg(m_voiceCount));
        m_takeTimer->start(15000);
        return;
    }

    // 第二次还超时: 回收, 报人工
    log(QString::fromUtf8("超时未取卡, 回收卡片"));
    showScreen(QString::fromUtf8("卡片已回收，请联系工作人员"));
    playVoice(VOICE_RECYCLED);
    m_alarmLbl->setText(QString::fromUtf8("报警: 超时未取卡, 卡片已回收"));
    m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
    finishVehicle(RES_TIMEOUT, QString::fromUtf8("超时未取卡"));
    doRecycle(RECYCLE_TIMEOUT);
}

// ============================================================================
//  手动调试 —— 绕过状态机, 想发什么发什么
// ============================================================================

void LaneWindow::onManualSelftest()
{
    sendCmd(CMD_SELFTEST, QByteArray());
}

void LaneWindow::onManualPrepare()
{
    // 脏点: 手动命令不维护 m_state, 点两下就能把自动流程搞乱
    sendCmd(CMD_PREPARE_CARD, QByteArray(1, (char)1));
}

void LaneWindow::onManualWrite()
{
    QString plate = m_cur.plate.isEmpty() ? QString::fromUtf8("苏A00000") : m_cur.plate;
    int kind = m_cur.kind > 0 ? m_cur.kind : 1;
    QString cardNo = m_cur.cardNo.isEmpty() ? QString("00000000") : m_cur.cardNo;
    QString t = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");

    QByteArray data = packEntryData(m_stationEdit->text(), m_laneEdit->text(),
                                    kind, plate, t, cardNo);
    sendCmd(CMD_WRITE_CARD, data);
}

void LaneWindow::onManualPresent()
{
    sendCmd(CMD_PRESENT_CARD, QByteArray());
}

void LaneWindow::onManualRetract()
{
    sendCmd(CMD_RETRACT_CARD, QByteArray());
}

void LaneWindow::onManualRecycle()
{
    sendCmd(CMD_RECYCLE_CARD, QByteArray(1, (char)RECYCLE_MANUAL));
}

void LaneWindow::onManualVoice()
{
    playVoice(VOICE_PLEASE_TAKE);
}

void LaneWindow::onManualBarrierUp()
{
    sendFire(CMD_BARRIER, QByteArray(1, (char)1));
    m_barrierUp = true;
    updateLaneView();
    updateHead();
}

void LaneWindow::onManualBarrierDown()
{
    sendFire(CMD_BARRIER, QByteArray(1, (char)0));
    m_barrierUp = false;
    updateLaneView();
    updateHead();
}

void LaneWindow::onResetDev()
{
    log(QString::fromUtf8("下发复位..."));
    sendCmd(CMD_RESET, QByteArray());

    // 脏点: 复位只改了本地状态, 设备那边是不是真复位了没人确认
    m_cmdTimer->stop();
    m_takeTimer->stop();
    m_retry = 0;
    m_voiceCount = 0;
    m_errCode = 0;
    m_alarmLbl->setText(QString::fromUtf8("无"));
    m_alarmLbl->setStyleSheet("color:#0a0;");
    setState(LS_IDLE);
    updateHead();
}

void LaneWindow::onRecoverFault()
{
    if (m_sock->state() != QAbstractSocket::ConnectedState)
    {
        log(QString::fromUtf8("未连接设备, 无法恢复"));
        return;
    }
    if (m_recovering)
    {
        log(QString::fromUtf8("正在恢复中, 请稍等"));
        return;
    }

    m_recovering = true;
    m_cmdTimer->stop();
    m_takeTimer->stop();

    log(QString::fromUtf8("开始故障恢复: 设备复位 -> 回收残留卡 -> 清空车道"));
    sendCmd(CMD_RESET, QByteArray());
}

void LaneWindow::onClearFlow()
{
    // 脏点: 正在处理的那辆车能被清掉, 流水号和界面就对不上了
    m_flows.clear();
    m_table->setRowCount(0);
    m_seq = 0;
    m_cur = VehicleRecord();
    updateVehiclePanel();
    log(QString::fromUtf8("流水已清空"));
}

// ============================================================================
//  模拟现场
// ============================================================================

void LaneWindow::onSimCarArrive()
{
    if (m_state == LS_FAULT)
    {
        log(QString::fromUtf8("设备还在故障态, 先点[故障恢复 / 跳过本车]"));
        return;
    }
    if (m_carPresent)
    {
        log(QString::fromUtf8("车道里还有车, 先点[模拟驶离]或[故障恢复 / 跳过本车]"));
        return;
    }
    sendDebug(DBG_CAR_ARRIVE);
}

void LaneWindow::onSimTakeCard()
{
    sendDebug(DBG_CAR_TAKE);
}

void LaneWindow::onSimCarLeave()
{
    sendDebug(DBG_CAR_LEAVE);
}
