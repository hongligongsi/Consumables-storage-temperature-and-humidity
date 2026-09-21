# 第三方软件声明

本项目使用 Arduino 框架，并通过 PlatformIO 管理第三方库。下表依据
`platformio.ini` 与当前 `esp32-s3-n16r8` 环境解析出的依赖整理。

许可证仍以各上游项目随源码发布的许可证文件为准。重新发布源码或固件时，
请保留本声明、本项目的 `LICENSE`，以及对应第三方组件要求保留的许可证与
版权声明。

## 直接依赖

| 组件 | 当前解析版本 | 许可证 | 上游项目 |
| --- | --- | --- | --- |
| Adafruit AHTX0 | 2.0.6 | BSD-3-Clause | [adafruit/Adafruit_AHTX0](https://github.com/adafruit/Adafruit_AHTX0) |
| Adafruit NeoPixel | 1.15.5 | LGPL-3.0-or-later；个别源文件另有其文件头所示许可证 | [adafruit/Adafruit_NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) |
| TFT_eSPI | 2.5.43 | 混合许可证，包含 MIT、BSD 及各字体/贡献代码自身声明；须保留发行包内 `license.txt` | [Bodmer/TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) |
| WiFiManager | 2.0.17 | MIT | [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) |
| PubSubClient | 2.8.0 | MIT | [knolleary/pubsubclient](https://github.com/knolleary/pubsubclient) |

## 间接依赖

| 组件 | 当前解析版本 | 许可证 | 引入来源 | 上游项目 |
| --- | --- | --- | --- | --- |
| Adafruit BusIO | 1.17.4 | MIT | Adafruit AHTX0 / SH110X / GFX | [adafruit/Adafruit_BusIO](https://github.com/adafruit/Adafruit_BusIO) |
| Adafruit Unified Sensor | 1.1.15 | Apache-2.0 | Adafruit AHTX0 | [adafruit/Adafruit_Sensor](https://github.com/adafruit/Adafruit_Sensor) |
| Adafruit SH110X | 2.1.15 | BSD-3-Clause | Adafruit AHTX0 的依赖声明 | [adafruit/Adafruit_SH110X](https://github.com/adafruit/Adafruit_SH110X) |
| Adafruit GFX Library | 1.12.6 | BSD-3-Clause | Adafruit SH110X | [adafruit/Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) |

## Arduino/ESP32 框架组件

固件还会链接 PlatformIO `espressif32@6.7.0` 提供的 Arduino-ESP32 框架组件，
例如 WiFi、WebServer、ESPmDNS、ArduinoOTA、Preferences 与 Wire。它们不是
`platformio.ini` 中单独声明的库，其版权和许可证以对应版本的
[Arduino-ESP32](https://github.com/espressif/arduino-esp32) 发行内容为准。

## 未使用用户清单中的组件

当前源码没有把 LVGL、Arduino-PID-Library、CRC32、ESP32_Knob、
RobTillaart/INA226、OneButton 或 CuteBuzzerSounds 作为第三方依赖：

- 双 PID 控制由本项目源码实现。
- INA226 使用本项目内的 `Ina226Sensor` 驱动实现。
- 屏幕界面直接使用 TFT_eSPI，不使用 LVGL。

如果今后加入或移除依赖，应同步更新本文件，并复核新版本随附的许可证文本。
