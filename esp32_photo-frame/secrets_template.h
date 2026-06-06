#pragma once

// --------------------------------------------------
// OPTIONAL: secrets.h
// --------------------------------------------------
//
// WiFi and MQTT are now configured at runtime from the web UI (and stored in
// flash), so this file is no longer required to build. It only exists to
// PRE-SEED the factory defaults — handy if you want a device to come up
// already knowing your network without touching the browser.
//
// To use it: copy this file to "secrets.h" (gitignored) and fill in values.
// Anything you set here is overridden once you save settings in the web UI.
//
// You can also override the access-point identity / rotation interval here
// (these otherwise come from config_defaults.h).

// --- Pre-seed the home network the frame should join (optional) ---
// #define WIFI_SSID     "your-ssid"
// #define WIFI_PASSWORD "your-password"

// --- Pre-seed the MQTT broker (optional) ---
// #define MQTT_SERVER   "192.168.1.10"
// #define MQTT_PORT     1883
// #define MQTT_USER     ""
// #define MQTT_PASSWORD ""

// --- Override the setup hotspot (optional; WPA2 password must be >= 8 chars) ---
// #define AP_SSID       "PhotoFrame-Setup"
// #define AP_PASSWORD   "photoframe"

// --- Override how often the displayed photo rotates, in minutes (optional) ---
// #define ROTATE_MINUTES 30
