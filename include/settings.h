#pragma once
#include "controller.h"
#include <Arduino.h>


// 所有可在“系统设置”页修改的参数；save() 后写入 ESP32 NVS。
struct SystemSettings {
  Language language = Language::Chinese;
  bool keySound = true;
  uint8_t brightness = 80;          // 1..100 %
  uint16_t screenSleepSeconds = 60; // 0 = never sleep
  bool keepScreenOnPrinting = true;
  uint8_t rgbMaxBrightness = 100; // RGB 状态灯带(4 颗串联)最大亮度 1-100%,
                                  // 对四颗统一缩放
  bool rgbFollowScreenSleep = false; // 开启后屏幕休眠时 RGB 灯也关闭,唤醒恢复
  bool encoderReversed = false;
  uint16_t pirStartSeconds = 25;
  uint16_t pirStopSeconds = 50;
  bool lightOnPrinting = true;
  bool lightOffAfterPrinting = true;
  bool beepOnStart = true;
  bool beepOnStop = true;
  uint8_t heaterMaxCurrentA = 6; // UI/电流采样校准完成后实施闭环限流
  uint8_t heaterFanPercent = 100;
  uint16_t heaterBoardLimitC = 150;
  uint16_t touchXMin = 300;
  uint16_t touchXMax = 3800;
  uint16_t touchYMin = 300;
  uint16_t touchYMax = 3800;
  bool touchCalibrated = false;
  // ---- 联网功能 ----
  bool wifiEnabled = true;  // 关闭则完全不初始化 WiFi(离线运行)
  bool mqttEnabled = false; // 是否上报/订阅 MQTT
  char mqttBroker[64] = ""; // MQTT broker 主机名/IP
  uint16_t mqttPort = 1883; // MQTT 端口
  char mqttTopicPrefix[24] =
      "chamber";          // 主题前缀,实际主题如 chamber/state、chamber/cmd
  bool ntpEnabled = true; // NTP 时间同步
  bool otaEnabled = true; // OTA 固件升级
  // POSIX TZ 字符串。中国标准时间为 CST-8（POSIX 的符号与 UTC 偏移相反）。
  char timezone[48] = "CST-8";
  char ntpServer1[64] = "ntp.aliyun.com";
  char ntpServer2[64] = "pool.ntp.org";
  // 可选静态 IPv4；关闭时使用 DHCP。地址使用点分十进制文本，便于 Web 配置。
  bool staticIpEnabled = false;
  char staticIp[16] = "192.168.1.50";
  char staticGateway[16] = "192.168.1.1";
  char staticSubnet[16] = "255.255.255.0";
  char staticDns1[16] = "223.5.5.5";
  char staticDns2[16] = "1.1.1.1";
  // ---- 主题与时间 ----
  uint8_t theme = 2;                 // 0=默认(iOS 浅色),1=IOS(深色 HMI),
                                     // 2=蓝白(日/夜自动,出厂默认)
  uint16_t dayStartMinutes = 360;    // 日间开始时刻(分钟,0-1439),默认 6:00
  uint16_t nightStartMinutes = 1080; // 夜间开始时刻(分钟,0-1439),默认 18:00
  uint32_t manualClockEpoch =
      0; // 手动校时 epoch;0=未设置,重启后 NTP 未同步时回退
};

// NVS 故障记忆:最近一次故障的档案(与 SystemSettings 分开存放,恢复出厂
// 时一并清除)。code 是 FaultCode 的枚举值(0=从未锁过故障);latched 表示
// 锁定是否仍未解除 —— 断电重启后 main.cpp 据此恢复热类故障的 Fault 锁定,
// 防止拔电绕过保护;count 为累计锁定次数;epoch 为最近一次锁定时刻
// (main.cpp 的 currentEpoch(),时钟不可用时为 0)。
// 解除锁定(长按复位/关系统)只清 latched,code/count/epoch 留作排障历史。
struct FaultRecord {
  uint8_t code = 0;
  bool latched = false;
  uint16_t count = 0;
  uint32_t epoch = 0;
};

class SettingsStore {
public:
  SystemSettings load();
  bool save(const SystemSettings &settings);
  void loadMaterialProfiles();
  bool saveMaterialProfiles();
  // 故障记忆读写:load 在键不存在时返回全零默认(从未锁过故障)。
  FaultRecord loadFaultRecord();
  bool saveFaultRecord(const FaultRecord &record);
  // 注册码读写:独立于 SystemSettings,恢复出厂时**刻意保留**(reset 的键
  // 清单不含 "regCode")。设备是否已注册由调用方用 pure::regCodeFromChipId
  // 与存储值比对得出,这里只管存取。load 无记录返回 false 且 out[0]='\0'。
  bool loadRegistration(char *out, size_t cap);
  bool saveRegistration(const char *code);
  bool reset();
};
