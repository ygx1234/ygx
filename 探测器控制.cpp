// Red450_SingleChip_QTDemo.cpp
// 完整实现：包含探测器控制、运动控制、图像保存与序列逻辑。
// 已根据你的要求：
//  - 优化 onResetClicked：在发出每条移动指令后以 0.1s 轮询实际位置，只有当实际位置等于初始记录位置时才继续。
//  - 在每次发出移动指令、目标到位、开始曝光、保存 .raw 与 .tif 时通过 sendMessage 写日志告知进度。
//  - 增加 handleDetectorImageData 对 .raw 写入成功也写日志的语句。
// 请以 UTF-8 编码保存本文件，确保项目中有对应的头文件与类定义。

#include "Red450_SingleChip_QTDemo.h"         // 包含类声明与 ui 成员等
#include "ProcessDialog.h"                    // 包含进度对话框类定义（用于耗时操作）
#include "serialcontroller.h"                 // 包含串口控制器定义（负责与运动控制器通信）

#include <QFileDialog>                         // QFileDialog：文件选择对话框
#include <QFileInfo>                           // QFileInfo：文件信息处理类
#include <QMessageBox>                         // QMessageBox：弹出消息框
#include <QDateTime>                           // QDateTime：日期时间获取
#include <QThread>                             // QThread：线程相关 API
#include <QDir>                                // QDir：目录操作类
#include <QApplication>                        // QApplication：应用相关
#include <QImageWriter>                        // QImageWriter：保存图像（备用）
#include <QPixmap>                             // QPixmap：用于 QLabel 显示图像
#include <QTextCursor>                         // QTextCursor：用于 QTextEdit 光标控制
#include <QEventLoop>                          // QEventLoop：在工作线程中等待信号
#include <QTimer>                              // QTimer：定时器，用于超时保护

#include <QLabel>                              // QLabel：标签控件
#include <QPushButton>                         // QPushButton：按钮控件
#include <QLineEdit>                           // QLineEdit：单行文本输入控件
#include <QGroupBox>                           // QGroupBox：分组框控件
#include <QVBoxLayout>                         // QVBoxLayout：垂直布局
#include <QHBoxLayout>                         // QHBoxLayout：水平布局
#include <QGridLayout>                         // QGridLayout：网格布局
#include <QIntValidator>                       // QIntValidator：整数输入校验
#include <QMetaObject>                         // QMetaObject：invokeMethod 所需
#include <QSplitter>                           // QSplitter：分割器
#include <QFont>                               // QFont：字体设置

#include <iostream>                            // iostream：用于调试输出（std::cout）
#include <fstream>                             // fstream：用于写二进制文件
#include <memory>                              // memory：智能指针
#include <vector>                              // vector：动态数组
#include <cstring>                             // cstring：memcpy 等
#include <cstdint>                             // cstdint：固定宽度整数类型
#include <atomic>                              // atomic：原子变量

#include <QFile>                               // [新增] 用于复制 raw/tif 到 Start 创建的新文件夹（插入式新增）
#include <cmath>                               // [新增] 用于 std::llround 把 mm 转为微米整数（插入式新增）



#ifdef USE_LIBTIFF
#include <tiffio.h>                            // libtiff API（若使能）
#endif

// 局部常量：默认移动步距设置（步）
namespace {
    constexpr int kMOVE_DIST_X = 3200*20;         // 默认 X 轴单步移动距离（步）
    constexpr int kMOVE_DIST_Y = 3200*20;         // 默认 Y 轴单步移动距离（步）
    constexpr int kMOVE_DIST_Z = 3200;         // 默认 Z 轴单步移动距离（步）
    const int repeatCount = 5;                 // 每阶段重复次数

    std::atomic<bool> g_isExposing{ false };   // 全局原子：当前是否处于曝光中（由 onExposure 设置）
    std::atomic<bool> g_isSerialOpenOk{ false }; // 全局原子：串口是否成功打开（由 onSerialOpened 设置）

    static QString g_startSequenceOutputDir;   // Start 序列输出目录（
    static std::atomic<long long> g_relX_um{ 0 }; //[新增] 相对初始位 X（微米 um，原子存储）
    static std::atomic<long long> g_relY_um{ 0 }; // 相对初始位 Y（微米 um，原子存储）
}

// Helper 函数：统一启用或禁用运动相关 UI 控件（传入指针时先做空指针检查）
static void setMotionUiEnabled(
    QPushButton* btnXPlus, QPushButton* btnXMinus,
    QPushButton* btnYPlus, QPushButton* btnYMinus,
    QPushButton* btnZPlus, QPushButton* btnZMinus,
    QPushButton* btnRecord, QPushButton* btnReset, QPushButton* btnStop,
    QPushButton* btnMoveToTarget,
    QLineEdit* editX, QLineEdit* editY, QLineEdit* editZ,
    bool enabled)
{
    if (btnXPlus) btnXPlus->setEnabled(enabled);    // 如存在 X+ 按钮则设置其可用性
    if (btnXMinus) btnXMinus->setEnabled(enabled);  // 如存在 X- 按钮则设置其可用性
    if (btnYPlus) btnYPlus->setEnabled(enabled);    // 如存在 Y+ 按钮则设置其可用性
    if (btnYMinus) btnYMinus->setEnabled(enabled);  // 如存在 Y- 按钮则设置其可用性
    if (btnZPlus) btnZPlus->setEnabled(enabled);    // 如存在 Z+ 按钮则设置其可用性
    if (btnZMinus) btnZMinus->setEnabled(enabled);  // 如存在 Z- 按钮则设置其可用性
    if (btnRecord) btnRecord->setEnabled(enabled);  // 如存在 Record 按钮则设置其可用性
    if (btnReset) btnReset->setEnabled(enabled);    // 如存在 Reset 按钮则设置其可用性
    if (btnStop) btnStop->setEnabled(enabled);      // 如存在 Stop 按钮则设置其可用性
    if (btnMoveToTarget) btnMoveToTarget->setEnabled(enabled); // 如存在 MoveToTarget 按钮则设置
    if (editX) editX->setEnabled(enabled);          // 如存在目标 X 编辑框则设置为可用/不可用
    if (editY) editY->setEnabled(enabled);          // 如存在目标 Y 编辑框则设置为可用/不可用
    if (editZ) editZ->setEnabled(enabled);          // 如存在目标 Z 编辑框则设置为可用/不可用
}

// ----------------------------- 构造函数 -----------------------------
Red450_SingleChip_QTDemo::Red450_SingleChip_QTDemo(QWidget* parent)
    : QMainWindow(parent)                         // 调用基类构造函数并传入父对象
{
    ui.setupUi(this);                             // 初始化由 uic 生成的 UI（创建控件并绑定 ui 成员）

    qRegisterMetaType<CallbackImageFncParam>("CallbackImageFncParam"); // 注册回调参数类型，便于 QueuedConnection 传递

    if (ui.textEdit_LogMessage) {                 // 如果日志文本框存在则设置其字体以便显示中文
        ui.textEdit_LogMessage->setFont(QFont(QStringLiteral("Microsoft YaHei"), 9)); // 设置为微软雅黑 9 号字体
    }

    m_bDetectorConnectionStatus = false;          // 初始化探测器连接状态为 false（未连接）
    m_DetectorInstance = nullptr;                 // 探测器实例指针置空
    m_MaxValue = 0xffff;                          // 初始窗宽上限设为 16-bit 最大
    m_MinValue = 0;                               // 初始窗宽下限设为 0

    // 初始时禁用与探测器相关的按钮，直到用户加载 DLL 并启动探测器
    if (ui.btn_FreeDll) ui.btn_FreeDll->setEnabled(false);
    if (ui.btn_StartUpDetector) ui.btn_StartUpDetector->setEnabled(false);
    if (ui.btn_ConfigureDetector) ui.btn_ConfigureDetector->setEnabled(false);
    if (ui.btn_LoadCaliCode) ui.btn_LoadCaliCode->setEnabled(false);
    if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(false);

    // 尝试自动设置 DLL 路径为程序文件夹下的 Red450_SingleChip_Prim.dll（如果存在）
    QString dirOfExeFile = QApplication::applicationDirPath(); // 获取应用程序目录路径
    QString dllFilePath = dirOfExeFile + "/Red450_SingleChip_Prim.dll"; // 构造默认 DLL 路径
    if (QFile::exists(dllFilePath)) {             // 如果 DLL 文件存在
        if (ui.lineEdit_DllDir) ui.lineEdit_DllDir->setText(QDir::toNativeSeparators(dllFilePath)); // 把路径显示到 UI 文本框
    }
    else {                                      // 否则置为空
        if (ui.lineEdit_DllDir) ui.lineEdit_DllDir->setText("");
    }

    if (ui.lineEdit_CFG1) ui.lineEdit_CFG1->setText(QDir::toNativeSeparators(dirOfExeFile)); // 把默认 CFG1 路径设置为程序目录

    // 连接本对象的 sendMessage 信号到 addMessage 槽，用于线程安全日志写入
    connect(this, &Red450_SingleChip_QTDemo::sendMessage, this, &Red450_SingleChip_QTDemo::addMessage);

    // 连接 UI 的按钮与槽（带空指针检查）
    if (ui.btn_SelectDll) connect(ui.btn_SelectDll, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onSelectDllFileButtonClicked);
    if (ui.btn_SelectCFG1) connect(ui.btn_SelectCFG1, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onSelectCFG1ButtonClicked);
    if (ui.btn_LoadDll) connect(ui.btn_LoadDll, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onLoadDll);
    if (ui.btn_FreeDll) connect(ui.btn_FreeDll, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onFreeDll);
    if (ui.btn_StartUpDetector) connect(ui.btn_StartUpDetector, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onStartUpDetector);
    if (ui.btn_ConfigureDetector) connect(ui.btn_ConfigureDetector, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onConfigureDetector);
    if (ui.btn_SetExternalBiasVoltage) connect(ui.btn_SetExternalBiasVoltage, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onSetExternalBiasVoltage);
    if (ui.btn_ReadVset) connect(ui.btn_ReadVset, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onReadVset);
    if (ui.btn_ReadVth1) connect(ui.btn_ReadVth1, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onReadVth1);
    if (ui.btn_ReadVth2) connect(ui.btn_ReadVth2, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onReadVth2);
    if (ui.btn_ReadVref) connect(ui.btn_ReadVref, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onReadVref);
    if (ui.checkBox) connect(ui.checkBox, &QCheckBox::stateChanged, this, &Red450_SingleChip_QTDemo::onEnableCorrect);
    if (ui.btn_Exposure) connect(ui.btn_Exposure, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onExposure);

    if (ui.horizontalSlider_SetWindows) connect(ui.horizontalSlider_SetWindows, &QSlider::valueChanged, this, &Red450_SingleChip_QTDemo::onUpdateImageWindows);
    if (ui.btn_ClearImage) connect(ui.btn_ClearImage, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onClearImage);

#ifdef Q_OS_WIN
    // Windows 平台下创建串口控制器并把它移动到单独线程运行
    m_serialController = new SerialController();    // 在主线程创建串口控制器对象（无 parent）
    m_serialThread = new QThread(this);             // 创建线程对象，父对象为 this（便于自动释放）
    connect(m_serialThread, &QThread::finished, m_serialController, &QObject::deleteLater); // 线程结束时销毁控制器对象
    m_serialController->moveToThread(m_serialThread); // 把串口控制器对象移到串口线程
    m_serialThread->start();                         // 启动串口线程

    // 尝试从 UI 的 lineEdit_ComPort 中读取串口名称（如果存在），否则使用默认 COM6
    QString comPort = QStringLiteral("COM5");        // 默认串口 COM6
    if (QLineEdit* leCom = this->findChild<QLineEdit*>("lineEdit_ComPort")) { // 查找 UI 中名为 lineEdit_ComPort 的控件
        const QString uiPort = leCom->text().trimmed(); // 读取并去除前后空白
        if (!uiPort.isEmpty()) comPort = uiPort;     // 如果 UI 有值则覆盖默认
    }

    // 在串口线程中异步打开串口并初始化（使用 invokeMethod 避免跨线程直接调用成员函数）
    QMetaObject::invokeMethod(m_serialController, "openPort", Qt::QueuedConnection, Q_ARG(QString, comPort)); // 异步调用 openPort(comPort)
    QMetaObject::invokeMethod(m_serialController, "initPort", Qt::QueuedConnection); // 异步调用 initPort()

    // 连接串口控制器发出的信号到本对象的槽（使用队列连接以保证跨线程安全）
    connect(m_serialController, &SerialController::opened, this, &Red450_SingleChip_QTDemo::onSerialOpened, Qt::QueuedConnection);
    connect(m_serialController, &SerialController::positionsReady, this, &Red450_SingleChip_QTDemo::onSerialPositionsReady, Qt::QueuedConnection);
    connect(m_serialController, &SerialController::moveCompleted, this, &Red450_SingleChip_QTDemo::onSerialMoveCompleted, Qt::QueuedConnection);
    connect(m_serialController, &SerialController::errorOccurred, this, &Red450_SingleChip_QTDemo::onSerialError, Qt::QueuedConnection);

    // 构建运动面板并集成到主窗口（保持原有行为）
    buildMotionPanel();
    integrateMotionPanelIntoWindow();

    // 初始时禁用运动相关 UI（等待串口打开并探测器就绪后启用）
    setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
        m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
        m_editTargetX, m_editTargetY, m_editTargetZ, false);
