#pragma once
#include "controller.h"

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
  EncoderClick,
  EncoderDoubleClick,
  EncoderLongPress
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
  ChamberState state;
  Language language; // 决定界面用中文还是英文
  char clock[9];     // "HH:MM:SS";NTP 未同步时为 "--:--:--"
  bool networkConnected;

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

  // ---- 排气区间与功率上限(来自耗材预设与用户设定) ----
  uint8_t exhaustMinPercent;
  uint8_t exhaustMaxPercent;
  uint8_t heatLimitPercent;
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

private:
  UiSettings settings_;
  size_t materialIndex_ = 0;
};
