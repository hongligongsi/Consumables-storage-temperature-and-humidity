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

### 生成合并镜像（对外分发）

对外发布/量产时，若想让对方"只烧一次"，可用 `esptool.py merge_bin` 把引导器、分区表与应用
拼成一个 `merged.bin`（约 1.2 MB），发送单个文件即可完成全新烧录：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py `
  --chip esp32s3 merge_bin `
  --flash_mode qio --flash_freq 80m --flash_size 16MB `
  --target-offset 0x0 `
  -o firmware/firmware-merged.bin `
  0x0     .pio/build/esp32-s3-n16r8/bootloader.bin `
  0x8000  .pio/build/esp32-s3-n16r8/partitions.bin `
  0xe000  "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin" `
  0x10000 firmware/firmware.bin
```

合并镜像的烧录只有一个偏移 `0x0`：

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash 0x0 firmware/firmware-merged.bin
```

> **两种分发形态**：日常 OTA 升级只需单个 `firmware.bin`（网页/ArduinoOTA 在线写应用槽）；
> 而"拆分 3 个文件（bootloader/partitions/app）"或"合并 1 个 merged.bin"都用于空片或量产，
> 烧录路径等价。合并镜像最大的好处是 `boot_app0.bin` 也一并打进，不依赖框架路径。

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
配网后，可由 ArduinoOTA 继续无线升级（OTA 主机名 `chamber`，每台设备的独立
密码可在 `http://chamber.local/` 查看，详见 [networking.md](networking.md)）。

## 串口写入中途中断（PermissionError 13）

典型现象——镜像写到九成左右突然断掉：

```
Writing at 0x0013354a... (93 %)
A serial exception error occurred: Cannot configure port, something went wrong.
Original message: PermissionError(13, '拒绝访问。', None, 5)
```

**这不是 esptool 或固件的问题，是 Windows 串口句柄在传输途中失效了。**
`write_flash` 每写一段都要重新配置串口，一旦句柄被夺走或设备从总线掉下来，
`SetCommState` 就返回拒绝访问。注意此时 **app0 已被写入大半、内容不完整**，
设备上电校验不过会回滚到 `app1`；若 `app1` 为空则反复重启，必须重新烧录一次。

按下面顺序排查，多数情况第 1、2 条就能解决：

1. **关掉所有占用串口的程序**：`pio device monitor`、VSCode 串口监视器、串口助手、
   PuTTY，以及上一次残留的 esptool。不确定就拔插一次 USB 后立刻烧录。
2. **降低写入波特率**。`platformio.ini` 已设 `upload_speed = 460800`；用原始
   esptool 命令时把 `--baud 921600` 改成 `--baud 460800`。仍失败就改 `115200`
   复测——若 115200 能一次写完，说明瓶颈在 USB 链路而不是驱动。
3. **换线、换口、去掉集线器**：用短一点的数据线直插主机后置 USB 口，避开 USB hub
   和延长线。ESP32-S3-DevKitC-1 有两个 USB-C 口，**优先用标着 `UART` 的那个
   （板载 CP2102/CH340 桥）**，比标着 `USB` 的原生 USB-CDC 口稳定得多。
   先用 `pio device list` 看清端口对应的芯片型号再动手。
4. **关掉 Windows 的 USB 节能**：设备管理器 → 端口 → 对应 COM 口 → 属性 →
   电源管理 → 取消勾选「允许计算机关闭此设备以节约电源」；电源选项 → 高级设置 →
   USB 设置 → USB 选择性暂停 → 已禁用。该项会在长时间传输中途挂起设备，正是
   「写到一半拒绝访问」的常见成因。
5. **手动进下载模式**：按住 `BOOT` → 点一下 `RST` → 松开 `BOOT`，让芯片稳定停在
   下载模式，再执行烧录，避开自动复位的时序竞争。必要时给 esptool 加
   `--before no_reset`。
6. **暂时停掉杀毒/安全软件**，或把 esptool 进程与串口加入白名单。
7. 以上都无效时，先 `erase_flash` 整片擦除再重烧；或改用无线 OTA 绕过 USB：
   设备若能正常启动，打开 `http://chamber.local/` 直接上传 `firmware.bin`。

### 判断设备是掉线还是被占用

- 烧录中 COM 号**跳变**（如 COM5 → COM7），或设备管理器里端口闪一下消失再出现
  → 是 USB 掉线/重枚举，走第 3、4 条。
- COM 号不变、只是报拒绝访问 → 多半是别的进程占着，走第 1 条。

