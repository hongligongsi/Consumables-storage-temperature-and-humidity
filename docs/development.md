# 开发、测试与工具链

本页面向**改代码的人**：一次改动可以在本地被哪些环节自动验证、哪些环节仍必须上机，
以及仓库里的几个脚本各自管什么。使用层面的说明见 [README](../README.md)，硬件与联网
细节见 [hardware.md](hardware.md) / [networking.md](networking.md)。

## 环境与构建目标

`platformio.ini` 里有两个环境，职责完全不重叠：

| 用途 | 环境 | 入口 | 依赖 |
| --- | --- | --- | --- |
| 固件构建 / 烧录 | `esp32-s3-n16r8`（`espressif32@6.7.0`，Arduino core 2.x） | `pio run` | xtensa 工具链（PlatformIO 自动装） |
| 主机端单元测试 | `native` | `pio test -e native` | 系统自带 `g++` / `clang++` |
| 仓库脚本 | —（纯 Python） | `python tools/<脚本>.py` | Python 3.8+，仅标准库 |

`[platformio] default_envs` 只列了固件环境，所以 `pio run` 不会连带去"构建"主机测试
环境——测试环境没有可执行产物，被 `pio run` 扫到只会白报一条失败。

主机测试环境有两个**必须**保留的构建宏（写在 `[env:native]` 的 `build_flags`）：

- `-std=gnu++11`：GCC 5.x 的默认标准是 `gnu++98`，而 `pure_logic.h` 用了
  `constexpr` / `nullptr`，不给这个参数会在编译期直接失败。
- `-DUNITY_SUPPORT_64`：Unity 默认关闭 64 位断言，而芯片 ID 的混洗与双射检查都在
  `uint64_t` 上做。不开这一项，`TEST_ASSERT_EQUAL_UINT64` 会**判定失败**（不是编译
  错误，容易误判为逻辑 bug）。

`test_build_src = no` 表示只编译 `test/` 下的用例，不把 `src/` 拉进来，因此主机端测试
不需要设备、不需要串口，毫秒级出结果。

> 固件侧的编译约束（平台锁版本、`USE_FSPI_PORT`、PSRAM 变体）另见
> [build-and-flash.md](build-and-flash.md) 与 `platformio.ini` 内的就地注释。

## 代码分层：纯逻辑与硬件绑定

`include/pure_logic.h` 是一个**不依赖 Arduino / ESP-IDF 的纯函数集合**，收录标准只有
一条：

> 输入到输出完全确定，不读全局状态、不访问硬件、不做 I/O。

符合这条的逻辑放进来，就能被 `pio test -e native` 在桌面端原样复现；需要
`millis()` / NVS / GPIO 的留在各自的 `.cpp` 里。目前收录三组：

| 逻辑 | 函数 | 固件侧的调用点 |
| --- | --- | --- |
| 芯片唯一 ID | `mixChipMac` / `formatChipId` | `NetworkManager::chipId()`（`src/network.cpp`） |
| 界面主题解析 | `effectiveTheme` / `inDayWindow` | `resolveTheme()`（`src/main.cpp`） |
| 日夜时刻钳制 | `clampMinutesOfDay` | `sanitize()`（`src/settings.cpp`） |

把这几段抽出来的直接收益是**同一份常量只有一处定义**：时刻上限 `kMinutesOfDayMax`
（23:45）原先在 `settings.cpp` 与 `main.cpp` 各写一遍，现在共同引用 `pure_logic.h`，
不会再出现"改了一处、另一处跑偏"。

维护约定：**改动 `pure_logic.h` 必须同步改 `test/test_pure_logic/` 并跑通主机端测试**，
否则 CI 会红。这几段逻辑此前全靠手工烧录回归，是审计里点名的风险面。

## 主机端单元测试

