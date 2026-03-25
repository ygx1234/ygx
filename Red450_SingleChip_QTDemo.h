#pragma once                                // 防止头文件被重复包含，保证只被包含一次（头文件保护）
// 本头文件声明主窗口类 Red450_SingleChip_QTDemo 的所有接口，包含 UI 成员、槽与信号等
// 请以 UTF-8 保存此文件，且编译选项开启 /utf-8（如果使用 MSVC）

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")    // 在 MSVC 下指定源文件执行字符集为 UTF-8，便于中文注释
#endif

#include <QtWidgets/QMainWindow>             // 包含 QMainWindow（主窗口类）
#include <QLibrary>                          // 包含 QLibrary（可选用于动态加载 DLL）
#include <QImage>                            // 包含 QImage（图像缓冲）
#include <QThread>                           // 包含 QThread（线程）
#include <QTimer>                            // 包含 QTimer（定时器）
#include <QSplitter>                         // 包含 QSplitter（布局）
#include <QString>                           // 包含 QString（字符串）
#include <atomic>                            // 包含 std::atomic（跨线程原子变量）

#include "ui_Red450_SingleChip_QTDemo.h"      // 包含由 .ui 生成的 ui 头文件，请确保生成并存在

// SDK 相关头文件（请根据工程实际路径修改 include 路径）
#include "include/APIErrorCode.h"            // SDK 返回码定义
#include "include/XVDInterface.h"           // SDK 接口与 CallbackImageFncParam 定义
#include "DetectorBase.h"                   // DetectorBase 抽象类与工厂


#define  USE_LIBTIFF  1
// 前向声明，减少头部依赖，提速编译
class SerialController;                     // 串口控制器前向声明
class QPushButton;                          // QPushButton 前向声明
class QLineEdit;                            // QLineEdit 前向声明
class QLabel;                               // QLabel 前向声明
class QGroupBox;                            // QGroupBox 前向声明

// 主窗口类：整合探测器控制与运动控制
class Red450_SingleChip_QTDemo : public QMainWindow
{
    Q_OBJECT                                // Qt 宏：启用信号与槽机制

public:
    explicit Red450_SingleChip_QTDemo(QWidget* parent = nullptr); // 构造函数，接受可选父对象
    ~Red450_SingleChip_QTDemo() override;   // 析构函数，虚函数重写

    // SDK 静态回调适配器（签名必须与 SDK 匹配）
    static bool __stdcall DetectorImageValidCallback(const CallbackImageFncParam& paramPtr, void* callbackData); // 图像回调
    static bool __stdcall DetectorEventCallback(char eventId, void* callbackData); // 事件回调

signals:
    void processFinished();                 // 配置/初始化完成信号（用于进度对话框）
    void sendMessage(QString msg);          // 线程安全的日志转发信号
    void exposureFinished();                // 曝光完成信号（序列等待使用）
    void imageSaved();                      // 图像保存完成信号（handleDetectorImageData 保存 tif 时发出）

public slots:
    // 探测器相关槽函数
    void handleDetectorEvent(char eventId);  // 处理探测器事件（连接/断开/曝光完成等）
    void handleDetectorImageData(const CallbackImageFncParam& paramPtr); // 处理图像回调并保存文件
    void onUpdateImageWindows(int max);      // 窗宽/窗位滑块变动处理

    // 串口相关槽
    void onSerialOpened(bool ok);            // 串口打开回调
    void onSerialPositionsReady(int x, int y, int z); // 串口报告位置
    void onSerialMoveCompleted(const QString& axis); // 串口报告移动完成
    void onSerialError(const QString& msg); // 串口错误回调

    // 运动 UI 槽函数
    void onRecordClicked();                  // 记录当前位为初始位
    void onResetClicked();                   // 回到记录的初始位
    void onMoveXPlus();                      // X+ 按钮槽
    void onMoveXMinus();                     // X- 按钮槽
    void onMoveYPlus();                      // Y+ 按钮槽
    void onMoveYMinus();                     // Y- 按钮槽
    void onMoveZPlus();                      // Z+ 按钮槽
    void onMoveZMinus();                     // Z- 按钮槽
    void onStopClicked();                    // 停止当前运动
    void onMoveToTargetClicked();            // 移动到目标位
    void pollPositionsTimeout();             // 轮询位置超时槽（定时器触发）

    // 文件/目录选择
    void onSelectDllFileButtonClicked();     // 选择 DLL 文件
    void onSelectCFG1ButtonClicked();        // 选择 CFG 文件夹

    // 探测器控制接口
    int onLoadDll();                         // 加载 DLL 并创建探测器实例
    int onFreeDll();                         // 释放探测器实例
    void onStartUpDetector();                // 启动探测器
    void onConfigureDetector();              // 配置探测器（可能耗时）
    void onSetExternalBiasVoltage();         // 设置外部偏压
    void onReadVset();                       // 读取 Vset
    void onReadVth1();                       // 读取 Vth1
    void onReadVth2();                       // 读取 Vth2
    void onReadVref();                       // 读取 Vref
    void onEnableCorrect(int state);         // 启用/禁用校正
    void onExposure();                       // 多帧曝光（UI 原始实现）
    void onExpousureComplated();             // 曝光完成处理（关闭电源）