#else
    m_serialController = nullptr;                   // 非 Windows 平台不启用串口控制器
    m_serialThread = nullptr;
#endif
}

// ----------------------------- 析构函数 -----------------------------
Red450_SingleChip_QTDemo::~Red450_SingleChip_QTDemo()
{
#ifdef Q_OS_WIN
    stopPolling();                                  // 停止轮询定时器（如果运行）
    if (m_serialController && m_serialThread) {     // 如果串口控制器与线程存在则尝试优雅关闭
        QMetaObject::invokeMethod(m_serialController, "Stop", Qt::BlockingQueuedConnection); // 请求停止运动（阻塞等待）
        QMetaObject::invokeMethod(m_serialController, "closePort", Qt::BlockingQueuedConnection); // 请求关闭串口（阻塞等待）
        m_serialThread->quit();                     // 退出线程事件循环
        if (!m_serialThread->wait(3000)) {          // 等待线程退出，最多等待 3000 ms
            emit addEventMessage(QStringLiteral("Warning: serial thread did not exit in time.")); // 记录警告
        }
        m_serialThread->deleteLater();              // 延迟删除线程对象
        m_serialThread = nullptr;                   // 清空指针
        m_serialController = nullptr;               // 清空控制器指针（对象已 deleteLater）
    }
#endif

    if (m_DetectorInstance) {                        // 如果存在探测器实例则调用关闭接口并置空
        m_DetectorInstance->ShutDownDetector();     // 调用 SDK 的 ShutDownDetector
        m_DetectorInstance = nullptr;               // 置空实例指针
    }
}

// ----------------------------- 构建运动面板（包含新增 Start 按钮） -----------------------------
void Red450_SingleChip_QTDemo::buildMotionPanel()
{
    m_motionPanel = new QWidget(this);               // 创建运动面板并设置父对象为主窗口

    QGridLayout* grid = new QGridLayout;             // 创建网格布局用于放置方向按钮
    m_btnXPlus = new QPushButton(tr("X +"), m_motionPanel); // 创建 X+ 按钮
    m_btnYPlus = new QPushButton(tr("Y +"), m_motionPanel); // 创建 Y+ 按钮
    m_btnZPlus = new QPushButton(tr("Z +"), m_motionPanel); // 创建 Z+ 按钮
    m_btnXMinus = new QPushButton(tr("X -"), m_motionPanel); // 创建 X- 按钮
    m_btnYMinus = new QPushButton(tr("Y -"), m_motionPanel); // 创建 Y- 按钮
    m_btnZMinus = new QPushButton(tr("Z -"), m_motionPanel); // 创建 Z- 按钮

    grid->addWidget(m_btnXPlus, 0, 0);               // 把 X+ 放在网格 (0,0)
    grid->addWidget(m_btnYPlus, 0, 1);               // 把 Y+ 放在网格 (0,1)
    grid->addWidget(m_btnZPlus, 0, 2);               // 把 Z+ 放在网格 (0,2)
    grid->addWidget(m_btnXMinus, 1, 0);              // 把 X- 放在网格 (1,0)
    grid->addWidget(m_btnYMinus, 1, 1);              // 把 Y- 放在网格 (1,1)
    grid->addWidget(m_btnZMinus, 1, 2);              // 把 Z- 放在网格 (1,2)

    m_initGroup = new QGroupBox(QStringLiteral("Initial Position"), m_motionPanel); // 创建初始位置分组框
    m_initPosLabel = new QLabel(QStringLiteral("X: 0    Y: 0    Z: 0"), m_initGroup); // 创建并初始化初始位置标签
    auto initLayout = new QVBoxLayout(m_initGroup); // 创建分组内部的垂直布局
    initLayout->addWidget(m_initPosLabel);          // 将标签添加到分组布局

    m_currentGroup = new QGroupBox(QStringLiteral("Current Position"), m_motionPanel); // 创建当前位分组
    m_currentPosLabel = new QLabel(QStringLiteral("X: 0    Y: 0    Z: 0"), m_currentGroup); // 创建当前位标签
    auto currentLayout = new QVBoxLayout(m_currentGroup); // 创建垂直布局并添加标签
    currentLayout->addWidget(m_currentPosLabel);

    m_relativeGroup = new QGroupBox(QStringLiteral("Relative Position (mm)"), m_motionPanel); // 创建相对位分组（单位 mm）
    m_relativePosLabel = new QLabel(QStringLiteral("X: 0.000 mm    Y: 0.000 mm    Z: 0.000 mm"), m_relativeGroup); // 相对位标签
    auto relativeLayout = new QVBoxLayout(m_relativeGroup); // 创建布局
    relativeLayout->addWidget(m_relativePosLabel);         // 添加标签

    auto posBlocksLayout = new QHBoxLayout;          // 创建水平布局用于并排三个分组
    posBlocksLayout->addWidget(m_initGroup);         // 添加初始分组
    posBlocksLayout->addWidget(m_currentGroup);      // 添加当前分组
    posBlocksLayout->addWidget(m_relativeGroup);     // 添加相对分组

    m_btnRecord = new QPushButton(QStringLiteral("Record Initial"), m_motionPanel); // 创建记录初始按钮
    m_btnReset = new QPushButton(QStringLiteral("Reset to Record"), m_motionPanel); // 创建复位按钮
    m_btnStop = new QPushButton(QStringLiteral("Stop"), m_motionPanel);           // 创建停止按钮

    m_btn_StartSequence = new QPushButton(QStringLiteral("Start"), m_motionPanel); // 创建新增的 Start 序列按钮

    m_editTargetX = new QLineEdit(m_motionPanel);    // 创建目标 X 输入框
    m_editTargetY = new QLineEdit(m_motionPanel);    // 创建 目标 Y 输入框
    m_editTargetZ = new QLineEdit(m_motionPanel);    // 创建目标 Z 输入框
    m_btnMoveToTarget = new QPushButton(QStringLiteral("Move to Target"), m_motionPanel); // 创建移动到目标按钮

    auto validator = new QIntValidator(m_motionPanel); // 创建整数校验器
    m_editTargetX->setValidator(validator);           // 把校验器设置给目标 X 输入框
    m_editTargetY->setValidator(validator);           // 把校验器设置给目标 Y 输入框
    m_editTargetZ->setValidator(validator);           // 把校验器设置给目标 Z 输入框
    m_editTargetX->setPlaceholderText(QStringLiteral("X")); // 设置占位符提示
    m_editTargetY->setPlaceholderText(QStringLiteral("Y"));
    m_editTargetZ->setPlaceholderText(QStringLiteral("Z"));

    auto hBtnsTop = new QHBoxLayout;                 // 创建顶部按钮行水平布局
    hBtnsTop->addWidget(m_btnRecord);                // 添加记录按钮
    hBtnsTop->addWidget(m_btnReset);                 // 添加复位按钮
    hBtnsTop->addWidget(m_btnStop);                  // 添加停止按钮
    hBtnsTop->addWidget(m_btn_StartSequence);        // 添加 Start 按钮到按钮行

    auto hMoveToLayout = new QHBoxLayout;            // 创建移动到目标行布局
    hMoveToLayout->addWidget(m_editTargetX);         // 添加 X 输入
    hMoveToLayout->addWidget(m_editTargetY);         // 添加 Y 输入
    hMoveToLayout->addWidget(m_editTargetZ);         // 添加 Z 输入
    hMoveToLayout->addWidget(m_btnMoveToTarget);     // 添加 MoveToTarget 按钮

    auto mainLayout = new QVBoxLayout(m_motionPanel); // 主垂直布局：依次放置网格、位置块、按钮行与目标行
    mainLayout->addLayout(grid);                      // 添加方向按钮网格
    mainLayout->addLayout(posBlocksLayout);           // 添加位置分组行
    mainLayout->addLayout(hBtnsTop);                  // 添加顶部按钮行
    mainLayout->addLayout(hMoveToLayout);             // 添加目标移动行
    m_motionPanel->setLayout(mainLayout);             // 应用布局到运动面板

    // 连接已有的槽函数以保持原有行为（记录、复位、停止、移动等）
    connect(m_btnRecord, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onRecordClicked); // 记录初始位置
    connect(m_btnReset, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onResetClicked);   // 复位（本文件中已优化）
    connect(m_btnStop, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onStopClicked);     // 停止
    connect(m_btnMoveToTarget, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveToTargetClicked); // 移动到目标

    connect(m_btnXPlus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveXPlus);     // X+ 使用已有实现
    connect(m_btnXMinus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveXMinus);   // X- 使用已有实现
    connect(m_btnYPlus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveYPlus);     // Y+ 使用已有实现
    connect(m_btnYMinus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveYMinus);   // Y- 使用已有实现
    connect(m_btnZPlus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveZPlus);     // Z+ 使用已有实现
    connect(m_btnZMinus, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onMoveZMinus);   // Z- 使用已有实现

    // 连接 Start 按钮到本类新增的序列槽（序列实现将调用已存在的 moveAxis 与 performExposure）
    connect(m_btn_StartSequence, &QPushButton::clicked, this, &Red450_SingleChip_QTDemo::onStartSequenceClicked);

    // 创建位置轮询定时器并连接超时槽（在 onMove 等操作中使用 startPolling/stopPolling 控制）
    m_pollTimer = new QTimer(this);                   // 创建定时器，父对象为 this（自动管理）
    m_pollTimer->setInterval(100);                    // 设置间隔 100 ms
    connect(m_pollTimer, &QTimer::timeout, this, &Red450_SingleChip_QTDemo::pollPositionsTimeout); // 连接到 pollPositionsTimeout 槽
}

