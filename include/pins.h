#pragma once

// ESP32-S3 N16R8 主控板。引脚名与 SCH_1-P1 原理图中的网络名对应。
// 修改硬件版本时,只需在此文件调整映射。
//
// ==== 2026-09-22 校正记录 ====
// 对照最新原理图(mainboard-sch-p1.png)发现多处错位,原因是早期版本引脚号
// 推断有误。按实物 N16R8 + 原理图重新核:
//   GPIO3 = PIR(红外感应)     之前误写为 ADC_NTC
//   GPIO2 = AIR_FAN_PWM(4线风扇PWM) 之前误写为 GPIO39
//   GPIO35 = AIR_FAN_DC(风扇电源MOS)  之前误写为 PIR
//   GPIO38 = HOT_PWM(发热板MPT40N08S) 之前误写为 AIR_FAN_DC
//   GPIO39 = ADC_NTC(NTC热敏采样)      之前误写为 AIR_FAN_PWM
//   GPIO40 = ADC_VCC(电压分压/INA226 Vin+) 之前误写为 HOT_PWM(现独立)
//   GPIO45 = BOARD_FAN(主板散热风扇MOS) 之前误写为 48(悬空)

namespace Pin {
// 无触摸显示屏保持 false;更换为四线电阻触摸版本并完成校准后改为 true。
constexpr bool HAS_TOUCH_PANEL = false;

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
constexpr int AIR_FAN_PWM = 2; // 4 线 PWM 调速(CN6)
constexpr int AIR_FAN_DC = 35; // 风扇电源 MOS Q4 栅极
constexpr int HOT_FAN = 36;    // 加热风扇 MOS Q5 栅极
constexpr int BOARD_FAN = 45;  // 主板散热风扇 MOS Q7 栅极

// ---- 加热 ----
constexpr int HOT_PWM = 38; // 发热板 MPT40N08S MOS 栅极

// ---- 传感器 ----
// 注意: GPIO3 也是 strapping 脚,但默认浮空、不参与启动(Strap JTAG 选择需先烧
// STRAP_JTAG_SEL eFuse 才生效),接 PIR 输出无冲突。
constexpr int PIR = 3;      // 红外感应模块输出(CN3)
constexpr int ADC_NTC = 39; // 发热板 NTC 热敏电阻采样(ZX-NTC1.25-P2ZZ)
constexpr int ADC_VCC = 40; // 电压分压采样(INA226 Vin+)

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
