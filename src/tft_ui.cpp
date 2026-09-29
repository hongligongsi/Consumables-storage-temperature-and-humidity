// 显示层实现:480x320 横屏 TFT 的全部绘制逻辑。main.cpp 每 500ms 生成
// 一份 UiSnapshot 经长度 1 的队列投递给独立 FreeRTOS 渲染任务
// (uiRenderTask,钉在 core0),渲染与热控/PID 完全解耦,绘制再慢也不会
// 拖慢 50ms 控制节拍。有八线 PSRAM 时开双缓冲 sprite(离屏绘制后一次
// push,无闪烁);分配失败则降级为直接写屏。
// 页面分发见 drawFrame:故障页/触摸校准/耗材设置/系统设置/主屏仪表盘。
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
constexpr int16_t SCREEN_W = 480; // 横屏宽(像素)
constexpr int16_t SCREEN_H = 320; // 横屏高(像素)

// 运行期调色板:日/夜两套配色,由 drawFrame 按快照的 theme 选择。
// 一比一还原 printer-hmi-redesign.html:夜间 = 设计稿深色 HMI token
// (青=气流 橙=温度 蓝=湿度 紫=电气,绿/黄/红 = 状态语义);
// 日间 = 同一语义结构的浅色适配。色值均为设计稿 RGB888 换算的 RGB565。
struct Palette {
  uint16_t bg;       // 页面底色
  uint16_t panel;    // 卡片底色
  uint16_t panelAlt; // 次级底色(开关轨道/输入底)
  uint16_t border;   // 分隔线/描边
  uint16_t line2;    // 高一级描边(箭头圆钮/开关关闭态)
  uint16_t muted;    // 次要文字 t2
  uint16_t ink3;     // 三级文字 t3
  uint16_t text;     // 主要文字 t1
  uint16_t active;   // 选中行底色
  uint16_t accent;   // 强调青
  uint16_t warn;     // 注意黄(=语义 amber)
  uint16_t good;     // 正常绿
  uint16_t maroon;   // 故障页标题条
  uint16_t gRed;
  uint16_t gYellow;
  uint16_t gMagenta; // 电气紫
  uint16_t gCyan;
  uint16_t sky;      // 湿度蓝
  uint16_t violet;   // 电气紫(=gMagenta)
  uint16_t orange;   // 温度橙
  uint16_t lamp;     // 灯光黄
};

constexpr Palette kNightPalette = {0x0882, 0x1905, 0x2166, 0x29A8, 0x3A2A,
                                   0xADB8, 0x7C73, 0xEF9E, 0x19E9, 0x3EBC,
                                   0xFDA8, 0x46CF, 0x50C4, 0xFAEB, 0xFDA8,
                                   0xB47F, 0x3EBC, 0x5D5F, 0xB47F, 0xFC47,
                                   0xFE88};

constexpr Palette kDayPalette = {0xEF9E, 0xFFFF, 0xF7BE, 0xDF3D, 0xCEBC,
                                 0x42AD, 0x63B0, 0x1926, 0xD77E, 0x0D57,
                                 0xD402, 0x1D4B, 0x8967, 0xD228, 0xD402,
                                 0x7A78, 0x0D57, 0x2BFA, 0x7A78, 0xEB43,
                                 0xCD01};

// 当前生效配色。渲染任务单线程写,drawFrame 每帧按快照主题刷新。
uint16_t BG = kNightPalette.bg;
uint16_t PANEL = kNightPalette.panel;
uint16_t PANEL_ALT = kNightPalette.panelAlt;
uint16_t BORDER = kNightPalette.border;
uint16_t LINE2 = kNightPalette.line2;
uint16_t MUTED = kNightPalette.muted;
uint16_t INK3 = kNightPalette.ink3;
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
uint16_t SKY = kNightPalette.sky;
uint16_t VIOLET = kNightPalette.violet;
uint16_t ORANGE = kNightPalette.orange;
uint16_t LAMP = kNightPalette.lamp;

// 按主题号(0=日 1=夜)把整套调色板复制到上面的全局颜色变量;
// drawFrame 每帧开头调用,所以切换主题下一帧即生效。
void applyPalette(uint8_t theme) {
  const Palette &p = theme == 0 ? kDayPalette : kNightPalette;
  BG = p.bg;
  PANEL = p.panel;
  PANEL_ALT = p.panelAlt;
  BORDER = p.border;
  LINE2 = p.line2;
  MUTED = p.muted;
  INK3 = p.ink3;
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
  SKY = p.sky;
  VIOLET = p.violet;
  ORANGE = p.orange;
  LAMP = p.lamp;
}

