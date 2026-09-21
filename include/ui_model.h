#pragma once
#include "controller.h"
#include "settings.h"

// 该模型对应 ui_main_desc 图中的主界面。显示库只需读取 UiSnapshot 即可绘制。
enum class UiAction : uint8_t {
  PreviousMaterial,
  NextMaterial,
  ToggleAutoExhaust,
  ToggleAutoTemperature,
  TogglePostPrintExhaust,
  ToggleSystem,
  TogglePreheat,
  ToggleLight,
  ToggleManualExhaust,
  OpenMaterialSettings,
  OpenSystemSettings,
  FocusPrevious,
  FocusNext,
  EncoderClick,
  EncoderDoubleClick,
  EncoderLongPress
};

enum class MainFocus : uint8_t {
  PreviousMaterial,
  CurrentMaterial,
  NextMaterial,
  AutoExhaust,
  AutoTemperature,
  PostExhaust,
  System,
  Preheat,
  Light,
  Settings,
  Count
};

// 系统设置页的条目,顺序即界面显示顺序。
// 增删条目时只需改这里:页码总数与滚动窗口都按 Count 动态计算,无需改渲染代码。
// 注意 Count 仅为末尾哨兵(既表示条目总数,也用作取模边界),不对应任何真实条目。
enum class SystemSettingField : uint8_t {
  Language,
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

struct UiSettings {
  bool autoExhaust = true;
  bool autoTemperature = true;
  bool postPrintExhaust = true;
  bool systemEnabled = true;
  bool preheat = false;
  bool light = false;
  bool manualExhaust = false;
  uint8_t heatLimit = 100;
};

struct UiSnapshot {
  // ---- 标识与状态 ----
  const char *material; // 耗材预设名(如 "PLA")
  const char *previousMaterial;
  const char *nextMaterial;
  ChamberState state;
  Language language; // 决定界面用中文还是英文
  char clock[24]; // "YYYY-MM-DD HH:MM:SS";无有效时间时为 "----/--/-- --:--:--"
  bool networkConnected;
  uint8_t theme; // 当前生效配色: 0=日间, 1=夜间
  bool ahtValid;
  bool ntcValid;
  bool inaValid;

  // ---- 八格仪表 ----
  float chamberC;
  float heaterBoardC;
  float heaterBoardLimitC; // 热端过温阈值,用于把温度读数染色成告警色
  float humidity;
  float mcuC;
  float voltageV;
  float currentA;
  uint8_t exhaustPercent;
  uint8_t heatPercent;
  uint8_t heaterFanPercent;

  // ---- 左侧三个自动开关的真实状态 ----
  bool autoExhaust;
  bool autoTemperature;
  bool postPrintExhaust;
  bool systemEnabled;
  bool preheat;
  bool light;
  bool manualExhaust;
  bool pirMotion;
  MainFocus mainFocus;

  // ---- 排气区间与功率上限(来自耗材预设与用户设定) ----
  uint8_t exhaustMinPercent;
  uint8_t exhaustMaxPercent;
  uint8_t postExhaustPercent;
  uint8_t heatLimitPercent;
  float profileMinC;
  float profileMaxC;
  uint16_t postExhaustSeconds;

  // ---- 耗材参数编辑页 ----
  bool materialSettingsOpen;
  bool materialSettingsDirty;
  MaterialField materialSettingField;
  bool systemSettingsOpen;
  bool systemSettingsEditing;
  bool systemSettingsDirty;
  SystemSettingField systemSettingField;
  uint8_t
      systemSettingsSubField; // 日期/时间编辑时的子段索引(年/月/日 或 时/分)
  SystemSettings systemSettings;
  bool touchCalibrationActive;
  uint8_t touchCalibrationStep;
};

class UiModel {
public:
  void apply(UiAction action, ChamberController &controller);
  UiSnapshot snapshot(const ChamberController &controller,
                      const Readings &readings, const Outputs &outputs) const;
  const UiSettings &settings() const { return settings_; }
  // 供 Web/MQTT 按索引切料(与 apply(NextMaterial/PreviousMaterial)
  // 路径一致,调用方须同步 controller.setProfile())。
  void setMaterialIndex(size_t i) {
    materialIndex_ = constrain((long)i, 0L, (long)MATERIAL_COUNT - 1);
  }
  size_t materialIndex() const { return materialIndex_; }
  bool materialSettingsOpen() const { return materialSettingsOpen_; }
  bool systemSettingsOpen() const { return systemSettingsOpen_; }
  void bindSystemSettings(SystemSettings &settings) {
    systemSettings_ = &settings;
  }
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
  UiSettings settings_;
  size_t materialIndex_ = 0;
  MainFocus mainFocus_ = MainFocus::CurrentMaterial;
  bool materialSettingsOpen_ = false;
  bool materialSettingsDirty_ = false;
  bool profileSaveRequested_ = false;
  MaterialField materialSettingField_ = MaterialField::ChamberMin;
  SystemSettings *systemSettings_ = nullptr;
  bool systemSettingsOpen_ = false;
  bool systemSettingsEditing_ = false;
  bool systemSettingsDirty_ = false;
  uint8_t systemSettingsSubField_ = 0; // 日期(3段)/时间(2段)编辑时的当前子段
  bool systemSettingsSaveRequested_ = false;
  bool touchCalibrationRequested_ = false;
  bool factoryResetRequested_ = false;
  SystemSettingField systemSettingField_ = SystemSettingField::Language;
  void adjustSystemSetting(int direction, ChamberController &controller);
  // 手动校时:dateField=true 调整年/月/日(子段 0/1/2),false 调整时/分(0/1)。
  void adjustClock(bool dateField, uint8_t sub, int direction);
};
