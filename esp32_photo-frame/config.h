// SPDX-License-Identifier: 0BSD
#pragma once

#include <Arduino.h>
#include "config_defaults.h"

// --------------------------------------------------
// SD card folders
// --------------------------------------------------
//   /originals/<base>.<ext>  uploaded source images (gallery + display source)
//   /dithered/<base>.bin     lazily built display cache (800x480 4bpp panel
//                            buffers, rendered on first display, reused after)
//   /thumbs/<base>.jpg|.bmp  gallery thumbnails (.jpg uploaded by the browser
//                            alongside new photos, .bmp generated on-device at
//                            display time for photos that lack one)
#define ORIGINALS_DIR "/originals"
#define DITHERED_DIR  "/dithered"
#define THUMBS_DIR    "/thumbs"

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

// Panel framebuffer (allocated by epdInit() in spectra73.ino). The lazy
// render pipeline dithers directly into it.
extern uint8_t* epdBuffer;

// Lazy dither cache: make sure /dithered/<base>.bin exists for the given
// original (basename with extension) and leave the image in epdBuffer, ready
// for epdDisplayCurrentBuffer(). Defined in esp32_photo-frame.ino.
bool ensureDitheredInBuffer(const String& origName);

// Failed-image set (image bases whose processing failed).
bool isFailedBase(const String& base);

// Write /thumbs/<base>.bmp from the dithered framebuffer if the photo has no
// thumbnail yet (defined in image_processing.ino).
void thumbEnsureFromBuffer(const String& base);

// Draw a red low-battery icon into the framebuffer if the battery is low
// (defined in esp32_photo-frame.ino; called by epdDisplayCurrentBuffer).
void overlayLowBatteryIcon();

void configLoad();
void configSaveWifi(const String& ssid, const String& pass);
void configSaveMqtt(const String& host, uint16_t port,
                    const String& user, const String& pass);
