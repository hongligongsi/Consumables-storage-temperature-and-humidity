#include "network.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sntp.h>
#include <time.h>

// 静态分发:WiFi 事件回调与 MQTT 回调跑在 wifi 任务,只经 self_ 转发到实例方法,
// 实例方法内只更新 volatile 标志或排入 loop() 处理,不直接动共享状态。
static NetworkManager *g_self = nullptr;
static WiFiManager g_wm;
static WebServer g_server(81);
static WiFiClient g_wifiClient;
static PubSubClient g_mqtt(g_wifiClient);

NetworkManager network;

static const char *kApSsid = "FilamentChamber-Setup";
static const uint32_t MQTT_PUBLISH_MS = 5000;
static const uint32_t MQTT_RECONNECT_MS = 5000;

// ---------- 动作名 <-> UiAction 映射 ----------
struct ActionMap {
  const char *name;
  UiAction action;
};
static const ActionMap kActions[] = {
    {"prevMaterial", UiAction::PreviousMaterial},
    {"nextMaterial", UiAction::NextMaterial},
    {"toggleAutoExhaust", UiAction::ToggleAutoExhaust},
    {"toggleAutoTemp", UiAction::ToggleAutoTemperature},
    {"togglePostExhaust", UiAction::TogglePostPrintExhaust},
};
static const size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

static bool parseAction(const String &s, UiAction &out) {
  for (size_t i = 0; i < kActionCount; ++i)
    if (s == kActions[i].name) {
      out = kActions[i].action;
      return true;
    }
  return false;
}

// 极简 JSON 串提取 value(无引号转义需求,输入来自可信端)。
static String jsonExtractString(const char *json, const char *key) {
  String k = String("\"") + key + "\":\"";
  const int p = String(json).indexOf(k);
  if (p < 0)
    return "";
  const int v0 = p + k.length();
  const int v1 = String(json).indexOf('"', v0);
  if (v1 < 0)
    return "";
  return String(json).substring(v0, v1);
}
static long jsonExtractInt(const char *json, const char *key, long fallback) {
  String k = String("\"") + key + "\":";
  const int p = String(json).indexOf(k);
  if (p < 0)
    return fallback;
  return String(json).substring(p + k.length()).toInt();
}
static bool parseBoolText(const String &value, bool fallback) {
  if (value == "1" || value == "true" || value == "on")
    return true;
  if (value == "0" || value == "false" || value == "off")
    return false;
  return fallback;
}

// ---------- WiFi 事件回调(只置标志) ----------
static void onWifiEvent(WiFiEvent_t event) {
  if (!g_self)
    return;
  switch (event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    g_self->notifyStaConnected();
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    g_self->notifyStaDisconnected();
    break;
  default:
    break;
  }
}

// ---------- MQTT 回调 ----------
static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  if (!g_self || len == 0)
    return;
  char message[513];
  const unsigned int copyLen =
      min(len, static_cast<unsigned int>(sizeof(message) - 1));
  memcpy(message, payload, copyLen);
  message[copyLen] = '\0';
  g_self->handleMqttCommand(message);
}

// ============================================================
// NetworkManager
// ============================================================
void NetworkManager::begin(ChamberController &controller, UiModel &ui,
                           const SystemSettings &settings) {
  controller_ = &controller;
  ui_ = &ui;
  settings_ = settings;
  g_self = this;
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  if (!settings_.wifiEnabled) {
    state_ = NetState::Off;
    return;
  }

  startWifiManager();
  state_ = NetState::Connecting;
  Serial.printf("[NET] WiFiManager started, AP fallback SSID=%s\n", kApSsid);
}

