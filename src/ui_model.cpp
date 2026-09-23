// UI 状态机实现:把编码器/触摸/串口/Web 来的 UiAction 翻译成控制器命令与
// 页面状态变化,并在 snapshot() 中汇总出渲染所需的只读快照。
// 三层职责划分:输入解析在 main.cpp,状态流转在本文件,绘制在 tft_ui.cpp。
#include "ui_model.h"
#include "pins.h"
#include <sys/time.h> // settimeofday:手动校时

// 调整当前系统设置项:direction>0 为加/正向,direction<0 为减/反向。
// 只处理可编辑项的取值变化,随后统一把新值下发给控制器并置脏(等待长按保存到
// NVS)。
void UiModel::adjustSystemSetting(int direction,
                                  ChamberController &controller) {
  if (!systemSettings_ || !direction)
    return;
  SystemSettings &s = *systemSettings_;
  const int d = direction > 0 ? 1 : -1;
  switch (systemSettingField_) {
  // 开关类:每次取反,不区分旋转方向。
  case SystemSettingField::Language:
    s.language =
        s.language == Language::Chinese ? Language::English : Language::Chinese;
    controller.setLanguage(s.language);
    break;
  // ---- 联网开关:与其余开关一致,单击即取反 ----
  case SystemSettingField::WifiEnabled:
    s.wifiEnabled = !s.wifiEnabled;
    break;
  case SystemSettingField::MqttEnabled:
    s.mqttEnabled = !s.mqttEnabled;
    break;
  case SystemSettingField::NtpEnabled:
    s.ntpEnabled = !s.ntpEnabled;
    break;
  case SystemSettingField::OtaEnabled:
    s.otaEnabled = !s.otaEnabled;
    break;
  case SystemSettingField::KeySound:
    s.keySound = !s.keySound;
    break;
  // 数值类:按固定步长增减,并 clamp 在各项允许区间内。
  case SystemSettingField::Brightness:
    s.brightness = constrain((int)s.brightness + d * 5, 1, 100);
    break;
  case SystemSettingField::ScreenSleep:
    s.screenSleepSeconds =
        constrain((int)s.screenSleepSeconds + d * 15, 0, 3600);
    break;
  case SystemSettingField::KeepOnPrinting:
    s.keepScreenOnPrinting = !s.keepScreenOnPrinting;
    break;
  case SystemSettingField::EncoderDirection:
    s.encoderReversed = !s.encoderReversed;
    break;
  case SystemSettingField::PirStart:
    s.pirStartSeconds = constrain((int)s.pirStartSeconds + d, 1, 300);
    break;
  case SystemSettingField::PirStop:
    s.pirStopSeconds = constrain((int)s.pirStopSeconds + d * 5, 10, 900);
    break;
  case SystemSettingField::LightOnStart:
    s.lightOnPrinting = !s.lightOnPrinting;
    break;
  case SystemSettingField::LightOffStop:
    s.lightOffAfterPrinting = !s.lightOffAfterPrinting;
    break;
  case SystemSettingField::BeepOnStart:
    s.beepOnStart = !s.beepOnStart;
    break;
  case SystemSettingField::BeepOnStop:
    s.beepOnStop = !s.beepOnStop;
    break;
  case SystemSettingField::HeaterCurrent:
    s.heaterMaxCurrentA = constrain((int)s.heaterMaxCurrentA + d, 1, 12);
    break;
  case SystemSettingField::HeaterFan:
    s.heaterFanPercent = constrain((int)s.heaterFanPercent + d * 5, 20, 100);
    break;
  case SystemSettingField::HeaterProtection:
    s.heaterBoardLimitC = constrain((int)s.heaterBoardLimitC + d, 40, 180);
    break;
  // ---- 日夜配色与时间 ----
  case SystemSettingField::Theme:
    // 0=日间 1=夜间 2=自动,旋转在三种取值间循环。
    s.theme = static_cast<uint8_t>(((int)s.theme + 3 + d) % 3);
    break;
  case SystemSettingField::DayStart:
    // 上界取 1425(23:45) 而非 1439,否则末次递增会被夹到 23:59,破坏 15
    // 分钟步长。
    s.dayStartMinutes = constrain((int)s.dayStartMinutes + d * 15, 0, 1425);
    break;
  case SystemSettingField::NightStart:
    s.nightStartMinutes = constrain((int)s.nightStartMinutes + d * 15, 0, 1425);
    break;
  // 日期/时间:直接改写系统时钟,不走下面的控制器下发。
  case SystemSettingField::Date:
    adjustClock(true, systemSettingsSubField_, direction);
    return;
  case SystemSettingField::Time:
    adjustClock(false, systemSettingsSubField_, direction);
    return;
  // 非数值项:触摸校准与恢复出厂由 apply() 通过请求标志处理,版本号只读,Count
  // 是哨兵。
  case SystemSettingField::TouchCalibration:
  case SystemSettingField::FactoryReset:
  case SystemSettingField::Version: // 只读展示,不参与修改,也不置脏。
  case SystemSettingField::Count:
    return;
  }
  // 已修改的值立即生效(风扇/限流/温度保护/PIR 延时),界面无需重新进设置页。
  controller.setPirDelays(s.pirStartSeconds * 1000UL,
                          s.pirStopSeconds * 1000UL);
  controller.setHeaterLimits(s.heaterMaxCurrentA, s.heaterBoardLimitC);
  controller.setHeaterFanPercent(s.heaterFanPercent);
  systemSettingsDirty_ = true;
}

