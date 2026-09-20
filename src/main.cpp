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

// 10 kΩ NTC、10 kΩ 上拉、B=3950 的常用模型；原理图热端 NTC 为 100 kΩ
// 时请改这里。
float readNtcCelsius(int pin, float seriesOhm = 10000.0f,
                     float nominalOhm = 10000.0f, float beta = 3950.0f) {
  const int raw = analogRead(pin);
  if (raw <= 0 || raw >= 4095)
    return NAN;
  const float resistance = seriesOhm * raw / (4095.0f - raw);
  const float invT = 1.0f / 298.15f + logf(resistance / nominalOhm) / beta;
  return 1.0f / invT - 273.15f;
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

void setHotPower(float percent) {
  if (!HEATER_ENABLED)
    percent = 0;
  percent = constrain(percent, 0.0f, 100.0f);
  ledcWrite(HOT_PWM_CHANNEL, lroundf(percent * ((1 << PWM_BITS) - 1) / 100.0f));
}

void emergencyStop(const char *reason) {
  setHotPower(0);
  digitalWrite(Pin::HOT_FAN, HIGH);
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
    if (aht.getEvent(&h, &t)) {
      chamberTemp = t.temperature; // AHT20 位于仓内，作为自动仓温闭环传感器
      humidity = h.relative_humidity;
      lastSensorOk = millis();
    }
  }
  if (inaAvailable) {
    if (!ina226.readCurrentA(heaterCurrentA))
      heaterCurrentA = NAN;
    if (!ina226.readBusVoltageV(supplyVoltage))
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
  if (ui.settings().manualExhaust)
    out.exhaustPercent = 100;
  latestOutputs = out;
  setHotPower(out.heaterPercent);
  digitalWrite(Pin::HOT_FAN, out.heaterFan ? HIGH : LOW);
  digitalWrite(Pin::AIR_FAN_DC, out.exhaustPercent ? HIGH : LOW);
  digitalWrite(Pin::BOARD_FAN, out.boardFan ? HIGH : LOW);
  ledcWrite(AIR_FAN_PWM_CHANNEL,
            lroundf(out.exhaustPercent * ((1 << PWM_BITS) - 1) / 100.0f));
  digitalWrite(Pin::LED_ENABLE,
               (out.light || ui.settings().light) ? HIGH : LOW);
  if (Pin::HAS_STATUS_OUTPUTS) {
    digitalWrite(Pin::PRINTING_STATUS,
                 out.state == ChamberState::Printing ? HIGH : LOW);
    digitalWrite(Pin::HEATING_STATUS,
                 (HEATER_ENABLED && out.heaterPercent > 0) ? HIGH : LOW);
  }
  if (out.state == ChamberState::Fault)
    setRgb(255, 0, 0);
  else if (out.state == ChamberState::Cooling)
    setRgb(255, 80, 0);
  else if (out.state == ChamberState::Printing)
    setRgb(0, 80, 180);
  else
    setRgb(0, 100, 0);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("ESP32-S3 N16R8 temperature-control board starting");

  pinMode(Pin::HOT_FAN, OUTPUT);
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
  tftUi.begin();
  network.begin(controller, ui,
                settings); // 联网:WiFi 配网门户 + Web/MQTT/NTP/OTA
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
    action = UiAction::EncoderDoubleClick;
    break;
  case 'W': // 打印联网状态,便于调试
    Serial.printf("NET: state=%d ip=%s time=%s mqtt=%d\n", (int)network.state(),
                  network.ipString().c_str(), network.timeString().c_str(),
                  settings.mqttEnabled);
    return;
  default:
    return;
  }
  ui.apply(action, controller);
  Serial.printf("UI: material=%s system=%d preheat=%d light=%d\n",
                controller.profile().name, ui.settings().systemEnabled,
                ui.settings().preheat, ui.settings().light);
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
