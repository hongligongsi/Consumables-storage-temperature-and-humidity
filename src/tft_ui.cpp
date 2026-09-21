#include "tft_ui.h"

#include "font_cn16.h"
#include "font_cn26.h"
#include "pins.h"
#include "version.h"
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

namespace {
constexpr int16_t SCREEN_W = 480;
constexpr int16_t SCREEN_H = 320;

// 运行期调色板:日/夜两套配色,由 drawFrame 按快照的 theme 选择。
// 夜间 = 改动前的原固件配色;日间取自 tools/ui_preview.html 的
// [data-theme="day"] 令牌(RGB565 换算)。
struct Palette {
  uint16_t bg;       // 页面底色
  uint16_t panel;    // 面板/未选中行底色
  uint16_t panelAlt; // 顶栏/按钮底色
  uint16_t border;   // 描边
  uint16_t muted;    // 次要文字
  uint16_t text;     // 主要文字
  uint16_t active;   // 选中行/激活按钮底色
  uint16_t accent;   // 强调色
  uint16_t warn;     // 告警/焦点色
  uint16_t good;     // 正常/联网色
  uint16_t maroon;   // 故障页标题条
  uint16_t gRed;
  uint16_t gYellow;
  uint16_t gMagenta;
  uint16_t gCyan;
};

constexpr Palette kNightPalette = {0x0861, 0x10E3, 0x1924, 0x31A6, 0x9CF3,
                                   0xFFFF, 0x2144, 0x06DD, 0xFD20, 0x4E69,
                                   0x7800, 0xF800, 0xFFE0, 0xF81F, 0x07FF};

constexpr Palette kDayPalette = {0xE77C, 0xFFFF, 0xDF3B, 0xB5F6, 0x634C,
                                 0x1903, 0xD75B, 0x0453, 0xC3A1, 0x34EA,
                                 0xA207, 0xD184, 0xB4A0, 0xA995, 0x0453};

// 当前生效配色。渲染任务单线程写,drawFrame 每帧按快照主题刷新。
uint16_t BG = kNightPalette.bg;
uint16_t PANEL = kNightPalette.panel;
uint16_t PANEL_ALT = kNightPalette.panelAlt;
uint16_t BORDER = kNightPalette.border;
uint16_t MUTED = kNightPalette.muted;
uint16_t TEXT = kNightPalette.text;
uint16_t ACTIVE = kNightPalette.active;
uint16_t ACCENT = kNightPalette.accent;
uint16_t WARN = kNightPalette.warn;
uint16_t GOOD = kNightPalette.good;
uint16_t MAROON = kNightPalette.maroon;
uint16_t G_RED = kNightPalette.gRed;
uint16_t G_YELLOW = kNightPalette.gYellow;
uint16_t G_MAGENTA = kNightPalette.gMagenta;
uint16_t G_CYAN = kNightPalette.gCyan;

void applyPalette(uint8_t theme) {
  const Palette &p = theme == 0 ? kDayPalette : kNightPalette;
  BG = p.bg;
  PANEL = p.panel;
  PANEL_ALT = p.panelAlt;
  BORDER = p.border;
  MUTED = p.muted;
  TEXT = p.text;
  ACTIVE = p.active;
  ACCENT = p.accent;
  WARN = p.warn;
  GOOD = p.good;
  MAROON = p.maroon;
  G_RED = p.gRed;
  G_YELLOW = p.gYellow;
  G_MAGENTA = p.gMagenta;
  G_CYAN = p.gCyan;
}

TFT_eSPI tft;
TFT_eSprite frame(&tft);
QueueHandle_t renderQueue = nullptr;
bool spriteReady = false;

enum class GaugeIcon : uint8_t {
  Exhaust,
  Mcu,
  Humidity,
  Chamber,
  HeaterFan,
  HeaterBoard,
  Voltage,
  Current
};
enum class ButtonIcon : uint8_t { Idle, Print, Preheat, Light, Settings };

struct Texts {
  const char *title;
  const char *state;
  const char *autoExhaust;
  const char *autoTemperature;
  const char *postExhaust;
  const char *minimum;
  const char *maximum;
  const char *fanSpeed;
  const char *gauges[8];
  const char *buttons[5];
};

const char *stateName(ChamberState state, bool chinese) {
  static const char *const zh[] = {"待机", "检测中", "预热",
                                   "打印", "排气",   "故障"};
  static const char *const en[] = {"IDLE",  "DETECT",  "PREHEAT",
                                   "PRINT", "EXHAUST", "FAULT"};
  const uint8_t i = static_cast<uint8_t>(state);
  return (chinese ? zh
                  : en)[i <= static_cast<uint8_t>(ChamberState::Fault) ? i : 0];
}

Texts texts(const UiSnapshot &s) {
  Texts out{};
  if (s.language == Language::Chinese) {
    out = {"智能耗材仓",
           stateName(s.state, true),
           "排风自动",
           "温度自动",
           "打印后排风",
           "最低",
           "最高",
           "风速",
           {"排风", "主板", "湿度", "仓温", "热风", "热板", "电压", "电流"},
           {"未打印", "工作中", "提前预热", "灯光", "系统设置"}};
  } else {
    out = {"SMART CHAMBER",
           stateName(s.state, false),
           "AUTO EXHAUST",
           "AUTO TEMP",
           "POST EXHAUST",
           "MIN",
           "MAX",
           "FAN",
           {"EXHAUST", "MCU", "HUMID", "CHAMBER", "HOT FAN", "HEATER",
            "VOLTAGE", "CURRENT"},
           {"IDLE", "WORKING", "PREHEAT", "LIGHT", "SETTINGS"}};
  }
  return out;
}

void drawGaugeIcon(TFT_eSPI &g, GaugeIcon icon, int16_t x, int16_t y,
                   uint16_t c) {
  switch (icon) {
  case GaugeIcon::Exhaust:
  case GaugeIcon::HeaterFan: {
    g.drawCircle(x, y, 9, c);
    g.fillCircle(x, y, 2, c);
    for (uint8_t i = 0; i < 4; ++i) {
      const float a = i * 1.5708f;
      const int16_t x1 = x + lroundf(cosf(a) * 3),
                    y1 = y + lroundf(sinf(a) * 3);
      const int16_t x2 = x + lroundf(cosf(a + .55f) * 8),
                    y2 = y + lroundf(sinf(a + .55f) * 8);
      g.drawLine(x1, y1, x2, y2, c);
      g.drawLine(x1 + (y2 - y1) / 3, y1 - (x2 - x1) / 3, x2, y2, c);
    }
    if (icon == GaugeIcon::HeaterFan) {
      g.drawFastHLine(x - 8, y + 12, 5, WARN);
      g.drawFastHLine(x + 3, y + 12, 5, WARN);
    }
    break;
  }
  case GaugeIcon::Mcu:
    g.drawRect(x - 7, y - 7, 15, 15, c);
    g.drawRect(x - 3, y - 3, 7, 7, c);
    for (int8_t i = -6; i <= 6; i += 4) {
      g.drawFastHLine(x - 11, y + i, 4, c);
      g.drawFastHLine(x + 8, y + i, 4, c);
      g.drawFastVLine(x + i, y - 11, 4, c);
      g.drawFastVLine(x + i, y + 8, 4, c);
    }
    break;
  case GaugeIcon::Humidity:
    g.fillTriangle(x, y - 11, x - 7, y + 3, x + 7, y + 3, c);
    g.fillCircle(x, y + 3, 7, c);
    g.fillCircle(x + 2, y + 1, 4, PANEL);
    break;
  case GaugeIcon::Chamber:
    g.drawLine(x - 10, y - 2, x, y - 11, c);
    g.drawLine(x, y - 11, x + 10, y - 2, c);
    g.drawRect(x - 8, y - 2, 16, 12, c);
    g.drawFastVLine(x, y + 2, 7, c);
    break;
  case GaugeIcon::HeaterBoard:
    g.drawRoundRect(x - 11, y - 8, 22, 16, 3, c);
    g.drawLine(x - 7, y + 3, x - 3, y - 3, c);
    g.drawLine(x - 3, y - 3, x + 1, y + 3, c);
    g.drawLine(x + 1, y + 3, x + 5, y - 3, c);
    g.drawLine(x + 5, y - 3, x + 8, y + 2, c);
    break;
  case GaugeIcon::Voltage:
    g.fillTriangle(x + 2, y - 12, x - 7, y + 2, x, y + 1, c);
    g.fillTriangle(x, y - 1, x + 7, y - 2, x - 3, y + 12, c);
    break;
  case GaugeIcon::Current:
    g.drawCircle(x, y, 10, c);
    g.drawLine(x, y, x + 5, y - 6, c);
    g.drawFastHLine(x - 6, y + 5, 12, c);
    g.fillCircle(x, y, 2, c);
    break;
  }
}

void drawBottomIcon(TFT_eSPI &g, ButtonIcon icon, int16_t x, int16_t y,
                    uint16_t c) {
  switch (icon) {
  case ButtonIcon::Idle:
    g.drawCircle(x, y, 9, c);
    g.drawFastVLine(x, y - 12, 10, c);
    break;
  case ButtonIcon::Print:
    g.fillTriangle(x - 6, y - 9, x - 6, y + 9, x + 9, y, c);
    break;
  case ButtonIcon::Preheat:
    g.fillTriangle(x, y - 12, x - 8, y + 8, x + 8, y + 8, c);
    g.fillCircle(x, y + 5, 7, c);
    g.fillCircle(x + 1, y + 4, 3, PANEL_ALT);
    break;
  case ButtonIcon::Light:
    g.drawCircle(x, y - 3, 8, c);
    g.drawFastHLine(x - 5, y + 7, 10, c);
    g.drawFastHLine(x - 3, y + 10, 6, c);
    break;
  case ButtonIcon::Settings:
    g.drawCircle(x, y, 10, c);
    g.drawCircle(x, y, 4, c);
    g.drawFastVLine(x - 1, y - 13, 3, c);
    g.drawFastVLine(x - 1, y + 11, 3, c);
    g.drawFastHLine(x - 13, y - 1, 3, c);
    g.drawFastHLine(x + 11, y - 1, 3, c);
    break;
  }
}

void drawToggle(TFT_eSPI &g, int16_t x, int16_t y, bool enabled, uint16_t c) {
  g.fillRoundRect(x, y, 48, 22, 11, enabled ? c : BORDER);
  g.fillCircle(enabled ? x + 37 : x + 11, y + 11, 9, TEXT);
}

void formatValue(char *dest, size_t size, float value, uint8_t decimals = 0) {
  if (isnan(value)) {
    strlcpy(dest, "--", size);
    return;
  }
  snprintf(dest, size, decimals ? "%.1f" : "%.0f", value);
}

uint16_t gaugeColor(uint8_t index, const UiSnapshot &s) {
  // 不能是 static:调色板随日夜切换,数组须每帧重建。
  const uint16_t colors[] = {G_RED,     GOOD, G_YELLOW, WARN,
                             G_MAGENTA, WARN, G_CYAN,   GOOD};
  if (index == 5 && !isnan(s.heaterBoardC) &&
      s.heaterBoardC >= s.heaterBoardLimitC)
    return G_RED;
  return colors[index];
}

bool buttonActive(uint8_t i, const UiSnapshot &s) {
  return (i == 0 && s.pirMotion) || (i == 1 && s.systemEnabled) ||
         (i == 2 && s.preheat) || (i == 3 && s.light);
}

void drawFaultFrame(TFT_eSPI &g, const UiSnapshot &s) {
  const bool zh = s.language == Language::Chinese;
  g.fillScreen(BG);
  g.fillRect(0, 0, SCREEN_W, 30, MAROON);
  g.fillTriangle(240, 48, 196, 124, 284, 124, G_RED);
  g.fillCircle(240, 107, 5, TEXT);
  g.fillRoundRect(237, 70, 7, 27, 3, TEXT);

  g.loadFont(FontCN26);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(TEXT, BG);
  g.drawString(zh ? "传感器错误" : "SENSOR FAULT", 240, 153);
  g.unloadFont();

  g.loadFont(FontCN16);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(WARN, BG);
  g.drawString(zh ? "系统已停止" : "SYSTEM STOPPED", 240, 184);
  g.setTextColor(MUTED, BG);
  g.drawString(zh ? "请检查" : "CHECK SENSORS", 240, 207);

  const char *names[] = {"AHT20", "NTC", "INA226"};
  const bool valid[] = {s.ahtValid, s.ntcValid, s.inaValid};
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t x = 104 + i * 136;
    const uint16_t c = valid[i] ? GOOD : G_RED;
    g.fillCircle(x - 32, 250, 5, c);
    g.setTextColor(c, BG);
    g.setTextDatum(ML_DATUM);
    g.drawString(names[i], x - 20, 250);
  }
  g.setTextColor(TEXT, MAROON);
  g.setTextDatum(MC_DATUM);
  g.drawString(s.clock, 240, 15);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

void drawMaterialSettings(TFT_eSPI &g, const UiSnapshot &s) {
  const bool zh = s.language == Language::Chinese;
  const char *labelsZh[] = {"最低温度", "最高温度", "最低风速",
                            "最高风速", "排气风速", "排气时间"};
  const char *labelsEn[] = {"MIN TEMP", "MAX TEMP", "MIN FAN",
                            "MAX FAN",  "POST FAN", "POST TIME"};
  const float values[] = {s.profileMinC,
                          s.profileMaxC,
                          static_cast<float>(s.exhaustMinPercent),
                          static_cast<float>(s.exhaustMaxPercent),
                          static_cast<float>(s.postExhaustPercent),
                          static_cast<float>(s.postExhaustSeconds)};
  const char *units[] = {"C", "C", "%", "%", "%", "s"};

  g.fillScreen(BG);
  g.fillRect(0, 0, SCREEN_W, 32, PANEL_ALT);
  g.loadFont(FontCN16);
  g.setTextColor(TEXT, PANEL_ALT);
  g.setTextDatum(ML_DATUM);
  g.drawString(zh ? "耗材设置" : "MATERIAL SETTINGS", 10, 16);
  g.setTextColor(ACCENT, PANEL_ALT);
  g.setTextDatum(MR_DATUM);
  g.drawString(s.materialSettingsDirty ? "*" : s.material, 468, 16);

  for (uint8_t i = 0; i < 6; ++i) {
    const int16_t y = 37 + i * 39;
    const bool selected = i == static_cast<uint8_t>(s.materialSettingField);
    const uint16_t rowBg = selected ? ACTIVE : PANEL;
    g.fillRoundRect(20, y, 440, 34, 5, rowBg);
    g.drawRoundRect(20, y, 440, 34, 5, selected ? ACCENT : BORDER);
    g.setTextColor(selected ? TEXT : MUTED, rowBg);
    g.setTextDatum(ML_DATUM);
    g.drawString(zh ? labelsZh[i] : labelsEn[i], 34, y + 17);
    char value[24];
    snprintf(value, sizeof(value), "%.0f%s", values[i], units[i]);
    g.setTextColor(selected ? ACCENT : TEXT, rowBg);
    g.setTextDatum(MR_DATUM);
    g.drawString(value, 442, y + 17);
  }
  g.setTextDatum(MC_DATUM);
  g.setTextColor(MUTED, BG);
  g.drawString(zh ? "旋转修改  单击下一项  长按保存返回"
                  : "TURN: EDIT  CLICK: NEXT  HOLD: SAVE/BACK",
               240, 286);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

// 把 "YYYY-MM-DD HH:MM:SS" 按需截取:日期行取年月日,时间行取时分秒;
// 编辑态下用方括号标出当前可旋转调整的子段(年/月/日 或 时/分)。
void formatClockField(char *out, size_t size, const char *clock, bool dateRow,
                      bool editing, uint8_t sub) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  // 未同步且未手动校时过时,快照里是 "----/--/-- --:--:--",解析失败保持占位。
  const bool valid =
      strlen(clock) >= 19 && sscanf(clock, "%d-%d-%d %d:%d:%d", &year, &month,
                                    &day, &hour, &minute, &second) == 6;
  if (!valid) {
    strlcpy(out, dateRow ? "----/--/--" : "--:--:--", size);
    return;
  }
  if (dateRow) {
    if (editing && sub == 0)
      snprintf(out, size, "[%04d]-%02d-%02d", year, month, day);
    else if (editing && sub == 1)
      snprintf(out, size, "%04d-[%02d]-%02d", year, month, day);
    else if (editing && sub == 2)
      snprintf(out, size, "%04d-%02d-[%02d]", year, month, day);
    else
      snprintf(out, size, "%04d-%02d-%02d", year, month, day);
  } else if (editing && sub == 0) {
    snprintf(out, size, "[%02d]:%02d:%02d", hour, minute, second);
  } else if (editing && sub == 1) {
    snprintf(out, size, "%02d:[%02d]:%02d", hour, minute, second);
  } else {
    snprintf(out, size, "%02d:%02d:%02d", hour, minute, second);
  }
}

void drawSystemSettings(TFT_eSPI &g, const UiSnapshot &s) {
  // 条目文案按 SystemSettingField 的下标索引取值,顺序必须与枚举保持一致。
  static const char *const zh[] = {
      "系统语言",       "WiFi联网",       "MQTT上报",
      "NTP校时",        "OTA升级",        "按键声音",
      "屏幕亮度",       "屏幕休眠时间",   "打印时保持屏幕开启",
      "编码器方向",     "PIR启动延时",    "PIR关闭延时",
      "启动后自动开灯", "关闭后自动关灯", "启动后蜂鸣提示",
      "关闭后蜂鸣提示", "发热板限流",     "发热板风扇风速",
      "发热板温度保护", "屏幕配色",       "日间开始时刻",
      "夜间开始时刻",   "日期",           "时间",
      "触摸屏校准",     "恢复出厂配置",   "固件版本"};
  static const char *const en[] = {"LANGUAGE",
                                   "WIFI",
                                   "MQTT",
                                   "NTP",
                                   "OTA",
                                   "KEY SOUND",
                                   "BRIGHTNESS",
                                   "SCREEN SLEEP",
                                   "KEEP ON PRINTING",
                                   "ENCODER DIRECTION",
                                   "PIR START DELAY",
                                   "PIR STOP DELAY",
                                   "LIGHT ON START",
                                   "LIGHT OFF STOP",
                                   "BEEP ON START",
                                   "BEEP ON STOP",
                                   "HEATER CURRENT",
                                   "HEATER FAN",
                                   "HEATER PROTECTION",
                                   "THEME",
                                   "DAY START",
                                   "NIGHT START",
                                   "DATE",
                                   "TIME",
                                   "TOUCH CALIBRATION",
                                   "FACTORY RESET",
                                   "FIRMWARE VERSION"};
  const bool chinese = s.language == Language::Chinese;
  // 条目总数与光标位置都从枚举推导,新增设置项后无需再改这里的硬编码数字。
  const uint8_t count = static_cast<uint8_t>(SystemSettingField::Count);
  constexpr uint8_t kRows = 5; // 一屏可见 5 行,超出部分靠滚动窗口展示。
  const uint8_t selected = static_cast<uint8_t>(s.systemSettingField);
  // 滚动窗口:选中项保持在 5 行内,且首行不超过 count-kRows,否则末项永远不可见。
  uint8_t first = selected >= 2 ? static_cast<uint8_t>(selected - 2) : 0;
  if (first > count - kRows)
    first = static_cast<uint8_t>(count - kRows);
  const SystemSettings &v = s.systemSettings;

  g.fillScreen(BG);
  g.fillRect(0, 0, SCREEN_W, 32, PANEL_ALT);
  g.loadFont(FontCN16);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(TEXT, PANEL_ALT);
  g.drawString(chinese ? "系统设置" : "SYSTEM SETTINGS", 10, 16);
  // 右上角页码:"当前项/总数",存在未保存修改时追加 " *"。
  char page[16];
  snprintf(page, sizeof(page), "%u/%u%s", selected + 1, count,
           s.systemSettingsDirty ? " *" : "");
  g.setTextDatum(MR_DATUM);
  g.setTextColor(ACCENT, PANEL_ALT);
  g.drawString(page, 468, 16);

  for (uint8_t row = 0; row < 5; ++row) {
    const uint8_t item = first + row;
    const int16_t y = 40 + row * 46;
    const bool active = item == selected;
    const uint16_t rowBg = active ? ACTIVE : PANEL;
    g.fillRoundRect(14, y, 452, 39, 5, rowBg);
    g.drawRoundRect(14, y, 452, 39, 5, active ? ACCENT : BORDER);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(active ? TEXT : MUTED, rowBg);
    g.drawString(chinese ? zh[item] : en[item], 27, y + 20);
    char value[24] = "";
    const char *on = chinese ? "开" : "ON", *off = chinese ? "关" : "OFF";
    // 右侧取值区:按字段类型格式化为开关、数值或状态文案。
    switch (static_cast<SystemSettingField>(item)) {
    case SystemSettingField::Language:
      strlcpy(value, v.language == Language::Chinese ? "中文" : "EN",
              sizeof(value));
      break;
    case SystemSettingField::WifiEnabled:
      strlcpy(value, v.wifiEnabled ? on : off, sizeof(value));
      break;
    case SystemSettingField::MqttEnabled:
      strlcpy(value, v.mqttEnabled ? on : off, sizeof(value));
      break;
    case SystemSettingField::NtpEnabled:
      strlcpy(value, v.ntpEnabled ? on : off, sizeof(value));
      break;
    case SystemSettingField::OtaEnabled:
      strlcpy(value, v.otaEnabled ? on : off, sizeof(value));
      break;
    case SystemSettingField::KeySound:
      strlcpy(value, v.keySound ? on : off, sizeof(value));
      break;
    case SystemSettingField::Brightness:
      snprintf(value, sizeof(value), "%u%%", v.brightness);
      break;
    case SystemSettingField::ScreenSleep:
      snprintf(value, sizeof(value), "%us", v.screenSleepSeconds);
      break;
    case SystemSettingField::KeepOnPrinting:
      strlcpy(value, v.keepScreenOnPrinting ? on : off, sizeof(value));
      break;
    case SystemSettingField::EncoderDirection:
      strlcpy(value,
              v.encoderReversed ? (chinese ? "反向" : "REVERSE")
                                : (chinese ? "正向" : "NORMAL"),
              sizeof(value));
      break;
    case SystemSettingField::PirStart:
      snprintf(value, sizeof(value), "%us", v.pirStartSeconds);
      break;
    case SystemSettingField::PirStop:
      snprintf(value, sizeof(value), "%us", v.pirStopSeconds);
      break;
    case SystemSettingField::LightOnStart:
      strlcpy(value, v.lightOnPrinting ? on : off, sizeof(value));
      break;
    case SystemSettingField::LightOffStop:
      strlcpy(value, v.lightOffAfterPrinting ? on : off, sizeof(value));
      break;
    case SystemSettingField::BeepOnStart:
      strlcpy(value, v.beepOnStart ? on : off, sizeof(value));
      break;
    case SystemSettingField::BeepOnStop:
      strlcpy(value, v.beepOnStop ? on : off, sizeof(value));
      break;
    case SystemSettingField::HeaterCurrent:
      snprintf(value, sizeof(value), "%uA", v.heaterMaxCurrentA);
      break;
    case SystemSettingField::HeaterFan:
      snprintf(value, sizeof(value), "%u%%", v.heaterFanPercent);
      break;
    case SystemSettingField::HeaterProtection:
      snprintf(value, sizeof(value), "%uC", v.heaterBoardLimitC);
      break;
    case SystemSettingField::Theme:
      // 0=日间 1=夜间 2=自动(按日/夜开始时刻切换)
      strlcpy(value,
              v.theme == 0 ? (chinese ? "日间" : "DAY")
                           : (v.theme == 1 ? (chinese ? "夜间" : "NIGHT")
                                           : (chinese ? "自动" : "AUTO")),
              sizeof(value));
      break;
    case SystemSettingField::DayStart:
    case SystemSettingField::NightStart: {
      const uint16_t minutes =
          item == static_cast<uint8_t>(SystemSettingField::DayStart)
              ? v.dayStartMinutes
              : v.nightStartMinutes;
      snprintf(value, sizeof(value), "%02u:%02u", minutes / 60, minutes % 60);
      break;
    }
    case SystemSettingField::Date:
      formatClockField(value, sizeof(value), s.clock, true,
                       s.systemSettingsEditing, s.systemSettingsSubField);
      break;
    case SystemSettingField::Time:
      formatClockField(value, sizeof(value), s.clock, false,
                       s.systemSettingsEditing, s.systemSettingsSubField);
      break;
    case SystemSettingField::TouchCalibration:
      strlcpy(value,
              !Pin::HAS_TOUCH_PANEL
                  ? (chinese ? "不支持" : "N/A")
                  : (v.touchCalibrated ? (chinese ? "已校准" : "READY")
                                       : (chinese ? "未校准" : "NOT SET")),
              sizeof(value));
      break;
    case SystemSettingField::FactoryReset:
      strlcpy(value,
              s.systemSettingsEditing ? (chinese ? "确认?" : "CONFIRM?")
                                      : (chinese ? "执行" : "RUN"),
              sizeof(value));
      break;
    case SystemSettingField::Version:
      // 只读显示编译期写入的版本号,源码见 include/version.h。
      strlcpy(value, FW_VERSION, sizeof(value));
      break;
    case SystemSettingField::Count:
      break;
    }
    g.setTextDatum(MR_DATUM);
    g.setTextColor(active ? ACCENT : TEXT, rowBg);
    g.drawString(value, 450, y + 20);
  }
  g.setTextDatum(MC_DATUM);
  g.setTextColor(MUTED, BG);
  g.drawString(
      s.systemSettingsEditing
          ? (chinese ? "旋转修改  单击完成" : "TURN TO EDIT  CLICK DONE")
          : (chinese ? "旋转选择  单击编辑  长按保存返回"
                     : "TURN SELECT  CLICK EDIT  HOLD SAVE"),
      240, 289);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

void drawTouchCalibration(TFT_eSPI &g, const UiSnapshot &s) {
  const bool first = s.touchCalibrationStep == 0;
  const int16_t x = first ? 28 : 452, y = first ? 28 : 292;
  g.fillScreen(BG);
  g.drawFastHLine(x - 14, y, 29, ACCENT);
  g.drawFastVLine(x, y - 14, 29, ACCENT);
  g.drawCircle(x, y, 9, TEXT);
  g.loadFont(FontCN16);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(TEXT, BG);
  g.drawString(s.language == Language::Chinese
                   ? (first ? "按住左上角十字并单击编码器"
                            : "按住右下角十字并单击编码器")
                   : (first ? "HOLD TOP-LEFT + CLICK ENCODER"
                            : "HOLD BOTTOM-RIGHT + CLICK ENCODER"),
               240, 150);
  g.setTextColor(MUTED, BG);
  g.drawString(s.language == Language::Chinese ? "长按编码器取消"
                                               : "HOLD ENCODER TO CANCEL",
               240, 184);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

void drawFrame(TFT_eSPI &g, const UiSnapshot &s) {
  // 每帧按快照里的生效主题重建调色板,日/夜切换与主题设置即时生效。
  applyPalette(s.theme);
  if (s.state == ChamberState::Fault) {
    drawFaultFrame(g, s);
    return;
  }
  if (s.touchCalibrationActive) {
    drawTouchCalibration(g, s);
    return;
  }
  if (s.materialSettingsOpen) {
    drawMaterialSettings(g, s);
    return;
  }
  if (s.systemSettingsOpen) {
    drawSystemSettings(g, s);
    return;
  }
  const Texts tx = texts(s);
  const bool chinese = s.language == Language::Chinese;
  g.fillScreen(BG);

  // Reference-style dashboard: wide control zone, compact 2 x 4 gauges,
  // and a persistent five-button status bar.
  g.fillRoundRect(4, 4, 292, 238, 6, PANEL);
  g.drawRoundRect(4, 4, 292, 238, 6, BORDER);
  g.drawFastHLine(8, 60, 284, BORDER);
  g.drawFastHLine(8, 120, 284, G_RED);
  g.drawFastHLine(8, 180, 284, ACCENT);
  g.drawFastHLine(8, 239, 284, WARN);

  // Right hand 2 x 4 instrument grid.
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    g.fillRoundRect(x, y, 87, 56, 5, PANEL);
    g.drawRoundRect(x, y, 87, 56, 5, BORDER);
    g.drawFastHLine(x + 3, y + 53, 81, gaugeColor(i, s));
    drawGaugeIcon(g, static_cast<GaugeIcon>(i), x + 15, y + 17,
                  gaugeColor(i, s));
  }

  // Bottom five action buttons.
  for (uint8_t i = 0; i < 5; ++i) {
    const int16_t x = 4 + i * 95;
    const bool active = buttonActive(i, s);
    const bool focused = (i == 1 && s.mainFocus == MainFocus::System) ||
                         (i == 2 && s.mainFocus == MainFocus::Preheat) ||
                         (i == 3 && s.mainFocus == MainFocus::Light) ||
                         (i == 4 && s.mainFocus == MainFocus::Settings);
    g.fillRoundRect(x, 248, 91, 68, 6, active ? ACTIVE : PANEL_ALT);
    g.drawRoundRect(x, 248, 91, 68, 6,
                    focused ? TEXT : (active ? ACCENT : BORDER));
    if (focused)
      g.drawRoundRect(x + 2, 250, 87, 64, 5, WARN);
    drawBottomIcon(g, static_cast<ButtonIcon>(i), x + 45, 268,
                   active ? WARN : TEXT);
  }

  // Render all labels with the 16 px Chinese+ASCII VLW font.
  g.loadFont(FontCN16);

  // Material carousel and compact runtime status.
  g.setTextDatum(MC_DATUM);
  const uint16_t prevColor =
      s.mainFocus == MainFocus::PreviousMaterial ? WARN : MUTED;
  const uint16_t materialColor =
      s.mainFocus == MainFocus::CurrentMaterial ? WARN : ACCENT;
  const uint16_t nextColor =
      s.mainFocus == MainFocus::NextMaterial ? WARN : MUTED;
  g.setTextColor(prevColor, PANEL);
  g.drawString(s.previousMaterial, 35, 23);
  g.drawCircle(82, 23, 13, prevColor);
  g.drawString("<", 82, 23);
  g.setTextColor(materialColor, PANEL);
  g.drawString(s.material, 145, 20);
  if (s.mainFocus == MainFocus::CurrentMaterial)
    g.drawFastHLine(122, 32, 46, WARN);
  g.drawCircle(207, 23, 13, nextColor);
  g.setTextColor(nextColor, PANEL);
  g.drawString(">", 207, 23);
  g.drawString(s.nextMaterial, 260, 23);
  // 日期在耗材行下方居中(面板中心
  // x=150),时间靠右对齐,状态文字左对齐与下方行标签同列。
  char dateBuf[16];
  char timeBuf[16];
  formatClockField(dateBuf, sizeof(dateBuf), s.clock, true, false, 0);
  formatClockField(timeBuf, sizeof(timeBuf), s.clock, false, false, 0);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(WARN, PANEL);
  g.drawString(s.manualExhaust ? (chinese ? "强制排气" : "MANUAL PURGE")
                               : tx.state,
               12, 46);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(s.networkConnected ? GOOD : MUTED, PANEL);
  g.drawString(dateBuf, 150, 46);
  g.setTextDatum(MR_DATUM);
  g.drawString(timeBuf, 290, 46);
  g.setTextDatum(MC_DATUM);

  const bool toggled[] = {s.autoExhaust, s.autoTemperature, s.postPrintExhaust};
  const char *titlesZh[] = {"排气风扇", "打印仓温", "打印结束"};
  const char *titlesEn[] = {"EXHAUST FAN", "CHAMBER TEMP", "PRINT FINISH"};
  const char *subtitlesZh[] = {"自动控制", "自动控制", "开启排气"};
  const char *subtitlesEn[] = {"AUTO CONTROL", "AUTO CONTROL", "POST EXHAUST"};
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t y = 62 + i * 60;
    const bool focused = static_cast<uint8_t>(s.mainFocus) ==
                         static_cast<uint8_t>(MainFocus::AutoExhaust) + i;
    if (focused)
      g.drawRoundRect(7, y, 286, 56, 4, WARN);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(TEXT, PANEL);
    g.drawString(chinese ? titlesZh[i] : titlesEn[i], 12, y + 14);
    g.drawString(chinese ? subtitlesZh[i] : subtitlesEn[i], 12, y + 38);
    drawToggle(g, 145, y + 18, toggled[i], i == 2 ? WARN : ACCENT);
  }

  char line[32];
  g.setTextDatum(MR_DATUM);
  g.setTextColor(MUTED, PANEL);
  snprintf(line, sizeof(line), "%s %u%%", tx.minimum, s.exhaustMinPercent);
  g.drawString(line, 288, 76);
  snprintf(line, sizeof(line), "%s %u%%", tx.maximum, s.exhaustMaxPercent);
  g.drawString(line, 288, 100);
  snprintf(line, sizeof(line), "%s %.0fC", tx.minimum, s.profileMinC);
  g.drawString(line, 288, 136);
  snprintf(line, sizeof(line), "%s %.0fC", tx.maximum, s.profileMaxC);
  g.drawString(line, 288, 160);
  snprintf(line, sizeof(line), "%s %u%%", tx.fanSpeed, s.postExhaustPercent);
  g.drawString(line, 288, 196);
  snprintf(line, sizeof(line), "%s %us", chinese ? "时间" : "TIME",
           s.postExhaustSeconds);
  g.drawString(line, 288, 220);

  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    g.setTextColor(MUTED, PANEL);
    g.setTextDatum(MC_DATUM);
    g.drawString(tx.gauges[i], x + 43, y + 43);
  }
  for (uint8_t i = 0; i < 5; ++i) {
    const int16_t x = 4 + i * 95;
    const char *label = tx.buttons[i];
    if (i == 0)
      label = chinese ? (s.pirMotion ? "检测到" : "未检测")
                      : (s.pirMotion ? "MOTION" : "NO MOTION");
    else if (i == 1)
      label = chinese ? (s.systemEnabled ? "工作中" : "已停止")
                      : (s.systemEnabled ? "WORKING" : "STOPPED");
    else if (i == 3)
      label = chinese ? (s.light ? "灯光开启" : "灯光关闭")
                      : (s.light ? "LIGHT ON" : "LIGHT OFF");
    g.setTextColor(TEXT, buttonActive(i, s) ? ACTIVE : PANEL_ALT);
    g.setTextDatum(MC_DATUM);
    g.drawString(label, x + 45, 302);
  }
  g.unloadFont();

