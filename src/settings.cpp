// 系统设置与耗材预设的持久化层,底层使用 ESP32 NVS(Preferences 封装)。
// 约定:
//  - 所有读出/写入的设置都先经过匿名命名空间内的 sanitize() 做范围钳制;
//  - NVS 键名为短字符串(节省空间),与结构体字段名不要求一致;
//  - load() 在键不存在时直接采用 SystemSettings 的默认值,首次上电即默认配置。
#include "settings.h"
#include <Preferences.h>

namespace {
// NVS 命名空间名;同一固件的全部键都写在这个 namespace 下。
constexpr const char *NAMESPACE_NAME = "chamber";

// 对设置做统一的合法性钳制:load 后、save 前各执行一次,既挡住 NVS 中
// 的脏数据,也挡住 UI/REST 写入的越界值。只改值,不返回错误。
void sanitize(SystemSettings &s) {
  // ---- 界面与人感 ----
  s.brightness = constrain(s.brightness, 1, 100); // 背光 1-100%
  s.screenSleepSeconds =
      min<uint16_t>(s.screenSleepSeconds, 3600); // 休眠最长 1h
  s.pirStartSeconds =
      constrain(s.pirStartSeconds, 1, 300); // 人感启动延时 1-300s
  s.pirStopSeconds =
      constrain(s.pirStopSeconds, 10, 900); // 人感关闭延时 10-900s
  // ---- 加热保护参数 ----
  s.heaterMaxCurrentA = constrain(s.heaterMaxCurrentA, 1, 12); // 限流 1-12A
  s.heaterFanPercent = constrain(s.heaterFanPercent, 20, 100); // 热风扇最低 20%
  s.heaterBoardLimitC =
      constrain(s.heaterBoardLimitC, 40, 180); // 过温阈值 40-180℃
  // ---- 电阻触摸校准值(12bit ADC,0-4095) ----
  s.touchXMin = min<uint16_t>(s.touchXMin, 4095);
  s.touchXMax = min<uint16_t>(s.touchXMax, 4095);
  s.touchYMin = min<uint16_t>(s.touchYMin, 4095);
  s.touchYMax = min<uint16_t>(s.touchYMax, 4095);
  // ---- 联网 ----
  s.mqttPort = constrain(s.mqttPort, 1, 65535); // 端口合法范围
  // 所有字符串字段强制末尾截尾,防止 NVS 读出超长数据时越界。
  s.mqttBroker[sizeof(s.mqttBroker) - 1] = '\0';
  s.mqttTopicPrefix[sizeof(s.mqttTopicPrefix) - 1] = '\0';
  s.timezone[sizeof(s.timezone) - 1] = '\0';
  s.ntpServer1[sizeof(s.ntpServer1) - 1] = '\0';
  s.ntpServer2[sizeof(s.ntpServer2) - 1] = '\0';
  s.staticIp[sizeof(s.staticIp) - 1] = '\0';
  s.staticGateway[sizeof(s.staticGateway) - 1] = '\0';
  s.staticSubnet[sizeof(s.staticSubnet) - 1] = '\0';
  s.staticDns1[sizeof(s.staticDns1) - 1] = '\0';
  s.staticDns2[sizeof(s.staticDns2) - 1] = '\0';
  // 日夜配色与时间:theme 限 0-2,时刻限 0-1425(23:45,与 15 分钟步长对齐)
  s.theme = constrain(s.theme, 0, 2);
  s.dayStartMinutes = min<uint16_t>(s.dayStartMinutes, 1425);
  s.nightStartMinutes = min<uint16_t>(s.nightStartMinutes, 1425);
}
} // namespace