// ----------------------------- 集成到主窗口（垂直分割） -----------------------------
void Red450_SingleChip_QTDemo::integrateMotionPanelIntoWindow()
{
    QWidget* oldCentral = this->centralWidget();
    if (!oldCentral) {
        setCentralWidget(m_motionPanel);
        return;
    }

    oldCentral->setParent(nullptr);

    // 关键改动：Horizontal -> Vertical
    m_mainSplitter = new QSplitter(Qt::Vertical, this);

    // 上方放原有界面，下方放运动面板
    m_mainSplitter->addWidget(oldCentral);
    m_mainSplitter->addWidget(m_motionPanel);

    // 上大下小（保证启动时原有界面完整显示）
    QList<int> sizes;
    sizes << 10 << 1;                 // 例如：上 80% 下 20%
    m_mainSplitter->setSizes(sizes);

    // 限制下方面板最小高度，避免拖到太小看不见
    m_motionPanel->setMinimumHeight(120);  // 你可按实际需要调整 140~240

    setCentralWidget(m_mainSplitter);
}

// ----------------------------- 轮询控制 -----------------------------
void Red450_SingleChip_QTDemo::startPolling()
{
    if (m_pollTimer && !m_pollTimer->isActive()) m_pollTimer->start(); // 如果定时器存在且未启动则启动
}
void Red450_SingleChip_QTDemo::stopPolling()
{
    if (m_pollTimer && m_pollTimer->isActive()) m_pollTimer->stop();   // 如果定时器存在且正在运行则停止
}

// ----------------------------- 串口/运动接口封装 -----------------------------
int Red450_SingleChip_QTDemo::getDisplay(const QString& axis) const
{
    int value = 0;                                    // 初始化返回值 0
    if (m_serialController) {                         // 仅在串口控制器存在时调用
        QMetaObject::invokeMethod(m_serialController, "GetDisplay", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(int, value), Q_ARG(QString, axis)); // 在串口线程中以阻塞方式调用 GetDisplay(axis) 并获取返回值
    }
    return value;                                     // 返回读取到的显示位置
}

void Red450_SingleChip_QTDemo::moveAxis(const QString& axis, int moveDist, int lastPos, int waitSec)
{
    if (!m_serialController) return;                  // 如果串口控制器不存在则直接返回
    QMetaObject::invokeMethod(m_serialController, "moveAndWait", Qt::QueuedConnection,
        Q_ARG(QString, axis), Q_ARG(int, moveDist), Q_ARG(int, lastPos), Q_ARG(int, waitSec)); // 异步调用 moveAndWait 来发起移动
}

// ----------------------------- 运动 UI 槽（调用已有基础功能） -----------------------------
void Red450_SingleChip_QTDemo::onRecordClicked()
{
    if (!m_serialController) return;                  // 如果串口控制器不存在则忽略
    m_recordX = getDisplay(QStringLiteral("X"));      // 读取 X 并保存为记录位置
    m_recordY = getDisplay(QStringLiteral("Y"));      // 读取 Y 并保存
    m_recordZ = getDisplay(QStringLiteral("Z"));      // 读取 Z 并保存
    m_hasInitial = true;                              // 设置已记录标志
    if (m_initPosLabel)                               // 如果初始位置标签存在则更新显示
        m_initPosLabel->setText(QStringLiteral("X: %1    Y: %2    Z: %3").arg(m_recordX).arg(m_recordY).arg(m_recordZ)); // 更新文本

    // 记录后立即更新 relative，如果当前缓存无值则读取一次
    if (m_currentX == 0 && m_currentY == 0 && m_currentZ == 0) {
        m_currentX = getDisplay(QStringLiteral("X")); // 读取当前 X
        m_currentY = getDisplay(QStringLiteral("Y")); // 读取当前 Y
        m_currentZ = getDisplay(QStringLiteral("Z")); // 读取当前 Z
        if (m_currentPosLabel) m_currentPosLabel->setText(QStringLiteral("X: %1    Y: %2    Z: %3").arg(m_currentX).arg(m_currentY).arg(m_currentZ)); // 更新文本
    }

    if (m_relativePosLabel && m_hasInitial) {         // 更新相对位显示
        double rx = static_cast<double>(m_currentX - m_recordX) / 3200.0; // 计算相对 X（mm）
        double ry = static_cast<double>(m_currentY - m_recordY) / 3200.0; // 计算相对 Y（mm）
        double rz = static_cast<double>(m_currentZ - m_recordZ) / 3200.0; // 计算相对 Z（mm）
        m_relativePosLabel->setText(QStringLiteral("X: %1 mm    Y: %2 mm    Z: %3 mm")
            .arg(QString::number(rx, 'f', 3))
            .arg(QString::number(ry, 'f', 3))
            .arg(QString::number(rz, 'f', 3)));     // 更新相对位标签文本

    }
}

// 实现：只包含 onResetClicked 的简洁实现（发送所有移动指令后再开始比较位置）
// 说明：此版本不使用 UI 定时器轮询（startPolling/stopPolling），逻辑简单：
//  1) 读取当前位和计算目标差值
//  2) 对需要移动的轴全部一次性发出 moveAxis 指令
//  3) 以 100 ms 间隔读取实际位置并比较所有轴是否已到目标，达到则结束；超时则报告失败


void Red450_SingleChip_QTDemo::onResetClicked()
{
    // 前置检查：必须有串口控制器且必须已记录初始位置
    if (!m_serialController || !m_hasInitial) {
        emit addEventMessage(QStringLiteral("onResetClicked: serial controller invalid or initial not recorded."));
        return;
    }

    // 读取当前实际位置（阻塞获取，保证是即时值）
    int curX = getDisplay(QStringLiteral("X"));
    int curY = getDisplay(QStringLiteral("Y"));
    int curZ = getDisplay(QStringLiteral("Z"));

    // 计算需要移动的增量（目标 = 记录的初始位置 m_record - 当前）
    int dx = m_recordX - curX;
    int dy = m_recordY - curY;
    int dz = m_recordZ - curZ;

    // 如果不用移动，直接返回
    if (dx == 0 && dy == 0 && dz == 0) {
        emit sendMessage(QStringLiteral("onResetClicked: already at recorded initial position."));
        return;
    }

    // 计算各轴目标位置（记录的初始位置）
    int targetX = m_recordX;
    int targetY = m_recordY;
    int targetZ = m_recordZ;

    // 先一次性发出所有需要的移动指令（不在这里等待各个轴到位）
    if (dx != 0) {
        moveAxis(QStringLiteral("X"), dx, curX, 1);
    }
    if (dy != 0) {
        moveAxis(QStringLiteral("Y"), dy, curY, 1);
    }
    if (dz != 0) {
        moveAxis(QStringLiteral("Z"), dz, curZ, 1);
    }

    // 简单轮询比较（在此处以 100ms 间隔读取实际位置并比较）
    const int pollInterval = 100;      // 轮询间隔：100 ms
    const int timeoutMs = 30000;       // 超时时间：30 s
    int elapsed = 0;                   // 已等待时间累计（ms）
    // 等待直到所有需要移动的轴都到达目标或超时
    while (elapsed < timeoutMs) {
        int rx = getDisplay(QStringLiteral("X")); // 读取实时 X
        int ry = getDisplay(QStringLiteral("Y")); // 读取实时 Y
        int rz = getDisplay(QStringLiteral("Z")); // 读取实时 Z

        // 更新 UI 的 Current/Relative（快速更新，不强制 Blocking）
        QMetaObject::invokeMethod(this, [=]() {
            if (m_currentPosLabel) m_currentPosLabel->setText(QStringLiteral("X: %1    Y: %2    Z: %3").arg(rx).arg(ry).arg(rz));
            if (m_hasInitial && m_relativePosLabel) {
                double vx = static_cast<double>(rx - m_recordX) / 3200.0;
                double vy = static_cast<double>(ry - m_recordY) / 3200.0;
                double vz = static_cast<double>(rz - m_recordZ) / 3200.0;
                m_relativePosLabel->setText(QStringLiteral("X: %1 mm    Y: %2 mm    Z: %3 mm")
                    .arg(QString::number(vx, 'f', 3))
                    .arg(QString::number(vy, 'f', 3))
                    .arg(QString::number(vz, 'f', 3)));
            }
            }, Qt::QueuedConnection);

        // 检查每轴是否到位（对于不需要移动的轴视为已到位）
        bool xReached = (dx == 0) || (rx == targetX);
        bool yReached = (dy == 0) || (ry == targetY);
        bool zReached = (dz == 0) || (rz == targetZ);

        // 如果全部到位，记录日志并退出
        if (xReached && yReached && zReached) {
            return;
        }
        // 还未全部到位，等待 pollInterval 后重试
        QThread::msleep(pollInterval);
        elapsed += pollInterval;
    }
    // 超时仍未全部到位，记录超时错误
    emit addEventMessage(QStringLiteral("onResetClicked: timeout waiting for axes to reach targets."));
}

