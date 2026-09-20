#include "ina226_sensor.h"
#include <math.h>

namespace {
constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_CURRENT = 0x04;
constexpr uint8_t REG_BUS_VOLTAGE = 0x02;
constexpr uint8_t REG_CALIBRATION = 0x05;
constexpr float SHUNT_OHMS = 0.01f;
}

bool Ina226Sensor::write16(uint8_t reg, uint16_t value) {
  wire_->beginTransmission(address_);
  wire_->write(reg); wire_->write(value >> 8); wire_->write(value & 0xff);
  return wire_->endTransmission() == 0;
}

bool Ina226Sensor::read16(uint8_t reg, uint16_t &value) {
  wire_->beginTransmission(address_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0 || wire_->requestFrom(address_, static_cast<uint8_t>(2)) != 2) return false;
  value = (static_cast<uint16_t>(wire_->read()) << 8) | wire_->read();
  return true;
}

bool Ina226Sensor::begin(TwoWire &wire, uint8_t address, float currentLsbA) {
  wire_ = &wire; address_ = address; currentLsbA_ = currentLsbA;
  // 连续采集总线/分流电压，平均 16 次。校准值=0.00512/(LSB×Rshunt)。
  const uint16_t calibration = lroundf(0.00512f / (currentLsbA_ * SHUNT_OHMS));
  return write16(REG_CONFIG, 0x4527) && write16(REG_CALIBRATION, calibration);
}

bool Ina226Sensor::readCurrentA(float &currentA) {
  uint16_t raw;
  if (!read16(REG_CURRENT, raw)) return false;
  currentA = static_cast<int16_t>(raw) * currentLsbA_;
  return true;
}

bool Ina226Sensor::readBusVoltageV(float &voltageV) {
  uint16_t raw;
  if (!read16(REG_BUS_VOLTAGE, raw)) return false;
  voltageV = raw * 0.00125f; // INA226 bus-voltage LSB = 1.25 mV
  return true;
}
