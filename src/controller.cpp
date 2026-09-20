#include "controller.h"
#include <math.h>

const MaterialProfile MATERIALS[] = {
    {"PLA", 0, 40, 30, 100, 180},  {"PETG", 25, 50, 30, 80, 180},
    {"TPU", 0, 40, 0, 30, 120},    {"ABS", 40, 70, 10, 30, 300},
    {"ASA", 40, 70, 10, 30, 300},  {"PC", 50, 90, 10, 30, 300},
    {"PA", 40, 70, 10, 30, 300},   {"PVA", 0, 40, 20, 60, 180},
    {"PET", 25, 50, 30, 80, 180},  {"PPA", 50, 90, 10, 30, 300},
    {"PEBA", 25, 50, 20, 60, 180}, {"CUSTOM", 0, 40, 0, 100, 180},
};
const size_t MATERIAL_COUNT = sizeof(MATERIALS) / sizeof(MATERIALS[0]);

namespace {
constexpr uint32_t SENSOR_FAULT_MS = 3UL * 1000UL;
constexpr float KP = 8.0f, KI = 0.04f, KD = 15.0f;
constexpr float BOARD_KP = 4.0f, BOARD_KI = 0.02f, BOARD_KD = 3.0f;
constexpr float PWM_SLOPE_PER_50MS = 3.0f;
} // namespace

void ChamberController::begin(uint32_t now) {
  lastMotionMs_ = lastPidMs_ = now;
}

void ChamberController::setProfile(size_t index) {
  if (index < MATERIAL_COUNT) {
    profileIndex_ = index;
    integral_ = previousError_ = 0;
  }
}

float ChamberController::chamberPid(float input, uint32_t now) {
  const float dt = max(0.1f, (now - lastPidMs_) / 1000.0f);
  lastPidMs_ = now;
  const float error = profile().chamberMinC - input;
  integral_ = constrain(integral_ + error * dt, -200.0f, 200.0f);
  const float derivative = (error - previousError_) / dt;
  previousError_ = error;
  return constrain(KP * error + KI * integral_ + KD * derivative, 0.0f, 100.0f);
}

float ChamberController::boardPid(float input, uint32_t now) {
  const float dt = max(0.05f, (now - lastPidMs_) / 1000.0f);
  const float error = boardLimitC_ - input;
  boardIntegral_ = constrain(boardIntegral_ + error * dt, -300.0f, 300.0f);
  const float derivative = (error - boardPreviousError_) / dt;
  boardPreviousError_ = error;
  return constrain(BOARD_KP * error + BOARD_KI * boardIntegral_ +
                       BOARD_KD * derivative,
                   0.0f, 100.0f);
}

Outputs ChamberController::update(const Readings &in, uint32_t now) {
  Outputs out{0, 0, false, false, false, state_};
  if (in.pirMotion) {
    lastMotionMs_ = now;
    if (!motionStartedMs_)
      motionStartedMs_ = now;
  } else
    motionStartedMs_ = 0;

  const bool sensorsValid = in.ahtValid && in.ntcValid && in.inaValid;
  if (!systemEnabled_) {
    state_ = ChamberState::Idle;
  } else if (!sensorsValid && (state_ == ChamberState::Preheat ||
                               state_ == ChamberState::Printing)) {
    if (!invalidSinceMs_)
      invalidSinceMs_ = now;
    if (now - invalidSinceMs_ > SENSOR_FAULT_MS)
      state_ = ChamberState::Fault;
  } else {
    invalidSinceMs_ = 0;
  }
  if (state_ == ChamberState::Fault) {
    // 传感器恢复后仍保持故障锁定；应由 UI 让用户检查后显式复位。
  } else if (in.ntcValid && in.heaterBoardC >= boardLimitC_) {
    state_ = ChamberState::Cooling;
  } else if (in.ahtValid && in.chamberC >= profile().chamberMaxC) {
    // 安全联锁:仓温达到耗材上限必须停加热并强排,与热板过温同级。
    // autoExhaust_ 只决定常规分段排气的强度,绝不可作为该联锁的开关,
    // 否则关掉“自动排气”就等于关掉了仓温超温保护。
    state_ = ChamberState::Cooling;
  } else if (motionStartedMs_ && now - motionStartedMs_ < pirStartDelayMs_) {
    state_ = ChamberState::Detecting;
  } else if ((motionStartedMs_ && now - motionStartedMs_ >= pirStartDelayMs_) ||
             (state_ == ChamberState::Printing &&
              now - lastMotionMs_ < pirStopDelayMs_)) {
    state_ = ChamberState::Printing;
  } else if (state_ == ChamberState::Printing ||
             state_ == ChamberState::Cooling) {
    state_ = ChamberState::Cooling;
    if (!coolingStartedMs_)
      coolingStartedMs_ = now;
  } else if (preheatRequested_) {
    state_ = ChamberState::Preheat;
  } else {
    state_ = ChamberState::Idle;
  }

  switch (state_) {
  case ChamberState::Preheat:
  case ChamberState::Printing: {
    if (sensorsValid && autoTemperature_) {
      float desired =
          min(chamberPid(in.chamberC, now), boardPid(in.heaterBoardC, now));
      if (in.heaterCurrentA > maxCurrentA_)
        desired *= maxCurrentA_ / in.heaterCurrentA;
      desired = min(desired, static_cast<float>(userHeatLimit_));
      // 每个控制周期最多变化 3%，避免热板功率骤变。
      desired = constrain(desired, lastPwm_ - PWM_SLOPE_PER_50MS,
                          lastPwm_ + PWM_SLOPE_PER_50MS);
      lastPwm_ = desired;
      out.heaterPercent = lroundf(desired);
    }
    out.heaterFan = out.heaterPercent > 0 ||
                    (in.ntcValid && in.heaterBoardC > in.chamberC + 10);
    const float over = in.chamberC - profile().chamberMaxC;
    if (autoExhaust_) {
      if (over >= 10)
        out.exhaustPercent = profile().fanMaxPercent;
      else if (over >= 5)
        out.exhaustPercent =
            (profile().fanMinPercent + profile().fanMaxPercent) / 2;
      else
        out.exhaustPercent = profile().fanMinPercent;
    }
    out.light = true;
    break;
  }
  case ChamberState::Cooling:
    out.exhaustPercent = 100;
    out.heaterFan = true;
    out.light = false;
    if (!coolingStartedMs_)
      coolingStartedMs_ = now;
    if (postPrintExhaust_ &&
        now - coolingStartedMs_ > profile().postExhaustSeconds * 1000UL) {
      state_ = ChamberState::Idle;
      coolingStartedMs_ = 0;
    }
    break;
  case ChamberState::Fault:
    out.exhaustPercent = 100;
    out.heaterFan = true;
    break;
  case ChamberState::Idle:
  case ChamberState::Detecting:
    break;
  }
  out.boardFan = in.mcuC > 60 || out.heaterPercent > 0 ||
                 out.exhaustPercent > 0 || out.light;
  out.state = state_;
  return out;
}
