// 热控核心:六态状态机(待机/检测/预热/打印/排气/故障)+ 双 PID 温控 +
// 耗材预设表。main.cpp 每 50ms 调一次 update(),把传感器读数翻译成执行器
// 输出(发热板占空比、各路风扇、灯光)。所有安全联锁都在本文件内闭环,
// 不依赖 UI 或网络层。
#include "controller.h"
#include <math.h>

// 12 种耗材的出厂预设。字段顺序与 MaterialProfile 一致:
// 名称, 仓温下限℃, 仓温上限℃, 排风最低%, 排风最高%, 打印后排风%,
// 打印后排风秒数。 运行时可被 NVS 中保存的用户预设覆盖(见 settings.cpp)。
MaterialProfile MATERIALS[] = {
    {"PLA", 0, 40, 30, 100, 100, 180}, {"PETG", 25, 50, 30, 80, 80, 180},
    {"TPU", 0, 40, 0, 30, 30, 120},    {"ABS", 40, 70, 10, 30, 30, 300},
    {"ASA", 40, 70, 10, 30, 30, 300},  {"PC", 50, 90, 10, 30, 30, 300},
    {"PA", 40, 70, 10, 30, 30, 300},   {"PVA", 0, 40, 20, 60, 60, 180},
    {"PET", 25, 50, 30, 80, 80, 180},  {"PPA", 50, 90, 10, 30, 30, 300},
    {"PEBA", 25, 50, 20, 60, 60, 180}, {"CUSTOM", 0, 40, 0, 100, 100, 180},
};
const size_t MATERIAL_COUNT = sizeof(MATERIALS) / sizeof(MATERIALS[0]);

// 把全部耗材预设恢复为上表中的出厂值(恢复出厂设置时调用)。
void resetMaterialProfiles() {
  // 与 MATERIALS 初始化顺序一致的六元组,避免名称字符串参与拷贝。
  static const float values[][6] = {
      {0, 40, 30, 100, 100, 180}, {25, 50, 30, 80, 80, 180},
      {0, 40, 0, 30, 30, 120},    {40, 70, 10, 30, 30, 300},
      {40, 70, 10, 30, 30, 300},  {50, 90, 10, 30, 30, 300},
      {40, 70, 10, 30, 30, 300},  {0, 40, 20, 60, 60, 180},
      {25, 50, 30, 80, 80, 180},  {50, 90, 10, 30, 30, 300},
      {25, 50, 20, 60, 60, 180},  {0, 40, 0, 100, 100, 180}};
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    MATERIALS[i].chamberMinC = values[i][0];
    MATERIALS[i].chamberMaxC = values[i][1];
    MATERIALS[i].fanMinPercent = values[i][2];
    MATERIALS[i].fanMaxPercent = values[i][3];
    MATERIALS[i].postExhaustPercent = values[i][4];
    MATERIALS[i].postExhaustSeconds = values[i][5];
  }
}

namespace {
constexpr uint32_t SENSOR_FAULT_MS =
    3UL * 1000UL; // 预热/打印中传感器失效多久判故障
constexpr float KP = 8.0f, KI = 0.04f, KD = 15.0f; // 仓温 PID 参数
constexpr float BOARD_KP = 4.0f, BOARD_KI = 0.02f,
                BOARD_KD = 3.0f;           // 热板限温 PID 参数
constexpr float PWM_SLOPE_PER_50MS = 3.0f; // 发热板 PWM 每 50ms 最大变化 3%
} // namespace

// 记录开机时刻基准(人感计时与 PID 的 dt 都以它为起点)。
void ChamberController::begin(uint32_t now) {
  lastMotionMs_ = lastPidMs_ = now;
}

// 切换当前耗材预设;同时清空两个 PID 的历史项,防止旧耗材的积分带到新耗材。
void ChamberController::setProfile(size_t index) {
  if (index < MATERIAL_COUNT) {
    profileIndex_ = index;
    integral_ = previousError_ = boardIntegral_ = boardPreviousError_ = 0;
  }
}

