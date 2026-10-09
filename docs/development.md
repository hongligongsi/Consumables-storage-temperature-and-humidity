# 开发、测试与工具链

本页面向**改代码的人**：一次改动能在本地被哪些环节自动验证、哪些环节仍必须上机，以及
仓库里每个脚本各自管什么、怎么读它的输出。

- 产品与使用说明见 [README](../README.md)
- 硬件接线与上电前复核见 [hardware.md](hardware.md)
- 联网实现见 [networking.md](networking.md)
- 温控算法本身见 [control.md](control.md)

读完应当能回答三个问题：改了一行代码，**跑什么**能最快发现问题？**哪些环节 CI 会替我
跑**？**哪些只能上机**？

## 目录

- [1. 环境与构建目标](#1-环境与构建目标)
- [2. 代码分层：纯逻辑与硬件绑定](#2-代码分层纯逻辑与硬件绑定)
- [3. 主机端单元测试](#3-主机端单元测试)
- [4. 持续集成](#4-持续集成)
- [5. 工具脚本详解](#5-工具脚本详解)
- [6. 按场景的改动流程](#6-按场景的改动流程)
- [7. 排障速查](#7-排障速查)
- [8. 仍需人工验证的部分](#8-仍需人工验证的部分)

---

## 1. 环境与构建目标

### 1.1 两个 PlatformIO 环境

`platformio.ini` 里定义了两个环境，职责完全不重叠：

| 环境 | 用途 | 入口 | 编译器来源 |
| --- | --- | --- | --- |
| `esp32-s3-n16r8` | 固件构建 / 烧录 | `pio run` | xtensa-esp32s3（PlatformIO 首次构建时自动下载） |
| `native` | 主机端纯逻辑单元测试 | `pio test -e native` | 操作系统自带的 `g++` / `clang++` |

仓库脚本（`tools/*.py`）不属于任何 PlatformIO 环境，用系统 Python 直接运行，只依赖标准
库；唯一例外是 `serial_regression.py` 需要 `pyserial`。

### 1.2 固件环境的关键取舍

这些参数不是随手写的，**改动前请先读 `platformio.ini` 里的就地注释**：

| 配置 | 取值 | 为什么不能随便动 |
| --- | --- | --- |
| `platform` | `espressif32@6.7.0`（锁定，非 `^`） | 热控 PWM 仍用 `ledcSetup()` / `ledcAttachPin()` 旧 API；`^6.7.0` 会解析到 core 3.x，该 API 已被移除，编译直接失败 |
| `upload_speed` | `460800` | 921600 在部分 USB 线 / 集线器 / 原生 USB-CDC 口上会在写到 9x% 时掉线（`PermissionError(13)`），降速可显著降低中断概率 |
| `board_build.arduino.memory_type` | `qio_qspi` | 本板 8MB OPI PSRAM 硬件不可用；换成 `qio_opi` 会让 WiFi/LWIP 往 PSRAM 分配缓冲而挂死总线 |
| `-DUSE_FSPI_PORT` | 必给 | 缺失时 TFT_eSPI 把 SPI 寄存器基址算成 0，`tft.init()` 触发 StoreProhibited 崩溃 |
| `-DARDUINO_USB_CDC_ON_BOOT=1` | 必给（见 [§5.3](#53-serial_regressionpy--串口回归)） | 让 `Serial` 走 S3 原生 USB-CDC；它决定应用日志出现在哪个串口 |
| `board_build.partitions` | `default_16MB.csv` | 含 `app0`/`app1` 双槽 + `otadata`，是 ArduinoOTA 能工作的前提 |

### 1.3 `[env:native]` 为什么需要额外三个设置

这三个都是"删了会出错、且不一定以编译错误的形式暴露"：

| 设置 | 缺失时的表现 |
| --- | --- |
| `-std=gnu++11` | GCC 5.x 默认标准是 `gnu++98`，而 `pure_logic.h` 用了 `constexpr` / `nullptr`，编译期直接失败 |
| `-DUNITY_SUPPORT_64` | Unity 默认关闭 64 位断言，芯片 ID 的双射检查（在 `uint64_t` 上做）会**判定失败**——这是运行时失败而非编译错误，极易误判成逻辑 bug |
| `test_build_src = no` | 会把 `src/` 一并拉进来编译，进而引入 Arduino / ESP-IDF 依赖，主机端就跑不起来 |

这也解释了主机端测试为什么是**毫秒级**：它只编译 `test/` 下的用例加 `include/pure_logic.h`，
不碰任何 ESP32 依赖。

### 1.4 为什么 `default_envs` 只列固件环境

```ini
[platformio]
default_envs = esp32-s3-n16r8
```

没有这一行，`pio run` 会连 `native` 环境一起"构建"——而测试环境不产出可执行文件，
`pio run` 只会白报一条失败。列上之后，`pio run` 的语义就是纯粹的"编固件"。

### 1.5 新机器从零搭建

1. 安装 [PlatformIO Core](https://platformio.org/install)（CLI 或 VSCode 插件均可）
2. 首次 `pio run` 会自动下载 `espressif32@6.7.0` 平台与 xtensa 工具链（数百 MB，耐心等；
   CI 里用 `~/.platformio` 缓存省掉这一步）
3. 要跑主机端测试：
   - Linux / macOS：装 `g++`（`xcode-select --install` 或 `apt-get install g++`）
   - Windows：**无需手动装 MinGW**，`python tools/run_host_tests.py` 会借用 PlatformIO
     包目录里的那一份
4. （可选）上机回归：`pip install pyserial`

## 2. 代码分层：纯逻辑与硬件绑定

### 2.1 收录标准

`include/pure_logic.h` 只收录满足这一条的逻辑，别无其它门槛：

> 输入到输出完全确定，不读全局状态、不访问硬件、不做 I/O。

- **能进**：数学换算、字符串格式化、区间/边界判定、纯状态解析
- **不能进**：需要 `millis()` / `delay()` / NVS / GPIO / `Serial` / `WiFi` 的逻辑——它们
  留在各自模块的 `.cpp` 里

判据很简单：**如果一段逻辑换成任意输入都能在桌面端算出确定结果，它就该进
`pure_logic.h`**，因为进去就能被单测覆盖。全部符号放在 `namespace pure` 下，与固件侧
符号隔离。

### 2.2 当前收录的四组

#### 芯片唯一 ID

```cpp
constexpr uint64_t kChipIdMask = 0x9E3779B97F4AULL; // 落在 48 位内
constexpr size_t   kChipIdLength = 15;              // "CH-" + 12 位十六进制

uint64_t mixChipMac(uint64_t efuseMac);                      // 掩码 + 全宽异或
size_t   formatChipId(uint64_t mac, char *out, size_t size); // → "CH-XXXXXXXXXXXX"
```

- 取值来自 eFuse 内的 48 位 MAC，做**低 48 位与固定掩码的全宽异或**
- 异或在 48 位空间上是**双射**：不同芯片必得不同 ID，且无法由 ID 反推 MAC（纯混淆，非
  密码学保护）
- 该 ID 同时是屏上「芯片ID」条目、Web 管理页的「本机 OTA 密码」、`ArduinoOTA` 接入密码
- **边界语义**：缓冲不足或参数非法时写入空串并返回 `0`，**绝不部分写入**——半个 ID 比
  没有 ID 更容易误判

固件侧调用点：`NetworkManager::chipId()`（`src/network.cpp`）。

> 改动掩码或格式会**静默改掉所有设备的 OTA 密码**。因此测试里专门钉了固定向量
> （`CH-9E3779B97F4A` / `CH-61C8864680B5`），一旦变化立刻变红，逼你显式确认这是有意的。

#### 界面主题解析

```cpp
constexpr uint8_t kThemeAuto      = 2; // 存储值：蓝白，按时刻自动切换
constexpr uint8_t kThemeBlueDay   = 2; // 生效值：蓝白·日间
constexpr uint8_t kThemeBlueNight = 3; // 生效值：蓝白·夜间

bool    inDayWindow(uint16_t minutes, uint16_t dayStart, uint16_t nightStart);
uint8_t effectiveTheme(uint8_t rawTheme, bool clockValid, uint16_t minutes,
                       uint16_t dayStart, uint16_t nightStart);
```

存储值只有 0/1/2 三档，但**生效值**多出一档「3 = 蓝白·夜间」：

| 存储值 | 含义 | 生效值 |
| --- | --- | --- |
| 0 | 默认（iOS 浅色） | 0（不分昼夜） |
| 1 | IOS（深色 HMI） | 1（不分昼夜） |
| 2 | 蓝白 | 落在日间区间 → 2；否则 → 3 |

判定规则：

- 日间区间是 `[dayStart, nightStart)`，**左闭右开**
- `dayStart > nightStart` 时区间**跨零点**，取补集
- `dayStart == nightStart` 视为"无日间"，恒返回 false，不产生未定义区间
- **时钟无效时按夜间渲染**：夜间配色是深底，误判代价远小于日间浅底在夜里刺眼——这是
  刻意选定的失败方向

固件侧调用点：`resolveTheme()`（`src/main.cpp`），它只负责把"现在几点"喂给这个纯函数。

#### 日夜时刻钳制

```cpp
constexpr uint16_t kMinutesOfDayMax = 1425; // 23:45，与 15 分钟步长对齐
uint16_t clampMinutesOfDay(uint16_t minutes);
```

上限取 **23:45（1425 分钟）而非 23:59**：设置页步长是 15 分钟，若上限取 23:59，末次递增
会被夹到 23:59，从而破坏步长。`clampMinutesOfDay` 同时被 `settings.cpp` 的 `sanitize()`
用于载入/保存时二次夹取——即使 NVS 被写坏也不会越界。

#### WiFi 信号格数

```cpp
constexpr int kRssiExcellent = -55, kRssiGood = -65, kRssiFair = -75;
constexpr int kRssiUnknown   =  0;   // 哨兵：0 dBm 实际不会出现
uint8_t rssiBars(int rssi);          // → 0..4
```

屏上主屏状态文字右侧那 4 格信号条的数据源。阈值与**内置 Web 管理页**（`web_page.h` 的
`setBars`）以及浏览器模拟器 `tools/ui_preview.html` 是同一套——三处任一处改都要同步，否则
屏上与网页会对同一个网络给出不同结论（故障码编号就吃过"同一规则写三遍"的亏，见 §7）。

`0` 是"无信号"哨兵而非合法读数，正值也一样按 0 格处理：调用方（`NetworkManager::rssiDbm()`）
未联网时直接返回哨兵，避免画出满格误导。测试里除了钉边界（`-55` 算 4 格、`-56` 算 3 格），
还钉了**单调性**：信号越强格数只能不减——将来若有人把阈值写反，固定向量看不出来，这条会红。

### 2.3 抽出来之后得到了什么

- **同一份常量只有一处定义**：`kMinutesOfDayMax` 原先在 `settings.cpp` 与 `main.cpp` 各
  写一遍（字面量 `1425`），现在共同引用 `pure_logic.h`，不会"改了一处、另一处跑偏"
- **从"只能烧录后肉眼验"变成"桌面端毫秒级验"**：芯片 ID 决定 OTA 密码、主题解析决定整屏
  配色，此前改错只能靠烧录发现
- **命名空间隔离**：全部在 `namespace pure` 下

### 2.4 维护约定（重要）

> **改动 `pure_logic.h` 必须同步改 `test/test_pure_logic/` 并跑通主机端测试**，否则 CI 红。

这条不是形式要求：这几段逻辑此前零自动化回归，是项目审计里点名的风险面。

## 3. 主机端单元测试

### 3.1 三种运行方式

```powershell
pio test -e native                          # 通用（Linux/macOS，或 PATH 里已有 g++）
python tools/run_host_tests.py              # Windows 推荐：自动补 MinGW 路径
python tools/run_host_tests.py -f "*theme*" # 只跑名字匹配的用例（-f 透传给 Unity）
```

`tools/run_host_tests.py` 的存在理由：PlatformIO 的 `native` 平台**不自带编译器**，它用
操作系统里的 GCC，而 Windows 默认没有。该脚本在 `~/.platformio/packages/` 下找一份
PlatformIO 自带的 MinGW（`toolchain-gccmingw32`），把它临时挂到 `PATH` 上——同一目录下
的运行时 DLL（`libstdc++-6.dll`、`libgcc_s_dw2-1.dll`）也就一并解决了——然后转发给
`pio test -e native`。**用完即散，不改系统环境、不写注册表。**

查找顺序：① `PATH` 上已有 `g++`/`clang++` → 直接用，不掺和；② PlatformIO 包目录里的
MinGW；③ 都没有 → 打印分平台的安装指引后退出。Linux / macOS 与 CI 上系统自带 `g++`，
脚本只做一层转发，行为与直接 `pio test -e native` 完全一致。

### 3.2 用例清单

用例在 `test/test_pure_logic/test_pure_logic.cpp`，共 **14 个**。

**芯片 ID（5 个）**

| 用例 | 钉住什么 |
| --- | --- |
| `test_chip_id_known_vectors` | 固定向量：`formatChipId(0)` = `CH-9E3779B97F4A`、`formatChipId(0xFFFFFFFFFFFF)` = `CH-61C8864680B5` |
| `test_chip_id_masks_upper_bits` | 高 48 位以外的位必须被掩掉；MAC 高位怎么变，ID 都不变 |
| `test_chip_id_is_bijective_on_sample` | 低 16 位**全枚举**：不同 MAC 不碰撞，且异或自反（洗两次还原出原 MAC） |
| `test_chip_id_shape` | 长度恒 15、前缀 `CH-`、后 12 位必须是大写十六进制 |
| `test_chip_id_rejects_short_buffer` | 缓冲不足 / 空指针 / 零长 → 返回 0 且写入空串，不截断、不越界 |

**主题解析（5 个）**

| 用例 | 钉住什么 |
| --- | --- |
| `test_theme_fixed_schemes_pass_through` | 0/1 两档原样返回，时钟是否有效都不影响 |
| `test_theme_auto_without_clock_falls_back_to_night` | 自动档 + 时钟无效 → 夜间（刻意的失败方向） |
| `test_theme_day_window_closed_open` | 不跨零点区间 `[06:00, 18:00)` 逐边界检查：左闭右开 |
| `test_theme_day_window_wraps_midnight` | 跨零点区间 `[18:00, 06:00)` 取补集 |
| `test_theme_equal_bounds_yield_no_day` | 两时刻相等 → 全时段夜间，不出现"永远日间"死区 |

**时刻钳制（1 个）**

| 用例 | 钉住什么 |
| --- | --- |
| `test_clamp_minutes_of_day` | `0`→`0`、`1425`→`1425`、`1426`/`1440`/`65535`→`1425`（不包成 0 点） |

**WiFi 信号格数（3 个）**

| 用例 | 钉住什么 |
| --- | --- |
| `test_rssi_bars_thresholds` | 四档边界取等号：`-55`→4、`-56`→3、`-65`→3、`-66`→2、`-75`→2、`-76`→1，`-100` 仍留一格 |
| `test_rssi_unknown_is_zero_bars` | 哨兵 `0` 与非法正值都落 0 格，不能被画成满格 |
| `test_rssi_bars_monotonic` | 结构性质：RSSI 从 -100 递增到 -30，格数单调不减且不超过 4 |

### 3.3 设计原则：不只钉输出，还钉结构性质

测试**同时钉边界值和结构性质**（双射、首尾闭合、形状约束）。好处是将来即便重写实现，只要
性质不变就能通过；如果实现改坏了性质，就会被拦住——而不是只比对"当下这一版的输出"。

举例：`test_chip_id_is_bijective_on_sample` 断言的不是"某个 MAC 得到某个 ID"，而是
"洗两次能还原 + 样本内不碰撞"。前者换个掩码就全红（噪音），后者只在真的破坏双射时才红。

### 3.4 新增用例怎么写

1. 在 `test_pure_logic.cpp` 里加一个 `void test_xxx(void)` 函数
2. 在文件末尾的 `main()` 里加一行 `RUN_TEST(test_xxx);`
3. 若测的是新的纯逻辑，先在 `pure_logic.h` 里加实现
4. 跑 `python tools/run_host_tests.py` 确认绿

## 4. 持续集成

### 4.1 触发与并发

`.github/workflows/ci.yml` 在 `push`、`pull_request`、`workflow_dispatch`（手动）时触发。

```yaml
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true
```

同一分支上的旧运行会被新的顶掉——所以整体显示 **cancelled 往往是正常现象**，不代表失败。

### 4.2 三个 job

| Job | 名字 | 步骤 | 失败说明什么 |
| --- | --- | --- | --- |
| `firmware` | 固件编译 | 装 Python/PlatformIO → 缓存 `~/.platformio` → `pio run` → 回显镜像体积 | 固件编译不过 |
| `host-tests` | 主机端单元测试 | 装 PlatformIO → `pio test -e native` | 纯逻辑回归被破坏 |
| `docs` | 文档与台账一致性 | `python -m compileall -q tools` → `python tools/doc_check.py` | 文档数字漂移 / 脚本语法错 / 固件台账不自洽 / 字库漏字 |

两个细节值得注意：

- **缓存键**：`firmware` job 的缓存 key 带 `hashFiles('platformio.ini')`，改了平台版本会
  自动重建缓存，不会拿旧工具链糊弄
- **`fetch-depth: 0`**：`docs` job 的 `checkout` 必须全量克隆，否则浅克隆里查不到台账记录
  的历史提交，新鲜度检查会被跳过

### 4.3 本地预演 CI

提交前把 CI 的三件事在本地跑一遍，能省一轮等待：

```powershell
pio run                          # ← 对应 firmware job
python tools/run_host_tests.py   # ← 对应 host-tests job
python tools/doc_check.py        # ← 对应 docs job（内含字库覆盖检查）
python tools/font_check.py       # 单独看字库覆盖明细（doc_check 里被 --quiet 抑制）
```

## 5. 工具脚本详解

| 脚本 | 用途 |
| --- | --- |
| [`publish_firmware.py`](#51-publish_firmwarepy--固件发布与台账) | 把 `.pio` 构建产物发布成 `firmware/firmware.bin`，并写下构建台账 |
| [`doc_check.py`](#52-doc_checkpy--文档一致性校验) | 校验文档与代码/产物是否一致，CI 用它拦住文档漂移 |
| [`serial_regression.py`](#53-serial_regressionpy--串口回归) | 读串口周期上报并按区间断言，把"手工盯日志"换成一条命令 |
| [`run_host_tests.py`](#54-run_host_testspy--主机端测试入口) | Windows 上跑主机端单测，自动补编译器路径 |
| [`genvlw.py`](#55-genvlwpy--中文字库生成) | 从系统 TTF/TTC 抽取汉字生成 `font_cn16.h` / `font_cn26.h` |
| [`font_check.py`](#56-font_checkpy--字库覆盖校验) | 校验源码用到的字符是否都在字库中，`doc_check.py` 会调用它 |

### 5.1 `publish_firmware.py` — 固件发布与台账

**为什么需要它**：仓库里那份 `firmware.bin` 曾经落后源码 20 多个提交，而 README 还写着
"与当前源码对应"。手工 `cp` 解决不了——手工步骤会忘，忘了也看不出来。本脚本把"发布"
变成一条命令，并顺手写下这台机器上无法伪造的台账。

**用法**（两步，别手工复制）：

```powershell
pio run                                  # 先构建
python tools/publish_firmware.py         # 再发布（写入镜像 + .sha256 + BUILD.txt）
python tools/publish_firmware.py --check # 只校验现状，不写文件；不一致返回码非 0
```

**产出三个文件**：

| 文件 | 内容 |
| --- | --- |
| `firmware/firmware.bin` | 应用镜像本体（纯 app 分区镜像，只能写入 `0x10000`） |
| `firmware/firmware.bin.sha256` | 摘要文件，格式与 `sha256sum -c` 兼容 |
| `firmware/BUILD.txt` | 构建台账，供 `doc_check.py` 校验 |

**台账字段**：

| 字段 | 含义 |
| --- | --- |
| `env` / `version` / `platform` | 构建环境名、`FW_VERSION`、平台版本 |
| `commit` / `commit_short` / `commit_date` | 构建时的 git 提交与日期 |
| `source_dirty` / `source_dirty_files` | 工作区是否含未提交改动，以及具体文件 |
| `build_time` / `size` / `sha256` | 构建产物的时间、字节数、摘要 |
| `bootloader_size` / `partitions_size` | 同批 `bootloader.bin` / `partitions.bin` 的大小 |

**判定"dirty"只看向 `src/`、`include/`、`platformio.ini` 的改动**——改 README 或设计稿
不会让镜像显得脏。若发布时工作区不干净，台账会如实标注 `source_dirty = true` 并列出文件；
想让镜像对应一个确定的提交，**先提交再重新发布一次**。

`--check` 会打印台账、核对镜像本体的体积与 SHA-256，若发现 `.pio` 里的构建产物与已发布
镜像不同，还会提示"源码改过但没重新发布"。

### 5.2 `doc_check.py` — 文档一致性校验

```powershell
python tools/doc_check.py            # 有 ERROR 时返回码 1
python tools/doc_check.py --strict   # WARN 也当 ERROR（想卡得更紧时用）
```

只依赖标准库与 git，校验五类：

1. **README 文件树里的行数 / 体积** vs 实际文件：形如 `xxx.h …（194 行）`、
   `…（约 336 KB）` 的声明会被逐条比对。体积按**整数 KB 精确比对**（约定是"N = 实际
   KB 四舍五入"），不容差——曾用 2% 容差，结果 805→793 这种真实漂移被吞掉了
2. **设置项数三方一致**：`SystemSettingField` 枚举的 `Count`、README 内联表、
   `docs/settings.md` 表，以及各处"N 项"字样。只认表头第一格是 `#` 的编号表，避免把
   引脚表、参数表误算进来
3. **固件台账自洽**：体积与 SHA-256 在 `BUILD.txt` / `firmware.bin.sha256` / 文件本体
   三方一致
4. **字库覆盖**：源码字符串字面量用到的字符是否都在字库里（复用 `tools/font_check.py`
   的实现，不重复写一份扫描器）
5. **镜像新鲜度**：源码在台账记录的提交之后是否又改过（**仅告警，不阻塞**；无 git 环境时
   跳过）

**输出怎么读**：每条问题一行，`WARN`/`ERROR` 前缀，下一行以 `→` 开头给出修法。结尾打印
`N 项通过 / M 条告警 / K 条错误`。

这类数字没有守护机制就会悄悄跑偏——README 曾标 `ui_model.h` 194 行（实际 230）、
`font_cn26.h` 约 805 KB（实际 793 KB），都是当初写对后来失守的。

### 5.3 `serial_regression.py` — 串口回归

把"烧录后手工盯日志"换成一条可复现的命令。

```powershell
# 抓 30 秒：断言有启动标记、有周期上报、读数在合理区间
python tools/serial_regression.py --port COM4 --seconds 30

# 更严：要求 AHT20 与 INA226 都在线、且读数不是 nan
python tools/serial_regression.py --port COM4 --seconds 30 --require-sensors

# 复核别人抓好的日志（不需要设备）
python tools/serial_regression.py --log serial.txt

# 设备已在上电运行、只补抓周期上报（跳过启动标记断言）
python tools/serial_regression.py --port COM4 --seconds 20 --no-boot-check
```

**参数**：

| 参数 | 说明 |
| --- | --- |
| `--port` / `--log` | 二选一（互斥且必填）：实时抓串口，或复核已有日志文件 |
| `--seconds` | 抓取时长；`--port` 模式默认 30 秒，`--log` 模式须显式给出才会断言上报条数 |
| `--baud` | 波特率，默认 115200 |
| `--require-sensors` | 要求 AHT20 与 INA226 均在线且读数非 `nan` |
| `--no-boot-check` | 跳过启动标记断言 |

**断言清单**（任一不满足即失败，返回码非 0）：

| 断言 | 判据 |
| --- | --- |
| 出现启动标记 | 文本含 `ESP32-S3 N16R8 temperature-control board starting` |
| 周期上报条数达标 | 按 **2 s/条**（`main.cpp` 的上报间隔）留 25% 余量算出下限 |
| 仓温在合理区间 | `[-20, 120] ℃`——超出说明传感器/换算坏了，而不是环境真到了这个温度 |
| 湿度在合理区间 | `[0, 100] %` |
| 无故障态 | 不出现 `FAULT: <reason>` |
| （可选）传感器在线 | `AHT20: detected` 且 `INA226 (0x40, 10mOhm): detected`，且上报不全为 `nan` |

脚本按正则抽取 `chamber=<t>C humidity=<h>%`、`AHT20|INA226 …: detected|not detected`、
`FAULT: …` 三类行。**`--require-sensors` 在未接传感器的板子上会失败，属预期**，所以它
不进 CI，只在上机时手动跑。依赖 `pyserial`（`pip install pyserial`）。

> ⚠️ **抓不到日志时先看这一条**：`platformio.ini` 的 `-DARDUINO_USB_CDC_ON_BOOT=1` 让
> Arduino `Serial` 走 S3 **原生 USB-CDC（板载 USB 口）**；FTDI 接的 UART0 上只剩 ESP-ROM
> 与 ESP-IDF 日志（`entry 0x` 等）。**所以在 FTDI 的 COM4 上看不到 `chamber=` 是正常的，
> 不代表设备卡死。** 要抓应用日志请改接板载 USB 口，或临时注释该宏后重烧（**抓完务必恢复
> 并重烧**）。

### 5.4 `run_host_tests.py` — 主机端测试入口

Windows 上跑主机端单测的包装脚本，自动补 MinGW 路径（详见 [§3.1](#31-三种运行方式)）。
Linux / macOS / CI 上直接 `pio test -e native` 即可，无需此脚本。

### 5.5 `genvlw.py` — 中文字库生成

从系统 TTF/TTC 抽取汉字生成 `include/font_cn16.h` / `font_cn26.h`。它自动扫描 `src/` 与
`include/` 中字符串字面量里的汉字（注释中的不算），并**排除** `web_page.h` /
`wifi_portal_page.h`——那两处中文只由浏览器渲染，纳进来只会白占 Flash。新增中文文案若用到
字库外的汉字，需重跑本脚本。完整用法见
[settings.md](settings.md#新增设置项的改法)。

**它能做什么、不能做什么**：只认普通字符串/字符字面量，看不懂原始字符串 `R"(...)"` 与
`\uXXXX` / `\xNN` 转义（真用了这类写法得手工把字补进 `BASE_CJK`，`font_check.py` 会报出来）。
脚本头部打印的"数据 N 字节"是**数据字节数**，头文件体积约为它的 5 倍（`0xNN,` 文本格式），
别把两者混比。

### 5.6 `font_check.py` — 字库覆盖校验

```powershell
python tools/font_check.py           # 有 ERROR 时返回码 1
python tools/font_check.py --quiet   # 不打告警明细
```

**为什么需要它**：字库由脚本扫源码生成，“加了中文文案忘了重新生成”没有任何机制拦着 ——
这个项目的字形数已经走过 295 → 322 → 326，三次都是靠人记得。漏字的现场表现**很难反推**：
`TFT_eSPI` 的 `Extensions/Smooth_font.cpp` 对查不到的码位会 `drawRect` 一个 `spaceWidth` 的
空心方框，并且 `textWidth` 只按 `spaceWidth + 1` 计宽，于是居中文本会跟着整体偏移，看起来像
“排版错乱”而不像“缺字”。

校验六项：

1. 头文件注释声明的字形数/数据字节与二进制本体一致（拦住手工编辑）
2. 两个字号（16/26）的**字形集**完全一致，且 `version = 0x0B`、`fontSize` 正确
3. ASCII 0x20–0x7E 齐全（数字、单位、百分号缺一个就是一屏方框）
4. 源码用到的非 ASCII 字符全在字库中 —— 报错会带上**字符、码位与来源文件**
5. 非空格字形不许是空位图（在表里却画不出来，比缺字更隐蔽）
6. 未被源码引用的字形 → 告警，并给出两个字号合计占用的 Flash 字节数

只依赖标准库（**不需要 PIL 与 TTF**，所以能在 CI 里跑；重新生成字库才需要 Pillow）。
扫描器是**独立实现**，刻意不复用 `genvlw.py` 的解析器 —— 同一份逻辑自查等于没查。
反向验证过：往 `src/tft_ui.cpp` 的字面量里塞一个未收录的字，本脚本报出
`字库缺少 1 个源码用到的字符:鼍(U+9F0D) ← src/tft_ui.cpp` 并以返回码 1 退出。

`doc_check.py` 已把它并入（第 4 类，告警按 `--quiet` 抑制），所以 CI 的 `docs` 作业会一并拦住。

## 6. 按场景的改动流程

| 你改了什么 | 提交前必须跑 | 还要注意 |
| --- | --- | --- |
| `include/pure_logic.h` | `python tools/run_host_tests.py` | 必须**同步改测试**，否则 CI 红 |
| `src/tft_ui.cpp`（视觉） | `pio run` + 用 `tools/ui_preview.html` 对照 | 触摸热区在 `src/main.cpp`，改视觉要同步热区或避开锚点 |
| 设置项枚举 / 界面文案 | `python tools/doc_check.py` | 设置项数要三方一致；用到新汉字要重跑 `genvlw.py`，`font_check.py` 会指出漏了哪个字 |
| `src/main.cpp` 的串口上报格式 | 同步 `serial_regression.py` 里的正则 | 格式变了回归脚本会失准 |
| `platformio.ini`（构建宏/平台） | `pio run` + `python tools/doc_check.py` | 平台版本牵连 ledc API 与文档 |
| 任何影响镜像的改动 | `pio run && python tools/publish_firmware.py` | 刷新固件台账，别手工 `cp` |
| 联网代码（`network.cpp`） | `pio run` | 主机端测不到，只能上机验 `http://chamber.local/` |

**通用四步**：改代码 → 跑对应的本地检查（§4.3）→ 提交 → 刷新固件台账 → 上机回归。

## 7. 排障速查

**主机端测试报"找不到主机端 C++ 编译器"**
→ Windows 上直接用 `python tools/run_host_tests.py`（会自动找 MinGW）；仍失败则
`pio pkg install --global --tool platformio/toolchain-gccmingw32`。

**主机端测试里 64 位断言"失败"（不是编译错）**
→ 检查 `[env:native]` 的 `-DUNITY_SUPPORT_64` 是否被删掉。

**`genvlw.py` / `pio run` 报 `pure_logic.h` 里的 `constexpr` 语法错**
→ `[env:native]` 的 `-std=gnu++11` 被删了。

**`doc_check.py` 报 ERROR**
→ 读每条下面的 `→` 修法：行数/体积不符就改 README 里的数字；设置项数不符就补表格到
`1..N` 连续；台账不符就重跑 `publish_firmware.py`。

**`doc_check.py` / `publish_firmware.py` 提示镜像"落后于源码"或"工作区不干净"**
→ 这是告警不是错误。要消除：先提交，再 `pio run && python tools/publish_firmware.py`。

**`pio run` 报 `ledcSetup` / `ledcAttachPin` 未定义**
→ 平台被解析到了 core 3.x。确认 `platform = espressif32@6.7.0` 没被改成 `^6.7.0`。

**串口抓不到 `chamber=` 上报**
→ 见 [§5.3](#53-serial_regressionpy--串口回归) 的 USB-CDC 说明：多半是接错了口。

**串口报 `AHT20: not detected` / `INA226 … not detected`**
→ 串口单键发 `i`，触发一次 I²C 总线扫描（0x08–0x77），逐地址打印 `0xNN ACK`。
一个 ACK 都没有是总线级问题（上拉电阻、3V3 供电，或 SDA/SCL 接反）；只出现
`0x38` 指向 INA226 侧，只出现 `0x40` 则多半是 AHT20 外接模块（CN2）没插好。

**屏上出现 W-0x 网络告警（主屏日期位变告警文案，设置页标题栏带 W 码）**
→ 这是**非阻塞提示**，不影响加热与打印，不用急着处理。对照
[networking.md](networking.md) 的 W 码表：W-01/W-02 查 WiFi 密码与 SSID，
W-03 查 broker 是否要求匿名/凭据，W-04 查 broker 地址、端口与防火墙。
串口（USB-CDC 口）里 `[NET] alert W-0x …` 会带连续失败次数与原始原因码；
MQTT 侧失败时 `connect failed rc=N` 的 N 就是 CONNACK/PubSubClient 错误码。
单次重连不会告警 —— 连续 10 次同类失败才提示（跨过路由器重启等瞬断窗口），连上即自动清除。

**开机直接进故障页（没看到检测/待机）**
→ NVS 故障记忆在起作用：上次是热类故障（F-03/F-04/F-05）锁定时断的电，重启
后自动恢复锁定，防止拔电绕过硬保护。这不是 bug；处理完根因后长按编码器复位
即可。历史记录（`lastFault`/`faultCount`/`lastFaultEpoch`）在 `/api/state`
里可查，恢复出厂设置会连记录一起清掉。串口 `[FAULT] F-0x lock restored …`
就是这条路径的日志。

**开机直接进注册页（旋转选字符、单击确认、长按跳过）**
→ 设备未注册：NVS 里没有与本机芯片 ID 派生码匹配的 `regCode`。注册码是一机
一码（FNV-1a-64(chipId+盐) 低 32 位，算法与厂商端 Python 参考实现见
[settings.md](settings.md) 的「注册码」一节）。输入正确码即注册；**长按编码
器可跳过**，功能不受限、下次上电再提示。恢复出厂不会清除已写入的注册码。

**CI 显示 cancelled**
→ 同一分支有新推送顶掉了旧运行，正常现象，看最新那次即可。

## 8. 仍需人工验证的部分

自动化到不了的地方，改动时请显式说明已上机验证：

- **加热链路**：预热 / 恒温 / 过温保护整条主链，需 `HEATER_ENABLED=true` 且 NTC 与
  INA226 已标定（见 [hardware.md](hardware.md#上电前必须复核)）
- **传感器接线**：AHT20 / INA226 的 I²C 供电与上拉，靠串口 `detected` 与回归脚本判定
- **屏上视觉**：三档主题的实际观感、字号与配色可读性
- **故障页**：F-01…F-06 的屏上标题、处理建议与配色，需构造对应工况实测
  （掉线类可拔线复现——注意只在自动温控开启且处于预热/打印时才检测；硬过温/过流需 `HEATER_ENABLED=true` 且已完成标定）。
  顺带核对**屏上编号与 MQTT `event` 里的编号都要等于枚举值**（F-03 应显示
  `BoardOverTemp`）—— 这两处曾一起多加了 1，导致码整体错位
- **网络告警**：W-01…W-04 的主屏日期位与设置页标题栏文案，需构造工况实测
  （W-01/W-02 可改错密码/SSID 复现，W-03/W-04 指向不可达或拒绝匿名的 broker）
- **触摸校准**：本机为无触摸版本（`Pin::HAS_TOUCH_PANEL = false`），换屏后需在实机两点采点。
  置 `true` 后若开机串口打 `Touch panel: NOT detected, touch disabled`，说明膜在位探测
  没过（排线/屏版本问题），触摸会被整路关闭——这是防幽灵触摸的保护，不是 bug