// 从 NVS 读取全部系统设置。begin(readOnly=true) 失败(命名空间尚未创建,
// 常见于首次上电)时直接返回带默认值的 SystemSettings。
// 每个 getXxx 的第二参数都是"键不存在时的默认值",即结构体自身的初值。
SystemSettings SettingsStore::load() {
  Preferences p;
  SystemSettings s;
  if (!p.begin(NAMESPACE_NAME, true))
    return s;
  // 语言在 NVS 中只存"是否英文"一个布尔,映射为枚举。
  s.language =
      p.getBool("english", false) ? Language::English : Language::Chinese;
  // ---- 界面 / 声音 / 编码器 ----
  s.keySound = p.getBool("keySound", s.keySound);
  s.brightness = p.getUChar("brightness", s.brightness);
  s.screenSleepSeconds = p.getUShort("screenSleep", s.screenSleepSeconds);
  s.keepScreenOnPrinting = p.getBool("keepOnPrint", s.keepScreenOnPrinting);
  s.encoderReversed = p.getBool("encoderRev", s.encoderReversed);
  // ---- PIR 人感延时 ----
  s.pirStartSeconds = p.getUShort("pirStart", s.pirStartSeconds);
  s.pirStopSeconds = p.getUShort("pirStop", s.pirStopSeconds);
  // ---- 灯光 / 蜂鸣联动 ----
  s.lightOnPrinting = p.getBool("lightStart", s.lightOnPrinting);
  s.lightOffAfterPrinting = p.getBool("lightStop", s.lightOffAfterPrinting);
  s.beepOnStart = p.getBool("beepStart", s.beepOnStart);
  s.beepOnStop = p.getBool("beepStop", s.beepOnStop);
  // ---- 加热参数 ----
  s.heaterMaxCurrentA = p.getUChar("heaterMaxA", s.heaterMaxCurrentA);
  s.heaterFanPercent = p.getUChar("heaterFan", s.heaterFanPercent);
  s.heaterBoardLimitC = p.getUShort("heaterLimit", s.heaterBoardLimitC);
  // ---- 电阻触摸四点校准值 ----
  s.touchXMin = p.getUShort("touchX0", s.touchXMin);
  s.touchXMax = p.getUShort("touchX1", s.touchXMax);
  s.touchYMin = p.getUShort("touchY0", s.touchYMin);
  s.touchYMax = p.getUShort("touchY1", s.touchYMax);
  s.touchCalibrated = p.getBool("touchCal", s.touchCalibrated);
  // 联网功能
  s.wifiEnabled = p.getBool("wifiEn", s.wifiEnabled);
  s.mqttEnabled = p.getBool("mqttEn", s.mqttEnabled);
  p.getString("mqttBroker", s.mqttBroker, sizeof(s.mqttBroker));
  s.mqttPort = p.getUShort("mqttPort", s.mqttPort);
  p.getString("mqttPrefix", s.mqttTopicPrefix, sizeof(s.mqttTopicPrefix));
  s.ntpEnabled = p.getBool("ntpEn", s.ntpEnabled);
  s.otaEnabled = p.getBool("otaEn", s.otaEnabled);
  p.getString("tz", s.timezone, sizeof(s.timezone));
  p.getString("ntp1", s.ntpServer1, sizeof(s.ntpServer1));
  p.getString("ntp2", s.ntpServer2, sizeof(s.ntpServer2));
  // ---- 静态 IPv4 ----
  s.staticIpEnabled = p.getBool("staticEn", s.staticIpEnabled);
  p.getString("staticIp", s.staticIp, sizeof(s.staticIp));
  p.getString("gateway", s.staticGateway, sizeof(s.staticGateway));
  p.getString("subnet", s.staticSubnet, sizeof(s.staticSubnet));
  p.getString("dns1", s.staticDns1, sizeof(s.staticDns1));
  p.getString("dns2", s.staticDns2, sizeof(s.staticDns2));
  // 日夜配色与时间
  s.theme = p.getUChar("theme", s.theme);
  s.dayStartMinutes = p.getUShort("dayStart", s.dayStartMinutes);
  s.nightStartMinutes = p.getUShort("nightStart", s.nightStartMinutes);
  s.manualClockEpoch = p.getULong("clockEp", s.manualClockEpoch);
  p.end();
  sanitize(s); // 读出后统一钳制,防止历史脏数据越界
  return s;
}

// 把系统设置整体写回 NVS。先 sanitize 再写入,返回值表示是否所有键都
// 写入成功(任一键失败都会累积成 false)。注意 Preferences 的 putXxx 返回
// 写入字节数,空字符串 putString 会返回 0,所以字符串走专用 putText 判定。
bool SettingsStore::save(const SystemSettings &input) {
  SystemSettings s = input;
  sanitize(s);
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false))
    return false;
  bool ok = true;
  // 数值/布尔键:putXxx 返回写入字节数,>0 即视为成功,逐键与进 ok。