  // Render the eight live values with the 26 px font.
  const float values[] = {static_cast<float>(s.exhaustPercent),
                          s.mcuC,
                          s.humidity,
                          s.chamberC,
                          static_cast<float>(s.heaterFanPercent),
                          s.heaterBoardC,
                          s.voltageV,
                          s.currentA};
  const char *units[] = {"%", "C", "%", "C", "%", "C", "V", "A"};
  g.loadFont(FontCN26);
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    char value[12], combined[16];
    formatValue(value, sizeof(value), values[i], i >= 6 ? 1 : 0);
    snprintf(combined, sizeof(combined), "%s%s", value, units[i]);
    g.setTextColor(gaugeColor(i, s), PANEL);
    g.setTextDatum(MR_DATUM);
    g.drawString(combined, x + 83, y + 16);
  }
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

void uiRenderTask(void *) {
  UiSnapshot snapshot{};
  uint8_t backBuffer = 1;
  for (;;) {
    if (xQueueReceive(renderQueue, &snapshot, portMAX_DELAY) != pdTRUE)
      continue;
    if (spriteReady) {
      frame.frameBuffer(backBuffer);
      drawFrame(frame, snapshot);
      frame.pushSprite(0, 0);
      backBuffer = backBuffer == 1 ? 2 : 1;
    } else {
      drawFrame(tft, snapshot);
    }
  }
}
} // namespace

