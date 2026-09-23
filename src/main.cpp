// 固件入口与硬件接线层:setup() 完成全部外设初始化,loop() 按不同节拍调度
// 传感器读取(500ms)、热控运算(50ms)、UI 输入轮询与屏幕渲染,并把控制器
// 输出的 Outputs 逐路写到 GPIO/LEDC。业务状态机在 controller.cpp,
// 界面状态在 ui_model.cpp,联网在 network.cpp,本文件只做装配与驱动。
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
#include <sys/time.h>

// --------------------------- 可按实际热端调整 ---------------------------
constexpr uint8_t PWM_BITS = 10;             // LEDC 分辨率(10bit → 0..1023)
constexpr uint32_t PWM_FREQ = 20000;         // 发热板 PWM 20kHz(超出可闻音频)
constexpr uint8_t HOT_PWM_CHANNEL = 0;       // LEDC 通道:发热板
constexpr uint8_t AIR_FAN_PWM_CHANNEL = 1;   // LEDC 通道:4 线排风风扇
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 2; // LEDC 通道:TFT 背光
constexpr uint8_t HOT_FAN_PWM_CHANNEL = 3;   // LEDC 通道:加热风扇
// 加热总保险开关：
// false = 仅运行状态机和 PID 计算，强制 GPIO38 加热 PWM 为 0，GPIO47“加热中”
//         状态输出也保持低电平；用于首次烧录和硬件调试，防止误加热。
// true  = 允许 PID 驱动 GPIO38；当实际加热 PWM > 0 时，GPIO47 输出 3.3 V
// 高电平。 只有确认 GPIO38/MOSFET 有效电平、NTC 型号与参数、INA226
// 电流方向及加热板接线 全部正确后，才可以改为
// true。过温或传感器故障仍会立即停止加热并拉低 GPIO47。
constexpr bool HEATER_ENABLED = false;

// ---- 全局外设与共享状态(各调度函数之间通过这些文件级变量传递数据) ----
Adafruit_AHTX0 aht; // 仓内温湿度传感器(I²C)
Adafruit_NeoPixel rgb(1, Pin::RGB, NEO_GRB + NEO_KHZ800); // 状态 RGB 灯
bool ahtAvailable = false;                                // AHT20 是否在线
float chamberTemp = NAN, humidity = NAN; // AHT20 仓温(℃)与湿度(%RH)
float heaterBoardTemp = NAN;             // NTC 参数确认后启用
float heaterCurrentA = NAN;              // INA226 电流比例/I2C 地址确认后启用
float supplyVoltage = NAN;               // INA226 总线电压(V)
Ina226Sensor ina226;
bool inaAvailable = false;    // INA226 是否在线
uint32_t lastSensorOk = 0;    // AHT20 最近一次成功读数时刻
ChamberController controller; // 热控状态机 + 双 PID
UiModel ui;                   // UI 状态机
SettingsStore settingsStore;  // NVS 持久化
SystemSettings settings;      // 当前系统设置(内存中的唯一份)
TftUi tftUi;                  // 显示层
Outputs latestOutputs{0,     0,     false,
                      false, false, ChamberState::Idle}; // 最近一次执行器输出
uint32_t lastUiActivityMs = 0, buzzerOffMs = 0; // 最近操作时刻 / 蜂鸣器停止时刻
bool automaticLight = false; // 打印联动自动开关的灯(区别于手动灯)
ChamberState previousControlState = ChamberState::Idle; // 上周期状态(边沿检测)
bool touchCalibrationActive = false;                    // 触摸校准流程进行中
uint8_t touchCalibrationStep = 0;                       // 校准第几步(0=第一点)
uint16_t touchFirstX = 0, touchFirstY = 0; // 校准第一点(左上)原始 ADC 值

