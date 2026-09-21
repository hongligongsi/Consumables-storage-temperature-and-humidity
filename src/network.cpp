#include "network.h"
#include "version.h"
#include "web_page.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sntp.h>
#include <time.h>
#include <ctype.h>

// 静态分发:WiFi 事件回调与 MQTT 回调跑在 wifi 任务,只经 self_ 转发到实例方法,
// 实例方法内只更新 volatile 标志或排入 loop() 处理,不直接动共享状态。
static NetworkManager *g_self = nullptr;
static WiFiManager g_wm;
static WebServer g_server(80);
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
static bool parseBoolStrict(const String &value, bool &out) {
  if (value == "1" || value == "true" || value == "on") {
    out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off") {
    out = false;
    return true;
  }
  return false;
}

static String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = input[i];
    switch (c) {
    case '"': out += F("\\\""); break;
    case '\\': out += F("\\\\"); break;
    case '\b': out += F("\\b"); break;
    case '\f': out += F("\\f"); break;
    case '\n': out += F("\\n"); break;
    case '\r': out += F("\\r"); break;
    case '\t': out += F("\\t"); break;
    default:
      if (c >= 0x20)
        out += static_cast<char>(c);
      break;
    }
  }
  return out;
}

static bool safeText(const String &value, size_t maxLength,
                     bool allowPosixTz = false) {
  if (!value.length() || value.length() > maxLength)
    return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
        c == '_' || c == '/')
      continue;
    if (allowPosixTz && (c == '+' || c == ':' || c == ',' || c == '<' ||
                         c == '>'))
      continue;
    return false;
  }
  return true;
}

static bool parseIp(const char *text, IPAddress &out, bool allowZero = false) {
  if (!text || !out.fromString(text))
    return false;
  return allowZero || static_cast<uint32_t>(out) != 0;
}

template <size_t N>
static bool copyChecked(char (&dest)[N], const String &value) {
  if (value.length() >= N)
    return false;
  strlcpy(dest, value.c_str(), N);
  return true;
}