// 手动校时:在现有系统时钟基础上增减单个子段(年/月/日 或 时/分),
// 归一化后写回 RTC 并记录到 manualClockEpoch(NTP 不可用时重启仍可用)。
void UiModel::adjustClock(bool dateField, uint8_t sub, int direction) {
  if (!systemSettings_ || !direction)
    return;
  SystemSettings &s = *systemSettings_;
  const int d = direction > 0 ? 1 : -1;

  // 基准时间:优先当前系统时钟,未同步时回退到上次手动校时值。
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (s.manualClockEpoch >= 1700000000)
      now = static_cast<time_t>(s.manualClockEpoch);
    else
      now = 1767225600; // 2026-01-01 00:00:00 UTC+8,仅作首次编辑起点
  }
  struct tm tm;
  localtime_r(&now, &tm);

  if (dateField) {
    switch (sub) {
    case 0:
      tm.tm_year += d; // 年
      break;
    case 1: {
      int month = tm.tm_mon + d; // 月:1..12 循环
      tm.tm_mon = (month + 12) % 12;
      break;
    }
    default: {
      // 日:按当月天数循环,避免 2 月 31 日这类无效值。
      static const uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
      const int month = tm.tm_mon;
      int limit = days[month];
      if (month == 1 &&
          ((tm.tm_year + 1900) % 4 == 0 && (tm.tm_year + 1900) % 100 != 0 ||
           (tm.tm_year + 1900) % 400 == 0))
        limit = 29; // 闰年 2 月
      int day = tm.tm_mday + d;
      if (day < 1)
        day = limit;
      if (day > limit)
        day = 1;
      tm.tm_mday = day;
      break;
    }
    }
  } else {
    if (sub == 0) {
      int hour = tm.tm_hour + d; // 时:0..23 循环
      tm.tm_hour = (hour + 24) % 24;
    } else {
      int minute = tm.tm_min + d; // 分:0..59 循环
      tm.tm_min = (minute + 60) % 60;
    }
  }
  tm.tm_isdst = -1;
  const time_t adjusted = mktime(&tm);
  if (adjusted <= 0)
    return;
  struct timeval tv{adjusted, 0};
  settimeofday(&tv, nullptr);
  s.manualClockEpoch = static_cast<uint32_t>(adjusted);
  systemSettingsDirty_ = true;
}

