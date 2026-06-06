// SPDX-License-Identifier: 0BSD
#pragma once

#include <Arduino.h>
#include "config_defaults.h"

// --------------------------------------------------
// SD card folders
// --------------------------------------------------
//   /originals/<base>.<ext>  uploaded source images (gallery source)
//   /dithered/<base>.bin     processed 800x480 4bpp panel buffers (display source)
#define ORIGINALS_DIR "/originals"
#define DITHERED_DIR  "/dithered"

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

// Background dithering queue (set by the web upload handler, drained in loop()).
extern bool     workPending;    // true when originals may need dithering
extern uint32_t lastUploadMs;   // millis() of the most recent upload

void configLoad();
void configSaveWifi(const String& ssid, const String& pass);
void configSaveMqtt(const String& host, uint16_t port,
                    const String& user, const String& pass);
