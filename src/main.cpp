// 固件入口与硬件接线层:setup() 完成全部外设初始化,loop() 按不同节拍调度
// 传感器读取(500ms)、热控运算(50ms)、UI 输入轮询与屏幕渲染,并把控制器
// 输出的 Outputs 逐路写到 GPIO/LEDC。业务状态机在 controller.cpp,
// 界面状态在 ui_model.cpp,联网在 network.cpp,本文件只做装配与驱动。
#include "controller.h"
#include "ina226_sensor.h"
#include "network.h"
#include "pins.h"
#include "pure_logic.h"
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
// false = 仅运行状态机和 PID 计算，强制 GPIO40 加热 PWM 为 0，GPIO47“加热中”
//         状态输出也保持低电平；用于首次烧录和硬件调试，防止误加热。
// true  = 允许 PID 驱动 GPIO40；当实际加热 PWM > 0 时，GPIO47 输出 3.3 V
// 高电平。 只有确认 GPIO40/MOSFET 有效电平、NTC 型号与参数、INA226
// 电流方向及加热板接线 全部正确后，才可以改为
// true。过温或传感器故障仍会立即停止加热并拉低 GPIO47。
constexpr bool HEATER_ENABLED = false;

// ---- 全局外设与共享状态(各调度函数之间通过这些文件级变量传递数据) ----
Adafruit_AHTX0 aht; // 仓内温湿度传感器(I²C)
Adafruit_NeoPixel rgb(4, Pin::RGB, NEO_GRB + NEO_KHZ800); // 状态 RGB 灯带
                                                          // (GPIO18 单线 DIN 串 4 颗,同显一色)
bool ahtAvailable = false;                                // AHT20 是否在线
float chamberTemp = NAN, humidity = NAN; // AHT20 仓温(℃)与湿度(%RH)
float heaterBoardTemp = NAN;             // NTC 参数确认后启用
float heaterCurrentA = NAN;              // INA226 电流比例/I2C 地址确认后启用
float supplyVoltage = NAN;               // INA226 总线电压(V)
Ina226Sensor ina226;
bool inaAvailable = false;    // INA226 是否在线
uint32_t lastSensorOk = 0;    // AHT20 最近一次成功读数时刻
// AHT20 读数的保持窗口:读取失败时 chamberTemp/humidity 会保留上次成功的值,
// 用这段时间顶住偶发 I²C 失败,避免加热被单次读失败抖断。窗口外一律按无效
// 处理(见 ahtFresh())。
constexpr uint32_t AHT_HOLD_MS = 3000;
ChamberController controller; // 热控状态机 + 双 PID
UiModel ui;                   // UI 状态机
SettingsStore settingsStore;  // NVS 持久化
SystemSettings settings;      // 当前系统设置(内存中的唯一份)
// NVS 故障记忆:最近一次故障的档案。锁故障时写、解除时只清 latched,
// 断电重启后热类故障据此恢复 Fault 锁定(见 setup 与 updateThermalControl)。
FaultRecord faultRecord;
// 上一个热控周期看到的故障码,用于检测 锁定/解除 的边沿;setup 恢复锁定后
// 会先与 controller.faultCode() 对齐,避免把"恢复"误记成一次新故障。
FaultCode lastSeenFault = FaultCode::None;
TftUi tftUi;                  // 显示层
// 最近一次执行器输出;末尾 fault 显式写成 None —— 漏写会被隐式零初始化,
// 值恰好也对,但那样等于把正确性押在"枚举 0 就是 None"上。
Outputs latestOutputs{0,      0,     false, false, false, ChamberState::Idle,
                      FaultCode::None};