// 统一的动作入口。先处理"系统设置页打开时"的专属交互(旋转改值/移动光标、
// 单击进入编辑、双击上一项、长按退出并按脏标记请求保存);不在该页时才进入
// 下方主界面/耗材页的动作分派。
void UiModel::apply(UiAction action, ChamberController &controller) {
  if (systemSettingsOpen_) {
    // 旋转:编辑态改值,浏览态移动光标。取模范围含 Version 等只读项,长按回绕。
    const uint8_t count = static_cast<uint8_t>(SystemSettingField::Count);
    if (action == UiAction::PreviousMaterial ||
        action == UiAction::NextMaterial) {
      const int d = action == UiAction::NextMaterial ? 1 : -1;
      if (systemSettingsEditing_)
        adjustSystemSetting(d, controller);
      else {
        systemSettingField_ = static_cast<SystemSettingField>(
            (static_cast<uint8_t>(systemSettingField_) + count + d) % count);
        systemSettingsSubField_ = 0; // 换项后子段回到第一段
      }
      return;
    }
    if (action == UiAction::EncoderDoubleClick) {
      systemSettingField_ = static_cast<SystemSettingField>(
          (static_cast<uint8_t>(systemSettingField_) + count - 1) % count);
      systemSettingsSubField_ = 0;
      return;
    }
    if (action == UiAction::EncoderClick) {
      if (systemSettingField_ == SystemSettingField::TouchCalibration) {
        if (Pin::HAS_TOUCH_PANEL)
          touchCalibrationRequested_ = true;
      } else if (systemSettingField_ == SystemSettingField::FactoryReset) {
        if (systemSettingsEditing_) {
          factoryResetRequested_ = true;
          systemSettingsEditing_ = false;
        } else {
          systemSettingsEditing_ = true; // 再次单击才执行，防止误触。
        }
      } else if (systemSettingField_ == SystemSettingField::Date ||
                 systemSettingField_ == SystemSettingField::Time) {
        // 日期(年/月/日)与时间(时/分)按子段编辑:单击进入并逐段推进,末段后退出。
        const uint8_t segments =
            systemSettingField_ == SystemSettingField::Date ? 3 : 2;
        if (!systemSettingsEditing_) {
          systemSettingsEditing_ = true;
          systemSettingsSubField_ = 0;
        } else if (++systemSettingsSubField_ >= segments) {
          systemSettingsEditing_ = false;
          systemSettingsSubField_ = 0;
        }
      } else {
        // 数值项单击切换"编辑中"状态,之后旋转即改值;开关项单击直接切换取值。
        // Version 等只读项走 adjustSystemSetting 后立即返回,单击无副作用。
        const bool numeric =
            systemSettingField_ == SystemSettingField::Brightness ||
            systemSettingField_ == SystemSettingField::ScreenSleep ||
            systemSettingField_ == SystemSettingField::PirStart ||
            systemSettingField_ == SystemSettingField::PirStop ||
            systemSettingField_ == SystemSettingField::HeaterCurrent ||
            systemSettingField_ == SystemSettingField::HeaterFan ||
            systemSettingField_ == SystemSettingField::HeaterProtection ||
            systemSettingField_ == SystemSettingField::DayStart ||
            systemSettingField_ == SystemSettingField::NightStart;
        if (numeric)
          systemSettingsEditing_ = !systemSettingsEditing_;
        else
          adjustSystemSetting(1, controller);
      }
      return;
    }
    if (action == UiAction::EncoderLongPress) {
      systemSettingsOpen_ = false;
      systemSettingsEditing_ = false;
      systemSettingsSubField_ = 0;
      systemSettingsSaveRequested_ = systemSettingsDirty_;
      systemSettingsDirty_ = false;
      return;
    }
  }
  // ---- 主界面 / 耗材参数页的动作分派 ----
  switch (action) {
  // 旋转:耗材页内调整当前字段;主界面则循环切换耗材并让控制器清 PID 历史。
  case UiAction::PreviousMaterial:
    if (materialSettingsOpen_) {
      materialSettingsDirty_ |=
          controller.adjustProfile(materialSettingField_, -1);
      break;
    }
    materialIndex_ =
        materialIndex_ == 0 ? MATERIAL_COUNT - 1 : materialIndex_ - 1;
    controller.setProfile(materialIndex_);
    break;
  case UiAction::NextMaterial:
    if (materialSettingsOpen_) {
      materialSettingsDirty_ |=
          controller.adjustProfile(materialSettingField_, 1);
      break;
    }
    materialIndex_ = (materialIndex_ + 1) % MATERIAL_COUNT;
    controller.setProfile(materialIndex_);
    break;
  // ---- 主界面三个自动开关 + 系统/预热/灯光/强排 ----
  // 每个开关都同时更新本地运行态并同步给控制器(控制器是执行的唯一真相)。
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
      // 关系统时联动取消预热,避免下次开机直接进入加热。
      settings_.preheat = false;
      controller.requestPreheat(false);
    }
    break;
  case UiAction::TogglePreheat:
    settings_.preheat = !settings_.preheat;
    if (settings_.preheat && !settings_.systemEnabled) {
      // 请求预热隐含开启系统,否则控制器会直接停在 Idle。
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
  // ---- 进入子页面与焦点移动 ----
  case UiAction::OpenMaterialSettings:
    materialSettingsOpen_ = true;
    materialSettingField_ = MaterialField::ChamberMin;
    break;
  case UiAction::OpenSystemSettings:
    systemSettingsOpen_ = true;
    systemSettingsEditing_ = false;
    break;
  case UiAction::FocusPrevious: {
    const uint8_t count = static_cast<uint8_t>(MainFocus::Count);
    mainFocus_ = static_cast<MainFocus>(
        (static_cast<uint8_t>(mainFocus_) + count - 1) % count);
    break;
  }
  case UiAction::FocusNext:
    mainFocus_ = static_cast<MainFocus>((static_cast<uint8_t>(mainFocus_) + 1) %
                                        static_cast<uint8_t>(MainFocus::Count));
    break;
  // ---- 编码器复合按键:双击/长按/短按在不同页面含义不同 ----
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
      // 耗材页:短按在字段间循环前进。
      materialSettingField_ = static_cast<MaterialField>(
          (static_cast<uint8_t>(materialSettingField_) + 1) %
          static_cast<uint8_t>(MaterialField::Count));
    } else {
      // 主界面:短按 = "激活当前焦点"。下表下标与 MainFocus 一一对应,
      // 把一次短按翻译成该焦点控件自己的动作(复用上面的分支)。
      static const UiAction actions[] = {
          UiAction::PreviousMaterial,      UiAction::OpenMaterialSettings,
          UiAction::NextMaterial,          UiAction::ToggleAutoExhaust,
          UiAction::ToggleAutoTemperature, UiAction::TogglePostPrintExhaust,
          UiAction::ToggleSystem,          UiAction::TogglePreheat,
          UiAction::ToggleLight,           UiAction::OpenSystemSettings};
      apply(actions[static_cast<uint8_t>(mainFocus_)], controller);
    }
    break;
  }
}

