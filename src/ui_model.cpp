#include "ui_model.h"

void UiModel::adjustSystemSetting(int direction, ChamberController &controller) {
  if (!systemSettings_ || !direction)
    return;
  SystemSettings &s = *systemSettings_;
  const int d = direction > 0 ? 1 : -1;
  switch (systemSettingField_) {
  case SystemSettingField::Language:
    s.language = s.language == Language::Chinese ? Language::English : Language::Chinese;
    controller.setLanguage(s.language); break;
  case SystemSettingField::KeySound: s.keySound = !s.keySound; break;
  case SystemSettingField::Brightness: s.brightness = constrain((int)s.brightness + d * 5, 1, 100); break;
  case SystemSettingField::ScreenSleep: s.screenSleepSeconds = constrain((int)s.screenSleepSeconds + d * 15, 0, 3600); break;
  case SystemSettingField::KeepOnPrinting: s.keepScreenOnPrinting = !s.keepScreenOnPrinting; break;
  case SystemSettingField::EncoderDirection: s.encoderReversed = !s.encoderReversed; break;
  case SystemSettingField::PirStart: s.pirStartSeconds = constrain((int)s.pirStartSeconds + d, 1, 300); break;
  case SystemSettingField::PirStop: s.pirStopSeconds = constrain((int)s.pirStopSeconds + d * 5, 10, 900); break;
  case SystemSettingField::LightOnStart: s.lightOnPrinting = !s.lightOnPrinting; break;
  case SystemSettingField::LightOffStop: s.lightOffAfterPrinting = !s.lightOffAfterPrinting; break;
  case SystemSettingField::BeepOnStart: s.beepOnStart = !s.beepOnStart; break;
  case SystemSettingField::BeepOnStop: s.beepOnStop = !s.beepOnStop; break;
  case SystemSettingField::HeaterCurrent: s.heaterMaxCurrentA = constrain((int)s.heaterMaxCurrentA + d, 1, 12); break;
  case SystemSettingField::HeaterFan: s.heaterFanPercent = constrain((int)s.heaterFanPercent + d * 5, 20, 100); break;
  case SystemSettingField::HeaterProtection: s.heaterBoardLimitC = constrain((int)s.heaterBoardLimitC + d, 40, 180); break;
  case SystemSettingField::TouchCalibration:
  case SystemSettingField::FactoryReset:
  case SystemSettingField::Count: return;
  }
  controller.setPirDelays(s.pirStartSeconds * 1000UL, s.pirStopSeconds * 1000UL);
  controller.setHeaterLimits(s.heaterMaxCurrentA, s.heaterBoardLimitC);
  controller.setHeaterFanPercent(s.heaterFanPercent);
  systemSettingsDirty_ = true;
}

