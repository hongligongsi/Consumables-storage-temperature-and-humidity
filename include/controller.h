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

// 故障原因码:进入 Fault 的那一次由控制器判定并保持,直到复位。
// 枚举值**本身就是**屏上显示的编号(AhtLost=1 → "F-01",None=0 只是哨兵),
// 故新增码只能追加在 Count 之前,不能改变已有码的相对顺序(否则历史故障记录
// 与文档表格的含义会整体错位)。
// 注意:枚举顺序是"显示编号",不是判定优先级 —— 判定顺序见 classifyFault()。
enum class FaultCode : uint8_t {
  None = 0,        // 无故障(非 Fault 状态一律为此值)
  AhtLost,         // F-01 仓温湿度传感器掉线或读数越界
  NtcLost,         // F-02 热板 NTC 断线,或 ADC 采到端点(判为开路/短路)
  BoardOverTemp,   // F-03 热板硬过温(超过限值 + 硬保护余量)
  ChamberOverTemp, // F-04 仓温硬超上限(超过耗材上限 + 硬保护余量)
  OverCurrent,     // F-05 加热回路硬过流(超过限值 × 硬保护倍数)
  InaLost,         // F-06 INA226 掉线或读数越界(电压/电流采样不可信)
  Count
};

struct MaterialProfile {
  const char *name;
  float chamberMinC;
  float chamberMaxC;
  uint8_t fanMinPercent;
  uint8_t fanMaxPercent;
  uint8_t postExhaustPercent;
  uint16_t postExhaustSeconds;
};

enum class MaterialField : uint8_t {
  ChamberMin,
  ChamberMax,
  FanMin,
  FanMax,
  PostFan,
  PostExhaust,
  Count
};

extern MaterialProfile MATERIALS[];
extern const size_t MATERIAL_COUNT;
void resetMaterialProfiles();

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
  FaultCode fault; // 仅当 state==Fault 时有意义,否则为 None
};

class ChamberController {
public:
  void begin(uint32_t now);
  void setProfile(size_t index);
  bool adjustProfile(MaterialField field, int direction);
  bool updateProfile(float chamberMinC, float chamberMaxC,
                     uint8_t fanMinPercent, uint8_t fanMaxPercent,
                     uint8_t postExhaustPercent,
                     uint16_t postExhaustSeconds);
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
  uint8_t heaterFanPercent() const { return heaterFanPercent_; }
  Outputs update(const Readings &input, uint32_t now);
  const MaterialProfile &profile() const { return MATERIALS[profileIndex_]; }
  ChamberState state() const { return state_; }
  // 本次故障的原因码;非 Fault 状态为 None。
  FaultCode faultCode() const { return fault_; }
  // 解除故障锁定(唯一触发源:屏上长按编码器)。热类故障复位后先进安全
  // 冷却姿态、其余回待机,详见实现处说明。关闭系统总使能走的是另一条路径
  // (update() 里的 !systemEnabled_ 分支),不经过这里。
  void clearFault();
  // NVS 故障记忆配套:该故障码在断电重启后是否应恢复 Fault 锁定。
  // 热类(硬过温/硬过流)是物理性危险,拔电重启不能绕过;传感器掉线类
  // 开机本来就会重新走检测流程,恢复了也会立即被新判定覆盖,故不恢复。
  static bool persistsAcrossReboot(FaultCode code);
  // 开机时从 NVS 恢复故障锁定(仅热类,判定见 persistsAcrossReboot)。
  // 效果与 update() 的自然锁定一致:state_=Fault、fault_=code,输出层
  // 自动进入全速排风的安全姿态;解除同样只能靠长按复位或关系统。
  void restoreFault(FaultCode code);

private:
  // 两个 PID 必须使用同一个采样周期，才能在调度抖动时仍可直接比较并取最小值。
  float chamberPid(float input, float dt);
  float boardPid(float input, float dt);
  // 判定本次进入 Fault 的原因码。优先级:硬过温 > 硬过流 > 传感器掉线
  // —— 越危险的越先报;掉线判据自带 valid 前置,不会掩盖真实超温。
  FaultCode classifyFault(const Readings &in) const;
  ChamberState state_ = ChamberState::Idle;
  FaultCode fault_ = FaultCode::None;
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
  uint32_t hardTripSinceMs_ = 0; // 硬保护(过温/过流)连续越界的起点
  uint32_t pirStartDelayMs_ = 25UL * 1000UL;
  uint32_t pirStopDelayMs_ = 50UL * 1000UL;
  uint32_t motionStartedMs_ = 0;
  bool safetyCooling_ = false;
  float integral_ = 0, previousError_ = 0;
  float boardIntegral_ = 0, boardPreviousError_ = 0, lastPwm_ = 0;
  float maxCurrentA_ = 6, boardLimitC_ = 80;
  uint8_t heaterFanPercent_ = 100;
};
