#pragma once
#include <Arduino.h>
#include "controller.h"

// 所有可在“系统设置”页修改的参数；save() 后写入 ESP32 NVS。
struct SystemSettings {
  Language language = Language::Chinese;
  bool keySound = true;
  uint8_t brightness = 80;                 // 1..100 %
  uint16_t screenSleepSeconds = 60;        // 0 = never sleep
  bool keepScreenOnPrinting = true;
  bool encoderReversed = false;
  uint16_t pirStartSeconds = 25;
  uint16_t pirStopSeconds = 50;
  bool lightOnPrinting = true;
  bool lightOffAfterPrinting = true;
  bool beepOnStart = true;
  bool beepOnStop = true;
  uint8_t heaterMaxCurrentA = 6;           // UI/电流采样校准完成后实施闭环限流
  uint8_t heaterFanPercent = 100;
  uint16_t heaterBoardLimitC = 80;
  // ---- 联网功能 ----
  bool wifiEnabled = true;                 // 关闭则完全不初始化 WiFi(离线运行)
  bool mqttEnabled = false;                 // 是否上报/订阅 MQTT
  char mqttBroker[64] = "";                // MQTT broker 主机名/IP
  uint16_t mqttPort = 1883;                // MQTT 端口
  char mqttTopicPrefix[24] = "chamber";    // 主题前缀,实际主题如 chamber/state、chamber/cmd
  bool ntpEnabled = true;                  // NTP 时间同步
  bool otaEnabled = true;                  // OTA 固件升级
};

class SettingsStore {
 public:
  SystemSettings load();
  bool save(const SystemSettings &settings);
  bool reset();
};
