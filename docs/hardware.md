# 硬件接线与注意事项

主控板 ESP32-S3-WROOM-1 **N16R8**（8MB Flash + 16MB PSRAM），
SCH_1-P1 原理图（2026-01-30，嘉立创EDA V1.0）。引脚名全部集中在
[include/pins.h](../include/pins.h)，修改硬件版本时只需改这一个文件。

原理图：[hardware/mainboard-sch-p1.png](../hardware/mainboard-sch-p1.png)（2.4 MB）。

> **2026-09-22 校正说明**：早期版本引脚推断有误，对照最新原理图发现 6 处严重
> 错位（风扇 PWM、发热板 MOS、PIR、ADC 采样等全部串位）。下方为校正后的值，
> 上板烧录前务必以此为准。

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
`Pin::HAS_TOUCH_PANEL` 当前为 `false`（无触摸版本）。

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

`Pin::HAS_TOUCH_PANEL` 当前为 `false`：本机为**无触摸版本**，触摸轮询完全停用，
系统设置里的「触摸屏校准」显示「不支持」。更换为四线电阻触摸屏后改为 `true`，
并在实机上完成两点采点校准。

### 执行器

| 信号 | GPIO | 原理图网络名 | 说明 |
| --- | --- | --- | --- |
| `RGB` | 18 | WS2812B DIN | LED 灯带，板载 WS2811S MOS 驱动 |
| `BUZZER` | 1 | BUZ | 蜂鸣器 TF0405-1-4P，2.7 kHz |
| `AIR_FAN_PWM` | 2 | PWM | 排气风扇 4 线 PWM 调速（CN6） |
| `AIR_FAN_DC` | 35 | MOS_G | 排气风扇电源 MOS Q4 栅极 |
| `HOT_FAN` | 36 | MOS_G | 加热风扇 MOS Q5 栅极 |
| `LED_ENABLE` | 37 | MOS_G | LED 灯带 MOS Q6 栅极 |
| `HOT_PWM` | 38 | HOT_PWM | 发热板 MPT40N08S MOS 栅极 |
| `BOARD_FAN` | 45 | BOARD_FAN | 主板散热风扇 MOS Q7 栅极 |

### 传感器

| 信号 | GPIO | 原理图网络名 | 说明 |
| --- | --- | --- | --- |
| `PIR` | 3 | PIR | 红外感应模块 CN3 输出（SR 红外感应） |
| `ADC_NTC` | 39 | ADC_T | 发热板 NTC 热敏电阻（ZX-NTC1.25-P2ZZ）采样 |
| `ADC_VCC` | 40 | ADC_VCC | 电压分压采样（INA226 Vin+ 前置） |
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

## 2026-09-22 校正的 6 处错位

对照原理图发现以下旧值全部打错脚，现已修正：

| 常量 | 旧值 | 新值 | 旧值问题 |
| --- | --- | --- | --- |
| `PIR` | 35 | **3** | GPIO35 是风扇 MOS 栅极（数字输出），接成输入读 PIR 必失败 |
| `AIR_FAN_PWM` | 39 | **2** | 风扇 PWM 打在 NTC 采样脚，风扇不转 + ADC 被干扰 |
| `AIR_FAN_DC` | 38 | **35** | 风扇电源 MOS 栅极 |
| `HOT_PWM` | 40 | **38** | 发热板 PWM 打在电压分压脚，加热不工作 + INA226 被干扰 |
| `ADC_NTC` | 3 | **39** | NTC 温度采样读 PIR 输出脚，温度必然错 |
| `BOARD_FAN` | 48 | **45** | GPIO48 是模组板载 LED，GPIO45 才是主板散热风扇 MOS |

新增 `ADC_VCC = 40`（电压分压 / INA226 Vin+ 采样）。

## 上电前必须复核

以下各项在实物确认前不能启用加热。

### 1. `HEATER_ENABLED` 默认关闭

[src/main.cpp](../src/main.cpp) 中
`constexpr bool HEATER_ENABLED = false;`。此状态下 PID 会算出占空比但
**不输出 PWM**，加热不会误启动。

确认 `GPIO38` 的 `HOT_PWM`、`GPIO39` 的 NTC 引脚与 MOSFET 有效电平均正确后，
才可改为 `true`。

### 2. 热板 NTC 参数

加热模块有独立两芯 NTC，接 `ADC_NTC`（GPIO39）。当前按
**100 kΩ / B3950** 预设换算：

```cpp
heaterBoardTemp = readNtcCelsius(Pin::ADC_NTC, 100000.0f, 100000.0f, 3950.0f);
```

通用 NTC 默认模型为 10 kΩ / B3950，两者不可混用。确认实际型号后再改。

### 3. INA226 地址与分流电阻

主板按原理图 R15 = 10 mΩ、I²C 地址 0x40 接入，量程 1 mA/LSB（
[src/ina226_sensor.cpp](../src/ina226_sensor.cpp)
中 `SHUNT_OHMS = 0.01f`）。实物地址不同则改 `ina226.begin(Wire)`。

INA226 用于加热电流软降功率；读取失败会在加热状态下触发故障保护。

### 4. 仓温传感器 AHT20

AHT20 位于仓内，作为自动仓温闭环的输入。上电串口会打印
`AHT20: detected / not detected`，未检测到时仓温项显示为无效。

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
