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
}
}

SystemSettings SettingsStore::load() {
  Preferences p;
  SystemSettings s;
  if (!p.begin(NAMESPACE_NAME, true)) return s;
  s.language = p.getBool("english", false) ? Language::English : Language::Chinese;
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
  p.end(); sanitize(s); return s;
}

bool SettingsStore::save(const SystemSettings &input) {
  SystemSettings s = input; sanitize(s);
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false)) return false;
  p.putBool("english", s.language == Language::English);
  p.putBool("keySound", s.keySound); p.putUChar("brightness", s.brightness);
  p.putUShort("screenSleep", s.screenSleepSeconds); p.putBool("keepOnPrint", s.keepScreenOnPrinting);
  p.putBool("encoderRev", s.encoderReversed); p.putUShort("pirStart", s.pirStartSeconds);
  p.putUShort("pirStop", s.pirStopSeconds); p.putBool("lightStart", s.lightOnPrinting);
  p.putBool("lightStop", s.lightOffAfterPrinting); p.putBool("beepStart", s.beepOnStart);
  p.putBool("beepStop", s.beepOnStop); p.putUChar("heaterMaxA", s.heaterMaxCurrentA);
  p.putUChar("heaterFan", s.heaterFanPercent); p.putUShort("heaterLimit", s.heaterBoardLimitC);
  p.putUShort("touchX0", s.touchXMin); p.putUShort("touchX1", s.touchXMax);
  p.putUShort("touchY0", s.touchYMin); p.putUShort("touchY1", s.touchYMax);
  p.putBool("touchCal", s.touchCalibrated);
  // 联网功能
  p.putBool("wifiEn", s.wifiEnabled);
  p.putBool("mqttEn", s.mqttEnabled);
  p.putString("mqttBroker", s.mqttBroker);
  p.putUShort("mqttPort", s.mqttPort);
  p.putString("mqttPrefix", s.mqttTopicPrefix);
  p.putBool("ntpEn", s.ntpEnabled);
  p.putBool("otaEn", s.otaEnabled);
  p.end(); return true;
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
      if (p.getBytes(key, &value, sizeof(value)) != sizeof(value)) continue;
    } else if (storedSize == sizeof(StoredProfileV1)) {
      StoredProfileV1 old{};
      if (p.getBytes(key, &old, sizeof(old)) != sizeof(old)) continue;
      value = {old.minC, old.maxC, old.fanMin, old.fanMax, old.fanMax,
               old.postSeconds};
    } else {
      continue;
    }
    if (isfinite(value.minC) && isfinite(value.maxC) && value.minC >= 0 &&
        value.minC <= 90 && value.maxC >= value.minC + 5 && value.maxC <= 100 &&
        value.fanMin <= value.fanMax && value.fanMax <= 100 &&
        value.postFan <= 100 &&
        value.postSeconds <= 1800) {
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
    const StoredProfile value{MATERIALS[i].chamberMinC, MATERIALS[i].chamberMaxC,
                              MATERIALS[i].fanMinPercent, MATERIALS[i].fanMaxPercent,
                              MATERIALS[i].postExhaustPercent,
                              MATERIALS[i].postExhaustSeconds};
    ok &= p.putBytes(key, &value, sizeof(value)) == sizeof(value);
  }
  p.end();
  return ok;
}

bool SettingsStore::reset() {
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false)) return false;
  // 只删除本固件拥有的配置键；未知键（例如注册码）必须保留。
  const char *keys[] = {
      "english", "keySound", "brightness", "screenSleep", "keepOnPrint",
      "encoderRev", "pirStart", "pirStop", "lightStart", "lightStop",
      "beepStart", "beepStop", "heaterMaxA", "heaterFan", "heaterLimit",
      "touchX0", "touchX1", "touchY0", "touchY1", "touchCal", "wifiEn",
      "mqttEn", "mqttBroker", "mqttPort", "mqttPrefix", "ntpEn", "otaEn"};
  bool ok = true;
  for (const char *key : keys)
    if (p.isKey(key)) ok &= p.remove(key);
  for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "mat%02u", static_cast<unsigned>(i));
    if (p.isKey(key)) ok &= p.remove(key);
  }
  p.end(); return ok;
}
