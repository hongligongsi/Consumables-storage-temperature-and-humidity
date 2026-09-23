#pragma once
#include "controller.h"
#include "settings.h"

// 该模型对应 ui_main_desc 图中的主界面。显示层只需读取 UiSnapshot 即可绘制。

// 输入层(编码器/触摸/串口/Web/MQTT)统一产出的语义动作,由 UiModel::apply()
// 消费。显示层与具体输入设备解耦:任何来源都先翻译成 UiAction。
enum class UiAction : uint8_t {
  PreviousMaterial,       // 上一种耗材预设
  NextMaterial,           // 下一种耗材预设
  ToggleAutoExhaust,      // 切换"排风自动"开关
  ToggleAutoTemperature,  // 切换"温度自动"开关
  TogglePostPrintExhaust, // 切换"打印后排风"开关
  ToggleSystem,           // 切换系统总使能(加热/排风整体启停)
  TogglePreheat,          // 切换提前预热
  ToggleLight,            // 切换仓灯
  ToggleManualExhaust,    // 切换手动强制排气
  OpenMaterialSettings,   // 进入耗材参数页
  OpenSystemSettings,     // 进入系统设置页
  FocusPrevious,          // 焦点在主界面可聚焦控件间前移
  FocusNext,              // 焦点后移
  EncoderClick,           // 编码器短按
  EncoderDoubleClick,     // 编码器双击
  EncoderLongPress        // 编码器长按(设置页内为"保存")
};

// 主界面当前焦点所在的控件,渲染时据此画高亮框。顺序即切换顺序,
// Count 为末尾哨兵,用于循环取模。
enum class MainFocus : uint8_t {
  PreviousMaterial, // 上一切料区
  CurrentMaterial,  // 当前耗材名
  NextMaterial,     // 下一切料区
  AutoExhaust,      // 排风自动开关
  AutoTemperature,  // 温度自动开关
  PostExhaust,      // 打印后排风开关
  System,           // 系统总使能
  Preheat,          // 预热开关
  Light,            // 仓灯开关
  Settings,         // "系统设置"入口
  Count
};

// 系统设置页的条目,顺序即界面显示顺序。
// 增删条目时只需改这里:页码总数与滚动窗口都按 Count 动态计算,无需改渲染代码。
// 注意 Count 仅为末尾哨兵(既表示条目总数,也用作取模边界),不对应任何真实条目。
enum class SystemSettingField : uint8_t {
  Language,
  // ---- 联网开关(长按保存后经 network.onSettingsChanged 即时生效) ----
  WifiEnabled, // 关闭后设备将断开网络,需重启才能恢复远程访问
  MqttEnabled, // 关闭后停止向 MQTT broker 上报
  NtpEnabled,  // 关闭后不再自动校时,沿用上次时钟
  OtaEnabled,  // 关闭后停止 ArduinoOTA 监听
  KeySound,
  Brightness,
  ScreenSleep,
  KeepOnPrinting,
  EncoderDirection,
  PirStart,
  PirStop,
  LightOnStart,
  LightOffStop,
  BeepOnStart,
  BeepOnStop,
  HeaterCurrent,
  HeaterFan,
  HeaterProtection,
  // ---- 日夜配色与时间(手动校时) ----
  Theme,      // 屏幕配色: 0=日间, 1=夜间, 2=自动
  DayStart,   // 日间开始时刻(分钟)
  NightStart, // 夜间开始时刻(分钟)
  Date,       // 日期: 年/月/日 三段编辑
  Time,       // 时间: 时/分 两段编辑
  TouchCalibration,
  FactoryReset,
  Version, // 只读展示固件版本号(值取自 version.h 的 FW_VERSION),不可修改
  Count
};