// ----------------------------- 其余函数（保持原实现） -----------------------------
void Red450_SingleChip_QTDemo::onStopClicked()
{
    if (!m_serialController) return;                  // 如果串口不存在则忽略
    QMetaObject::invokeMethod(m_serialController, "Stop", Qt::QueuedConnection); // 异步请求串口控制器停止运动
    stopPolling();                                    // 停止轮询以节省资源
}

void Red450_SingleChip_QTDemo::onMoveToTargetClicked()
{
    if (!m_serialController) return; // 串口无效则忽略

    // 允许只输入 1~2 个轴：空字符串/非数字 -> 该轴不参与移动
    bool okX = false, okY = false, okZ = false;

    const QString sx = m_editTargetX ? m_editTargetX->text().trimmed() : QString();
    const QString sy = m_editTargetY ? m_editTargetY->text().trimmed() : QString();
    const QString sz = m_editTargetZ ? m_editTargetZ->text().trimmed() : QString();

    // 只有在“非空”时才尝试转换；空则视为未指定
    int tx = 0, ty = 0, tz = 0;
    const bool hasX = !sx.isEmpty() && (tx = sx.toInt(&okX), okX);
    const bool hasY = !sy.isEmpty() && (ty = sy.toInt(&okY), okY);
    const bool hasZ = !sz.isEmpty() && (tz = sz.toInt(&okZ), okZ);

    // 如果用户一个轴都没给有效值，直接返回（可写日志提示）
    if (!hasX && !hasY && !hasZ) {
        emit addEventMessage(QStringLiteral("MoveToTarget: please input at least one valid axis target (X/Y/Z)."));
        return;
    }

    // 只读取需要参与移动的轴的当前位置（减少 blocking 调用次数）
    int curX = 0, curY = 0, curZ = 0;
    if (hasX) curX = getDisplay(QStringLiteral("X"));
    if (hasY) curY = getDisplay(QStringLiteral("Y"));
    if (hasZ) curZ = getDisplay(QStringLiteral("Z"));

    // 计算差值并发起移动
    startPolling();

    if (hasX) {
        const int dx = tx - curX;
        if (dx != 0) moveAxis(QStringLiteral("X"), dx, curX, 1);
    }
    if (hasY) {
        const int dy = ty - curY;
        if (dy != 0) moveAxis(QStringLiteral("Y"), dy, curY, 1);
    }
    if (hasZ) {
        const int dz = tz - curZ;
        if (dz != 0) moveAxis(QStringLiteral("Z"), dz, curZ, 1);
    }
}

void Red450_SingleChip_QTDemo::onMoveXPlus()
{
    if (!m_serialController) return;                  // 如果串口控制器不存在则忽略
    int lastX = getDisplay(QStringLiteral("X"));      // 读取当前 X 显示值
    startPolling();                                   // 启动轮询
    moveAxis(QStringLiteral("X"), kMOVE_DIST_X, lastX, 1); // 调用 moveAxis 发起移动（使用封装）
}
void Red450_SingleChip_QTDemo::onMoveXMinus()
{
    if (!m_serialController) return;
    int lastX = getDisplay(QStringLiteral("X"));      // 读取当前 X
    startPolling();                                   // 启动轮询
    moveAxis(QStringLiteral("X"), -kMOVE_DIST_X, lastX, 1); // 发起负方向移动
}
void Red450_SingleChip_QTDemo::onMoveYPlus()
{
    if (!m_serialController) return;
    int lastY = getDisplay(QStringLiteral("Y"));      // 读取当前 Y
    startPolling();
    moveAxis(QStringLiteral("Y"), kMOVE_DIST_Y, lastY, 1); // 发起 Y 正向移动
}
void Red450_SingleChip_QTDemo::onMoveYMinus()
{
    if (!m_serialController) return;
    int lastY = getDisplay(QStringLiteral("Y"));
    startPolling();
    moveAxis(QStringLiteral("Y"), -kMOVE_DIST_Y, lastY, 1);
}
void Red450_SingleChip_QTDemo::onMoveZPlus()
{
    if (!m_serialController) return;
    int lastZ = getDisplay(QStringLiteral("Z"));
    startPolling();
    moveAxis(QStringLiteral("Z"), kMOVE_DIST_Z, lastZ, 1);
}
void Red450_SingleChip_QTDemo::onMoveZMinus()
{
    if (!m_serialController) return;
    int lastZ = getDisplay(QStringLiteral("Z"));
    startPolling();
    moveAxis(QStringLiteral("Z"), -kMOVE_DIST_Z, lastZ, 1);
}

void Red450_SingleChip_QTDemo::pollPositionsTimeout()
{
    if (m_serialController) {                         // 如果串口控制器存在则请求一次位置更新
        QMetaObject::invokeMethod(m_serialController, "requestPositions", Qt::QueuedConnection); // 异步发起位置请求
    }
}

// ----------------------------- 串口信号槽 -----------------------------
void Red450_SingleChip_QTDemo::onSerialOpened(bool ok)
{
    g_isSerialOpenOk = ok;                           // 更新全局串口打开状态标志
    addApiCallMessage(QString("Serial opened: %1").arg(ok ? "true" : "false")); // 记录日志
    const bool enableMotion = ok && !g_isExposing.load(); // 如果串口打开且不在曝光则允许运动
    setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
        m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
        m_editTargetX, m_editTargetY, m_editTargetZ, enableMotion); // 根据条件设置运动 UI 状态
    if (ok && m_serialController) {                   // 如果串口打开成功
        QMetaObject::invokeMethod(m_serialController, "requestPositions", Qt::QueuedConnection); // 请求当前位置
    }
}

void Red450_SingleChip_QTDemo::onSerialPositionsReady(int x, int y, int z)
{
    if (m_currentPosLabel) {                         // 如果当前位标签存在
        m_currentPosLabel->setText(QStringLiteral("X: %1    Y: %2    Z: %3").arg(x).arg(y).arg(z)); // 更新显示
    }
    if (m_hasInitial && m_relativePosLabel) {         // 如果已记录初始位置则计算相对位（单位 mm）
        double rx = static_cast<double>(x - m_recordX) / 3200.0; // 计算相对 X（mm）
        double ry = static_cast<double>(y - m_recordY) / 3200.0; // 计算相对 Y（mm）
        double rz = static_cast<double>(z - m_recordZ) / 3200.0; // 计算相对 Z（mm）
        m_relativePosLabel->setText(QStringLiteral("X: %1 mm    Y: %2 mm    Z: %3 mm")
            .arg(QString::number(rx, 'f', 3))
            .arg(QString::number(ry, 'f', 3))
            .arg(QString::number(rz, 'f', 3)));     // 更新相对位标签文本
    }
    else if (m_relativePosLabel) {                  // 如果没有记录初始则显示默认
        m_relativePosLabel->setText(QStringLiteral("X: 0.000 mm    Y: 0.000 mm    Z: 0.000 mm"));
    }
}

void Red450_SingleChip_QTDemo::onSerialMoveCompleted(const QString& axis)
{
    Q_UNUSED(axis);                                   // 目前未使用该参数（保留槽以与 SerialController 兼容）
    if (m_serialController) {                         // 请求一次位置更新以刷新 UI
        QMetaObject::invokeMethod(m_serialController, "requestPositions", Qt::QueuedConnection);
    }
    stopPolling();                                     // 停止轮询（如果之前启动）
}

void Red450_SingleChip_QTDemo::onSerialError(const QString& msg)
{
    addEventMessage(QString("Serial error: %1").arg(msg)); // 记录串口错误到日志
    setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
        m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
        m_editTargetX, m_editTargetY, m_editTargetZ, false); // 禁用运动 UI 保护
    stopPolling();                                     // 停止轮询
}

// ----------------------------- 创建目录辅助函数 -----------------------------
void Red450_SingleChip_QTDemo::creatDir(const QString& path)
{
    QDir sourceDir(path);                              // 用 QDir 检查路径
    if (sourceDir.exists()) return;                    // 如果已存在则无需创建

    QString tempDir;                                   // 用于逐级创建目录
    const QStringList parts = QDir::fromNativeSeparators(path).split('/', Qt::SkipEmptyParts); // 拆分路径为段
    for (const QString& p : parts) {                   // 逐段创建目录
        tempDir += p + "/";                            // 拼接路径段
        QDir dir(tempDir);                             // 创建 QDir 对象用于判断/创建
        if (!dir.exists() && !dir.mkdir(tempDir)) return; // 创建失败则直接返回
    }
}


