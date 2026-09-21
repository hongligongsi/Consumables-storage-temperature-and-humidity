# ESP32-S3 N16R8 温控主板

本工程按 `SCH_1-P1_2026-01-30-X3hS6itA.png` 创建，使用 Arduino 框架与 PlatformIO。控制逻辑参考 SmartChamber-3D 的公开功能描述：PIR 判断打印状态、PID 仓温控制、超温强排气、打印结束按耗材设定排气，以及耗材预设。

`src/controller.cpp` 包含 PLA、PETG、TPU、ABS、ASA、PC、PA、PVA、PET、PPA、PEBA 与 CUSTOM 的温度/安全阈值预设。它与硬件访问分开，屏幕或编码器 UI 可直接调用 `setProfile()`、`requestPreheat()` 和 `setHeatLimit()`。

控制状态为 `Idle → Preheat / Printing → Cooling`；温度采样连续失效 3 秒进入锁定 `Fault`，任何状态温度达到耗材安全阈值则停止加热并 100% 排气。`Fault` 不会自动复位，避免传感器偶发恢复后重新加热。

控制器现在以 50 ms 节拍工作，包含 `Idle → Detecting → Printing → Cooling` PIR 三态、分段排气、仓温/热板双 PID、热板硬过温、INA226 电流软降和 3%/周期 PWM 斜率限制。热板 NTC 与 INA226 的读取代码已接入，但 GPIO、NTC 参数和 INA226 地址仍须实物验证；`HEATER_ENABLED` 默认关闭，因此不会误启动加热。

Gerber 飞针网表已确认 `GPIO8`（打印中）和 `GPIO47`（加热中）为独立 3.3 V 状态输出，因此已启用。外接负载必须经光耦或驱动器。

## 目录

