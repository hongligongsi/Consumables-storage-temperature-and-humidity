#pragma once
#include "controller.h"
#include "settings.h"
#include "ui_model.h"
#include <Arduino.h>

// 联网总管理:WiFi(非阻塞 AP 配网门户)+ WebServer REST API +
// MQTT 上报/订阅 + NTP 时间同步 + ArduinoOTA 固件升级。
// 全部在主循环 loop() 内轮询,单线程,不阻塞 50ms PID 热控节拍。
enum class NetState : uint8_t {
  Off,          // wifiEnabled=false 或 begin() 未调用
  Connecting,   // STA 正在连接
  Connected,    // STA 已连,但 MQTT 未连或未启用
  ConfigPortal, // WiFiManager 配网门户活跃(AP 模式)
  MqttConnected // STA + MQTT 均连上
};

class NetworkManager {
public:
  void begin(ChamberController &controller, UiModel &ui,
             SystemSettings &settings);
  void loop();
  // 主循环每周期喂数据;Web/MQTT/状态查询据此返回。Readings 已含
  // voltage/current,无需再传。
  void updateReadings(const Readings &r, const Outputs &o, float humidity);
  // 设置页改动 wifi/mqtt/ntp/ota 后调用,触发重连/重配。
  void onSettingsChanged(const SystemSettings &settings);

  NetState state() const { return state_; }
  bool connected() const {
    return state_ == NetState::Connected || state_ == NetState::MqttConnected;
  }
  String ipString() const;   // STA IP "x.x.x.x",未连返回 ""
  String timeString() const; // NTP "HH:MM:SS",未同步返回 ""
  const char *hostname() const { return "chamber"; }

  // 由静态回调(WiFi 事件 / MQTT)转发,公开以便 free function 经 g_self 调用。
  void notifyStaConnected() { wifiJustConnected_ = true; }
  void notifyStaDisconnected(uint8_t reason) {
    lastDisconnectReason_ = reason;
    wifiJustDisconnected_ = true;
  }
  void handleMqttCommand(const char *payload);

private:
  ChamberController *controller_ = nullptr;
  UiModel *ui_ = nullptr;
  SystemSettings settings_{};
  SystemSettings *sharedSettings_ = nullptr;
  NetState state_ = NetState::Off;
  bool wmStarted_ = false;      // WiFiManager.autoConnect 已调用过
  bool serverStarted_ = false;  // WebServer 已 begin
  bool otaStarted_ = false;     // ArduinoOTA.begin 已调用
  bool mqttConfigured_ = false; // PubSubClient 已 setServer/订阅
  bool ntpStarted_ = false;     // configTime 已调用
  bool mdnsStarted_ = false;    // mDNS 已在 STA 下启动
  SettingsStore settingsStore_;
  uint32_t lastMqttPublish_ = 0;
  uint8_t lastChamberState_ = 0xff; // 检测状态变化上报 event
  Readings readings_{};
  Outputs outputs_{};
  float humidity_ = NAN;
  // WiFi 事件回调置位,loop() 内处理,避免 wifi 任务与主循环竞争。
  volatile bool wifiJustConnected_ = false;
  volatile bool wifiJustDisconnected_ = false;
  volatile uint8_t lastDisconnectReason_ = 0;
  uint32_t disconnectCount_ = 0;
  bool configPortalWasActive_ = false;
  uint32_t portalRetryAtMs_ = 0;

  struct RateLimitSlot {
    uint32_t ip = 0;
    uint32_t lastReadMs = 0;
    uint32_t lastWriteMs = 0;
  };
  RateLimitSlot rateSlots_[4]{};

  void startWifiManager();
  void startWebServer();
  void startMqtt();
  void startNtp();
  void startOta();
  void applyIpConfig();
  bool allowApiRequest(bool write);
  void sendRateLimited();
  String otaPassword() const;
  bool dispatchAction(const String &name);
  void setProfile(size_t index);
  void publishState();
  void publishEvent(const char *type);
  String buildStateJson() const;
  String buildSettingsJson() const;
  const char *stateName(ChamberState s) const;
};

extern NetworkManager network;
