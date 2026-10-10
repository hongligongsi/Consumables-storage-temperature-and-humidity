# 硬件接线与注意事项

主控板 ESP32-S3-WROOM-1 **N16R8**（16MB Flash + 8MB PSRAM），
SCH_1-P1 原理图（2026-01-30，嘉立创EDA V1.0）。引脚名全部集中在
[include/pins.h](../include/pins.h)，修改硬件版本时只需改这一个文件。

原理图：[hardware/sch-p1-2026-01-30.png](../hardware/sch-p1-2026-01-30.png)
（2026-01-30 高清版）；整机接线：[hardware/wiring-diagram.png](../hardware/wiring-diagram.png)；
主界面 UI 功能标注：[docs/images/ui-main-desc.png](images/ui-main-desc.png)。

> **2026-09-29 复核（重要）**：2026-09-22 那次「校正」把引脚改错了——它是按
> **ESP32 classic** 的脚位习惯改的（classic 上 GPIO39/40 是 ADC），但本板是
> **ESP32-S3**：ADC 只有 GPIO1~10（ADC1）与 GPIO11~20（ADC2，与 WiFi 冲突禁用），
> GPIO35~40 是纯数字 IO。实机日志每 500ms 报
> `[E][esp32-hal-adc.c:158] __analogReadRaw(): Pin 39 is not ADC pin!`、温度显示
> nan，实锤 ADC_NTC=39 非法。本次对照 2026-01-30 原理图网络名逐一回滚，
> 下表为**当前生效值**（与原理图一致）。

## 引脚映射

### 显示（ST7796，SPI，480×320 横屏）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `TFT_RESET` | 9 | 复位 |
| `TFT_SPI_MISO` | 10 | 主入从出 |
| `TFT_SPI_MOSI` | 11 | 主出从入 |
| `TFT_SPI_SCLK` | 12 | 时钟 |
| `TFT_DATA_CMD` | 13 | 数据/命令选择（A0） |
| `TFT_CHIP_SELECT` | 14 | 片选 |
| `TFT_BACKLIGHT` | 21 | 背光 + LED 使能 |

SPI 频率 27 MHz。驱动在 `platformio.ini` 的 `build_flags` 中以
`-DST7796_DRIVER`、`-DTFT_WIDTH=320`、`-DTFT_HEIGHT=480` 等宏配置。

屏幕排线为 FPC-05FB-40PH20（40 针 0.5 mm），原理图标注「不带触摸/带触摸」两种版本，
`Pin::HAS_TOUCH_PANEL` 当前为 `true`（TFT 排针已引出 X+/X−/Y+/Y− 四线，具备触摸能力）。

### 编码器与触摸

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `ENCODER_KEY` | 15 | EC11 中键（EC_D） |
| `ENCODER_B` | 16 | EC11 B 相（EC_B） |
| `ENCODER_A` | 17 | EC11 A 相（EC_A） |
| `TFT_YD` | 4 | 电阻触摸 Y−（预留） |
| `TFT_XR` | 5 | 电阻触摸 X+（预留） |
| `TFT_YU` | 6 | 电阻触摸 Y+（预留） |
| `TFT_XL` | 7 | 电阻触摸 X−（预留） |

`Pin::HAS_TOUCH_PANEL` 是**编译期硬件能力闸**（当前为 `true`）：只有当硬件根本没有
触摸走线时才置 `false`，此时触摸采样完全不编译。运行时是否真的启用触摸另由系统设置
「触摸模式」（`touchMode`，三态）决定：

| 模式 | 行为 |
| --- | --- |
| 自动（出厂默认） | 开机做电阻膜在位探测，探到膜才启用轮询与校准 |
| 开 | 强制启用，用于探测不准或特殊排线（用户自负误触风险） |
| 关 | 强制关闭，只用编码器 |

改完保存即时生效、无需重启。判定合并在 `main.cpp` 的 `touchEnabledNow()`（编译期闸 ×
模式 × 探测结果），轮询与校准入口都只问它，系统设置的「触摸屏校准」项也据此显示
「已关闭 / 不支持 / 未校准 / 已校准」。

「自动」档的**电阻膜在位探测**（`detectTouchPanel()`，把每层膜一端拉低、另一端弱上拉
读电平，膜在则读数脚被层电阻拉低）针对的是：固件开了触摸但实装非触摸屏、或排线未接。
探测不到膜时触摸轮询与校准入口整路关闭，浮空触摸线上的随机 ADC 采样不会再被误判成
触点（幽灵触摸）。探测结果见开机串口日志 `Touch panel: detected / NOT detected`。

### 执行器