```powershell
pio test -e native                  # Linux / macOS，或 PATH 里已有 g++
python tools/run_host_tests.py      # Windows：自动补上 PlatformIO 自带的 MinGW
python tools/run_host_tests.py -f "*theme*"   # 只跑名字匹配的用例
```

`tools/run_host_tests.py` 存在的理由：PlatformIO 的 `native` 平台不自带编译器，它用操作
系统里的 GCC，而 Windows 默认没有。该脚本把 PlatformIO 包目录里已有的 MinGW 临时挂到
`PATH`（同一目录下的 `libstdc++-6.dll` 等运行时 DLL 也一并解决），用完即散，不改系统
环境、不写注册表。Linux / macOS 与 CI 上系统自带 `g++`，脚本只做一层转发。

用例位于 `test/test_pure_logic/test_pure_logic.cpp`，覆盖范围：

| 分组 | 用例 |
| --- | --- |
| 芯片 ID | 固定向量（掩码/格式一旦变会静默改掉 OTA 密码，必须显式确认）、高位掩码、低 16 位全枚举双射 + 可逆、`CH-` 前缀与 12 位大写十六进制形状、缓冲不足时"全有或全无" |
| 主题解析 | 固定配色直通、无有效时钟退夜间、日间窗口 `[day, night)` 左闭右开、跨零点取补集、上下界相等时无日间 |
| 时刻钳制 | 0 / 23:45 / 越界回钳到 23:45，不包成 0 点 |

测试**同时钉住边界值和结构性质**（双射、首尾闭合），这样将来重写实现也能被拦住，而不
只是比对当下的输出。

## 持续集成

`.github/workflows/ci.yml` 在 `push` / `pull_request` / 手动触发时跑三个 job：

| Job | 做什么 |
| --- | --- |
| `firmware` 固件编译 | `pio run` 编过即可，并回显镜像体积；缓存 `~/.platformio`，缓存键带 `platformio.ini` 的哈希，改平台版本会自动重建 |
| `host-tests` 主机端单测 | `pio test -e native`，用 ubuntu 预装的 `g++` |
| `docs` 文档与台账一致性 | `python -m compileall -q tools` + `python tools/doc_check.py`；`checkout` 设 `fetch-depth: 0`，否则浅克隆查不到台账记录的历史提交 |

同一分支上的旧运行会被 `concurrency` 掐掉，省额度也省等待。

## 工具脚本

