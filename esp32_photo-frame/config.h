// SPDX-License-Identifier: 0BSD
#pragma once

#include <Arduino.h>
#include "config_defaults.h"
#include "app_support.h"

// --------------------------------------------------
// SD card folders
// --------------------------------------------------
//   /originals/<filename>  display source (browser-prepared or copied to SD)
//   /dithered/<key>-<source-revision>-r2.bin  generated panel buffers
//   /thumbs/<key>-<thumbnail-revision>.jpg|.bmp  generated gallery previews
// Photo keys include the complete filename, including extension.
#define ORIGINALS_DIR "/originals"
#define DITHERED_DIR  "/dithered"
#define THUMBS_DIR    "/thumbs"

// --------------------------------------------------
// Runtime configuration (persisted in NVS, namespace "cfg")
// --------------------------------------------------
//
// Loaded at boot; use configSnapshot() after the HTTP task starts.
// Declared here so every .ino sees them regardless of compile order.
//

extern String   cfgWifiSsid;
extern String   cfgWifiPass;
extern String   cfgMqttHost;
extern uint16_t cfgMqttPort;
extern String   cfgMqttUser;
extern String   cfgMqttPass;
extern String   cfgHostname;

// Current WiFi / power state (set during boot).
extern bool apMode;       // true when hosting our own access point
extern bool serverMode;   // true when on USB power and running the web server

// Panel framebuffer (allocated by epdInit() in spectra73.ino). The lazy
// render pipeline dithers directly into it.
extern uint8_t* epdBuffer;

// Lazy dither cache: load or build the revisioned cache for the given
// original (basename with extension) and leave the image in epdBuffer, ready
// for epdDisplayCurrentBuffer(). Defined in esp32_photo-frame.ino.
bool ensureDitheredInBuffer(const String& origName);

// Failed-image set, keyed by complete filename.
bool isFailedBase(const String& base);

// Write a revisioned BMP from the dithered framebuffer if the photo has no
// thumbnail yet (defined in image_processing.ino).
void thumbEnsureFromBuffer(const String& name);

// Draw a red low-battery icon into the framebuffer if the battery is low
// (defined in esp32_photo-frame.ino; called by epdDisplayCurrentBuffer).
void overlayLowBatteryIcon();

void configLoad();
