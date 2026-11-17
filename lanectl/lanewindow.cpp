#include "lanewindow.h"
#include "logger.h"
#include "protocol.h"
#include "CommandTracker.h"
#include "ThreadPool.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <memory>
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

// 命令默认超时 (每条命令各自计时, 不再共用一个 QTimer)
static const int kCmdTimeoutMs = 8000;

// ----------------------------------------------------------------------------

LaneWindow::LaneWindow(QWidget *parent)
    : QWidget(parent)
{
    // 脏点: 成员初始化散在构造函数 / buildUi / onConnected 三处
    m_sock = new QTcpSocket(this);

    m_pool = std::make_unique<ThreadPool>(4);
    m_tracker = std::make_unique<CommandTracker>();

    m_takeTimer = new QTimer(this);
    m_takeTimer->setSingleShot(true);
    m_heartTimer = new QTimer(this);

    m_state = LS_IDLE;
    m_seq = 0;
    m_retry = 0;
    m_voiceCount = 0;
    m_carPresent = false;
    m_barrierUp = false;
    m_recovering = false;

    m_devState = -1;
    m_curPos = 0;
    m_stretch = 0;
    m_vehKind = 0;
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        m_posState[i] = -1;
        m_posBin[i] = -1;
    }

    buildUi();

    connect(m_sock, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(m_sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_sock, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onSockError(QAbstractSocket::SocketError)));

    connect(m_takeTimer, SIGNAL(timeout()), this, SLOT(onTakeTimeout()));
    connect(m_heartTimer, SIGNAL(timeout()), this, SLOT(onHeartCheck()));

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

    m_heartTimer->start(5000);

    showScreen(QString::fromUtf8("欢迎光临"));
    log(QString::fromUtf8("车道控制软件启动. 先启动 LaneSim, 再点[连接]"));
    log(QString::fromUtf8("提示: 备卡是机器人自己维持的, 车道只管 选工位->写卡->送卡"));
}