| 信号 | GPIO | 原理图网络名 | 说明 |
| --- | --- | --- | --- |
| `RGB` | 18 | WS2812B DIN | LED 灯带，板载 WS2811S MOS 驱动 |
| `BUZZER` | 1 | BUZ | 蜂鸣器 TF0405-1-4P，2.7 kHz |
| `AIR_FAN_PWM` | 39 | AIR_FAN_PWM | 排气风扇 4 线 PWM 调速（CN6） |
| `AIR_FAN_DC` | 38 | AIR_FAN_DC | 排气风扇电源 MOS Q4 栅极 |
| `HOT_FAN` | 36 | HOT_FAN | 加热风扇 MOS Q5 栅极 |
| `LED_ENABLE` | 37 | LED | LED 灯带 MOS Q6 栅极 |
| `HOT_PWM` | 40 | HOT_PWM | 发热板 MPT40N08S MOS 栅极 |
| `BOARD_FAN` | 45 | BOARD_FAN | 主板散热风扇 MOS Q7 栅极（strapping，见下） |

### 传感器

| 信号 | GPIO | 原理图网络名 | 说明 |
| --- | --- | --- | --- |
| `PIR` | 35 | PIR | 红外感应模块输出（HC-SR501） |
| `ADC_NTC` | 3 | ADC_NTC | 发热板 NTC 热敏电阻（ZX-NTC1.25-P2ZZ）采样，ADC1_CH2 |
| `ADC_VCC` | 2 | IO2 | 24 V 分压采样（R16 10k + R18 1k，量程 36.3 V），ADC1_CH1；固件暂未用，电压走 INA226 |
| `I2C_SCL` | 41 | SCL | I²C 总线（AHT20 + INA226） |
| `I2C_SDA` | 42 | SDA | I²C 总线（AHT20 + INA226） |

### 状态输出

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `PRINTING_STATUS` | 8 | 打印中 3.3 V 状态输出 |
| `HEATING_STATUS` | 47 | 加热中 3.3 V 状态输出 |

Gerber 飞针网表已确认这两路为独立 3.3 V 输出，故
`HAS_STATUS_OUTPUTS = true`。

**外接负载必须经光耦或驱动器，不可由 GPIO 直接驱动。**

### USB 与 Strapping

| GPIO | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| 0 | Strapping | 弱上拉 | BOOT 按钮，按低时进入下载模式 |
| 3 | Strapping | 浮空 | 默认不参与启动（需烧 `STRAP_JTAG_SEL` eFuse 才生效），现接 PIR 输出 |
| 19 | USB | — | USB D−，板载 USB-SERIAL-JTAG bridge 专用，别作 GPIO |
| 20 | USB | — | USB D+，同上 |
| 45 | Strapping | 弱下拉 | VDD_SPI 电压选择：低 = 3.3 V，高 = 1.8 V |
| 46 | Strapping | 弱下拉 | 启动模式（与 GPIO0 配合）+ ROM messages 打印控制 |
| 48 | 板载 | — | ESP32-S3 模组板载 WS2812 RGB LED，LGA-33 封装没对外引出 |

USB-UART 走内置 USB-SERIAL-JTAG bridge（UART0），无需占用 GPIO43/44。

### 封装限制（LGA-33 N16R8 未引出）

以下 GPIO 在 ESP32-S3 内核 die 上存在，但 LGA-33 封装没引出到 PCB：

- **GPIO33 / GPIO34** — ROM / RTC 功能
- **GPIO43 / GPIO44** — UART0 默认 RX / TX（已由 USB-SERIAL-JTAG 取代）

## 引脚校正史（2026-09-22 → 2026-09-29）

### 2026-09-22：一次误判（已回滚）

9-22 那次「校正」是**按 ESP32 classic 的脚位习惯改的**：classic 的 ADC1 确实挂在
GPIO36/37/38/39/40（GPIO39 = ADC1_CH3、GPIO40 = ADC1_CH0），所以那次改动看起来
「把 ADC 挪到 39/40 很合理」。

但**本板是 ESP32-S3**，ADC 分布完全不同：

| 芯片 | ADC1 | ADC2 | GPIO35~40 |
| --- | --- | --- | --- |
| ESP32 classic | GPIO32~39 | GPIO0/2/4/12~15/25~27 | ADC / 输入专用 |
| **ESP32-S3（本板）** | **GPIO1~10** | GPIO11~20（与 WiFi 冲突，禁用） | **纯数字 IO，无 ADC** |

所以在 S3 上 `ADC_NTC = 39` 会让 `analogRead()` 直接报
`Pin 39 is not ADC pin!`，温度恒为 nan —— 这就是 9-29 复查的触发点。

### 2026-09-29：对照 2026-01-30 原理图回滚

下表给出 9-22 误改后、9-29 回滚后的完整对照（网络名取自 2026-01-30 原理图）：

