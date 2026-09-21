# 编译烧录与预编译固件

## 环境要求

- PlatformIO（CLI 或 VSCode 插件）
- 平台锁版本 `espressif32@6.7.0`

> **为什么锁版本**：`platformio.ini` 里写的是 `espressif32@6.7.0` 而不是 `^6.7.0`。
> 项目热控 PWM 仍使用 `ledcSetup()` / `ledcAttachPin()` 旧 API，而 Arduino core 3.x
> 已移除这套接口。`^6.7.0` 会解析到 6.13.0（core 3.x）导致编译失败。升级平台前
> 必须先迁移 ledc 调用。

## 常用命令

```powershell
pio run                    # 仅编译
pio run -t upload          # 编译并烧录
pio device monitor -b 115200
```

## 烧录预编译固件

仓库中的 `firmware/firmware.bin` 是与当前源码对应的应用镜像（约 1.2 MB）。
下面把 `COM3` 换成实际串口。

### 参数说明

| 片段 | 含义 |
| --- | --- |
| `pio pkg exec -p tool-esptoolpy --` | 借用 PlatformIO 内置的 esptool（实测 v4.11.0）。`esptool.py` 不在系统 PATH 中，直接调用会报「无法识别」，故经此转发；`--` 之后才是传给 esptool 的参数 |
| `--chip esp32s3` | 目标芯片型号，必须与实物一致 |
| `--port COM3` | 串口设备名。Windows 形如 `COM3`，Linux/macOS 形如 `/dev/ttyUSB0`、`/dev/cu.usbserial-*` |
| `--baud 921600` | 写入波特率，仅影响刷写速度；`write_flash` 之外的命令不必带 |
| `write_flash` | 子命令，其后参数**两两成组**：先是偏移，再是文件 |

### 只更新应用

板上已有可用的 bootloader 与分区表，例如日常升级：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 write_flash 0x10000 firmware/firmware.bin
```

### 从空片全新烧录

需要完整四件套，其中 `bootloader.bin` 与 `partitions.bin` 由 `pio run` 生成于
`.pio/build/esp32-s3-n16r8/`：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash `
  0x0     .pio/build/esp32-s3-n16r8/bootloader.bin `
  0x8000  .pio/build/esp32-s3-n16r8/partitions.bin `
  0xe000  "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin" `
  0x10000 firmware/firmware.bin
```

> 命令块中不能写 `#` 行内注释：PowerShell 的多行续行符 `` ` `` 必须是行尾最后一个
> 字符，注释会截断续行导致命令被拆成多条。

### 擦除

烧录失败或分区表错乱时，先整片擦除再重烧：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 erase_flash
```

## 分区表偏移含义

取自 `board_build.partitions` 指定的 `default_16MB.csv`：

| 偏移 | 写入的文件 | 用途 |
| --- | --- | --- |
| `0x0` | `bootloader.bin` | 二级引导，上电后最先执行 |
| `0x8000` | `partitions.bin` | 分区表，描述各分区的起止地址 |
| `0xe000` | `boot_app0.bin` | `otadata` 槽，记录 OTA 应从哪个 app 槽启动 |
| `0x10000` | `firmware.bin` | 应用镜像，写入 `app0` |

分区表中 `otadata` 的尺寸为 `0x2000`，与 `boot_app0.bin` 的 8192 字节一致。
该分区表含 `app0`/`app1` 双槽，是 ArduinoOTA 能工作的前提；**缺少 `otadata`
会让设备在 OTA 后无法引导**。

> `firmware.bin` 是纯应用镜像，只能写入 `0x10000`。烧到 `0x0` 会覆盖 bootloader，
> 设备将无法启动。

## `boot_app0.bin` 的路径

`boot_app0.bin` 不由本工程生成，来自 Arduino 框架包。本机可能同时存在
`framework-arduinoespressif32`、`...@3.20016.0`、`...@3.20017.241212+sha.dcc1105b`
等多个目录，上面的命令用的是无版本后缀那个。若该路径不存在，用下式取其实际位置后替换：

```powershell
Get-ChildItem "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32*" -Recurse -Filter boot_app0.bin | Select-Object -First 1 -ExpandProperty FullName
```

## 烧录后确认

首次上电若设备进入 `FilamentChamber-Setup` 配置热点，说明烧录成功。连上该热点
配网后，可由 ArduinoOTA 继续无线升级（OTA 主机名 `filament-chamber`，密码见
[networking.md](networking.md)）。