| 脚本 | 用途 |
| --- | --- |
| `tools/publish_firmware.py` | 把 `.pio` 构建产物发布成 `firmware/firmware.bin`，并写下构建台账 |
| `tools/doc_check.py` | 校验文档与代码/产物是否一致，CI 用它拦住文档漂移 |
| `tools/serial_regression.py` | 读串口周期上报并按区间断言，把"手工盯日志"换成一条命令 |
| `tools/run_host_tests.py` | Windows 上跑主机端单测，自动补编译器路径 |
| `tools/genvlw.py` | 从系统 TTF/TTC 抽取汉字生成 `font_cn16.h` / `font_cn26.h`（用法见 [settings.md](settings.md#新增设置项的改法)） |

### 固件发布与台账

仓库里的 `firmware/firmware.bin` 是**某一次构建的快照**，不是"永远等于当前源码"。
不要手工 `cp`——手工步骤会忘，忘了也看不出来。正确姿势是两步：

```powershell
pio run                                  # 先构建
python tools/publish_firmware.py         # 再发布（写入镜像 + .sha256 + BUILD.txt）
python tools/publish_firmware.py --check # 只校验现状，不写文件；不一致返回码非 0
```

`firmware/BUILD.txt` 是自动生成的台账，记录环境、版本、平台、源码提交、**工作区是否
干净**、构建时间、体积与 SHA-256。只看 `src/`、`include/`、`platformio.ini` 的改动判定
"dirty"——改 README 或设计稿不会让镜像显得脏。若发布时工作区不干净，台账会如实标注
`source_dirty = true` 并列出文件；想让镜像对应一个确定的提交，先提交再重新发布。

### 文档一致性校验

```powershell
python tools/doc_check.py            # 有 ERROR 时返回码 1
python tools/doc_check.py --strict   # WARN 也当 ERROR
```

校验四类，只依赖标准库与 git：

1. README 文件树里声明的**行数 / 字库体积** vs 实际文件（`（194 行）`、`（约 336 KB）`
   这类写法会被逐条比对；体积按整数 KB 精确比对，不容差）
2. **设置项数**三方一致：`SystemSettingField` 枚举的 `Count`、README 内联表、
   `docs/settings.md` 表，以及各处"N 项"字样
3. **固件台账自洽**：体积与 SHA-256 在 `BUILD.txt` / `firmware.bin.sha256` / 文件本体
   三方一致
4. **镜像新鲜度**：源码在台账记录的提交之后是否又改过（仅告警，不阻塞；无 git 环境
   时跳过）

这类数字没有守护机制就会悄悄跑偏——README 曾标 `ui_model.h` 194 行、实际 230 行，
`font_cn26.h` 约 805 KB、实际 793 KB，都是当初写对后来失守的。

### 串口回归

```powershell
python tools/serial_regression.py --port COM4 --seconds 30
python tools/serial_regression.py --port COM4 --seconds 30 --require-sensors
python tools/serial_regression.py --log serial.txt          # 复核别人抓好的日志，不接设备
python tools/serial_regression.py --port COM4 --seconds 20 --no-boot-check
```

断言内容：出现启动标记、周期上报条数达标（按 2 s/条留 25% 余量）、仓温落在
`-20 ~ 120 ℃`、湿度落在 `0 ~ 100 %`、无 `FAULT`。加 `--require-sensors` 还要求 AHT20 与
INA226 均在线且读数非 `nan`——**未接传感器的板子会失败，属预期**，所以它不进 CI，只在
上机时手动跑。

> ⚠️ 抓不到日志时先看这一条：`platformio.ini` 的 `-DARDUINO_USB_CDC_ON_BOOT=1` 让
> Arduino `Serial` 走 S3 **原生 USB-CDC（板载 USB 口）**；FTDI 接的 UART0 上只剩
> ESP-ROM 与 ESP-IDF 日志。所以在 FTDI 的 COM4 上看不到 `chamber=` 是正常的，不代表
> 设备卡死——要抓应用日志请接板载 USB 口，或临时注释该宏后重烧（**抓完务必恢复并重烧**）。

依赖 `pyserial`：`pip install pyserial`。

## 一次改动的推荐流程

1. 动到纯逻辑（芯片 ID / 主题 / 时刻）→ 改 `pure_logic.h` **并同步改测试**，跑
   `python tools/run_host_tests.py`
2. 动到 UI 渲染 → 先改 `tools/ui_preview.html` 预览，再同步 `src/tft_ui.cpp`；改视觉
   记得核对 `src/main.cpp` 的触摸热区
3. 动到文案或增删设置项 → 跑 `python tools/doc_check.py`，按提示修正文档里的数字与表格
4. `pio run` 编译通过
5. 提交后 `pio run && python tools/publish_firmware.py` 刷新固件台账
6. 上机：`python tools/serial_regression.py --port <口> --seconds 30`

## 仍需人工验证的部分

自动化到不了的地方，改动时请显式说明已上机验证：

- **加热链路**：预热 / 恒温 / 过温保护整条主链，需 `HEATER_ENABLED=true` 且 NTC 与
  INA226 已标定（见 [hardware.md](hardware.md#上电前必须复核)）
- **传感器接线**：AHT20 / INA226 的 I²C 供电与上拉，靠串口 `detected` 与回归脚本判定
- **屏上视觉**：三档主题的实际观感、字号与配色可读性
- **触摸校准**：本机为无触摸版本，换屏后需在实机两点采点
