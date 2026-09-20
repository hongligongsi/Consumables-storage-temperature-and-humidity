#pragma once

// ESP32-S3 N16R8 主控板。引脚名与 SCH_1-P1 原理图中的网络名对应。
// 修改硬件版本时，只需在此文件调整映射。
namespace Pin {
constexpr int TFT_YD       = 4;
constexpr int TFT_XR       = 5;
constexpr int TFT_YU       = 6;
constexpr int TFT_XL       = 7;
constexpr int ENCODER_KEY  = 15;
constexpr int ENCODER_B    = 16;
constexpr int ENCODER_A    = 17;
constexpr int RGB          = 18;

constexpr int TFT_RESET    = 9;
constexpr int TFT_SPI_MISO = 10;
constexpr int TFT_SPI_MOSI = 11;
constexpr int TFT_SPI_SCLK = 12;
constexpr int TFT_DATA_CMD = 13;
constexpr int TFT_CHIP_SELECT = 14;
constexpr int TFT_BACKLIGHT = 21;

constexpr int BUZZER       = 1;
constexpr int I2C_SDA      = 42;
constexpr int I2C_SCL      = 41;
constexpr int HOT_PWM      = 40;
constexpr int AIR_FAN_PWM  = 39;
constexpr int AIR_FAN_DC   = 38;
constexpr int LED_ENABLE   = 37;
constexpr int HOT_FAN      = 36;
constexpr int PIR          = 35;
constexpr int BOARD_FAN    = 48;

constexpr int ADC_NTC      = 3;

// Gerber 飞针网表确认：GPIO8=打印中、GPIO47=加热中。
constexpr bool HAS_STATUS_OUTPUTS = true;
constexpr int PRINTING_STATUS = 8;
constexpr int HEATING_STATUS = 47;
}