#define PUT_OK(call) ok &= (call) > 0
  // 字符串键的成功判定要兼容"空串":有内容时要求写入长度等于串长;
  // 空串时 putString 返回 0,改为检查键确实存在且读回为空。
  auto putText = [&p](const char *key, const char *value) {
    const size_t written = p.putString(key, value);
    return value[0] ? written == strlen(value)
                    : p.isKey(key) && p.getString(key, "__error__") == "";
  };
  PUT_OK(p.putBool("english", s.language == Language::English));
  PUT_OK(p.putBool("keySound", s.keySound));
  PUT_OK(p.putUChar("brightness", s.brightness));
  PUT_OK(p.putUShort("screenSleep", s.screenSleepSeconds));
  PUT_OK(p.putBool("keepOnPrint", s.keepScreenOnPrinting));
  PUT_OK(p.putBool("encoderRev", s.encoderReversed));
  PUT_OK(p.putUShort("pirStart", s.pirStartSeconds));
  PUT_OK(p.putUShort("pirStop", s.pirStopSeconds));
  PUT_OK(p.putBool("lightStart", s.lightOnPrinting));
  PUT_OK(p.putBool("lightStop", s.lightOffAfterPrinting));
  PUT_OK(p.putBool("beepStart", s.beepOnStart));
  PUT_OK(p.putBool("beepStop", s.beepOnStop));
  PUT_OK(p.putUChar("heaterMaxA", s.heaterMaxCurrentA));
  PUT_OK(p.putUChar("heaterFan", s.heaterFanPercent));
  PUT_OK(p.putUShort("heaterLimit", s.heaterBoardLimitC));
  PUT_OK(p.putUShort("touchX0", s.touchXMin));
  PUT_OK(p.putUShort("touchX1", s.touchXMax));
  PUT_OK(p.putUShort("touchY0", s.touchYMin));
  PUT_OK(p.putUShort("touchY1", s.touchYMax));
  PUT_OK(p.putBool("touchCal", s.touchCalibrated));
  // 联网功能
  PUT_OK(p.putBool("wifiEn", s.wifiEnabled));
  PUT_OK(p.putBool("mqttEn", s.mqttEnabled));
  ok &= putText("mqttBroker", s.mqttBroker);
  PUT_OK(p.putUShort("mqttPort", s.mqttPort));
  ok &= putText("mqttPrefix", s.mqttTopicPrefix);
  PUT_OK(p.putBool("ntpEn", s.ntpEnabled));
  PUT_OK(p.putBool("otaEn", s.otaEnabled));
  ok &= putText("tz", s.timezone);
  ok &= putText("ntp1", s.ntpServer1);
  ok &= putText("ntp2", s.ntpServer2);
  // 静态 IPv4
  PUT_OK(p.putBool("staticEn", s.staticIpEnabled));
  ok &= putText("staticIp", s.staticIp);
  ok &= putText("gateway", s.staticGateway);
  ok &= putText("subnet", s.staticSubnet);
  ok &= putText("dns1", s.staticDns1);
  ok &= putText("dns2", s.staticDns2);
  // 日夜配色与时间
  PUT_OK(p.putUChar("theme", s.theme));
  PUT_OK(p.putUShort("dayStart", s.dayStartMinutes));
  PUT_OK(p.putUShort("nightStart", s.nightStartMinutes));
  PUT_OK(p.putULong("clockEp", s.manualClockEpoch));
#undef PUT_OK
  p.end();
  return ok;
}

