# 联网功能实现方案(WiFi + Web API + MQTT + NTP + OTA,AP 配网门户)

## Context(背景与目标)
这是 ESP32-S3 N16R8 的 3D 打印耗材干燥/恒温仓控制器,当前完全离线:传感器读数、PID 热控、TFT 显示、串口单字符命令。用户希望接入网络,实现:手机/电脑浏览器远程查看温湿度/电流/状态并下发控制指令;MQTT 上报到 Home Assistant 等平台;NTP 同步系统时间(无 RTC,重启归零);OTA 远程固件升级;首次配网走 AP 门户。

**前置问题(已确认,必须先修)**:`platformio.ini` 的 `platform=espressif32` 未锁版本,现解析到 v7.x(Arduino core 3.x),而 core 3.x 已**移除** `ledcSetup/ledcAttachPin`(`src/main.cpp` 第 81-89 行用的正是这套旧 API)——干净环境下项目目前编译不过。加联网前先锁回 core 2.x 兼容版本,不动既有热控代码。

## 技术栈与库选择
| 能力 | 库 | 说明 |
|---|---|---|
| WiFi 基础 | 内置 `WiFi.h` | 随 core 附带 |
| AP 配网门户 + 凭据持久化 | `tzapu/WiFiManager @ ^2.0.17` | 非阻塞模式,凭据存 NVS,resetSettings() 可清 |
| Web REST API | 内置 `WebServer.h` | **端口 81**(规避 WiFiManager 占 80),串行 handler,微秒级 |
| MQTT | `knolleary/PubSubClient @ ^2.8.0` | 设 `setBufferSize(512)` 容纳整快照 JSON |
| NTP | 内置 `configTime()` + `<time.h>` | CST=UTC+8 |
| OTA + mDNS | 内置 `ArduinoOTA` + `ESPmDNS.h` | 随 core 附带,需 OTA 分区表 |

JSON 用 `snprintf` 手写拼装,**不引入 ArduinoJson**(项目零 JSON 依赖,快照仅 ~12 字段)。

## 改动清单

### 1. `platformio.ini`(必须先改)
```ini
platform = espressif32@6.7.0       ; 精确锁版本:^6.7.0 仍会解析到 6.13.0(core 3.x)
; ...
board_build.partitions = default_16MB.csv   ; 16MB 含 OTA app0/app1 槽
lib_deps =
  adafruit/Adafruit AHTX0 @ ^2.0.5
  adafruit/Adafruit NeoPixel @ ^1.12.3
  bodmer/TFT_eSPI @ ^2.5.43
  tzapu/WiFiManager @ ^2.0.17
  knolleary/PubSubClient @ ^2.8.0
```

### 2. 新建 `include/network.h` + `src/network.cpp`
封装 `NetworkManager` 类(全局单例,持有 WiFiManager/WebServer/PubSubClient),对外接口:
```cpp
enum class NetState { Off, Connecting, Connected, ConfigPortal, MqttConnected };

class NetworkManager {
public:
  void begin(ChamberController &controller, UiModel &ui, const SystemSettings &settings);
  void loop();                                            // poll OTA/MQTT/WebServer/wm.process()
  void updateReadings(const Readings &r, const Outputs &o, float humidity);  // 主循环每周期喂数据
  void onSettingsChanged(const SystemSettings &s);       // 设置页改 MQTT/开关后重连
  NetState state() const;
  bool connected() const;
  String ipString() const;        // "x.x.x.x" 或 ""
  String timeString() const;      // NTP "HH:MM:SS" 未同步为 ""
  String hostname() const;        // "filament-chamber.local"
private:
  // WiFiManager 非阻塞、WebServer:81、PubSubClient、ArduinoOTA、NTP 全在此
  // Web/MQTT 命令统一经 ui.apply(UiAction, controller) 下发;切料用 ui.setMaterialIndex()+controller.setProfile()
  // WiFi.onEvent 回调内只置 volatile 标志,处理放 loop()
};
extern NetworkManager network;  // 全局实例
```
关键内部行为:
- **配网**:`WiFiManager wm; wm.setConfigPortalBlocking(false); wm.setConnectTimeout(15); wm.autoConnect("FilamentChamber-Setup")`。无凭据/连不上自动开 AP 门户,`wm.process()` 在 `loop()` 内调用,不阻塞 50ms PID。
- **WebServer:81**:仅当 `WiFi.status()==WL_CONNECTED` 后 `server.begin()`。端点:
  - `GET /api/state` → JSON:chamberC/heaterBoardC/humidity/currentA/voltageV/state/profile/exhaustPercent/heatPercent/light/net(ip/time)
  - `GET /api/settings` → JSON:当前 SystemSettings 关键项
  - `POST /api/settings?mqttEnabled=1&mqttBroker=...&mqttPort=1883&mqttTopicPrefix=chamber&ntpEnabled=1&otaEnabled=1&wifiEnabled=1` → 只覆盖请求中出现的字段,先回包再存 NVS,然后 `onSettingsChanged()` 重配子系统(无需重启)
  - `POST /api/action?name=toggleLight|toggleSystem|togglePreheat|...` → `ui.apply(对应 UiAction, controller)`
  - `POST /api/profile?index=N` → `ui.setMaterialIndex(N); controller.setProfile(N)`
  - `POST /api/wifi/reset` → `wm.resetSettings(); ESP.restart()`