void TftUi::begin() {
  tft.init();
  tft.setRotation(1);
  tft.setAttribute(UTF8_SWITCH, 1);
  tft.fillScreen(BG);

  if (psramFound()) {
    frame.setColorDepth(16);
    frame.setAttribute(PSRAM_ENABLE, 1);
    spriteReady = frame.createSprite(SCREEN_W, SCREEN_H, 2) != nullptr;
  }
  doubleBuffered_ = spriteReady;
  Serial.printf("TFT: %s, PSRAM free=%u bytes\n",
                spriteReady ? "480x320 double buffer"
                            : "direct-render fallback",
                ESP.getFreePsram());

  renderQueue = xQueueCreate(1, sizeof(UiSnapshot));
  queue_ = renderQueue;
  if (renderQueue) {
    const BaseType_t ok = xTaskCreatePinnedToCore(uiRenderTask, "tft-ui", 8192,
                                                  nullptr, 1, nullptr, 0);
    if (ok != pdPASS) {
      vQueueDelete(renderQueue);
      renderQueue = nullptr;
      queue_ = nullptr;
      Serial.println("TFT: render task creation failed; UI disabled to protect "
                     "control timing");
    }
  }
  ready_ = renderQueue != nullptr;
}

void TftUi::render(const UiSnapshot &snapshot) {
  if (!ready_)
    return;
  // A length-one queue deliberately drops stale frames if SPI is still busy.
  // Rendering is never allowed to fall back into the PID/Arduino loop task.
  xQueueOverwrite(static_cast<QueueHandle_t>(queue_), &snapshot);
}
