// 显示层实现:480x320 横屏 TFT 的全部绘制逻辑。main.cpp 每 500ms 生成
// 一份 UiSnapshot 经长度 1 的队列投递给独立 FreeRTOS 渲染任务
// (uiRenderTask,钉在 core0),渲染与热控/PID 完全解耦,绘制再慢也不会
// 拖慢 50ms 控制节拍。有八线 PSRAM 时开双缓冲 sprite(离屏绘制后一次
// push,无闪烁);分配失败则降级为直接写屏。
// 页面分发见 drawFrame:故障页/触摸校准/耗材设置/系统设置/主屏仪表盘。
#include "tft_ui.h"

#include "pure_logic.h" // rssiBars():信号格阈值(与 Web 管理页同一套)
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
constexpr int16_t SCREEN_W = 480; // 横屏宽(像素)
constexpr int16_t SCREEN_H = 320; // 横屏高(像素)

// 运行期调色板:三套主题共四组配色,由 drawFrame 按快照的生效主题选择。
//   0=默认  iOS 浅色(printer-hmi-ios-prototype.html 浅色外观:白灰底、白卡、
//          iOS 蓝 #007AFF、绿色开关、iOS 家族彩仪表)
//   1=IOS   深色 HMI(printer-hmi-redesign.html:近黑底、青色强调、语义家族色)
//   2/3=蓝白 日间浅蓝白 / 夜间亮蓝(原双模配色,蓝白主题下按时钟自动切换)
// 色值均为设计稿 RGB888 换算的 RGB565。
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

// 默认:iOS 浅色(白灰底/白卡/黑字/iOS 蓝强调/绿开关/家族彩状态色)。
// 黄/橙取加深版保证白底可读(同蓝白·日间的处理);G_CYAN 给 iOS 绿,
// 开关(drawToggle 用 G_CYAN)即原型签名性的绿色开关。
constexpr Palette kIosLightPalette = {0xF79E, 0xFFFF, 0xFFDF, 0xC639, 0x6B6E,
                                      0x0000, 0xDF5F, 0x03DF, 0xDC20, 0x362B,
                                      0xC185, 0xF9C6, 0xC440, 0xF96A, 0x362B};

// IOS:深色 HMI(取自 04e973b 一比一还原 printer-hmi-redesign.html 的调色板)
constexpr Palette kIosDarkPalette = {0x0882, 0x1905, 0x2166, 0x29A8, 0xADB8,
                                     0xEF9E, 0x19E9, 0x3EBC, 0xFDA8, 0x46CF,
                                     0x50C4, 0xFAEB, 0xFDA8, 0xB47F, 0x3EBC};

// 蓝白·夜间:亮蓝 iOS 风(蓝底白字、iOS 蓝色开关、黄色激活态)
constexpr Palette kNightPalette = {0x1B7A, 0x12B7, 0x0A33, 0x3C3C, 0xAE5E,
                                   0xFFFF, 0x2BDD, 0x4E1E, 0xFEA7, 0x4EE9,
                                   0xB145, 0xFA8A, 0xFEA7, 0xFB56, 0x5E5F};

// 蓝白·日间:同一设计语言的浅蓝白配色
constexpr Palette kDayPalette = {0xEF9F, 0xFFFF, 0xD73F, 0xB67D, 0x5B91,
                                 0x0908, 0xCF3F, 0x03DF, 0xDC80, 0x2D89,
                                 0x88E3, 0xD9A6, 0xC440, 0xC233, 0x0C3F};

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

// 按生效主题号(0=默认 1=IOS 2=蓝白·日 3=蓝白·夜)把整套调色板复制到
// 上面的全局颜色变量;drawFrame 每帧开头调用,所以切换主题下一帧即生效。
void applyPalette(uint8_t theme) {
  const Palette &p = theme == 0   ? kIosLightPalette
                     : theme == 1 ? kIosDarkPalette
                     : theme == 2 ? kDayPalette
                                  : kNightPalette;
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

TFT_eSPI tft;                        // 底层屏幕驱动(SPI)
TFT_eSprite frame(&tft);             // 全屏离屏画布(双缓冲时两块帧缓冲交替)
QueueHandle_t renderQueue = nullptr; // 长度 1 的快照队列(主循环 → 渲染任务)
bool spriteReady = false;            // 双缓冲 sprite 是否成功建立

// 右侧 2x4 仪表的顺序:外排/主控/湿度/仓温/热风/热板/电压/电流(按 UI 标注图),
// 数值数组与 texts().gauges 都按此枚举下标排列。
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
// 底部五个常驻按钮:人感状态/系统开关/预热/灯光/系统设置入口。
enum class ButtonIcon : uint8_t { Idle, Print, Preheat, Light, Settings };

// 一屏内所有可切换中英文的文案,texts() 按快照语言一次性填好。
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

// 六态状态机 → 界面状态文案(中文/英文),下标与 ChamberState 枚举一致。
const char *stateName(ChamberState state, bool chinese) {
  static const char *const zh[] = {"待机", "检测中", "预热",
                                   "打印", "排气",   "故障"};
  static const char *const en[] = {"IDLE",  "DETECT",  "PREHEAT",
                                   "PRINT", "EXHAUST", "FAULT"};
  const uint8_t i = static_cast<uint8_t>(state);
  return (chinese ? zh
                  : en)[i <= static_cast<uint8_t>(ChamberState::Fault) ? i : 0];
}

// 按快照语言组装主屏全部静态文案(标题/状态/三个开关/仪表名/按钮名)。
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
           {"外排", "主控", "湿度", "仓温", "热风", "热板", "电压", "电流"},
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

// 用基础图元手绘 8 个仪表小图标(风扇/芯片/水滴/仓体/热风/热板/闪电/表头),
// 全部以 (x,y) 为中心,避免引入图标位图资源。
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
    g.drawCircle(x, y + 3, 7, c);
    g.drawLine(x, y - 11, x - 6, y - 1, c);
    g.drawLine(x, y - 11, x + 6, y - 1, c);
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
    g.drawLine(x + 2, y - 12, x - 7, y + 2, c);
    g.drawLine(x - 7, y + 2, x, y + 1, c);
    g.drawLine(x, y + 1, x - 3, y + 12, c);
    g.drawLine(x - 3, y + 12, x + 7, y - 2, c);
    g.drawLine(x + 7, y - 2, x, y - 1, c);
    g.drawLine(x, y - 1, x + 2, y - 12, c);
    break;
  case GaugeIcon::Current:
    g.drawCircle(x, y, 10, c);
    g.drawLine(x, y, x + 5, y - 6, c);
    g.drawFastHLine(x - 6, y + 5, 12, c);
    g.fillCircle(x, y, 2, c);
    break;
  }
}

