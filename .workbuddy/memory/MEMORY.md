# 耗材仓项目 · 长期要点

ESP32-S3 N16R8 智能耗材仓(温湿度 + 加热 + 排风),PlatformIO + TFT_eSPI 480×320 横屏。
仓库公开(GitHub: hongligongsi/Consumables-storage-temperature-and-humidity),严禁提交凭据。

## 构建与烧录
- `pio run` 编译 / `pio run -t upload --upload-port COM4` 烧录;`git push` 需 dangerouslyDisableSandbox
- platform 锁 `espressif32@6.7.0`(core 2.x):保留 `ledcSetup`/`ledcAttachPin` 旧 API,升级平台前必须先迁移
- TFT_eSPI 必须带 `-DUSE_FSPI_PORT`,否则 SPI 寄存器基址算成 0 → StoreProhibited 崩溃

## 工程化与测试(2026-10-09 起)
- 纯逻辑层 `include/pure_logic.h`:不依赖 Arduino 的纯函数(芯片 ID `formatChipId` / 主题解析
  `effectiveTheme` / 时刻钳制 `clampMinutesOfDay`),固件与主机端单测共用;
  **改它必须同步改 `test/test_pure_logic/` 并跑通测试**,否则 CI 红
- 主机端单测:`python tools/run_host_tests.py`(Windows 自动补 PlatformIO 自带 MinGW 路径)
  或 `pio test -e native`;`[env:native]` 的 `-std=gnu++11`、`-DUNITY_SUPPORT_64` 不可删
- 文档/产物校验:`python tools/doc_check.py`(README 树行数体积、设置项数三方一致、固件台账 sha256)
- 固件发布:改源码后 `pio run && python tools/publish_firmware.py` 刷新 `firmware/firmware.bin`
  与台账 `BUILD.txt`/`.sha256`,**别手工 cp**
- 串口回归:`python tools/serial_regression.py --port COM4 --seconds 30`(加 `--require-sensors`
  才要求 AHT20/INA226 在线,未接传感器的板子会失败属预期)
- CI `.github/workflows/ci.yml` 三 job:固件编译 / 主机端单测 / 文档一致性;
  已在 GitHub runner 上验过(10-09):文档一致性与主机端单测两 job green,固件编译作业
  耗时最长(首次要下 xtensa 工具链),本机 `pio run` 已通过。
  `concurrency.cancel-in-progress` 会让新推送顶掉旧运行,整体显示 cancelled 属正常
- 查 CI 只能走 REST(curl api.github.com/.../actions/runs),本机 **没有 gh CLI**
- 开发文档入口:`docs/development.md`(2026-10-09 新增)

## ⚠️ 串口日志分流(排障必读,踩过坑)
`platformio.ini` 的 `-DARDUINO_USB_CDC_ON_BOOT=1` 让 Arduino `Serial` 走 **S3 原生 USB-CDC(板载 USB 口)**;
FTDI→COM4(UART0) **只剩 ESP-ROM 与 ESP-IDF 日志**(`entry 0x`、`[E][Preferences.cpp]` 等)。
**所以在 COM4 看不到 `chamber=` 周期上报、AHT20 打印,是正常现象,不代表设备卡死。**
要抓应用日志:临时注释该宏 → 烧录 → 抓完**务必恢复并重烧**。
- 判据:只看 COM4 时,`BOOT_OK` = 出现 `entry 0x`

## PSRAM 与显示
- 板载 8MB OPI PSRAM 硬件不可用(大块分配挂死总线)。用 `qio_qspi` 变体 → PSRAM 不并入堆
- 因此渲染走 `TFT: direct-render fallback, PSRAM free=0 bytes`,**属预期,非故障**

## 硬件现状
- AHT20 / INA226(0x40, 10mOhm) 当前报 `not detected` → 温湿度读数 nan,待排查 I²C 接线/供电
- 设备唯一 ID `NetworkManager::chipId()`:eFuse 48 位 MAC 全宽异或(双射),格式 `CH-XXXXXXXXXXXX`,兼作 OTA 密码

## UI 工作流
- 设备主屏:`src/tft_ui.cpp`(调色板/图标/布局);快照 `UiSnapshot` 由 ui_model 产出,clock/humidity/chipId 由 main.cpp 补填
- **三档主题(2620f35)**:`settings.theme` 0=默认(iOS 浅色原型)/1=IOS(深色 HMI)/2=蓝白(出厂默认,日/夜按时钟自动);resolveTheme 解析为生效值 0/1/2/3,applyPalette 四选一。**新增主题只改 tft_ui.cpp 的 Palette 表**,浅底主题黄/橙必须取加深版保证可读;开关颜色=G_CYAN
- 触摸热区在 `src/main.cpp`(notifyTouch 分区),**改视觉必须同步热区或避开锚点**
- 触摸两道闸:编译期 `HAS_TOUCH_PANEL`(语义="允许触摸")+ 开机 `detectTouchPanel()`
  电阻膜在位探测(一端拉低、另一端弱上拉读电平,4 次全低才算在);探测不到 →
  `touchPanelPresent=false`,轮询/校准整路关闭防幽灵触摸,串口打 `Touch panel: NOT detected`
