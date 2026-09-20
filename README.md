# ESP32-S3 N16R8 温控主板

Consumables storage temperature and humidity（耗材仓储温湿度）指耗材（如试剂、打印耗材、医疗用品等）在存放时所需的环境温度与湿度条件。

本工程按 `SCH_1-P1_2026-01-30-X3hS6itA.png` 创建，使用 Arduino 框架与 PlatformIO。控制逻辑参考 SmartChamber-3D 的公开功能描述：PIR 判断打印状态、PID 仓温控制、超温/打印结束强排气，以及耗材预设。

`src/controller.cpp` 包含 PLA、PETG、TPU、ABS、ASA、PC、PA、PVA、PET、PPA、PEBA 与 CUSTOM 的温度/安全阈值预设。它与硬件访问分开，屏幕或编码器 UI 可直接调用 `setProfile()`、`requestPreheat()` 和 `setHeatLimit()`。

控制状态为 `Idle → Preheat / Printing → Cooling`；温度采样连续失效 3 秒进入锁定 `Fault`，任何状态温度达到耗材安全阈值则停止加热并 100% 排气。`Fault` 不会自动复位，避免传感器偶发恢复后重新加热。

控制器现在以 50 ms 节拍工作，包含 `Idle → Detecting → Printing → Cooling` PIR 三态、分段排气、仓温/热板双 PID、热板硬过温、INA226 电流软降和 3%/周期 PWM 斜率限制。热板 NTC 与 INA226 的读取代码已接入，但 GPIO、NTC 参数和 INA226 地址仍须实物验证；`HEATER_ENABLED` 默认关闭，因此不会误启动加热。

Gerber 飞针网表已确认 `GPIO8`（打印中）和 `GPIO47`（加热中）为独立 3.3 V 状态输出，因此已启用。外接负载必须经光耦或驱动器。

## 主界面交互映射

`UiModel` 与所给主界面标注对应：耗材前后切换、自动排气、自动恒温、结束排气、系统工作状态、提前预热与灯光开关。未接上屏幕时可通过串口调试：`[`/`]` 切换耗材，`e` 排气自动控制，`t` 恒温自动控制，`p` 预热，`l` 灯光，`s` 系统启停，`x` 手动强排。

`SystemSettings` 已持久化语言、按键声音、屏幕亮度/休眠、打印保持亮屏、编码器方向、PIR 启动/关闭延时、自动灯光/蜂鸣、发热板最大电流、发热板风扇速度及保护温度。参数范围会在载入/保存时限制在安全区间。触摸屏校准数据需在确定触摸控制器型号后另行存储。

## 编译与烧录

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

### 直接烧录预编译固件

`firmware/firmware.bin` 是随仓库提供的应用镜像，对应提交 `a4140db`（RAM 16.1%、Flash 14.4%）。

`esptool.py` 未加入系统 PATH，需通过 PlatformIO 调用，或直接改用 `pio run -t upload`。以下命令把 `COM3` 换成实际串口。

**只更新应用**（板上已有可用的 bootloader 与分区表，例如日常升级）：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 write_flash 0x10000 firmware/firmware.bin
```

**从空片全新烧录**（需要完整四件套；`bootloader.bin` 与 `partitions.bin` 由 `pio run` 生成于 `.pio/build/esp32-s3-n16r8/`）：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash `
  0x0     .pio/build/esp32-s3-n16r8/bootloader.bin `
  0x8000  .pio/build/esp32-s3-n16r8/partitions.bin `
  0xe000  "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32@3.20016.0\tools\partitions\boot_app0.bin" `
  0x10000 firmware/firmware.bin
```

偏移量取自 `default_16MB.csv` 分区表：`bootloader` 在 `0x0`、分区表在 `0x8000`、`otadata` 在 `0xe000`、`app0` 在 `0x10000`。首次上电若设备进入配置热点，说明烧录成功。

> 注意：`firmware.bin` 是纯应用镜像，**不能**单独烧到 `0x0`，否则设备无法启动。

## 重要硬件核对

Gerber 飞针网表确认本板上的 ST7796 为 SPI 连接：`RST=GPIO9`、`MISO=10`、`MOSI=11`、`SCLK=12`、`DC=13`、`CS=14`、`BL=21`；电阻触摸为 `YD=4`、`XR=5`、`YU=6`、`XL=7`。`TftUi` 已使用 TFT_eSPI 提供横屏主页面：耗材导航、三项自动控制、八格仪表及五个底部入口。电阻触摸坐标须在实机上校准后加入。

同样需要在首次上电前复核 `include/pins.h` 的两个 ADC 引脚。热端控制已默认由 `HEATER_ENABLED = false` 锁定；确认 GPIO39 的 `HOT_PWM`、NTC 引脚和 MOSFET 有效电平后，才改为 `true`。GPIO40 为蜂鸣器，GPIO39 为热端 PWM。

仓温闭环采用 I²C 上的 AHT20。接线图显示加热模块有独立两芯 NTC，因此 `ADC_NTC` 已接入热板保护链，暂按 100 kΩ/B3950 模型换算。确认实际 NTC 型号与 PCB GPIO 前不可开启加热。

主板 INA226 已按原理图 R15=10 mΩ、I²C 0x40 和 1 mA/LSB 接入 `Ina226Sensor`；若实物地址不同，可在 `ina226.begin(Wire)` 中调整。它用于加热电流的软降功率，读取失败会在加热状态触发故障保护。
