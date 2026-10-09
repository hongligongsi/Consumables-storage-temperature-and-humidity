#pragma once

// 不依赖 Arduino / ESP-IDF 的纯函数集合,由固件与主机端单元测试共用。
// 收录标准只有一条:输入到输出完全确定,不读全局状态、不访问硬件、不做 I/O。
// 符合这条的逻辑放进来,就能被 `pio test -e native` 在桌面端原样复现;
// 不符合的(需要 millis()/NVS/GPIO 的)留在各自的 .cpp 里。
//
// 维护约定:改动本文件必须同步改 test/test_pure_logic 并跑通主机端测试,
// 否则 CI 会红 —— 这几段逻辑此前全靠手工烧录回归,是审计里点名的风险面。

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace pure {

// ------------------------------ 芯片唯一 ID ------------------------------
// 与 NetworkManager::chipId() 共用同一套算法。eFuse MAC 的低 48 位与固定
// 掩码做全宽异或:异或运算在 48 位空间上是双射,故不同 MAC 必得不同 ID,
// 且不可由 ID 直接读出 MAC —— Web 设置接口与 OTA 密码都明文返回它。
constexpr uint64_t kChipIdMask = 0x9E3779B97F4AULL; // 落在 48 位内
constexpr size_t kChipIdLength = 15;                // "CH-" + 12 位十六进制
constexpr size_t kChipIdBufferSize = kChipIdLength + 1; // 含结尾 '\0'

inline uint64_t mixChipMac(uint64_t efuseMac) {
  return (efuseMac & 0xFFFFFFFFFFFFULL) ^ kChipIdMask;
}

// 把 eFuse MAC 渲染成 "CH-XXXXXXXXXXXXXXXX"。
// 返回写入的字符数(不含结尾 '\0');缓冲不足时写入空串并返回 0,
// 绝不部分写入 —— 半个 ID 比没有 ID 更容易误判。
inline size_t formatChipId(uint64_t efuseMac, char *out, size_t size) {
  if (out == nullptr || size == 0)
    return 0;
  if (size < kChipIdBufferSize) {
    out[0] = '\0';
    return 0;
  }
  const int written = std::snprintf(out, size, "CH-%012llX",
                                    (unsigned long long)mixChipMac(efuseMac));
  if (written <= 0 || static_cast<size_t>(written) != kChipIdLength) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

// ------------------------------ 界面主题解析 ------------------------------
// 存储值(settings.theme):0=默认(iOS 浅色) / 1=IOS(深色 HMI) / 2=蓝白。
// 生效值比存储值多一档:3=蓝白·夜间,只在存储值为 2 时由时刻分出。
constexpr uint8_t kThemeAuto = 2;      // 存储值:蓝白,按时刻自动切换
constexpr uint8_t kThemeBlueDay = 2;   // 生效值:蓝白·日间
constexpr uint8_t kThemeBlueNight = 3; // 生效值:蓝白·夜间

// 日/夜时刻的钳制上限:与设置页 15 分钟步长对齐,最大 23:45 = 1425 分钟。
constexpr uint16_t kMinutesOfDayMax = 1425;

inline uint16_t clampMinutesOfDay(uint16_t minutes) {
  return minutes > kMinutesOfDayMax ? kMinutesOfDayMax : minutes;
}

// 分钟数是否落在日间区间 [dayStart, nightStart)。
// dayStart > nightStart 时区间跨零点,取补集;两者相等视为"无日间",
// 恒返回 false(调用方据此按夜间渲染,不产生未定义区间)。
inline bool inDayWindow(uint16_t minutes, uint16_t dayStart,
                        uint16_t nightStart) {
  if (dayStart < nightStart)
    return minutes >= dayStart && minutes < nightStart;
  if (dayStart > nightStart)
    return minutes >= dayStart || minutes < nightStart;
  return false;
}

// 把存储主题解析为当前应当生效的配色。
// rawTheme 非 2 时原样返回 —— 固定配色的两档不分昼夜。
// 时钟无效时按夜间渲染:夜间配色是深底,误判代价远小于日间浅底在夜里刺眼。
inline uint8_t effectiveTheme(uint8_t rawTheme, bool clockValid,
                              uint16_t minutes, uint16_t dayStart,
                              uint16_t nightStart) {
  if (rawTheme != kThemeAuto)
    return rawTheme;
  if (!clockValid)
    return kThemeBlueNight;
  return inDayWindow(minutes, dayStart, nightStart) ? kThemeBlueDay
                                                    : kThemeBlueNight;
}

} // namespace pure
