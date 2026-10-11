#pragma once
#include <Arduino.h>
#include <Wire.h>

// INA226：主板原理图（2026-01-30，SCH_1-P1）中热板回路的 **R15 = 5 mΩ** 分流
// 电阻，I²C 地址 0x40（A1/A0 均接 GND），默认量程 1 mA/LSB。
//
// ⚠️ Rshunt 必须与实物一致：校准寄存器 = 0.00512 / (Current_LSB × Rshunt)。
// 实物 5 mΩ 却按 10 mΩ 算 → 写进去的校准值只有一半 → 电流读数只有真实值的
// 一半，于是「限流 8 A」实际要 16 A 才动作、硬过流保护也整体延后一倍。
// 改实物分流电阻时只改 kShuntOhms 一处，并在串口 'd' 自检里核对校准值。
class Ina226Sensor {
 public:
  // 板载分流电阻（原理图 R15）与默认电流分辨率,供换算与诊断打印共用。
  static constexpr float kShuntOhms = 0.005f;   // 原理图 R15 = 5 mΩ
  static constexpr float kCurrentLsbA = 0.001f; // 1 mA/LSB

  bool begin(TwoWire &wire, uint8_t address = 0x40,
             float currentLsbA = kCurrentLsbA);
  bool readCurrentA(float &currentA);
  bool readBusVoltageV(float &voltageV);
 private:
  bool write16(uint8_t reg, uint16_t value);
  bool read16(uint8_t reg, uint16_t &value);
  TwoWire *wire_ = nullptr;
  uint8_t address_ = 0x40;
  float currentLsbA_ = kCurrentLsbA;
};