- **MQTT**:连上后每 5s 发 `<prefix>/state` JSON;状态变化发 `<prefix>/event`;订阅 `<prefix>/cmd` 收 `{"action":"toggleLight"}` 或 `{"profile":2}`。断线 PubSubClient 自带重连。
- **NTP**:`configTime(8*3600, 0, "ntp.aliyun.com", "pool.ntp.org")`;`localtime_r()` 取时分秒。
- **OTA**:`ArduinoOTA.setHostname("filament-chamber")`;`onStart` 回调里 `controller.setSystemEnabled(false)` + `setHotPower(0)` 保安全(OTA 期间 PID 失步)。mDNS `MDNS.begin("filament-chamber")`。

### 3. `include/settings.h` + `src/settings.cpp` 扩展 SystemSettings
新增字段(Preferences 用 `putString/getString` 存 char[],`putBool/putUShort` 存标量):
```cpp
bool wifiEnabled = true;
bool mqttEnabled = false;
char mqttBroker[64] = "";
uint16_t mqttPort = 1883;
char mqttTopicPrefix[24] = "chamber";
bool ntpEnabled = true;
bool otaEnabled = true;
```
`settings.cpp` 的 `load()/save()` 加对应读写;`sanitize()` 加 `mqttPort=constrain(mqttPort,1,65535)`。

### 4. `include/ui_model.h` + `src/ui_model.cpp` 小改
新增公有方法 `void setMaterialIndex(size_t i)`,内部 `materialIndex_ = constrain(i,0,MATERIAL_COUNT-1)`。供 Web/MQTT 按索引切料(与现有 `apply(NextMaterial/PreviousMaterial)` 路径一致,均同步更新 controller profile)。

### 5. `src/main.cpp` 集成
- 头文件加 `#include "network.h"`。
- 文件作用域加 `NetworkManager network;`(或用头里的 extern)。
- `setup()` 传感器初始化后:`network.begin(controller, ui, settings);`
- `loop()` 顶部每周期:`network.loop();`
- `readSensors()` 后(即 500ms 分支内)调:`network.updateReadings(in, latestOutputs, humidity);`(in 为已构建的 Readings;去掉冗余的单独传 supplyV/currentA,它们已在 Readings 内)
- 设置页保存后调:`network.onSettingsChanged(settings);`
- 串口命令 `pollConsoleUi()` 旁可加 `'W'` 命令打印网络状态,便于调试。

## 已处理的关键风险(来自子代理验证)
1. ✅ platform 锁 `^6.7.0` 保留旧 ledc API,不动 6 处热控 PWM 调用。
2. ✅ 加 `default_16MB.csv` 分区表,OTA 才有 app0/app1 槽。
3. ✅ WebServer 用 81 端口 + STA 连上后启动,规避 WiFiManager 占 80 的 `bind: Address already in use`。
4. ✅ WiFiManager 非阻塞 `process()`,配网门户期间 PID 仍跑(扫描页 `scanNetworks()` 偶发 0.3–2s 抖动,仅首启/reset 发生,可接受)。
5. ✅ PubSubClient `setBufferSize(512)` 防快照 JSON 截断。
6. ✅ `WiFi.onEvent` 回调只置 volatile 标志,处理放 `loop()`(回调跑 wifi 任务,避免与主循环竞争)。
7. ✅ OTA `onStart` 关加热保安全。
8. ✅ TFT SPI(GPIO9–14、21)与 WiFi 射频无冲突;PSRAM(qio_opi)+ WiFi 共存无问题。
9. ✅ `updateReadings` 用 `Readings` 结构传参,supplyV/currentA 不重复(已在结构内),单线程值拷贝安全。
10. ✅ `heaterFanPercent` 真实百分比是私有,网络层报 0/100 与 TFT 一致(不加 getter,最小改动)。

## 验证方法
1. `pio run -e esp32-s3-n16r8` 编译通过(锁 platform 后旧 ledc API 不报错)。
2. 烧录后串口(115200)看启动日志:`WiFi: Connecting...` → 连上打印 IP;连不上自动开 `FilamentChamber-Setup` AP。
3. 首次配网:手机连 `FilamentChamber-Setup` → 浏览器自动弹出或访问 `192.168.4.1` → 输入家里 WiFi 密码提交 → 设备切 STA 重连。
4. 浏览器访问 `http://filament-chamber.local:81/api/state`(或 `http://<IP>:81/api/state`)返回温湿度/电流/状态 JSON。
5. `curl -X POST 'http://<IP>:81/api/action?name=toggleLight'` → TFT 照明应切换。
6. `curl -X POST 'http://<IP>:81/api/wifi/reset'` → 设备重启进配网门户。
7. MQTT:在 settings 开 `mqttEnabled` 填 broker,用 mosquitto_sub 订阅 `chamber/state` 每 5s 收到 JSON;发 `chamber/cmd {"action":"togglePreheat"}` 设备响应。
8. NTP:`/api/state` 的 `time` 字段从空变为 `HH:MM:SS`。
9. OTA:ArduinoIDE/PlatformIO OTA 上传主机名 `filament-chamber`,更新期间加热关闭,完成后自动重启。
10. 全程观察串口:50ms PID 节拍不被网络任务打断(温控日志稳定)。

## 不在本次范围
- 不迁移 ledc 到 core 3.x 新 API(锁 platform 即可)。
- 不加 ArduinoJson(手写 JSON)。
- 不加 Wi-Fi 信号强度图/历史曲线(仅实时快照)。
- 不动 TFT UI 显示网络状态(如需要可后续在 UiSnapshot 加 net 字段,本次仅串口暴露)。