// 读电阻触摸屏的一个轴(原始 ADC 值)。四线电阻屏的做法:给该轴的两根
// 电极加高低电平,在另一方向的电极上用 ADC 测分压;8 次平均降噪。
uint16_t readTouchAxis(bool xAxis) {
  const int driveLow = xAxis ? Pin::TFT_XL : Pin::TFT_YD;
  const int driveHigh = xAxis ? Pin::TFT_XR : Pin::TFT_YU;
  const int sense = xAxis ? Pin::TFT_YD : Pin::TFT_XR;
  pinMode(driveLow, OUTPUT);
  digitalWrite(driveLow, LOW);
  pinMode(driveHigh, OUTPUT);
  digitalWrite(driveHigh, HIGH);
  // 下拉使未触摸时稳定回到 0，避免浮空 ADC 产生幽灵点击。
  pinMode(sense, INPUT_PULLDOWN);
  delayMicroseconds(30);
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; ++i)
    sum += analogRead(sense);
  pinMode(Pin::TFT_XL, INPUT);
  pinMode(Pin::TFT_XR, INPUT);
  pinMode(Pin::TFT_YD, INPUT);
  pinMode(Pin::TFT_YU, INPUT);
  return sum / 8;
}

// 鸣蜂鸣器并安排在 durationMs 后由 loop() 关闭(非阻塞,不占用控制周期)。
void startBuzzer(uint16_t durationMs) {
  digitalWrite(Pin::BUZZER, HIGH);
  buzzerOffMs = millis() + durationMs;
}

// 2023-11-14 之后的 epoch 视为已同步;未同步时 time() 返回 1970 年起的小值。
bool clockValid(time_t t) { return t >= 1700000000; }

// 取当前时刻:NTP 已同步用系统时钟,否则回退上次手动校时值。
time_t currentEpoch() {
  const time_t now = time(nullptr);
  if (clockValid(now))
    return now;
  return settings.manualClockEpoch >= 1700000000
             ? static_cast<time_t>(settings.manualClockEpoch)
             : 0;
}

// "YYYY-MM-DD HH:MM:SS";无有效时间时退化为占位串。
void formatClock(char *out, size_t size) {
  const time_t now = currentEpoch();
  if (!clockValid(now)) {
    strlcpy(out, "----/--/-- --:--:--", size);
    return;
  }
  struct tm tm{};
  localtime_r(&now, &tm);
  snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// 生效配色:theme 0/1 直接采用,theme==2(自动)按时刻落在日间区间与否判定。
uint8_t resolveTheme() {
  if (settings.theme != 2)
    return settings.theme;
  const time_t now = currentEpoch();
  if (!clockValid(now))
    return 1; // 无有效时间时按夜间渲染,避免白天误亮/夜间误暗
  struct tm tm{};
  localtime_r(&now, &tm);
  const uint16_t minutes = static_cast<uint16_t>(tm.tm_hour * 60 + tm.tm_min);
  const uint16_t day = settings.dayStartMinutes;
  const uint16_t night = settings.nightStartMinutes;
  // 日间区间 [dayStart, nightStart);跨零点时取补集。
  const bool isDay = day <= night ? (minutes >= day && minutes < night)
                                  : (minutes >= day || minutes < night);
  return isDay ? 0 : 1;
}

// 把系统设置里"影响热控行为"的项同步给控制器(语言/PIR 延时/加热限制/
// 热风风速),并刷新用户活动计时。恢复出厂与每次 UI 操作后都会调用。
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
  return isfinite(celsius) && celsius >= -40.0f && celsius <= 250.0f ? celsius
                                                                     : NAN;
}