// ---------- WiFi 事件回调(只置标志) ----------
static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (!g_self)
    return;
  switch (event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    g_self->notifyStaConnected();
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    g_self->notifyStaDisconnected(info.wifi_sta_disconnected.reason);
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
                           SystemSettings &settings) {
  controller_ = &controller;
  ui_ = &ui;
  settings_ = settings;
  sharedSettings_ = &settings;
  g_self = this;
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname());
  WiFi.setAutoReconnect(true);
  if (!settings_.wifiEnabled) {
    state_ = NetState::Off;
    return;
  }

  applyIpConfig();
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
  portalRetryAtMs_ = 0;
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
  // 门户超时而仍未联网时，稍后重新开放；避免设备永久停在 Connecting。
  if (configPortalWasActive_ && !portalActive &&
      WiFi.status() != WL_CONNECTED) {
    configPortalWasActive_ = false;
    portalRetryAtMs_ = millis() + 5000;
  }
  if (portalRetryAtMs_ && (int32_t)(millis() - portalRetryAtMs_) >= 0 &&
      WiFi.status() != WL_CONNECTED && !portalActive)
    startWifiManager();
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
      if (mdnsStarted_)
        MDNS.addService("http", "tcp", 80);
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
    ++disconnectCount_;
    state_ = NetState::Connecting;
    Serial.printf("[NET] STA disconnected (%u: %s), reconnecting...\n",
                  (unsigned)lastDisconnectReason_,
                  WiFi.disconnectReasonName(
                      static_cast<wifi_err_reason_t>(lastDisconnectReason_)));
  }

  // 服务器仅在 STA 连上、配置门户已关闭后处理请求，因此可安全使用标准 80 端口。
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
    } else if (millis() - lastMqttPublish_ >= MQTT_RECONNECT_MS &&
               outputs_.state != ChamberState::Preheat &&
               outputs_.state != ChamberState::Printing &&
               outputs_.heaterPercent == 0) {
      lastMqttPublish_ = millis(); // 复用为重连节流
      if (g_mqtt.connect(
              hostname(), nullptr, nullptr,
              (String(settings_.mqttTopicPrefix) + "/online").c_str(), 1, true,
              "0")) {
        const String cmdTopic = String(settings_.mqttTopicPrefix) + "/cmd";
        g_mqtt.subscribe(cmdTopic.c_str());
        const String onlineTopic =
            String(settings_.mqttTopicPrefix) + "/online";
        g_mqtt.publish(onlineTopic.c_str(), "1", true);
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
  const SystemSettings old = settings_;
  settings_ = settings;
  // Language is a persisted SystemSettings value, but the UI snapshot reads
  // the controller so all local/remote setting paths converge here.
  if (controller_)
    controller_->setLanguage(settings_.language);

  if (!settings_.wifiEnabled) {
    if (old.wifiEnabled) {
      WiFi.disconnect(true);
      state_ = NetState::Off;
    }
    return;
  }
  // 本地关掉 WiFi 后又打开:重新拉起配网流程。否则 state_ 一直停在 Off,
  // loop() 会立即 return,联网再也起不来。
  if (!old.wifiEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname());
    applyIpConfig();
    startWifiManager();
  }
  const bool ipChanged =
      old.staticIpEnabled != settings_.staticIpEnabled ||
      strcmp(old.staticIp, settings_.staticIp) != 0 ||
      strcmp(old.staticGateway, settings_.staticGateway) != 0 ||
      strcmp(old.staticSubnet, settings_.staticSubnet) != 0 ||
      strcmp(old.staticDns1, settings_.staticDns1) != 0 ||
      strcmp(old.staticDns2, settings_.staticDns2) != 0;
  if (old.wifiEnabled && ipChanged) {
    WiFi.disconnect(false);
    applyIpConfig();
    startWifiManager();
  }
  // MQTT 参数(broker/port/enabled)变化则断开重配,loop() 内自动重连。
  const bool brokerChanged =
      strcmp(old.mqttBroker, settings_.mqttBroker) != 0 ||
      old.mqttPort != settings_.mqttPort ||
      strcmp(old.mqttTopicPrefix, settings_.mqttTopicPrefix) != 0;
  if (old.mqttEnabled != settings_.mqttEnabled || brokerChanged) {
    if (g_mqtt.connected())
      g_mqtt.disconnect();
    mqttConfigured_ = false;
  }
  if (settings_.mqttEnabled && settings_.mqttBroker[0] &&
      WiFi.status() == WL_CONNECTED && !mqttConfigured_)
    startMqtt();
  // NTP/OTA:开关翻转时双向生效(开则起,关则停),仅在已连网时才需要动手。
  const bool ntpChanged =
      old.ntpEnabled != settings_.ntpEnabled ||
      strcmp(old.timezone, settings_.timezone) != 0 ||
      strcmp(old.ntpServer1, settings_.ntpServer1) != 0 ||
      strcmp(old.ntpServer2, settings_.ntpServer2) != 0;
  if (ntpChanged) {
    if (ntpStarted_) {
      esp_sntp_stop();
      ntpStarted_ = false;
    }
    if (settings_.ntpEnabled) {
      if (WiFi.status() == WL_CONNECTED && !ntpStarted_)
        startNtp();
    } else {
      Serial.println("[NTP] stopped");
    }
  }
  if (old.otaEnabled != settings_.otaEnabled) {
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

void NetworkManager::applyIpConfig() {
  if (!settings_.staticIpEnabled) {
    const IPAddress zero(0, 0, 0, 0);
    if (!WiFi.config(zero, zero, zero, zero, zero))
      Serial.println("[NET] DHCP configuration failed");
    return;
  }
  IPAddress ip, gateway, subnet, dns1, dns2;
  if (!parseIp(settings_.staticIp, ip) ||
      !parseIp(settings_.staticGateway, gateway) ||
      !parseIp(settings_.staticSubnet, subnet) ||
      !parseIp(settings_.staticDns1, dns1) ||
      !parseIp(settings_.staticDns2, dns2)) {
    Serial.println("[NET] Invalid static IPv4 configuration; using DHCP");
    const IPAddress zero(0, 0, 0, 0);
    WiFi.config(zero, zero, zero, zero, zero);
    return;
  }
  Serial.printf("[NET] Static IPv4 %s gateway %s\n", settings_.staticIp,
                settings_.staticGateway);
  if (!WiFi.config(ip, gateway, subnet, dns1, dns2))
    Serial.println("[NET] Static IPv4 configuration failed");
}

bool NetworkManager::allowApiRequest(bool write) {
  const uint32_t now = millis();
  const uint32_t ip = static_cast<uint32_t>(g_server.client().remoteIP());
  RateLimitSlot *slot = nullptr;
  for (auto &candidate : rateSlots_) {
    if (candidate.ip == ip) {
      slot = &candidate;
      break;
    }
    if (!slot && candidate.ip == 0)
      slot = &candidate;
  }
  if (!slot)
    slot = &rateSlots_[ip % 4];
  if (slot->ip != ip)
    *slot = RateLimitSlot{};
  slot->ip = ip;
  uint32_t &last = write ? slot->lastWriteMs : slot->lastReadMs;
  const uint32_t minimumMs = write ? 500 : 100;
  if (last && now - last < minimumMs)
    return false;
  last = now;
  return true;
}

void NetworkManager::sendRateLimited() {
  g_server.sendHeader("Retry-After", "1");
  g_server.send(429, "application/json",
                "{\"ok\":false,\"err\":\"rate limit\"}");
}

String NetworkManager::otaPassword() const {
  const uint64_t mac = ESP.getEfuseMac();
  const uint32_t mixed = static_cast<uint32_t>(mac) ^
                         static_cast<uint32_t>(mac >> 32) ^ 0x9E3779B9UL;
  char password[20];
  snprintf(password, sizeof(password), "CH-%08lX", (unsigned long)mixed);
  return String(password);
}

// ---------- 子系统启动 ----------
void NetworkManager::startWebServer() {
  g_server.on("/", HTTP_GET, []() {
    g_server.send_P(200, "text/html; charset=utf-8", WEB_MANAGEMENT_PAGE);
  });
  g_server.on("/api/state", HTTP_GET, [this]() {
    if (!allowApiRequest(false)) {
      sendRateLimited();
      return;
    }
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", buildStateJson());
  });
  g_server.on("/api/settings", HTTP_GET, [this]() {
    if (!allowApiRequest(false)) {
      sendRateLimited();
      return;
    }
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", buildSettingsJson());
  });
  // 在线配置联网参数:仅覆盖请求中出现的字段,写入 NVS 后立即重配子系统。
  g_server.on("/api/settings", HTTP_POST, [this]() {
    if (!allowApiRequest(true)) {
      sendRateLimited();
      return;
    }
    SystemSettings candidate = settings_;
    String error;
    auto readBool = [&](const char *name, bool &target) {
      if (!g_server.hasArg(name) || error.length())
        return;
      if (!parseBoolStrict(g_server.arg(name), target))
        error = String(name) + " must be true or false";
    };
    // 该接口自身走网络;关闭 WiFi 等于自断通道,只能在串口/UI 侧操作。
    if (g_server.hasArg("wifiEnabled")) {
      bool enabled = true;
      if (!parseBoolStrict(g_server.arg("wifiEnabled"), enabled) || !enabled)
        error = "wifiEnabled off not allowed remotely";
    }
    readBool("mqttEnabled", candidate.mqttEnabled);
    readBool("ntpEnabled", candidate.ntpEnabled);
    readBool("otaEnabled", candidate.otaEnabled);
    readBool("staticIpEnabled", candidate.staticIpEnabled);

    if (g_server.hasArg("mqttBroker")) {
      const String broker = g_server.arg("mqttBroker");
      if (broker.length() && !safeText(broker, 63))
        error = "invalid mqttBroker";
      else if (!copyChecked(candidate.mqttBroker, broker))
        error = "mqttBroker too long";
    }
    if (g_server.hasArg("mqttPort")) {
      const String value = g_server.arg("mqttPort");
      char *end = nullptr;
      const long port = strtol(value.c_str(), &end, 10);
      if (!value.length() || !end || *end || port < 1 || port > 65535)
        error = "mqttPort must be 1..65535";
      else
        candidate.mqttPort = static_cast<uint16_t>(port);
    }
    if (g_server.hasArg("mqttTopicPrefix")) {
      const String prefix = g_server.arg("mqttTopicPrefix");
      if (!safeText(prefix, 23) || !copyChecked(candidate.mqttTopicPrefix, prefix))
        error = "invalid mqttTopicPrefix";
    }

    auto readText = [&](const char *name, char *dest, size_t cap,
                        bool posixTz) {
      if (!g_server.hasArg(name) || error.length())
        return;
      const String value = g_server.arg(name);
      if (!safeText(value, cap - 1, posixTz)) {
        error = String("invalid ") + name;
        return;
      }
      strlcpy(dest, value.c_str(), cap);
    };
    readText("timezone", candidate.timezone, sizeof(candidate.timezone), true);
    readText("ntpServer1", candidate.ntpServer1,
             sizeof(candidate.ntpServer1), false);
    readText("ntpServer2", candidate.ntpServer2,
             sizeof(candidate.ntpServer2), false);
    readText("staticIp", candidate.staticIp, sizeof(candidate.staticIp), false);
    readText("staticGateway", candidate.staticGateway,
             sizeof(candidate.staticGateway), false);
    readText("staticSubnet", candidate.staticSubnet,
             sizeof(candidate.staticSubnet), false);
    readText("staticDns1", candidate.staticDns1,
             sizeof(candidate.staticDns1), false);
    readText("staticDns2", candidate.staticDns2,
             sizeof(candidate.staticDns2), false);

    if (g_server.hasArg("language")) {
      const String language = g_server.arg("language");
      if (language == "zh" || language == "cn")
        candidate.language = Language::Chinese;
      else if (language == "en")
        candidate.language = Language::English;
      else
        error = "language must be zh or en";
    }

    if (!error.length() && candidate.mqttEnabled && !candidate.mqttBroker[0])
      error = "mqttBroker required when MQTT is enabled";
    if (!error.length() && candidate.staticIpEnabled) {
      IPAddress ip;
      if (!parseIp(candidate.staticIp, ip) ||
          !parseIp(candidate.staticGateway, ip) ||
          !parseIp(candidate.staticSubnet, ip) ||
          !parseIp(candidate.staticDns1, ip) ||
          !parseIp(candidate.staticDns2, ip))
        error = "invalid static IPv4 settings";
    }
    if (error.length()) {
      g_server.send(400, "application/json",
                    String("{\"ok\":false,\"err\":\"") +
                        jsonEscape(error) + "\"}");
      return;
    }
    if (!settingsStore_.save(candidate)) {
      g_server.send(500, "application/json",
                    "{\"ok\":false,\"err\":\"NVS save failed\"}");
      return;
    }
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(200, "application/json", "{\"ok\":true}");
    if (sharedSettings_)
      *sharedSettings_ = candidate;
    onSettingsChanged(candidate);
  });
  g_server.on("/api/action", HTTP_POST, [this]() {
    if (!allowApiRequest(true)) {
      sendRateLimited();
      return;
    }
    const String name = g_server.arg("name");
    if (dispatchAction(name)) {
      g_server.send(200, "application/json", "{\"ok\":true}");
    } else {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"bad action\"}");
    }
  });
  g_server.on("/api/profile", HTTP_POST, [this]() {
    if (!allowApiRequest(true)) {
      sendRateLimited();
      return;
    }
    if (!g_server.hasArg("index")) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"index required\"}");
      return;
    }
    long idx = g_server.arg("index").toInt();
    if (idx < 0 || idx >= (long)MATERIAL_COUNT) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"bad index\"}");
      return;
    }
    const size_t previousIndex = ui_->materialIndex();
    const MaterialProfile previous = MATERIALS[idx];
    const MaterialProfile &current = MATERIALS[idx];
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
    if (!isfinite(minC) || !isfinite(maxC) || minC < 0 || minC > 90 ||
        maxC < minC + 5 || maxC > 100 || fanMin < 0 || fanMin > 100 ||
        fanMin > fanMax || fanMax < 0 || fanMax > 100 ||
        postFan < 0 || postFan > 100 || postSeconds < 0 || postSeconds > 1800 ||
        !controller_ || !ui_) {
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"invalid profile values\"}");
      return;
    }
    setProfile((size_t)idx);
    if (!controller_->updateProfile(minC, maxC, fanMin, fanMax, postFan,
                                    postSeconds)) {
      setProfile(previousIndex);
      g_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"invalid profile values\"}");
      return;
    }
    if (!settingsStore_.saveMaterialProfiles()) {
      MATERIALS[idx] = previous;
      setProfile(previousIndex);
      g_server.send(500, "application/json",
                    "{\"ok\":false,\"err\":\"NVS save failed\"}");
      return;
    }
    g_server.send(200, "application/json", "{\"ok\":true}");
  });
  g_server.on("/api/wifi/reset", HTTP_POST, [this]() {
    if (!allowApiRequest(true)) {
      sendRateLimited();
      return;
    }
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
  Serial.println("[NET] WebServer :80 started (http://chamber.local/)");
}

void NetworkManager::startMqtt() {
  g_mqtt.setServer(settings_.mqttBroker, settings_.mqttPort);
  g_mqtt.setBufferSize(512);
  // 把不可达 broker 的单次等待压到 1 秒；避免默认 15 秒长期卡住主循环。
  g_mqtt.setSocketTimeout(1);
  g_mqtt.setCallback(onMqttMessage);
  mqttConfigured_ = true;
  lastMqttPublish_ = 0; // 触发立即重连
  Serial.printf("[MQTT] configured %s:%u\n", settings_.mqttBroker,
                settings_.mqttPort);
}

void NetworkManager::startNtp() {
  configTzTime(settings_.timezone, settings_.ntpServer1,
               settings_.ntpServer2);
  ntpStarted_ = true;
  Serial.printf("[NTP] started (TZ=%s, %s, %s)\n", settings_.timezone,
                settings_.ntpServer1, settings_.ntpServer2);
}

void NetworkManager::startOta() {
  ArduinoOTA.setHostname(hostname());
  const String password = otaPassword();
  ArduinoOTA.setPassword(password.c_str());
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
  Serial.printf("[OTA] ready, device password=%s\n", password.c_str());
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

String NetworkManager::buildStateJson() const {
  const Readings &r = readings_;
  const Outputs &o = outputs_;
  const MaterialProfile &m =
      controller_ ? controller_->profile() : MATERIALS[0];
  auto numberOrNull = [](float value) {
    return isnan(value) ? String("null") : String(value, 2);
  };
  const bool online = WiFi.status() == WL_CONNECTED;
  const String disconnectReason =
      lastDisconnectReason_
          ? String(WiFi.disconnectReasonName(
                static_cast<wifi_err_reason_t>(lastDisconnectReason_)))
          : String();
  String json;
  json.reserve(900);
  json += F("{\"material\":\"");
  json += jsonEscape(m.name);
  json += F("\",\"materialIndex\":");
  json += ui_ ? String((unsigned)ui_->materialIndex()) : String("0");
  json += F(",\"state\":\"");
  json += stateName(o.state);
  json += F("\",\"chamberC\":"); json += numberOrNull(r.chamberC);
  json += F(",\"heaterBoardC\":"); json += numberOrNull(r.heaterBoardC);
  json += F(",\"humidity\":"); json += numberOrNull(humidity_);
  json += F(",\"currentA\":"); json += numberOrNull(r.heaterCurrentA);
  json += F(",\"voltageV\":"); json += numberOrNull(r.supplyVoltageV);
  json += F(",\"exhaustPercent\":");
  json += String((unsigned)(ui_ && ui_->settings().manualExhaust
                                ? 100
                                : o.exhaustPercent));
  json += F(",\"heatPercent\":"); json += String((unsigned)o.heaterPercent);
  json += F(",\"heaterFan\":");
  json += String((unsigned)(o.heaterFan ? settings_.heaterFanPercent : 0));
  json += F(",\"light\":"); json += o.light ? F("true") : F("false");
  json += F(",\"systemEnabled\":");
  json += controller_ && controller_->systemEnabled() ? F("true") : F("false");
  json += F(",\"pirMotion\":"); json += r.pirMotion ? F("true") : F("false");
  json += F(",\"time\":\""); json += jsonEscape(timeString());
  json += F("\",\"ip\":\""); json += jsonEscape(ipString());
  json += F("\",\"net\":\""); json += connected() ? F("online") : F("offline");
  json += F("\",\"ssid\":\""); json += online ? jsonEscape(WiFi.SSID()) : String();
  json += F("\",\"rssi\":"); json += online ? String(WiFi.RSSI()) : String("null");
  json += F(",\"uptimeSeconds\":"); json += String(millis() / 1000UL);
  json += F(",\"firmware\":\""); json += jsonEscape(FW_VERSION);
  json += F("\",\"disconnectCount\":"); json += String(disconnectCount_);
  json += F(",\"lastDisconnectReason\":\""); json += jsonEscape(disconnectReason);
  json += F("\",\"lastDisconnectReasonCode\":");
  json += String((unsigned)lastDisconnectReason_);
  json += '}';
  return json;
}

String NetworkManager::buildSettingsJson() const {
  String json;
  json.reserve(900);
  auto addString = [&](const char *key, const char *value) {
    json += F(",\""); json += key; json += F("\":\"");
    json += jsonEscape(value); json += '"';
  };
  json = settings_.wifiEnabled ? F("{\"wifiEnabled\":true")
                               : F("{\"wifiEnabled\":false");
  json += settings_.mqttEnabled ? F(",\"mqttEnabled\":true")
                                : F(",\"mqttEnabled\":false");
  addString("mqttBroker", settings_.mqttBroker);
  json += F(",\"mqttPort\":"); json += String(settings_.mqttPort);
  addString("mqttTopicPrefix", settings_.mqttTopicPrefix);
  json += settings_.ntpEnabled ? F(",\"ntpEnabled\":true")
                               : F(",\"ntpEnabled\":false");
  json += settings_.otaEnabled ? F(",\"otaEnabled\":true")
                               : F(",\"otaEnabled\":false");
  addString("timezone", settings_.timezone);
  addString("ntpServer1", settings_.ntpServer1);
  addString("ntpServer2", settings_.ntpServer2);
  json += settings_.staticIpEnabled ? F(",\"staticIpEnabled\":true")
                                    : F(",\"staticIpEnabled\":false");
  addString("staticIp", settings_.staticIp);
  addString("staticGateway", settings_.staticGateway);
  addString("staticSubnet", settings_.staticSubnet);
  addString("staticDns1", settings_.staticDns1);
  addString("staticDns2", settings_.staticDns2);
  addString("otaPassword", otaPassword().c_str());
  json += F(",\"heaterMaxCurrentA\":"); json += String(settings_.heaterMaxCurrentA);
  json += F(",\"heaterFanPercent\":"); json += String(settings_.heaterFanPercent);
  json += F(",\"heaterBoardLimitC\":"); json += String(settings_.heaterBoardLimitC);
  json += F(",\"brightness\":"); json += String(settings_.brightness);
  addString("language", settings_.language == Language::English ? "en" : "zh");
  json += '}';
  return json;
}
