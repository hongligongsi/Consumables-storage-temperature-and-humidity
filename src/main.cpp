#include "controller.h"
#include "ina226_sensor.h"
#include "network.h"
#include "pins.h"
#include "settings.h"
#include "tft_ui.h"
#include "ui_model.h"
#include <Adafruit_AHTX0.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Wire.h>

// --------------------------- 可按实际热端调整 ---------------------------
constexpr uint8_t PWM_BITS = 10;
constexpr uint32_t PWM_FREQ = 20000;
constexpr uint8_t HOT_PWM_CHANNEL = 0;
constexpr uint8_t AIR_FAN_PWM_CHANNEL = 1;
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 2;
constexpr uint8_t HOT_FAN_PWM_CHANNEL = 3;
// 初次烧录/引脚尚未经万用表核实前保持 false，避免未知 ADC 值使热端上电。
constexpr bool HEATER_ENABLED = false;

Adafruit_AHTX0 aht;
Adafruit_NeoPixel rgb(1, Pin::RGB, NEO_GRB + NEO_KHZ800);
bool ahtAvailable = false;
float chamberTemp = NAN, humidity = NAN;
float heaterBoardTemp = NAN; // NTC 参数确认后启用
float heaterCurrentA = NAN;  // INA226 电流比例/I2C 地址确认后启用
float supplyVoltage = NAN;
Ina226Sensor ina226;
bool inaAvailable = false;
uint32_t lastSensorOk = 0;
ChamberController controller;
UiModel ui;
SettingsStore settingsStore;
SystemSettings settings;
TftUi tftUi;
Outputs latestOutputs{0, 0, false, false, false, ChamberState::Idle};
uint32_t lastUiActivityMs = 0, buzzerOffMs = 0;
bool automaticLight = false;
ChamberState previousControlState = ChamberState::Idle;
bool touchCalibrationActive = false;
uint8_t touchCalibrationStep = 0;
uint16_t touchFirstX = 0, touchFirstY = 0;

uint16_t readTouchAxis(bool xAxis) {
  const int driveLow = xAxis ? Pin::TFT_XL : Pin::TFT_YD;
  const int driveHigh = xAxis ? Pin::TFT_XR : Pin::TFT_YU;
  const int sense = xAxis ? Pin::TFT_YD : Pin::TFT_XR;
  pinMode(driveLow, OUTPUT); digitalWrite(driveLow, LOW);
  pinMode(driveHigh, OUTPUT); digitalWrite(driveHigh, HIGH);
  // 下拉使未触摸时稳定回到 0，避免浮空 ADC 产生幽灵点击。
  pinMode(sense, INPUT_PULLDOWN);
  delayMicroseconds(30);
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; ++i) sum += analogRead(sense);
  pinMode(Pin::TFT_XL, INPUT); pinMode(Pin::TFT_XR, INPUT);
  pinMode(Pin::TFT_YD, INPUT); pinMode(Pin::TFT_YU, INPUT);
  return sum / 8;
}

void startBuzzer(uint16_t durationMs) {
  digitalWrite(Pin::BUZZER, HIGH);
  buzzerOffMs = millis() + durationMs;
}

void applyRuntimeSettings() {
  controller.setLanguage(settings.language);
  controller.setPirDelays(settings.pirStartSeconds * 1000UL,
                          settings.pirStopSeconds * 1000UL);
  controller.setHeaterLimits(settings.heaterMaxCurrentA,
                             settings.heaterBoardLimitC);
  controller.setHeaterFanPercent(settings.heaterFanPercent);
  lastUiActivityMs = millis();
}