void NetworkManager::startWifiManager() {
  g_wm.setDebugOutput(false);
  g_wm.setConfigPortalBlocking(false); // 非阻塞:不卡 50ms PID 循环
  g_wm.setConnectTimeout(15);          // STA 连接超时秒
  g_wm.setConfigPortalTimeout(180);    // 门户 3 分钟无操作自动关闭重试
  g_wm.setSaveConfigCallback(nullptr);
  wmStarted_ = true;
  // autoConnect 在非阻塞下立即返回;有保存凭据则尝试连 STA,否则后台开 AP 门户。
  const bool ok = g_wm.autoConnect(kApSsid);
  if (ok) {
    notifyStaConnected();
  } else {
    configPortalWasActive_ = g_wm.getConfigPortalActive();
    state_ =
        configPortalWasActive_ ? NetState::ConfigPortal : NetState::Connecting;
    Serial.println(
        "[NET] No saved creds / connect failed, AP config portal active");
  }
}

void NetworkManager::loop() {
  if (state_ == NetState::Off)
    return;

  // WiFiManager 后台处理配网门户;保存成功后 process() 返回 true。
  bool wmConnected = false;
  if (wmStarted_)
    wmConnected = g_wm.process();
  const bool portalActive = g_wm.getConfigPortalActive();
  if (portalActive) {
    state_ = NetState::ConfigPortal;
    configPortalWasActive_ = true;
  }
  if (wmConnected)
    notifyStaConnected();
  // WiFiManager 非阻塞模式保存成功后可能未自动关闭门户,显式释放其 80 端口。
  if (configPortalWasActive_ && WiFi.status() == WL_CONNECTED) {
    g_wm.stopConfigPortal();
    configPortalWasActive_ = false;
    notifyStaConnected();
  }

  // 处理 wifi 任务置的事件标志。
  if (wifiJustConnected_) {
    wifiJustConnected_ = false;
    if (!mdnsStarted_) {
      mdnsStarted_ = MDNS.begin(hostname());
      Serial.printf("[NET] mDNS: %s\n", mdnsStarted_ ? "ready" : "failed");
    }
    if (!serverStarted_)
      startWebServer();
    if (!ntpStarted_ && settings_.ntpEnabled)
      startNtp();
    if (!otaStarted_ && settings_.otaEnabled)
      startOta();
    Serial.printf("[NET] STA connected, IP=%s\n", ipString().c_str());
  }
  if (wifiJustDisconnected_) {
    wifiJustDisconnected_ = false;
    state_ = NetState::Connecting;
    Serial.println("[NET] STA disconnected, reconnecting...");
  }

  // 服务器仅在 STA 连上后处理请求(规避与 WiFiManager 端口 80 冲突;本服务走
  // 81)。
  if (serverStarted_ && WiFi.status() == WL_CONNECTED)
    g_server.handleClient();
  if (otaStarted_)
    ArduinoOTA.handle();

  // MQTT 轮询与重连。
  if (settings_.mqttEnabled && settings_.mqttBroker[0] &&
      WiFi.status() == WL_CONNECTED) {
    if (!mqttConfigured_)
      startMqtt();
    if (g_mqtt.loop()) {
      // 周期上报。
      if (millis() - lastMqttPublish_ >= MQTT_PUBLISH_MS) {
        lastMqttPublish_ = millis();
        publishState();
      }
    } else if (millis() - lastMqttPublish_ >= MQTT_RECONNECT_MS) {
      lastMqttPublish_ = millis(); // 复用为重连节流
      if (g_mqtt.connect(
              hostname(), nullptr, nullptr,
              (String(settings_.mqttTopicPrefix) + "/online").c_str(), 1, true,
              "0")) {
        const String cmdTopic = String(settings_.mqttTopicPrefix) + "/cmd";
        g_mqtt.subscribe(cmdTopic.c_str());
        publishEvent("online");
        Serial.printf("[MQTT] connected, sub %s\n", cmdTopic.c_str());
      }
    }
  }

  // 状态变化上报 event(首次只初始化基准,不上报)。
  const uint8_t cur = (uint8_t)outputs_.state;
  if (lastChamberState_ == 0xff) {
    lastChamberState_ = cur;
  } else if (cur != lastChamberState_) {
    if (settings_.mqttEnabled && WiFi.status() == WL_CONNECTED &&
        g_mqtt.connected())
      publishEvent(stateName((ChamberState)cur));
    lastChamberState_ = cur;
  }

  // 综合状态。
  if (WiFi.status() == WL_CONNECTED) {
    state_ = (settings_.mqttEnabled && g_mqtt.connected())
                 ? NetState::MqttConnected
                 : NetState::Connected;
  } else if (state_ != NetState::ConfigPortal) {
    state_ = NetState::Connecting;
  }
}

