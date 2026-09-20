#pragma once
#include <Arduino.h>

enum class Language : uint8_t { Chinese, English };
enum class ChamberState : uint8_t {
  Idle,
  Detecting,
  Preheat,
  Printing,
  Cooling,
  Fault
};

struct MaterialProfile {
  const char *name;
  float chamberMinC;
  float chamberMaxC;
  uint8_t fanMinPercent;
  uint8_t fanMaxPercent;
  uint16_t postExhaustSeconds;
};

extern const MaterialProfile MATERIALS[];
extern const size_t MATERIAL_COUNT;

struct Readings {
  float chamberC;
  float heaterBoardC;
  float heaterCurrentA;
  float supplyVoltageV;
  float mcuC;
  bool pirMotion;
  bool ahtValid;
  bool ntcValid;
  bool inaValid;
};

struct Outputs {
  uint8_t heaterPercent;
  uint8_t exhaustPercent;
  bool heaterFan;
  bool boardFan;
  bool light;
  ChamberState state;
};

class ChamberController {
public:
  void begin(uint32_t now);
  void setProfile(size_t index);
  void setLanguage(Language language) { language_ = language; }
  Language language() const { return language_; }
  void requestPreheat(bool enabled) { preheatRequested_ = enabled; }
  void setHeatLimit(uint8_t percent) {
    userHeatLimit_ = constrain(percent, 0, 100);
  }
  void setSystemEnabled(bool enabled) { systemEnabled_ = enabled; }
  bool systemEnabled() const { return systemEnabled_; }
  void setAutoTemperature(bool enabled) { autoTemperature_ = enabled; }
  void setAutoExhaust(bool enabled) { autoExhaust_ = enabled; }
  void setPostPrintExhaust(bool enabled) { postPrintExhaust_ = enabled; }
  // 以下 getter 供界面回读真实开关状态,避免 UI
  // 自己维护一份可能与控制器不一致的副本。
  bool autoTemperature() const { return autoTemperature_; }
  bool autoExhaust() const { return autoExhaust_; }
  bool postPrintExhaust() const { return postPrintExhaust_; }
  bool preheatRequested() const { return preheatRequested_; }
  uint8_t heatLimit() const { return userHeatLimit_; }
  float boardLimitC() const { return boardLimitC_; }
  void setPirDelays(uint32_t startMs, uint32_t stopMs) {
    pirStartDelayMs_ = startMs;
    pirStopDelayMs_ = stopMs;
  }
  void setHeaterLimits(float maxCurrentA, float boardLimitC) {
    maxCurrentA_ = maxCurrentA;
    boardLimitC_ = boardLimitC;
  }
  void setHeaterFanPercent(uint8_t percent) {
    heaterFanPercent_ = constrain(percent, 20, 100);
  }
  Outputs update(const Readings &input, uint32_t now);
  const MaterialProfile &profile() const { return MATERIALS[profileIndex_]; }
  ChamberState state() const { return state_; }

private:
  float chamberPid(float input, uint32_t now);
  float boardPid(float input, uint32_t now);
  ChamberState state_ = ChamberState::Idle;
  Language language_ = Language::Chinese;
  size_t profileIndex_ = 0;
  uint8_t userHeatLimit_ = 100;
  bool preheatRequested_ = false;
  bool systemEnabled_ = true;
  bool autoTemperature_ = true;
  bool autoExhaust_ = true;
  bool postPrintExhaust_ = true;
  uint32_t lastMotionMs_ = 0;
  uint32_t lastPidMs_ = 0, coolingStartedMs_ = 0;
  uint32_t invalidSinceMs_ = 0;
  uint32_t pirStartDelayMs_ = 5UL * 1000UL;
  uint32_t pirStopDelayMs_ = 50UL * 1000UL;
  uint32_t motionStartedMs_ = 0;
  float integral_ = 0, previousError_ = 0;
  float boardIntegral_ = 0, boardPreviousError_ = 0, lastPwm_ = 0;
  float maxCurrentA_ = 6, boardLimitC_ = 80;
  uint8_t heaterFanPercent_ = 100;
};