// 10 kΩ NTC、10 kΩ 上拉、B=3950 的常用模型；原理图热端 NTC 为 100 kΩ
// 时请改这里。
float readNtcCelsius(int pin, float seriesOhm = 10000.0f,
                     float nominalOhm = 10000.0f, float beta = 3950.0f) {
  const int raw = analogRead(pin);
  if (raw <= 0 || raw >= 4095)
    return NAN;
  const float resistance = seriesOhm * raw / (4095.0f - raw);
  const float invT = 1.0f / 298.15f + logf(resistance / nominalOhm) / beta;
  const float celsius = 1.0f / invT - 273.15f;
  return isfinite(celsius) && celsius >= -40.0f && celsius <= 250.0f
             ? celsius
             : NAN;
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t previous = UINT32_MAX;
  const uint32_t color = (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(g) << 8) | b;
  if (color == previous)
    return;
  previous = color;
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

void updateStatusRgb(const Readings &in, const Outputs &out) {
  const bool slowOn = ((millis() / 450) & 1U) == 0;
  const bool fastOn = ((millis() / 180) & 1U) == 0;
  if (out.state == ChamberState::Fault)
    setRgb(fastOn ? 255 : 25, 0, 0);
  else if (out.heaterPercent > 0)
    setRgb(slowOn ? 255 : 80, slowOn ? 55 : 8, 0);
  else if (ui.settings().manualExhaust)
    setRgb(0, fastOn ? 180 : 25, fastOn ? 255 : 50);
  else if (out.state == ChamberState::Cooling)
    setRgb(slowOn ? 255 : 60, slowOn ? 90 : 15, 0);
  else if (out.heaterFan)
    setRgb(slowOn ? 170 : 45, 0, slowOn ? 220 : 55);
  else if (out.state == ChamberState::Printing)
    setRgb(0, 80, 180);
  else if (out.state == ChamberState::Preheat)
    setRgb(120, 20, 180);
  else if (in.pirMotion || out.state == ChamberState::Detecting)
    setRgb(slowOn ? 220 : 45, slowOn ? 150 : 25, 0);
  else
    setRgb(0, 70, 18);
}

void setHotPower(float percent) {
  if (!HEATER_ENABLED)
    percent = 0;
  percent = constrain(percent, 0.0f, 100.0f);
  ledcWrite(HOT_PWM_CHANNEL, lroundf(percent * ((1 << PWM_BITS) - 1) / 100.0f));
}

void emergencyStop(const char *reason) {
  setHotPower(0);
  ledcWrite(HOT_FAN_PWM_CHANNEL, (1 << PWM_BITS) - 1);
  digitalWrite(Pin::AIR_FAN_DC, HIGH);
  setRgb(255, 0, 0);
  Serial.printf("FAULT: %s\n", reason);
}

void readSensors() {
  // 加热模块的独立 NTC 两芯线接入 ADC_NTC；当前按 100 kΩ/B3950 预设。
  // PCB 网表或实物 NTC 型号不同，必须先在 pins.h/此处改正再开启加热。
  heaterBoardTemp = readNtcCelsius(Pin::ADC_NTC, 100000.0f, 100000.0f, 3950.0f);
  if (ahtAvailable) {
    sensors_event_t h, t;
    if (aht.getEvent(&h, &t) && isfinite(t.temperature) &&
        t.temperature >= -40.0f && t.temperature <= 100.0f &&
        isfinite(h.relative_humidity) && h.relative_humidity >= 0.0f &&
        h.relative_humidity <= 100.0f) {
      chamberTemp = t.temperature; // AHT20 位于仓内，作为自动仓温闭环传感器
      humidity = h.relative_humidity;
      lastSensorOk = millis();
    }
  }
  if (inaAvailable) {
    if (!ina226.readCurrentA(heaterCurrentA) || !isfinite(heaterCurrentA) ||
        fabsf(heaterCurrentA) > 32.0f)
      heaterCurrentA = NAN;
    if (!ina226.readBusVoltageV(supplyVoltage) || !isfinite(supplyVoltage) ||
        supplyVoltage < 0.0f || supplyVoltage > 40.0f)
      supplyVoltage = NAN;
  }
}

void updateThermalControl() {
  const Readings in{chamberTemp,
                    heaterBoardTemp,
                    heaterCurrentA,
                    supplyVoltage,
                    temperatureRead(),
                    digitalRead(Pin::PIR) == HIGH,
                    !isnan(chamberTemp) && millis() - lastSensorOk < 3000,
                    !isnan(heaterBoardTemp),
                    !isnan(heaterCurrentA)};
  Outputs out = controller.update(in, millis());
  if (out.state == ChamberState::Printing &&
      previousControlState != ChamberState::Printing) {
    if (settings.lightOnPrinting) automaticLight = true;
    if (settings.beepOnStart) startBuzzer(180);
  } else if (previousControlState == ChamberState::Printing &&
             out.state != ChamberState::Printing) {
    if (settings.lightOffAfterPrinting) automaticLight = false;
    if (settings.beepOnStop) startBuzzer(300);
  }
  previousControlState = out.state;
  out.light = automaticLight || ui.settings().light;
  out.boardFan = out.boardFan || out.light;
  if (ui.settings().manualExhaust)
    out.exhaustPercent = 100;
  latestOutputs = out;
  setHotPower(out.heaterPercent);
  ledcWrite(HOT_FAN_PWM_CHANNEL,
            out.heaterFan ? lroundf(settings.heaterFanPercent *
                                    ((1 << PWM_BITS) - 1) / 100.0f) : 0);
  digitalWrite(Pin::AIR_FAN_DC, out.exhaustPercent ? HIGH : LOW);
  digitalWrite(Pin::BOARD_FAN, out.boardFan ? HIGH : LOW);
  ledcWrite(AIR_FAN_PWM_CHANNEL,
            lroundf(out.exhaustPercent * ((1 << PWM_BITS) - 1) / 100.0f));
  digitalWrite(Pin::LED_ENABLE, out.light ? HIGH : LOW);
  if (Pin::HAS_STATUS_OUTPUTS) {
    digitalWrite(Pin::PRINTING_STATUS,
                 out.state == ChamberState::Printing ? HIGH : LOW);
    digitalWrite(Pin::HEATING_STATUS,
                 (HEATER_ENABLED && out.heaterPercent > 0) ? HIGH : LOW);
  }
  updateStatusRgb(in, out);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("ESP32-S3 N16R8 temperature-control board starting");

  pinMode(Pin::HOT_FAN, OUTPUT);
  pinMode(Pin::BUZZER, OUTPUT);
  digitalWrite(Pin::BUZZER, LOW);
  pinMode(Pin::AIR_FAN_DC, OUTPUT);
  pinMode(Pin::BOARD_FAN, OUTPUT);
  pinMode(Pin::PIR, INPUT);
  pinMode(Pin::LED_ENABLE, OUTPUT);
  digitalWrite(Pin::LED_ENABLE, LOW);
  pinMode(Pin::ENCODER_A, INPUT_PULLUP);
  pinMode(Pin::ENCODER_B, INPUT_PULLUP);
  pinMode(Pin::ENCODER_KEY, INPUT_PULLUP);
  pinMode(Pin::TFT_BACKLIGHT, OUTPUT);
  if (Pin::HAS_STATUS_OUTPUTS) {
    pinMode(Pin::PRINTING_STATUS, OUTPUT);
    pinMode(Pin::HEATING_STATUS, OUTPUT);
    digitalWrite(Pin::PRINTING_STATUS, LOW);
    digitalWrite(Pin::HEATING_STATUS, LOW);
  }
  analogReadResolution(12);
  analogSetPinAttenuation(Pin::ADC_NTC, ADC_11db);

  ledcSetup(HOT_PWM_CHANNEL, PWM_FREQ, PWM_BITS);
  ledcAttachPin(Pin::HOT_PWM, HOT_PWM_CHANNEL);
  ledcSetup(AIR_FAN_PWM_CHANNEL, 25000, PWM_BITS);
  ledcAttachPin(Pin::AIR_FAN_PWM, AIR_FAN_PWM_CHANNEL);
  ledcWrite(AIR_FAN_PWM_CHANNEL, (1 << PWM_BITS) - 1);
  ledcSetup(BACKLIGHT_PWM_CHANNEL, 5000, PWM_BITS);
  ledcAttachPin(Pin::TFT_BACKLIGHT, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, (1 << PWM_BITS) - 1);
  ledcSetup(HOT_FAN_PWM_CHANNEL, 25000, PWM_BITS);
  ledcAttachPin(Pin::HOT_FAN, HOT_FAN_PWM_CHANNEL);

  rgb.begin();
  setRgb(80, 80, 0);
  Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
  ahtAvailable = aht.begin(&Wire);
  inaAvailable = ina226.begin(Wire);
  Serial.printf("AHT20: %s\n", ahtAvailable ? "detected" : "not detected");
  Serial.printf("INA226 (0x40, 10mOhm): %s\n",
                inaAvailable ? "detected" : "not detected");
  lastSensorOk = millis();
  settings = settingsStore.load();
  settingsStore.loadMaterialProfiles();
  ui.bindSystemSettings(settings);
  controller.setLanguage(settings.language);
  controller.setProfile(0); // PLA；UI/串口设置可调用 setProfile() 选择其它耗材
  controller.setHeatLimit(
      100); // 以 MOSFET PWM 功率配置；电流闭环需 ADC 校准后启用
  controller.setPirDelays(settings.pirStartSeconds * 1000UL,
                          settings.pirStopSeconds * 1000UL);
  controller.setHeaterLimits(settings.heaterMaxCurrentA,
                             settings.heaterBoardLimitC);
  controller.setHeaterFanPercent(settings.heaterFanPercent);
  ledcWrite(BACKLIGHT_PWM_CHANNEL,
            lroundf(settings.brightness * ((1 << PWM_BITS) - 1) / 100.0f));
  controller.begin(millis());
  lastUiActivityMs = millis();
  tftUi.begin();
  network.begin(controller, ui,
                settings); // 联网:WiFi 配网门户 + Web/MQTT/NTP/OTA
}

void dispatchUiAction(UiAction action) {
  lastUiActivityMs = millis();
  if (touchCalibrationActive) {
    if (action == UiAction::EncoderLongPress) {
      touchCalibrationActive = false;
      Serial.println("Touch calibration cancelled");
    } else if (action == UiAction::EncoderClick) {
      const uint16_t x = readTouchAxis(true), y = readTouchAxis(false);
      if (touchCalibrationStep == 0) {
        touchFirstX = x; touchFirstY = y; touchCalibrationStep = 1;
      } else {
        // 保留左上与右下的原始方向，兼容 X/Y 反接的触摸屏。
        settings.touchXMin = touchFirstX;
        settings.touchXMax = x;
        settings.touchYMin = touchFirstY;
        settings.touchYMax = y;
        settings.touchCalibrated = abs(static_cast<int>(settings.touchXMax) -
                                       static_cast<int>(settings.touchXMin)) > 200 &&
                                   abs(static_cast<int>(settings.touchYMax) -
                                       static_cast<int>(settings.touchYMin)) > 200;
        settingsStore.save(settings);
        touchCalibrationActive = false;
        Serial.printf("Touch calibration: x=%u..%u y=%u..%u %s\n",
                      settings.touchXMin, settings.touchXMax, settings.touchYMin,
                      settings.touchYMax, settings.touchCalibrated ? "saved" : "invalid");
      }
    }
    return;
  }
  ui.apply(action, controller);
  applyRuntimeSettings();
  if (settings.keySound)
    startBuzzer(35);
  if (ui.takeProfileSaveRequest()) {
    const bool saved = settingsStore.saveMaterialProfiles();
    Serial.printf("Profiles: %s\n", saved ? "saved" : "save failed");
  }
  if (ui.takeSystemSettingsSaveRequest()) {
    const bool saved = settingsStore.save(settings);
    Serial.printf("Settings: %s\n", saved ? "saved" : "save failed");
  }
  if (ui.takeTouchCalibrationRequest()) {
    touchCalibrationActive = true;
    touchCalibrationStep = 0;
    Serial.println("Touch calibration: hold top-left, click EC11; then bottom-right, click EC11");
  }
  if (ui.takeFactoryResetRequest()) {
    const bool reset = settingsStore.reset();
    resetMaterialProfiles();
    settings = SystemSettings{};
    applyRuntimeSettings();
    Serial.printf("Factory reset: %s (registration keys preserved)\n",
                  reset ? "done" : "failed");
  }
}

bool screenIsSleeping(uint32_t now) {
  const bool keepAwake = settings.keepScreenOnPrinting &&
                         latestOutputs.state == ChamberState::Printing;
  return settings.screenSleepSeconds > 0 && !keepAwake &&
         now - lastUiActivityMs >= settings.screenSleepSeconds * 1000UL;
}

bool readTouchPoint(int16_t &screenX, int16_t &screenY) {
  const uint16_t rawX = readTouchAxis(true);
  delayMicroseconds(80);
  const uint16_t rawY = readTouchAxis(false);
  if (rawX < 80 || rawX > 4015 || rawY < 80 || rawY > 4015)
    return false;
  const int xSpan = static_cast<int>(settings.touchXMax) - settings.touchXMin;
  const int ySpan = static_cast<int>(settings.touchYMax) - settings.touchYMin;
  if (abs(xSpan) < 200 || abs(ySpan) < 200)
    return false;
  screenX = constrain(static_cast<int>(map(rawX, settings.touchXMin,
                                           settings.touchXMax, 28, 452)),
                      0, 479);
  screenY = constrain(static_cast<int>(map(rawY, settings.touchYMin,
                                           settings.touchYMax, 28, 292)),
                      0, 319);
  return true;
}

void pollTouchUi() {
  static uint32_t lastSampleMs = 0;
  static bool held = false;
  static uint8_t releaseSamples = 0;
  const uint32_t now = millis();
  if (!Pin::HAS_TOUCH_PANEL || now - lastSampleMs < 35 ||
      touchCalibrationActive)
    return;
  lastSampleMs = now;
  int16_t x = 0, y = 0;
  if (!readTouchPoint(x, y)) {
    if (++releaseSamples >= 2) held = false;
    return;
  }
  releaseSamples = 0;
  if (held)
    return;
  held = true;

  // 睡眠状态首次触摸只唤醒屏幕，避免误操作。
  if (screenIsSleeping(now)) {
    lastUiActivityMs = now;
    return;
  }
  if (ui.materialSettingsOpen() || ui.systemSettingsOpen())
    return; // 设置页继续由 EC11 编辑，避免触摸误改参数。

  UiAction action;
  bool actionable = true;
  if (y < 60 && x >= 58 && x <= 106)
    action = UiAction::PreviousMaterial;
  else if (y < 60 && x >= 184 && x <= 230)
    action = UiAction::NextMaterial;
  else if (y < 60 && x >= 108 && x <= 182)
    action = UiAction::OpenMaterialSettings;
  else if (x >= 132 && x <= 208 && y >= 62 && y < 120)
    action = UiAction::ToggleAutoExhaust;
  else if (x >= 132 && x <= 208 && y >= 120 && y < 180)
    action = UiAction::ToggleAutoTemperature;
  else if (x >= 132 && x <= 208 && y >= 180 && y < 244)
    action = UiAction::TogglePostPrintExhaust;
  else if (y >= 246) {
    const uint8_t button = min<uint8_t>(x / 95, 4);
    if (button == 1) action = UiAction::ToggleSystem;
    else if (button == 2) action = UiAction::TogglePreheat;
    else if (button == 3) action = UiAction::ToggleLight;
    else if (button == 4) action = UiAction::OpenSystemSettings;
    else actionable = false; // 第一格是 PIR 状态指示，不是开关。
  } else {
    actionable = false;
  }
  if (actionable)
    dispatchUiAction(action);
}

void pollConsoleUi() {
  // 临时调试入口，与主界面按钮一一对应；屏幕完成后由触摸/EC11 事件调用
  // ui.apply()。
  if (!Serial.available())
    return;
  const char command = Serial.read();
  UiAction action;
  switch (command) {
  case '[':
    action = UiAction::PreviousMaterial;
    break;
  case ']':
    action = UiAction::NextMaterial;
    break;
  case 'e':
    action = UiAction::ToggleAutoExhaust;
    break;
  case 't':
    action = UiAction::ToggleAutoTemperature;
    break;
  case 'p':
    action = UiAction::TogglePreheat;
    break;
  case 'l':
    action = UiAction::ToggleLight;
    break;
  case 's':
    action = UiAction::ToggleSystem;
    break;
  case 'x':
    action = UiAction::ToggleManualExhaust;
    break;
  case 'c':
    action = UiAction::EncoderClick;
    break;
  case 'h':
    action = UiAction::EncoderLongPress;
    break;
  case 'W': // 打印联网状态,便于调试
    Serial.printf("NET: state=%d ip=%s time=%s mqtt=%d\n", (int)network.state(),
                  network.ipString().c_str(), network.timeString().c_str(),
                  settings.mqttEnabled);
    return;
  default:
    return;
  }
  dispatchUiAction(action);
  Serial.printf("UI: material=%s system=%d preheat=%d light=%d\n",
                controller.profile().name, ui.settings().systemEnabled,
                ui.settings().preheat, ui.settings().light);
}

void pollEncoderUi() {
  static uint8_t previous = 0xff;
  static int8_t accumulator = 0;
  static bool rawKey = HIGH, stableKey = HIGH, longSent = false;
  static bool clickPending = false;
  static uint32_t keyChangedMs = 0, pressedMs = 0, clickDeadlineMs = 0;
  static const int8_t transitions[16] = {
      0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

  const uint32_t now = millis();
  const uint8_t ab = (digitalRead(Pin::ENCODER_A) << 1) |
                     digitalRead(Pin::ENCODER_B);
  if (previous == 0xff)
    previous = ab;
  if (ab != previous) {
    accumulator += transitions[(previous << 2) | ab];
    previous = ab;
    if (accumulator >= 4 || accumulator <= -4) {
      int direction = accumulator > 0 ? 1 : -1;
      accumulator = 0;
      if (settings.encoderReversed)
        direction = -direction;
      if (ui.materialSettingsOpen() || ui.systemSettingsOpen())
        dispatchUiAction(direction > 0 ? UiAction::NextMaterial
                                       : UiAction::PreviousMaterial);
      else
        dispatchUiAction(direction > 0 ? UiAction::FocusNext
                                       : UiAction::FocusPrevious);
    }
  }

  const bool key = digitalRead(Pin::ENCODER_KEY);
  if (key != rawKey) {
    rawKey = key;
    keyChangedMs = now;
  }
  if (key != stableKey && now - keyChangedMs >= 30) {
    stableKey = key;
    if (stableKey == LOW) {
      pressedMs = now;
      longSent = false;
    } else if (!longSent) {
      if (clickPending && (int32_t)(clickDeadlineMs - now) >= 0) {
        clickPending = false;
        dispatchUiAction(UiAction::EncoderDoubleClick);
      } else {
        clickPending = true;
        clickDeadlineMs = now + 350;
      }
    }
  }
  if (stableKey == LOW && !longSent && now - pressedMs >= 800) {
    longSent = true;
    clickPending = false;
    dispatchUiAction(UiAction::EncoderLongPress);
  }
  if (clickPending && (int32_t)(now - clickDeadlineMs) >= 0) {
    clickPending = false;
    dispatchUiAction(UiAction::EncoderClick);
  }
}

void loop() {
  network
      .loop(); // 联网轮询:WiFiManager/WebServer/MQTT/OTA/NTP(不阻塞 50ms PID)
  static uint32_t lastRead = 0, lastControl = 0, lastReport = 0;
  if (millis() - lastRead >= 500) {
    lastRead = millis();
    readSensors();
  }
  if (millis() - lastControl >= 50) {
    lastControl = millis();
    updateThermalControl();
  }
  pollConsoleUi();
  pollEncoderUi();
  pollTouchUi();
  if (buzzerOffMs && (int32_t)(millis() - buzzerOffMs) >= 0) {
    digitalWrite(Pin::BUZZER, LOW);
    buzzerOffMs = 0;
  }
  const bool sleeping = screenIsSleeping(millis());
  static int lastBacklight = -1;
  const int backlight = sleeping ? 0 : settings.brightness;
  if (backlight != lastBacklight) {
    ledcWrite(BACKLIGHT_PWM_CHANNEL,
              lroundf(backlight * ((1 << PWM_BITS) - 1) / 100.0f));
    lastBacklight = backlight;
  }
  if (millis() - lastReport >= 2000) {
    lastReport = millis();
    Serial.printf("chamber=%.1fC humidity=%.1f%%\n", chamberTemp, humidity);
  }
  static uint32_t lastScreen = 0;
  if (millis() - lastScreen >= 500) {
    lastScreen = millis();
    Readings in{chamberTemp,           heaterBoardTemp,
                heaterCurrentA,        supplyVoltage,
                temperatureRead(),     digitalRead(Pin::PIR) == HIGH,
                !isnan(chamberTemp),   !isnan(heaterBoardTemp),
                !isnan(heaterCurrentA)};
    UiSnapshot screen = ui.snapshot(controller, in, latestOutputs);
    screen.touchCalibrationActive = touchCalibrationActive;
    screen.touchCalibrationStep = touchCalibrationStep;
    screen.humidity = humidity;
    // 时钟与联网标记由网络层提供,ui_model 不反向依赖 network.h。
    const String clock = network.timeString();
    if (clock.length() > 0) {
      strncpy(screen.clock, clock.c_str(), sizeof(screen.clock) - 1);
      screen.clock[sizeof(screen.clock) - 1] = '\0';
    }
    screen.networkConnected = network.connected();
    network.updateReadings(in, latestOutputs,
                           humidity); // Web/MQTT 据此返回数据
    tftUi.render(screen);
  }
}