uint32_t lastUiActivityMs = 0, buzzerOffMs = 0; // 最近操作时刻 / 蜂鸣器停止时刻
bool automaticLight = false; // 打印联动自动开关的灯(区别于手动灯)
ChamberState previousControlState = ChamberState::Idle; // 上周期状态(边沿检测)
bool touchCalibrationActive = false;                    // 触摸校准流程进行中
uint8_t touchCalibrationStep = 0;                       // 校准第几步(0=第一点)
bool touchPanelPresent = false; // 运行时探测到的触摸膜存在与否(setup 时判定)
bool detectTouchPanel(); // 前置声明:定义在 readTouchPoint 一节,setup 要用
// ---- 注册码流程(模态,与触摸校准同构:输入在 dispatchUiAction 截获) ----
bool registrationActive = false; // 注册页接管整屏中
uint8_t regPos = 0;              // 当前输入位(0..7)
char regBuf[9] = "00000000";     // 8 位候选码,初值全 0
uint8_t regResult = 0;           // 0=输入中 1=校验通过 2=校验失败
// 注册态缓存:-1=未比对 0=未注册 1=已注册;注册成功时置 1。
int8_t registeredCache = -1;
// 注册流程两个助手定义在 dispatchUiAction 之前,这里先声明给 setup 用。
bool registrationValid();
void startRegistration();
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

