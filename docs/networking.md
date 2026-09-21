# 联网功能

联网由 `NetworkManager` 单例统管，实现在 [src/network.cpp](../src/network.cpp)，
全部在主循环 `loop()` 内轮询，**不阻塞 50 ms PID 热控节拍**。

| 能力 | 依赖 | 默认 |
| --- | --- | --- |
| WiFi 配网 | WiFiManager（非阻塞 AP 门户） | 开 |
| Web 管理页 / REST API | WebServer，标准端口 **80** | 随 WiFi |
| MQTT | PubSubClient | **关** |
| NTP 校时 | `configTzTime`，时区与服务器可配置 | 开 |
| OTA 升级 | ArduinoOTA + ESPmDNS | 开 |

五个开关都能在**屏幕的设置页**里改（见 [settings.md](settings.md)），也可经
`POST /api/settings` 远程改。**改完即时生效**，无需重启。

## 配网

首次上电或凭据失效时，设备开出热点：

- SSID：`FilamentChamber-Setup`
- 密码：无

手机连上后自动跳转配置页填入 2.4 GHz WiFi 凭据（**不支持 5 GHz**）。门户
3 分钟无操作自动关闭并重试，不会阻塞主循环。

配网成功并关闭 WiFiManager 门户后 WebServer 才启动，因此管理页可使用标准 80
端口。浏览器直接访问 `http://chamber.local/`；mDNS 不可用时改用设备 IP。

### 忘记密码 / 换网络

```powershell
curl -X POST http://<设备IP>/api/wifi/reset
```

设备清空凭据后重启，重新进入 `FilamentChamber-Setup` 门户。也可在设置页执行
「恢复出厂配置」。

## Web 管理页

打开 `http://chamber.local/` 可查看实时温湿度、WiFi 信号、SSID、IP、运行时间、
固件版本与断线诊断，并可配置 NTP/时区、MQTT、OTA 和静态 IPv4。页面每 3 秒刷新
一次，不依赖外部 CDN。

## REST API

基址 `http://<设备IP>`。读请求最短间隔 100 ms，写请求最短间隔 500 ms；超限
返回 HTTP 429。所有文本在输出 JSON 前进行转义，设置写入会先校验并确认 NVS
保存成功，失败时不改变运行配置。

### `GET /api/state`

实时状态。字段为 `null` 表示对应传感器读数无效。

```json
{
  "material": "PLA", "materialIndex": 0, "state": "Printing",
  "chamberC": 38.50, "heaterBoardC": 55.20, "humidity": 22.10,
  "currentA": 4.30, "voltageV": 24.10,
  "exhaustPercent": 40, "heatPercent": 65, "heaterFan": 100,
  "light": false, "systemEnabled": true, "pirMotion": true,
  "time": "14:32:05", "ip": "192.168.1.50", "net": "online",
  "ssid": "HomeWiFi", "rssi": -55, "uptimeSeconds": 3600,
  "firmware": "1.0.0", "disconnectCount": 1,
  "lastDisconnectReason": "NO_AP_FOUND", "lastDisconnectReasonCode": 201
}
```

`state` 取值：`Idle` / `Detecting` / `Preheat` / `Printing` / `Cooling` /
`Fault`。`net` 为 `online` / `offline`。

### `GET /api/settings`

```json
{
  "wifiEnabled": true, "mqttEnabled": false, "mqttBroker": "",
  "mqttPort": 1883, "mqttTopicPrefix": "chamber",
  "ntpEnabled": true, "otaEnabled": true,
  "timezone": "CST-8", "ntpServer1": "ntp.aliyun.com",
  "ntpServer2": "pool.ntp.org", "staticIpEnabled": false,
  "staticIp": "192.168.1.50", "staticGateway": "192.168.1.1",
  "staticSubnet": "255.255.255.0", "staticDns1": "223.5.5.5",
  "staticDns2": "1.1.1.1", "otaPassword": "CH-XXXXXXXX",
  "heaterMaxCurrentA": 6, "heaterFanPercent": 100, "heaterBoardLimitC": 80,
  "brightness": 80, "language": "zh"
}
```