    // 序列控制槽
    void onStartSequenceClicked();           // 启动移动+曝光序列（复用 moveAxis/performExposure）

    // 可通过 QMetaObject::invokeMethod 调用的单帧曝光接口（序列线程使用）
   // Q_INVOKABLE void performExposure(int exposureTimeMs); // 发起单帧曝光（count = 1）

private:
    Ui::Red450_SingleChip_QTDemo ui;         // uic 生成的 UI 对象（包含 .ui 中的控件）

    // 探测器实例与状态
    bool m_bDetectorConnectionStatus = false;// 探测器连接状态
    DetectorBase* m_DetectorInstance = nullptr; // 探测器实例指针
    QLibrary m_DllLibrary;                   // QLibrary（备用）

    // 图像显示与窗宽窗位
    unsigned short m_MaxValue = 0xffff;     // 窗宽最大值
    unsigned short m_MinValue = 0;          // 窗宽最小值
    QImage m_Image;                          // 保存 16-bit 原图用于窗宽调整

    // 串口控制器
    SerialController* m_serialController = nullptr; // 串口控制器实例
    QThread* m_serialThread = nullptr;         // 串口线程指针

    // 主窗口与运动面板
    QSplitter* m_mainSplitter = nullptr;      // 主分割器
    QWidget* m_motionPanel = nullptr;         // 运动面板

    // 运动 UI 控件
    QPushButton* m_btnXPlus = nullptr;        // X+ 按钮
    QPushButton* m_btnXMinus = nullptr;       // X- 按钮
    QPushButton* m_btnYPlus = nullptr;        // Y+ 按钮
    QPushButton* m_btnYMinus = nullptr;       // Y- 按钮
    QPushButton* m_btnZPlus = nullptr;        // Z+ 按钮
    QPushButton* m_btnZMinus = nullptr;       // Z- 按钮

    // 位置显示
    QGroupBox* m_initGroup = nullptr;         // 初始位分组
    QLabel* m_initPosLabel = nullptr;         // 初始位标签
    QGroupBox* m_currentGroup = nullptr;      // 当前位分组
    QLabel* m_currentPosLabel = nullptr;      // 当前位标签
    QGroupBox* m_relativeGroup = nullptr;     // 相对位分组（mm）
    QLabel* m_relativePosLabel = nullptr;     // 相对位标签

    // 控制按钮
    QPushButton* m_btnRecord = nullptr;       // 记录初始
    QPushButton* m_btnReset = nullptr;        // 复位
    QPushButton* m_btnStop = nullptr;         // 停止
    QPushButton* m_btn_StartSequence = nullptr; // Start 序列按钮

    // 目标输入
    QLineEdit* m_editTargetX = nullptr;       // 目标 X
    QLineEdit* m_editTargetY = nullptr;       // 目标 Y
    QLineEdit* m_editTargetZ = nullptr;       // 目标 Z
    QPushButton* m_btnMoveToTarget = nullptr; // 移动到目标

    // 位置轮询
    QTimer* m_pollTimer = nullptr;            // 轮询定时器

    // 初始位置记录（步）
    int m_recordX = 0;                        // 记录 X（步）
    int m_recordY = 0;                        // 记录 Y（步）
    int m_recordZ = 0;                        // 记录 Z（步）
    bool m_hasInitial = false;                // 是否已记录初始

    // 当前位置缓存（用于实时更新 Relative）
    int m_currentX = 0;                       // 当前 X 缓存
    int m_currentY = 0;                       // 当前 Y 缓存
    int m_currentZ = 0;                       // 当前 Z 缓存

    // 序列参数
    QString seq_axis_x = QStringLiteral("X"); // X 轴名
    QString seq_axis_y = QStringLiteral("Y"); // Y 轴名
    int seq_move_dist_x = 3200;               // X 单次移动步数
    int seq_move_dist_y = 3200;               // Y 单次移动步数
    int seq_repeat_count = 5;                 // 重复次数

    // 图像保存完成标志（跨线程安全）
    std::atomic<bool> m_imageSavedFlag{ false }; // 图像保存完成标志

private:
    // 私有辅助函数（实现见 cpp）
    void buildMotionPanel();                  // 构建运动面板
    void integrateMotionPanelIntoWindow();    // 集成到主窗口
    void startPolling();                      // 启动轮询
    void stopPolling();                       // 停止轮询
    int getDisplay(const QString& axis) const;// 线程安全读取显示值
    void moveAxis(const QString& axis, int moveDist, int lastPos, int waitSec); // 发起移动
    void addMessage(const QString msg);       // 日志写入（线程安全 via sendMessage）
    void addApiCallMessage(const QString msg);// API 调用日志
    void addEventMessage(const QString msg);  // 事件日志
    void onClearImage();                      // 清空图像
    void creatDir(const QString& path);       // 递归创建目录
};