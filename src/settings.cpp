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

bool SettingsStore::reset() {
  Preferences p;
  if (!p.begin(NAMESPACE_NAME, false)) return false;
  const bool ok = p.clear(); p.end(); return ok;
}