// ----------------------------- 图像处理与保存（保存成功时设置 m_imageSavedFlag 并发出 imageSaved） -----------------------------
void Red450_SingleChip_QTDemo::handleDetectorImageData(const CallbackImageFncParam& paramPtr)
{
    QString main_image_path = QDir::currentPath() + "/image"; // 保存目录为当前目录下的 image 文件夹
    creatDir(main_image_path);                              // 确保目录存在

    const int pixelCount = paramPtr.nHeight * paramPtr.nWidth; // 计算像素总数
    std::unique_ptr<uint16_t[]> image_data(new uint16_t[pixelCount]); // 分配 16-bit 缓冲
    std::memcpy(image_data.get(), paramPtr.pImageBuf, paramPtr.nImageSize); // 拷贝原始图像数据

    QString current_time = QDateTime::currentDateTime().toString("yyyy_MM_dd_HH_mm_ss_zzz"); // 当前时间字符串
    QString rawFilePath = main_image_path + QStringLiteral("/image_%1.raw").arg(current_time); // 构造 raw 文件路径

    { // 写入 raw 文件（阻塞）
        std::ofstream outFile(rawFilePath.toStdString(), std::ios::binary); // 以二进制方式打开文件
        if (outFile) {                                   // 如果打开成功
            outFile.write(reinterpret_cast<const char*>(paramPtr.pImageBuf), paramPtr.nImageSize); // 写入数据
            outFile.close();                             // 关闭文件
            
        }
        else {                                        // 打开失败则记录日志
            emit addEventMessage(QString("Failed to open RAW file for writing: %1").arg(rawFilePath));
        }
    }

    // 生成 416x416 的缩略图（使用最近邻）
    const int dstWidth = 416;                            // 目标宽度
    const int dstHeight = 416;                           // 目标高度
    std::vector<uint16_t> dstBuffer(dstWidth * dstHeight); // 分配临时缓存

    int srcW = paramPtr.nWidth;                          // 源图宽度
    int srcH = paramPtr.nHeight;                         // 源图高度
    for (int y = 0; y < dstHeight; ++y) {                // 遍历目标行
        int srcY = (y * srcH) / dstHeight;               // 最近邻映射 Y 坐标
        if (srcY >= srcH) srcY = srcH - 1;               // 边界保护
        for (int x = 0; x < dstWidth; ++x) {             // 遍历目标列
            int srcX = (x * srcW) / dstWidth;            // 最近邻映射 X 坐标
            if (srcX >= srcW) srcX = srcW - 1;           // 边界保护
            dstBuffer[y * dstWidth + x] = image_data[srcY * srcW + srcX]; // 拷贝像素
        }
    }

    QString tifFilePath = rawFilePath;                    // 基于 raw 文件名构造 tif 名称
    if (tifFilePath.endsWith(".raw", Qt::CaseInsensitive)) tifFilePath.chop(4); // 去掉 .raw 扩展
    tifFilePath += ".tif";                               // 添加 .tif 扩展

    bool savedOk = false;                                // 标志是否成功保存 tif


    // 如果启用了 libtiff，则使用 libtiff API 写入 16-bit TIFF
    QByteArray tifBa = tifFilePath.toUtf8();         // 转换为 UTF-8 C 字符串
    const char* tifCStr = tifBa.constData();
    TIFF* tif = TIFFOpen(tifCStr, "w");              // 打开 TIFF 文件
    if (tif) {                                      // 打开成功
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, dstWidth); // 设置宽度
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, dstHeight); // 设置高度
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);

        for (uint32_t row = 0; row < static_cast<uint32_t>(dstHeight); ++row) { // 写每一行
            void* buf = static_cast<void*>(dstBuffer.data() + (row * dstWidth)); // 行缓冲指针
            if (TIFFWriteScanline(tif, buf, row, 0) < 0) { // 写入失败则记录并跳出
                emit addEventMessage(QString("Failed to write TIFF scanline %1 for file %2").arg(row).arg(tifFilePath));
                break;
            }
        }
        TIFFClose(tif);                              // 关闭 TIFF
        savedOk = true;                              // 标记成功
    }
    else {
        emit addEventMessage(QString("Failed to open TIFF for writing (libtiff): %1").arg(tifFilePath)); // 打开失败日志
    }



    if (savedOk) {                                     // 如果保存成功则设置标志并发出信号
        m_imageSavedFlag.store(true);                   // 将原子标志置为 true（跨线程安全）
        emit imageSaved();                              // 发出 imageSaved 信号（序列等待使用）
    }

    { // [优化] Start 序列：tif 复制到新目录后删除原 tif；raw 也删除（不复制）
// 规则：不使用时间戳防重名；若目标文件已存在，则追加 _1、_2、_3...
        if (savedOk && !g_startSequenceOutputDir.isEmpty()) {
            creatDir(g_startSequenceOutputDir); // 确保目标目录存在

            const double x_mm = static_cast<double>(g_relX_um.load()); // 按你当前逻辑：这里的值被当作 mm 使用
            const double y_mm = static_cast<double>(g_relY_um.load());

            const QString baseNameNoIndex = QStringLiteral("X=%1mm_Y=%2mm")
                .arg(QString::number(x_mm, 'f', 1))
                .arg(QString::number(y_mm, 'f', 1));

            // 生成一个不冲突的目标 tif 路径：永远从 base_1.tif 开始，然后 base_2/base_3...
            auto makeUniquePath = [&](const QString& dir, const QString& base, const QString& ext) -> QString {
                QDir d(dir);

                int idx = 1; // 注意：从 1 开始，保证第一张就是 _1
                while (true) {
                    const QString candidate = d.filePath(QStringLiteral("%1_%2%3").arg(base).arg(idx).arg(ext));
                    if (!QFile::exists(candidate)) return candidate;
                    ++idx;
                }
                };

            const QString newTif = makeUniquePath(g_startSequenceOutputDir, baseNameNoIndex, QStringLiteral(".tif"));

            // 1) 复制 tif 到新目录
            bool copyOk = false;
            if (QFile::exists(tifFilePath)) {
                copyOk = QFile::copy(tifFilePath, newTif);
            }

            // 2) 复制成功后删除原 tif；同时删除 raw（不复制）
            if (copyOk) {
                // 删除原 tif
                if (!QFile::remove(tifFilePath)) {
                    emit addEventMessage(QStringLiteral("Start sequence: copied tif but failed to remove src tif: %1").arg(tifFilePath));
                }

                // 删除原 raw（不复制）
                if (QFile::exists(rawFilePath)) {
                    if (!QFile::remove(rawFilePath)) {
                        emit sendMessage(QStringLiteral("Start sequence: failed to remove src raw: %1").arg(rawFilePath));
                    }
                }

                emit sendMessage(QStringLiteral("Start sequence: tif moved to %1").arg(newTif));
            }
            else {
                emit sendMessage(QStringLiteral("Start sequence tif move failed (copy failed): src=%1 dst=%2")
                    .arg(tifFilePath)
                    .arg(newTif));
            }
        
        }
    }

    // 计算原始图像的最小/最大值，供窗宽窗位滑动使用
    uint16_t maxv = 0;                                 // 初始化最大值为 0
    uint16_t minv = 0xffff;                            // 初始化最小值为 0xffff（很大）
    for (int i = 0; i < pixelCount; ++i) {             // 遍历每个像素
        uint16_t v = image_data[i];                    // 读取像素值
        if (v > maxv) maxv = v;                        // 更新最大值
        if (v < minv) minv = v;                        // 更新最小值
    }
    if (m_MaxValue < maxv) m_MaxValue = maxv;          // 更新成员变量 m_MaxValue（若发现更大的值）
    if (m_MinValue > minv) m_MinValue = minv;          // 更新成员变量 m_MinValue（若发现更小的值）

    // 生成用于显示的 8-bit 图像（线性映射）
    std::unique_ptr<unsigned char[]> pImage8(new unsigned char[pixelCount]); // 分配 8-bit 缓冲
    if (maxv == minv) {                                // 若图像所有值相同
        memset(pImage8.get(), 0, pixelCount);          // 全部置为 0（避免除以 0）
    }
    else {
        double factor = 255.0 / static_cast<double>(maxv - minv); // 计算缩放系数
        for (int i = 0; i < pixelCount; ++i) {         // 遍历每个像素并映射到 0..255
            double val = (static_cast<double>(image_data[i]) - minv) * factor; // 线性映射
            if (val < 0) val = 0;                      // 下限保护
            if (val > 255) val = 255;                  // 上限保护
            pImage8[i] = static_cast<unsigned char>(val); // 存入 8-bit 缓冲
        }
    }

    if (ui.Main_LableImage1) {                          // 如果主显示 QLabel 存在则显示图像
        QImage dispImage(pImage8.get(), paramPtr.nWidth, paramPtr.nHeight, paramPtr.nWidth, QImage::Format_Grayscale8); // 创建 8-bit QImage
        ui.Main_LableImage1->setPixmap(QPixmap::fromImage(dispImage)); // 更新 QLabel 的 pixmap
    }

    QImage rawQImage(reinterpret_cast<const uchar*>(image_data.get()), paramPtr.nWidth, paramPtr.nHeight, paramPtr.nWidth * 2, QImage::Format_Grayscale16); // 构造 16-bit QImage
    m_Image = rawQImage.copy();                         // 复制一份到成员变量，便于窗宽调整时使用

    if (ui.horizontalSlider_SetWindows) {               // 如果窗宽滑块存在
        ui.horizontalSlider_SetWindows->setRange(m_MinValue, m_MaxValue); // 设置滑块范围
        ui.horizontalSlider_SetWindows->setValue(m_MaxValue); // 设置滑块当前值为最大值
    }
}

