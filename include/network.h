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

// 联网子系统的唯一单例类型,main.cpp 持有全局对象 network,所有网络能力
// (WiFi 配网 / REST / MQTT / NTP / OTA / mDNS)都由它统一编排。
class NetworkManager {
public:
  // 开机时由 setup() 调用一次:登记 WiFi 事件回调、设置主机名,并按
  // wifiEnabled 决定是否初始化 WiFi、拉起配网门户。faultRecord 为 NVS
  // 故障记忆(main.cpp 持有,锁/解锁时更新),/api/state 的 lastFault
  // 等字段据此回传 —— 按引用存指针,读到的永远是最新档案。
  void begin(ChamberController &controller, UiModel &ui,
             SystemSettings &settings, FaultRecord &faultRecord);
  // 每个主循环周期调用:轮询 WiFiManager 门户、WebServer、MQTT、NTP、OTA,
  // 内部只做非阻塞处理,绝不卡住 50ms 热控节拍。
  void loop();
  // 主循环每周期喂数据;Web/MQTT/状态查询据此返回。Readings 已含
  // voltage/current,无需再传。
  void updateReadings(const Readings &r, const Outputs &o, float humidity);
  // 设置页改动 wifi/mqtt/ntp/ota 后调用,触发重连/重配。
  void onSettingsChanged(const SystemSettings &settings);

  // 当前网络状态机的状态(见上方 NetState)。
  NetState state() const { return state_; }
  // STA 是否已拿到 IP(不区分 MQTT 是否连上);UI 联网图标据此显示。
  bool connected() const {
    return state_ == NetState::Connected || state_ == NetState::MqttConnected;
  }
  // 当前 STA 的接收信号强度(dBm),供屏上画 4 格信号条。
  // 未联网(含 wifiEnabled=false 的离线运行)返回哨兵 0 —— 0 dBm 实际不会出现,
  // 调用方 pure::rssiBars() 会把它算成 0 格。定义在 .cpp:WiFi.RSSI() 需要 WiFi.h。
  int8_t rssiDbm() const;
  // 当前连接的 WiFi SSID,未联网返回空串。定义在 .cpp(WiFi.SSID() 需要
  // WiFi.h)。注意语义与"从未配网"判定相反:那里读 WiFiManager 的持久化
  // 凭据,这里要的是"此刻连着谁"。
  String ssidString() const;

  // 当前网络告警码(见 ui_model.h 的 NetAlert)。纯提示,不影响热控与状态机。
  NetAlert alert() const {
    return wifiAlert_ != NetAlert::None ? wifiAlert_ : mqttAlert_;
  }
  // 告警码的英文短名("WifiAuthFail"/"MqttUnreachable"…,无告警为 "none"),
  // 与固件枚举同名,便于把 REST 数据直接对回代码。串口日志与 /api/state 共用。
  const char *alertName() const;
  String ipString() const;   // STA IP "x.x.x.x",未连返回 ""
  String timeString() const; // NTP "HH:MM:SS",未同步返回 ""
  // OTA 是否进行中(供主循环在屏幕叠加升级进度横幅)。
  bool otaActive() const;
  // 最近一次 OTA 进度 0..100(未升级时为 0)。
  uint8_t otaProgress() const;
  // 固定 mDNS/OTA 主机名,局域网访问 http://chamber.local/ 。
  const char *hostname() const { return "chamber"; }
  // 逐机唯一的芯片 ID:由 eFuse 48 位 MAC 全宽映射(双射、不丢位),格式
  // CH-XXXXXXXXXXXX(12 位十六进制),任意两颗芯片必然不同。屏幕「系统设置 →
  // 关于本机」与 Web 设置页都展示它;同一取值也用作 ArduinoOTA 接入密码。
  String chipId() const;