// 设置状态灯颜色;与上次相同时跳过 show(),省一次单线协议传输。
void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t previous = UINT32_MAX;
  const uint32_t color =
      (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  if (color == previous)
    return;
  previous = color;
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

// 按状态机/执行器输出选择状态灯颜色与闪烁节奏:故障快闪红、加热慢闪橙、
// 手动强排快闪青、排气慢闪琥珀、热风扇转紫色、打印蓝、预热紫、人感橙、
// 空闲绿色。优先级从上到下,故障最高。
void updateStatusRgb(const Readings &in, const Outputs &out) {
  const bool slowOn = ((millis() / 450) & 1U) == 0; // 慢闪节拍(约 2.2Hz 切换)
  const bool fastOn = ((millis() / 180) & 1U) == 0; // 快闪节拍(故障/强排)
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

// 发热板功率输出:百分比换算成 LEDC 占空比。HEATER_ENABLED=false 时
// 无条件输出 0,是比状态机更外层的硬件保险。
void setHotPower(float percent) {
  if (!HEATER_ENABLED)
    percent = 0;
  percent = constrain(percent, 0.0f, 100.0f);
  ledcWrite(HOT_PWM_CHANNEL, lroundf(percent * ((1 << PWM_BITS) - 1) / 100.0f));
}

// 紧急停机:停加热、热风/排风全速、红灯、打印原因。当前预留,供严重故障调用。
void emergencyStop(const char *reason) {
  setHotPower(0);
  ledcWrite(HOT_FAN_PWM_CHANNEL, (1 << PWM_BITS) - 1);
  digitalWrite(Pin::AIR_FAN_DC, HIGH);
  setRgb(255, 0, 0);
  Serial.printf("FAULT: %s\n", reason);
}

// 每 500ms 读一路传感器并做量程合理性检查,越界值一律置 NAN,
// 控制器据此把对应传感器判为无效(NAN 不会参与加热决策)。
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

// 每 50ms 执行的热控节拍:组装 Readings → 跑控制器状态机 → 处理打印开始/
// 结束的灯光蜂鸣联动 → 叠加手动灯/手动强排等 UI 覆盖 → 把 Outputs 写硬件。
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
    if (settings.lightOnPrinting)
      automaticLight = true;
    if (settings.beepOnStart)
      startBuzzer(180);
  } else if (previousControlState == ChamberState::Printing &&
             out.state != ChamberState::Printing) {
    if (settings.lightOffAfterPrinting)
      automaticLight = false;
    if (settings.beepOnStop)
      startBuzzer(300);
  }
  previousControlState = out.state;
  out.light = automaticLight || ui.settings().light;
  out.boardFan = out.boardFan || out.light;
  if (ui.settings().manualExhaust)
    out.exhaustPercent = 100;
  latestOutputs = out;
  setHotPower(out.heaterPercent);
  ledcWrite(HOT_FAN_PWM_CHANNEL, out.heaterFan
                                     ? lroundf(settings.heaterFanPercent *
                                               ((1 << PWM_BITS) - 1) / 100.0f)
                                     : 0);
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

// 开机一次性初始化,顺序:GPIO → ADC → LEDC 四路 PWM → RGB/I²C 传感器 →
// NVS 设置 → 控制器初值 → TFT/UI → 联网子系统。
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("ESP32-S3 N16R8 temperature-control board starting");

  // ---- GPIO 方向与安全初值(执行器默认全部断开) ----
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
  // ---- ADC:12bit 分辨率,NTC 采样脚 11dB 衰减(量程约 0-3.3V) ----
  analogReadResolution(12);
  analogSetPinAttenuation(Pin::ADC_NTC, ADC_11db);

  // ---- LEDC 四路 PWM:发热板/排风/背光/热风风扇 ----
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

  // ---- RGB 状态灯与 I²C 总线(AHT20 + INA226 同址不同设备) ----
  rgb.begin();
  setRgb(80, 80, 0);
  Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
  ahtAvailable = aht.begin(&Wire);
  inaAvailable = ina226.begin(Wire);
  Serial.printf("AHT20: %s\n", ahtAvailable ? "detected" : "not detected");
  Serial.printf("INA226 (0x40, 10mOhm): %s\n",
                inaAvailable ? "detected" : "not detected");
  lastSensorOk = millis();
  // ---- 从 NVS 恢复系统设置与耗材预设,并让 UI 绑定这份设置 ----
  settings = settingsStore.load();
  settingsStore.loadMaterialProfiles();
  ui.bindSystemSettings(settings);
  // ---- 控制器初始参数(默认 PLA、用户保存的加热/PIR 参数) ----
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
  // ---- 显示层(含渲染任务)与联网子系统,放最后启动 ----
  tftUi.begin();
  network.begin(controller, ui,
                settings); // 联网:WiFi 配网门户 + Web/MQTT/NTP/OTA
}

