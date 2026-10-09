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
- CI `.github/workflows/ci.yml` 三 job:固件编译 / 主机端单测 / 文档一致性
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