  // 由静态回调(WiFi 事件 / MQTT)转发,公开以便 free function 经 g_self 调用。
  void notifyStaConnected() { wifiJustConnected_ = true; }
  void notifyStaDisconnected(uint8_t reason) {
    lastDisconnectReason_ = reason;
    wifiJustDisconnected_ = true;
  }
  // 处理一条已复制到本地缓冲的 MQTT 下行命令(JSON 动作/切料/设置)。
  void handleMqttCommand(const char *payload);

private:
  ChamberController *controller_ = nullptr; // 动作命令最终下发给热控器
  // NVS 故障记忆档案(main.cpp 的 faultRecord),REST 查询只读。
  FaultRecord *faultRecord_ = nullptr;
  UiModel *ui_ = nullptr;                  // 用于推送 Web/MQTT 触发的 UI 动作
  SystemSettings settings_{};              // 当前生效的联网等设置副本
  SystemSettings *sharedSettings_ = nullptr; // 指向 main.cpp 的全局设置(同一对象)
  NetState state_ = NetState::Off;
  bool wmStarted_ = false;      // WiFiManager.autoConnect 已调用过
  bool serverStarted_ = false;  // WebServer 已 begin
  bool otaStarted_ = false;     // ArduinoOTA.begin 已调用
  bool mqttConfigured_ = false; // PubSubClient 已 setServer/订阅
  bool ntpStarted_ = false;     // configTime 已调用
  bool mdnsStarted_ = false;    // mDNS 已在 STA 下启动
  SettingsStore settingsStore_; // 当前未直接使用,预留设置持久化入口
  uint32_t lastMqttPublish_ = 0; // 上次周期上报时间戳(ms),按 MQTT_PUBLISH_MS 节流
  uint8_t lastChamberState_ = 0xff; // 检测状态变化上报 event
  Readings readings_{};            // 最近一次传感器读数(供 REST/MQTT 返回)
  Outputs outputs_{};              // 最近一次执行器输出
  float humidity_ = NAN;           // 湿度单独保存(AHT20,不在 Readings 内)
  // WiFi 事件回调置位,loop() 内处理,避免 wifi 任务与主循环竞争。
  volatile bool wifiJustConnected_ = false;
  volatile bool wifiJustDisconnected_ = false;
  volatile uint8_t lastDisconnectReason_ = 0; // 最近一次断网原因码(esp_wifi reason)
  uint32_t disconnectCount_ = 0;              // 累计断线次数,并入 /api/state
  bool configPortalWasActive_ = false;        // 门户曾被拉起,用于超时后重试
  uint32_t portalRetryAtMs_ = 0;              // 门户下次重新开放的时刻(ms)

  // ---- 网络告警(只提示,不停机)----
  // 认证类失败与"未找到 AP"分开计数:两者处理办法不同(改密码 vs 改 SSID/
  // 挪位置)。连续失败 NET_ALERT_STREAK 次才置位,避免路由器重启、信号抖动
  // 时的单次重连就弹提示。WiFi 告警优先于 MQTT —— 前者是根因,后者多半是
  // 连带结果。
  NetAlert wifiAlert_ = NetAlert::None; // W-01/W-02
  NetAlert mqttAlert_ = NetAlert::None; // W-03/W-04
  uint8_t wifiAuthFailStreak_ = 0;      // WiFi 连续认证/关联失败次数
  uint8_t wifiNoApStreak_ = 0;          // WiFi 连续"未找到 AP"次数
  uint8_t mqttFailStreak_ = 0;          // MQTT 连续连接失败次数(不分原因)
  // 置位/清除告警,变化时打一行串口日志(COM4 走 USB-CDC 的注意事项见文档)。
  void setWifiAlert(NetAlert alert);
  void setMqttAlert(NetAlert alert);

  // REST 简单限流:按客户端 IP 记录最近读/写请求时刻,超出间隔返回 429。
  struct RateLimitSlot {
    uint32_t ip = 0;          // 客户端 IPv4(0 表示该槽空闲)
    uint32_t lastReadMs = 0;  // 最近一次读请求时刻(ms)
    uint32_t lastWriteMs = 0; // 最近一次写请求时刻(ms)
  };
  RateLimitSlot rateSlots_[4]{};

  void startWifiManager(); // 配置并启动 WiFiManager(非阻塞 AP 配网门户)
  void startWebServer();   // 注册全部 REST 路由并在 80 端口启动 WebServer
  void startMqtt();        // 配置 PubSubClient 的 broker/回调,实际连接在 loop()
  void startNtp();         // 按 timezone/ntpServer 启动 SNTP 同步
  void startOta();         // 配置并启动 ArduinoOTA(加热期间拒绝升级)
  void applyIpConfig();    // 启用静态 IP 时在 WiFi.begin 前下发 IPv4 配置
  bool allowApiRequest(bool write); // 限流判定:write=true 用写间隔,否则读间隔
  void sendRateLimited();           // 统一回复 429
  // OTA 密码复用唯一芯片 ID(见公开接口 chipId),Web 设置接口明文返回。
  String otaPassword() const { return chipId(); }
  bool dispatchAction(const String &name); // 执行 Web/MQTT 发来的具名动作
  void setProfile(size_t index);           // 切料并同步 UI/控制器
  void publishState();                     // 周期发布完整状态 JSON
  void publishEvent(const char *type);     // 发布单次事件(状态变化等)
  String buildStateJson() const;           // 组装 /api/state 的 JSON
  String buildSettingsJson() const;        // 组装 /api/settings 返回的 JSON
  const char *stateName(ChamberState s) const; // 仓状态枚举转英文字符串(MQTT)
};

// 全局唯一实例,main.cpp 与各静态回调都通过它访问联网子系统。
extern NetworkManager network;