// 主界面上的运行态开关(掉电不保存的临时操作状态),由 UiModel 自己持有。
struct UiSettings {
  bool autoExhaust = true;      // 排风自动
  bool autoTemperature = true;  // 仓温自动
  bool postPrintExhaust = true; // 打印结束自动排风
  bool systemEnabled = true;    // 系统总使能(关则停止加热/排风输出)
  bool preheat = false;         // 提前预热
  bool light = false;           // 仓灯
  bool manualExhaust = false;   // 手动强制排气(优先级高于自动逻辑)
  uint8_t heatLimit = 100;      // 手动功率上限百分比
};

struct UiSnapshot {
  // ---- 标识与状态 ----
  const char *material;         // 耗材预设名(如 "PLA")
  const char *previousMaterial; // 上一种耗材名(切料区显示)
  const char *nextMaterial;     // 下一种耗材名(切料区显示)
  ChamberState state;           // 六态状态机:待机/检测/预热/打印/排气/故障
  Language language;            // 决定界面用中文还是英文
  char clock[24]; // "YYYY-MM-DD HH:MM:SS";无有效时间时为 "----/--/-- --:--:--"
  bool networkConnected; // STA 是否已联网(顶栏日期着色用)
  uint8_t theme;         // 当前生效配色: 0=日间, 1=夜间
  bool ahtValid;         // AHT20 温湿度读数是否有效
  bool ntcValid;         // NTC 温度读数是否有效
  bool inaValid;         // INA226 电压/电流读数是否有效

  // ---- 八格仪表 ----
  float chamberC;           // 仓内温度(℃)
  float heaterBoardC;       // 发热板/主板温度(℃)
  float heaterBoardLimitC;  // 热端过温阈值,用于把温度读数染色成告警色
  float humidity;           // 相对湿度(%RH)
  float mcuC;               // MCU 内部温度(℃)
  float voltageV;           // 供电电压(V)
  float currentA;           // 加热回路电流(A)
  uint8_t exhaustPercent;   // 排风风扇占空比(%)
  uint8_t heatPercent;      // 发热板功率占空比(%)
  uint8_t heaterFanPercent; // 热风风扇占空比(%)

  // ---- 左侧三个自动开关的真实状态 ----
  bool autoExhaust;
  bool autoTemperature;
  bool postPrintExhaust;
  bool systemEnabled;  // 系统总使能
  bool preheat;        // 预热中
  bool light;          // 仓灯亮
  bool manualExhaust;  // 手动强制排气中
  bool pirMotion;      // PIR 当前是否检测到人
  MainFocus mainFocus; // 主界面焦点

  // ---- 排气区间与功率上限(来自耗材预设与用户设定) ----
  uint8_t exhaustMinPercent;   // 自动排风最低占空比(%)
  uint8_t exhaustMaxPercent;   // 自动排风最高占空比(%)
  uint8_t postExhaustPercent;  // 打印结束后排风占空比(%)
  uint8_t heatLimitPercent;    // 发热功率上限(%)
  float profileMinC;           // 当前耗材仓温下限(℃)
  float profileMaxC;           // 当前耗材仓温上限(℃)
  uint16_t postExhaustSeconds; // 打印结束后持续排风时长(s)

  // ---- 耗材参数编辑页 ----
  bool materialSettingsOpen;             // 耗材参数页是否打开
  bool materialSettingsDirty;            // 耗材参数是否有未保存修改
  MaterialField materialSettingField;    // 耗材页当前选中的字段
  bool systemSettingsOpen;               // 系统设置页是否打开
  bool systemSettingsEditing;            // 是否处于某项的编辑态(旋转改值)
  bool systemSettingsDirty;              // 系统设置是否有未保存修改
  SystemSettingField systemSettingField; // 系统设置页当前选中条目
  uint8_t
      systemSettingsSubField;    // 日期/时间编辑时的子段索引(年/月/日 或 时/分)
  SystemSettings systemSettings; // 系统设置的可编辑副本(保存时才落 NVS)
  bool touchCalibrationActive;   // 是否处于触摸屏校准流程
  uint8_t touchCalibrationStep;  // 校准当前步骤(第几个校准点)
};