void NetworkManager::updateReadings(const Readings &r, const Outputs &o,
                                    float humidity) {
  readings_ = r;
  outputs_ = o;
  // 手动排风是 UI 状态,控制器输出未包含该覆盖项;统一在这里补齐给硬件/Web/MQTT。
  if (ui_ && ui_->settings().manualExhaust)
    outputs_.exhaustPercent = 100;
  humidity_ = humidity;
}

void NetworkManager::onSettingsChanged(const SystemSettings &settings) {
  // 保存旧值用于比较。
  char oldBroker[64];
  memcpy(oldBroker, settings_.mqttBroker, sizeof(oldBroker));
  const uint16_t oldPort = settings_.mqttPort;
  const bool mqttWas = settings_.mqttEnabled;
  const bool ntpWas = settings_.ntpEnabled;
  const bool otaWas = settings_.otaEnabled;
  const bool wifiWas = settings_.wifiEnabled;

  settings_ = settings;
  // Language is a persisted SystemSettings value, but the UI snapshot reads
  // the controller so all local/remote setting paths converge here.
  if (controller_)
    controller_->setLanguage(settings_.language);

  if (!settings_.wifiEnabled) {
    if (wifiWas) {
      WiFi.disconnect(true);
      state_ = NetState::Off;
    }
    return;
  }
  // 本地关掉 WiFi 后又打开:重新拉起配网流程。否则 state_ 一直停在 Off,
  // loop() 会立即 return,联网再也起不来。
  if (!wifiWas) {
    WiFi.mode(WIFI_STA);
    startWifiManager();
  }
  // MQTT 参数(broker/port/enabled)变化则断开重配,loop() 内自动重连。
  const bool brokerChanged = strcmp(oldBroker, settings_.mqttBroker) != 0 ||
                             oldPort != settings_.mqttPort;
  if (mqttWas != settings_.mqttEnabled || brokerChanged) {
    if (g_mqtt.connected())
      g_mqtt.disconnect();
    mqttConfigured_ = false;
  }
  if (settings_.mqttEnabled && settings_.mqttBroker[0] &&
      WiFi.status() == WL_CONNECTED && !mqttConfigured_)
    startMqtt();
  // NTP/OTA:开关翻转时双向生效(开则起,关则停),仅在已连网时才需要动手。
  if (ntpWas != settings_.ntpEnabled) {
    if (settings_.ntpEnabled) {
      if (WiFi.status() == WL_CONNECTED && !ntpStarted_)
        startNtp();
    } else if (ntpStarted_) {
      esp_sntp_stop();
      ntpStarted_ = false;
      Serial.println("[NTP] stopped");
    }
  }
  if (otaWas != settings_.otaEnabled) {
    if (settings_.otaEnabled) {
      if (WiFi.status() == WL_CONNECTED && !otaStarted_)
        startOta();
    } else if (otaStarted_) {
      ArduinoOTA.end();
      otaStarted_ = false;
      Serial.println("[OTA] stopped");
    }
  }
}

String NetworkManager::ipString() const {
  if (WiFi.status() != WL_CONNECTED)
    return "";
  return WiFi.localIP().toString();
}

String NetworkManager::timeString() const {
  time_t now = time(nullptr);
  if (now < 1700000000)
    return ""; // 未同步
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
           tm.tm_sec);
  return String(buf);
}

