# 系统设置逐项说明

设置页共 **27 项**，顺序与屏幕显示一致，枚举定义在
[include/ui_model.h](../include/ui_model.h#L41-L72) 的 `SystemSettingField`。

真机每屏只显示 **5 行**，超出部分靠旋转滚动查看。

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
| 2 | WiFi联网 | WIFI | 开 | 关闭后设备断开网络，Web 与 MQTT 均不可用。**本地关闭后可在本页重新打开**；远程 `POST /api/settings` 传 `wifiEnabled=0` 会被拒绝并返回 400 |
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
| 9 | 打印时保持屏幕开启 | KEEP ON PRINTING | 开 / 关 | — |
| 10 | 编码器方向 | ENCODER DIRECTION | 正向 / 反向 | — |
| 11 | PIR启动延时 | PIR START DELAY | 1–300 s | 1 |
| 12 | PIR关闭延时 | PIR STOP DELAY | 10–900 s | 5 |
| 13 | 启动后自动开灯 | LIGHT ON START | 开 / 关 | — |
| 14 | 关闭后自动关灯 | LIGHT OFF STOP | 开 / 关 | — |
| 15 | 启动后蜂鸣提示 | BEEP ON START | 开 / 关 | — |
| 16 | 关闭后蜂鸣提示 | BEEP ON STOP | 开 / 关 | — |
| 17 | 发热板限流 | HEATER CURRENT | 1–12 A | 1 |
| 18 | 发热板风扇风速 | HEATER FAN | 20–100 % | 5 |
| 19 | 发热板温度保护 | HEATER PROTECTION | 40–180 °C | 1 |
| 20 | 屏幕配色 | THEME | 日间 / 夜间 / 自动 | — |
| 21 | 日间开始时刻 | DAY START | 00:00–23:45 | 15 min |
| 22 | 夜间开始时刻 | NIGHT START | 00:00–23:45 | 15 min |
| 23 | 日期 | DATE | 年 / 月 / 日 三段 | 见下 |
| 24 | 时间 | TIME | 时 / 分 两段 | 见下 |
| 25 | 触摸屏校准 | TOUCH CALIBRATION | 不支持 / 未校准 / 已校准 | — |
| 26 | 恢复出厂配置 | FACTORY RESET | 执行 / 确认? | — |
| 27 | 固件版本 | FIRMWARE VERSION | 只读 | — |

取值范围由 [src/ui_model.cpp](../src/ui_model.cpp#L8-L99) 的 `adjustSystemSetting`
限制，并在 [src/settings.cpp](../src/settings.cpp#L6-L25) 的 `sanitize()` 中于载入
和保存时二次夹取，即使 NVS 被写坏也不会越界。

## 重点条目说明

### 屏幕休眠时间

仅关闭背光，**系统继续运行**。设为 0 表示不休眠。若「打印时保持屏幕开启」为开，
则打印机工作时忽略此项。

### PIR 启动/关闭延时

PIR 判定打印机工作状态：

- 检测到运动持续 `PIR启动延时` 秒 → 判定开始打印，启动仓温控制
- 连续 `PIR关闭延时` 秒无运动 → 判定打印停止

默认 50 s。多色打印换色时 PIR 可能长时间无动作，建议调到 90 s 以免误判停止。

### 发热板温度保护

热板温度超过此值即暂停加热。这是软件层的第二道防线，硬件过温保护独立于此，
不可互相替代。

### 屏幕配色与日/夜时刻

「自动」模式下按两个时刻切换：到达「日间开始时刻」切日间配色，「夜间开始时刻」
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

### 恢复出厂配置

需二次确认（显示「确认?」）才执行。**只删除本固件拥有的设置键与耗材键**，未知键
会保留，例如预留的注册码数据不会丢失。删除后立即重启。

### 固件版本

只读，取值来自 [include/version.h](../include/version.h) 的 `FW_VERSION`，
当前为 `1.0.0`。可用 build_flags 追加 `-DFW_VERSION=\"x.y.z\"` 覆盖，无需改源码。

## 新增设置项的改法

设置页是枚举驱动的，新增一项只需同步 **4 处**：

1. [include/ui_model.h](../include/ui_model.h#L41-L72) — 在 `SystemSettingField`
   中按显示顺序插入枚举项（放在 `Version` 之前）
2. [src/ui_model.cpp](../src/ui_model.cpp#L8-L99) — 在 `adjustSystemSetting` 的
   switch 中加 case；数值类用 `constrain(..., min, max)`，开关类直接取反
3. [src/tft_ui.cpp](../src/tft_ui.cpp#L390-L584) — 在 `zh[]` / `en[]` 数组的
   **同一位置**插入文案，并在 value 格式化 switch 中加对应 case
4. [tools/ui_preview.html](../tools/ui_preview.html#L597-L733) — 在 `F` 数组同一
   位置插入元数据（`zh`/`en`/`type`/`key`/`desc`/`range`）

若新条目是**数值类**且要支持单击进入编辑态，还需在 [src/ui_model.cpp](../src/ui_model.cpp#L229-L238)
的 `numeric` 白名单中加入它；开关类不需要。

新增中文文案若用到字库外的汉字，需重跑 `python tools/genvlw.py` 重新生成
`include/font_cn16.h` / `font_cn26.h`。该脚本会自动扫描 `src/` 与 `include/` 中
字符串字面量里的汉字（注释中的不算）。

## 预览页

`tools/ui_preview.html` 是**可操作的真机模拟器**，浏览器直接打开即可：

```powershell
python -m http.server 8765 --directory tools
```

然后访问 http://localhost:8765/ui_preview.html。

真机每屏只显示 5 行，预览页因此在屏下把全部 27 项逐条列出说明与取值范围，并随
屏内光标实时高亮。顶部快捷按钮中的「夜间/日间/自动」等价于修改「屏幕配色」。
