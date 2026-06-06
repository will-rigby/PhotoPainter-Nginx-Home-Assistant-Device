// SPDX-License-Identifier: 0BSD

#include "config.h"

// --------------------------------------------------
// WiFi management (station + access-point fallback)
// --------------------------------------------------
//
// The frame no longer downloads images from an nginx server. Photos live on
// the SD card (uploaded via the web UI). WiFi is used for:
//   - the embedded web server (USB-powered "server mode")
//   - publishing sensor data to MQTT
//
// Behaviour: try to join the saved network as a station (STA). If no creds
// are saved, the network can't be found, or the join fails, host our own
// WPA2 access point (AP) so the user can connect and configure it.
//

#define CONNECT_TIMEOUT   12000   // STA join timeout (ms)

// --------------------------------------------------
// Station connect (returns true on success)
// --------------------------------------------------

bool wifiConnectSTA(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return false;

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ESP32-PhotoFrame");
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > CONNECT_TIMEOUT) {
      Serial.println("\nWiFi connection timeout!");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected — IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void wifiDisconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected");
}

// --------------------------------------------------
// Scan to confirm a saved SSID is actually in range
// --------------------------------------------------

static bool wifiSsidInRange(const String& ssid) {
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) { found = true; break; }
  }
  WiFi.scanDelete();
  Serial.printf("Scan: '%s' %s\n", ssid.c_str(), found ? "found" : "not found");
  return found;
}

// --------------------------------------------------
// Start our own access point (WPA2)
// --------------------------------------------------

void startAP() {
  Serial.print("Starting access point: ");
  Serial.println(AP_SSID);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

// --------------------------------------------------
// Bring WiFi up for server mode: STA if possible, else AP.
// Sets the global apMode flag accordingly.
// --------------------------------------------------

void wifiBringUp() {
  if (cfgWifiSsid.length() > 0 && wifiSsidInRange(cfgWifiSsid) &&
      wifiConnectSTA(cfgWifiSsid, cfgWifiPass)) {
    apMode = false;
    return;
  }
  startAP();
  apMode = true;
}