// UI 核心状态机:持有焦点、当前页、运行态开关等全部界面状态,并把输入动作
// (UiAction)翻译成对控制器/系统设置的修改。本身不画屏,显示由 TftUi 完成。
class UiModel {
public:
  // 消费一个输入动作:切料、翻页、改焦点、进入编辑、保存等全部分支在此。
  void apply(UiAction action, ChamberController &controller);
  // 把控制器状态、传感器读数、执行器输出汇总成一帧不可变快照,供显示层渲染。
  UiSnapshot snapshot(const ChamberController &controller,
                      const Readings &readings, const Outputs &outputs) const;
  // 主界面运行态开关集合(掉电不保存的临时状态)。
  const UiSettings &settings() const { return settings_; }
  // 供 Web/MQTT 按索引切料(与 apply(NextMaterial/PreviousMaterial)
  // 路径一致,调用方须同步 controller.setProfile())。
  void setMaterialIndex(size_t i) {
    materialIndex_ = constrain((long)i, 0L, (long)MATERIAL_COUNT - 1);
  }
  size_t materialIndex() const { return materialIndex_; } // 当前耗材预设下标
  bool materialSettingsOpen() const {
    return materialSettingsOpen_;
  } // 耗材页是否打开
  bool systemSettingsOpen() const {
    return systemSettingsOpen_;
  } // 系统设置页是否打开
  // 绑定 main.cpp 中那份持久化系统设置;调整后只改这份内存,长按保存才落 NVS。
  void bindSystemSettings(SystemSettings &settings) {
    systemSettings_ = &settings;
  }
  // 以下 take*() 都是"读一次即清零"的边沿请求:main.cpp 轮询到后执行
  // 保存/校准/恢复出厂等副作用,避免在输入回调里直接做重操作。
  bool takeProfileSaveRequest() {
    const bool requested = profileSaveRequested_;
    profileSaveRequested_ = false;
    return requested;
  }
  bool takeSystemSettingsSaveRequest() {
    const bool requested = systemSettingsSaveRequested_;
    systemSettingsSaveRequested_ = false;
    return requested;
  }
  bool takeTouchCalibrationRequest() {
    const bool requested = touchCalibrationRequested_;
    touchCalibrationRequested_ = false;
    return requested;
  }
  bool takeFactoryResetRequest() {
    const bool requested = factoryResetRequested_;
    factoryResetRequested_ = false;
    return requested;
  }

private:
  UiSettings settings_;                              // 主界面运行态开关
  size_t materialIndex_ = 0;                         // 当前耗材预设下标
  MainFocus mainFocus_ = MainFocus::CurrentMaterial; // 主界面焦点
  bool materialSettingsOpen_ = false;                // 耗材参数页打开标志
  bool materialSettingsDirty_ = false;               // 耗材参数有未保存修改
  bool profileSaveRequested_ = false;                // 请求保存耗材参数(边沿)
  MaterialField materialSettingField_ =
      MaterialField::ChamberMin;             // 耗材页选中字段
  SystemSettings *systemSettings_ = nullptr; // 绑定的持久化系统设置(不拥有)
  bool systemSettingsOpen_ = false;          // 系统设置页打开标志
  bool systemSettingsEditing_ = false;       // 当前条目处于编辑态
  bool systemSettingsDirty_ = false;         // 系统设置有未保存修改
  uint8_t systemSettingsSubField_ = 0; // 日期(3段)/时间(2段)编辑时的当前子段
  bool systemSettingsSaveRequested_ = false; // 请求保存系统设置(边沿)
  bool touchCalibrationRequested_ = false;   // 请求进入触摸校准(边沿)
  bool factoryResetRequested_ = false;       // 请求恢复出厂设置(边沿)
  SystemSettingField systemSettingField_ =
      SystemSettingField::Language; // 设置页选中条目
  // 旋转编码器改系统设置:开关类取反、数值类按步长增减,并置脏。
  void adjustSystemSetting(int direction, ChamberController &controller);
  // 手动校时:dateField=true 调整年/月/日(子段 0/1/2),false 调整时/分(0/1)。
  void adjustClock(bool dateField, uint8_t sub, int direction);
};
