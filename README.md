# ESP32-S3 N16R8 温控主板

本工程按 `SCH_1-P1_2026-01-30-X3hS6itA.png` 创建，使用 Arduino 框架与 PlatformIO。控制逻辑参考 SmartChamber-3D 的公开功能描述：PIR 判断打印状态、PID 仓温控制、超温强排气、打印结束按耗材设定排气，以及耗材预设。

`src/controller.cpp` 包含 PLA、PETG、TPU、ABS、ASA、PC、PA、PVA、PET、PPA、PEBA 与 CUSTOM 的温度/安全阈值预设。它与硬件访问分开，屏幕或编码器 UI 可直接调用 `setProfile()`、`requestPreheat()` 和 `setHeatLimit()`。

控制状态为 `Idle → Preheat / Printing → Cooling`；温度采样连续失效 3 秒进入锁定 `Fault`，任何状态温度达到耗材安全阈值则停止加热并 100% 排气。`Fault` 不会自动复位，避免传感器偶发恢复后重新加热。

控制器现在以 50 ms 节拍工作，包含 `Idle → Detecting → Printing → Cooling` PIR 三态、分段排气、仓温/热板双 PID、热板硬过温、INA226 电流软降和 3%/周期 PWM 斜率限制。热板 NTC 与 INA226 的读取代码已接入，但 GPIO、NTC 参数和 INA226 地址仍须实物验证；`HEATER_ENABLED` 默认关闭，因此不会误启动加热。

Gerber 飞针网表已确认 `GPIO8`（打印中）和 `GPIO47`（加热中）为独立 3.3 V 状态输出，因此已启用。外接负载必须经光耦或驱动器。

## 主界面交互映射

`UiModel` 与所给主界面标注对应：耗材前后切换、自动排气、自动恒温、结束排气、系统工作状态、提前预热与灯光开关。未接上屏幕时可通过串口调试：`[`/`]` 切换耗材，`e` 排气自动控制，`t` 恒温自动控制，`p` 预热，`l` 灯光，`s` 系统启停，`x` 手动强排。

主界面支持无触摸 EC11 完整导航：旋转移动白色/橙色焦点，单击执行当前项，双击开启/关闭手动强排，长按开启/关闭系统。可聚焦上一耗材、当前耗材设置、下一耗材、三个自动开关，以及底部的系统启停、提前预热、灯光和系统设置。进入耗材设置页后，旋转修改选中值，单击依次选择最低/最高仓温、最低/最高自动排气风速、打印结束排气风速和排气时长，双击选择上一项，长按写入 NVS 并返回。温度上下限至少相差 5℃，自动风速上下限会互相约束，打印结束风速范围为 0–100%，排气时长范围为 0–1800 秒（0 表示关闭）。REST `POST /api/profile` 除 `index` 外也接受 `minC`、`maxC`、`fanMin`、`fanMax`、`postFan`、`postSeconds`，校验通过后立即持久化。

主界面底部第一格为 PIR 状态指示，其余四格均可通过 EC11 聚焦并执行。进入 17 项系统设置后由 EC11 旋转选择，单击切换或进入数值编辑，长按保存返回。系统设置包括中英文、按键音、亮度/休眠、打印保持亮屏、编码器方向、PIR 延时、灯光/蜂鸣联动、发热板限流/风扇 PWM/温度保护和恢复出厂。当前硬件配置 `Pin::HAS_TOUCH_PANEL=false`，触摸轮询被完全停用，触摸校准项显示“不支持”；更换四线电阻触摸屏时可改为 `true` 后校准。恢复出厂只删除固件拥有的设置与耗材键，不清除未知键，因此预留的注册码数据会保留。

`SystemSettings` 已持久化语言、按键声音、屏幕亮度/休眠、打印保持亮屏、编码器方向、PIR 启动/关闭延时、自动灯光/蜂鸣、发热板最大电流、发热板风扇速度及保护温度。参数范围会在载入/保存时限制在安全区间。触摸屏校准数据需在确定触摸控制器型号后另行存储。

主界面采用 480×320 三分布局：左侧控制区、右侧 2×4 仪表、底部 5 个入口。整帧先绘制到 PSRAM 中的两帧 16-bit Sprite，再一次推送到屏幕；显示刷新在独立低优先级任务中执行，主循环提交快照时只覆盖长度为 1 的队列，不会等待 SPI 刷屏。PSRAM 初始化失败时自动降级为直绘并在串口报告。`SystemSettings.language` 载入后直接驱动中英文文案，REST 设置接口也接受 `language=zh|en` 并立即切换。

字库由 `tools/genvlw.py` 从系统中文 TTF/TTC 抽取源码所需汉字和可打印 ASCII，生成 16 px/26 px 两套 TFT_eSPI VLW 头文件。脚本会自动搜索 Windows、macOS、Linux 常见字体，也可显式指定：

```bash
python3 -m pip install Pillow
python3 tools/genvlw.py --font /path/to/chinese-font.ttf --preview
```

## 编译与烧录

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

### 烧录预编译固件

仓库中的 `firmware/firmware.bin` 是与当前源码对应的应用镜像（约 1.2 MB）。`esptool.py` 未加入系统 PATH，需经 PlatformIO 调用。以下命令把 `COM3` 换成实际串口。

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

偏移量取自 `board_build.partitions` 指定的 `default_16MB.csv`：bootloader 在 `0x0`、分区表在 `0x8000`、`otadata` 在 `0xe000`、`app0` 在 `0x10000`。该分区表含 `app0`/`app1` 双槽，是 ArduinoOTA 能工作的前提；`otadata` 处的 `boot_app0.bin` 负责指示从哪个槽启动，缺它会导致 OTA 后无法引导。

> `firmware.bin` 是纯应用镜像，只能写入 `0x10000`。烧到 `0x0` 会覆盖 bootloader，设备将无法启动。

首次上电若设备进入 `FilamentChamber-Setup` 配置热点，说明烧录成功；连上该热点配网后，可由 ArduinoOTA 继续无线升级（OTA 密码见 `startOta()`）。

## 重要硬件核对

Gerber 飞针网表确认本板上的 ST7796 为 SPI 连接：`RST=GPIO9`、`MISO=10`、`MOSI=11`、`SCLK=12`、`DC=13`、`CS=14`、`BL=21`；电阻触摸为 `YD=4`、`XR=5`、`YU=6`、`XL=7`。`TftUi` 已使用 TFT_eSPI 提供横屏主页面：耗材导航、三项自动控制、八格仪表及五个底部入口。电阻触摸坐标须在实机上校准后加入。

同样需要在首次上电前复核 `include/pins.h` 的两个 ADC 引脚。热端控制已默认由 `HEATER_ENABLED = false` 锁定；确认 GPIO39 的 `HOT_PWM`、NTC 引脚和 MOSFET 有效电平后，才改为 `true`。GPIO40 为蜂鸣器，GPIO39 为热端 PWM。

仓温闭环采用 I²C 上的 AHT20。接线图显示加热模块有独立两芯 NTC，因此 `ADC_NTC` 已接入热板保护链，暂按 100 kΩ/B3950 模型换算。确认实际 NTC 型号与 PCB GPIO 前不可开启加热。

主板 INA226 已按原理图 R15=10 mΩ、I²C 0x40 和 1 mA/LSB 接入 `Ina226Sensor`；若实物地址不同，可在 `ina226.begin(Wire)` 中调整。它用于加热电流的软降功率，读取失败会在加热状态触发故障保护。