// 前景色以 num/256 不透明度线性混入底色(RGB565 各通道插值)。
// 用于设计稿里的 rgba 半透明:胶囊底 10%、描边 38%、图标盒 12% 等,
// 运行时混合省掉为每个主题硬编码一整套预混合色。
uint16_t mix(uint16_t fg, uint16_t bgc, uint8_t num) {
  const uint32_t r1 = (fg >> 11) & 0x1F, g1 = (fg >> 5) & 0x3F, b1 = fg & 0x1F;
  const uint32_t r2 = (bgc >> 11) & 0x1F, g2 = (bgc >> 5) & 0x3F,
                 b2 = bgc & 0x1F;
  return static_cast<uint16_t>(((r1 * num + r2 * (256 - num) + 128) >> 8)
                                   << 11 |
                               ((g1 * num + g2 * (256 - num) + 128) >> 8) << 5 |
                               ((b1 * num + b2 * (256 - num) + 128) >> 8));
}

TFT_eSPI tft;                        // 底层屏幕驱动(SPI)
TFT_eSprite frame(&tft);             // 全屏离屏画布(双缓冲时两块帧缓冲交替)
QueueHandle_t renderQueue = nullptr; // 长度 1 的快照队列(主循环 → 渲染任务)
bool spriteReady = false;            // 双缓冲 sprite 是否成功建立

// 右侧 2x4 监控 tile 的显示顺序:外排/主控/湿度/仓温/热风/热板/电压/电流,
// 数值数组、texts().gauges 与图标函数都按此下标排列。
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

// 监控 tile 的小尺寸线性图标(11px 内径,对应设计稿 18px 图标的 4/9 缩放),
// 顺序与 GaugeIcon 枚举一致:风扇/芯片/水滴/仓体/风线/热板/闪电/表盘。
void drawTileIcon(TFT_eSPI &g, uint8_t i, int16_t x, int16_t y, uint16_t c) {
  switch (i) {
  case 0: // 外排:风扇
    g.drawCircle(x, y, 5, c);
    g.fillCircle(x, y, 1, c);
    for (uint8_t k = 0; k < 3; ++k) {
      const float a = k * 2.0944f - 1.5708f;
      g.drawLine(x + lroundf(cosf(a) * 2), y + lroundf(sinf(a) * 2),
                 x + lroundf(cosf(a) * 5), y + lroundf(sinf(a) * 5), c);
    }
    break;
  case 4: // 热风:风线
    g.drawFastHLine(x - 5, y - 4, 7, c);
    g.drawFastVLine(x + 2, y - 6, 2, c);
    g.drawFastHLine(x - 5, y, 10, c);
    g.drawFastVLine(x + 5, y - 2, 2, c);
    g.drawFastHLine(x - 5, y + 4, 4, c);
    g.drawFastVLine(x - 1, y + 2, 2, c);
    break;
  case 1: // 主控:芯片
    g.drawRect(x - 4, y - 4, 9, 9, c);
    g.drawRect(x - 1, y - 1, 3, 3, c);
    for (int8_t t = -3; t <= 3; t += 3) {
      g.drawFastVLine(x + t, y - 7, 2, c);
      g.drawFastVLine(x + t, y + 6, 2, c);
      g.drawFastHLine(x - 7, y + t, 2, c);
      g.drawFastHLine(x + 6, y + t, 2, c);
    }
    break;
  case 2: // 湿度:水滴
    g.drawCircle(x, y + 1, 4, c);
    g.drawLine(x, y - 6, x - 4, y - 1, c);
    g.drawLine(x, y - 6, x + 4, y - 1, c);
    break;
  case 3: // 仓温:仓体
    g.drawLine(x - 6, y - 1, x, y - 6, c);
    g.drawLine(x, y - 6, x + 6, y - 1, c);
    g.drawRect(x - 4, y - 1, 9, 7, c);
    break;
  case 5: // 热板:加热床
    g.drawRoundRect(x - 6, y + 1, 13, 4, 2, c);
    for (int8_t t = -4; t <= 4; t += 4)
      g.drawFastVLine(x + t, y - 3, 3, c);
    break;
  case 6: // 电压:闪电
    g.drawLine(x + 2, y - 7, x - 4, y + 1, c);
    g.drawLine(x - 4, y + 1, x, y + 1, c);
    g.drawLine(x, y + 1, x - 2, y + 7, c);
    g.drawLine(x - 2, y + 7, x + 4, y - 1, c);
    g.drawLine(x + 4, y - 1, x + 1, y - 1, c);
    g.drawLine(x + 1, y - 1, x + 2, y - 7, c);
    break;
  case 7: // 电流:表盘
    g.drawCircle(x, y + 2, 6, c);
    g.fillRect(x - 7, y + 3, 15, 6, PANEL); // 抹掉下半圆,只留表盘弧
    g.drawLine(x, y + 2, x + 3, y - 3, c);
    g.fillCircle(x, y + 2, 1, c);
    break;
  }
}

