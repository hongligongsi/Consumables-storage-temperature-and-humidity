#include "settings.h"
#include <Preferences.h>

namespace {
constexpr const char *NAMESPACE_NAME = "chamber";
void sanitize(SystemSettings &s) {
  s.brightness = constrain(s.brightness, 1, 100);
  s.screenSleepSeconds = min<uint16_t>(s.screenSleepSeconds, 3600);
  s.pirStartSeconds = constrain(s.pirStartSeconds, 1, 300);
  s.pirStopSeconds = constrain(s.pirStopSeconds, 10, 900);
  s.heaterMaxCurrentA = constrain(s.heaterMaxCurrentA, 1, 12);
  s.heaterFanPercent = constrain(s.heaterFanPercent, 20, 100);
  s.heaterBoardLimitC = constrain(s.heaterBoardLimitC, 40, 180);
  s.touchXMin = min<uint16_t>(s.touchXMin, 4095);
  s.touchXMax = min<uint16_t>(s.touchXMax, 4095);
  s.touchYMin = min<uint16_t>(s.touchYMin, 4095);
  s.touchYMax = min<uint16_t>(s.touchYMax, 4095);
  s.mqttPort = constrain(s.mqttPort, 1, 65535);
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

SystemSettings SettingsStore::load() {
  Preferences p;
  SystemSettings s;
  if (!p.begin(NAMESPACE_NAME, true))
    return s;
  s.language =
      p.getBool("english", false) ? Language::English : Language::Chinese;
  s.keySound = p.getBool("keySound", s.keySound);
  s.brightness = p.getUChar("brightness", s.brightness);
  s.screenSleepSeconds = p.getUShort("screenSleep", s.screenSleepSeconds);
  s.keepScreenOnPrinting = p.getBool("keepOnPrint", s.keepScreenOnPrinting);
  s.encoderReversed = p.getBool("encoderRev", s.encoderReversed);
  s.pirStartSeconds = p.getUShort("pirStart", s.pirStartSeconds);
  s.pirStopSeconds = p.getUShort("pirStop", s.pirStopSeconds);
  s.lightOnPrinting = p.getBool("lightStart", s.lightOnPrinting);
  s.lightOffAfterPrinting = p.getBool("lightStop", s.lightOffAfterPrinting);
  s.beepOnStart = p.getBool("beepStart", s.beepOnStart);
  s.beepOnStop = p.getBool("beepStop", s.beepOnStop);
  s.heaterMaxCurrentA = p.getUChar("heaterMaxA", s.heaterMaxCurrentA);
  s.heaterFanPercent = p.getUChar("heaterFan", s.heaterFanPercent);
  s.heaterBoardLimitC = p.getUShort("heaterLimit", s.heaterBoardLimitC);
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
  sanitize(s);
  return s;
}

bool SettingsStore::save(const SystemSettings &input) {
  SystemSettings s = input;
  sanitize(s);
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false))
    return false;
  bool ok = true;
#define PUT_OK(call) ok &= (call) > 0
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

void SettingsStore::loadMaterialProfiles() {
  struct __attribute__((packed)) StoredProfileV1 {
    float minC, maxC;
    uint8_t fanMin, fanMax;
    uint16_t postSeconds;
  };
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
      if (p.getBytes(key, &value, sizeof(value)) != sizeof(value))
        continue;
    } else if (storedSize == sizeof(StoredProfileV1)) {
      StoredProfileV1 old{};
      if (p.getBytes(key, &old, sizeof(old)) != sizeof(old))
        continue;
      value = {old.minC,   old.maxC,   old.fanMin,
               old.fanMax, old.fanMax, old.postSeconds};
    } else {
      continue;
    }
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
    const StoredProfile value{
        MATERIALS[i].chamberMinC,        MATERIALS[i].chamberMaxC,
        MATERIALS[i].fanMinPercent,      MATERIALS[i].fanMaxPercent,
        MATERIALS[i].postExhaustPercent, MATERIALS[i].postExhaustSeconds};
    ok &= p.putBytes(key, &value, sizeof(value)) == sizeof(value);
  }
  p.end();
  return ok;
}

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
  for (const char *key : keys)
    if (p.isKey(key))
      ok &= p.remove(key);
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "mat%02u", static_cast<unsigned>(i));
    if (p.isKey(key))
      ok &= p.remove(key);
  }
  p.end();
  return ok;
}
