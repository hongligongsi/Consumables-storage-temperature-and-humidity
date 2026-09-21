# 硬件接线与注意事项

引脚名与 `SCH_1-P1_2026-01-30-X3hS6itA.png` 原理图中的网络名对应，全部集中在
[include/pins.h](../include/pins.h)。修改硬件版本时只需改这一个文件。

## 引脚映射

### 显示（ST7796，SPI）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `TFT_RESET` | 9 | 复位 |
| `TFT_SPI_MISO` | 10 | 主入从出 |
| `TFT_SPI_MOSI` | 11 | 主出从入 |
| `TFT_SPI_SCLK` | 12 | 时钟 |
| `TFT_DATA_CMD` | 13 | 数据/命令选择 |
| `TFT_CHIP_SELECT` | 14 | 片选 |
| `TFT_BACKLIGHT` | 21 | 背光 |

屏幕为 480×320 横屏（`setRotation(1)`），驱动在 `platformio.ini` 的 `build_flags`
中以 `-DST7796_DRIVER`、`-DTFT_WIDTH=320`、`-DTFT_HEIGHT=480` 等宏配置，
SPI 频率 27 MHz。

### 编码器与触摸

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `ENCODER_KEY` | 15 | EC11 按键 |
| `ENCODER_B` | 16 | EC11 B 相 |
| `ENCODER_A` | 17 | EC11 A 相 |
| `TFT_YD` | 4 | 电阻触摸 Y− |
| `TFT_XR` | 5 | 电阻触摸 X+ |
| `TFT_YU` | 6 | 电阻触摸 Y+ |
| `TFT_XL` | 7 | 电阻触摸 X− |

`Pin::HAS_TOUCH_PANEL` 当前为 `false`：本机为**无触摸版本**，触摸轮询完全停用，
系统设置里的「触摸屏校准」显示「不支持」。更换为四线电阻触摸屏后改为 `true`，
并在实机上完成两点采点校准。

### 执行器与传感器

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| `RGB` | 18 | 状态指示灯（NeoPixel） |
| `BUZZER` | 1 | 蜂鸣器 |
| `I2C_SDA` | 42 | AHT20 + INA226 |
| `I2C_SCL` | 41 | AHT20 + INA226 |
| `HOT_PWM` | 40 | 热端 PWM |
| `AIR_FAN_PWM` | 39 | 排气风扇 PWM |
| `AIR_FAN_DC` | 38 | 排气风扇方向 |
| `LED_ENABLE` | 37 | 照明 |
| `HOT_FAN` | 36 | 发热板风扇 |
| `PIR` | 35 | 人体红外，用于判断打印机是否工作 |
| `BOARD_FAN` | 48 | 主板风扇 |
| `ADC_NTC` | 3 | 热板 NTC |

### 状态输出

Gerber 飞针网表已确认 `GPIO8`（打印中）与 `GPIO47`（加热中）是独立 3.3 V 状态输出，
故 `HAS_STATUS_OUTPUTS = true`。

**外接负载必须经光耦或驱动器，不可由 GPIO 直接驱动。**

## 上电前必须复核

以下各项在实物确认前不能启用加热。

### 1. `HEATER_ENABLED` 默认关闭

[src/main.cpp](../src/main.cpp#L22) 中 `constexpr bool HEATER_ENABLED = false;`。
此状态下 PID 会算出占空比但**不输出 PWM**，加热不会误启动。

确认 `GPIO40` 的 `HOT_PWM`、NTC 引脚与 MOSFET 有效电平均正确后，才可改为 `true`。

> 注意 `HOT_PWM` 与 `AIR_FAN_PWM` 的引脚定义：`include/pins.h` 中
> `HOT_PWM = 40`、`AIR_FAN_PWM = 39`。

### 2. 热板 NTC 参数

加热模块有独立两芯 NTC，接 `ADC_NTC`。当前按 **100 kΩ / B3950** 预设换算：

```cpp
heaterBoardTemp = readNtcCelsius(Pin::ADC_NTC, 100000.0f, 100000.0f, 3950.0f);
```

通用 NTC 默认模型为 10 kΩ / B3950，两者不可混用。确认实际型号后再改。

### 3. INA226 地址与分流电阻

主板按原理图 R15 = 10 mΩ、I²C 地址 0x40 接入，量程 1 mA/LSB
（[src/ina226_sensor.cpp](../src/ina226_sensor.cpp) 中 `SHUNT_OHMS = 0.01f`）。
实物地址不同则改 `ina226.begin(Wire)`。

INA226 用于加热电流软降功率；读取失败会在加热状态下触发故障保护。

### 4. 仓温传感器 AHT20

AHT20 位于仓内，作为自动仓温闭环的输入。上电串口会打印
`AHT20: detected / not detected`，未检测到时仓温项显示为无效。

## 分区表与 Flash

`board_build.partitions = default_16MB.csv`，含 `app0` / `app1` 双槽，
这是 ArduinoOTA 能工作的前提。详见 [build-and-flash.md](build-and-flash.md)。