// 手绘底部 5 个按钮图标(人感/打印播放/预热火焰/灯泡/设置齿轮)。
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
    g.drawCircle(x, y + 5, 7, c);
    g.drawLine(x, y - 12, x - 6, y + 2, c);
    g.drawLine(x, y - 12, x + 6, y + 2, c);
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

// iOS 风格滑动开关:48x22 圆角轨道 + 圆点,enabled 决定轨道色与圆点位置。
// 美化:关闭态浅底暗描边+灰色圆点,开启态轨道同色描边,层次更清晰。
void drawToggle(TFT_eSPI &g, int16_t x, int16_t y, bool enabled, uint16_t c) {
  g.fillRoundRect(x, y, 48, 22, 11, enabled ? c : PANEL_ALT);
  g.drawRoundRect(x, y, 48, 22, 11, enabled ? c : BORDER);
  g.fillCircle(enabled ? x + 37 : x + 11, y + 11, 9, enabled ? TEXT : MUTED);
}

// 仪表数值格式化:NAN(传感器无效)统一显示 "--",否则按 0/1 位小数输出,
// 单位由调用处拼接。
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

// 底部前 4 个按钮的"激活态"来源各不相同:0=PIR 有人、1=系统使能、
// 2=预热中、3=灯亮;第 4 个(设置入口)无激活态。
bool buttonActive(uint8_t i, const UiSnapshot &s) {
  return (i == 0 && s.pirMotion) || (i == 1 && s.systemEnabled) ||
         (i == 2 && s.preheat) || (i == 3 && s.light);
}

// 故障页文案表:下标必须与 controller.h 的 FaultCode 一一对应
// (None 占位不可删,否则全部故障码整体串位)。标题回答"哪里坏了",
// 建议行回答"下一步做什么",现场不必回串口就能判断。
struct FaultText {
  const char *titleZh;
  const char *titleEn;
  const char *hintZh;
  const char *hintEn;
};

const FaultText FAULT_TEXTS[] = {
    {"系统故障", "SYSTEM FAULT", "请检查设备", "CHECK DEVICE"},
    {"仓温传感器掉线", "AHT20 OFFLINE", "检查 I2C 接线与供电", "CHECK I2C WIRING"},
    {"热板传感器掉线", "NTC OFFLINE", "检查 NTC 两芯接线", "CHECK NTC WIRING"},
    {"热板超温", "BOARD OVERHEAT", "检查加热板与热风风扇", "CHECK HEATER AND FAN"},
    {"仓温超上限", "CHAMBER OVERHEAT", "开门散热并检查排风",
     "OPEN DOOR AND CHECK FAN"},
    {"加热回路过流", "OVER CURRENT", "检查加热板与功率接线",
     "CHECK HEATER WIRING"},
    {"电流采样掉线", "INA226 OFFLINE", "检查 I2C 接线与供电", "CHECK I2C WIRING"},
};
static_assert(sizeof(FAULT_TEXTS) / sizeof(FAULT_TEXTS[0]) ==
                  static_cast<size_t>(FaultCode::Count),
              "FAULT_TEXTS 的条目数必须与 FaultCode 一致");

// 网络告警文案表:下标与 ui_model.h 的 NetAlert 一一对应(None 占位不可删)。
// 这类告警是**非阻塞提示** —— 不进故障页、不锁状态机、不停加热,只是把主屏
// 日期位置和设置页标题栏换成一行说明,用户完全可以在离线状态继续打印。
// 字数按主屏那一格的宽度(约 140 px)反推:中文字面 ≤6 字,英文 ≤14 字符,
// 再长就会顶到右侧的时间。故障码前缀 "W-0x" 只在设置页显示,主屏不显示。
struct NetAlertText {
  const char *zh;
  const char *en;
};

const NetAlertText NET_ALERT_TEXTS[] = {
    {"", ""},
    {"WiFi 认证失败", "WiFi AUTH FAIL"},   // W-01 密码错/加密方式不匹配
    {"找不到 WiFi", "NO WiFi AP"},        // W-02 SSID 错或超出覆盖范围
    {"MQTT 认证失败", "MQTT AUTH FAIL"},   // W-03 broker 拒绝匿名或凭据错
    {"MQTT 连接超时", "MQTT TIMEOUT"},     // W-04 地址/端口/网络不通
};
static_assert(sizeof(NET_ALERT_TEXTS) / sizeof(NET_ALERT_TEXTS[0]) ==
                  static_cast<size_t>(NetAlert::Count),
              "NET_ALERT_TEXTS 的条目数必须与 NetAlert 一致");