// 整体更新当前耗材预设(UI/Web 保存时调用)。先做与 settings.cpp 一致的
// 白名单校验:温度有限幅且上下限至少相差 5℃,风扇下限不超过上限。
// 校验通过才写入并清零 PID 历史,返回是否成功。
bool ChamberController::updateProfile(float chamberMinC, float chamberMaxC,
                                      uint8_t fanMinPercent,
                                      uint8_t fanMaxPercent,
                                      uint8_t postExhaustPercent,
                                      uint16_t postExhaustSeconds) {
  if (!isfinite(chamberMinC) || !isfinite(chamberMaxC) || chamberMinC < 0 ||
      chamberMinC > 90 || chamberMaxC < 5 || chamberMaxC > 100 ||
      chamberMinC + 5 > chamberMaxC || fanMinPercent > fanMaxPercent ||
      fanMaxPercent > 100 || postExhaustPercent > 100 ||
      postExhaustSeconds > 1800)
    return false;
  MaterialProfile &p = MATERIALS[profileIndex_];
  p.chamberMinC = chamberMinC;
  p.chamberMaxC = chamberMaxC;
  p.fanMinPercent = fanMinPercent;
  p.fanMaxPercent = fanMaxPercent;
  p.postExhaustPercent = postExhaustPercent;
  p.postExhaustSeconds = postExhaustSeconds;
  integral_ = previousError_ = boardIntegral_ = boardPreviousError_ = 0;
  return true;
}

// 耗材参数页旋转编码器的单步调整:按字段决定步长(温度 1℃、风扇 1%、
// 打印后排风 5%、后排风时长 30s),并在各字段的合法区间内钳制,
// 最终统一交给 updateProfile() 做整组校验后写入。
bool ChamberController::adjustProfile(MaterialField field, int direction) {
  if (!direction)
    return false;
  const int d = direction > 0 ? 1 : -1;
  // 复制一份当前预设做"候选值",避免直接改到全局表后校验失败留下半写入状态。
  const MaterialProfile p = profile();
  switch (field) {
  case MaterialField::ChamberMin:
    return updateProfile(
        constrain(p.chamberMinC + d, 0.0f, p.chamberMaxC - 5.0f), p.chamberMaxC,
        p.fanMinPercent, p.fanMaxPercent, p.postExhaustPercent,
        p.postExhaustSeconds);
  case MaterialField::ChamberMax:
    return updateProfile(
        p.chamberMinC,
        constrain(p.chamberMaxC + d, p.chamberMinC + 5.0f, 100.0f),
        p.fanMinPercent, p.fanMaxPercent, p.postExhaustPercent,
        p.postExhaustSeconds);
  case MaterialField::FanMin:
    return updateProfile(
        p.chamberMinC, p.chamberMaxC,
        static_cast<uint8_t>(
            constrain((int)p.fanMinPercent + d, 0, (int)p.fanMaxPercent)),
        p.fanMaxPercent, p.postExhaustPercent, p.postExhaustSeconds);
  case MaterialField::FanMax:
    return updateProfile(
        p.chamberMinC, p.chamberMaxC, p.fanMinPercent,
        static_cast<uint8_t>(
            constrain((int)p.fanMaxPercent + d, (int)p.fanMinPercent, 100)),
        p.postExhaustPercent, p.postExhaustSeconds);
  case MaterialField::PostFan:
    return updateProfile(p.chamberMinC, p.chamberMaxC, p.fanMinPercent,
                         p.fanMaxPercent,
                         constrain((int)p.postExhaustPercent + d * 5, 0, 100),
                         p.postExhaustSeconds);
  case MaterialField::PostExhaust: {
    const int seconds = constrain((int)p.postExhaustSeconds + d * 30, 0, 1800);
    return updateProfile(p.chamberMinC, p.chamberMaxC, p.fanMinPercent,
                         p.fanMaxPercent, p.postExhaustPercent, seconds);
  }
  case MaterialField::Count:
    return false;
  }
  return false;
}

// 仓温加热 PID:目标为耗材仓温下限,输出 0-100% 的"热需求"。
// 误差=目标-实测(温度越低需求越大),积分限幅 ±200 防 windup。
float ChamberController::chamberPid(float input, float dt) {
  const float error = profile().chamberMinC - input;
  integral_ = constrain(integral_ + error * dt, -200.0f, 200.0f);
  const float derivative = (error - previousError_) / dt;
  previousError_ = error;
  return constrain(KP * error + KI * integral_ + KD * derivative, 0.0f, 100.0f);
}

