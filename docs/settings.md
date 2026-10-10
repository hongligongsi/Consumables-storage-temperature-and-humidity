# 系统设置逐项说明

设置页共 **31 项**，顺序与屏幕显示一致，枚举定义在
[include/ui_model.h](../include/ui_model.h#L47-L79) 的 `SystemSettingField`。
渲染时在 7 个分区起点插入标题行（**基础 / 显示与声音 / 输入与联动 / 热控 /
外观 / 时钟 / 维护**，定义在 `tft_ui.cpp` 的 `SECTIONS` 表），标题行仅作视觉
分组、不可聚焦，导航与页码计数仍按 31 个条目走。

真机每屏只显示 **5 行**（分区标题行占一行），超出部分靠旋转滚动查看。

## 操作方式

| 操作 | 作用 |
| --- | --- |
| 旋转 | 移动光标；在编辑态下改值 |
| 单击 | 开关类：直接取反<br>数值类：进入编辑，再旋转改值<br>日期/时间：切到下一段 |
| 长按 | 保存到 NVS 并返回主界面 |

开关类即使进入编辑态也无意义，单击即切换、不区分旋转方向。数值类需先单击进入
编辑态，之后旋转才改值；日间/夜间时刻、日期/时间等多段项靠单击在段间跳转。

设置项总数由枚举的 `Count` 哨兵推导，页码与滚动窗口都动态计算，增删条目无需改
渲染代码。

## 联网开关

这四项在**长按保存后**经 `network.onSettingsChanged()` 即时生效，**无需重启**。
字段定义见 [include/settings.h](../include/settings.h#L28-L36)。

| # | 条目 | 英文 | 默认 | 说明 |
| --- | --- | --- | --- | --- |
| 2 | WiFi联网 | WIFI | 开 | 关闭后设备断开网络，Web 与 MQTT 均不可用。**本地关闭后可在本页重新打开**；远程 `POST /api/settings` 传 `wifiEnabled=0` 会被拒绝并返回 400。联网时本项右侧显示当前连接的 SSID（超长截断）与迷你信号格，方便确认连的是哪张网 |
| 3 | MQTT上报 | MQTT | 关 | 打开后向 broker 上报状态。需先经 Web 接口配置 `mqttBroker`，broker 为空时打开也不会有流量 |
| 4 | NTP校时 | NTP | 开 | 关闭则调用 `esp_sntp_stop()`，沿用上次时钟；已同步的时间在断电前保持有效 |
| 5 | OTA升级 | OTA | 开 | 关闭则 `ArduinoOTA.end()`，设备不再出现在 OTA 端口列表。**关闭后无法远程重新打开**，只能回设置页或重启 |

> 设置页只暴露开关。`mqttBroker`、`mqttPort`、`mqttTopicPrefix` 三个参数仍需经
> `POST /api/settings` 配置，详见 [networking.md](networking.md)。

## 完整列表

| # | 条目 | 英文 | 取值 | 步长 |
| --- | --- | --- | --- | --- |
| 1 | 系统语言 | LANGUAGE | 中文 / English | — |
| 2 | WiFi联网 | WIFI | 开 / 关 | — |
| 3 | MQTT上报 | MQTT | 开 / 关 | — |
| 4 | NTP校时 | NTP | 开 / 关 | — |
| 5 | OTA升级 | OTA | 开 / 关 | — |
| 6 | 按键声音 | KEY SOUND | 开 / 关 | — |
| 7 | 屏幕亮度 | BRIGHTNESS | 1–100 % | 5 |
| 8 | 屏幕休眠时间 | SCREEN SLEEP | 0–3600 s（0 = 不休眠） | 15 |
| 9 | RGB最大亮度 | RGB BRIGHTNESS | 1–100 % | 5 |
| 10 | RGB跟随屏幕休眠 | RGB SLEEP SYNC | 开 / 关 | — |
| 11 | 打印时保持屏幕开启 | KEEP ON PRINTING | 开 / 关 | — |
| 12 | 编码器方向 | ENCODER DIRECTION | 正向 / 反向 | — |
| 13 | PIR启动延时 | PIR START DELAY | 1–300 s | 1 |
| 14 | PIR关闭延时 | PIR STOP DELAY | 10–900 s | 5 |
| 15 | 启动后自动开灯 | LIGHT ON START | 开 / 关 | — |
| 16 | 关闭后自动关灯 | LIGHT OFF STOP | 开 / 关 | — |
| 17 | 启动后蜂鸣提示 | BEEP ON START | 开 / 关 | — |
| 18 | 关闭后蜂鸣提示 | BEEP ON STOP | 开 / 关 | — |
| 19 | 发热板限流 | HEATER CURRENT | 1–12 A | 1 |
| 20 | 发热板风扇风速 | HEATER FAN | 20–100 % | 5 |
| 21 | 发热板温度保护 | HEATER PROTECTION | 40–180 °C | 1 |
| 22 | 界面主题 | THEME | 默认 / IOS / 蓝白 | — |
| 23 | 日间开始时刻 | DAY START | 00:00–23:45 | 15 min |
| 24 | 夜间开始时刻 | NIGHT START | 00:00–23:45 | 15 min |
| 25 | 日期 | DATE | 年 / 月 / 日 三段 | 见下 |
| 26 | 时间 | TIME | 时 / 分 两段 | 见下 |
| 27 | 触摸屏校准 | TOUCH CALIBRATION | 不支持 / 未校准 / 已校准 | — |
| 28 | 恢复出厂配置 | FACTORY RESET | 执行 / 确认? | — |
| 29 | 注册码 | REGISTRATION | 已注册 / 未注册 | 见下 |
| 30 | 固件版本 | FIRMWARE VERSION | 只读 | — |
| 31 | 芯片ID | CHIP ID | 只读（`CH-` + 12 位十六进制） | — |

取值范围由 [src/ui_model.cpp](../src/ui_model.cpp#L8-L99) 的 `adjustSystemSetting`
限制，并在 [src/settings.cpp](../src/settings.cpp#L6-L25) 的 `sanitize()` 中于载入
和保存时二次夹取，即使 NVS 被写坏也不会越界。

## 重点条目说明

### 屏幕休眠时间

仅关闭背光，**系统继续运行**。设为 0 表示不休眠。若「打印时保持屏幕开启」为开，
则打印机工作时忽略此项。

### RGB最大亮度 / RGB跟随屏幕休眠

RGB 状态灯带（GPIO18 单线 DIN 串 4 颗 WS2812，四颗同显一种状态色）的两个
亮度相关项：「RGB最大亮度」对所有状态色统一按比例缩放（1–100 %，夜间场景可调
低）；「RGB跟随屏幕休眠」开启后，屏幕休眠时 RGB 灯同步熄灭，唤醒即恢复——与
背光共用同一个休眠判定，状态机照常运行、只是灯不再亮。

### 注册码

设备注册采用**一机一码**：注册码由本机芯片 ID（`CH-` + 12 位十六进制，即
`NetworkManager::chipId()`，eFuse MAC 双射而来）经固定算法派生，算法在
`pure::regCodeFromChipId()`（`include/pure_logic.h`）：

```
h = FNV-1a-64(chipId 字符串 + "FilamentChamber-RG1")
注册码 = h 低 32 位的 8 位大写十六进制
```

厂商端可用同一算法离线生成（Python 参考实现）：

```python
def reg_code(chip_id, salt="FilamentChamber-RG1"):
    h = 14695981039346656037
    for c in chip_id + salt:
        h = ((h ^ ord(c)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%08X" % (h & 0xFFFFFFFF)
```

也可以直接用 `tools/regcode_generator.html`（浏览器打开，纯离线）：输入芯片 ID
生成 8 位注册码，并可反向核对「授权码与芯片 ID 是否匹配」；页面内置与主机端
单测相同的自检向量（`CH-1A2B3C4D5E6F → 02812C46`）。

行为约定：

- **上电未注册** → 先弹注册页：旋转编码器在当前位循环 `0-9`/`A-F`，单击确认
  进位，8 位输完即校验；**长按编码器跳过**（提示但不限制，功能不受限，下次
  上电再提示）。校验通过写入 NVS 键 `regCode` 并提示成功；失败提示重输。
- 设置页本项显示「已注册 / 未注册」，单击可随时重新进入注册页。
- **恢复出厂不会清除注册码**（`regCode` 不在出厂清除的键清单里）。
- 这是轻量完整性校验，不是密码学保护；目标是防误输与防随手复用。

### PIR 启动/关闭延时

PIR 判定打印机工作状态：

- 检测到运动持续 `PIR启动延时` 秒 → 判定开始打印，启动仓温控制
- 连续 `PIR关闭延时` 秒无运动 → 判定打印停止

默认 50 s。多色打印换色时 PIR 可能长时间无动作，建议调到 90 s 以免误判停止。

### 发热板温度保护

热板温度超过此值即暂停加热，默认 150 ℃（可调 40–180 ℃）。这是软件层的第二道
防线，硬件过温保护独立于此，不可互相替代。

### 界面主题与日/夜时刻

三档主题：**默认**（iOS 浅色，printer-hmi-ios-prototype.html 浅色外观）、
**IOS**（深色 HMI，printer-hmi-redesign.html）、**蓝白**（日间浅蓝白 + 夜间亮蓝）。
「蓝白」下按两个时刻切换：到达「日间开始时刻」切日间配色，「夜间开始时刻」
切夜间配色。两者上限为 23:45 而非 23:59 —— 这样末次递增不会被夹到 23:59 从而
破坏 15 分钟步长。

### 日期 / 时间

手动校时，结果写入 RTC 与 NVS，**重启后仍生效**。系统时钟的取值优先级为：
NTP → 手动校时值（`manualClockEpoch`）→ 占位串 `----/--/-- --:--:--`。

因此断网环境下也能保持正确时间；一旦 NTP 恢复同步则以其为准。

### 触摸屏校准

当前硬件 `Pin::HAS_TOUCH_PANEL = false`（见 [include/pins.h](../include/pins.h#L7)），
本机为无触摸版本，此项固定显示「不支持」，触摸轮询完全停用。

更换为四线电阻触摸屏后需把该常量改为 `true`，并在实机上完成两点采点校准。
改为 `true` 后开机还会做一次电阻膜在位探测：探测不到膜（混用非触摸屏、排线
未接）时此项同样显示「不支持」，触摸轮询保持关闭，避免浮空线被误判成触点。

### 恢复出厂配置

需二次确认（显示「确认?」）才执行。**只删除本固件拥有的设置键与耗材键**，未知键
会保留，例如预留的注册码数据不会丢失。删除后立即重启。

### 固件版本

只读，取值来自 [include/version.h](../include/version.h) 的 `FW_VERSION`，
当前为 `1.0.0`。可用 build_flags 追加 `-DFW_VERSION=\"x.y.z\"` 覆盖，无需改源码。

### 芯片ID

只读，展示本机**逐台唯一**的芯片 ID，格式 `CH-XXXXXXXXXXXX`（12 位十六进制）。
取值由 `NetworkManager::chipId()` 从 ESP32 eFuse 内的 48 位 MAC 生成：MAC 由
IEEE 逐颗分配、天然唯一，固件再对其做 48 位全宽异或（双射、不丢位），因此
**任意两颗芯片的结果必然不同，不会重复**。同一取值也用作 ArduinoOTA 接入密码
（见 [networking.md](networking.md#ota-升级)）。

批量部署时可在屏上直接核对本机 ID，与 Web 管理页「本机 OTA 密码（只读）」、
开机串口日志 `[NET] chip id = CH-…` 三处一致。

该条目的取值由 `main.cpp` 在生成界面快照时从 `network.chipId()` 补入
（`UiModel` 不反向依赖网络层），显示层只读渲染，单击/旋转均无副作用、不置脏。

### 只读条目

「固件版本」与「芯片ID」同属系统设置页末端的只读条目：可被旋转选中、可被高亮，
但单击不进入编辑态、旋转不改值、也不会把设置标记为「有未保存修改」。

## 新增设置项的改法

设置页是枚举驱动的，新增一项只需同步 **4 处**：

1. [include/ui_model.h](../include/ui_model.h#L47-L79) — 在 `SystemSettingField`
   中按显示顺序插入枚举项（功能项放在 `Version` 之前，只读的「关于本机」信息项
   放在 `Version` 之后、`Count` 之前）
2. [src/ui_model.cpp](../src/ui_model.cpp#L11-L115) — 在 `adjustSystemSetting` 的
   switch 中加 case；数值类用 `constrain(..., min, max)`，开关类直接取反，
   只读项与 `Version` 并列进「直接 return」分组
3. [src/tft_ui.cpp](../src/tft_ui.cpp#L428-L620) — 在 `zh[]` / `en[]` 数组的
   **同一位置**插入文案，并在 value 格式化 switch 中加对应 case
4. [tools/ui_preview.html](../tools/ui_preview.html#L597-L741) — 在 `F` 数组同一
   位置插入元数据（`zh`/`en`/`type`/`key`/`desc`/`range`），并视类型在
   `clickEncoder` / `settingValue` 中各加一个分支

若新条目是**数值类**且要支持单击进入编辑态，还需在 [src/ui_model.cpp](../src/ui_model.cpp#L229-L238)
的 `numeric` 白名单中加入它；开关类不需要。

新增中文文案若用到字库外的汉字，需重跑 `python tools/genvlw.py` 重新生成
`include/font_cn16.h` / `font_cn26.h`。该脚本会自动扫描 `src/` 与 `include/` 中
字符串字面量里的汉字（注释中的不算；`web_page.h` / `wifi_portal_page.h` 属浏览器
渲染文案，已被脚本排除，不会撑大字库）。

## 预览页

`tools/ui_preview.html` 是**可操作的真机模拟器**，浏览器直接打开即可：

```powershell
python -m http.server 8765 --directory tools
```

然后访问 http://localhost:8765/ui_preview.html。

真机每屏只显示 5 行，预览页因此在屏下把全部 31 项按分区逐条列出说明与取值范围，并随
屏内光标实时高亮。顶部快捷按钮中的「默认/IOS/蓝白」等价于修改「界面主题」。