// 取当前告警的本地化短文案;None 返回空串(调用方已先判空)。
const char *netAlertText(const UiSnapshot &s) {
  const size_t i = static_cast<size_t>(s.netAlert);
  if (i == 0 || i >= sizeof(NET_ALERT_TEXTS) / sizeof(NET_ALERT_TEXTS[0]))
    return "";
  const bool chinese = s.language == Language::Chinese;
  return chinese ? NET_ALERT_TEXTS[i].zh : NET_ALERT_TEXTS[i].en;
}

// 故障页的一行传感器读数:在线状态点 + 名称 + 实测值(无效时整行标红)。
void drawFaultRow(TFT_eSPI &g, int16_t y, const char *name, bool valid,
                  const char *value) {
  const uint16_t c = valid ? GOOD : G_RED;
  g.fillCircle(112, y, 5, c);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(TEXT, BG);
  g.drawString(name, 128, y);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(valid ? MUTED : G_RED, BG);
  g.drawString(value, 368, y);
}

// 故障全屏页:顶栏给故障码与类别,三角警告符下方是按码变化的标题与处理
// 建议,再把 AHT20/NTC/INA226 的在线状态与实测值逐行列出 —— 故障码告诉
// 现场"哪种故障",实测值告诉"差多少",建议行告诉"做什么"。
void drawFaultFrame(TFT_eSPI &g, const UiSnapshot &s) {
  const bool zh = s.language == Language::Chinese;
  const uint8_t code = static_cast<uint8_t>(s.fault);
  // 枚举值本身就是屏上编号(AhtLost=1 → "F-01",None=0 只是哨兵),不要再 +1。
  // 越界值理论上不会出现(只有 state==Fault 才画本页),真出现时退化为"只显示
  // 文案、不显示编号",宁可没有编号也不能编出一个对不上表的码。
  const bool hasCode =
      code > 0 && code < static_cast<uint8_t>(FaultCode::Count);
  const FaultText &ft = FAULT_TEXTS[hasCode ? code : 0];

  g.fillScreen(BG);
  // ---- 顶栏:故障码 + 类别(左)、时间(右) ----
  g.fillRect(0, 0, SCREEN_W, 30, MAROON);
  g.loadFont(FontCN16);
  char header[40];
  if (hasCode)
    snprintf(header, sizeof(header), "F-%02u %s", code,
             zh ? ft.titleZh : ft.titleEn);
  else
    snprintf(header, sizeof(header), "%s", zh ? ft.titleZh : ft.titleEn);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(0xFFFF, MAROON);
  g.drawString(header, 12, 15);
  g.setTextDatum(MR_DATUM);
  // clock 为 "YYYY-MM-DD HH:MM:SS",+11 跳过日期部分只保留 HH:MM:SS。
  g.drawString(s.clock + 11, SCREEN_W - 12, 15);
  g.unloadFont();

  // ---- 警告三角 ----
  g.fillTriangle(240, 44, 198, 116, 282, 116, G_RED);
  g.fillRoundRect(237, 62, 7, 26, 3, TEXT);
  g.fillCircle(240, 100, 5, TEXT);

  // ---- 标题(按码)+ 处理建议 ----
  g.loadFont(FontCN26);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(TEXT, BG);
  g.drawString(zh ? ft.titleZh : ft.titleEn, 240, 142);
  g.unloadFont();

  g.loadFont(FontCN16);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(WARN, BG);
  g.drawString(zh ? ft.hintZh : ft.hintEn, 240, 172);

  // ---- 三路传感器:名称 + 在线点 + 实测值 ----
  char aht[28], ntc[28], ina[28];
  if (s.ahtValid)
    snprintf(aht, sizeof(aht), "%.1f℃ %.0f%%", s.chamberC, s.humidity);
  else
    strlcpy(aht, "--", sizeof(aht));
  if (s.ntcValid)
    snprintf(ntc, sizeof(ntc), "%.0f℃ / 限 %.0f℃", s.heaterBoardC,
             s.heaterBoardLimitC);
  else
    strlcpy(ntc, "--", sizeof(ntc));
  if (s.inaValid)
    snprintf(ina, sizeof(ina), "%.1fV %.2fA", s.voltageV, s.currentA);
  else
    strlcpy(ina, "--", sizeof(ina));
  drawFaultRow(g, 208, "AHT20", s.ahtValid, aht);
  drawFaultRow(g, 236, "NTC", s.ntcValid, ntc);
  drawFaultRow(g, 264, "INA226", s.inaValid, ina);

  // ---- 复位提示(长按编码器键,见 main.cpp 的故障态输入处理)----
  g.setTextDatum(MC_DATUM);
  g.setTextColor(MUTED, BG);
  g.drawString(zh ? "长按编码器键复位" : "HOLD ENCODER KEY TO RESET", 240, 296);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

// 耗材参数设置页:6 行(温区上下限/风速上下限/打印后排风风速与时长),
// 当前编辑行高亮,右上角有未保存修改时显示 "*";操作提示固定在底行。
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
  const char *units[] = {"℃", "℃", "%", "%", "%", "s"};

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
    g.fillRoundRect(20, y, 440, 34, 8, rowBg);
    g.drawRoundRect(20, y, 440, 34, 8, selected ? ACCENT : BORDER);
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

// 系统设置页:全部 SystemSettingField 条目,一屏 5 行带滚动窗口;每行左侧
// 名称右侧当前值(开关/百分比/秒/温度/时刻/日期/状态等各自格式化)。
// 标签数组下标必须与 SystemSettingField 枚举一一对应。
void drawSystemSettings(TFT_eSPI &g, const UiSnapshot &s) {
  // 条目文案按 SystemSettingField 的下标索引取值,顺序必须与枚举保持一致。
  static const char *const zh[] = {
      "系统语言",       "WiFi联网",       "MQTT上报",
      "NTP校时",        "OTA升级",        "按键声音",
      "屏幕亮度",       "屏幕休眠时间",   "RGB最大亮度",
      "RGB跟随屏幕休眠", "打印时保持屏幕开启",
      "编码器方向",     "PIR启动延时",    "PIR关闭延时",
      "启动后自动开灯", "关闭后自动关灯", "启动后蜂鸣提示",
      "关闭后蜂鸣提示", "发热板限流",     "发热板风扇风速",
      "发热板温度保护", "界面主题",       "日间开始时刻",
      "夜间开始时刻",   "日期",           "时间",
      "触摸屏校准",     "恢复出厂配置",   "注册码",
      "固件版本",       "芯片ID"};
  static const char *const en[] = {"LANGUAGE",
                                   "WIFI",
                                   "MQTT",
                                   "NTP",
                                   "OTA",
                                   "KEY SOUND",
                                   "BRIGHTNESS",
                                   "SCREEN SLEEP",
                                   "RGB BRIGHTNESS",
                                   "RGB SLEEP SYNC",
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
                                   "REGISTRATION",
                                   "FIRMWARE VERSION",
                                   "CHIP ID"};
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
  // 标题栏中段顺带显示网络告警(带故障码前缀):来这一页的多半就是要改
  // WiFi/MQTT 参数,原因直接摆在眼前,不必退回主屏看。仍然是提示性质 ——
  // 不拦任何设置项的编辑与保存。
  if (s.netAlert != NetAlert::None) {
    char alert[28];
    snprintf(alert, sizeof(alert), "W-0%u %s",
             (unsigned)static_cast<uint8_t>(s.netAlert), netAlertText(s));
    g.setTextDatum(MC_DATUM);
    g.setTextColor(WARN, PANEL_ALT);
    g.drawString(alert, 258, 16);
  }

  for (uint8_t row = 0; row < 5; ++row) {
    const uint8_t item = first + row;
    const int16_t y = 40 + row * 46;
    const bool active = item == selected;
    const uint16_t rowBg = active ? ACTIVE : PANEL;
    g.fillRoundRect(14, y, 452, 39, 8, rowBg);
    g.drawRoundRect(14, y, 452, 39, 8, active ? ACCENT : BORDER);
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
      // 联网时右侧直接显示当前 SSID,比孤零零一个"开"信息量大:连的哪张网
      // 一眼可见(信号格画在值文本右侧,见循环尾)。SSID 最长 32 字符,行内
      // 放不下就截到 17 字符加 "...";未联网保持 开/关。
      if (v.wifiEnabled && s.networkConnected && s.ssid[0]) {
        if (strlen(s.ssid) > 20)
          snprintf(value, sizeof(value), "%.17s...", s.ssid);
        else
          strlcpy(value, s.ssid, sizeof(value));
      } else {
        strlcpy(value, v.wifiEnabled ? on : off, sizeof(value));
      }
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
    case SystemSettingField::RgbBrightness:
      snprintf(value, sizeof(value), "%u%%", v.rgbMaxBrightness);
      break;
    case SystemSettingField::RgbFollowSleep:
      strlcpy(value, v.rgbFollowScreenSleep ? on : off, sizeof(value));
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
      snprintf(value, sizeof(value), "%u℃", v.heaterBoardLimitC);
      break;
    case SystemSettingField::Theme:
      // 存储原始值:0=默认 1=IOS 2=蓝白(蓝白按日/夜开始时刻自动切换,
      // 渲染生效值在快照 theme 里由 main.cpp 解析,这里只显示选项名)
      strlcpy(value,
              v.theme == 0 ? (chinese ? "默认" : "DEFAULT")
                           : (v.theme == 1 ? "IOS"
                                           : (chinese ? "蓝白" : "BLUE-WHITE")),
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
      // 编译期无触摸、或运行时探测不到触摸膜(非触摸屏/膜未接),都显示不支持。
      strlcpy(value,
              (!Pin::HAS_TOUCH_PANEL || !s.touchPresent)
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
    case SystemSettingField::Registration:
      // 注册态由 main.cpp 比对 NVS 存储码与 chipId 派生码得出。
      strlcpy(value,
              s.registered ? (chinese ? "已注册" : "REGISTERED")
                           : (chinese ? "未注册" : "NOT SET"),
              sizeof(value));
      break;
    case SystemSettingField::Version:
      // 只读显示编译期写入的版本号,源码见 include/version.h。
      strlcpy(value, FW_VERSION, sizeof(value));
      break;
    case SystemSettingField::ChipId:
      // 只读显示本机唯一芯片 ID(由 eFuse MAC 生成,逐机不重复),
      // 供批量部署时在屏上直接核对型号。
      strlcpy(value, s.chipId, sizeof(value));
      break;
    case SystemSettingField::Count:
      break;
    }
    g.setTextDatum(MR_DATUM);
    g.setTextColor(active ? ACCENT : TEXT, rowBg);
    g.drawString(value, 450, y + 20);
    // WiFi 行联网时,在值文本与行右边之间补一组迷你信号格:与主屏同一套
    // 阈值(pure::rssiBars)与配色(≤1 格红/2 格黄/≥3 格绿),尺寸缩小到
    // 2px 条宽 + 1px 间隙,高度 3/6/9/12,不与右对齐到 x=450 的文本重叠。
    if (static_cast<SystemSettingField>(item) ==
            SystemSettingField::WifiEnabled &&
        s.networkConnected) {
      const uint8_t level = pure::rssiBars(s.rssi);
      const uint16_t barColor = level <= 1 ? G_RED : (level == 2 ? WARN : GOOD);
      const int16_t bottom = y + 31;
      for (uint8_t i = 0; i < 4; ++i) {
        const int16_t bh = 3 + i * 3;
        g.fillRect(453 + i * 3, bottom - bh, 2, bh,
                   i < level ? barColor : MUTED);
      }
    }
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

// 注册码页(模态):标题 + 本机芯片 ID + 8 位候选字符格 + 校验结果 + 操作提示。
// 输入状态(已输字符/当前位/结果)全在 main.cpp,这里只按快照渲染。
void drawRegistration(TFT_eSPI &g, const UiSnapshot &s) {
  const bool zh = s.language == Language::Chinese;
  g.fillScreen(BG);
  g.loadFont(FontCN26);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(TEXT, BG);
  g.drawString(zh ? "注册码输入" : "REGISTRATION", 240, 44);
  g.unloadFont();

  g.loadFont(FontCN16);
  g.setTextColor(MUTED, BG);
  // 芯片 ID 摆在码格上方:厂商按它离线生成一机一码,用户照着屏幕读给厂商。
  char line[40];
  snprintf(line, sizeof(line), "%s %s", zh ? "芯片ID:" : "CHIP ID:", s.chipId);
  g.drawString(line, 240, 86);

  // 8 位字符格:当前位高亮描边,未输入位显示下划线占位。
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 36 + i * 52; // 44px 格宽 + 8px 间隙
    const bool current = (s.regResult == 0 && i == s.regPos);
    g.fillRoundRect(x, 116, 44, 56, 6, PANEL);
    g.drawRoundRect(x, 116, 44, 56, 6, current ? ACCENT : BORDER);
    char ch[2] = {'_', '\0'};
    if (i < s.regPos || s.regResult || (s.regResult == 0 && i == s.regPos))
      ch[0] = s.regBuf[i];
    g.loadFont(FontCN26);
    g.setTextColor(current ? ACCENT : TEXT, PANEL);
    g.drawString(ch, x + 22, 144);
    g.unloadFont();
    g.loadFont(FontCN16);
  }

  // 校验结果:成功绿/失败红,结果态下单击分别进系统或重新输入。
  if (s.regResult == 1) {
    g.setTextColor(GOOD, BG);
    g.drawString(zh ? "注册成功,单击进入系统" : "REGISTERED - CLICK TO START",
                 240, 208);
  } else if (s.regResult == 2) {
    g.setTextColor(G_RED, BG);
    g.drawString(zh ? "注册码错误,单击重新输入" : "WRONG CODE - CLICK TO RETRY",
                 240, 208);
  }

  g.setTextColor(MUTED, BG);
  g.drawString(zh ? "旋转选字符  单击确认  长按跳过"
                  : "TURN: CHAR  CLICK: OK  HOLD: SKIP",
               240, 280);
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

// 触摸校准引导页:第 0 步在左上角、第 1 步在右下角显示十字靶点,
// 配双语操作提示;采样动作由 main.cpp 的 dispatchUiAction 完成。
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

// 整帧绘制总入口,也是页面分发器:故障/校准/耗材设置/系统设置四种全屏
// 模式优先返回,否则画主屏仪表盘(左侧控制区:耗材轮播+三个自动开关+
// 参数;右侧 2x4 实时仪表;底部 5 按钮状态栏)。目标 g 可以是 sprite
// 也可以是直屏 tft,两种路径绘制代码完全相同。
// OTA 屏显横幅的状态(文件级,供渲染任务与 setOtaProgress 跨作用域共享)。
// 主循环写、渲染任务读,均为原子量,无需加锁。
volatile bool gOtaActive = false; // 是否显示 OTA 横幅
volatile uint8_t gOtaPct = 0;     // 进度 0..100

// 在整屏(或 sprite)底部叠加一条 OTA 进度横幅:深色底 + 文字 + 进度块。
// 每帧末尾在 drawFrame 之后调用,双缓冲模式下同样画进帧缓冲并随 push 出现。
void tintOtaBanner(TFT_eSPI &g, bool active, uint8_t pct) {
  if (!active)
    return;
  const int16_t barH = 46;                                   // 横幅高度
  const int16_t y = SCREEN_H - barH;                         // 贴底
  g.fillRoundRect(6, y, SCREEN_W - 12, barH - 8, 6, 0x1082); // 深灰底
  g.drawRoundRect(6, y, SCREEN_W - 12, barH - 8, 6, 0xFFE0); // 黄描边
  // 状态文案与百分比:用项目自带 16px 中英 VLW 字体
  g.loadFont(FontCN16);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(0xFFFF, 0x1082);
  g.drawString("OTA 升级中", 16, y + 10);
  char s[8];
  snprintf(s, sizeof(s), "%u%%", (unsigned)pct);
  g.setTextDatum(MR_DATUM);
  g.drawString(s, SCREEN_W - 14, y + 11);
  g.unloadFont();
  // 中部绿色进度条
  const int16_t px = 16, pw = SCREEN_W - 32, ph = 8, py = y + barH - 24;
  g.drawRoundRect(px, py, pw, ph, 4, 0x7BEF);
  g.fillRoundRect(px, py, (int16_t)((uint32_t)pw * pct / 100), ph, 4, 0x07E0);
}

// 主屏状态点的颜色:与状态灯优先级一致的简化映射(手动强排青、故障红、
// 排气黄、打印蓝、预热紫、检测橙、待机绿)。
uint16_t stateAccent(const UiSnapshot &s) {
  if (s.manualExhaust)
    return G_CYAN;
  switch (s.state) {
  case ChamberState::Fault:
    return G_RED;
  case ChamberState::Cooling:
    return G_YELLOW;
  case ChamberState::Printing:
    return TEXT; // 打印态:蓝底上深蓝不可见,转白色
  case ChamberState::Preheat:
    return 0x78B6; // 预热紫(RGB 120,20,180)
  case ChamberState::Detecting:
    return WARN;
  default:
    return GOOD;
  }
}

// 整屏页面分发函数(入口,负责按快照的分支转发到各页面绘制例程)。
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
  // 注册页是模态整屏:上电未注册时由 main.cpp 拉起,或从设置页"注册码"项进入。
  if (s.registrationActive) {
    drawRegistration(g, s);
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
  g.fillRoundRect(4, 4, 292, 238, 12, PANEL);
  g.drawRoundRect(4, 4, 292, 238, 12, BORDER);
  // 行分隔统一为暗色细线,iOS 分组列表式:与行文字左缘对齐内嵌。
  g.drawFastHLine(16, 60, 274, BORDER);
  g.drawFastHLine(16, 120, 274, BORDER);
  g.drawFastHLine(16, 180, 274, BORDER);
  g.drawFastHLine(16, 239, 274, BORDER);
  // 每行左侧识别色条:排风/仓温/打印后排风(随调色板,须每帧重建)。
  const uint16_t rowAccents[3] = {ACCENT, G_YELLOW, G_CYAN};
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t y = 62 + i * 60;
    g.fillRoundRect(8, y + 9, 3, 38, 1, rowAccents[i]);
  }

  // Right hand 2 x 4 instrument grid: 图标底衬 + 底部微型进度条。
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    const uint16_t c = gaugeColor(i, s);
    g.fillRoundRect(x, y, 87, 56, 9, PANEL);
    g.drawRoundRect(x, y, 87, 56, 9, BORDER);
    g.fillCircle(x + 15, y + 15, 11, PANEL_ALT); // 图标底衬圆片
    drawGaugeIcon(g, static_cast<GaugeIcon>(i), x + 15, y + 15, c);
    // 百分比型仪表(外排/湿度/热风)按实时值填充进度,其余画满宽状态色条。
    g.fillRoundRect(x + 3, y + 50, 81, 4, 2, PANEL_ALT);
    if (i == 0 || i == 2 || i == 4) {
      const float v = i == 0   ? static_cast<float>(s.exhaustPercent)
                      : i == 2 ? s.humidity
                               : static_cast<float>(s.heaterFanPercent);
      if (!isnan(v)) {
        const int w =
            static_cast<int>(constrain(v, 0.0f, 100.0f) * 0.81f + 0.5f);
        if (w > 0)
          g.fillRoundRect(x + 3, y + 50, w, 4, 2, c);
      }
    } else {
      g.fillRoundRect(x + 3, y + 50, 81, 4, 2, c);
    }
  }

  // Bottom five action buttons. 参考图式激活态:底色不变,图标/文字/描边
  // 转为黄色,底部小色条同步黄色。
  for (uint8_t i = 0; i < 5; ++i) {
    const int16_t x = 4 + i * 95;
    const bool active = buttonActive(i, s);
    const bool focused = (i == 1 && s.mainFocus == MainFocus::System) ||
                         (i == 2 && s.mainFocus == MainFocus::Preheat) ||
                         (i == 3 && s.mainFocus == MainFocus::Light) ||
                         (i == 4 && s.mainFocus == MainFocus::Settings);
    g.fillRoundRect(x, 248, 91, 68, 10, PANEL_ALT);
    g.drawRoundRect(x, 248, 91, 68, 10,
                    focused ? TEXT : (active ? G_YELLOW : BORDER));
    if (focused)
      g.drawRoundRect(x + 2, 250, 87, 64, 8, WARN);
    drawBottomIcon(g, static_cast<ButtonIcon>(i), x + 45, 268,
                   active ? G_YELLOW : TEXT);
    if (active) // 激活态底部小色条,强化“此路已开”的直觉
      g.fillRoundRect(x + 24, 309, 43, 3, 1, G_YELLOW);
  }

  // Render all labels with the 16 px Chinese+ASCII VLW font.
  g.loadFont(FontCN16);

  // Material carousel and compact runtime status. 参考图:耗材名与箭头
  // 均为白色,聚焦编辑时转黄色提示。
  g.setTextDatum(MC_DATUM);
  const uint16_t prevColor =
      s.mainFocus == MainFocus::PreviousMaterial ? WARN : TEXT;
  const uint16_t materialColor =
      s.mainFocus == MainFocus::CurrentMaterial ? WARN : TEXT;
  const uint16_t nextColor =
      s.mainFocus == MainFocus::NextMaterial ? WARN : TEXT;
  g.setTextColor(prevColor, PANEL);
  g.drawString(s.previousMaterial, 35, 23);
  g.fillCircle(82, 23, 13, PANEL_ALT); // 箭头底衬,立体按钮感
  g.drawCircle(82, 23, 13, prevColor);
  g.drawString("<", 82, 23);
  g.setTextColor(materialColor, PANEL);
  g.drawString(s.material, 145, 20);
  if (s.mainFocus == MainFocus::CurrentMaterial)
    g.drawFastHLine(122, 32, 46, WARN);
  g.fillCircle(207, 23, 13, PANEL_ALT);
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
  const uint16_t stateColor = stateAccent(s);
  g.fillCircle(15, 46, 4, stateColor); // 状态色点,一眼读出当前状态
  g.setTextColor(stateColor, PANEL);
  g.drawString(s.manualExhaust ? (chinese ? "强制排气" : "MANUAL PURGE")
                               : tx.state,
               24, 46);
  g.setTextDatum(MC_DATUM);
  if (s.netAlert != NetAlert::None) {
    // 有网络告警时,日期位让给告警文案(告警色)。选这里正是因为这一格
    // 不承载任何操作:不影响离线打印、不遮挡焦点,时间也照常在右侧走。
    // 联网图标同时由上方逻辑保持"绿=WiFi 通/灰色带斜杠=断网",两者合起来
    // 足以区分"WiFi 断了"与"WiFi 通但 MQTT 失败"。
    g.setTextColor(WARN, PANEL);
    g.drawString(netAlertText(s), 150, 46);
  } else {
    g.setTextColor(s.networkConnected ? GOOD : MUTED, PANEL);
    g.drawString(dateBuf, 150, 46);
  }
  g.setTextDatum(MR_DATUM);
  g.drawString(timeBuf, 290, 46);
  // WiFi 信号格,紧跟状态文字之后。原来是"通=绿 / 断=灰+斜杠"的二态扇形图标 ——
  // 只反映 networkConnected 这个 bool,信号从满格掉到一格在屏上毫无区别,而
  // REST/MQTT 早就上报 rssi 了。现在按 pure::rssiBars() 画 4 格:
  // 阈值与内置 Web 管理页一致(≥-55 四格 / ≥-65 三格 / ≥-75 两格 / 更弱一格),
  // 弱信号标黄、极弱标红;未联网时四格全空 + 红斜杠,与"连着但很弱"区分开。
  {
    const char *st =
        s.manualExhaust ? (chinese ? "强制排气" : "MANUAL PURGE") : tx.state;
    const int16_t wx = 24 + g.textWidth(st) + 12, wy = 50;
    const uint8_t level =
        s.networkConnected ? pure::rssiBars(s.rssi) : 0; // 未联网恒 0 格
    const uint16_t barColor = level <= 1 ? G_RED : (level == 2 ? WARN : GOOD);
    const int16_t bottom = wy + 10; // 底边:下方卡片自 y=62 起,留 2px 间隙
    for (uint8_t i = 0; i < 4; ++i) {
      const int16_t bx = wx - 9 + i * 5; // 3px 条宽 + 2px 间隙
      const int16_t bh = 4 + i * 3;      // 逐级升高:4 / 7 / 10 / 13
      g.fillRect(bx, bottom - bh, 3, bh, i < level ? barColor : MUTED);
    }
    if (!s.networkConnected) {
      g.drawLine(wx - 10, wy - 5, wx + 10, bottom + 1, G_RED);
      g.drawLine(wx - 9, wy - 5, wx + 11, bottom + 1, G_RED);
    }
  }
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
    g.drawString(chinese ? titlesZh[i] : titlesEn[i], 16, y + 14);
    g.drawString(chinese ? subtitlesZh[i] : subtitlesEn[i], 16, y + 38);
    drawToggle(g, 145, y + 18, toggled[i], G_CYAN); // iOS 式蓝色开关(参考图)
  }

  char line[32];
  g.setTextDatum(MR_DATUM);
  g.setTextColor(MUTED, PANEL);
  snprintf(line, sizeof(line), "%s %u%%", tx.minimum, s.exhaustMinPercent);
  g.drawString(line, 288, 76);
  snprintf(line, sizeof(line), "%s %u%%", tx.maximum, s.exhaustMaxPercent);
  g.drawString(line, 288, 100);
  snprintf(line, sizeof(line), "%s %.0f℃", tx.minimum, s.profileMinC);
  g.drawString(line, 288, 136);
  snprintf(line, sizeof(line), "%s %.0f℃", tx.maximum, s.profileMaxC);
  g.drawString(line, 288, 160);
  snprintf(line, sizeof(line), "%s %u%%", tx.fanSpeed, s.postExhaustPercent);
  g.drawString(line, 288, 196);
  snprintf(line, sizeof(line), "%s %us", chinese ? "时间" : "TIME",
           s.postExhaustSeconds);
  g.drawString(line, 288, 220);

  // 仪表名与单位合成一行底部说明(如 "电压 V"、"主控 ℃"),居中排布。
  const float values[] = {static_cast<float>(s.exhaustPercent),
                          s.mcuC,
                          s.humidity,
                          s.chamberC,
                          static_cast<float>(s.heaterFanPercent),
                          s.heaterBoardC,
                          s.voltageV,
                          s.currentA};
  const char *units[] = {"%", "℃", "%", "℃", "%", "℃", "V", "A"};
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    char cap[24];
    snprintf(cap, sizeof(cap), "%s %s", tx.gauges[i], units[i]);
    g.setTextColor(MUTED, PANEL);
    g.setTextDatum(MC_DATUM);
    g.drawString(cap, x + 43, y + 40);
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
    const bool on = buttonActive(i, s);
    g.setTextColor(on ? G_YELLOW : TEXT, PANEL_ALT);
    g.setTextDatum(MC_DATUM);
    g.drawString(label, x + 45, 299);
  }
  g.unloadFont();

  // Render the eight live values: 大数字(CN26)右对齐、垂直居中于格子
  // 上半区,单位已并入底部仪表名行。数字统一白色,状态色交给图标与
  // 微型进度条;仅热板超限时数字转红。超宽(≥100/负温)自动退为整数。
  g.loadFont(FontCN26);
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 300 + (i & 1) * 89;
    const int16_t y = 4 + (i >> 1) * 60;
    char value[12];
    uint8_t baseDec = (i == 0 || i == 4) ? 0 : 1;
    // 温度超过 100 不再显示小数(如 104.3 → 104),短值保留一位小数。
    if (baseDec && values[i] >= 100.0f)
      baseDec = 0;
    formatValue(value, sizeof(value), values[i], baseDec);
    // 图标底衬右缘约 x+27,数字右缘 x+83,可用 56px。
    if (!isnan(values[i]) && g.textWidth(value) > 56) {
      formatValue(value, sizeof(value), values[i], 0);
    }
    const bool fault = i == 5 && !isnan(s.heaterBoardC) &&
                       s.heaterBoardC >= s.heaterBoardLimitC;
    g.setTextColor(fault ? G_RED : TEXT, PANEL);
    g.setTextDatum(MR_DATUM);
    g.drawString(value, x + 83, y + 15);
  }
  g.unloadFont();
  g.setTextDatum(TL_DATUM);
}