LaneWindow::~LaneWindow()
{
    // 脏点: 关程序时设备可能还在动, 不回收卡也不落杆
    if (m_pool)
        m_pool->shutdown();
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
    m_stationEdit->setFixedWidth(55);
    top->addWidget(m_stationEdit);

    top->addWidget(new QLabel(QString::fromUtf8("车道:")));
    m_laneEdit = new QLineEdit(QString::fromUtf8("03"));
    m_laneEdit->setFixedWidth(40);
    top->addWidget(m_laneEdit);

    top->addSpacing(8);
    top->addWidget(new QLabel(QString::fromUtf8("IP:")));
    m_ipEdit = new QLineEdit(QString::fromUtf8("127.0.0.1"));
    m_ipEdit->setFixedWidth(95);
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
    m_heartLbl = new QLabel(QString::fromUtf8("心跳: -"));
    m_barrierLbl = new QLabel(QString::fromUtf8("栏杆: -"));

    QFont bold = m_devLbl->font();
    bold.setBold(true);
    m_devLbl->setFont(bold);

    top->addWidget(m_devLbl);
    top->addWidget(m_heartLbl);
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

    lg->addWidget(new QLabel(QString::fromUtf8("当前工位")), 1, 0);
    m_curPosLbl = new QLabel(QString::fromUtf8("-"));
    m_curPosLbl->setStyleSheet("font-size:16px; color:#0044aa; font-weight:bold;");
    lg->addWidget(m_curPosLbl, 1, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("伸缩机构")), 2, 0);
    m_slotIcon = new QLabel(QString::fromUtf8("──  缩回"));
    m_slotIcon->setStyleSheet("font-size:16px; color:#555;");
    lg->addWidget(m_slotIcon, 2, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("栏杆")), 3, 0);
    m_barrierIcon = new QLabel(QString::fromUtf8("▬  落杆"));
    m_barrierIcon->setStyleSheet("font-size:16px; color:#555;");
    lg->addWidget(m_barrierIcon, 3, 1);

    lg->addWidget(new QLabel(QString::fromUtf8("存在线圈 B")), 4, 0);
    m_coilB = new QLabel(QString::fromUtf8("●"));
    setLamp(m_coilB, false);
    lg->addWidget(m_coilB, 4, 1);

    laneLay->addLayout(lg);

    laneLay->addWidget(new QLabel(QString::fromUtf8("信息屏 (LED):")));
    m_screen = new QLabel(QString::fromUtf8("欢迎光临"));
    m_screen->setAlignment(Qt::AlignCenter);
    m_screen->setWordWrap(true);
    m_screen->setMinimumHeight(70);
    m_screen->setStyleSheet(
        "background:#101010; color:#33ff66; border:1px solid #333;"
        "font-family:'Microsoft YaHei'; font-size:18px; font-weight:bold;");
    laneLay->addWidget(m_screen);

    laneLay->addSpacing(4);
    QGridLayout *sg = new QGridLayout;
    m_bSimCar = new QPushButton(QString::fromUtf8("模拟来车"));
    m_bSimTake = new QPushButton(QString::fromUtf8("模拟取卡"));
    m_bSimLeave = new QPushButton(QString::fromUtf8("模拟驶离"));
    m_bSimCar->setStyleSheet("font-weight:bold; color:#0044aa;");
    m_bSimCar->setMinimumHeight(30);
    sg->addWidget(m_bSimCar, 0, 0, 1, 2);
    sg->addWidget(m_bSimTake, 1, 0);
    sg->addWidget(m_bSimLeave, 1, 1);
    laneLay->addLayout(sg);
    laneLay->addStretch();

    mid->addWidget(laneBox, 3);

    // ---- 当前车辆 ----
    QGroupBox *vehBox = new QGroupBox(QString::fromUtf8("当前车辆"));
    QGridLayout *vg = new QGridLayout(vehBox);

    QFont big = font();
    big.setPointSize(13);
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

    // ---- 4 工位 ----
    QGroupBox *posBox = new QGroupBox(QString::fromUtf8("发卡工位 (4 工位)"));
    QVBoxLayout *pv = new QVBoxLayout(posBox);

    m_posTable = new QTableWidget(POS_COUNT, 4);
    QStringList ph;
    ph << QString::fromUtf8("工位")
       << QString::fromUtf8("状态")
       << QString::fromUtf8("卡数")
       << QString::fromUtf8("卡号");
    m_posTable->setHorizontalHeaderLabels(ph);
    m_posTable->horizontalHeader()->setStretchLastSection(true);
    m_posTable->setColumnWidth(0, 60);
    m_posTable->setColumnWidth(1, 80);
    m_posTable->setColumnWidth(2, 50);
    m_posTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_posTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_posTable->verticalHeader()->setVisible(false);
    for (int i = 0; i < POS_COUNT; ++i)
    {
        m_posTable->setRowHeight(i, 26);
        for (int c = 0; c < 4; ++c)
            m_posTable->setItem(i, c, new QTableWidgetItem(QString("-")));
    }
    pv->addWidget(m_posTable);
    mid->addWidget(posBox, 3);

    root->addLayout(mid, 3);

    // ---------------- 第三行: 手动控制 ----------------
    QGroupBox *manBox = new QGroupBox(QString::fromUtf8("手动控制 (单步 / 排障)"));
    QGridLayout *mg = new QGridLayout(manBox);

    m_bSelfTest = new QPushButton(QString::fromUtf8("自检"));
    m_bPrepare = new QPushButton(QString::fromUtf8("全部备卡"));
    m_bWrite = new QPushButton(QString::fromUtf8("写卡"));
    m_bPresent = new QPushButton(QString::fromUtf8("送卡"));
    m_bRetract = new QPushButton(QString::fromUtf8("缩卡"));
    m_bRecycle = new QPushButton(QString::fromUtf8("回收"));
    m_bVoice = new QPushButton(QString::fromUtf8("播报"));
    m_bUp = new QPushButton(QString::fromUtf8("抬杆"));
    m_bDown = new QPushButton(QString::fromUtf8("落杆"));
    m_bReset = new QPushButton(QString::fromUtf8("复位"));
    m_bClear = new QPushButton(QString::fromUtf8("清流水"));
    m_bRecover = new QPushButton(QString::fromUtf8("故障恢复 / 跳过本车"));
    m_bRecover->setStyleSheet("font-weight:bold; color:#c00;");

    mg->addWidget(m_bSelfTest, 0, 0);
    mg->addWidget(m_bPrepare, 0, 1);
    mg->addWidget(m_bWrite, 0, 2);
    mg->addWidget(m_bPresent, 0, 3);
    mg->addWidget(m_bRetract, 0, 4);
    mg->addWidget(m_bRecycle, 0, 5);
    mg->addWidget(m_bVoice, 1, 0);
    mg->addWidget(m_bUp, 1, 1);
    mg->addWidget(m_bDown, 1, 2);
    mg->addWidget(m_bReset, 1, 3);
    mg->addWidget(m_bClear, 1, 4);
    mg->addWidget(m_bRecover, 1, 5);

    root->addWidget(manBox);

    // ---------------- 第四行: 流水 + 日志 ----------------
    QHBoxLayout *bottom = new QHBoxLayout;

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
    m_table->setColumnWidth(0, 60);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 90);
    m_table->setColumnWidth(3, 85);
    m_table->setColumnWidth(4, 90);
    m_table->setColumnWidth(5, 80);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    fl->addWidget(m_table);
    bottom->addWidget(flowBox, 3);

    QGroupBox *logBox = new QGroupBox(QString::fromUtf8("运行日志"));
    QVBoxLayout *lb = new QVBoxLayout(logBox);
    m_log = new QTextEdit;
    m_log->setReadOnly(true);
    m_log->setStyleSheet("font-family:Consolas,'Courier New'; font-size:12px;");
    lb->addWidget(m_log);
    bottom->addWidget(logBox, 2);

    root->addLayout(bottom, 2);
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
    QString dev = QString::fromUtf8("设备: ") + devSummaryText(m_devState);
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        dev = QString::fromUtf8("设备: 未连接");
    m_devLbl->setText(dev);

    if (m_lastHeart.isValid() && m_sock->state() == QAbstractSocket::ConnectedState)
    {
        qint64 sec = m_lastHeart.secsTo(QDateTime::currentDateTime());
        m_heartLbl->setText(QString::fromUtf8("心跳: %1 秒前").arg(sec));
        if (sec > 45)
            m_heartLbl->setStyleSheet("color:#c00; font-weight:bold;");
        else
            m_heartLbl->setStyleSheet("color:#0a0;");
    }
    else
    {
        m_heartLbl->setText(QString::fromUtf8("心跳: -"));
        m_heartLbl->setStyleSheet("color:#888;");
    }

    m_barrierLbl->setText(QString::fromUtf8("栏杆: %1")
                          .arg(m_barrierUp ? QString::fromUtf8("抬起") : QString::fromUtf8("落下")));

    // 脏点: 状态灯颜色靠重新 setStyleSheet, 每次刷新都触发一次样式重算
    if (m_devState == DS_ALLDOWN)
        m_devLbl->setStyleSheet("color:#c00; font-weight:bold;");
    else if (m_devState == DS_PARTIAL)
        m_devLbl->setStyleSheet("color:#e08000; font-weight:bold;");
    else if (m_sock->state() != QAbstractSocket::ConnectedState)
        m_devLbl->setStyleSheet("color:#888; font-weight:bold;");
    else
        m_devLbl->setStyleSheet("color:#0a0; font-weight:bold;");
}