### `POST /api/settings`

表单编码。**只覆盖请求中出现的字段**，未提交的保持原值。写入 NVS 后调用
`onSettingsChanged()` 立即重配。

| 字段 | 取值 |
| --- | --- |
| `mqttEnabled` | `1`/`0`、`true`/`false`、`on`/`off` |
| `mqttBroker` | 主机名或 IP，最长 63 字符 |
| `mqttPort` | 1–65535 |
| `mqttTopicPrefix` | 最长 23 字符 |
| `ntpEnabled` / `otaEnabled` | 同上布尔写法 |
| `language` | `zh`、`cn` 或 `en` |
| `timezone` | POSIX TZ，如中国 `CST-8`、UTC `UTC0` |
| `ntpServer1` / `ntpServer2` | NTP 主机名 |
| `staticIpEnabled` | 是否使用静态 IPv4 |
| `staticIp` / `staticGateway` / `staticSubnet` | 静态地址、网关和掩码 |
| `staticDns1` / `staticDns2` | DNS 地址 |

```powershell
# 打开 MQTT 并指向局域网 broker
curl -X POST http://192.168.1.50/api/settings -d "mqttEnabled=1&mqttBroker=192.168.1.10&mqttPort=1883&mqttTopicPrefix=chamber"

# 切换英文
curl -X POST http://192.168.1.50/api/settings -d "language=en"
```

> **`wifiEnabled=0` 会被拒绝并返回 400**。该接口本身走网络，关掉 WiFi 等于自断
> 通道，因此只能在屏幕设置页或串口操作。

成功返回 `{"ok":true}`，参数非法返回 400 带 `err` 说明。

### `POST /api/action`

```powershell
curl -X POST http://192.168.1.50/api/action -d "name=toggleLight"
```

可用 `name` 与界面按钮一一对应：

| `name` | 作用 |
| --- | --- |
| `toggleSystem` | 系统启停 |
| `togglePreheat` | 提前预热 |
| `toggleLight` | 灯光 |
| `toggleManualExhaust` | 手动强排 |
| `prevMaterial` / `nextMaterial` | 切换耗材 |
| `toggleAutoExhaust` | 自动排气 |
| `toggleAutoTemp` | 自动恒温 |
| `togglePostExhaust` | 打印结束排气 |

### `POST /api/profile`

修改当前耗材预设。`index` 为必填的耗材序号（0–11），其余字段可省略，省略时
沿用当前值。

```powershell
curl -X POST http://192.168.1.50/api/profile -d "index=0&minC=0&maxC=40&fanMin=30&fanMax=100&postFan=100&postSeconds=180"
```

约束：`minC` 0–90、`maxC` ≥ `minC+5` 且 ≤ 100、风速 0–100、
`postSeconds` 0–1800。越界返回 400。校验通过后立即持久化到 NVS。

### `POST /api/wifi/reset`

清空 WiFi 凭据并重启，见上文。

## MQTT

默认**关闭**（`mqttEnabled = false`），需在设置页或 `POST /api/settings` 打开并
填写 broker 地址。

### 主题

前缀取自 `mqttTopicPrefix`，默认 `chamber`。

| 主题 | 方向 | 载荷 | QoS / retain |
| --- | --- | --- | --- |
| `<前缀>/state` | 发布 | 同 `GET /api/state` 的 JSON | QoS 0 / **retain** |
| `<前缀>/event` | 发布 | `{"event":"<名称>"}` | QoS 0 / 不 retain |
| `<前缀>/online` | 发布 | 上线 `"1"`；异常离线由 LWT 发布 `"0"` | QoS 1 / retain |
| `<前缀>/cmd` | 订阅 | 见下文 | — |

`state` 每 5 秒上报一次并 retain，新订阅者能立刻拿到最后状态。

`event` 在**状态机跃迁**时上报（载荷为 `Idle`/`Preheat`/`Printing` 等 state
名），也在下发命令成功和 MQTT 上线时上报。