// 生成一帧只读快照供显示层渲染。原则:开关类状态一律回读控制器(执行端的
// 真相),本类的 settings_ 只保留 UI 临时态;clock/humidity/networkConnected
// 因依赖网络/AHT 层,由 main.cpp 在快照返回后补填,避免头文件循环依赖。
UiSnapshot UiModel::snapshot(const ChamberController &controller,
                             const Readings &r, const Outputs &o) const {
  const MaterialProfile &profile = controller.profile();
  UiSnapshot s{};
  s.material = profile.name;
  s.previousMaterial =
      MATERIALS[materialIndex_ == 0 ? MATERIAL_COUNT - 1 : materialIndex_ - 1]
          .name;
  s.nextMaterial = MATERIALS[(materialIndex_ + 1) % MATERIAL_COUNT].name;
  s.state = o.state;
  s.language = controller.language();
  // clock/networkConnected 由 main.cpp 填充:ui_model
  // 不依赖网络层,避免循环包含。
  strncpy(s.clock, "--:--:--", sizeof(s.clock) - 1);
  s.networkConnected = false;
  s.theme = systemSettings_ ? systemSettings_->theme : 1;
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
  s.mainFocus = mainFocus_;

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
  s.systemSettingsSubField = systemSettingsSubField_;
  if (systemSettings_)
    s.systemSettings = *systemSettings_;
  return s;
}
