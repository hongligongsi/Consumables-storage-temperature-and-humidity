#pragma once
#include <Arduino.h>
#include <Wire.h>

// INA226：主板原理图中的 R15=0.01 Ω 分流电阻，默认地址为 0x40。
class Ina226Sensor {
 public:
  bool begin(TwoWire &wire, uint8_t address = 0x40, float currentLsbA = 0.001f);
  bool readCurrentA(float &currentA);
  bool readBusVoltageV(float &voltageV);
 private:
  bool write16(uint8_t reg, uint16_t value);
  bool read16(uint8_t reg, uint16_t &value);
  TwoWire *wire_ = nullptr;
  uint8_t address_ = 0x40;
  float currentLsbA_ = 0.001f;
};