// ---------- 子系统启动 ----------
void NetworkManager::startWebServer() {
  g_server.on("/api/state", HTTP_GET, [this]() {
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", buildStateJson());
  });
  g_server.on("/api/settings", HTTP_GET, [this]() {
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", buildSettingsJson());
  });
  // 在线配置联网参数:仅覆盖请求中出现的字段,写入 NVS 后立即重配子系统。
  g_server.on("/api/settings", HTTP_POST, [this]() {
    // 该接口自身走网络;关闭 WiFi 等于自断通道,只能在串口/UI 侧操作。
    if (g_server.hasArg("wifiEnabled") &&
        !parseBoolText(g_server.arg("wifiEnabled"), true)) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"wifiEnabled off not allowed "
                    "remotely\"}");
      return;
    }
    if (g_server.hasArg("mqttEnabled"))
      settings_.mqttEnabled =
          parseBoolText(g_server.arg("mqttEnabled"), settings_.mqttEnabled);
    if (g_server.hasArg("mqttBroker")) {
      const String broker = g_server.arg("mqttBroker");
      strncpy(settings_.mqttBroker, broker.c_str(),
              sizeof(settings_.mqttBroker) - 1);
      settings_.mqttBroker[sizeof(settings_.mqttBroker) - 1] = '\0';
    }
    if (g_server.hasArg("mqttPort"))
      settings_.mqttPort =
          (uint16_t)constrain(g_server.arg("mqttPort").toInt(), 1L, 65535L);
    if (g_server.hasArg("mqttTopicPrefix")) {
      const String prefix = g_server.arg("mqttTopicPrefix");
      strncpy(settings_.mqttTopicPrefix, prefix.c_str(),
              sizeof(settings_.mqttTopicPrefix) - 1);
      settings_.mqttTopicPrefix[sizeof(settings_.mqttTopicPrefix) - 1] = '\0';
    }
    if (g_server.hasArg("ntpEnabled"))
      settings_.ntpEnabled =
          parseBoolText(g_server.arg("ntpEnabled"), settings_.ntpEnabled);
    if (g_server.hasArg("otaEnabled"))
      settings_.otaEnabled =
          parseBoolText(g_server.arg("otaEnabled"), settings_.otaEnabled);
    if (g_server.hasArg("language")) {
      const String language = g_server.arg("language");
      if (language == "zh" || language == "cn")
        settings_.language = Language::Chinese;
      else if (language == "en")
        settings_.language = Language::English;
      else {
        g_server.send(400, "application/json",
                      "{\"ok\":false,\"err\":\"language must be zh or en\"}");
        return;
      }
    }

    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", "{\"ok\":true}");
    settingsStore_.save(settings_); // NVS 持久化(内部含 sanitize)
    onSettingsChanged(settings_);
  });
  g_server.on("/api/action", HTTP_POST, [this]() {
    const String name = g_server.arg("name");
    if (dispatchAction(name)) {
      g_server.send(200, "application/json", "{\"ok\":true}");
    } else {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"bad action\"}");
    }
  });
  g_server.on("/api/profile", HTTP_POST, [this]() {
    long idx = g_server.arg("index").toInt();
    if (idx < 0 || idx >= (long)MATERIAL_COUNT) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"bad index\"}");
      return;
    }
    setProfile((size_t)idx);
    const MaterialProfile &current = controller_->profile();
    const float minC = g_server.hasArg("minC") ? g_server.arg("minC").toFloat()
                                               : current.chamberMinC;
    const float maxC = g_server.hasArg("maxC") ? g_server.arg("maxC").toFloat()
                                               : current.chamberMaxC;
    const int fanMin = g_server.hasArg("fanMin")
                           ? g_server.arg("fanMin").toInt()
                           : current.fanMinPercent;
    const int fanMax = g_server.hasArg("fanMax")
                           ? g_server.arg("fanMax").toInt()
                           : current.fanMaxPercent;
    const int postFan = g_server.hasArg("postFan")
                            ? g_server.arg("postFan").toInt()
                            : current.postExhaustPercent;
    const int postSeconds = g_server.hasArg("postSeconds")
                                ? g_server.arg("postSeconds").toInt()
                                : current.postExhaustSeconds;
    if (fanMin < 0 || fanMin > 100 || fanMax < 0 || fanMax > 100 ||
        postFan < 0 || postFan > 100 || postSeconds < 0 || postSeconds > 1800 ||
        !controller_->updateProfile(minC, maxC, fanMin, fanMax, postFan,
                                    postSeconds)) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"invalid profile values\"}");
      return;
    }
    if (!settingsStore_.saveMaterialProfiles()) {
      g_server.send(500, "application/json",
                    "{\"ok\":false,\"err\":\"NVS save failed\"}");
      return;
    }
    g_server.send(200, "application/json", "{\"ok\":true}");
  });
  g_server.on("/api/wifi/reset", HTTP_POST, [this]() {
    g_server.send(200, "text/plain", "rebooting to config portal");
    delay(200);
    g_wm.resetSettings();
    ESP.restart();
  });
  g_server.onNotFound([this]() {
    g_server.send(404, "application/json", "{\"err\":\"not found\"}");
  });
  g_server.begin();
  serverStarted_ = true;
  Serial.println("[NET] WebServer :81 started");
}

