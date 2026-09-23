// INA226 I²C 电流/电压传感器的薄封装:负责寄存器配置与读数换算,
// 供 main.cpp 读取加热回路电流(过流 PID 限幅)和供电电压(仪表显示)。
#include "ina226_sensor.h"
#include <math.h>

namespace {
constexpr uint8_t REG_CONFIG = 0x00;  // 配置寄存器(模式/转换时间/平均次数)
constexpr uint8_t REG_CURRENT = 0x04; // 电流结果寄存器(有符号,单位 CURRENT_LSB)
constexpr uint8_t REG_BUS_VOLTAGE = 0x02; // 总线电压结果寄存器(LSB 1.25mV)
constexpr uint8_t REG_CALIBRATION = 0x05; // 校准寄存器(决定 CURRENT_LSB)
constexpr float SHUNT_OHMS = 0.01f;       // 板载分流电阻 10mΩ
} // namespace

// 向指定寄存器写入 16bit(大端),返回 I²C 传输是否成功。
bool Ina226Sensor::write16(uint8_t reg, uint16_t value) {
  wire_->beginTransmission(address_);
  wire_->write(reg);
  wire_->write(value >> 8);
  wire_->write(value & 0xff);
  return wire_->endTransmission() == 0;
}

// 从指定寄存器读取 16bit(大端)。endTransmission(false) 用重复起始保持
// 总线占用,紧接 requestFrom 读两字节;任一步失败都返回 false。
bool Ina226Sensor::read16(uint8_t reg, uint16_t &value) {
  wire_->beginTransmission(address_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0 ||
      wire_->requestFrom(address_, static_cast<uint8_t>(2)) != 2)
    return false;
  value = (static_cast<uint16_t>(wire_->read()) << 8) | wire_->read();
  return true;
}

// 绑定 I²C 总线与从机地址,并写入配置/校准值。
// currentLsbA 为电流分辨率(A/bit),由调用方按量程选择。
bool Ina226Sensor::begin(TwoWire &wire, uint8_t address, float currentLsbA) {
  wire_ = &wire;
  address_ = address;
  currentLsbA_ = currentLsbA;
  // 连续采集总线/分流电压，平均 16 次。校准值=0.00512/(LSB×Rshunt)。
  const uint16_t calibration = lroundf(0.00512f / (currentLsbA_ * SHUNT_OHMS));
  return write16(REG_CONFIG, 0x4527) && write16(REG_CALIBRATION, calibration);
}

// 读加热回路电流:电流寄存器为有符号数,乘以 CURRENT_LSB 得安培值。
bool Ina226Sensor::readCurrentA(float &currentA) {
  uint16_t raw;
  if (!read16(REG_CURRENT, raw))
    return false;
  currentA = static_cast<int16_t>(raw) * currentLsbA_;
  return true;
}

// 读总线(供电)电压。
bool Ina226Sensor::readBusVoltageV(float &voltageV) {
  uint16_t raw;
  if (!read16(REG_BUS_VOLTAGE, raw))
    return false;
  voltageV = raw * 0.00125f; // INA226 bus-voltage LSB = 1.25 mV
  return true;
}
