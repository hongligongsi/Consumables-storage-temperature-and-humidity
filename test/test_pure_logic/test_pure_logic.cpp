// 主机端单元测试:只覆盖 include/pure_logic.h 里的纯逻辑。
// 运行方式:`pio test -e native`(Windows 上由 tools/run_host_tests.py 补编译器路径)。
//
// 这几段逻辑此前没有任何自动化回归 —— 芯片 ID 决定 OTA 密码、主题解析决定
// 整屏配色,改错只能靠烧录后肉眼发现。测试里同时钉住"边界值"和"结构性质"
// (双射、首尾闭合),这样将来重写实现也能被拦住,而不仅是比对当下的输出。

#include "pure_logic.h"
#include <unity.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

void setUp(void) {}
void tearDown(void) {}

// ------------------------------ 芯片唯一 ID ------------------------------

// 固定向量:掩码与格式化格式一旦改动,OTA 密码会静默变化,必须显式确认。
void test_chip_id_known_vectors(void) {
  char buf[pure::kChipIdBufferSize];

  TEST_ASSERT_EQUAL_size_t(15, pure::formatChipId(0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("CH-9E3779B97F4A", buf);

  TEST_ASSERT_EQUAL_size_t(15,
                           pure::formatChipId(0xFFFFFFFFFFFFULL, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("CH-61C8864680B5", buf);
}

// 高位(48 位以外)必须被掩掉:同一个 MAC 无论高位怎么变,ID 都应一致。
void test_chip_id_masks_upper_bits(void) {
  char plain[pure::kChipIdBufferSize];
  char dirty[pure::kChipIdBufferSize];

  // 两者低 48 位相同(0x001122334455),只有第 49 位往上不同(AABB 那 16 位)。
  pure::formatChipId(0x001122334455ULL, plain, sizeof(plain));
  pure::formatChipId(0xAABB001122334455ULL, dirty, sizeof(dirty));
  TEST_ASSERT_EQUAL_STRING(plain, dirty);

  // 只有高位、低 48 位全 0 → 与 MAC=0 等价。
  TEST_ASSERT_EQUAL_UINT64(pure::mixChipMac(0),
                           pure::mixChipMac(0xFF00000000000000ULL));
}

// 结构性质:异或掩码必须构成双射,否则会出现两台设备同 ID(OTA 密码撞车)。
// 用低 16 位全枚举 + 若干极值点采样,既证明不同 MAC 不碰撞,也证明可逆。
void test_chip_id_is_bijective_on_sample(void) {
  std::vector<uint64_t> ids;
  ids.reserve(1 << 16);

  for (uint64_t mac = 0; mac < (1ULL << 16); ++mac) {
    const uint64_t mixed = pure::mixChipMac(mac);
    // 异或自反:再用同一个掩码洗一次必须还原出原 MAC。
    TEST_ASSERT_EQUAL_UINT64(mac, pure::mixChipMac(mixed));
    ids.push_back(mixed);
  }
  ids.push_back(pure::mixChipMac(0x800000000000ULL));
  ids.push_back(pure::mixChipMac(0xFFFFFFFFFFFFULL));

  std::vector<uint64_t> sorted(ids);
  std::sort(sorted.begin(), sorted.end());
  TEST_ASSERT_EQUAL_size_t(ids.size(),
                           static_cast<size_t>(std::unique(sorted.begin(), sorted.end()) -
                                               sorted.begin()));
}

// 形状约束:长度恒为 15、前缀 "CH-"、后 12 位必须是大写十六进制。
void test_chip_id_shape(void) {
  const uint64_t samples[] = {0ULL, 1ULL, 0x123456789ABCULL, 0xFFFFFFFFFFFFULL};
  for (uint64_t mac : samples) {
    char buf[pure::kChipIdBufferSize];
    TEST_ASSERT_EQUAL_size_t(15, pure::formatChipId(mac, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("CH-", buf, 3);
    for (size_t i = 3; i < pure::kChipIdLength; ++i)
      TEST_ASSERT_TRUE((buf[i] >= '0' && buf[i] <= '9') ||
                       (buf[i] >= 'A' && buf[i] <= 'F'));
  }
}

// 缓冲不足时必须"全有或全无":写空串并返回 0,不得截断,也不得越界。
void test_chip_id_rejects_short_buffer(void) {
  char buf[8];
  buf[0] = 'X'; // 哨兵:验证函数确实写了终止符而不是原样留着
  TEST_ASSERT_EQUAL_size_t(0, pure::formatChipId(0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);

  TEST_ASSERT_EQUAL_size_t(0, pure::formatChipId(0, buf, 0));
  TEST_ASSERT_EQUAL_size_t(0, pure::formatChipId(0, nullptr, 32));
}

// ------------------------------ 主题解析 ------------------------------

// 固定配色(0/1)不分昼夜,时钟是否有效都不影响。
void test_theme_fixed_schemes_pass_through(void) {
  TEST_ASSERT_EQUAL_UINT8(0, pure::effectiveTheme(0, true, 720, 360, 1080));
  TEST_ASSERT_EQUAL_UINT8(0, pure::effectiveTheme(0, false, 0, 360, 1080));
  TEST_ASSERT_EQUAL_UINT8(1, pure::effectiveTheme(1, true, 720, 360, 1080));
  TEST_ASSERT_EQUAL_UINT8(1, pure::effectiveTheme(1, false, 0, 360, 1080));
}

// 自动档 + 时钟无效 → 按夜间渲染(深底),这是刻意选择的失败方向。
void test_theme_auto_without_clock_falls_back_to_night(void) {
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, false, 0, 360, 1080));
}

// 不跨零点的区间 [06:00, 18:00) 逐边界检查:左闭右开。
void test_theme_day_window_closed_open(void) {
  const uint16_t day = 6 * 60;    // 360
  const uint16_t night = 18 * 60; // 1080

  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, 0, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, day - 1, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, day, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, night - 1, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, night, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, 1439, day, night));
}

// 跨零点:日间区间从 18:00 绕到次日 06:00,补集才是夜间。
void test_theme_day_window_wraps_midnight(void) {
  const uint16_t day = 18 * 60; // 1080
  const uint16_t night = 6 * 60; // 360

  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, 0, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, night - 1, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, night, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                          pure::effectiveTheme(pure::kThemeAuto, true, day - 1, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, day, day, night));
  TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueDay,
                          pure::effectiveTheme(pure::kThemeAuto, true, 1439, day, night));
}

// 日夜时刻相等时不存在日间区间,一律按夜间,不能出现"永远日间"的死区。
void test_theme_equal_bounds_yield_no_day(void) {
  for (uint16_t minutes = 0; minutes <= 1439; minutes += 137)
    TEST_ASSERT_EQUAL_UINT8(pure::kThemeBlueNight,
                            pure::effectiveTheme(pure::kThemeAuto, true, minutes, 0, 0));
}

// 15 分钟步长上限:23:45 合法,再往上钳到 23:45,不能包成 0 点。
void test_clamp_minutes_of_day(void) {
  TEST_ASSERT_EQUAL_UINT16(0, pure::clampMinutesOfDay(0));
  TEST_ASSERT_EQUAL_UINT16(1425, pure::clampMinutesOfDay(1425));
  TEST_ASSERT_EQUAL_UINT16(1425, pure::clampMinutesOfDay(1426));
  TEST_ASSERT_EQUAL_UINT16(1425, pure::clampMinutesOfDay(1440)); // 次日 00:00
  TEST_ASSERT_EQUAL_UINT16(1425, pure::clampMinutesOfDay(65535));
}

// ------------------------------ WiFi 信号格数 ------------------------------

// 边界取等号(≥ -55 才算 4 格),与网页 setBars 的写法一致。
void test_rssi_bars_thresholds(void) {
  TEST_ASSERT_EQUAL_UINT8(4, pure::rssiBars(-40));
  TEST_ASSERT_EQUAL_UINT8(4, pure::rssiBars(-55));
  TEST_ASSERT_EQUAL_UINT8(3, pure::rssiBars(-56));
  TEST_ASSERT_EQUAL_UINT8(3, pure::rssiBars(-65));
  TEST_ASSERT_EQUAL_UINT8(2, pure::rssiBars(-66));
  TEST_ASSERT_EQUAL_UINT8(2, pure::rssiBars(-75));
  TEST_ASSERT_EQUAL_UINT8(1, pure::rssiBars(-76));
  TEST_ASSERT_EQUAL_UINT8(1, pure::rssiBars(-100)); // 极弱也留一格,与断网区分
}

// 无信号哨兵必须落 0 格:0 dBm 不会出现,正值属非法输入,同样不能画成满格。
void test_rssi_unknown_is_zero_bars(void) {
  TEST_ASSERT_EQUAL_UINT8(0, pure::rssiBars(pure::kRssiUnknown));
  TEST_ASSERT_EQUAL_UINT8(0, pure::rssiBars(1));
  TEST_ASSERT_EQUAL_UINT8(0, pure::rssiBars(127));
}

// 结构性质:信号越强,格数只能单调不减。将来若有人把阈值改反(例如强弱颠倒),
// 光比几个固定向量看不出来,这条会立刻红。
void test_rssi_bars_monotonic(void) {
  uint8_t previous = 0;
  for (int rssi = -100; rssi <= -30; ++rssi) {
    const uint8_t bars = pure::rssiBars(rssi);
    TEST_ASSERT_TRUE(bars >= previous);
    TEST_ASSERT_TRUE(bars <= 4);
    previous = bars;
  }
}

// ------------------------------ 注册码派生 ------------------------------

// 固定向量:由厂商端 Python 参考实现预计算(同一 FNV-1a(64) + 固定盐,
// 见 docs/settings.md)。盐或算法一旦改动,存量设备的注册码会全部失效,
// 必须显式确认。
void test_reg_code_known_vectors(void) {
  char buf[pure::kRegCodeBufferSize];

  pure::regCodeFromChipId("CH-1A2B3C4D5E6F", buf);
  TEST_ASSERT_EQUAL_STRING("02812C46", buf);

  pure::regCodeFromChipId("CH-000000000000", buf);
  TEST_ASSERT_EQUAL_STRING("59BB71B4", buf);

  pure::regCodeFromChipId("CH-FFFFFFFFFFFF", buf);
  TEST_ASSERT_EQUAL_STRING("25D03EB4", buf);
}

// 结构性质:输出恒为 8 位大写十六进制(屏上键盘只有 0-9/A-F 十六个键,
// 若某天算法产出其它字符,注册页将永远无法输入)。
void test_reg_code_shape(void) {
  for (uint64_t mac = 0; mac < 256; ++mac) {
    char chip[pure::kChipIdBufferSize];
    char code[pure::kRegCodeBufferSize];
    pure::formatChipId(mac, chip, sizeof(chip));
    pure::regCodeFromChipId(chip, code);
    TEST_ASSERT_EQUAL_size_t(pure::kRegCodeLength, strlen(code));
    for (size_t i = 0; code[i]; ++i)
      TEST_ASSERT_TRUE((code[i] >= '0' && code[i] <= '9') ||
                       (code[i] >= 'A' && code[i] <= 'F'));
  }
}

// 空指针防御:chipId 为 nullptr 不崩溃(视为只对盐哈希),out 为 nullptr 直接返回。
void test_reg_code_null_safety(void) {
  char code[pure::kRegCodeBufferSize] = "zzzzzzzz";
  pure::regCodeFromChipId(nullptr, code);
  TEST_ASSERT_EQUAL_size_t(pure::kRegCodeLength, strlen(code));
  pure::regCodeFromChipId("CH-1A2B3C4D5E6F", nullptr); // 不得崩溃
}

// ------------------------------ 入口 ------------------------------

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_chip_id_known_vectors);
  RUN_TEST(test_chip_id_masks_upper_bits);
  RUN_TEST(test_chip_id_is_bijective_on_sample);
  RUN_TEST(test_chip_id_shape);
  RUN_TEST(test_chip_id_rejects_short_buffer);
  RUN_TEST(test_theme_fixed_schemes_pass_through);
  RUN_TEST(test_theme_auto_without_clock_falls_back_to_night);
  RUN_TEST(test_theme_day_window_closed_open);
  RUN_TEST(test_theme_day_window_wraps_midnight);
  RUN_TEST(test_theme_equal_bounds_yield_no_day);
  RUN_TEST(test_clamp_minutes_of_day);
  RUN_TEST(test_rssi_bars_thresholds);
  RUN_TEST(test_rssi_unknown_is_zero_bars);
  RUN_TEST(test_rssi_bars_monotonic);
  RUN_TEST(test_reg_code_known_vectors);
  RUN_TEST(test_reg_code_shape);
  RUN_TEST(test_reg_code_null_safety);
  return UNITY_END();
}
