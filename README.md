# ESP32-S3 N16R8 温控主板

本工程按 `SCH_1-P1_2026-01-30-X3hS6itA.png` 创建，使用 Arduino 框架与 PlatformIO。控制逻辑参考 SmartChamber-3D 的公开功能描述：PIR 判断打印状态、PID 仓温控制、超温强排气、打印结束按耗材设定排气，以及耗材预设。

`src/controller.cpp` 包含 PLA、PETG、TPU、ABS、ASA、PC、PA、PVA、PET、PPA、PEBA 与 CUSTOM 的温度/安全阈值预设。它与硬件访问分开，屏幕或编码器 UI 可直接调用 `setProfile()`、`requestPreheat()` 和 `setHeatLimit()`。

控制状态为 `Idle → Preheat / Printing → Cooling`；温度采样连续失效 3 秒进入锁定 `Fault`，任何状态温度达到耗材安全阈值则停止加热并 100% 排气。`Fault` 不会自动复位，避免传感器偶发恢复后重新加热。

控制器现在以 50 ms 节拍工作，包含 `Idle → Detecting → Printing → Cooling` PIR 三态、分段排气、仓温/热板双 PID、热板硬过温、INA226 电流软降和 3%/周期 PWM 斜率限制。热板 NTC 与 INA226 的读取代码已接入，但 GPIO、NTC 参数和 INA226 地址仍须实物验证；`HEATER_ENABLED` 默认关闭，因此不会误启动加热。

Gerber 飞针网表已确认 `GPIO8`（打印中）和 `GPIO47`（加热中）为独立 3.3 V 状态输出，因此已启用。外接负载必须经光耦或驱动器。

## 主界面交互映射

`UiModel` 与所给主界面标注对应：耗材前后切换、自动排气、自动恒温、结束排气、系统工作状态、提前预热与灯光开关。未接上屏幕时可通过串口调试：`[`/`]` 切换耗材，`e` 排气自动控制，`t` 恒温自动控制，`p` 预热，`l` 灯光，`s` 系统启停，`x` 手动强排。

主界面支持无触摸 EC11 完整导航：旋转移动白色/橙色焦点，单击执行当前项，双击开启/关闭手动强排，长按开启/关闭系统。可聚焦上一耗材、当前耗材设置、下一耗材、三个自动开关，以及底部的系统启停、提前预热、灯光和系统设置。进入耗材设置页后，旋转修改选中值，单击依次选择最低/最高仓温、最低/最高自动排气风速、打印结束排气风速和排气时长，双击选择上一项，长按写入 NVS 并返回。温度上下限至少相差 5℃，自动风速上下限会互相约束，打印结束风速范围为 0–100%，排气时长范围为 0–1800 秒（0 表示关闭）。REST `POST /api/profile` 除 `index` 外也接受 `minC`、`maxC`、`fanMin`、`fanMax`、`postFan`、`postSeconds`，校验通过后立即持久化。

主界面底部第一格为 PIR 状态指示，其余四格均可通过 EC11 聚焦并执行。进入 23 项系统设置后由 EC11 旋转选择，单击切换或进入数值编辑，长按保存返回。系统设置包括中英文、按键音、亮度/休眠、打印保持亮屏、编码器方向、PIR 延时、灯光/蜂鸣联动、发热板限流/风扇 PWM/温度保护、屏幕配色（日间/夜间/自动）与日间/夜间开始时刻、日期与时间校准、恢复出厂和固件版本。当前硬件配置 `Pin::HAS_TOUCH_PANEL=false`，触摸轮询被完全停用，触摸校准项显示“不支持”；更换四线电阻触摸屏时可改为 `true` 后校准。恢复出厂只删除固件拥有的设置与耗材键，不清除未知键，因此预留的注册码数据会保留。

主界面时钟行显示完整的 `YYYY-MM-DD HH:MM:SS`，数据优先取自 NTP；断网且未同步时回退到上次手动校时值（`manualClockEpoch`），两者都无有效时间时显示占位串。系统设置的“日期/时间”两项同样直接改写系统时钟并写入 NVS，因此手动校时结果重启后仍然有效。

`SystemSettings` 已持久化语言、按键声音、屏幕亮度/休眠、打印保持亮屏、编码器方向、PIR 启动/关闭延时、自动灯光/蜂鸣、发热板最大电流、发热板风扇速度及保护温度，以及屏幕配色（`theme`）、日间/夜间开始时刻（`dayStart`/`nightStart`）和手动校时基准（`clockEp`）。参数范围会在载入/保存时限制在安全区间。触摸屏校准数据需在确定触摸控制器型号后另行存储。

主界面采用 480×320 三分布局：左侧控制区、右侧 2×4 仪表、底部 5 个入口。整帧先绘制到 PSRAM 中的两帧 16-bit Sprite，再一次推送到屏幕；显示刷新在独立低优先级任务中执行，主循环提交快照时只覆盖长度为 1 的队列，不会等待 SPI 刷屏。PSRAM 初始化失败时自动降级为直绘并在串口报告。`SystemSettings.language` 载入后直接驱动中英文文案，REST 设置接口也接受 `language=zh|en` 并立即切换。