| 常量 | 原始值 | 9-22 误改 | **9-29 生效值** | 原理图网络名 |
| --- | --- | --- | --- | --- |
| `PIR` | 3 | 35 | **35** | PIR |
| `AIR_FAN_PWM` | 2 | 39 | **39** | AIR_FAN_PWM |
| `AIR_FAN_DC` | 35 | 38 | **38** | AIR_FAN_DC |
| `HOT_PWM` | 38 | 40 | **40** | HOT_PWM |
| `ADC_NTC` | 39 | 3 | **3** | ADC_NTC |
| `ADC_VCC` | 40 | 40 | **2** | IO2 |
| `BOARD_FAN` | 45 | 45 | **45** | BOARD_FAN |
| `LED_ENABLE` | 37 | 37 | **37** | LED |
| `HOT_FAN` | 36 | 36 | **36** | HOT_FAN |

要点：9-22 的「新值」和 9-29 的「生效值」其实是同一批数字，只是**方向不同**。
原始值（`PIR=35`、`AIR_FAN_DC=38`、`HOT_PWM=40`）本来就是对的，
9-22 把它们改到了 ADC 脚位上；9-29 又原样改回来。**结论：以原理图网络名为准，
不要按「哪个脚是 ADC」去猜。**

`ADC_VCC` 是 9-22 新增的常量，原始值写成 40，9-29 按原理图网络名 `IO2` 改为 **2**。

## 上电前必须复核

以下各项在实物确认前不能启用加热。

### 1. `HEATER_ENABLED` 默认关闭

[src/main.cpp](../src/main.cpp) 中
`constexpr bool HEATER_ENABLED = false;`。此状态下 PID 会算出占空比但
**不输出 PWM**，加热不会误启动。

确认 `GPIO40` 的 `HOT_PWM`、`GPIO3` 的 NTC 引脚与 MOSFET 有效电平均正确后，
才可改为 `true`。

### 2. 热板 NTC 参数

加热模块有独立两芯 NTC，接 `ADC_NTC`（**GPIO3**，ADC1_CH2）。NTC 型号为
**ZX-NTC1.25-P2ZZ**，原理图上分压上臂电阻 **R14 = 10 kΩ**，故 `seriesOhm` 应取
10 kΩ。当前预设换算（已按原理图校正）：

```cpp
heaterBoardTemp = readNtcCelsius(Pin::ADC_NTC, 10000.0f, 10000.0f, 3950.0f);
```

`readNtcCelsius(pin, seriesOhm, nominalOhm, beta)` 的公式为
`R_ntc = seriesOhm × raw / (4095 − raw)`，其中 `seriesOhm` 是**与 NTC 串联的那只
固定电阻**（即 R14），`nominalOhm` 是 NTC 在 25 ℃ 的标称阻值。此前误按
**100 kΩ / B3950** 预设（`seriesOhm = nominalOhm = 100000.0f`），与 R14 = 10 kΩ
不符，会把温度算错。若实物 NTC 阻值/β 与预设不同，请以万用表实测 25 ℃ 阻值为准再改。

### 3. INA226 地址与分流电阻

主板按原理图 R15 = 10 mΩ、I²C 地址 0x40 接入，量程 1 mA/LSB（
[src/ina226_sensor.cpp](../src/ina226_sensor.cpp)
中 `SHUNT_OHMS = 0.01f`）。实物地址不同则改 `ina226.begin(Wire)`。

INA226 用于加热电流软降功率；读取失败会在加热状态下触发故障保护。

### 4. 仓温传感器 AHT20

AHT20 位于仓内，作为自动仓温闭环的输入。上电串口会打印
`AHT20: detected / not detected`，未检测到时仓温项显示为无效；若在预热或打印中
连续 3 秒无效，会锁定 `Fault` 并在故障页显示 F-01（`AhtLost`）。

### 5. GPIO45（BOARD_FAN）上电风险

**GPIO45 是 ESP32-S3 的 strapping 脚**，上电时选择 VDD_SPI 电压：

| GPIO45 上电电平 | VDD_SPI |
| --- | --- |
| **低（默认，内部弱下拉）** | **3.3 V** |
| 高 | 1.8 V |

本板用模组内置 flash，走默认 3.3 V，因此 **GPIO45 在上电瞬间必须为低**。
它接 `BOARD_FAN` MOS Q7 栅极，栅极是高阻输入，正常不会影响电平。

风险在于 MOS 栅极电阻分压或栅源电容残留把 GPIO45 拉高：一旦上电被读成高，
芯片会按 1.8 V 配置 VDD_SPI，**与 3.3 V 的 flash 不匹配，直接启动失败**。

**首次上板烧录前，建议确认 Q7 栅极在上电瞬间没有把 GPIO45 拉高**（示波器看
GPIO45 上升沿，或临时断开 BOARD_FAN 那路）。确认能正常启动后再接回去。
注意**不要**为了"保险"给 GPIO45 加上拉 —— 那恰好会触发 1.8 V 配置。

### 6. Flash 分区表

`board_build.partitions = default_16MB.csv`，含 `app0` / `app1` 双槽，这是
ArduinoOTA 能工作的前提。详见 [build-and-flash.md](build-and-flash.md)。