### 命令

```json
{"action": "toggleLight"}
{"profile": 3}
```

`action` 的取值同 `/api/action` 的 `name`。`profile` 为耗材序号。执行成功后会
在该主题回发一条 `event`。

```bash
mosquitto_pub -h 192.168.1.10 -t chamber/cmd -m '{"action":"togglePreheat"}'
mosquitto_sub -h 192.168.1.10 -t 'chamber/#' -v
```

### 重连

broker 不可达时每 5 秒重试一次，同一节流器也用于 `state` 周期上报。
`mqttBroker`、`mqttPort`、`mqttEnabled` 任一变化都会断开重连。
为保护 50 ms 温控节拍，预热/打印或加热输出不为零时不会发起可能阻塞的 MQTT
TCP 重连；温控停止后再恢复连接。单次 MQTT socket 等待上限为 1 秒。

## NTP 校时

```cpp
configTzTime(timezone, ntpServer1, ntpServer2);
```

默认使用 `CST-8`（UTC+8），时区和两个 NTP 服务器均可在 Web 管理页配置。采用
POSIX TZ 语法，例如 UTC 使用 `UTC0`。同步成功后主界面显示完整时间。

断网时的回退顺序：NTP → 上次手动校时（`manualClockEpoch`）→ 占位串
`----/--/-- --:--:--`。手动校时在设置页的「日期」「时间」两项完成，写入 RTC 与
NVS，重启后仍生效。

关闭 NTP 会调用 `esp_sntp_stop()`，已同步的时钟在断电前继续保持。

## OTA 升级

主机名 `chamber`。OTA 密码根据 ESP32 芯片 MAC 为每台设备生成，格式为
`CH-XXXXXXXX`，可在 Web 管理页查看，也会在启动串口日志中打印。

- Arduino IDE：工具 → 端口 → 选择 `chamber`
- PlatformIO：需显式指定 espota 协议，因为 `platformio.ini` 未配置
  `upload_protocol`，默认走串口 esptool

```powershell
pio run -t upload --upload-port chamber.local --upload-protocol espota
```

若上面这条不便使用，也可在 `platformio.ini` 的 `[env:esp32-s3-n16r8]` 中加
`upload_protocol = espota` 与 `upload_port = chamber.local` 固化下来。

前提是 `board_build.partitions` 指定的分区表带 `app0`/`app1` 双槽，详见
[build-and-flash.md](build-and-flash.md)。

**OTA 开始时会自动关闭加热**（`controller_->setSystemEnabled(false)`），因为刷写
期间 PID 节拍会失步，必须切断加热保证安全。升级完成后需手动重新启动系统。

关闭 OTA 会调用 `ArduinoOTA.end()`，之后该设备不再出现在 OTA 端口列表中。已停
的 OTA 无法远程重新打开（通道已断），只能回设置页或重启。

## mDNS

STA 连上后注册 `chamber.local`，因此可以用主机名代替 IP 访问：

```
http://chamber.local/api/state
```

Windows 需安装 Bonjour（装过 iTunes 即有）。解析失败时改用 IP。

## 状态与串口调试

`NetState` 五态：`Off`（WiFi 关闭）、`Connecting`、`Connected`、`ConfigPortal`、
`MqttConnected`。

串口按 `W` 打印当前联网配置（读 `main` 侧的 settings 拷贝）。关键日志前缀：
`[NET]`、`[MQTT]`、`[NTP]`、`[OTA]`。

## 已知限制

- 只支持 **2.4 GHz** WiFi。
- 未做 HTTPS/TLS：REST 无鉴权，MQTT 无账号密码；OTA 已使用每台设备独立密码。
  **不要暴露到公网**，仅限可信局域网使用。
- `GET /api/settings` 会把 `mqttBroker` 明文返回，无脱敏。
- 屏幕设置页只暴露联网开关；MQTT、NTP、时区和静态 IP 的文本参数在 Web 管理页配置。
