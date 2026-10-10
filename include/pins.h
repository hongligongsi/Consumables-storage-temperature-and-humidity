#pragma once

// ESP32-S3 N16R8 主控板。引脚名与 SCH_1-P1 原理图(2026-01-30)中的网络名对应。
// 修改硬件版本时,只需在此文件调整映射。
//
// ==== 2026-09-29 复核(对照 2026-01-30 原理图 + 实机日志) ====
// 2026-09-22 那次「校正」是按 ESP32 **classic** 的脚位习惯误改的:S3 的 ADC
// 只有 GPIO1~10(ADC1)/GPIO11~20(ADC2,与 WiFi 冲突),GPIO35~40 是纯数字 IO。
// 实机日志每 500ms 报 "Pin 39 is not ADC pin!"、温度 nan,实锤 ADC_NTC=39 非法。
// 本次按原理图网络名逐一回滚:
//   GPIO35 = PIR          (IO35→PIR, pin28)
//   GPIO39 = AIR_FAN_PWM  (IO39→AIR_FAN_PWM, pin32)
//   GPIO38 = AIR_FAN_DC   (IO38→AIR_FAN_DC, pin31)
//   GPIO40 = HOT_PWM      (IO40→HOT_PWM, pin33)
//   GPIO3  = ADC_NTC      (IO3→ADC_NTC, pin15, ADC1_CH2)
//   GPIO2  = ADC_VCC      (IO2→ADC_VCC, pin38, ADC1_CH1, 24V 分压)
//   GPIO45 = BOARD_FAN    保留(模组下方注释明确写「使用IO45时确保上电时外部
//                          电路不会将 IO45 拉高」,即设计者已知并接受该 strapping)
// 注意:IO35/36/37 在 OPI PSRAM 模组上被 PSRAM 占用;本板 PSRAM 已禁用
// (platformio.ini 换 qio_qspi),故可用。若恢复 PSRAM,这三脚必须换。

namespace Pin {
// 硬件是否具备四线电阻触摸(默认 true:TFT 排针已引出 X+/X-/Y+/Y- 四线)。
// 这只是编译期“硬件能力”闸,真正是否启用由系统设置「触摸模式」(开/关/自动)
// 运行时决定,见 main.cpp 的 touchEnabledNow():
//   自动(出厂默认)= 开机做电阻膜在位探测(detectTouchPanel),探测不到膜
//                   (混用纯显示屏/排线未接)时整路关闭,防浮空线幽灵触摸;
//   开 = 强制启用;关 = 强制关闭只用编码器。
// 仅当为完全无触摸走线的精简硬件版本编译时才把此常量改为 false。
constexpr bool HAS_TOUCH_PANEL = true;

// ---- TFT 屏(ST7796, 480x320, SPI + 可选四线电阻触摸) ----
constexpr int TFT_YD = 4; // 触摸 Y 下
constexpr int TFT_XR = 5; // 触摸 X 右
constexpr int TFT_YU = 6; // 触摸 Y 上
constexpr int TFT_XL = 7; // 触摸 X 左
constexpr int TFT_RESET = 9;
constexpr int TFT_SPI_MISO = 10;
constexpr int TFT_SPI_MOSI = 11;
constexpr int TFT_SPI_SCLK = 12;
constexpr int TFT_DATA_CMD = 13; // 也叫 A0
constexpr int TFT_CHIP_SELECT = 14;
constexpr int TFT_BACKLIGHT = 21; // 同时驱动 TFT 背光 + LED 使能

// ---- 旋转编码器(EC11) ----
constexpr int ENCODER_KEY = 15; // 中键(EC_D)
constexpr int ENCODER_B = 16;   // B 相(EC_B)
constexpr int ENCODER_A = 17;   // A 相(EC_A)

// ---- RGB / WS2812B LED 灯带 ----
constexpr int RGB = 18;        // DIN
constexpr int LED_ENABLE = 37; // LED 灯带 MOS 管栅极

// ---- 蜂鸣器(TF0405-1-4P) ----
constexpr int BUZZER = 1;

// ---- I²C 总线 ----
// 同时挂: 温湿度传感器 AHT20 + INA226 电压/电流采样
constexpr int I2C_SDA = 42;
constexpr int I2C_SCL = 41;

// ---- 风扇 ----
constexpr int AIR_FAN_PWM = 39; // 4 线 PWM 调速(CN6)
constexpr int AIR_FAN_DC = 38;  // 风扇电源 MOS Q4 栅极
constexpr int HOT_FAN = 36;     // 加热风扇 MOS Q5 栅极
constexpr int BOARD_FAN = 45;   // 主板散热风扇 MOS Q7 栅极(strapping,见上)

// ---- 加热 ----
constexpr int HOT_PWM = 40; // 发热板 MPT40N08S MOS 栅极

// ---- 传感器 ----
// 注意: GPIO3 也是 strapping 脚,但默认浮空、不参与启动(Strap JTAG 选择需先烧
// STRAP_JTAG_SEL eFuse 才生效),接 PIR 输出无冲突。
constexpr int ADC_NTC = 3;  // 发热板 NTC 热敏电阻采样(ZX-NTC1.25-P2ZZ),ADC1_CH2
constexpr int ADC_VCC = 2;  // 电压分压采样(INA226 Vin+ 前置),ADC1_CH1。
                            // 该脚为 strapping(上电需为高):24V 分压网络静态
                            // ≈2.2V,恰为高电平,不影响启动。
constexpr int PIR = 35;     // 红外感应模块输出(CN3)

// ---- H2 / H3 扩展排针 ----
// Gerber 飞针网表 + 原理图确认:
constexpr bool HAS_STATUS_OUTPUTS = true;
constexpr int PRINTING_STATUS = 8; // GPIO8 = 打印中
constexpr int HEATING_STATUS = 47; // GPIO47 = 加热中

// ---- 未分配 ----
// GPIO0  (BOOT 按钮,strapping,默认弱上拉,按低时进入下载模式)
// GPIO19 (USB D-,USB 专用,别作 GPIO)
// GPIO20 (USB D+,USB 专用,别作 GPIO)
// GPIO33/34/43/44 — N16R8 LGA-33 封装未引出到 PCB
// GPIO46 (strapping,默认弱下拉。与 GPIO0 共同决定启动模式,同时控制 ROM
//         启动日志是否打印;默认低=打印。保持默认即可)
// GPIO48 (模组板载 WS2812 LED,LGA-33 封装没对外引出)
// USB-UART 走内置 USB-SERIAL-JTAG bridge(UART0),无需占用 43/44
//
// ---- Strapping 注意 ----
// GPIO45 已分配给 BOARD_FAN,但它同时是 strapping 脚,上电电平选择 VDD_SPI:
//   低(默认,内部弱下拉) = 3.3V flash;高 = 1.8V flash。
// 本板为模组内置 flash,必须走 3.3V,故上电瞬间 GPIO45 必须为低。
// 若 MOS Q7 栅极把这脚拉高,芯片会按 1.8V 配置 VDD_SPI 而启动失败。
// 详见 docs/hardware.md「上电前必须复核」第 5 项。
} // namespace Pin