// 所有输入来源(编码器/触摸/串口)的统一出口。校准流程中先截获按键采集
// 两点;否则交给 UiModel,再处理它产生的边沿请求(保存耗材/保存设置/
// 进入校准/恢复出厂),按键音也在这里统一播放。
void dispatchUiAction(UiAction action) {
  lastUiActivityMs = millis();
  if (touchCalibrationActive) {
    if (action == UiAction::EncoderLongPress) {
      touchCalibrationActive = false;
      Serial.println("Touch calibration cancelled");
    } else if (action == UiAction::EncoderClick) {
      const uint16_t x = readTouchAxis(true), y = readTouchAxis(false);
      if (touchCalibrationStep == 0) {
        touchFirstX = x;
        touchFirstY = y;
        touchCalibrationStep = 1;
      } else {
        // 保留左上与右下的原始方向，兼容 X/Y 反接的触摸屏。
        settings.touchXMin = touchFirstX;
        settings.touchXMax = x;
        settings.touchYMin = touchFirstY;
        settings.touchYMax = y;
        settings.touchCalibrated =
            abs(static_cast<int>(settings.touchXMax) -
                static_cast<int>(settings.touchXMin)) > 200 &&
            abs(static_cast<int>(settings.touchYMax) -
                static_cast<int>(settings.touchYMin)) > 200;
        settingsStore.save(settings);
        touchCalibrationActive = false;
        Serial.printf("Touch calibration: x=%u..%u y=%u..%u %s\n",
                      settings.touchXMin, settings.touchXMax,
                      settings.touchYMin, settings.touchYMax,
                      settings.touchCalibrated ? "saved" : "invalid");
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
    // 设置页刚改过的联网开关立即作用于网络子系统,无需重启。
    network.onSettingsChanged(settings);
    Serial.printf("Settings: %s\n", saved ? "saved" : "save failed");
  }
  if (ui.takeTouchCalibrationRequest()) {
    touchCalibrationActive = true;
    touchCalibrationStep = 0;
    Serial.println("Touch calibration: hold top-left, click EC11; then "
                   "bottom-right, click EC11");
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

// 屏幕是否应熄灭:设置了休眠秒数、当前没在打印(或未开启打印常亮)、
// 且距上次 UI 活动超过休眠时长。
bool screenIsSleeping(uint32_t now) {
  const bool keepAwake = settings.keepScreenOnPrinting &&
                         latestOutputs.state == ChamberState::Printing;
  return settings.screenSleepSeconds > 0 && !keepAwake &&
         now - lastUiActivityMs >= settings.screenSleepSeconds * 1000UL;
}

// 采样一次触摸并换算成屏幕像素坐标(480x320 横屏,留 28px 边距)。
// 未触摸、超量程或校准跨度太小都返回 false。
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

// 触摸屏轮询(约 28fps):按下坐标映射到主屏各热区并只在"按下瞬间"触发
// 一次动作,连续两次无有效采样才认为松手;设置页禁用触摸以防误改。
void pollTouchUi() {
  static uint32_t lastSampleMs = 0;  // 上次采样时刻(35ms 限频)
  static bool held = false;          // 当前手指仍按住,去重连续触发
  static uint8_t releaseSamples = 0; // 连续无触点采样计数(消抖)
  const uint32_t now = millis();
  if (!Pin::HAS_TOUCH_PANEL || now - lastSampleMs < 35 ||
      touchCalibrationActive)
    return;
  lastSampleMs = now;
  int16_t x = 0, y = 0;
  if (!readTouchPoint(x, y)) {
    if (++releaseSamples >= 2)
      held = false;
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
    if (button == 1)
      action = UiAction::ToggleSystem;
    else if (button == 2)
      action = UiAction::TogglePreheat;
    else if (button == 3)
      action = UiAction::ToggleLight;
    else if (button == 4)
      action = UiAction::OpenSystemSettings;
    else
      actionable = false; // 第一格是 PIR 状态指示，不是开关。
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

// EC11 旋转编码器轮询:用 4 状态转移表对 A/B 相解码,累计满 4 个微步算
// 一格(手感一个定位);按键做 30ms 消抖,并识别单击/双击(350ms 窗)/
// 长按(800ms)。设置页中旋转复用为改选耗材,主屏则是焦点上下移动。
void pollEncoderUi() {
  static uint8_t previous = 0xff; // 上次 AB 相位组合
  static int8_t accumulator = 0;  // 微步累加器(±4 输出一格)
  static bool rawKey = HIGH, stableKey = HIGH,
              longSent = false;     // 原始电平/消抖电平/长按已发
  static bool clickPending = false; // 已按一次,等双击窗口结束
  static uint32_t keyChangedMs = 0, pressedMs = 0,
                  clickDeadlineMs = 0; // 抖动时刻/按下时刻/单击判定时刻
  // AB 四相(00/01/10/11)间的合法转移增量表,非法跳变记 0(抗抖动)。
  static const int8_t transitions[16] = {0,  -1, 1, 0, 1, 0, 0,  -1,
                                         -1, 0,  0, 1, 0, 1, -1, 0};

  const uint32_t now = millis();
  const uint8_t ab =
      (digitalRead(Pin::ENCODER_A) << 1) | digitalRead(Pin::ENCODER_B);
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

// 主循环,所有任务都是非阻塞节拍调度:联网每轮先服务,传感器 500ms、
// 热控 50ms、界面快照/渲染 500ms、串口上报 2000ms,输入三路每轮都查。
void loop() {
  network
      .loop(); // 联网轮询:WiFiManager/WebServer/MQTT/OTA/NTP(不阻塞 50ms PID)
  static uint32_t lastRead = 0, lastControl = 0, lastReport = 0;
  if (millis() - lastRead >= 500) { // 500ms:读传感器
    lastRead = millis();
    readSensors();
  }
  if (millis() - lastControl >= 50) { // 50ms:热控状态机 + PID + 输出
    lastControl = millis();
    updateThermalControl();
  }
  pollConsoleUi(); // 串口调试命令
  pollEncoderUi(); // EC11 旋转/按键
  pollTouchUi();   // 电阻触摸热区
  if (buzzerOffMs && (int32_t)(millis() - buzzerOffMs) >= 0) {
    digitalWrite(Pin::BUZZER, LOW); // 到点关蜂鸣器
    buzzerOffMs = 0;
  }
  const bool sleeping = screenIsSleeping(millis());
  static int lastBacklight = -1;
  const int backlight = sleeping ? 0 : settings.brightness;
  if (backlight != lastBacklight) { // 亮度/休眠状态变化才重写 PWM
    ledcWrite(BACKLIGHT_PWM_CHANNEL,
              lroundf(backlight * ((1 << PWM_BITS) - 1) / 100.0f));
    lastBacklight = backlight;
  }
  if (millis() - lastReport >= 2000) { // 2s:串口健康上报
    lastReport = millis();
    Serial.printf("chamber=%.1fC humidity=%.1f%%\n", chamberTemp, humidity);
  }
  static uint32_t lastScreen = 0;
  if (millis() - lastScreen >= 500) { // 500ms:生成界面快照并渲染
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
    // 日期时间与生效配色由 main.cpp 填充:ui_model 不反向依赖 network.h。
    // 时钟优先 NTP,断网/未同步时回退上次手动校时值(main.cpp 的 currentEpoch)。
    formatClock(screen.clock, sizeof(screen.clock));
    screen.networkConnected = network.connected();
    screen.theme = resolveTheme();
    network.updateReadings(in, latestOutputs,
                           humidity); // Web/MQTT 据此返回数据
    tftUi.render(screen);
  }
}