- [详细文档](#详细文档)
- [工程目录结构](#工程目录结构)
- [功能特性总览](#功能特性总览)
- [控制状态机](#控制状态机)
- [引脚与接口分配](#引脚与接口分配)
- [耗材预设与安全阈值](#耗材预设与安全阈值)
- [REST 接口与联网功能](#rest-接口与联网功能)
- [主界面交互映射](#主界面交互映射)
- [字库生成](#字库生成)
- [系统设置说明](#系统设置说明)
- [编译与烧录](#编译与烧录)
- [重要硬件核对](#重要硬件核对)
- [待验证项清单](#待验证项清单)
- [常见问题](#常见问题)

## 详细文档

见 [docs/](docs/README.md)：[硬件接线](docs/hardware.md) · [编译烧录](docs/build-and-flash.md) · [联网功能](docs/networking.md) · [系统设置](docs/settings.md) · [第三方软件声明](THIRD_PARTY_NOTICES.md)。

## 工程目录结构

```text
3-1/
├─ platformio.ini            PlatformIO 工程配置（板卡/分区/库依赖/构建宏）
├─ README.md                 本文件
├─ LICENSE                   Apache License 2.0 全文
├─ .gitignore                忽略 .pio 与 .vscode 本地文件
├─ THIRD_PARTY_NOTICES.md    第三方依赖许可证声明
├─ docs/                     详细文档
│  ├─ README.md              文档索引
│  ├─ hardware.md            硬件接线与引脚
│  ├─ build-and-flash.md     编译与烧录细节
│  ├─ networking.md          WiFi / Web 管理页 / REST / MQTT / NTP / OTA
│  ├─ control.md             双 PID 协同、过流软降、斜率限制与安全停机
│  └─ settings.md            27 项系统设置逐项说明
├─ firmware/
│  └─ firmware.bin           与当前源码对应的预编译应用镜像（约 1.2 MB，1231056 字节）
├─ include/                  接口与常量（头文件）
│  ├─ version.h              固件版本宏 FW_VERSION（当前 1.0.0）
│  ├─ pins.h                 全部 GPIO 映射与外设开关宏（唯一引脚来源）
│  ├─ controller.h           状态枚举、MaterialProfile、ChamberController 接口
│  ├─ settings.h             SystemSettings 结构与默认值
│  ├─ ui_model.h             UiAction / MainFocus / SystemSettingField 枚举、UiSnapshot 快照（194 行）
│  ├─ tft_ui.h               TftUi 显示接口（15 行）
│  ├─ network.h              NetworkManager 接口 / NetState
│  ├─ web_page.h             内置 Web 管理页 HTML
│  ├─ wifi_portal_page.h     WiFi 配网门户定制页 HTML
│  ├─ ina226_sensor.h        INA226 采样接口（16 行）
│  ├─ font_cn16.h            16 px 中文 + ASCII VLW 字库（genvlw.py 生成，约 336 KB）
│  └─ font_cn26.h            26 px 中文 + ASCII VLW 字库（genvlw.py 生成，约 805 KB）
├─ src/                      实现
│  ├─ main.cpp               启动、外设初始化、50 ms 控制节拍、输入分发
│  ├─ controller.cpp         状态机、12 种耗材预设、双 PID、安全联锁
│  ├─ settings.cpp           NVS 读写与参数范围钳制
│  ├─ ui_model.cpp           交互模型：动作映射、焦点移动、设置项编辑
│  ├─ tft_ui.cpp             TFT_eSPI 渲染：主界面 / 设置页 / 故障页 / 触摸校准
│  ├─ network.cpp            WiFiManager 配网、Web 管理页/REST :80、MQTT、NTP、OTA
│  └─ ina226_sensor.cpp      INA226 I²C 采样实现
├─ chamber-web-preview.html  内置 Web 管理页的浏览器预览稿
├─ wifi-setup-preview.html   WiFi 配网门户的浏览器预览稿
└─ tools/
   ├─ genvlw.py               从系统 TTF/TTC 抽取汉字生成 VLW 字库
   ├─ ui_preview.html         可操作的真机界面模拟器
   └─ preview_cn.png          字库生成预览图
```

`.pio/` 为 PlatformIO 构建产物目录（含 `build/esp32-s3-n16r8/` 下的 `bootloader.bin`、`partitions.bin`、`firmware.bin`），不入版本库。

## 功能特性总览

| 模块 | 关键实现 | 说明 |
| --- | --- | --- |
| 状态机 | `ChamberController::update()`（`src/controller.cpp`） | 六态：`Idle / Detecting / Preheat / Printing / Cooling / Fault`，50 ms 更新一次 |
| 仓温闭环 | AHT20（I²C）+ 位置式 PID | 目标为当前耗材的仓温下限，KP=8.0 / KI=0.04 / KD=15.0 |
| 热板保护 | 独立 NTC + 热板 PID | 目标为「发热板温度保护」阈值（默认 80 ℃），KP=4.0 / KI=0.02 / KD=3.0 |
| 功率输出 | 取两个 PID 的较小值 | 再依次经过电流软降、用户功率上限、3%/周期斜率限制 |
| 自动排气 | 按仓温高出最低仓温的幅度分三档 | 高出 ≥10 ℃ 用最高风速，≥5 ℃ 用中值，否则最低风速 |
| 打印结束排气 | 进入 `Cooling` 计时 | 风速/时长取自耗材预设，可在设置页与 REST 修改 |
| 安全联锁 | 热板硬过温 + 仓温超上限 | 立即停加热、排气 100%、热板风扇全开，带 5 ℃/2 ℃ 回差 |
| 电流限制 | INA226 分流采样 | 实测电流超限时按 3%/周期降压，读数失败视为传感器无效 |
| 交互 | EC11 编码器 + 触摸（预留）+ 串口单键 | 旋转 / 单击 / 双击 / 长按四种手势 |
| 显示 | TFT_eSPI，480×320 横屏 | PSRAM 内 16-bit 双缓冲 Sprite，独立低优先级渲染任务 |
| 主题 | 日间 / 夜间 / 自动三档 | 自动模式按「日间开始时刻 / 夜间开始时刻」切换 |
| 联网 | WiFiManager + WebServer(80) + PubSubClient + SNTP + ArduinoOTA | 定制配网门户、Web 管理页、REST、MQTT、NTP、OTA 全部非阻塞 |
| 持久化 | NVS（`SettingsStore`） | 设置、耗材预设、手动校时基准；恢复出厂只删本固件拥有的键 |

## 控制状态机

状态枚举定义在 `include/controller.h` 的 `ChamberState`，屏幕与 REST 输出的名称映射见下表。

| 状态 | 屏幕文案（中/英） | REST `state` | 含义与进入条件 |
| --- | --- | --- | --- |
| `Idle` | 待机 / IDLE | `Idle` | 默认态：系统开启但无运动、无预热请求 |
| `Detecting` | 检测中 / DETECT | `Detecting` | PIR 连续检测到运动，但尚未达到 `PIR启动延时` |
| `Preheat` | 预热 / PREHEAT | `Preheat` | 「提前预热」被开启（`requestPreheat()` / 串口 `p` / REST `togglePreheat`），优先于 PIR |
| `Printing` | 打印 / PRINT | `Printing` | 运动持续 ≥ `PIR启动延时`（默认 25 s，1–300 s 可调）后判定开始打印 |
| `Cooling` | 排气 / EXHAUST | `Cooling` | ① 打印结束且「打印后排风」开启、时长为正时计时排气；② 安全联锁触发时的强制排气 |
| `Fault` | 故障 / FAULT | `Fault` | 温度采样连续失效 3 s 后锁定 |

转移规则（`src/controller.cpp`）：

| 当前态 | 条件 | 目标态 |
| --- | --- | --- |
| 任意 | `systemEnabled == false` | `Idle`（清冷却计时与安全标记） |
| 任意（预热/打印中） | 传感器无效持续 ≥ 3 s（`SENSOR_FAULT_MS`） | `Fault`（锁定） |
| `Fault` | — | 保持 `Fault`，需关闭再开启系统才允许重新进入状态机 |
| 任意 | 热板 ≥ 保护温度 或 仓温 ≥ 耗材上限 | `Cooling`（含安全标记） |
| `Cooling`（安全） | 热板 ≥ 保护温度 −5 ℃ 或 仓温 ≥ 上限 −2 ℃ | 继续 `Cooling`（回差） |
| 任意 | 预热请求开启 | `Preheat` |
| 任意 | PIR 达标 或 在 `Printing` 且距最近运动 < `PIR关闭延时` | `Printing` |
| 任意 | PIR 运动持续但未达启动延时 | `Detecting` |
| `Printing` | 运动停止超时，且「打印后排风」开启且时长 > 0 | `Cooling`（开始计时） |
| `Printing` | 运动停止超时，且结束排气关闭或时长为 0 | `Idle` |
| `Cooling` | 结束排气计时未满 | 继续 `Cooling` |
| 其余 | — | `Idle` |

各状态的实际输出（`Outputs`）：

| 状态 | 加热 | 排气 | 热板风扇 |
| --- | --- | --- | --- |
| `Preheat` / `Printing` | 双 PID 计算值（受电流、上限、斜率约束） | 自动排气开启时按分档风速 | 加热 > 0 或 热板 > 仓温 + 10 ℃ |
| `Cooling` | 0 | 安全联锁 100%，否则取耗材「结束排气风速」 | 仅安全联锁时开启 |
| `Fault` | 0 | 100% | 开启 |
| `Idle` / `Detecting` | 0 | 0 | 关闭 |

控制参数与常量（`src/controller.cpp`、`src/main.cpp`）：

| 项 | 值 | 位置 |
| --- | --- | --- |
| 控制节拍 | 50 ms | `src/main.cpp` `loop()` |
| 传感器采样周期 | 500 ms | `src/main.cpp` `loop()` |
| 状态快照/刷屏周期 | 500 ms | `src/main.cpp` `loop()` |
| 串口状态打印周期 | 2000 ms | `src/main.cpp` `loop()` |
| 传感器失效判定 | 3 s | `SENSOR_FAULT_MS` |
| 仓温 PID | KP 8.0 / KI 0.04 / KD 15.0，积分限幅 ±200 | `chamberPid()` |
| 热板 PID | KP 4.0 / KI 0.02 / KD 3.0，积分限幅 ±300 | `boardPid()` |
| PWM 斜率限制 | 3 %/周期（即 60 %/s） | `PWM_SLOPE_PER_50MS` |
| 功率上限 | `setHeatLimit(100)`，默认 100 % | `setup()` |
| 温度采样量程保护 | NTC −40–250 ℃、AHT20 −40–100 ℃ / 0–100 %RH、电流 \|I\| ≤ 32 A、电压 0–40 V | `readSensors()` |

## 引脚与接口分配

全部引脚集中在 `include/pins.h` 的 `Pin` 命名空间，改板只改这一处。

| 功能 | 宏 | GPIO | 说明 |
| --- | --- | --- | --- |
| 显示复位 | `TFT_RESET` | 9 | ST7796，SPI 接口 |
| 显示 MISO | `TFT_SPI_MISO` | 10 | |
| 显示 MOSI | `TFT_SPI_MOSI` | 11 | |
| 显示时钟 | `TFT_SPI_SCLK` | 12 | |
| 数据/命令 | `TFT_DATA_CMD` | 13 | |
| 片选 | `TFT_CHIP_SELECT` | 14 | |
| 背光 | `TFT_BACKLIGHT` | 21 | LEDC 通道 2，5 kHz，10 bit |
| 触摸 YD / XR / YU / XL | `TFT_YD` / `TFT_XR` / `TFT_YU` / `TFT_XL` | 4 / 5 / 6 / 7 | 四线电阻触摸，`HAS_TOUCH_PANEL = false` 时完全停用 |
| 编码器按键 | `ENCODER_KEY` | 15 | 输入上拉，单击 / 双击 / 长按 |
| 编码器 B / A | `ENCODER_B` / `ENCODER_A` | 16 / 17 | 输入上拉，正交解码 |
| 状态灯 | `RGB` | 18 | 单灯珠 WS2812（GRB，800 kHz） |
| 蜂鸣器 | `BUZZER` | 1 | 数字输出 |
| I²C 数据 / 时钟 | `I2C_SDA` / `I2C_SCL` | 42 / 41 | AHT20（仓温/湿度）+ INA226 |
| 热端 PWM | `HOT_PWM` | 40 | LEDC 通道 0，20 kHz，10 bit |
| 排气 PWM | `AIR_FAN_PWM` | 39 | LEDC 通道 1，25 kHz，10 bit |
| 排气开关 | `AIR_FAN_DC` | 38 | 数字输出，排气 > 0 时置高 |
| 照明使能 | `LED_ENABLE` | 37 | 数字输出 |
| 热板风扇 | `HOT_FAN` | 36 | LEDC 通道 3，25 kHz，10 bit |
| 人体感应 | `PIR` | 35 | 数字输入，高电平表示有运动 |
| 主板风扇 | `BOARD_FAN` | 48 | 数字输出 |
| 热板 NTC | `ADC_NTC` | 3 | 12 bit ADC，11 dB 衰减 |
| 打印中状态输出 | `PRINTING_STATUS` | 8 | 独立 3.3 V 输出，`HAS_STATUS_OUTPUTS = true` |
| 加热中状态输出 | `HEATING_STATUS` | 47 | 独立 3.3 V 输出，`HEATER_ENABLED && heaterPercent > 0` 时置高 |

PWM 通道分配：

| 通道 | 宏 | 频率 | 分辨率 | 引脚 |
| --- | --- | --- | --- | --- |
| 0 | `HOT_PWM_CHANNEL` | 20 kHz | 10 bit | GPIO40 |
| 1 | `AIR_FAN_PWM_CHANNEL` | 25 kHz | 10 bit | GPIO39 |
| 2 | `BACKLIGHT_PWM_CHANNEL` | 5 kHz | 10 bit | GPIO21 |
| 3 | `HOT_FAN_PWM_CHANNEL` | 25 kHz | 10 bit | GPIO36 |

状态灯（RGB）颜色与闪烁约定（`updateStatusRgb()`）：

| 条件 | 颜色 | 闪烁 |
| --- | --- | --- |
| `Fault` | 红 | 快闪（180 ms） |
| 加热中（`heaterPercent > 0`） | 橙 | 慢闪（450 ms） |
| 手动强排 | 蓝 | 快闪（180 ms） |
| `Cooling` | 橙黄 | 慢闪（450 ms） |
| 热板风扇开启 | 紫 | 慢闪（450 ms） |
| `Printing` | 蓝（0,80,180） | 常亮 |
| `Preheat` | 紫（120,20,180） | 常亮 |
| `Detecting` 或有运动 | 黄 | 慢闪（450 ms） |
| 其他 | 绿（0,70,18） | 常亮 |

NTC 换算：`readNtcCelsius(pin, seriesOhm, nominalOhm, beta)` 默认按 10 kΩ 上拉 / 10 kΩ / B=3950；热板实际调用为 100 kΩ / 100 kΩ / B=3950，采样值 `raw <= 0 || raw >= 4095` 时返回 `NAN`（视为无效）。

## 耗材预设与安全阈值

`src/controller.cpp` 的 `MATERIALS[]` 与 `resetMaterialProfiles()` 给出 12 组出厂预设：

| 耗材 | 仓温下限 (℃) | 仓温上限 (℃) | 自动排气最低风速 (%) | 自动排气最高风速 (%) | 结束排气风速 (%) | 结束排气时长 (s) |
| --- | --- | --- | --- | --- | --- | --- |
| PLA | 0 | 40 | 30 | 100 | 100 | 180 |
| PETG | 25 | 50 | 30 | 80 | 80 | 180 |
| TPU | 0 | 40 | 0 | 30 | 30 | 120 |
| ABS | 40 | 70 | 10 | 30 | 30 | 300 |
| ASA | 40 | 70 | 10 | 30 | 30 | 300 |
| PC | 50 | 90 | 10 | 30 | 30 | 300 |
| PA | 40 | 70 | 10 | 30 | 30 | 300 |
| PVA | 0 | 40 | 20 | 60 | 60 | 180 |
| PET | 25 | 50 | 30 | 80 | 80 | 180 |
| PPA | 50 | 90 | 10 | 30 | 30 | 300 |
| PEBA | 25 | 50 | 20 | 60 | 60 | 180 |
| CUSTOM | 0 | 40 | 0 | 100 | 100 | 180 |

预设修改的校验规则（`updateProfile()`，UI、REST、MQTT 三条路径共用）：

| 参数 | 允许范围 | 约束 |
| --- | --- | --- |
| 仓温下限 `chamberMinC` | 0–90 ℃ | 与上限至少相差 5 ℃ |
| 仓温上限 `chamberMaxC` | 5–100 ℃ | 同上 |
| 最低风速 `fanMinPercent` | 0–100 % | 不高于最高风速 |
| 最高风速 `fanMaxPercent` | 0–100 % | 不低于最低风速 |
| 结束排气风速 `postExhaustPercent` | 0–100 % | — |
| 结束排气时长 `postExhaustSeconds` | 0–1800 s | 0 表示关闭结束排气 |

编码器编辑步长（`adjustProfile()`）：仓温上下限 ±1 ℃、风速 ±1 %（受互相约束）、结束排气风速 ±5 %、结束排气时长 ±30 s（上限 1800 s）。

安全阈值与限流：

| 项 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- |
| 发热板温度保护 `heaterBoardLimitC` | 80 ℃ | 40–180 ℃ | 热板超此值即停加热并 100 % 排气；回差 5 ℃ |
| 仓温上限（按耗材） | 见表 | 5–100 ℃ | 超限同上，回差 2 ℃ |
| 发热板限流 `heaterMaxCurrentA` | 6 A | 1–12 A | 实测电流超限按比例降功率 |
| 发热板风扇风速 `heaterFanPercent` | 100 % | 20–100 % | 热板风扇 PWM 占空比 |

## REST 接口与联网功能

配网门户关闭后 WebServer 才启动，因此管理页与 API 直接使用**标准 80 端口**（访问 `http://chamber.local/`），仅在 STA 已连接时处理请求，响应均带 `Access-Control-Allow-Origin: *`。读请求限流 100 ms、写请求 500 ms，超限返回 429。

| 方法 | 路径 | 参数 | 返回 |
| --- | --- | --- | --- |
| GET | `/api/state` | — | 状态 JSON（见下表） |
| GET | `/api/settings` | — | 联网与本地关键设置 |
| POST | `/api/settings` | `mqttEnabled` / `mqttBroker` / `mqttPort`(1–65535) / `mqttTopicPrefix` / `ntpEnabled` / `otaEnabled` / `language`(`zh`\|`en`) / `timezone` / `ntpServer1`/`ntpServer2` / `staticIp*`（完整字段见 [docs/networking.md](docs/networking.md)） | `{"ok":true}`；`wifiEnabled=0` 被拒绝返回 400 |
| POST | `/api/action` | `name` | `{"ok":true}` 或 `{"ok":false,"err":"bad action"}` |
| POST | `/api/profile` | `index` 必填；可选 `minC` / `maxC` / `fanMin` / `fanMax` / `postFan` / `postSeconds` | 校验并写入 NVS，失败返回 400/500 |
| POST | `/api/wifi/reset` | — | 清除 WiFi 凭据并重启进入配网门户 |

`GET /api/state` 字段：

| 字段 | 含义 |
| --- | --- |
| `material` / `materialIndex` | 当前耗材名称与索引 |
| `state` | `Idle` / `Detecting` / `Preheat` / `Printing` / `Cooling` / `Fault` |
| `chamberC` / `heaterBoardC` / `humidity` | 仓温、热板温度、湿度（无效时为 `null`） |
| `currentA` / `voltageV` | 加热电流与供电电压 |
| `exhaustPercent` / `heatPercent` / `heaterFan` | 排气、加热、热板风扇输出百分比 |
| `light` / `systemEnabled` / `pirMotion` | 灯光、系统启停、PIR 状态 |
| `time` / `ip` / `net` | 当前时间、IP、`online`/`offline` |

`POST /api/action` 支持的 `name`：

| 动作名 | 效果 |
| --- | --- |
| `toggleSystem` | 系统启停 |
| `togglePreheat` | 提前预热开关 |
| `toggleLight` | 灯光开关 |
| `toggleManualExhaust` | 手动强排（100 %） |
| `prevMaterial` / `nextMaterial` | 上一个 / 下一个耗材 |
| `toggleAutoExhaust` | 自动排气开关 |
| `toggleAutoTemp` | 自动恒温开关 |
| `togglePostExhaust` | 打印结束排气开关 |

MQTT（需先在 Web 接口配置 broker）：

| 主题 | 方向 | 载荷 |
| --- | --- | --- |
| `<prefix>/state` | 上报 | 与 `/api/state` 相同，每 5 s 一次，retain |
| `<prefix>/event` | 上报 | `{"event":"<动作名/状态名/online>"}` |
| `<prefix>/online` | 上报 | LWT，离线自动置 `"0"` |
| `<prefix>/cmd` | 订阅 | `{"action":"<动作名>"}` 或 `{"profile":<索引>}` |

其他联网能力：

- **WiFi 配网**：无凭据时启动 AP 门户，SSID 为 `FilamentChamber-Setup`，门户提供扫描列表与定制配网页，180 s 无操作自动关闭并重试；STA 连接超时 15 s。配网过程非阻塞，不打断 50 ms 控制节拍。
- **Web 管理页**：浏览器访问 `http://chamber.local/`，可查看实时状态、配置 MQTT、NTP/时区与静态 IPv4，每 3 秒刷新，无外部 CDN 依赖。
- **mDNS**：主机名固定为 `chamber`（即 `chamber.local`），便于局域网直接访问。
- **NTP**：`configTzTime`，默认时区 `CST-8`（UTC+8）、服务器 `ntp.aliyun.com` 与 `pool.ntp.org`，时区与服务器均可在 Web 管理页修改；可在设置页关闭。
- **OTA**：ArduinoOTA，主机名 `chamber`；密码按芯片 MAC 逐机生成，格式 `CH-XXXXXXXX`，可在 Web 管理页查看。OTA 开始时自动关闭加热，避免 PID 失步。关闭 OTA 后设备不再出现在 OTA 端口列表，且无法远程重新打开。
- **静态 IPv4**（可选）：默认 DHCP；在 Web 管理页填入地址/网关/掩码/DNS 后即时生效。

## 主界面交互映射

`UiModel` 与所给主界面标注对应：耗材前后切换、自动排气、自动恒温、结束排气、系统工作状态、提前预热与灯光开关。未接上屏幕时可通过串口调试：`[`/`]` 切换耗材，`e` 排气自动控制，`t` 恒温自动控制，`p` 预热，`l` 灯光，`s` 系统启停，`x` 手动强排。

主界面支持无触摸 EC11 完整导航：旋转移动白色/橙色焦点，单击执行当前项，双击开启/关闭手动强排，长按开启/关闭系统。可聚焦上一耗材、当前耗材设置、下一耗材、三个自动开关，以及底部的系统启停、提前预热、灯光和系统设置。进入耗材设置页后，旋转修改选中值，单击依次选择最低/最高仓温、最低/最高自动排气风速、打印结束排气风速和排气时长，双击选择上一项，长按写入 NVS 并返回。温度上下限至少相差 5℃，自动风速上下限会互相约束，打印结束风速范围为 0–100%，排气时长范围为 0–1800 秒（0 表示关闭）。REST `POST /api/profile` 除 `index` 外也接受 `minC`、`maxC`、`fanMin`、`fanMax`、`postFan`、`postSeconds`，校验通过后立即持久化。

主界面底部第一格为 PIR 状态指示，其余四格均可通过 EC11 聚焦并执行。进入 27 项系统设置后由 EC11 旋转选择，单击切换或进入数值编辑，长按保存返回。系统设置包括中英文、WiFi/MQTT/NTP/OTA 四项联网开关、按键音、亮度/休眠、打印保持亮屏、编码器方向、PIR 延时、灯光/蜂鸣联动、发热板限流/风扇 PWM/温度保护、屏幕配色（日间/夜间/自动）与日间/夜间开始时刻、日期与时间校准、恢复出厂和固件版本。当前硬件配置 `Pin::HAS_TOUCH_PANEL=false`，触摸轮询被完全停用，触摸校准项显示“不支持”；更换四线电阻触摸屏时可改为 `true` 后校准。恢复出厂只删除固件拥有的设置与耗材键，不清除未知键，因此预留的注册码数据会保留。

主界面时钟行显示完整的 `YYYY-MM-DD HH:MM:SS`，数据优先取自 NTP；断网且未同步时回退到上次手动校时值（`manualClockEpoch`），两者都无有效时间时显示占位串。系统设置的“日期/时间”两项同样直接改写系统时钟并写入 NVS，因此手动校时结果重启后仍然有效。

`SystemSettings` 已持久化语言、按键声音、屏幕亮度/休眠、打印保持亮屏、编码器方向、PIR 启动/关闭延时、自动灯光/蜂鸣、发热板最大电流、发热板风扇速度及保护温度，以及屏幕配色（`theme`）、日间/夜间开始时刻（`dayStartMinutes`/`nightStartMinutes`）和手动校时基准（`manualClockEpoch`）。参数范围会在载入/保存时限制在安全区间。触摸屏校准数据需在确定触摸控制器型号后另行存储。

主界面采用 480×320 三分布局：左侧控制区、右侧 2×4 仪表、底部 5 个入口。整帧先绘制到 PSRAM 中的两帧 16-bit Sprite，再一次推送到屏幕；显示刷新在独立低优先级任务中执行，主循环提交快照时只覆盖长度为 1 的队列，不会等待 SPI 刷屏。PSRAM 初始化失败时自动降级为直绘并在串口报告。`SystemSettings.language` 载入后直接驱动中英文文案，REST 设置接口也接受 `language=zh|en` 并立即切换。

补充的界面细节：

- 右侧 2×4 仪表依次为：排风、主板（MCU）、湿度、仓温、热风（热板风扇）、热板、电压、电流；热板数值 ≥ 保护温度时该格变红。
- 底部五格：PIR 状态、系统启停、提前预热、灯光、系统设置。第 1 格只是状态指示，不可聚焦、不可点击（触摸与编码器均跳过）。
- 编码器手势判定：正交解码累计 ±4 计数触发一次旋转；按键消抖 30 ms，单击/双击判定窗口 350 ms，长按阈值 800 ms。
- 触摸（`HAS_TOUCH_PANEL = true` 时）：35 ms 采样一次，需连续两帧释放才判定抬手；屏幕休眠时首次触摸只唤醒屏幕；耗材/系统设置页内触摸被禁用，避免误改参数。
- 屏幕休眠只关闭背光，系统继续运行；「打印时保持屏幕开启」为开时打印过程中不休眠。

## 字库生成

字库由 `tools/genvlw.py` 从系统中文 TTF/TTC 抽取源码所需汉字和可打印 ASCII，生成 16 px/26 px 两套 TFT_eSPI VLW 头文件。脚本会自动搜索 Windows、macOS、Linux 常见字体，也可显式指定：

```bash
python3 -m pip install Pillow
python3 tools/genvlw.py --font /path/to/chinese-font.ttf --preview
```

## 系统设置说明

系统设置共 27 项，顺序与屏幕显示一致，**逐项说明与取值范围见
[docs/settings.md](docs/settings.md)**。联网相关的 WiFi / MQTT / NTP / OTA 四项
开关在长按保存后即时生效，无需重启。

界面布局与中英文、日/夜配色可参考 `tools/ui_preview.html`（浏览器直接打开）。该页是可操作的真机模拟器：真机每屏只显示 5 行，需旋转滚动才能看全 27 项，页内因此把 27 项在屏下逐条列出说明与取值范围，并随屏内光标实时高亮。顶部快捷按钮中的“夜间/日间/自动”等价于修改“屏幕配色”这一项。

设置页取值范围与默认值（`src/ui_model.cpp` 限制，`src/settings.cpp` 二次夹取）：

| # | 条目 | 默认 | 取值范围 | 步长 |
| --- | --- | --- | --- | --- |
| 1 | 系统语言 | 中文 | 中文 / English | — |
| 2 | WiFi 联网 | 开 | 开 / 关 | — |
| 3 | MQTT 上报 | 关 | 开 / 关 | — |
| 4 | NTP 校时 | 开 | 开 / 关 | — |
| 5 | OTA 升级 | 开 | 开 / 关 | — |
| 6 | 按键声音 | 开 | 开 / 关 | — |
| 7 | 屏幕亮度 | 80 % | 1–100 % | 5 |
| 8 | 屏幕休眠时间 | 60 s | 0–3600 s（0 = 不休眠） | 15 |
| 9 | 打印时保持屏幕开启 | 开 | 开 / 关 | — |
| 10 | 编码器方向 | 正向 | 正向 / 反向 | — |
| 11 | PIR 启动延时 | 25 s | 1–300 s | 1 |
| 12 | PIR 关闭延时 | 50 s | 10–900 s | 5 |
| 13 | 启动后自动开灯 | 开 | 开 / 关 | — |
| 14 | 关闭后自动关灯 | 开 | 开 / 关 | — |
| 15 | 启动后蜂鸣提示 | 开 | 开 / 关 | — |
| 16 | 关闭后蜂鸣提示 | 开 | 开 / 关 | — |
| 17 | 发热板限流 | 6 A | 1–12 A | 1 |
| 18 | 发热板风扇风速 | 100 % | 20–100 % | 5 |
| 19 | 发热板温度保护 | 80 ℃ | 40–180 ℃ | 1 |
| 20 | 屏幕配色 | 夜间 | 日间 / 夜间 / 自动 | — |
| 21 | 日间开始时刻 | 06:00 | 00:00–23:45 | 15 min |
| 22 | 夜间开始时刻 | 18:00 | 00:00–23:45 | 15 min |
| 23 | 日期 | — | 年 / 月 / 日 三段 | — |
| 24 | 时间 | — | 时 / 分 两段 | — |
| 25 | 触摸屏校准 | — | 不支持 / 未校准 / 已校准 | — |
| 26 | 恢复出厂配置 | — | 执行 / 确认? | — |
| 27 | 固件版本 | 1.0.0 | 只读 | — |

说明：设置项总数由 `SystemSettingField::Count` 哨兵推导，页码与滚动窗口动态计算，增删条目无需改渲染代码；「自动」配色按日间/夜间两个时刻在跨零点场景下取区间补集，无有效时间时按夜间渲染。

## 编译与烧录

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

### 烧录预编译固件

仓库中的 `firmware/firmware.bin` 是与当前源码对应的应用镜像（约 1.2 MB）。以下命令把 `COM3` 换成实际串口。

两条命令的公共前缀与参数：

| 片段 | 含义 |
| --- | --- |
| `pio pkg exec -p tool-esptoolpy --` | 借用 PlatformIO 内置的 esptool（实测 v4.11.0）。`esptool.py` 不在系统 PATH 中，直接调用会报「无法识别」，故经此转发；`--` 之后才是传给 esptool 的参数 |
| `--chip esp32s3` | 目标芯片型号，必须与实物一致 |
| `--port COM3` | 串口设备名，Windows 形如 `COM3`，Linux/macOS 形如 `/dev/ttyUSB0`、`/dev/cu.usbserial-*` |
| `--baud 921600` | 写入波特率，仅影响刷写速度；`write_flash` 之外的命令不必带 |
| `write_flash` | 子命令，其后参数**两两成组**：先是偏移，再是文件 |

**只更新应用**——板上已有可用的 bootloader 与分区表，例如日常升级：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 write_flash 0x10000 firmware/firmware.bin
```

**从空片全新烧录**——需要完整四件套，其中 `bootloader.bin` 与 `partitions.bin` 由 `pio run` 生成于 `.pio/build/esp32-s3-n16r8/`：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash `
  0x0     .pio/build/esp32-s3-n16r8/bootloader.bin `
  0x8000  .pio/build/esp32-s3-n16r8/partitions.bin `
  0xe000  "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin" `
  0x10000 firmware/firmware.bin
```

烧录失败或分区表错乱时，先整片擦除再重烧：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 erase_flash
```

上面四个偏移量的含义（取自 `board_build.partitions` 指定的 `default_16MB.csv`）：

| 偏移 | 写入的文件 | 用途 |
| --- | --- | --- |
| `0x0` | `bootloader.bin` | 二级引导，上电后最先执行 |
| `0x8000` | `partitions.bin` | 分区表，描述各分区的起止地址 |
| `0xe000` | `boot_app0.bin` | `otadata` 槽，记录 OTA 应从哪个 app 槽启动 |
| `0x10000` | `firmware.bin` | 应用镜像，写入 `app0` |

分区表中 `otadata` 的尺寸为 `0x2000`，与 `boot_app0.bin` 的 8192 字节一致。该分区表含 `app0`/`app1` 双槽，是 ArduinoOTA 能工作的前提；缺少 `otadata` 会让设备在 OTA 后无法引导。

`boot_app0.bin` 不由本工程生成，来自 Arduino 框架包。本机同时存在 `framework-arduinoespressif32`、`...@3.20016.0`、`...@3.20017.241212+sha.dcc1105b` 三个目录，上面的命令用的是无版本后缀那个。若该路径不存在，用下式取其实际位置后替换：

```powershell
Get-ChildItem "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32*" -Recurse -Filter boot_app0.bin | Select-Object -First 1 -ExpandProperty FullName
```

命令块中不能写 `#` 行内注释：PowerShell 的多行续行符 `` ` `` 必须是行尾最后一个字符，注释会截断续行导致命令被拆成多条。

> `firmware.bin` 是纯应用镜像，只能写入 `0x10000`。烧到 `0x0` 会覆盖 bootloader，设备将无法启动。

首次上电若设备进入 `FilamentChamber-Setup` 配置热点，说明烧录成功；配网后访问 `http://chamber.local/` 打开管理页，并可查看本机独立 OTA 密码继续无线升级。

## 重要硬件核对

Gerber 飞针网表确认本板上的 ST7796 为 SPI 连接：`RST=GPIO9`、`MISO=10`、`MOSI=11`、`SCLK=12`、`DC=13`、`CS=14`、`BL=21`；电阻触摸为 `YD=4`、`XR=5`、`YU=6`、`XL=7`。`TftUi` 已使用 TFT_eSPI 提供横屏主页面：耗材导航、三项自动控制、八格仪表及五个底部入口。电阻触摸坐标须在实机上校准后加入。

同样需要在首次上电前复核 `include/pins.h` 的 `ADC_NTC` 引脚。热端控制已默认由 `HEATER_ENABLED = false` 锁定；确认 GPIO40 的 `HOT_PWM`、NTC 引脚和 MOSFET 有效电平后，才改为 `true`。GPIO1 为蜂鸣器，GPIO40 为热端 PWM，GPIO39 为排气风扇 PWM。

仓温闭环采用 I²C 上的 AHT20。接线图显示加热模块有独立两芯 NTC，因此 `ADC_NTC` 已接入热板保护链，暂按 100 kΩ/B3950 模型换算。确认实际 NTC 型号与 PCB GPIO 前不可开启加热。

主板 INA226 已按原理图 R15=10 mΩ、I²C 0x40 和 1 mA/LSB 接入 `Ina226Sensor`；若实物地址不同，可在 `ina226.begin(Wire)` 中调整。它用于加热电流的软降功率，读取失败会在加热状态触发故障保护。

### 待验证项清单

| # | 项目 | 固件当前设定 | 依据位置 | 需要确认的动作 |
| --- | --- | --- | --- | --- |
| 1 | 加热总开关 | `HEATER_ENABLED = false` | `src/main.cpp` | 复核 GPIO40 PWM、NTC 引脚与 MOSFET 有效电平后改为 `true` |
| 2 | 热板 NTC 型号/分压 | 100 kΩ / 100 kΩ / B3950 | `readSensors()`、`readNtcCelsius()` | 万用表测常温阻值，按实物参数改写 |
| 3 | 热板 NTC 引脚 | `ADC_NTC = GPIO3` | `include/pins.h` | 对照原理图网络名与 PCB 焊盘 |
| 4 | INA226 地址与分流 | `0x40`、R15 = 10 mΩ、1 mA/LSB | `src/ina226_sensor.cpp` | 核对实物丝印与分流电阻，必要时调整地址 |
| 5 | 仓温传感器安装位 | AHT20（I²C） | `src/main.cpp` | 确认位于仓内且具代表性，避开热风直吹 |
| 6 | 电阻触摸 | `HAS_TOUCH_PANEL = false` | `include/pins.h` | 更换触摸屏后置 `true`，用「触摸屏校准」做两点采点 |
| 7 | 状态输出负载 | GPIO8 / GPIO47 独立 3.3 V | `include/pins.h` | 外接负载须经光耦或驱动器 |
| 8 | 排气风扇双路 | `AIR_FAN_DC`(38) + `AIR_FAN_PWM`(39) | `src/main.cpp` | 确认两路接线与风扇调速方式 |
| 9 | 显示与背光 | ST7796 SPI，480×320，BL=GPIO21 | `platformio.ini`、`include/pins.h` | 花屏/白屏时核对 TFT_eSPI 配置与 `setRotation(1)` |
| 10 | 限流闭环 | 电压/电流采样接入但未标定 | `src/controller.cpp` | 用钳形表或分流比对读数后启用完整电流闭环 |
| 11 | 主板风扇条件 | MCU > 60 ℃ 或 电流 > 0.05 A 等 | `src/controller.cpp` | 确认 GPIO48 驱动方式与风道方向 |
| 12 | 环境总电流 | 热板 + 风扇 + 灯带同开 | — | 核对电源额定功率与线径 |

## 常见问题

**Q：烧录后设备出现 `FilamentChamber-Setup` 热点，是失败了吗？**
不是。这表示固件已运行且没有保存过的 WiFi 凭据，已进入配网门户。连上该热点完成配网后，设备会连接路由器并开启 Web 管理页与 REST（80 端口）、MQTT、NTP 与 OTA。门户 180 秒无操作会自动关闭重试。

**Q：屏幕上排气风扇在转，但热板完全不加温。**
先确认 `HEATER_ENABLED` 是否为 `false`——出厂长按此值锁定加热，属预期行为。确认后还需保证 NTC、AHT20、INA226 三者读数有效，任一无效且处于预热/打印态超过 3 秒会进入 `Fault`。

**Q：某些温度/电流显示 `--`。**
对应传感器读数无效：AHT20 读数超出 −40–100 ℃、NTC 采样值触到 ADC 端点（`raw <= 0` 或 `>= 4095`）、INA226 读数超量程或读取失败，都会显示占位符，同时该传感器在故障页显示为红色。

**Q：浏览器打不开 Web 页面。**
服务在标准 **80** 端口，地址形如 `http://<设备IP>/api/state`（也可用 `http://chamber.local/`）；且 `WiFi联网` 为关时设备完全不初始化网络，只能通过串口或本地设置页重新开启。

**Q：远程把 `wifiEnabled` 改成 0 被拒绝。**
设计如此。该接口本身依赖网络，关闭等于自断通道，因此 `POST /api/settings` 传 `wifiEnabled=0` 返回 400，只能在串口或本地设置页操作。

**Q：OTA 端口列表里找不到设备。**
需 `OTA升级` 为开且设备已连上 WiFi。该项关闭后无法通过远程接口重新打开，只能回到本地设置页开启或重启设备。

**Q：时间不对或显示占位串。**
时钟优先级为 NTP → 手动校时值 → 占位串。NTP 关闭或断网且未手动校时就会显示 `----/--/-- --:--:--`；在系统设置里用「日期/时间」手动校准后会写入 NVS，重启仍有效。

**Q：改了设置，重启后却恢复原值。**
系统设置与耗材预设都需要**长按编码器保存**；未保存的修改不写入 NVS。设置页右上角出现 `*` 表示存在未保存改动。

**Q：屏幕变黑但设备仍在工作。**
这是屏幕休眠，仅关闭背光。到「屏幕休眠时间」把值调到 0 可禁用，或开启「打印时保持屏幕开启」让打印过程中不休眠。

**Q：中文显示成方块或缺字。**
字库只包含源码字符串里出现过的汉字。新增文案后需重跑 `python tools/genvlw.py` 重新生成 `include/font_cn16.h` 与 `include/font_cn26.h`。

**Q：多色打印中途被判定打印结束。**
属于 PIR 长时间无动作导致的误判，把「PIR关闭延时」调大（例如 90 秒）即可。

**Q：进入 `Fault` 后无法自动恢复。**
这是刻意的故障锁定，防止传感器偶发恢复后重新加热。需关闭再开启系统（或重启设备）才能重新进入状态机。

**Q：编译时提示找不到 `boot_app0.bin`。**
该文件来自 Arduino 框架包而非本工程。用「编译与烧录」章节给出的 `Get-ChildItem` 命令定位实际路径后替换即可。

**Q：恢复出厂后预留的注册码数据会丢吗？**
不会。恢复出厂只删除本固件拥有的设置键与耗材键，未知键一律保留。

**Q：固件版本显示 1.0.0，如何改成自己的版本号？**
版本号来自 `include/version.h` 的 `FW_VERSION`，也可用 build_flags 追加 `-DFW_VERSION=\"x.y.z\"` 覆盖，无需改源码。
