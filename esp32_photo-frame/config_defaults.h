// SPDX-License-Identifier: 0BSD
#pragma once

// --------------------------------------------------
// Factory defaults
// --------------------------------------------------
//
// These are the values used until they are overwritten at runtime via the
// web UI (which persists them to NVS / flash). You normally only need to set
// the access-point name/password here; WiFi and MQTT are configured in the
// browser. A legacy secrets.h, if present, can still seed the defaults.
//

// Optionally pull in a user-provided secrets.h to pre-seed defaults.
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

// Access point (setup hotspot). WPA2 requires a password of >= 8 characters.
#ifndef AP_SSID
#  define AP_SSID      "PhotoFrame-Setup"
#endif
#ifndef AP_PASSWORD
#  define AP_PASSWORD  "photoframe"
#endif

// How often (minutes) to rotate the displayed photo.
#ifndef ROTATE_MINUTES
#  define ROTATE_MINUTES  30
#endif

// Optional factory WiFi / MQTT seeds (left blank unless provided by secrets.h).
#ifndef WIFI_SSID
#  define WIFI_SSID      ""
#endif
#ifndef WIFI_PASSWORD
#  define WIFI_PASSWORD  ""
#endif
#ifndef MQTT_SERVER
#  define MQTT_SERVER    ""
#endif
#ifndef MQTT_PORT
#  define MQTT_PORT      1883
#endif
#ifndef MQTT_USER
#  define MQTT_USER      ""
#endif
#ifndef MQTT_PASSWORD
#  define MQTT_PASSWORD  ""
#endif