- 屏上 mockup:`.workbuddy/tmp/ui-main-mockup.html` → 无头 Chrome
  `chrome --headless --disable-gpu --force-device-scale-factor=2 --window-size=W,H --user-data-dir=.workbuddy/tmp/chrome-profile --screenshot=OUT.png file:///...`
- 浏览器模拟器:`tools/ui_preview.html`
- 管理页:`include/web_page.h`(PROGMEM raw string,无 mock 段)↔ `tools/web_management_preview.html`(含 mock)
  改完用正则 `R"HTML\((.*)\)HTML";` 反提取 + difflib 与预览稿比对,校验 raw string 未破损
- 字号换算脚本 `tools/genvlw.py` 生成 `include/font_cn16.h`/`font_cn26.h`;
  已排除 `web_page.h`/`wifi_portal_page.h`(其中文只由浏览器渲染,纳入只会白占 Flash)

## .workbuddy 入库策略(2026-10-09 改版)
`.gitignore` 为**白名单式(只列排除项)**,仅排除:
- `.pio/` —— 100+ MB 构建缓存,`pio run` 可重建
- `.workbuddy/tmp/chrome-profile/` 与 `.workbuddy/tmp/cp2/` —— 两份 Chrome 个人档案,
  含 `Login Data`(已存账号密码)/`Cookies`/`History`,**公开仓库红线,任何情况不得放行**
- `__pycache__/`、`*.pyc`、`*.tmp`
其余**全部纳入版本控制**,含 `.github/`、`.trae/`、`.vscode/`(`c_cpp_properties.json`/`launch.json`)
与 `.workbuddy/` 整棵(记忆日志 / 原理图切片 / 脚本 / `tmp/` 下的设计稿 html 与 png 截图)。
> 旧策略为「整体忽略 + 逐项放行」,曾明确排除 `tmp/*.png`;用户 2026-10-09 明确要求
> 「全部文件上传(含 .xxx)」后改为此版。若日后想收紧,只需把 `tmp/*.png` 加回排除项。

## 字库与行尾(2026-10-09 踩坑)
- genvlw.py 打印的字节是**数据字节数**,头文件大小≈数据×5(0xNN, 文本格式);README/
  doc_check 标注的是**文件大小**。别把两者混比 —— 曾因此误判字库被缩小
- autocrlf=true 下 `git checkout` 会把字库头文件转成 CRLF(每行+1B),doc_check 的
  体积校验会误报;字库保持 genvlw 生成的 LF 版,勿用 git checkout 恢复
- FaultCode 故障码 F-01…F-06:新增码只能追加在 Count 前;硬保护阈值是派生常量
  (15℃/1.5 倍),**不加设置项**否则 doc_check 设置项数三方校验会红

## 网络告警通道(2026-10-09)
- `NetAlert`(ui_model.h)与 `FaultCode` 是**两条互不相干的通道**:W 码只提示
  不停机,不进故障页不锁状态机;改任何一处都要同步 NET_ALERT_TEXTS 与 W 码表
- 判定全是「连续 10 次同类失败」(NET_ALERT_STREAK=10,2026-10-09 从 3 提到 10,
  跨过路由器重启/信号抖动;MQTT≈50s,WiFi≈1~2 分钟);WiFi 关闭/离线/从未配网
  **刻意不报警**;WiFi 告警
  优先于 MQTT。MQTT 告警**发不进 MQTT**,远程只能轮询 REST `netAlert` 字段
- ⚠️ 判「从未配网」只能用 `g_wm.getWiFiSSID()`(读持久化 STA 配置);
  **`WiFi.SSID()` 读的是"当前已连接的 AP",在 DISCONNECTED 事件里恒为空** ——
  曾误用它当判据,导致 W-01/W-02 永不触发(2026-10-09 修)。与之相对,屏显
  "当前连着谁"用 `network.ssidString()`(connected() 才返回 WiFi.SSID())

## NVS 故障记忆(2026-10-09)
- `FaultRecord`(settings.h)四键:code/latched/count/epoch;锁故障落盘、
  解锁只清 latch 留历史、恢复出厂全清
- 只有热类(F-03/04/05)跨重启恢复锁定(`persistsAcrossReboot`/`restoreFault`);
  传感器掉线类开机清 latch 留历史。main.cpp 的 `lastSeenFault` 必须先与
  setup 恢复态对齐,否则"恢复"会被边沿检测记成一次新故障
- REST `/api/state` 的 lastFault/faultCount/lastFaultEpoch 即此档案;
  NVS 写失败只打日志不重试(记忆是增强不是前提,别拖热控)

## RGB 灯带与注册码(2026-10-09)
- RGB:GPIO18 单线 DIN 串 **4 颗** WS2812(同显一色);「RGB最大亮度」在
  setRgb 内做全局缩放且缩放进颜色缓存(改设置即使颜色不变也会重发一帧);
  「RGB跟随屏幕休眠」复用 screenIsSleeping 判定,熄屏帧靠缓存只发一次