void LaneWindow::updateLaneView()
{
    setLamp(m_coilA, m_carPresent);
    setLamp(m_coilB, m_carPresent);

    m_curPosLbl->setText(m_curPos > 0 ? posText(m_curPos) : QString("-"));

    if (m_stretch == STRETCH_OUT)
    {
        m_slotIcon->setText(QString::fromUtf8("═══> 伸出20cm"));
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

void LaneWindow::updatePosTable()
{
    // 脏点: 每来一帧状态就把整张表刷一遍
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        int r = i - 1;

        m_posTable->item(r, 0)->setText(posText(i));
        m_posTable->item(r, 1)->setText(m_posState[i] < 0 ? QString("-") : posStateText(m_posState[i]));
        m_posTable->item(r, 2)->setText(m_posBin[i] < 0 ? QString("-") : QString::number(m_posBin[i]));
        m_posTable->item(r, 3)->setText(m_posCard[i].isEmpty() ? QString("-") : m_posCard[i]);

        QColor c(80, 80, 80);
        if (m_posState[i] == PS_READY)
            c = QColor(0, 130, 0);
        else if (m_posState[i] == PS_SENT)
            c = QColor(200, 120, 0);
        else if (m_posState[i] == PS_FAILED || m_posState[i] == PS_FAULT)
            c = QColor(190, 0, 0);

        m_posTable->item(r, 1)->setForeground(QBrush(c));

        // 当前工位高亮
        QColor bg = (i == m_curPos) ? QColor(255, 250, 205) : QColor(255, 255, 255);
        for (int col = 0; col < 4; ++col)
            m_posTable->item(r, col)->setBackground(QBrush(bg));
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

void LaneWindow::autoConnect(const QString &host, int port)
{
    m_ipEdit->setText(host);
    m_portEdit->setText(QString::number(port));

    // 等窗口画出来再连, 否则日志控件还没创建
    QTimer::singleShot(600, this, SLOT(onConnect()));
}

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

    // 真实车道: 连上先对时, 再自检 (现场 C1 时间同步)
    sendCmd(CMD_TIME_SYNC,
            QDateTime::currentDateTime().toString("yyyyMMddhhmmss").toLocal8Bit());
}

void LaneWindow::onConnected()
{
    m_connBtn->setEnabled(false);
    m_discBtn->setEnabled(true);
    m_lastHeart = QDateTime::currentDateTime();
    log(QString::fromUtf8("已连接车道设备"));

    sendFire(CMD_GET_STATUS, QByteArray());
    updateHead();
    updatePosTable();
}

void LaneWindow::onDisconnect()
{
    m_takeTimer->stop();
    m_sock->disconnectFromHost();
    log(QString::fromUtf8("主动断开连接"));
}

void LaneWindow::onDisconnected()
{
    m_connBtn->setEnabled(true);
    m_discBtn->setEnabled(false);
    m_devState = -1;
    m_curPos = 0;
    m_stretch = STRETCH_IN;
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        m_posState[i] = -1;
        m_posBin[i] = -1;
    }
    m_buf.clear();
    m_takeTimer->stop();

    // 断线时把所有还没回来的命令一次性失败掉, 别让工作线程干等到超时
    if (m_tracker)
        m_tracker->failAllPending(RC_ABORT);

    log(QString::fromUtf8("连接已断开"));
    if (m_state != LS_IDLE)
        log(QString::fromUtf8("注意: 断开时车道还在 [%1], 卡可能留在机器里")
            .arg(hostStateText(m_state)));
    updateHead();
    updatePosTable();
}

void LaneWindow::onSockError(QAbstractSocket::SocketError e)
{
    // 脏点: 不区分错误类型, 不重连, 不提示处理办法
    log(QString::fromUtf8("socket 错误(%1): %2").arg((int)e).arg(m_sock->errorString()));
}

void LaneWindow::onHeartCheck()
{
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        return;
    if (!m_lastHeart.isValid())
        return;

    // 脏点: 心跳超时只报警, 不重连, 不落地当前流程
    qint64 sec = m_lastHeart.secsTo(QDateTime::currentDateTime());
    if (sec > 45)
    {
        if (m_alarmLbl->text().indexOf(QString::fromUtf8("心跳")) < 0)
        {
            m_alarmLbl->setText(QString::fromUtf8("报警: 设备心跳超时 %1 秒").arg(sec));
            m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
            log(QString::fromUtf8("!! 设备心跳超时"));
        }
    }
    updateHead();
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

        if (cmd == CMD_RESULT)
        {
            if (payload.size() < 2)
            {
                log(QString::fromUtf8("结果帧长度不够, 丢弃"));
                continue;
            }

            // 只负责"兑现", 真正的业务处理在工作线程唤醒后回到界面线程做
            int rcmd = (unsigned char)payload.at(0);
            int rrc = (unsigned char)payload.at(1);
            if (!m_tracker->complete(rcmd, rrc))
            {
                // 语音/屏显/栏杆/查状态这类 fire-and-forget 命令也会回 B1, 直接忽略
            }
        }
        else if (cmd == CMD_STATE)
        {
            onState(payload);
        }
        else if (cmd == CMD_EVENT)
        {
            if (payload.size() < 2)
            {
                log(QString::fromUtf8("事件帧长度不够, 丢弃"));
                continue;
            }
            onEvent((unsigned char)payload.at(0), (unsigned char)payload.at(1));
        }
        else if (cmd == CMD_CARD_DATA)
        {
            if (payload.size() < 2)
                continue;
            int pos = (unsigned char)payload.at(0);
            QString no = QString::fromLocal8Bit(payload.mid(1)).trimmed();
            if (pos >= 1 && pos <= POS_COUNT)
            {
                m_posCard[pos] = no;
                log(QString::fromUtf8("%1 工位备卡完成, 卡号 %2").arg(posText(pos)).arg(no));
                updatePosTable();
            }
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
        else if (cmd == CMD_HEARTBEAT)
        {
            m_lastHeart = QDateTime::currentDateTime();
            updateHead();
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

    // 先登记 waiter 再写 socket: 设备回得快也不会漏掉
    CommandTracker::Waiter waiter = m_tracker->begin(cmd);

    m_sock->write(f);
    m_sock->flush();

    log(QString::fromUtf8(">> CMD=0x%1 len=%2").arg(cmd, 2, 16, QChar('0')).arg(f.size()));

    // 把"等结果 + 超时"丢给线程池, 界面线程不再被定时器绑住
    m_pool->submit([this, waiter = std::move(waiter)]() mutable {
        CmdResult r;

        if (waiter.future.wait_for(std::chrono::milliseconds(kCmdTimeoutMs)) ==
            std::future_status::ready)
        {
            r = waiter.future.get();
        }
        else
        {
            m_tracker->drop(waiter.id);
            r.cmd = waiter.cmd;
            r.rc = RC_TIMEOUT;
            r.timeout = true;
        }

        // 结果必须回到界面线程再动 UI / 状态机
        QMetaObject::invokeMethod(this, [this, r]() { onCmdDone(r); }, Qt::QueuedConnection);
    });
}

void LaneWindow::sendFire(int cmd, const QByteArray &payload)
{
    // 不需要结果的通知类命令 (语音/屏显/栏杆/查状态/调试), 只发不等
    if (m_sock->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray f = buildFrame(cmd, payload);
    m_sock->write(f);
    m_sock->flush();
}

void LaneWindow::onCmdDone(const CmdResult &r)
{
    if (r.timeout)
    {
        // 脏点: 还是不知道设备卡在哪一步, 只知道"超时了"
        log(QString::fromUtf8("命令 0x%1 超时").arg(r.cmd, 2, 16, QChar('0')));
        enterFault(RC_TIMEOUT, QString::fromUtf8("等待设备响应超时"));
        return;
    }

    onResult(r.cmd, r.rc);
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
//  上行分发
// ============================================================================

void LaneWindow::onResult(int cmd, int rc)
{

    if (rc != RC_OK)
    {
        log(QString::fromUtf8("命令 0x%1 失败: %2").arg(cmd, 2, 16, QChar('0')).arg(rcText(rc)));

        if (cmd == CMD_RECYCLE_CARD)
        {
            if (m_recovering)
            {
                log(QString::fromUtf8("机构里没有残留卡, 跳过回收"));
                return;                 // 由恢复流程的兜底定时器收尾
            }
            log(QString::fromUtf8("回收失败, 卡片可能还在机构里"));
            setState(LS_FAULT);
            return;
        }

        if (cmd == CMD_WRITE_CARD || cmd == CMD_PRESENT_CARD)
        {
            // 脏点: 工位不可用时换一个工位重试, 但没区分"该工位坏了"和"整机忙"
            if (m_retry < 1 && m_carPresent)
            {
                m_retry++;
                log(QString::fromUtf8("换个工位重试 (第 %1 次)").arg(m_retry));
                int p = pickPosition();
                if (p > 0)
                {
                    m_curPos = p;
                    updateLaneView();
                    startWrite();
                    return;
                }
            }
            enterFault(rc, QString::fromUtf8("写卡/送卡失败"));
            return;
        }

        enterFault(rc, QString::fromUtf8("命令执行失败"));
        return;
    }

    // ---- 成功 ----
    if (cmd == CMD_WRITE_CARD)
    {
        startPresent();
    }
    else if (cmd == CMD_PRESENT_CARD)
    {
        if (m_curPos >= POS_DOWN_LEFT)
        {
            m_stretch = STRETCH_OUT;
            updateLaneView();
        }
        setState(LS_WAIT_TAKE);
        showScreen(QString::fromUtf8("请取卡"));
        playVoice(VOICE_PLEASE_TAKE);
        m_voiceCount = 1;
        m_takeTimer->start(15000);      // 现场一般 30s, 演示用 15s
    }
    else if (cmd == CMD_RECYCLE_CARD)
    {
        log(QString::fromUtf8("卡片已回收"));
        if (m_recovering)
            finishRecover();
        else
            setState(LS_WAIT_LEAVE);
    }
    else if (cmd == CMD_SELFTEST)
    {
        log(QString::fromUtf8("自检完成, 设备就绪"));
    }
    else if (cmd == CMD_TIME_SYNC)
    {
        log(QString::fromUtf8("对时完成"));
        sendCmd(CMD_SELFTEST, QByteArray());
    }
    else if (cmd == CMD_RESET)
    {
        log(QString::fromUtf8("设备复位完成"));
        if (m_recovering)
        {
            if (m_curPos > 0)
            {
                doRecycle(RECYCLE_MANUAL);
                QTimer::singleShot(1200, this, [this]() { finishRecover(); });
            }
            else
            {
                finishRecover();
            }
        }
        else
        {
            setState(LS_IDLE);
        }
    }
}

void LaneWindow::onState(const QByteArray &payload)
{
    // 脏点: 9 这个长度是数出来的, 协议文档里只有一句"4 个工位"
    if (payload.size() < 9)
    {
        log(QString::fromUtf8("状态帧长度不对: %1").arg(payload.size()));
        return;
    }

    m_devState = (unsigned char)payload.at(0);
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        m_posState[i] = (unsigned char)payload.at(1 + (i - 1) * 2);
        m_posBin[i] = (unsigned char)payload.at(2 + (i - 1) * 2);
    }

    updateHead();
    updatePosTable();

    // 脏点: "没工位可用"之后的恢复, 是偷偷在这里补的:
    //       只要设备状态帧显示有工位备好了, 就直接接着写卡
    if (m_state == LS_NOPOS && m_carPresent)
    {
        int p = pickPosition();
        if (p > 0)
        {
            log(QString::fromUtf8("工位恢复了, 继续发卡"));
            m_curPos = p;
            m_alarmLbl->setText(QString::fromUtf8("无"));
            m_alarmLbl->setStyleSheet("color:#0a0;");
            startWrite();
        }
    }
}

void LaneWindow::onEvent(int ev, int pos)
{
    // 脏点: 整个车道流程是一条 if/else 链, 事件到达时从不校验当前状态
    log(QString::fromUtf8("[事件] %1 %2")
        .arg(eventText(ev))
        .arg(pos > 0 ? posText(pos) : QString()));

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

        // 真实车道: 来车直接选一个已经备好卡的工位
        int p = pickPosition();
        if (p == 0)
        {
            setState(LS_NOPOS);
            showScreen(QString::fromUtf8("请稍候"));
            playVoice(VOICE_WAIT);
            log(QString::fromUtf8("没有已备卡的工位, 等设备补备"));
            m_alarmLbl->setText(QString::fromUtf8("报警: 无可用工位"));
            m_alarmLbl->setStyleSheet("color:#e08000; font-weight:bold;");
            return;
        }

        m_curPos = p;
        updateLaneView();
        updatePosTable();
        startWrite();
    }
    else if (ev == EV_CARD_TAKEN)
    {
        m_takeTimer->stop();
        m_stretch = STRETCH_IN;
        m_cur.takeTime = QDateTime::currentDateTime();
        finishVehicle(RES_ISSUED, QString::fromUtf8("正常发卡"));
        setState(LS_TAKEN);

        showScreen(QString::fromUtf8("卡已被取走，一路平安"));
        playVoice(VOICE_GOOD_LUCK);

        // 取卡后放行
        sendFire(CMD_BARRIER, QByteArray(1, (char)1));
        m_barrierUp = true;
        updateLaneView();
        updateHead();
    }
    else if (ev == EV_PREPARE_RECOVER)
    {
        log(QString::fromUtf8("%1 工位备卡恢复").arg(posText(pos)));
        if (m_state == LS_NOPOS && m_carPresent)
        {
            int p = pickPosition();
            if (p > 0)
            {
                m_curPos = p;
                m_alarmLbl->setText(QString::fromUtf8("无"));
                m_alarmLbl->setStyleSheet("color:#0a0;");
                startWrite();
            }
        }
    }
    else if (ev == EV_POS_NO_CARD)
    {
        log(QString::fromUtf8("%1 工位卡箱无卡, 需要补卡").arg(posText(pos)));
        m_alarmLbl->setText(QString::fromUtf8("预警: %1 工位卡箱无卡").arg(posText(pos)));
        m_alarmLbl->setStyleSheet("color:#e08000; font-weight:bold;");
    }
    else if (ev == EV_POS_LOW)
    {
        log(QString::fromUtf8("%1 工位余量不足, 请补卡").arg(posText(pos)));
    }
    else if (ev == EV_POS_FAULT)
    {
        log(QString::fromUtf8("%1 工位机构故障").arg(posText(pos)));
    }
    else if (ev == EV_LEFT_NO_CARD)
    {
        // 车没取卡就走, 卡还挂在工位上 -> 必须回收, 否则下一辆会拿错卡
        log(QString::fromUtf8("!! 车辆未取卡驶离, 工位 %1").arg(posText(pos)));
        m_alarmLbl->setText(QString::fromUtf8("报警: 车辆未取卡驶离, 卡片已回收"));
        m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
        m_takeTimer->stop();
        m_curPos = pos;
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
        if (m_state == LS_WAIT_LEAVE || m_state == LS_IDLE || m_state == LS_TAKEN)
        {
            setState(LS_IDLE);
            m_curPos = 0;
            showScreen(QString::fromUtf8("欢迎光临"));
        }
        updateLaneView();
        updatePosTable();
        updateHead();
    }
    else
    {
        log(QString::fromUtf8("未处理的事件码 %1").arg(ev));
    }
}

// ============================================================================
//  车道流程
// ============================================================================

int LaneWindow::pickPosition()
{
    // 脏点: 工位优先级写死成 1->2->3->4, 不考虑上下组负载均衡,
    //       也不看该工位卡箱还剩多少
    for (int i = 1; i <= POS_COUNT; ++i)
    {
        if (m_posState[i] == PS_READY)
            return i;
    }
    return 0;
}

void LaneWindow::startWrite()
{
    if (m_curPos < 1 || m_curPos > POS_COUNT)
    {
        enterFault(RC_NO_POS, QString::fromUtf8("没有选定工位"));
        return;
    }

    QString t = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");
    QByteArray data;
    data.append((char)m_curPos);
    data.append(packEntryData(m_stationEdit->text(), m_laneEdit->text(),
                              m_cur.kind, m_cur.plate, t, m_posCard[m_curPos]));

    setState(LS_WRITING);
    sendCmd(CMD_WRITE_CARD, data);

    log(QString::fromUtf8("写卡: %1 工位 / 入口 %2 站 %3 车道 / %4 / 车牌 %5")
        .arg(posText(m_curPos))
        .arg(m_stationEdit->text())
        .arg(m_laneEdit->text())
        .arg(kindText(m_cur.kind))
        .arg(m_cur.plate));
}

void LaneWindow::startPresent()
{
    setState(LS_PRESENTING);
    sendCmd(CMD_PRESENT_CARD, QByteArray(1, (char)m_curPos));
    log(QString::fromUtf8("送卡: %1 工位").arg(posText(m_curPos)));
}

void LaneWindow::doRecycle(int reason)
{
    // 脏点: 好几个地方调过来, 每次意图都不一样, 回收完该干嘛全靠外面判断
    setState(LS_RECOVERING);
    QByteArray p;
    p.append((char)m_curPos);
    p.append((char)reason);
    sendCmd(CMD_RECYCLE_CARD, p);
}

void LaneWindow::finishVehicle(int result, const QString &remark)
{
    if (m_cur.result != RES_RUNNING)
        return;                     // 脏: 用"结果是否已填"来防止重复关单

    m_cur.result = result;
    m_cur.remark = remark;
    m_flows.prepend(m_cur);
    appendFlowRow(m_cur);

    log(QString::fromUtf8("[流水] %1 %2 %3 工位%4 卡号%5 -> %6")
        .arg(m_cur.seq)
        .arg(m_cur.plate)
        .arg(kindText(m_cur.kind))
        .arg(posText(m_curPos))
        .arg(m_cur.cardNo)
        .arg(resText(result)));
}

void LaneWindow::enterFault(int rc, const QString &what)
{
    m_takeTimer->stop();

    setState(LS_FAULT);
    m_alarmLbl->setText(QString::fromUtf8("故障: ") + what);
    m_alarmLbl->setStyleSheet("color:#c00; font-weight:bold;");
    showScreen(QString::fromUtf8("设备故障，请联系工作人员"));
    log(QString::fromUtf8("!! %1: %2").arg(what).arg(rcText(rc)));

    // 本车按"已中止"记一笔 (旧版记成"已回收", 其实卡还在机构里)
    finishVehicle(RES_ABORTED, what);
    log(QString::fromUtf8("处理完现场后点[故障恢复 / 跳过本车], 否则下一辆车发不出去"));
}

void LaneWindow::finishRecover()
{
    if (!m_recovering)
        return;             // EV 和兜底定时器只认第一个

    m_recovering = false;

    // 脏点: 车道清空靠一条调试命令, 真机上是人工确认后按"车道复位"
    if (m_sock->state() == QAbstractSocket::ConnectedState)
        sendFire(CMD_DEBUG, QByteArray(1, (char)DBG_LANE_CLEAR));

    m_takeTimer->stop();
    m_carPresent = false;
    m_retry = 0;
    m_voiceCount = 0;
    m_stretch = STRETCH_IN;

    for (int i = 1; i <= POS_COUNT; ++i)
        m_posState[i] = PS_EMPTY;

    if (m_barrierUp)
    {
        sendFire(CMD_BARRIER, QByteArray(1, (char)0));
        m_barrierUp = false;
    }

    // 还在跑的车按"已中止"落一条流水, 不留半截
    finishVehicle(RES_ABORTED, QString::fromUtf8("故障恢复, 本车中止"));

    m_cur = VehicleRecord();
    m_curPos = 0;
    setState(LS_IDLE);
    showScreen(QString::fromUtf8("欢迎光临"));
    m_alarmLbl->setText(QString::fromUtf8("已恢复, 可接待下一辆车"));
    m_alarmLbl->setStyleSheet("color:#0a0;");
    updateVehiclePanel();
    updateLaneView();
    updatePosTable();
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

void LaneWindow::onTakeTimeout()
{
    m_voiceCount++;

    if (m_voiceCount <= 2)
    {
        showScreen(QString::fromUtf8("请取卡"));
        playVoice(VOICE_PLEASE_TAKE);
        log(QString::fromUtf8("超时未取卡, 第 %1 次重复提示").arg(m_voiceCount));
        m_takeTimer->start(15000);
        return;
    }

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
    // 脏点: 循环发 4 条命令, 共用同一个超时定时器, 后一条把前一条的超时顶掉
    for (int i = 1; i <= POS_COUNT; ++i)
        sendCmd(CMD_PREPARE_CARD, QByteArray(1, (char)i));
}

void LaneWindow::onManualWrite()
{
    int pos = m_curPos > 0 ? m_curPos : 1;
    QString plate = m_cur.plate.isEmpty() ? QString::fromUtf8("苏A00000") : m_cur.plate;
    int kind = m_cur.kind > 0 ? m_cur.kind : 1;
    QString cardNo = m_posCard[pos].isEmpty() ? QString("00000000") : m_posCard[pos];
    QString t = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");

    QByteArray data;
    data.append((char)pos);
    data.append(packEntryData(m_stationEdit->text(), m_laneEdit->text(),
                              kind, plate, t, cardNo));
    sendCmd(CMD_WRITE_CARD, data);
}

void LaneWindow::onManualPresent()
{
    int pos = m_curPos > 0 ? m_curPos : 1;
    sendCmd(CMD_PRESENT_CARD, QByteArray(1, (char)pos));
}

void LaneWindow::onManualRetract()
{
    int pos = m_curPos > 0 ? m_curPos : 1;
    sendCmd(CMD_RETRACT_CARD, QByteArray(1, (char)pos));
    m_stretch = STRETCH_IN;
    updateLaneView();
}

void LaneWindow::onManualRecycle()
{
    int pos = m_curPos > 0 ? m_curPos : 1;
    QByteArray p;
    p.append((char)pos);
    p.append((char)RECYCLE_MANUAL);
    sendCmd(CMD_RECYCLE_CARD, p);
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
    m_takeTimer->stop();
    m_retry = 0;
    m_voiceCount = 0;
    m_alarmLbl->setText(QString::fromUtf8("无"));
    m_alarmLbl->setStyleSheet("color:#0a0;");
    setState(LS_IDLE);
    updateHead();
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
    m_takeTimer->stop();

    log(QString::fromUtf8("开始故障恢复: 设备复位 -> 回收残留卡 -> 清空车道"));
    sendCmd(CMD_RESET, QByteArray());
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