// 读取全部耗材预设。每种耗材以定长二进制 blob 存在键 "mat00".."matNN"。
// 为兼容老固件保留了两种结构:V1 没有"打印后排风百分比"(postFan),
// 读到 V1 时用 fanMax 兜底填充;既不是 V1 也不是 V2 的脏数据直接跳过。
void SettingsStore::loadMaterialProfiles() {
  // V1 旧布局(早期固件):缺少独立的打印后排风百分比。
  struct __attribute__((packed)) StoredProfileV1 {
    float minC, maxC;
    uint8_t fanMin, fanMax;
    uint16_t postSeconds;
  };
  // V2 当前布局:新增 postFan(打印结束后排风占空比)。
  // packed 保证 NVS 中布局与结构体内存布局逐字节一致。
  struct __attribute__((packed)) StoredProfileV2 {
    float minC, maxC;
    uint8_t fanMin, fanMax, postFan;
    uint16_t postSeconds;
  };
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, true))
    return;
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "mat%02u", static_cast<unsigned>(i));
    StoredProfileV2 value{};
    const size_t storedSize = p.getBytesLength(key);
    if (storedSize == sizeof(StoredProfileV2)) {
      // 当前版本:整块读出,长度不符则放弃这一项。
      if (p.getBytes(key, &value, sizeof(value)) != sizeof(value))
        continue;
    } else if (storedSize == sizeof(StoredProfileV1)) {
      // 旧版本迁移:读出 V1 后,postFan 沿用 fanMax。
      StoredProfileV1 old{};
      if (p.getBytes(key, &old, sizeof(old)) != sizeof(old))
        continue;
      value = {old.minC,   old.maxC,   old.fanMin,
               old.fanMax, old.fanMax, old.postSeconds};
    } else {
      continue; // 键不存在或数据长度未知:保留编译期默认预设
    }
    // 取值范围白名单校验,任何一项越界都整体丢弃,防止脏数据驱动执行器。
    if (isfinite(value.minC) && isfinite(value.maxC) && value.minC >= 0 &&
        value.minC <= 90 && value.maxC >= value.minC + 5 && value.maxC <= 100 &&
        value.fanMin <= value.fanMax && value.fanMax <= 100 &&
        value.postFan <= 100 && value.postSeconds <= 1800) {
      MATERIALS[i].chamberMinC = value.minC;
      MATERIALS[i].chamberMaxC = value.maxC;
      MATERIALS[i].fanMinPercent = value.fanMin;
      MATERIALS[i].fanMaxPercent = value.fanMax;
      MATERIALS[i].postExhaustPercent = value.postFan;
      MATERIALS[i].postExhaustSeconds = value.postSeconds;
    }
  }
  p.end();
}

// 把全部耗材预设按 V2 定长结构写回 NVS,任一项写入失败都累积为 false。
bool SettingsStore::saveMaterialProfiles() {
  struct __attribute__((packed)) StoredProfile {
    float minC, maxC;
    uint8_t fanMin, fanMax, postFan;
    uint16_t postSeconds;
  };
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false))
    return false;
  bool ok = true;
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "mat%02u", static_cast<unsigned>(i));
    // 逐字段组装成 packed blob,保证落盘布局与 StoredProfileV2 相同。
    const StoredProfile value{
        MATERIALS[i].chamberMinC,        MATERIALS[i].chamberMaxC,
        MATERIALS[i].fanMinPercent,      MATERIALS[i].fanMaxPercent,
        MATERIALS[i].postExhaustPercent, MATERIALS[i].postExhaustSeconds};
    ok &= p.putBytes(key, &value, sizeof(value)) == sizeof(value);
  }
  p.end();
  return ok;
}

// 恢复出厂设置:只删除本固件拥有的键(系统设置 + 耗材预设),
// 之后上层会用 SystemSettings 默认值重新运行。
bool SettingsStore::reset() {
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false))
    return false;
  // 只删除本固件拥有的配置键；未知键（例如注册码）必须保留。
  const char *keys[] = {
      "english",    "keySound", "brightness", "screenSleep", "keepOnPrint",
      "encoderRev", "pirStart", "pirStop",    "lightStart",  "lightStop",
      "beepStart",  "beepStop", "heaterMaxA", "heaterFan",   "heaterLimit",
      "touchX0",    "touchX1",  "touchY0",    "touchY1",     "touchCal",
      "wifiEn",     "mqttEn",   "mqttBroker", "mqttPort",    "mqttPrefix",
      "ntpEn",      "otaEn",    "tz",         "ntp1",        "ntp2",
      "staticEn",   "staticIp", "gateway",    "subnet",      "dns1",
      "dns2",       "theme",    "dayStart",   "nightStart",  "clockEp"};
  bool ok = true;
  // isKey 判定后再 remove,键不存在不算错误。
  for (const char *key : keys)
    if (p.isKey(key))
      ok &= p.remove(key);
  // 耗材预设 blob 键随系统设置一并清除。
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "mat%02u", static_cast<unsigned>(i));
    if (p.isKey(key))
      ok &= p.remove(key);
  }
  p.end();
  return ok;
}