// 生效配色:theme 0/1(默认/IOS)为固定单套配色直接采用;
// theme==2(蓝白)按时刻落在日间区间与否,解析为 2=蓝白·日间 / 3=蓝白·夜间。
// 判定规则本体在 pure_logic.h,本函数只负责把"现在几点"喂给它。
uint8_t resolveTheme() {
  const time_t now = currentEpoch();
  const bool valid = clockValid(now);
  uint16_t minutes = 0;
  if (valid) {
    struct tm tm{};
    localtime_r(&now, &tm);
    minutes = static_cast<uint16_t>(tm.tm_hour * 60 + tm.tm_min);
  }
  return pure::effectiveTheme(settings.theme, valid, minutes,
                              settings.dayStartMinutes,
                              settings.nightStartMinutes);
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
// 「RGB最大亮度」在这里做全局缩放:对四颗统一生效,且缩放进缓存比对,
// 设置一改动即使颜色不变也会重新发一帧。颜色值直接读 settings(内存唯一份),
// 不用像背光那样走 applyRuntimeSettings 二次下发。
void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t k = settings.rgbMaxBrightness; // 1-100,settings 层已钳制
  r = static_cast<uint8_t>((static_cast<uint16_t>(r) * k) / 100);
  g = static_cast<uint8_t>((static_cast<uint16_t>(g) * k) / 100);
  b = static_cast<uint8_t>((static_cast<uint16_t>(b) * k) / 100);
  static uint32_t previous = UINT32_MAX;
  const uint32_t color =
      (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  if (color == previous)
    return;
  previous = color;
  for (uint8_t i = 0; i < rgb.numPixels(); ++i)
    rgb.setPixelColor(i, rgb.Color(r, g, b));
  rgb.show();
}

// 屏幕休眠判定,定义在本文件后面(loop 之前),这里先声明。
bool screenIsSleeping(uint32_t now);

// 按状态机/执行器输出选择状态灯颜色与闪烁节奏:故障快闪红、加热慢闪橙、
// 手动强排快闪青、排气慢闪琥珀、热风扇转紫色、打印蓝、预热紫、人感橙、
// 正常(待机)状态为绿色呼吸灯。优先级从上到下,故障最高。
void updateStatusRgb(const Readings &in, const Outputs &out) {
  // 「RGB跟随屏幕休眠」:熄屏时灯也熄灭,唤醒恢复。与背光共用同一个判定,
  // setRgb 的颜色缓存保证熄屏帧只发送一次,唤醒时颜色变化自然重发。
  if (settings.rgbFollowScreenSleep && screenIsSleeping(millis())) {
    setRgb(0, 0, 0);
    return;
  }
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
  else {
    // 正常(待机)状态:绿色呼吸灯。3s 一个三角波周期,亮度在 15%~100%
    // 之间往返,渐亮渐暗;每 50ms 控制节拍刷新一帧,颜色缓存不会拦截。
    const uint16_t phase = millis() % 3000;
    const uint16_t tri = phase < 1500 ? phase : 3000 - phase; // 0..1500
    const uint8_t scale = 38 + (uint32_t)tri * (255 - 38) / 1500; // ≈15%..100%
    setRgb(0, (70 * scale) / 255, (18 * scale) / 255);
  }
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
  // 加热模块的独立 NTC 两芯线接入 ADC_NTC(GPIO3, ADC1_CH2)，型号 ZX-NTC1.25-P2ZZ。
  // 原理图分压上臂 R14 = 10 kΩ，故 seriesOhm = nominalOhm = 10000.0f。
  // 实物 NTC 阻值/β 不同时，以万用表实测 25 ℃ 阻值为准改正后再开启加热。
  heaterBoardTemp = readNtcCelsius(Pin::ADC_NTC, 10000.0f, 10000.0f, 3950.0f);
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

// AHT20 读数是否还算"新鲜"。读取失败时 chamberTemp/humidity 保留上次成功的
// 值,用 AHT_HOLD_MS 顶住偶发 I²C 失败(加热不至于因为单次读失败就断);超出
// 窗口即视为无效。
// 热控与界面快照**必须共用这一个判据**:若界面那边只看 !isnan(),AHT20 挂掉后
// 会一直拿旧值当"当前温度"显示,于是出现"故障页写着 AHT20 掉线、那一行的实测
// 值却还是温度、指示灯还是绿的"这种自相矛盾。
bool ahtFresh() {
  return !isnan(chamberTemp) && millis() - lastSensorOk < AHT_HOLD_MS;
}

// 每 50ms 执行的热控节拍:组装 Readings → 跑控制器状态机 → 处理打印开始/
// 结束的灯光蜂鸣联动 → 叠加手动灯/手动强排等 UI 覆盖 → 把 Outputs 写硬件。
void updateThermalControl() {
  const bool ahtOk = ahtFresh();
  const Readings in{ahtOk ? chamberTemp : NAN,
                    heaterBoardTemp,
                    heaterCurrentA,
                    supplyVoltage,
                    temperatureRead(),
                    digitalRead(Pin::PIR) == HIGH,
                    ahtOk,
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
  // ---- NVS 故障记忆:锁定/解除的边沿落盘 ----
  // 锁定:记码+置 latched+累计次数+时刻;解除(长按复位/关系统)只清
  // latched,码与次数留作排障历史,REST /api/state 的 lastFault 字段可查。
  // NVS 写失败只打日志不重试 —— 记忆是安全增强而非前提,别让它拖住热控。
  if (out.fault != lastSeenFault) {
    if (out.fault != FaultCode::None) {
      faultRecord.code = static_cast<uint8_t>(out.fault);
      faultRecord.latched = true;
      faultRecord.count =
          faultRecord.count < 0xffff ? faultRecord.count + 1 : 0xffff;
      faultRecord.epoch = static_cast<uint32_t>(currentEpoch());
      const bool saved = settingsStore.saveFaultRecord(faultRecord);
      Serial.printf("[FAULT] F-%02u latched, NVS %s (total %u)\n",
                    (unsigned)out.fault, saved ? "saved" : "SAVE FAILED",
                    (unsigned)faultRecord.count);
    } else {
      faultRecord.latched = false;
      const bool saved = settingsStore.saveFaultRecord(faultRecord);
      Serial.printf("[FAULT] latch released, NVS %s (history kept)\n",
                    saved ? "saved" : "SAVE FAILED");
    }
    lastSeenFault = out.fault;
  }
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

  // 触摸膜运行时探测:编译期开了触摸但实装非触摸屏时,探测不到电阻膜,
  // 触摸轮询与校准入口整路关闭,杜绝浮空线的幽灵触摸。
  if (Pin::HAS_TOUCH_PANEL) {
    touchPanelPresent = detectTouchPanel();
    Serial.printf("Touch panel: %s\n",
                  touchPanelPresent ? "detected" : "NOT detected, touch disabled");
  }

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
  // ---- NVS 故障记忆:热类故障断电重启后恢复锁定,防止拔电绕过保护 ----
  faultRecord = settingsStore.loadFaultRecord();
  if (faultRecord.latched && faultRecord.code != 0) {
    const FaultCode code = static_cast<FaultCode>(faultRecord.code);
    if (ChamberController::persistsAcrossReboot(code)) {
      controller.restoreFault(code);
      Serial.printf("[FAULT] F-%02u lock restored after reboot "
                    "(total %u latches)\n",
                    (unsigned)faultRecord.code, (unsigned)faultRecord.count);
    } else {
      // 传感器掉线类不恢复:开机本来就会重新检测,锁定只留历史记录。
      faultRecord.latched = false;
      settingsStore.saveFaultRecord(faultRecord);
    }
  }
  // 对齐边沿基准:若上面恢复了锁定,首个热控周期不再当作"新故障"计数。
  lastSeenFault = controller.faultCode();
  lastUiActivityMs = millis();
  // ---- 显示层(含渲染任务)与联网子系统,放最后启动 ----
  tftUi.begin();
  network.begin(controller, ui,
                settings,
                faultRecord); // 联网:WiFi 配网门户 + Web/MQTT/NTP/OTA
                              // (faultRecord 供 /api/state 回传故障记忆)
  // ---- 注册码:未注册先弹注册页(提示但不限制 —— 长按编码器即可跳过,
  // 下次上电再提示;已注册的设备这里静默通过)。----
  if (!registrationValid())
    startRegistration();
}

// 本机是否已注册:NVS 存储码与 chipId 派生码(pure::regCodeFromChipId,算法与
// 盐在 pure_logic.h,厂商端用同一实现离线生成)逐位比对。结果缓存,注册成功
// 时置 1;NVS 读失败一律按未注册处理 —— 宁可多弹一次注册页,不能误判已注册。
bool registrationValid() {
  if (registeredCache < 0) {
    char stored[24];
    char expected[pure::kRegCodeBufferSize];
    const String chip = network.chipId();
    pure::regCodeFromChipId(chip.c_str(), expected);
    registeredCache = (settingsStore.loadRegistration(stored, sizeof(stored)) &&
                       strncmp(stored, expected, pure::kRegCodeLength) == 0)
                          ? 1
                          : 0;
  }
  return registeredCache == 1;
}

// 拉起注册页:输入区归零、结果清空。上电未注册时由 setup 调用,设置页
// "注册码"项进入时由 dispatchUiAction 的边沿请求调用。
void startRegistration() {
  registrationActive = true;
  regPos = 0;
  memset(regBuf, '0', 8);
  regBuf[8] = '\0';
  regResult = 0;
  Serial.println("[REG] registration page opened");
}

// 所有输入来源(编码器/触摸/串口)的统一出口。校准流程中先截获按键采集
// 两点;否则交给 UiModel,再处理它产生的边沿请求(保存耗材/保存设置/
// 进入校准/恢复出厂/解除故障锁定),按键音也在这里统一播放。
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
  // ---- 注册码流程:全模态接管输入(与触摸校准同构) ----
  // 旋转在当前位循环 0-9/A-F;单击确认进位,8 位齐即校验;
  // 结果态下单击分别进系统(成功)/清零重输(失败);长按随时跳过/退出。
  if (registrationActive) {
    static const char kRegChars[] = "0123456789ABCDEF";
    if (action == UiAction::FocusNext || action == UiAction::FocusPrevious) {
      if (!regResult) { // 结果态下字符已定,旋转不再改动
        const int d = action == UiAction::FocusNext ? 1 : -1;
        const char *cur = strchr(kRegChars, regBuf[regPos]);
        int idx = cur ? static_cast<int>(cur - kRegChars) : 0;
        idx = (idx + 16 + d) % 16;
        regBuf[regPos] = kRegChars[idx];
      }
    } else if (action == UiAction::EncoderClick) {
      if (regResult == 1) {
        registrationActive = false; // 注册成功 → 进系统
      } else if (regResult == 2) {
        regResult = 0; // 失败 → 清零重输
        regPos = 0;
        memset(regBuf, '0', 8);
      } else if (++regPos >= 8) {
        char expected[pure::kRegCodeBufferSize];
        const String chip = network.chipId();
        pure::regCodeFromChipId(chip.c_str(), expected);
        if (strncmp(regBuf, expected, pure::kRegCodeLength) == 0) {
          const bool saved = settingsStore.saveRegistration(regBuf);
          registeredCache = 1;
          regResult = 1;
          Serial.printf("[REG] registered OK, NVS %s\n",
                        saved ? "saved" : "SAVE FAILED");
        } else {
          regResult = 2;
          Serial.println("[REG] code mismatch");
        }
      }
    } else if (action == UiAction::EncoderLongPress) {
      // 跳过/退出:未注册不写入,下次上电还会再提示(提示但不限制)。
      registrationActive = false;
      Serial.println("[REG] skipped");
    }
    if (settings.keySound)
      startBuzzer(35);
    return;
  }
  ui.apply(action, controller);
  // 故障页长按编码器 → UiModel 只置一个边沿标志,真正的复位动作在这里执行:
  // 状态机归 controller 所有,输入回调不该直接改它。clearFault() 会清故障码
  // 与 PID 历史,热类故障还会先进入安全冷却姿态(保持排风),不会瞬间停风。
  if (ui.takeFaultResetRequest()) {
    controller.clearFault();
    Serial.println("Fault: cleared by encoder long press");
  }
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
    if (touchPanelPresent) {
      touchCalibrationActive = true;
      touchCalibrationStep = 0;
      Serial.println("Touch calibration: hold top-left, click EC11; then "
                     "bottom-right, click EC11");
    } else {
      Serial.println("Touch calibration: no panel detected, ignored");
    }
  }
  if (ui.takeRegistrationRequest()) {
    startRegistration();
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

// 运行时探测四线电阻触摸屏是否真的接上:分别把 X/Y 两层电阻膜的一端拉低、
// 另一端开弱上拉去读。膜存在时层电阻(几百欧,远小于 ~45k 上拉)会把读数脚
// 拉成低电平;没接屏时读数脚被上拉成高。编译期 HAS_TOUCH_PANEL 只表示"这块
// 板可能带触摸",混用非触摸屏时由这个探测兜底 —— 浮空触摸线产生的随机 ADC
// 采样偶尔会落在合法区间、被误判成触点(幽灵触摸),探测不到膜就整路关闭。
bool detectTouchPanel() {
  auto sheetPresent = [](int drivePin, int sensePin) {
    pinMode(drivePin, OUTPUT);
    digitalWrite(drivePin, LOW);
    pinMode(sensePin, INPUT_PULLUP);
    delay(1); // 等上拉/层电阻分压稳定
    uint8_t lowCount = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      if (digitalRead(sensePin) == LOW)
        ++lowCount;
      delayMicroseconds(200);
    }
    return lowCount == 4; // 四次全低才算在,滤掉瞬态干扰
  };
  const bool xSheet = sheetPresent(Pin::TFT_XL, Pin::TFT_XR);
  const bool ySheet = sheetPresent(Pin::TFT_YD, Pin::TFT_YU);
  // 恢复全输入,与 readTouchAxis 每次测量后的收尾状态一致。
  pinMode(Pin::TFT_XL, INPUT);
  pinMode(Pin::TFT_XR, INPUT);
  pinMode(Pin::TFT_YD, INPUT);
  pinMode(Pin::TFT_YU, INPUT);
  return xSheet && ySheet;
}

// 采样一次触摸并换算成屏幕像素坐标(480x320 横屏,留 28px 边距)。
// 映射目标 28..452 / 28..292 与校准两个采点位置(左上/右下)一致;
// 限幅也钳在同一区间,防止触摸超出校准角点时线性外推出框。
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
                      28, 452);
  screenY = constrain(static_cast<int>(map(rawY, settings.touchYMin,
                                           settings.touchYMax, 28, 292)),
                      28, 292);
  return true;
}