void UiModel::apply(UiAction action, ChamberController &controller) {
  if (systemSettingsOpen_) {
    const uint8_t count = static_cast<uint8_t>(SystemSettingField::Count);
    if (action == UiAction::PreviousMaterial || action == UiAction::NextMaterial) {
      const int d = action == UiAction::NextMaterial ? 1 : -1;
      if (systemSettingsEditing_)
        adjustSystemSetting(d, controller);
      else
        systemSettingField_ = static_cast<SystemSettingField>(
            (static_cast<uint8_t>(systemSettingField_) + count + d) % count);
      return;
    }
    if (action == UiAction::EncoderDoubleClick) {
      systemSettingField_ = static_cast<SystemSettingField>(
          (static_cast<uint8_t>(systemSettingField_) + count - 1) % count);
      return;
    }
    if (action == UiAction::EncoderClick) {
      if (systemSettingField_ == SystemSettingField::TouchCalibration) {
        touchCalibrationRequested_ = true;
      } else if (systemSettingField_ == SystemSettingField::FactoryReset) {
        if (systemSettingsEditing_) {
          factoryResetRequested_ = true;
          systemSettingsEditing_ = false;
        } else {
          systemSettingsEditing_ = true; // 再次单击才执行，防止误触。
        }
      } else {
        const bool numeric = systemSettingField_ == SystemSettingField::Brightness ||
            systemSettingField_ == SystemSettingField::ScreenSleep ||
            systemSettingField_ == SystemSettingField::PirStart ||
            systemSettingField_ == SystemSettingField::PirStop ||
            systemSettingField_ == SystemSettingField::HeaterCurrent ||
            systemSettingField_ == SystemSettingField::HeaterFan ||
            systemSettingField_ == SystemSettingField::HeaterProtection;
        if (numeric) systemSettingsEditing_ = !systemSettingsEditing_;
        else adjustSystemSetting(1, controller);
      }
      return;
    }
    if (action == UiAction::EncoderLongPress) {
      systemSettingsOpen_ = false;
      systemSettingsEditing_ = false;
      systemSettingsSaveRequested_ = systemSettingsDirty_;
      systemSettingsDirty_ = false;
      return;
    }
  }
  switch (action) {
  case UiAction::PreviousMaterial:
    if (materialSettingsOpen_) {
      materialSettingsDirty_ |= controller.adjustProfile(materialSettingField_, -1);
      break;
    }
    materialIndex_ =
        materialIndex_ == 0 ? MATERIAL_COUNT - 1 : materialIndex_ - 1;
    controller.setProfile(materialIndex_);
    break;
  case UiAction::NextMaterial:
    if (materialSettingsOpen_) {
      materialSettingsDirty_ |= controller.adjustProfile(materialSettingField_, 1);
      break;
    }
    materialIndex_ = (materialIndex_ + 1) % MATERIAL_COUNT;
    controller.setProfile(materialIndex_);
    break;
  case UiAction::ToggleAutoExhaust:
    settings_.autoExhaust = !settings_.autoExhaust;
    controller.setAutoExhaust(settings_.autoExhaust);
    break;
  case UiAction::ToggleAutoTemperature:
    settings_.autoTemperature = !settings_.autoTemperature;
    controller.setAutoTemperature(settings_.autoTemperature);
    break;
  case UiAction::TogglePostPrintExhaust:
    settings_.postPrintExhaust = !settings_.postPrintExhaust;
    controller.setPostPrintExhaust(settings_.postPrintExhaust);
    break;
  case UiAction::ToggleSystem:
    settings_.systemEnabled = !settings_.systemEnabled;
    controller.setSystemEnabled(settings_.systemEnabled);
    if (!settings_.systemEnabled) {
      settings_.preheat = false;
      controller.requestPreheat(false);
    }
    break;
  case UiAction::TogglePreheat:
    settings_.preheat = !settings_.preheat;
    if (settings_.preheat && !settings_.systemEnabled) {
      settings_.systemEnabled = true;
      controller.setSystemEnabled(true);
    }
    controller.requestPreheat(settings_.preheat);
    break;
  case UiAction::ToggleLight:
    settings_.light = !settings_.light;
    break;
  case UiAction::ToggleManualExhaust:
    settings_.manualExhaust = !settings_.manualExhaust;
    break;
  case UiAction::OpenMaterialSettings:
    materialSettingsOpen_ = true;
    materialSettingField_ = MaterialField::ChamberMin;
    break;
  case UiAction::OpenSystemSettings:
    systemSettingsOpen_ = true;
    systemSettingsEditing_ = false;
    break;
  case UiAction::EncoderDoubleClick:
    if (materialSettingsOpen_) {
      uint8_t field = static_cast<uint8_t>(materialSettingField_);
      field = field == 0 ? static_cast<uint8_t>(MaterialField::Count) - 1
                         : field - 1;
      materialSettingField_ = static_cast<MaterialField>(field);
    } else {
      // 主界面双击按参考交互强制开启/关闭排气。
      settings_.manualExhaust = !settings_.manualExhaust;
    }
    break;
  case UiAction::EncoderLongPress:
    if (materialSettingsOpen_) {
      materialSettingsOpen_ = false;
      profileSaveRequested_ = materialSettingsDirty_;
      materialSettingsDirty_ = false;
      break;
    }
    settings_.systemEnabled = !settings_.systemEnabled;
    controller.setSystemEnabled(settings_.systemEnabled);
    if (!settings_.systemEnabled) {
      settings_.preheat = false;
      controller.requestPreheat(false);
    }
    break;
  case UiAction::EncoderClick:
    if (materialSettingsOpen_) {
      materialSettingField_ = static_cast<MaterialField>(
          (static_cast<uint8_t>(materialSettingField_) + 1) %
          static_cast<uint8_t>(MaterialField::Count));
    } else {
      // 主界面单击开关照明；设置入口由底部触摸按钮提供。
      settings_.light = !settings_.light;
    }
    break;
  }
}