字库由 `tools/genvlw.py` 从系统中文 TTF/TTC 抽取源码所需汉字和可打印 ASCII，生成 16 px/26 px 两套 TFT_eSPI VLW 头文件。脚本会自动搜索 Windows、macOS、Linux 常见字体，也可显式指定：

```bash
python3 -m pip install Pillow
python3 tools/genvlw.py --font /path/to/chinese-font.ttf --preview
```

## 系统设置说明

系统设置共 23 项，顺序与屏幕显示一致：

1. **系统语言**：切换系统语言，目前支持中文和英文。
2. **按键声音**：打开/关闭按键声音。
3. **屏幕亮度**：调节屏幕亮度。
4. **屏幕休眠时间**：设置屏幕多少秒后休眠，仅仅是屏幕关闭，不会影响系统运行。
5. **打印时保持屏幕开启**：3D 打印机工作时保持屏幕开启，不进行休眠。
6. **EC11 编码器方向**：选择 EC11 编码器的方向，如果你购买的是正向的，请选择“正向”，如果你购买的是反向的，请选择“反向”。
7. **PIR 启动延时**：通过 PIR 红外感应模块检测打印机是否运动，如果运动持续 n 秒，则开启系统内的仓温控制功能。
8. **PIR 关闭延时**：如果 PIR 红外感应模块 n 秒内没有检测到打印机运动，则认为打印机已经停止工作，停止系统内的仓温控制功能。（系统默认时间为 50 s，如果经常打印多色模型，由于换色时间过长，建议设置为 90 s。）
9. **启动后自动开灯**：当检测到 3D 打印机工作后，自动打开 LED 灯光系统。
10. **关闭后自动关灯**：当检测到 3D 打印机停止工作后，自动关闭 LED 灯光系统。
11. **启动后蜂鸣提示**：当检测到 3D 打印机工作后，蜂鸣提示。
12. **关闭后蜂鸣提示**：当检测到 3D 打印机停止工作后，蜂鸣提示。
13. **发热板限流**：设置发热板最大工作电流。
14. **发热板风扇风速**：设置发热板风扇的工作速度。
15. **发热板温度保护**：设置发热板最大温度，当温度超过后，暂停加热功能。
16. **屏幕配色**：切换屏幕配色，可选“日间”“夜间”或“自动”。选“自动”时按下面两个时刻自动切换。也可在预览页顶部快捷按钮直接切换。
17. **日间开始时刻**：到达该时刻切换到日间配色，步长 15 分钟。
18. **夜间开始时刻**：到达该时刻切换到夜间配色，步长 15 分钟。
19. **日期**：手动校准年月日三段，旋转改当前段，单击切到下一段，末段后再单击退出编辑。
20. **时间**：手动校准时分两段，操作方式同上。校时结果写入 RTC 与 NVS，重启后仍生效。
21. **触摸屏校准**：校准触摸屏。
22. **恢复出厂配置**：清除所有自定义的参数、数据。
23. **固件版本**：只读显示当前固件版本号（当前 1.0.0），不可修改。

界面布局与中英文、日/夜配色可参考 `tools/ui_preview.html`（浏览器直接打开）。该页是可操作的真机模拟器：真机每屏只显示 5 行，需旋转滚动才能看全 23 项，页内因此把 23 项在屏下逐条列出说明与取值范围，并随屏内光标实时高亮。顶部快捷按钮中的“夜间/日间/自动”等价于修改“屏幕配色”这一项。

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

首次上电若设备进入 `FilamentChamber-Setup` 配置热点，说明烧录成功；连上该热点配网后，可由 ArduinoOTA 继续无线升级（OTA 密码见 `startOta()`）。

## 重要硬件核对

Gerber 飞针网表确认本板上的 ST7796 为 SPI 连接：`RST=GPIO9`、`MISO=10`、`MOSI=11`、`SCLK=12`、`DC=13`、`CS=14`、`BL=21`；电阻触摸为 `YD=4`、`XR=5`、`YU=6`、`XL=7`。`TftUi` 已使用 TFT_eSPI 提供横屏主页面：耗材导航、三项自动控制、八格仪表及五个底部入口。电阻触摸坐标须在实机上校准后加入。

同样需要在首次上电前复核 `include/pins.h` 的两个 ADC 引脚。热端控制已默认由 `HEATER_ENABLED = false` 锁定；确认 GPIO39 的 `HOT_PWM`、NTC 引脚和 MOSFET 有效电平后，才改为 `true`。GPIO40 为蜂鸣器，GPIO39 为热端 PWM。

仓温闭环采用 I²C 上的 AHT20。接线图显示加热模块有独立两芯 NTC，因此 `ADC_NTC` 已接入热板保护链，暂按 100 kΩ/B3950 模型换算。确认实际 NTC 型号与 PCB GPIO 前不可开启加热。

主板 INA226 已按原理图 R15=10 mΩ、I²C 0x40 和 1 mA/LSB 接入 `Ina226Sensor`；若实物地址不同，可在 `ina226.begin(Wire)` 中调整。它用于加热电流的软降功率，读取失败会在加热状态触发故障保护。