void NetworkManager::startMqtt() {
  g_mqtt.setServer(settings_.mqttBroker, settings_.mqttPort);
  g_mqtt.setBufferSize(512);
  g_mqtt.setCallback(onMqttMessage);
  mqttConfigured_ = true;
  lastMqttPublish_ = 0; // 触发立即重连
  Serial.printf("[MQTT] configured %s:%u\n", settings_.mqttBroker,
                settings_.mqttPort);
}

void NetworkManager::startNtp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  ntpStarted_ = true;
  Serial.println("[NTP] started (UTC+8)");
}

void NetworkManager::startOta() {
  ArduinoOTA.setHostname(hostname());
  ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart([this]() {
    // OTA 写入期间 PID 失步,先关加热保安全。
    controller_->setSystemEnabled(false);
    Serial.println("[OTA] start, heater disabled");
  });
  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] done"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u/%u\r", p, t);
  });
  ArduinoOTA.onError(
      [](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();
  otaStarted_ = true;
  Serial.println("[OTA] ready");
}

// ---------- 远程命令 ----------
bool NetworkManager::dispatchAction(const String &name) {
  if (!ui_ || !controller_)
    return false;
  if (name == "toggleSystem") {
    ui_->apply(UiAction::ToggleSystem, *controller_);
    return true;
  }
  if (name == "togglePreheat") {
    ui_->apply(UiAction::TogglePreheat, *controller_);
    return true;
  }
  if (name == "toggleLight") {
    ui_->apply(UiAction::ToggleLight, *controller_);
    return true;
  }
  if (name == "toggleManualExhaust") {
    ui_->apply(UiAction::ToggleManualExhaust, *controller_);
    return true;
  }
  UiAction action;
  if (!parseAction(name, action))
    return false;
  ui_->apply(action, *controller_);
  return true;
}

void NetworkManager::setProfile(size_t index) {
  if (!ui_ || !controller_ || index >= MATERIAL_COUNT)
    return;
  ui_->setMaterialIndex(index);
  controller_->setProfile(index);
}

// ---------- MQTT 上报 ----------
void NetworkManager::publishState() {
  if (!g_mqtt.connected())
    return;
  const String topic = String(settings_.mqttTopicPrefix) + "/state";
  g_mqtt.publish(topic.c_str(), buildStateJson().c_str(), true);
}

void NetworkManager::publishEvent(const char *type) {
  if (!g_mqtt.connected())
    return;
  const String topic = String(settings_.mqttTopicPrefix) + "/event";
  String payload = String("{\"event\":\"") + type + "\"}";
  g_mqtt.publish(topic.c_str(), payload.c_str(), false);
}

void NetworkManager::handleMqttCommand(const char *payload) {
  const String action = jsonExtractString(payload, "action");
  if (action.length() && dispatchAction(action)) {
    publishEvent(action.c_str());
    Serial.printf("[MQTT] cmd action=%s\n", action.c_str());
    return;
  }
  // profile 按索引切料。
  if (String(payload).indexOf("\"profile\"") >= 0) {
    long idx = jsonExtractInt(payload, "profile", -1);
    if (idx >= 0 && idx < (long)MATERIAL_COUNT) {
      setProfile((size_t)idx);
      Serial.printf("[MQTT] cmd profile=%ld\n", idx);
    }
  }
}

// ---------- JSON ----------
const char *NetworkManager::stateName(ChamberState s) const {
  switch (s) {
  case ChamberState::Idle:
    return "Idle";
  case ChamberState::Detecting:
    return "Detecting";
  case ChamberState::Preheat:
    return "Preheat";
  case ChamberState::Printing:
    return "Printing";
  case ChamberState::Cooling:
    return "Cooling";
  case ChamberState::Fault:
    return "Fault";
  }
  return "Unknown";
}

static void appendFloat(char *buf, size_t &off, size_t cap, const char *key,
                        float v) {
  if (isnan(v)) {
    off += snprintf(buf + off, cap - off, "\"%s\":null", key);
  } else {
    off += snprintf(buf + off, cap - off, "\"%s\":%.2f", key, v);
  }
}

String NetworkManager::buildStateJson() const {
  static char buf[640];
  size_t off = 0;
  const Readings &r = readings_;
  const Outputs &o = outputs_;
  const MaterialProfile &m =
      controller_ ? controller_->profile() : MATERIALS[0];

  off += snprintf(buf + off, sizeof(buf) - off,
                  "{\"material\":\"%s\",\"materialIndex\":%u,\"state\":\"%s\"",
                  m.name, ui_ ? (unsigned)ui_->materialIndex() : 0u,
                  stateName(o.state));
  buf[off++] = ',';
  appendFloat(buf, off, sizeof(buf), "chamberC", r.chamberC);
  buf[off++] = ',';
  appendFloat(buf, off, sizeof(buf), "heaterBoardC", r.heaterBoardC);
  buf[off++] = ',';
  appendFloat(buf, off, sizeof(buf), "humidity", humidity_);
  buf[off++] = ',';
  appendFloat(buf, off, sizeof(buf), "currentA", r.heaterCurrentA);
  buf[off++] = ',';
  appendFloat(buf, off, sizeof(buf), "voltageV", r.supplyVoltageV);
  off += snprintf(
      buf + off, sizeof(buf) - off,
      ",\"exhaustPercent\":%u,\"heatPercent\":%u,\"heaterFan\":%u,\"light\":%s,"
      "\"systemEnabled\":%s,\"pirMotion\":%s,\"time\":\"%s\",\"ip\":\"%s\","
      "\"net\":\"%s\"}",
      (unsigned)(ui_ && ui_->settings().manualExhaust ? 100 : o.exhaustPercent),
      (unsigned)o.heaterPercent,
      (unsigned)(o.heaterFan && settings_.heaterFanPercent
                     ? settings_.heaterFanPercent
                     : 0),
      o.light ? "true" : "false",
      (controller_ && controller_->systemEnabled()) ? "true" : "false",
      r.pirMotion ? "true" : "false", timeString().c_str(), ipString().c_str(),
      connected() ? "online" : "offline");
  return String(buf);
}

String NetworkManager::buildSettingsJson() const {
  static char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"wifiEnabled\":%s,\"mqttEnabled\":%s,\"mqttBroker\":\"%s\","
           "\"mqttPort\":%u,"
           "\"mqttTopicPrefix\":\"%s\",\"ntpEnabled\":%s,\"otaEnabled\":%s,"
           "\"heaterMaxCurrentA\":%u,\"heaterFanPercent\":%u,"
           "\"heaterBoardLimitC\":%u,"
           "\"brightness\":%u,\"language\":\"%s\"}",
           settings_.wifiEnabled ? "true" : "false",
           settings_.mqttEnabled ? "true" : "false", settings_.mqttBroker,
           (unsigned)settings_.mqttPort, settings_.mqttTopicPrefix,
           settings_.ntpEnabled ? "true" : "false",
           settings_.otaEnabled ? "true" : "false",
           (unsigned)settings_.heaterMaxCurrentA,
           (unsigned)settings_.heaterFanPercent,
           (unsigned)settings_.heaterBoardLimitC,
           (unsigned)settings_.brightness,
           settings_.language == Language::English ? "en" : "zh");
  return String(buf);
}