UiSnapshot UiModel::snapshot(const ChamberController &controller,
                             const Readings &r, const Outputs &o) const {
  const MaterialProfile &profile = controller.profile();
  UiSnapshot s{};
  s.material = profile.name;
  s.previousMaterial = MATERIALS[materialIndex_ == 0 ? MATERIAL_COUNT - 1
                                                     : materialIndex_ - 1].name;
  s.nextMaterial = MATERIALS[(materialIndex_ + 1) % MATERIAL_COUNT].name;
  s.state = o.state;
  s.language = controller.language();
  // clock/networkConnected 由 main.cpp 填充:ui_model
  // 不依赖网络层,避免循环包含。
  strncpy(s.clock, "--:--:--", sizeof(s.clock) - 1);
  s.networkConnected = false;
  s.ahtValid = r.ahtValid;
  s.ntcValid = r.ntcValid;
  s.inaValid = r.inaValid;

  s.chamberC = r.chamberC;
  s.heaterBoardC = r.heaterBoardC;
  s.heaterBoardLimitC = controller.boardLimitC();
  s.humidity = NAN; // 由 main.cpp 从 AHT20 补入
  s.mcuC = r.mcuC;
  s.voltageV = r.supplyVoltageV;
  s.currentA = r.heaterCurrentA;
  s.exhaustPercent = settings_.manualExhaust ? 100 : o.exhaustPercent;
  s.heatPercent = o.heaterPercent;
  s.heaterFanPercent = o.heaterFan ? controller.heaterFanPercent() : 0;

  // 开关状态一律回读控制器,不用 UiModel 内部的副本,保证界面与真实执行一致。
  s.autoExhaust = controller.autoExhaust();
  s.autoTemperature = controller.autoTemperature();
  s.postPrintExhaust = controller.postPrintExhaust();
  s.systemEnabled = controller.systemEnabled();
  s.preheat = controller.preheatRequested();
  s.light = o.light;
  s.manualExhaust = settings_.manualExhaust;
  s.pirMotion = r.pirMotion;

  s.exhaustMinPercent = profile.fanMinPercent;
  s.exhaustMaxPercent = profile.fanMaxPercent;
  s.postExhaustPercent = profile.postExhaustPercent;
  s.heatLimitPercent = controller.heatLimit();
  s.profileMinC = profile.chamberMinC;
  s.profileMaxC = profile.chamberMaxC;
  s.postExhaustSeconds = profile.postExhaustSeconds;
  s.materialSettingsOpen = materialSettingsOpen_;
  s.materialSettingsDirty = materialSettingsDirty_;
  s.materialSettingField = materialSettingField_;
  s.systemSettingsOpen = systemSettingsOpen_;
  s.systemSettingsEditing = systemSettingsEditing_;
  s.systemSettingsDirty = systemSettingsDirty_;
  s.systemSettingField = systemSettingField_;
  if (systemSettings_) s.systemSettings = *systemSettings_;
  return s;
}