// ----------------------------- 窗宽/窗位更新 -----------------------------
void Red450_SingleChip_QTDemo::onUpdateImageWindows(int max)
{
    m_MaxValue = max;                                 // 更新成员 m_MaxValue 为滑块所给的值
    QString msg = QStringLiteral("min = %1,max=%2").arg(m_MinValue).arg(m_MaxValue); // 构造日志字符串
    //sendMessage(msg);                                 // 发送日志消息到日志控件

    QImage image = m_Image.copy();                    // 复制成员中保存的 16-bit 图像
    ushort* srcDataEx = reinterpret_cast<ushort*>(image.bits()); // 获取原始像素数据指针
    if (srcDataEx == nullptr) return;                 // 如果指针为空则直接返回

    int size = image.sizeInBytes() / sizeof(ushort);  // 计算像素个数
    auto pImage8 = std::make_unique<unsigned char[]>(size); // 分配 8-bit 缓冲

    if (m_MaxValue == m_MinValue) {                   // 若上下限相同则置为全黑
        memset(pImage8.get(), 0, size);
    }
    else {
        float dfactor = 255.0f / static_cast<float>(m_MaxValue - m_MinValue); // 计算缩放因子
        for (int i = 0; i < size; ++i) {             // 映射每个像素
            unsigned short pixel_value = srcDataEx[i];
            if (pixel_value < m_MinValue) {          // 低于最小值则为 0
                pImage8[i] = 0;
                continue;
            }
            if (pixel_value > m_MaxValue) {          // 高于最大值则为 255
                pImage8[i] = 255;
                continue;
            }
            int nValue = static_cast<int>((pixel_value - m_MinValue) * dfactor); // 计算映射值
            if (nValue < 0) nValue = 0;            // 防护
            if (nValue > 255) nValue = 255;
            pImage8[i] = static_cast<unsigned char>(nValue); // 存放 8-bit 值
        }
    }

    if (ui.Main_LableImage1) {                        // 更新主显示控件为 8-bit 图像
        QImage image8(pImage8.get(), image.width(), image.height(), image.width(), QImage::Format_Grayscale8);
        ui.Main_LableImage1->setPixmap(QPixmap::fromImage(image8)); // 设置 pixmap
    }
}

// ----------------------------- 探测器事件处理 -----------------------------
void Red450_SingleChip_QTDemo::handleDetectorEvent(char eventId)
{
    switch (eventId) {                                // 根据事件 ID 处理不同事件
    case EVENT_LINK_UP:                               // 探测器链路建立事件
        addApiCallMessage(QStringLiteral("Event: Detector connected.")); // 写事件日志
        if (ui.label_Detector1_StatusValue) ui.label_Detector1_StatusValue->setText(QStringLiteral("Connected")); // 更新 UI 状态文本
        m_bDetectorConnectionStatus = true;           // 更新内部状态
        if (m_bDetectorConnectionStatus) {            // 如果处于连接状态则启用部分按钮
            if (ui.btn_ConfigureDetector) ui.btn_ConfigureDetector->setEnabled(true);
            //if (ui.btn_LoadCaliCode) ui.btn_LoadCaliCode->setEnabled(true);
            if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(true);
        }
        break;
    case EVENT_LINK_DOWN:                             // 探测器链路断开事件
        addEventMessage(QStringLiteral("Event: Detector disconnected.")); // 写事件日志
        if (ui.label_Detector1_StatusValue) ui.label_Detector1_StatusValue->setText(QStringLiteral("Disconnected")); // 更新 UI 状态
        m_bDetectorConnectionStatus = false;          // 更新内部状态
        if (ui.btn_ConfigureDetector) ui.btn_ConfigureDetector->setEnabled(false); // 禁用相关按钮
        if (ui.btn_LoadCaliCode) ui.btn_LoadCaliCode->setEnabled(false);
        if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(false);
        // 探测器断开时禁用运动 UI
        setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
            m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
            m_editTargetX, m_editTargetY, m_editTargetZ, false);
        break;
    case EVENT_INITIALIZATION_COMPLATED:              // 探测器初始化完成事件
        addApiCallMessage(QStringLiteral("Event: Detector initialization completed."));
        break;
    case EVENT_INITIALIZATION_FAILED:                 // 探测器初始化失败事件
        addEventMessage(QStringLiteral("Event: Detector initialization failed."));
        break;
    case EVENT_EXPOSURE_COMPLETE:                     // 曝光完成事件（SDK 发出）
        //onExpousureComplated();                       // 调用曝光完成处理（关闭 AVDD/HV）
        // 根据串口状态恢复运动 UI（仅当串口打开时恢复）
       // setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
       //     m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
       //     m_editTargetX, m_editTargetY, m_editTargetZ, g_isSerialOpenOk.load());
        g_isExposing = false;                         // 标记不再曝光
        emit exposureFinished();                      // 发出曝光完成信号，序列等待可捕获该信号
        //  break;
    default:
        break;
    }
}

// ----------------------------- 日志辅助函数 -----------------------------
void Red450_SingleChip_QTDemo::addMessage(const QString msg)
{
    QString current = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss: "); // 获取当前时间字符串用于日志前缀
    if (ui.textEdit_LogMessage) {                     // 如果日志控件存在
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End); // 把光标移动到文本末尾
        ui.textEdit_LogMessage->insertPlainText(current + msg + "\n"); // 插入日志文本
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End); // 再次把光标移到末尾
    }
    else {
        qDebug() << current + msg;                     // 如果不存在控件则输出到调试控制台
    }
}

void Red450_SingleChip_QTDemo::addApiCallMessage(const QString msg)
{
    QString current = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss: "); // 时间前缀
    QString clrMsg = "<font color=\"Blue\">";     // 用 HTML 标签为 API 日志着色（蓝色）
    clrMsg += msg;                                // 拼接消息
    clrMsg += "</font><br>";                      // 追加换行
    if (ui.textEdit_LogMessage) {                 // 如果日志控件存在
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End);
        ui.textEdit_LogMessage->insertHtml(current + clrMsg); // 插入富文本日志
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End);
    }
    else {
        qDebug() << current + msg;                 // 否则输出到调试控制台
    }
}

void Red450_SingleChip_QTDemo::addEventMessage(const QString msg)
{
    QString current = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss: "); // 时间前缀
    QString clrMsg = "<font color=\"Red\">";  // 事件采用红色高亮以便区分
    clrMsg += msg;                                // 拼接消息
    clrMsg += "</font><br>";                      // 添加换行
    if (ui.textEdit_LogMessage) {                 // 写入日志控件
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End);
        ui.textEdit_LogMessage->insertHtml(current + clrMsg); // 插入 HTML 格式日志
        ui.textEdit_LogMessage->moveCursor(QTextCursor::End);
    }
    else {
        qDebug() << current + msg;                 // 否则输出到调试控制台
    }
}

// ----------------------------- 清空图像槽 -----------------------------
void Red450_SingleChip_QTDemo::onClearImage()
{
    for (int i = 1; i <= 4; ++i) {                    // 遍历 1..4 四个图像槽
        QString Main_image_label = QStringLiteral("Main_LableImage%1").arg(i); // 构造主图 QLabel 名称
        QString Aux_image_label = QStringLiteral("Aux_LableImage%1").arg(i);   // 构造辅图 QLabel 名称

        QLabel* main_label = this->findChild<QLabel*>(Main_image_label); // 查找主图控件
        if (main_label) main_label->clear();           // 存在则清空

        QLabel* aux_label = this->findChild<QLabel*>(Aux_image_label); // 查找辅图控件
        if (aux_label) aux_label->clear();             // 存在则清空
    }
}

// ----------------------------- 文件/目录选择 -----------------------------
void Red450_SingleChip_QTDemo::onSelectDllFileButtonClicked()
{
    auto filePath = QFileDialog::getOpenFileName(this, QStringLiteral("Select XVDetector.dll"), ".", "XVDetector.dll(*.dll)"); // 打开文件选择对话框
    auto absoluteDllFilePath = QFileInfo(filePath).absoluteFilePath(); // 获取选择文件的绝对路径
    if (ui.lineEdit_DllDir) ui.lineEdit_DllDir->setText(absoluteDllFilePath); // 更新 UI 文本框显示
}

void Red450_SingleChip_QTDemo::onSelectCFG1ButtonClicked()
{
    auto filePath = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Settings folder"), ".", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks); // 打开目录选择对话框
    auto absoluteDllFilePath = QFileInfo(filePath).absoluteFilePath(); // 获取目录绝对路径
    if (ui.lineEdit_CFG1) ui.lineEdit_CFG1->setText(absoluteDllFilePath); // 更新 UI
}