// 触摸屏轮询(约 28fps):按下坐标映射到主屏各热区并只在"按下瞬间"触发
// 一次动作,连续两次无有效采样才认为松手;设置页禁用触摸以防误改。
void pollTouchUi() {
  static uint32_t lastSampleMs = 0;  // 上次采样时刻(35ms 限频)
  static bool held = false;          // 当前手指仍按住,去重连续触发
  static uint8_t releaseSamples = 0; // 连续无触点采样计数(消抖)
  const uint32_t now = millis();
  if (!Pin::HAS_TOUCH_PANEL || !touchPanelPresent ||
      now - lastSampleMs < 35 || touchCalibrationActive)
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
  // 'i' —— I²C 总线扫描:用来判断 "AHT20/INA226 not detected" 是总线级还是
  // 单器件级故障。0x08–0x77 是 7 位地址的可用区间(代码里的 8..119 等价于
  // 0x08..0x77,0x00–0x07 与 0x78–0x7F 为保留地址)。逐地址发一次空写,
  // endTransmission() 返回 0 表示从机应答(ACK)了地址。
  //   一个 ACK 都没有 → 总线问题:上拉电阻、3V3 供电或 SDA/SCL 接反;
  //   只有 0x38(或只有 0x40)→ 单颗器件问题(I²C 上 AHT20=0x38,INA226=0x40)。
  case 'i': {
    Serial.printf("I2C scan SDA=%d SCL=%d (0x08-0x77):\n", Pin::I2C_SDA,
                  Pin::I2C_SCL);
    uint8_t found = 0;
    for (uint8_t addr = 8; addr < 120; ++addr) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) { // 0 = 该地址收到 ACK
        Serial.printf("  0x%02X ACK\n", addr);
        ++found;
      }
    }
    if (found == 0)
      Serial.println("  no ACK: check pull-ups / 3V3 supply / SDA-SCL swap");
    return;
  }
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
    // 与热控节拍用同一个有效性判据(ahtFresh),并且无效时把值也传成 NAN:
    // 显示层的约定是 NaN 显示 "--",这样 AHT20 掉线后屏上不会继续挂着旧温度。
    const bool ahtOk = ahtFresh();
    Readings in{ahtOk ? chamberTemp : NAN,  heaterBoardTemp,
                heaterCurrentA,            supplyVoltage,
                temperatureRead(),         digitalRead(Pin::PIR) == HIGH,
                ahtOk,                     !isnan(heaterBoardTemp),
                !isnan(heaterCurrentA)};
    UiSnapshot screen = ui.snapshot(controller, in, latestOutputs);
    screen.touchCalibrationActive = touchCalibrationActive;
    screen.touchCalibrationStep = touchCalibrationStep;
    screen.touchPresent = touchPanelPresent;
    // 注册码页:模态状态全在本文件,搬进快照供 drawRegistration 渲染。
    screen.registrationActive = registrationActive;
    screen.registered = registrationValid();
    screen.regPos = regPos;
    memcpy(screen.regBuf, regBuf, sizeof(screen.regBuf));
    screen.regResult = regResult;
    screen.humidity = ahtOk ? humidity : NAN;
    // 日期时间与生效配色由 main.cpp 填充:ui_model 不反向依赖 network.h。
    // 时钟优先 NTP,断网/未同步时回退上次手动校时值(main.cpp 的 currentEpoch)。
    formatClock(screen.clock, sizeof(screen.clock));
    screen.networkConnected = network.connected();
    // 信号强度:屏上原本只有"通/断"二态图标(networkConnected),WiFi 弱到一格
    // 也看不出来。这里补上真实 dBm,由界面折算成 4 格信号条;未联网时为哨兵 0。
    screen.rssi = network.rssiDbm();
    // 当前连接的 SSID:系统设置页 WiFi 项显示网络名用;未联网为空串。
    strlcpy(screen.ssid, network.ssidString().c_str(), sizeof(screen.ssid));
    // 网络告警(WiFi/MQTT 认证失败等)只作提示,不参与热控联锁、不停机;
    // 判定与计数在 network.cpp,这里只搬运给显示层。
    screen.netAlert = network.alert();
    // 芯片 ID 恒定,首次读取后缓存,避免每 500ms 重新构造 String。
    static char chipIdCache[20] = "";
    if (!chipIdCache[0])
      strlcpy(chipIdCache, network.chipId().c_str(), sizeof(chipIdCache));
    strlcpy(screen.chipId, chipIdCache, sizeof(screen.chipId));
    screen.theme = resolveTheme();
    network.updateReadings(in, latestOutputs,
                           ahtOk ? humidity : NAN); // Web/MQTT 据此返回数据
    // OTA 期间在屏幕底部叠加升级进度条(网络子系统→显示层桥接)。
    tftUi.setOtaProgress(network.otaActive(), network.otaProgress());
    tftUi.render(screen);
  }
}