// 左列控制卡的 13px 线性图标(设计稿 22px 图标缩放):风扇/仓体/排风线。
void drawCtlIcon(TFT_eSPI &g, uint8_t i, int16_t x, int16_t y, uint16_t c) {
  switch (i) {
  case 0: // 排气风扇
    g.drawCircle(x, y, 6, c);
    g.fillCircle(x, y, 2, c);
    for (uint8_t k = 0; k < 3; ++k) {
      const float a = k * 2.0944f - 1.5708f;
      g.drawLine(x + lroundf(cosf(a) * 2), y + lroundf(sinf(a) * 2),
                 x + lroundf(cosf(a) * 6), y + lroundf(sinf(a) * 6), c);
    }
    break;
  case 1: // 打印仓温:仓体
    g.drawLine(x - 7, y, x, y - 6, c);
    g.drawLine(x, y - 6, x + 7, y, c);
    g.drawRect(x - 5, y, 10, 7, c);
    break;
  case 2: // 打印结束排气:风线
    g.drawFastHLine(x - 6, y - 4, 8, c);
    g.drawFastVLine(x + 2, y - 6, 2, c);
    g.drawFastHLine(x - 6, y, 11, c);
    g.drawFastVLine(x + 5, y - 2, 2, c);
    g.drawFastHLine(x - 6, y + 4, 5, c);
    g.drawFastVLine(x - 1, y + 2, 2, c);
    break;
  }
}

// 底栏 5 个 14px 线性图标(设计稿:雷达/播放/火焰/灯泡/齿轮)。
void drawNavIcon(TFT_eSPI &g, ButtonIcon icon, int16_t x, int16_t y,
                 uint16_t c) {
  switch (icon) {
  case ButtonIcon::Idle: // 人感雷达
    g.drawCircle(x, y, 6, c);
    g.drawFastVLine(x, y - 10, 3, c);
    g.drawFastVLine(x, y + 7, 3, c);
    g.drawFastHLine(x - 10, y, 3, c);
    g.drawFastHLine(x + 7, y, 3, c);
    g.fillCircle(x, y, 1, c);
    break;
  case ButtonIcon::Print:
    g.fillTriangle(x - 4, y - 6, x - 4, y + 6, x + 7, y, c);
    break;
  case ButtonIcon::Preheat: // 火焰
    g.drawCircle(x, y + 2, 5, c);
    g.drawLine(x - 2, y - 2, x, y - 9, c);
    g.drawLine(x + 2, y - 2, x, y - 9, c);
    break;
  case ButtonIcon::Light: // 灯泡
    g.drawCircle(x, y - 2, 5, c);
    g.drawFastHLine(x - 3, y + 5, 6, c);
    g.drawFastHLine(x - 2, y + 8, 4, c);
    break;
  case ButtonIcon::Settings: // 齿轮
    g.drawCircle(x, y, 6, c);
    g.fillCircle(x, y, 2, c);
    for (uint8_t k = 0; k < 8; ++k) {
      const float a = k * 0.7854f;
      g.drawLine(x + lroundf(cosf(a) * 6), y + lroundf(sinf(a) * 6),
                 x + lroundf(cosf(a) * 9), y + lroundf(sinf(a) * 9), c);
    }
    break;
  }
}