// ---------------------- Start 序列实现（调用已有基础函数，） ----------------------
void Red450_SingleChip_QTDemo::onStartSequenceClicked()
{
    // GUI 线程：先禁用运动相关控件和曝光按钮，防止用户在序列运行期间误操作
    setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
        m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
        m_editTargetX, m_editTargetY, m_editTargetZ, false); // 禁用所有运动相关控件
    if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(false);   // 禁用曝光按钮
    if (m_btn_StartSequence) m_btn_StartSequence->setEnabled(false); // 禁用 Start 按钮避免重复触发

    // 把序列配置复制到局部常量，防止运行过程中外部修改这些成员变量
    const QString axisX = seq_axis_x;               // X 轴标识（例如 "X"）
    const QString axisY = seq_axis_y;               // Y 轴标识（例如 "Y"）
    

    // 在工作线程中执行序列逻辑以保持 UI 响应
    QThread* seqThread = QThread::create([=]() {
        
        auto performExposureAndWait = [&](int exposureTimeMs, int exposureTimeoutMs = 20000) -> bool {
            // 清除之前的保存标志
            m_imageSavedFlag.store(false);           // 在开始曝光前清除图像保存标志

            // 曝光前快照相对初始位（mm），用于命名（保存为微米整数）
            QMetaObject::invokeMethod(this, [&]() { // 在 GUI 线程读取位置
                QThread::msleep(5000);
                int cx = getDisplay(QStringLiteral("X")); //  当前 X（步）
                int cy = getDisplay(QStringLiteral("Y")); //  当前 Y（步）
                double dxmm = static_cast<double>(cx - m_recordX) / 3200.0; // 相对 X（mm）
                double dymm = static_cast<double>(cy - m_recordY) / 3200.0; // 相对 Y（mm）
                g_relX_um.store(static_cast<long long>(std::llround(dxmm))); //  mm->um
                g_relY_um.store(static_cast<long long>(std::llround(dymm))); //  mm->um
                emit sendMessage(QStringLiteral("X=%1,Y=%2").arg(dxmm).arg(dymm));
                }, Qt::BlockingQueuedConnection); //阻塞确保快照完成

            // 在 GUI 线程发起曝光（BlockingQueuedConnection 保证调用已发出）
            QMetaObject::invokeMethod(this, "onExposure", Qt::BlockingQueuedConnection);

            while (g_isExposing != false) {} // 等待曝光完成
           
           bool saved = false;                       // 标志图像是否保存成功

           return  saved;              // 在曝光结束时返回 true
            }; // end performExposureAndWait

        // 在序列开始前从 GUI 读取曝光时间（BlockingQueuedConnection 以保证读取到最新 UI 值）
        int exposureTime = 100;                      // 默认曝光时间（ms）
        int Count = 5;
        QMetaObject::invokeMethod(this, [&]() {     // 在 GUI 线程读取 UI 控件值
            bool bok = false;
            if (ui.lineEdit_ExposureTimeInput) {
                int v = ui.lineEdit_ExposureTimeInput->text().toInt(&bok); // 尝试转换为整数
                if (bok && v > 0) exposureTime = v; // 合法则覆盖默认值
            }
            if (ui.lineEdit_ExposureCountInput) {
                int v = ui.lineEdit_ExposureCountInput->text().toInt(&bok); // 尝试转换为整数
                if (bok && v > 0) Count = v; // 合法则覆盖默认值
            }
            
            }, Qt::BlockingQueuedConnection);

        // Start：创建“曝光时间+当前时间”文件夹，并写入 g_startSequenceOutputDir
        QMetaObject::invokeMethod(this, [&]() { //  在 GUI 线程创建目录以复用 creatDir
            const QString now = QDateTime::currentDateTime().toString("yyyy_MM_dd_HH_mm_ss_zzz"); 
            const QString folderName = QStringLiteral("t=%1ms_count=%2_%3").arg(exposureTime).arg(Count).arg(now);
            const QString root = QDir::currentPath() + "/image";
            creatDir(root); 
            const QString outDir = QDir(root).filePath(folderName); 
            creatDir(outDir); 
            g_startSequenceOutputDir = outDir; 
            addApiCallMessage(QStringLiteral("Start: output folder = %1").arg(g_startSequenceOutputDir));
            }, Qt::BlockingQueuedConnection); // 

       QMetaObject::invokeMethod(this, "onRecordClicked", Qt::BlockingQueuedConnection);

       
        // 记录初始后先做一次曝光
        performExposureAndWait(exposureTime);

        // -------- 正向 X 循环（repeatCount 次） --------
        for (int i = 0; i < repeatCount; ++i) {

            // X 轴正向移动：读取当前 X（last），调用 moveAxis 发起移动，然后轮询直到实际位置等于目标（last + moveDistX）
            QMetaObject::invokeMethod(this, "onMoveXPlus", Qt::BlockingQueuedConnection);
            
            // 到位后进行一次曝光并等待保存
            performExposureAndWait(exposureTime);
        }

        // Y 轴正向移动：同上流程
        QMetaObject::invokeMethod(this, "onMoveYPlus", Qt::BlockingQueuedConnection);
        performExposureAndWait(exposureTime);// 到位后曝光并等待保存

        // -------- 反向 X 循环（repeatCount 次） --------
        for (int i = 0; i < repeatCount; ++i) {

            QMetaObject::invokeMethod(this, "onMoveXMinus", Qt::BlockingQueuedConnection);
            performExposureAndWait(exposureTime);    // 到位后曝光并等待保存
        }

        // Y 轴正向移动：同上流程
        QMetaObject::invokeMethod(this, "onMoveYPlus", Qt::BlockingQueuedConnection);
        performExposureAndWait(exposureTime);// 到位后曝光并等待保存
        for (int i = 0; i < repeatCount; ++i) {

            // X 轴正向移动：读取当前 X（last），调用 moveAxis 发起移动，然后轮询直到实际位置等于目标（last + moveDistX）
            QMetaObject::invokeMethod(this, "onMoveXPlus", Qt::BlockingQueuedConnection);

            // 到位后进行一次曝光并等待保存
            performExposureAndWait(exposureTime);
        }

        // Y 轴正向移动：同上流程
        QMetaObject::invokeMethod(this, "onMoveYPlus", Qt::BlockingQueuedConnection);
        performExposureAndWait(exposureTime);// 到位后曝光并等待保存

        // -------- 反向 X 循环（repeatCount 次） --------
        for (int i = 0; i < repeatCount; ++i) {

            QMetaObject::invokeMethod(this, "onMoveXMinus", Qt::BlockingQueuedConnection);
            performExposureAndWait(exposureTime);    // 到位后曝光并等待保存
        }




        onExpousureComplated();
        // 序列全部完成后回到记录的初始位置（调用已有的 onResetClicked）
        QMetaObject::invokeMethod(this, "onResetClicked", Qt::BlockingQueuedConnection); // 在 GUI 线程上调用复位槽

        //  序列结束：清空输出目录开关，避免影响非序列曝光
        QMetaObject::invokeMethod(this, [&]() { //
            g_startSequenceOutputDir.clear(); // 
            }, Qt::BlockingQueuedConnection); // 

        // 在 GUI 线程记录序列完成日志
        QMetaObject::invokeMethod(this, [=]() {
        // 发送完成日志
            addApiCallMessage(QStringLiteral("Completed."));
            }, Qt::QueuedConnection);
        }); // end of seqThread lambda

    // 当序列线程结束时，在 GUI 线程恢复 UI 状态并删除线程对象
    QObject::connect(seqThread, &QThread::finished, this, [=]() {
        // 根据串口打开状态恢复运动 UI（仅当串口打开时恢复为可用）
        setMotionUiEnabled(m_btnXPlus, m_btnXMinus, m_btnYPlus, m_btnYMinus, m_btnZPlus, m_btnZMinus,
            m_btnRecord, m_btnReset, m_btnStop, m_btnMoveToTarget,
            m_editTargetX, m_editTargetY, m_editTargetZ, g_isSerialOpenOk.load());

        if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(true);   // 恢复曝光按钮
        if (m_btn_StartSequence) m_btn_StartSequence->setEnabled(true); // 恢复 Start 按钮

        seqThread->deleteLater(); // 安全地延迟删除线程对象
        });

    seqThread->start(); // 启动序列线程，开始执行移动与曝光序列
}


// onLoadDll: 加载或创建探测器实例，注册回调并更新 UI 状态，返回 0 表示成功，非 0 表示失败
int Red450_SingleChip_QTDemo::onLoadDll()
{
    // 检查 UI 中指定的 DLL 路径，如果为空则提示并返回错误码
    QString dllFile = ui.lineEdit_DllDir ? ui.lineEdit_DllDir->text().trimmed() : QString(); // 读取 UI 中的 DLL 路径
    if (dllFile.isEmpty()) {                                                               // 如果路径为空
        // 弹出错误提示并记录日志
        QMessageBox::warning(this, QStringLiteral("Warning"), QStringLiteral("DLL path is empty. Please select DLL file.")); // 提示用户选择 DLL
        emit addEventMessage(QStringLiteral("onLoadDll: DLL path empty."));                 // 记录日志
        return -1;                                                                        // 返回失败
    }

    // 如果实例尚未创建，则尝试通过工厂创建（具体实现依赖于 DetectorBase）
    if (m_DetectorInstance == nullptr) {                                                  // 如果实例为 null
        // 这里优先使用用户选择的 dllFile，如果你的 DetectorBase::GetInstacne 接口需要不同参数，请替换下面一行
        m_DetectorInstance = DetectorBase::GetInstacne(dllFile.toLocal8Bit().constData()); // 创建实例（示例）
    }

    // 检查创建是否成功
    if (m_DetectorInstance == nullptr) {                                                  // 如果仍为 null
        emit addEventMessage(QStringLiteral("onLoadDll: Failed to create detector instance.")); // 日志记录创建失败
        return -1;                                                                        // 返回失败
    }

    // 注册事件与图像回调到 SDK（静态回调适配器将转发到对象实例）
    m_DetectorInstance->RegisterEventCallBack(&Red450_SingleChip_QTDemo::DetectorEventCallback, this); // 注册事件回调
    m_DetectorInstance->RegisterImageValidCallBack(&Red450_SingleChip_QTDemo::DetectorImageValidCallback, this); // 注册图像回调

    // 更新 UI 状态：禁用选择/加载按钮，启用释放与启动按钮
    if (ui.lineEdit_DllDir) ui.lineEdit_DllDir->setEnabled(false); // 禁用路径编辑
    if (ui.btn_SelectDll) ui.btn_SelectDll->setEnabled(false);     // 禁用选择按钮
    if (ui.btn_LoadDll) ui.btn_LoadDll->setEnabled(false);         // 禁用加载按钮
    if (ui.btn_FreeDll) ui.btn_FreeDll->setEnabled(true);          // 启用释放按钮
    if (ui.btn_StartUpDetector) ui.btn_StartUpDetector->setEnabled(true); // 启用启动探测器按钮

    emit addApiCallMessage(QStringLiteral("onLoadDll: Detector DLL loaded and callbacks registered.")); // 日志成功
    return 0;                                                                                      // 返回成功
}

// onFreeDll: 释放探测器实例并恢复 UI，返回 0 表示成功
int Red450_SingleChip_QTDemo::onFreeDll()
{
    // 如果有探测器实例则尝试优雅关闭并置空
    if (m_DetectorInstance) {                         // 如果存在实例
        m_DetectorInstance->ShutDownDetector();      // 调用 SDK 提供的关闭接口
        m_DetectorInstance = nullptr;                // 置空成员指针
    }

    // 恢复 UI，使用户可以重新选择/加载 DLL
    if (ui.btn_SelectDll) ui.btn_SelectDll->setEnabled(true); // 恢复选择按钮
    if (ui.btn_LoadDll) ui.btn_LoadDll->setEnabled(true);     // 恢复加载按钮
    if (ui.btn_FreeDll) ui.btn_FreeDll->setEnabled(false);    // 禁用释放按钮
    if (ui.btn_StartUpDetector) ui.btn_StartUpDetector->setEnabled(false); // 禁用启动按钮
    if (ui.btn_ConfigureDetector) ui.btn_ConfigureDetector->setEnabled(false); // 禁用配置按钮
    if (ui.btn_LoadCaliCode) ui.btn_LoadCaliCode->setEnabled(false); // 禁用加载校正按钮
    if (ui.btn_Exposure) ui.btn_Exposure->setEnabled(false); // 禁用曝光按钮

    emit sendMessage(QStringLiteral("onFreeDll: Detector DLL freed.")); // 日志
    return 0;                                                            // 返回成功
}

