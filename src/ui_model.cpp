#include "ui_model.h"

void UiModel::apply(UiAction action, ChamberController &controller) {
  switch (action) {
  case UiAction::PreviousMaterial:
    materialIndex_ =
        materialIndex_ == 0 ? MATERIAL_COUNT - 1 : materialIndex_ - 1;
    controller.setProfile(materialIndex_);
    break;
  case UiAction::NextMaterial:
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
    break;
  case UiAction::TogglePreheat:
    settings_.preheat = !settings_.preheat;
    controller.requestPreheat(settings_.preheat);
    break;
  case UiAction::ToggleLight:
    settings_.light = !settings_.light;
    break;
  case UiAction::EncoderDoubleClick:
    settings_.manualExhaust = !settings_.manualExhaust;
    break;
  case UiAction::EncoderLongPress:
    settings_.systemEnabled = !settings_.systemEnabled;
    controller.setSystemEnabled(settings_.systemEnabled);
    break;
  case UiAction::EncoderClick:
    break; // 显示层用此事件进入/退出当前设置项。
  }
}

UiSnapshot UiModel::snapshot(const ChamberController &controller,
                             const Readings &r, const Outputs &o) const {
  const MaterialProfile &profile = controller.profile();
  UiSnapshot s{};
  s.material = profile.name;
  s.state = o.state;
  s.language = controller.language();
  // clock/networkConnected 由 main.cpp 填充:ui_model
  // 不依赖网络层,避免循环包含。
  strncpy(s.clock, "--:--:--", sizeof(s.clock) - 1);
  s.networkConnected = false;

  s.chamberC = r.chamberC;
  s.heaterBoardC = r.heaterBoardC;
  s.heaterBoardLimitC = controller.boardLimitC();
  s.humidity = NAN; // 由 main.cpp 从 AHT20 补入
  s.mcuC = r.mcuC;
  s.voltageV = r.supplyVoltageV;
  s.currentA = r.heaterCurrentA;
  s.exhaustPercent = settings_.manualExhaust ? 100 : o.exhaustPercent;
  s.heatPercent = o.heaterPercent;
  s.heaterFanPercent = o.heaterFan ? 100 : 0;

  // 开关状态一律回读控制器,不用 UiModel 内部的副本,保证界面与真实执行一致。
  s.autoExhaust = controller.autoExhaust();
  s.autoTemperature = controller.autoTemperature();
  s.postPrintExhaust = controller.postPrintExhaust();
  s.systemEnabled = controller.systemEnabled();
  s.preheat = controller.preheatRequested();
  s.light = settings_.light;

  s.exhaustMinPercent = profile.fanMinPercent;
  s.exhaustMaxPercent = profile.fanMaxPercent;
  s.heatLimitPercent = controller.heatLimit();
  return s;
}
