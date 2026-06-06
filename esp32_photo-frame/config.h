// SPDX-License-Identifier: 0BSD
#pragma once

#include <Arduino.h>
#include "config_defaults.h"

// --------------------------------------------------
// Runtime configuration (persisted in NVS, namespace "cfg")
// --------------------------------------------------
//
// These globals are loaded by configLoad() at boot and updated by the web UI.
// Declared here so every .ino sees them regardless of compile order.
//

extern String   cfgWifiSsid;
extern String   cfgWifiPass;
extern String   cfgMqttHost;
extern uint16_t cfgMqttPort;
extern String   cfgMqttUser;
extern String   cfgMqttPass;

// Current WiFi / power state (set during boot).
extern bool apMode;       // true when hosting our own access point
extern bool serverMode;   // true when on USB power and running the web server

void configLoad();
void configSaveWifi(const String& ssid, const String& pass);
void configSaveMqtt(const String& host, uint16_t port,
                    const String& user, const String& pass);
