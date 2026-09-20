#include "tft_ui.h"
#include <TFT_eSPI.h>

namespace {
TFT_eSPI tft;
const char *stateName(ChamberState state) {
  switch (state) {
    case ChamberState::Idle: return "READY";
    case ChamberState::Detecting: return "DETECTING";
    case ChamberState::Preheat: return "PREHEAT";
    case ChamberState::Printing: return "PRINTING";
    case ChamberState::Cooling: return "EXHAUST";
    case ChamberState::Fault: return "FAULT";
  }
  return "UNKNOWN";
}
void card(int x, int y, const char *label, float v, const char *unit, uint16_t color) {
  tft.drawRoundRect(x, y, 88, 58, 4, TFT_DARKGREY);
  tft.drawFastHLine(x + 2, y + 55, 84, color);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK); tft.drawString(label, x + 5, y + 4, 2);
  tft.setTextColor(color, TFT_BLACK); tft.drawFloat(v, 0, x + 51, y + 20, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK); tft.drawString(unit, x + 68, y + 37, 2);
}
void toggle(int y, const char *label, bool enabled, float v, const char *unit) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.drawString(label, 12, y, 2);
  tft.fillRoundRect(120, y - 2, 46, 20, 10, enabled ? TFT_DARKCYAN : TFT_DARKGREY);
  tft.fillCircle(enabled ? 156 : 130, y + 8, 8, TFT_WHITE);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.drawFloat(v, 0, 230, y - 4, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK); tft.drawString(unit, 276, y + 7, 2);
}
}

void TftUi::begin() {
  tft.init();
  tft.setRotation(1); // 480 x 320 landscape，与主界面参考图一致
  tft.fillScreen(TFT_NAVY);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString("SmartChamber-3D", 240, 6, 2);
  tft.drawFastHLine(6, 27, 468, TFT_DARKGREY);
  ready_ = true;
}

void TftUi::render(const UiSnapshot &s) {
  if (!ready_) return;
  tft.fillRect(2, 30, 476, 288, TFT_NAVY);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.drawString("<", 12, 38, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.drawCentreString(s.material, 145, 39, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.drawString(">", 267, 38, 4);
  tft.setTextColor(s.state == ChamberState::Fault ? TFT_RED : TFT_ORANGE, TFT_BLACK);
  tft.drawRightString(stateName(s.state), 288, 80, 2);
  toggle(92, "EXHAUST AUTO", true, s.exhaustPercent, "%");
  toggle(132, "TEMP AUTO", true, s.chamberC, "C");
  toggle(172, "POST EXHAUST", true, s.heatPercent, "%");
  card(296, 34, "EXHAUST", s.exhaustPercent, "%", TFT_RED);
  card(388, 34, "MCU", s.mcuC, "C", TFT_GREEN);
  card(296, 95, "HUMID", s.humidity, "%", TFT_YELLOW);
  card(388, 95, "CHAMBER", s.chamberC, "C", TFT_ORANGE);
  card(296, 156, "HOT FAN", s.heaterFanPercent, "%", TFT_MAGENTA);
  card(388, 156, "HEATER", s.heaterBoardC, "C", TFT_ORANGE);
  card(296, 217, "VOLTAGE", s.voltageV, "V", TFT_BLUE);
  card(388, 217, "CURRENT", s.currentA, "A", TFT_GREEN);
  const char *buttons[] = {"IDLE", "WORK", "PREHEAT", s.light ? "LIGHT ON" : "LIGHT OFF", "SETTINGS"};
  for (int i = 0; i < 5; ++i) {
    const int x = 5 + i * 57;
    tft.drawRoundRect(x, 266, 52, 48, 4, i == 1 ? TFT_ORANGE : TFT_DARKGREY);
    tft.setTextColor(i == 1 ? TFT_ORANGE : TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(buttons[i], x + 26, 283, 2);
  }
}