- 注册码 = FNV-1a-64(chipId + "FilamentChamber-RG1") 低 32 位 8 位大写 hex,
  实现在 pure::regCodeFromChipId(**有主机端向量测试**,盐/算法改动会红);
  NVS 键 regCode,恢复出厂刻意不清;上电未注册弹页、**长按跳过(提示不限制)**;
  模态与 touchCalibrationActive 同构(main.cpp 持状态,dispatchUiAction 截获输入)
- **genvlw.py 需要 PIL,要用系统 Python 跑**(AppData\Local\Programs\Python\Python313),
  托管 Python 没装 PIL;font_check 只依赖标准库所以 CI 无碍
- 新增设置项的完整链路(本次走两遍):settings.h/cpp(字段+NVS+sanitize+reset 键表)
  → SystemSettingField 枚举 → ui_model.cpp(adjust 范围 + numeric 判定 + 请求标志)
  → tft_ui(zh/en 标签 + value case)→ REST(network.cpp readInt/GET JSON)→
  web_page.h ↔ web_management_preview.html → ui_preview.html(F 数组 + 默认值)→
  README/settings.md/docs/README.md 三处表与"N 项" → genvlw + doc_check

## 故障码/告警编号与有效性(2026-10-09 复查)
- **显示编号 = 枚举值**(AhtLost=1 → F-01,None=0 只是哨兵)。曾因 controller.h
  注释写成"下标+1",tft_ui 与 network 的 MQTT event 都多加了 1,整码错位 ——
  改码时务必核对三处:屏上(tft_ui)、MQTT(event)、README/docs 表格
- 故障页/告警**有效性判据只能有一份**:AHT20 用 main.cpp 的 `ahtFresh()`
  (`!isnan && millis()-lastSensorOk<AHT_HOLD_MS`);chamberTemp 读失败会保留旧值,
  显示侧若只看 `!isnan` 会把旧值当现值,与热控侧判据打架
- 复位两条路径效果不同:长按 → `clearFault()`(热类故障进安全冷却,保留硬保护
  计时以立即复锁);关系统 → update() 的 `!systemEnabled_` 分支(直接 Idle)。
  故障锁定期只放行长按与系统总开关

## 字库覆盖检查(2026-10-09 新增,防漏字)
- `tools/font_check.py`:源码字面量字符 ⊆ `font_cn*.h` 字形表。**已并入 doc_check.py**
  (importlib 按路径加载),CI docs 作业自动跑;**只依赖标准库,不需要 PIL/TTF**
- 缺字的两种表现:**①码位不在表里 → 空心方框**(Smooth_font.cpp 的 else 分支 drawRect,
  且只推进 spaceWidth+1、textWidth 同算 → 居中文本会整体偏移,现场像"排版错乱");
  **②码位在表里但位图为空 → 静默不画**。加中文文案后必须重跑 genvlw.py
- 扫描器刻意**独立实现**,不得与 genvlw.py 的 `collect_literal_chars` 合并(同逻辑自查
  等于没查);font_check 额外看得懂 `R"(...)"` 与 `\uXXXX`/`\xNN`,genvlw 看不懂
- genvlw.py 是确定性的:重跑后与仓库字库逐字节一致 → 可用来验证"字库是否与源码同步"
  (先 `cp` 备份再 `cmp`;别用 git checkout,见上一条 autocrlf 坑)
- 写脚本文档字符串时若要写 `\uXXXX` / `\xNN`,module docstring 必须是 `r"""..."""`
  否则整个文件 SyntaxError(`compileall -q tools` 会拦住)
- `BASE_CJK` 是**静态**兜底表,会随文案过期:当前 41 个字无引用,两字号合计白占 38 KB
  Flash(font_check 会告警列出)。要精简就删 BASE_CJK 里的字再重生成,并跑 doc_check

## WiFi 信号格(2026-10-09)
- 主屏 4 格信号条的数据链:`NetworkManager::rssiDbm()` → `UiSnapshot::rssi`(int8_t,
  未联网为哨兵 0)→ `pure::rssiBars()` → tft_ui 画 4 格
- **阈值 -55/-65/-75 写在三处**:`include/pure_logic.h`(固件)、`include/web_page.h`
  的 setBars(网页)、`tools/ui_preview.html`(模拟器)—— 改一处必须三处一起改
- `kRssiUnknown = 0` 是哨兵(0 dBm 不会出现);`WiFi.RSSI()` 异常返回 ≥0 或 ≤-128 也按无信号
- 绘制参数(固件与模拟器必须一致):条宽 3 / 间隙 2 / 起点 wx-9 / 底边 wy+10 / 高 4,7,10,13;
  ≤1 格红、2 格黄、≥3 格绿;断网四格全空 + 红斜杠。底边 56,下方卡片自 y=62 起,别下移
- UI 布局约定:主屏左侧状态行 y=46,X 方向从 15(状态点)→24(状态文字)→图标,长度按
  `g.textWidth(stateText)` 累加;改文案长度会挤动右侧图标位置