// 滑动开关:48x22 圆角轨道 + 圆点,对应设计稿 56x32 开关的 4/9 缩放。
// 设计稿色彩心智:开启 = 绿色轨道 + 近白圆点;关闭 = 深灰轨道 + 灰圆点。
void drawToggle(TFT_eSPI &g, int16_t x, int16_t y, bool enabled, uint16_t c) {
  g.fillRoundRect(x, y, 48, 22, 11, enabled ? c : PANEL_ALT);
  g.drawRoundRect(x, y, 48, 22, 11, enabled ? c : LINE2);
  g.fillCircle(enabled ? x + 37 : x + 11, y + 11, 9, enabled ? 0xF2FB : INK3);
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

// 仪表家族色:青=气流(外排/热风) 橙=温度(主控/仓温/热板) 蓝=湿度 紫=电气,
// 对应设计稿 f-cyan / f-orange / f-sky / f-violet 四个物理量家族。
uint16_t familyColor(uint8_t index) {
  // 不能是 static:调色板随日夜切换,数组须每帧重建。
  const uint16_t colors[] = {ACCENT, ORANGE, SKY,    ORANGE,
                             ACCENT, ORANGE, VIOLET, VIOLET};
  return colors[index];
}

// 仪表状态灯:0 正常绿 / 1 注意黄 / 2 告警红 / -1 传感器无效(灰点)。
// 阈值取自设计稿 st() 语义(外排 70/88、主控 55/64、湿度 55、仓温 32、
// 热风 70/88、热板 70/78、电压 23.6~24.9、电流 9/10.2);
// 热板超过固件保护限值时无条件告警。
int8_t gaugeStatus(uint8_t index, const UiSnapshot &s) {
  const float values[] = {static_cast<float>(s.exhaustPercent), s.mcuC,
                          s.humidity,
                          s.chamberC,
                          static_cast<float>(s.heaterFanPercent),
                          s.heaterBoardC, s.voltageV, s.currentA};
  const float v = values[index];
  if (isnan(v))
    return -1;
  switch (index) {
  case 0: return v < 70 ? 0 : v < 88 ? 1 : 2;
  case 1: return v < 55 ? 0 : v < 64 ? 1 : 2;
  case 2: return v <= 55 ? 0 : 1;
  case 3: return v < 32 ? 0 : 1;
  case 4: return v < 70 ? 0 : v < 88 ? 1 : 2;
  case 5:
    if (!isnan(s.heaterBoardC) && s.heaterBoardC >= s.heaterBoardLimitC)
      return 2;
    return v < 70 ? 0 : v < 78 ? 1 : 2;
  case 6: return (v > 23.6f && v < 24.9f) ? 0 : 1;
  default: return v < 9 ? 0 : v < 10.2f ? 1 : 2;
  }
}

// 量程条填充百分比:与设计稿 full() 映射一致,按工程量程归一化
// (仓温 0-50℃、热板 0-120℃、电压 18-30V、电流 0-15A)。
uint8_t gaugeFill(uint8_t index, float v) {
  if (isnan(v))
    return 0;
  float p;
  switch (index) {
  case 0: case 1: case 2: case 4: p = v; break;
  case 3: p = v * 2; break;
  case 5: p = v / 1.2f; break;
  case 6: p = (v - 18) / 12 * 100; break;
  default: p = v / 15 * 100; break;
  }
  return static_cast<uint8_t>(constrain(p, 0.0f, 100.0f) + 0.5f);
}

// 底部前 4 个按钮的"激活态"来源各不相同:0=PIR 有人、1=系统使能、
// 2=预热中、3=灯亮;第 4 个(设置入口)无激活态。
bool buttonActive(uint8_t i, const UiSnapshot &s) {
  return (i == 0 && s.pirMotion) || (i == 1 && s.systemEnabled) ||
         (i == 2 && s.preheat) || (i == 3 && s.light);
}

// 故障全屏页:三角警告符 + 传感器错误/系统已停止提示,底部逐一点名
// AHT20/NTC/INA226 三个传感器的在线状态(绿点/红点),便于现场排查。
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
      snprintf(value, sizeof(value), "%u℃", v.heaterBoardLimitC);
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

// 顶部状态胶囊颜色:手动强排青、故障红、检测/排气黄、预热橙,其余绿。
uint16_t statusColor(const UiSnapshot &s) {
  if (s.manualExhaust)
    return ACCENT;
  switch (s.state) {
  case ChamberState::Fault:
    return G_RED;
  case ChamberState::Cooling:
  case ChamberState::Detecting:
    return WARN;
  case ChamberState::Preheat:
    return ORANGE;
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

  // 一比一还原 printer-hmi-redesign.html:1080x720 设计稿按 4/9 等比缩放。
  // 顶栏信息带(6,6,468x44) | 左列 3 行控制卡(6,56,247x186,行高 62) |
  // 右侧 2x4 监控矩阵(105x42,起 x259) | 底栏 5 键(88x66,y248)。
  // 卡片锚点同时贴合触摸热区:材料箭头 x58-106/x184-230(y<60)、
  // 中间 x108-182 打开耗材设置、开关列 x132-208(y62-244)、底栏 y>=246。
  g.fillRoundRect(6, 6, 468, 44, 8, PANEL);
  g.drawRoundRect(6, 6, 468, 44, 8, BORDER);
  g.fillRoundRect(6, 56, 247, 186, 8, PANEL);
  g.drawRoundRect(6, 56, 247, 186, 8, BORDER);
  g.drawFastHLine(18, 118, 222, BORDER);
  g.drawFastHLine(18, 180, 222, BORDER);

  const float values[] = {static_cast<float>(s.exhaustPercent),
                          s.mcuC,
                          s.humidity,
                          s.chamberC,
                          static_cast<float>(s.heaterFanPercent),
                          s.heaterBoardC,
                          s.voltageV,
                          s.currentA};
  const char *units[] = {"%", "℃", "%", "℃", "%", "℃", "V", "A"};
  int16_t unitX[8];

  // 监控矩阵卡壳 + 状态灯点 + 量程条(设计稿 tile:状态灯与量程条双编码)。
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 259 + (i & 1) * 110;
    const int16_t y = 56 + (i >> 1) * 48;
    const int8_t st = gaugeStatus(i, s);
    const uint16_t stC =
        st < 0 ? INK3 : st == 0 ? GOOD : st == 1 ? WARN : G_RED;
    g.fillRoundRect(x, y, 105, 42, 7, PANEL);
    g.drawRoundRect(x, y, 105, 42, 7, BORDER);
    g.fillCircle(x + 97, y + 7, 2, stC); // 状态灯:绿/黄/红,无效灰
    g.fillRoundRect(x + 7, y + 37, 91, 3, 1, mix(TEXT, PANEL, 13));
    const uint8_t fill = gaugeFill(i, values[i]);
    if (fill > 0) {
      const uint16_t fam = familyColor(i);
      g.fillRoundRect(x + 7, y + 37, fill * 91 / 100, 3, 1,
                      st == 2 ? G_RED : st == 1 ? WARN : fam);
    }
  }

  // 底栏 5 键:激活态按设计稿语义配色(绿=开启/橙=预热/黄=灯光),
  // 激活时底/边/图标/文字/底部色条同转该色。
  for (uint8_t i = 0; i < 5; ++i) {
    const int16_t x = 6 + i * 95;
    const bool active = buttonActive(i, s);
    const uint16_t c = i == 2 ? ORANGE : i == 3 ? LAMP : GOOD;
    g.fillRoundRect(x, 248, 88, 66, 7, active ? mix(c, PANEL, 26) : PANEL);
    g.drawRoundRect(x, 248, 88, 66, 7, active ? mix(c, PANEL, 97) : BORDER);
    const bool focused = (i == 1 && s.mainFocus == MainFocus::System) ||
                         (i == 2 && s.mainFocus == MainFocus::Preheat) ||
                         (i == 3 && s.mainFocus == MainFocus::Light) ||
                         (i == 4 && s.mainFocus == MainFocus::Settings);
    if (focused)
      g.drawRoundRect(x + 2, 250, 84, 62, 5, WARN);
    drawNavIcon(g, static_cast<ButtonIcon>(i), x + 44, 268, active ? c : INK3);
    if (active) // 设计稿底栏 ::after 小色条
      g.fillRoundRect(x + 39, 300, 11, 3, 1, c);
  }

  // ---- 文本层 1:16px 中英 VLW ----
  g.loadFont(FontCN16);

  // 顶部信息带·材料轮播(锚定触摸热区);箭头为线性 chevron,
  // 当前材料为青字胶囊(10% 青底 + 38% 青边),编码器聚焦转黄。
  g.setTextDatum(MC_DATUM);
  g.setTextColor(
      s.mainFocus == MainFocus::PreviousMaterial ? WARN : MUTED, PANEL);
  g.drawString(s.previousMaterial, 35, 17);
  g.drawCircle(82, 17, 10,
               s.mainFocus == MainFocus::PreviousMaterial ? WARN : LINE2);
  g.drawLine(85, 12, 79, 17,
             s.mainFocus == MainFocus::PreviousMaterial ? WARN : MUTED);
  g.drawLine(79, 17, 85, 22,
             s.mainFocus == MainFocus::PreviousMaterial ? WARN : MUTED);
  {
    const bool curFocus = s.mainFocus == MainFocus::CurrentMaterial;
    const uint16_t chipBg =
        curFocus ? mix(WARN, PANEL, 26) : mix(ACCENT, PANEL, 26);
    const uint16_t chipBd =
        curFocus ? mix(WARN, PANEL, 97) : mix(ACCENT, PANEL, 97);
    const int16_t cw = g.textWidth(s.material) + 18;
    g.fillRoundRect(145 - cw / 2, 7, cw, 20, 5, chipBg);
    g.drawRoundRect(145 - cw / 2, 7, cw, 20, 5, chipBd);
    g.setTextColor(curFocus ? WARN : ACCENT, chipBg);
    g.drawString(s.material, 145, 17);
  }
  g.drawCircle(207, 17, 10,
               s.mainFocus == MainFocus::NextMaterial ? WARN : LINE2);
  g.drawLine(204, 12, 210, 17,
             s.mainFocus == MainFocus::NextMaterial ? WARN : MUTED);
  g.drawLine(210, 17, 204, 22,
             s.mainFocus == MainFocus::NextMaterial ? WARN : MUTED);
  g.setTextColor(MUTED, PANEL);
  g.drawString(s.nextMaterial, 260, 17);

  // 顶部信息带·状态胶囊:状态色点 + 状态文字 + WiFi 信号图标,
  // 胶囊底/边为状态色 10%/35% 混合(设计稿 pill + 呼吸灯)。
  const uint16_t sc = statusColor(s);
  const uint16_t pillBg = mix(sc, PANEL, 26);
  const char *st =
      s.manualExhaust ? (chinese ? "强制排气" : "MANUAL PURGE") : tx.state;
  const int16_t stw = g.textWidth(st);
  g.fillRoundRect(12, 27, stw + 56, 20, 10, pillBg);
  g.drawRoundRect(12, 27, stw + 56, 20, 10, mix(sc, PANEL, 89));
  g.fillCircle(21, 37, 2, sc);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(sc, pillBg);
  g.drawString(st, 27, 37);
  {
    // WiFi 扇形信号(缩小到 r7 以放进胶囊):联网绿,断网灰加红斜杠。
    const int16_t wx = 27 + stw + 14, wy = 37;
    const uint16_t wc = s.networkConnected ? GOOD : MUTED;
    g.drawCircle(wx, wy, 7, wc);
    g.fillRect(wx - 8, wy + 1, 17, 7, pillBg); // 抹掉下半圆,只留上弧
    g.drawCircle(wx, wy, 4, wc);
    g.fillRect(wx - 5, wy + 1, 11, 4, pillBg);
    g.fillCircle(wx, wy, 2, wc);
    if (!s.networkConnected) {
      g.drawLine(wx - 5, wy - 6, wx + 6, wy + 5, G_RED);
      g.drawLine(wx - 4, wy - 6, wx + 7, wy + 5, G_RED);
    }
  }

  // 顶部信息带·时钟日期(时间在 26px 数字层绘制,右对齐)。
  char dateBuf[16];
  char timeBuf[16];
  formatClockField(dateBuf, sizeof(dateBuf), s.clock, true, false, 0);
  formatClockField(timeBuf, sizeof(timeBuf), s.clock, false, false, 0);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(INK3, PANEL);
  g.drawString(dateBuf, 468, 39);

  // 左列 3 行控制卡:族色识别条 + 图标盒 + 标题 + 参数行 + 绿色开关。
  // 设计稿的 ± 步进按钮不还原(参数编辑走 EC11/耗材设置页),
  // 参数就地显示为一行区间文本。
  const bool toggled[] = {s.autoExhaust, s.autoTemperature, s.postPrintExhaust};
  const char *titlesZh[] = {"排气风扇", "打印仓温", "打印结束"};
  const char *titlesEn[] = {"EXHAUST FAN", "CHAMBER TEMP", "PRINT FINISH"};
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t y = 56 + i * 62;
    const bool focused = static_cast<uint8_t>(s.mainFocus) ==
                         static_cast<uint8_t>(MainFocus::AutoExhaust) + i;
    if (focused)
      g.drawRoundRect(8, y + 2, 243, 58, 6, WARN);
    const uint16_t fam = i == 1 ? WARN : ACCENT; // 设计稿:行2 琥珀,行1/3 青
    g.fillRoundRect(6, y + 4, 2, 54, 1, fam);
    g.fillRoundRect(15, y + 21, 20, 20, 5, mix(fam, PANEL, 31));
    drawCtlIcon(g, i, 25, y + 31, fam);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(TEXT, PANEL);
    g.drawString(chinese ? titlesZh[i] : titlesEn[i], 41, y + 18);
    char par[24];
    if (i == 0)
      snprintf(par, sizeof(par), "%u%%~%u%%", s.exhaustMinPercent,
               s.exhaustMaxPercent);
    else if (i == 1)
      snprintf(par, sizeof(par), "%.0f~%.0f℃", s.profileMinC, s.profileMaxC);
    else
      snprintf(par, sizeof(par), "%u%%·%us", s.postExhaustPercent,
               s.postExhaustSeconds);
    g.setTextColor(MUTED, PANEL);
    g.drawString(par, 41, y + 37);
    drawToggle(g, 145, y + 20, toggled[i], GOOD);
  }

  // 监控矩阵:族色小图标 + 标签。
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 259 + (i & 1) * 110;
    const int16_t y = 56 + (i >> 1) * 48;
    drawTileIcon(g, i, x + 9, y + 7, familyColor(i));
    g.setTextDatum(ML_DATUM);
    g.setTextColor(MUTED, PANEL);
    g.drawString(tx.gauges[i], x + 17, y + 7);
  }

  // 底栏按钮文字(动态状态文案,激活态转语义色)。
  for (uint8_t i = 0; i < 5; ++i) {
    const int16_t x = 6 + i * 95;
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
    const uint16_t c = i == 2 ? ORANGE : i == 3 ? LAMP : GOOD;
    g.setTextColor(on ? c : MUTED, on ? mix(c, PANEL, 26) : PANEL);
    g.setTextDatum(MC_DATUM);
    g.drawString(label, x + 44, 287);
  }
  g.unloadFont();

  // ---- 文本层 2:26px 数字(大时钟 + 族色读数) ----
  g.loadFont(FontCN26);
  g.setTextColor(TEXT, PANEL);
  g.setTextDatum(MR_DATUM);
  g.drawString(timeBuf, 468, 16);
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t x = 259 + (i & 1) * 110;
    const int16_t y = 56 + (i >> 1) * 48;
    char value[12];
    uint8_t baseDec = (i == 0 || i == 4) ? 0 : 1;
    // 温度超过 100 不再显示小数(如 104.3 → 104),短值保留一位小数。
    if (baseDec && values[i] >= 100.0f)
      baseDec = 0;
    formatValue(value, sizeof(value), values[i], baseDec);
    if (!isnan(values[i]) && g.textWidth(value) > 73)
      formatValue(value, sizeof(value), values[i], 0); // 超宽兜底取整
    const bool breach = i == 5 && !isnan(s.heaterBoardC) &&
                        s.heaterBoardC >= s.heaterBoardLimitC;
    g.setTextColor(breach ? G_RED : familyColor(i), PANEL);
    g.setTextDatum(ML_DATUM);
    g.drawString(value, x + 8, y + 25);
    unitX[i] = x + 8 + g.textWidth(value) + 3;
  }
  g.unloadFont();

  // ---- 文本层 3:16px 单位后缀(跟随读数尾部) ----
  g.loadFont(FontCN16);
  g.setTextDatum(ML_DATUM);
  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t y = 56 + (i >> 1) * 48;
    g.setTextColor(INK3, PANEL);
    g.drawString(units[i], unitX[i], y + 25);
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