// 渲染任务主体:永久阻塞等队列,拿到快照后——双缓冲模式下选另一块帧缓冲
// 离屏绘制再整屏 push(交替 backBuffer 实现无闪烁);无 sprite 则直接在
// 屏上绘制。队列由主循环 overwrite 投递,本任务只做消费者。
void uiRenderTask(void *) {
  UiSnapshot snapshot{};
  uint8_t backBuffer = 1;
  for (;;) {
    if (xQueueReceive(renderQueue, &snapshot, portMAX_DELAY) != pdTRUE)
      continue;
    if (spriteReady) {
      frame.frameBuffer(backBuffer);
      drawFrame(frame, snapshot);
      tintOtaBanner(frame, gOtaActive, gOtaPct);
      frame.pushSprite(0, 0);
      backBuffer = backBuffer == 1 ? 2 : 1;
    } else {
      drawFrame(tft, snapshot);
      tintOtaBanner(tft, gOtaActive, gOtaPct);
    }
  }
}
} // namespace

// 显示初始化:屏幕横屏 + UTF-8;探测 PSRAM 并尝试创建双缓冲全屏 sprite
// (成功才无闪烁,失败降级直绘);建长度 1 队列并在 core0 创建渲染任务,
// 任一失败都放弃 UI(ready_=false),优先保证控制节拍不受影响。
void TftUi::begin() {
  tft.init();
  tft.setRotation(1);
  tft.setAttribute(UTF8_SWITCH, 1);
  tft.fillScreen(BG);

  // 默认不创建全屏 sprite:本板大块 PSRAM 分配会挂死(实测 ps_calloc 200B 正常、
  // 300KB 起卡住并触发任务看门狗复位,整块设备因此不断重启)。而 PSRAM 已并入
  // 默认堆,走内部 RAM 的 calloc(614KB) 同样会落进 PSRAM 卡住,所以必须整块
  // 跳过分配,而不是只关掉 PSRAM_ENABLE。跳过即落到既定的「直绘降级」分支:
  // 屏幕可用(可能有闪烁),且不再拖累控制节拍与联网。
  // 换板或修好 PSRAM 后,在 platformio.ini 加 -DTFT_SPRITE_IN_PSRAM 恢复双缓冲。
#if defined(TFT_SPRITE_IN_PSRAM)
  if (psramFound()) {
    frame.setColorDepth(16);
    frame.setAttribute(PSRAM_ENABLE, 1);
    spriteReady = frame.createSprite(SCREEN_W, SCREEN_H, 2) != nullptr;
  }
#endif
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

// 投递一帧快照。长度 1 队列 + overwrite:渲染跟不上时旧帧被直接丢弃,
// 只保留最新状态;主循环调用永不阻塞、永不失败,UI 不反压控制任务。
void TftUi::render(const UiSnapshot &snapshot) {
  if (!ready_)
    return;
  // A length-one queue deliberately drops stale frames if SPI is still busy.
  // Rendering is never allowed to fall back into the PID/Arduino loop task.
  xQueueOverwrite(static_cast<QueueHandle_t>(queue_), &snapshot);
}

// 主循环每帧调用,传入 OTA 状态。写文件级原子量,渲染任务下一帧读取。
void TftUi::setOtaProgress(bool active, uint8_t pct) {
  gOtaActive = active;
  gOtaPct = pct;
}