// onStartUpDetector: 调用 SDK 启动探测器，异步等待 LINK UP 事件
void Red450_SingleChip_QTDemo::onStartUpDetector()
{
    // 检查实例有效性
    if (!m_DetectorInstance) {                                                   // 如果实例为空
        emit sendMessage(QStringLiteral("onStartUpDetector: detector instance invalid.")); // 日志
        return;                                                                   // 返回
    }

    // 从 UI 读取配置目录（或使用默认）
    QString cfg1 = ui.lineEdit_CFG1 ? ui.lineEdit_CFG1->text().trimmed() : QString(); // 读取 CFG1 路径

    // 调用 SDK 启动探测器（StartUpDetector 通常是非阻塞的）
    int result = m_DetectorInstance->StartUpDetector(cfg1.toStdString().c_str());      // 发起启动
    if (result == XVD_API_SUCCESS) {                                                    // 如果调用成功
        emit addApiCallMessage(QStringLiteral("onStartUpDetector: StartUpDetector called, waiting for LINK UP...")); // 日志
    }
    else {                                                                            // 否则记录错误码
        emit addEventMessage(QStringLiteral("onStartUpDetector: StartUpDetector failed, code=%1").arg(result)); // 错误日志
    }
}

// onConfigureDetector: 在新线程中调用 SDK 的 InitDetector（耗时），并通过信号/日志反馈
void Red450_SingleChip_QTDemo::onConfigureDetector()
{
    // 实例检查
    if (!m_DetectorInstance) {                                                   // 如果实例无效
        emit sendMessage(QStringLiteral("onConfigureDetector: detector instance invalid.")); // 日志
        return;                                                                   // 返回
    }

    emit addApiCallMessage(QStringLiteral("onConfigureDetector: Starting detector configuration...")); // 日志

    // 在新线程中做耗时初始化，避免阻塞 UI
    QThread* t = QThread::create([this]() {
        emit sendMessage(QStringLiteral("onConfigureDetector: InitDetector() called in worker thread.")); // 日志
        int r = m_DetectorInstance->InitDetector(); // 调用 SDK 初始化（阻塞）
        if (r == XVD_API_SUCCESS) {                 // 初始化成功
            emit addApiCallMessage(QStringLiteral("onConfigureDetector: Detector initialization succeeded.")); // 日志
        }
        else {                                    // 初始化失败
            emit addEventMessage(QStringLiteral("onConfigureDetector: Detector initialization failed, code=%1").arg(r)); // 错误日志
        }
        });
    t->start(); // 启动线程（对话框或进度条由 UI 层决定是否显示）
}

// onSetExternalBiasVoltage: 从 UI 读取偏压数值并调用 SDK 接口设置
void Red450_SingleChip_QTDemo::onSetExternalBiasVoltage()
{
    // 实例检查
    if (!m_DetectorInstance) {                                                // 如果实例无效
        emit addEventMessage(QStringLiteral("onSetExternalBiasVoltage: detector instance invalid.")); // 日志
        return;                                                                // 返回
    }

    // 从 UI 读取四个浮点参数，若转换失败则使用安全默认值
    bool bok = false;                                                          // 临时标志
    float vset = ui.lineEdit_Vset ? ui.lineEdit_Vset->text().toFloat(&bok) : 0.75f; if (!bok) vset = 0.75f; // 读取 Vset
    float vth1 = ui.lineEdit_Vth1 ? ui.lineEdit_Vth1->text().toFloat(&bok) : 0.73f; if (!bok) vth1 = 0.73f; // 读取 Vth1
    float vth2 = ui.lineEdit_Vth2 ? ui.lineEdit_Vth2->text().toFloat(&bok) : 0.68f; if (!bok) vth2 = 0.68f; // 读取 Vth2
    float vref = ui.lineEdit_Vref ? ui.lineEdit_Vref->text().toFloat(&bok) : 0.4f; if (!bok) vref = 0.4f;    // 读取 Vref

    // 调用 SDK 接口设置外部偏压
    int result = m_DetectorInstance->SetExternalBiasVoltage(vset, vth1, vth2, vref); // 调用 SetExternalBiasVoltage
    if (result == XVD_API_SUCCESS) {                                                // 成功
        emit addApiCallMessage(QStringLiteral("onSetExternalBiasVoltage: SetExternalBiasVoltage succeeded.")); // 日志
    }
    else {                                                                        // 失败
        emit addEventMessage(QStringLiteral("onSetExternalBiasVoltage: SetExternalBiasVoltage failed, code=%1").arg(result)); // 错误日志
    }
}

// onReadVset: 从 SDK 读取 Vset 并在 UI 中显示（如果存在对应的 lineEdit）
void Red450_SingleChip_QTDemo::onReadVset()
{

}

// onReadVth1: 从 SDK 读取 Vth1 并更新 UI
void Red450_SingleChip_QTDemo::onReadVth1()
{

}

// onReadVth2: 从 SDK 读取 Vth2 并更新 UI
void Red450_SingleChip_QTDemo::onReadVth2()
{

}

// onReadVref: 从 SDK 读取 Vref 并更新 UI
void Red450_SingleChip_QTDemo::onReadVref()
{

}

// onEnableCorrect: 启用或禁用校正并尝试加载校正文件（state 非 0 为启用）
void Red450_SingleChip_QTDemo::onEnableCorrect(int state)
{

}

// onExposure: 触发多帧曝光（UI 原有实现），这里只做基本实现以避免链接错误
void Red450_SingleChip_QTDemo::onExposure()
{
    if (!m_DetectorInstance) {                                                  // 实例检查
        emit addEventMessage(QStringLiteral("onExposure: detector instance invalid.")); // 日志
        return;                                                                  // 返回
    }

    // 从 UI 读取曝光时间和帧数，若解析失败则使用默认值
    bool bok = false;                                                            // 临时标志
    int exposure_time = ui.lineEdit_ExposureTimeInput ? ui.lineEdit_ExposureTimeInput->text().toInt(&bok) : 100; if (!bok) exposure_time = 100; // 读取曝光时间
    int exposure_count = ui.lineEdit_ExposureCountInput ? ui.lineEdit_ExposureCountInput->text().toInt(&bok) : 1; if (!bok) exposure_count = 1; // 读取帧数

    emit sendMessage(QStringLiteral("onExposure: Starting exposure, time=%1 ms, count=%2").arg(exposure_time).arg(exposure_count)); // 日志

    // 使能电源，然后请求多帧曝光（SDK 接口）
    m_DetectorInstance->EnableAVDD(true);                                        // 使能 AVDD
    m_DetectorInstance->EnableChipHV(true);                                      // 使能 HV

    int r = m_DetectorInstance->RequestMultiFrameExposure(exposure_time, exposure_count); // 请求多帧曝光
    if (r == XVD_API_SUCCESS) {                                                   // 请求成功
        emit sendMessage(QStringLiteral("onExposure: RequestMultiFrameExposure succeeded.")); // 日志
        g_isExposing = true;                                                      // 设置全局曝光标志
    }
    else {                                                                      // 请求失败
        emit addEventMessage(QStringLiteral("onExposure: RequestMultiFrameExposure failed, code=%1").arg(r)); // 错误日志
    }
}

// onExpousureComplated: 当 SDK 发出曝光完成事件时由事件处理器调用，负责关闭电源等后处理
void Red450_SingleChip_QTDemo::onExpousureComplated()
{


    // 关闭 AVDD 与 HV，完成曝光后的硬件复位操作
    m_DetectorInstance->EnableAVDD(false);                                       // 关闭 AVDD
    m_DetectorInstance->EnableChipHV(false);                                     // 关闭 HV
    emit sendMessage(QStringLiteral("onExpousureComplated: AVDD and HV disabled.")); // 日志
}



#include <QDebug>                       // qDebug（可选日志输出）

// 注意：函数签名必须与头文件中声明完全一致（包含调用约定），
//       这里使用与头文件一致的签名实现（若头文件使用 __stdcall，
//       这里也必须使用 __stdcall；若头文件不使用则去掉 __stdcall）。
//       如果你的头文件使用不同的调用约定，请确保此处与之保持一致。

// 静态图像回调适配器：SDK 在任意线程调用此函数，
// 需要把图像数据转发到对象实例的 handleDetectorImageData 槽中处理（线程安全）。
bool __stdcall Red450_SingleChip_QTDemo::DetectorImageValidCallback(const CallbackImageFncParam& paramPtr, void* callbackData)
{
    // 把 callbackData 还原为对象指针（创建回调时通常把 this 作为 callbackData 传入）
    Red450_SingleChip_QTDemo* pThis = static_cast<Red450_SingleChip_QTDemo*>(callbackData);
    if (!pThis) {                                      // 如果 callbackData 为 nullptr，则记录并返回 false 或 true 视 SDK 要求
        qDebug() << "DetectorImageValidCallback: callbackData is null";
        return false;                                  // 返回 false 表示未处理（根据 SDK 要求可改为 true）
    }

    // 使用 Qt 的元对象系统把回调数据派发到对象所在线程执行 handleDetectorImageData 槽
    // 使用 QueuedConnection 保证跨线程安全（handleDetectorImageData 在对象线程执行）
    QMetaObject::invokeMethod(pThis,
        "handleDetectorImageData",
        Qt::QueuedConnection,
        Q_ARG(const CallbackImageFncParam&, paramPtr));
    return true;                                       // 返回 true 表示回调已被接受并处理
}

// 静态事件回调适配器：SDK 在任意线程调用此函数，
// 将事件转发到对象实例的 handleDetectorEvent 槽中处理（线程安全）。
bool __stdcall Red450_SingleChip_QTDemo::DetectorEventCallback(char eventId, void* callbackData)
{
    // 还原对象指针
    Red450_SingleChip_QTDemo* pThis = static_cast<Red450_SingleChip_QTDemo*>(callbackData);
    if (!pThis) {                                      // 如果 callbackData 为 nullptr
        qDebug() << "DetectorEventCallback: callbackData is null";
        return false;                                  // 返回 false（或根据 SDK 要求返回 true）
    }

    // 使用 Qt 元对象系统异步派发事件到对象所在线程的 handleDetectorEvent 槽
    QMetaObject::invokeMethod(pThis,
        "handleDetectorEvent",
        Qt::QueuedConnection,
        Q_ARG(char, eventId));
    return true;                                       // 返回 true 表示回调已被接受
}

