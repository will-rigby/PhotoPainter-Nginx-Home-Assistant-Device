// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Persistent configuration via Preferences (NVS / flash)
// --------------------------------------------------
//
// On the ESP32-S3 "EEPROM" is emulated on top of NVS; Preferences is the
// idiomatic, wear-levelled way to store key/value config. Until a value is
// written from the web UI, the compile-time defaults in config_defaults.h
// are used as fallbacks (so first boot just works).
//

#include <Preferences.h>
#include "config.h"

static Preferences prefs;

// Definitions for the globals declared extern in config.h
String   cfgWifiSsid;
String   cfgWifiPass;
String   cfgMqttHost;
uint16_t cfgMqttPort;
String   cfgMqttUser;
String   cfgMqttPass;

bool apMode     = false;
bool serverMode = false;

void configLoad() {
  prefs.begin("cfg", true);  // read-only
  cfgWifiSsid = prefs.getString("wifi_ssid", WIFI_SSID);
  cfgWifiPass = prefs.getString("wifi_pass", WIFI_PASSWORD);
  cfgMqttHost = prefs.getString("mqtt_host", MQTT_SERVER);
  cfgMqttPort = prefs.getUShort("mqtt_port", MQTT_PORT);
  cfgMqttUser = prefs.getString("mqtt_user", MQTT_USER);
  cfgMqttPass = prefs.getString("mqtt_pass", MQTT_PASSWORD);
  prefs.end();

  Serial.printf("Config loaded: wifi='%s' mqtt='%s:%u'\n",
                cfgWifiSsid.c_str(), cfgMqttHost.c_str(), cfgMqttPort);
}

void configSaveWifi(const String& ssid, const String& pass) {
  prefs.begin("cfg", false);  // read-write
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.end();
  cfgWifiSsid = ssid;
  cfgWifiPass = pass;
  Serial.printf("WiFi config saved: '%s'\n", ssid.c_str());
}

void configSaveMqtt(const String& host, uint16_t port,
                    const String& user, const String& pass) {
  prefs.begin("cfg", false);
  prefs.putString("mqtt_host", host);
  prefs.putUShort("mqtt_port", port);
  prefs.putString("mqtt_user", user);
  prefs.putString("mqtt_pass", pass);
  prefs.end();
  cfgMqttHost = host;
  cfgMqttPort = port;
  cfgMqttUser = user;
  cfgMqttPass = pass;
  Serial.printf("MQTT config saved: '%s:%u'\n", host.c_str(), port);
}
