// SPDX-License-Identifier: 0BSD
#include <Preferences.h>
#include "config.h"
#include "policies.h"
#include "transactions.h"

String cfgWifiSsid, cfgWifiPass, cfgMqttHost, cfgMqttUser, cfgMqttPass;
String cfgHostname = "photoframe";
uint16_t cfgMqttPort = 1883;
bool apMode = false;
bool serverMode = false;
static uint32_t mqttConfigRevision = 0;

struct WifiRecord {
  uint32_t version;
  char ssid[33], password[65], hostname[64];
};
struct MqttRecord {
  uint32_t version;
  char host[254], user[129], password[257];
  uint16_t port;
};

void configLoad() {
  Preferences prefs;
  prefs.begin("cfg", true);
  cfgWifiSsid = prefs.getString("wifi_ssid", WIFI_SSID);
  cfgWifiPass = prefs.getString("wifi_pass", WIFI_PASSWORD);
  cfgMqttHost = prefs.getString("mqtt_host", MQTT_SERVER);
  cfgMqttPort = prefs.getUShort("mqtt_port", MQTT_PORT);
  cfgMqttUser = prefs.getString("mqtt_user", MQTT_USER);
  cfgMqttPass = prefs.getString("mqtt_pass", MQTT_PASSWORD);
  WifiRecord wifi{};
  MqttRecord broker{};
  if (prefs.getBytesLength("wifi_v2") == sizeof(wifi) &&
      prefs.getBytes("wifi_v2", &wifi, sizeof(wifi)) == sizeof(wifi) && wifi.version == 2 &&
      wifi.ssid[32] == 0 && wifi.password[64] == 0 && wifi.hostname[63] == 0) {
    cfgWifiSsid = wifi.ssid; cfgWifiPass = wifi.password; cfgHostname = wifi.hostname;
  }
  if (prefs.getBytesLength("mqtt_v2") == sizeof(broker) &&
      prefs.getBytes("mqtt_v2", &broker, sizeof(broker)) == sizeof(broker) && broker.version == 2 &&
      broker.host[253] == 0 && broker.user[128] == 0 && broker.password[256] == 0 && broker.port) {
    cfgMqttHost = broker.host; cfgMqttUser = broker.user;
    cfgMqttPass = broker.password; cfgMqttPort = broker.port;
  }
  prefs.end();
}

ConfigSnapshot configSnapshot() {
  MutexGuard guard(configMutex);
  ConfigSnapshot c;
  c.ssid = cfgWifiSsid; c.wifiPass = cfgWifiPass; c.hostname = cfgHostname;
  c.host = cfgMqttHost; c.port = cfgMqttPort; c.user = cfgMqttUser;
  c.mqttPass = cfgMqttPass; c.mqttRevision = mqttConfigRevision;
  return c;
}
uint32_t configMqttRevision() { MutexGuard guard(configMutex); return mqttConfigRevision; }

bool configSaveWifi(const String& ssid, const String& pass, bool clear,
                    const String& hostname, String& error) {
  if (ssid.length() > 32 || pass.length() > 64 || !validHostname(hostname.c_str()) ||
      (clear && pass.length())) { error = "Invalid WiFi settings"; return false; }
  MutexGuard guard(configMutex);
  WifiRecord r{}; r.version = 2;
  strlcpy(r.ssid, ssid.c_str(), sizeof(r.ssid));
  strlcpy(r.password, savedPassword(cfgWifiPass.c_str(), pass.c_str(), clear), sizeof(r.password));
  strlcpy(r.hostname, hostname.c_str(), sizeof(r.hostname));
  Preferences prefs;
  bool ok = persistSettings(prefs, "wifi_v2", r);
  if (!ok) { error = "Could not persist WiFi settings"; return false; }
  cfgWifiSsid = r.ssid; cfgWifiPass = r.password; cfgHostname = r.hostname;
  return true;
}

bool configSaveMqtt(const String& host, const String& portText, const String& user,
                    const String& pass, bool clear, String& error) {
  uint16_t port;
  if (!parsePort(portText.c_str(), port) || host.length() > 253 || user.length() > 128 ||
      pass.length() > 256 || (clear && pass.length())) {
    error = "Invalid MQTT settings (port must be 1-65535)"; return false;
  }
  MutexGuard guard(configMutex);
  MqttRecord r{}; r.version = 2; r.port = port;
  strlcpy(r.host, host.c_str(), sizeof(r.host)); strlcpy(r.user, user.c_str(), sizeof(r.user));
  strlcpy(r.password, savedPassword(cfgMqttPass.c_str(), pass.c_str(), clear), sizeof(r.password));
  Preferences prefs;
  bool ok = persistSettings(prefs, "mqtt_v2", r);
  if (!ok) { error = "Could not persist MQTT settings"; return false; }
  cfgMqttHost = r.host; cfgMqttUser = r.user; cfgMqttPass = r.password;
  cfgMqttPort = port; ++mqttConfigRevision;
  return true;
}