// 热板限温 PID:目标为过温保护阈值,输出 0-100% 的"安全允许功率"。
// 热板越接近阈值允许功率越小,与仓温需求取 min 后生效,安全永远优先。
float ChamberController::boardPid(float input, float dt) {
  const float error = boardLimitC_ - input;
  boardIntegral_ = constrain(boardIntegral_ + error * dt, -300.0f, 300.0f);
  const float derivative = (error - boardPreviousError_) / dt;
  boardPreviousError_ = error;
  return constrain(BOARD_KP * error + BOARD_KI * boardIntegral_ +
                       BOARD_KD * derivative,
                   0.0f, 100.0f);
}

// 每个 50ms 控制周期执行一次:先跑状态机(人感/预热/过温/打印结束),
// 再按状态计算发热板占空比与各路风扇。返回的 Outputs 由 main.cpp 写硬件。
Outputs ChamberController::update(const Readings &in, uint32_t now) {
  Outputs out{0, 0, false, false, false, state_};
  const ChamberState previousState = state_;
  // ---- PIR 人感计时 ----
  // lastMotionMs_:最近一次看到人的时刻(用于打印保持);
  // motionStartedMs_:本轮连续检测到人的起始时刻(用于启动延时),人一离开即清零。
  if (in.pirMotion) {
    lastMotionMs_ = now;
    if (!motionStartedMs_)
      motionStartedMs_ = now;
  } else
    motionStartedMs_ = 0;

  // 三路安全相关传感器(AHT20 仓温/NTC 板温/INA226 电流)全部有效才允许加热。
  const bool sensorsValid = in.ahtValid && in.ntcValid && in.inaValid;
  if (!systemEnabled_) {
    state_ = ChamberState::Idle;
    safetyCooling_ = false;
    coolingStartedMs_ = 0;
  } else if (state_ == ChamberState::Fault) {
    // 故障锁定，关闭再开启系统才允许重新进入状态机。
  } else if (!sensorsValid && (previousState == ChamberState::Preheat ||
                               previousState == ChamberState::Printing)) {
    if (!invalidSinceMs_)
      invalidSinceMs_ = now;
    if (now - invalidSinceMs_ >= SENSOR_FAULT_MS)
      state_ = ChamberState::Fault;
  } else {
    invalidSinceMs_ = 0;
    const bool overTemperature =
        (in.ntcValid && in.heaterBoardC >= boardLimitC_) ||
        (in.ahtValid && in.chamberC >= profile().chamberMaxC);
    // 安全排气增加少量回差，避免温度恰好卡在阈值处反复切换。
    const bool safetyStillHot =
        safetyCooling_ &&
        ((in.ntcValid && in.heaterBoardC >= boardLimitC_ - 5.0f) ||
         (in.ahtValid && in.chamberC >= profile().chamberMaxC - 2.0f));

    if (overTemperature || safetyStillHot) {
      state_ = ChamberState::Cooling;
      safetyCooling_ = true;
      if (!coolingStartedMs_)
        coolingStartedMs_ = now;
    } else {
      if (safetyCooling_) {
        safetyCooling_ = false;
        coolingStartedMs_ = 0;
      }
      const bool pirTriggered =
          motionStartedMs_ && now - motionStartedMs_ >= pirStartDelayMs_;
      const bool pirWaiting =
          motionStartedMs_ && now - motionStartedMs_ < pirStartDelayMs_;
      const bool pirHold = previousState == ChamberState::Printing &&
                           now - lastMotionMs_ < pirStopDelayMs_;

      // 手动提前预热优先于 PIR，开启后立即进入温控，不再等待打印机运动。
      if (preheatRequested_) {
        state_ = ChamberState::Preheat;
        coolingStartedMs_ = 0;
      } else if (pirTriggered || pirHold) {
        state_ = ChamberState::Printing;
        coolingStartedMs_ = 0;
      } else if (pirWaiting) {
        state_ = ChamberState::Detecting;
      } else if (previousState == ChamberState::Printing) {
        if (postPrintExhaust_ && profile().postExhaustSeconds > 0) {
          state_ = ChamberState::Cooling;
          coolingStartedMs_ = now;
        } else {
          state_ = ChamberState::Idle;
        }
      } else if (previousState == ChamberState::Cooling && coolingStartedMs_ &&
                 postPrintExhaust_ &&
                 now - coolingStartedMs_ <
                     profile().postExhaustSeconds * 1000UL) {
        state_ = ChamberState::Cooling;
      } else {
        state_ = ChamberState::Idle;
        coolingStartedMs_ = 0;
      }
    }
  }

  // ---- 按当前状态计算执行器输出 ----
  switch (state_) {
  case ChamberState::Preheat:
  case ChamberState::Printing: {
    if (sensorsValid && autoTemperature_) {
      // 同一控制周期同时运行两个 PID：仓温 PID 给出热需求，热板 PID 给出
      // 安全允许功率。取二者最小值，热板安全限制始终拥有更高优先级。
      const float dt = constrain((now - lastPidMs_) / 1000.0f, 0.01f, 0.25f);
      lastPidMs_ = now;
      const float chamberDemand = chamberPid(in.chamberC, dt);
      const float boardSafeLimit = boardPid(in.heaterBoardC, dt);
      float desired = min(chamberDemand, boardSafeLimit);

      // INA226 可能因采样方向返回负值，限流判断统一取绝对值。超限时按
      // I_limit / I_measured 连续缩放，而不是瞬间切断；后续再经过 PWM 斜率
      // 限制，使功率平滑下降。
      const float measuredCurrentA = fabsf(in.heaterCurrentA);
      if (measuredCurrentA > maxCurrentA_ && measuredCurrentA > 0.01f)
        desired *= maxCurrentA_ / measuredCurrentA;
      desired = min(desired, static_cast<float>(userHeatLimit_));
      // 每个控制周期最多变化 3%，避免热板功率骤变。
      desired = constrain(desired, lastPwm_ - PWM_SLOPE_PER_50MS,
                          lastPwm_ + PWM_SLOPE_PER_50MS);
      lastPwm_ = desired;
      out.heaterPercent = lroundf(desired);
    } else {
      // 自动温控关闭或任一安全传感器无效时，本周期立即输出 0，并丢弃 PID
      // 历史状态；传感器恢复后必须从斜率限制的 0% 重新爬升。
      lastPwm_ = 0;
      lastPidMs_ = now;
      integral_ = previousError_ = boardIntegral_ = boardPreviousError_ = 0;
    }
    out.heaterFan = out.heaterPercent > 0 ||
                    (in.ntcValid && in.heaterBoardC > in.chamberC + 10);
    // 排气分档相对耗材的最低允许仓温：未超过目标保持最低风速，
    // 高出 5/10 °C 后依次进入中速/最高风速。最高温度仍由上面的
    // 安全联锁直接接管为 100% 强排。
    const float over = in.chamberC - profile().chamberMinC;
    if (autoExhaust_) {
      if (over >= 10)
        out.exhaustPercent = profile().fanMaxPercent;
      else if (over >= 5)
        out.exhaustPercent =
            (profile().fanMinPercent + profile().fanMaxPercent) / 2;
      else
        out.exhaustPercent = profile().fanMinPercent;
    }
    break;
  }
  case ChamberState::Cooling:
    // 过温安全排气必须全速；正常打印结束排气遵循当前耗材最高风速。
    out.exhaustPercent = safetyCooling_ ? 100 : profile().postExhaustPercent;
    out.heaterFan = safetyCooling_;
    break;
  case ChamberState::Fault:
    out.exhaustPercent = 100;
    out.heaterFan = true;
    break;
  case ChamberState::Idle:
  case ChamberState::Detecting:
    break;
  }
  if (state_ != ChamberState::Preheat && state_ != ChamberState::Printing) {
    lastPwm_ = 0;
    // 非温控状态持续刷新时间基准并清除历史项，避免长时间待机后首次进入
    // 温控时把整段待机时间计入积分。
    lastPidMs_ = now;
    integral_ = previousError_ = boardIntegral_ = boardPreviousError_ = 0;
  }
  // 主板散热风扇:MCU 超 60℃、检测到加热电流、或任何执行器/灯光在工作时开启。
  out.boardFan = in.mcuC > 60 ||
                 (in.inaValid && fabsf(in.heaterCurrentA) > 0.05f) ||
                 out.heaterPercent > 0 || out.exhaustPercent > 0 || out.light;
  out.state = state_;
  return out;
}
