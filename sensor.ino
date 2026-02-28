// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// SHTC3 Temperature & Humidity → MQTT (Home Assistant)
// --------------------------------------------------
//
// The SHTC3 sits on the same I2C bus as the AXP2101.
// Wire.begin() is already called in setup().
//

#define SHTC3_ADDR  0x70

// MQTT topics — HA auto-discovery friendly
#define MQTT_TOPIC_TEMP     "homeassistant/sensor/photoframe/temperature/state"
#define MQTT_TOPIC_HUMID    "homeassistant/sensor/photoframe/humidity/state"
#define MQTT_TOPIC_BATTERY  "homeassistant/sensor/photoframe/battery/state"
#define MQTT_TOPIC_CONFIG_T "homeassistant/sensor/photoframe_temperature/config"
#define MQTT_TOPIC_CONFIG_H "homeassistant/sensor/photoframe_humidity/config"
#define MQTT_TOPIC_CONFIG_B "homeassistant/sensor/photoframe_battery/config"

static bool shtc3Ready = false;

// --------------------------------------------------
// SHTC3 low-level I2C helpers
// --------------------------------------------------

static bool shtc3WriteCmd(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return (Wire.endTransmission() == 0);
}

static void shtc3Wakeup() {
  shtc3WriteCmd(0x3517);  // Wake-up
  delay(1);
}

static void shtc3Sleep() {
  shtc3WriteCmd(0xB098);  // Sleep
}

// --------------------------------------------------
// Init — verify the sensor is present
// --------------------------------------------------

void sensorInit() {
  shtc3Wakeup();

  // Read ID register
  if (!shtc3WriteCmd(0xEFC8)) {
    Serial.println("SHTC3 not found on I2C");
    return;
  }

  delay(10);
  Wire.requestFrom((uint8_t)SHTC3_ADDR, (uint8_t)3);
  if (Wire.available() < 3) {
    Serial.println("SHTC3 ID read failed");
    shtc3Sleep();
    return;
  }

  uint8_t id_hi  = Wire.read();
  uint8_t id_lo  = Wire.read();
  uint8_t id_crc = Wire.read();
  (void)id_crc;

  uint16_t id = ((uint16_t)id_hi << 8) | id_lo;
  // Bits [5:0] of the upper byte should be 0x07 for SHTC3
  if ((id & 0x083F) == 0x0807) {
    shtc3Ready = true;
    Serial.printf("SHTC3 detected (ID: 0x%04X)\n", id);
  } else {
    Serial.printf("SHTC3 unexpected ID: 0x%04X\n", id);
  }

  shtc3Sleep();
}

// --------------------------------------------------
// Read temperature & humidity
// --------------------------------------------------

static bool shtc3Read(float& tempC, float& humidity) {
  shtc3Wakeup();

  // Normal mode, temp first, clock stretching enabled
  if (!shtc3WriteCmd(0x7CA2)) {
    shtc3Sleep();
    return false;
  }

  delay(15);  // measurement time

  Wire.requestFrom((uint8_t)SHTC3_ADDR, (uint8_t)6);
  if (Wire.available() < 6) {
    shtc3Sleep();
    return false;
  }

  uint8_t t_hi  = Wire.read();
  uint8_t t_lo  = Wire.read();
  uint8_t t_crc = Wire.read();
  uint8_t h_hi  = Wire.read();
  uint8_t h_lo  = Wire.read();
  uint8_t h_crc = Wire.read();
  (void)t_crc; (void)h_crc;

  uint16_t rawT = ((uint16_t)t_hi << 8) | t_lo;
  uint16_t rawH = ((uint16_t)h_hi << 8) | h_lo;

  tempC    = -45.0f + 175.0f * rawT / 65535.0f;
  humidity = 100.0f * rawH / 65535.0f;

  shtc3Sleep();
  return true;
}

// --------------------------------------------------
// MQTT connect + HA auto-discovery
// --------------------------------------------------

static bool mqttConnect() {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(512);

  const char* clientId = "ESP32-PhotoFrame";
  bool ok;

  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(clientId, MQTT_USER, MQTT_PASSWORD);
  } else {
    ok = mqtt.connect(clientId);
  }

  if (!ok) {
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }

  Serial.println("MQTT connected");

  // Publish Home Assistant auto-discovery configs (retained)
  String cfgTemp = "{"
    "\"name\":\"Photo Frame Temperature\","
    "\"stat_t\":\"" MQTT_TOPIC_TEMP "\","
    "\"unit_of_meas\":\"°C\","
    "\"dev_cla\":\"temperature\","
    "\"uniq_id\":\"photoframe_temp\","
    "\"dev\":{\"ids\":[\"esp32_photoframe\"],\"name\":\"ESP32 Photo Frame\",\"mf\":\"DIY\"}"
    "}";
  mqtt.publish(MQTT_TOPIC_CONFIG_T, cfgTemp.c_str(), true);

  String cfgHumid = "{"
    "\"name\":\"Photo Frame Humidity\","
    "\"stat_t\":\"" MQTT_TOPIC_HUMID "\","
    "\"unit_of_meas\":\"%\","
    "\"dev_cla\":\"humidity\","
    "\"uniq_id\":\"photoframe_humid\","
    "\"dev\":{\"ids\":[\"esp32_photoframe\"],\"name\":\"ESP32 Photo Frame\",\"mf\":\"DIY\"}"
    "}";
  mqtt.publish(MQTT_TOPIC_CONFIG_H, cfgHumid.c_str(), true);

  String cfgBatt = "{"
    "\"name\":\"Photo Frame Battery\","
    "\"stat_t\":\"" MQTT_TOPIC_BATTERY "\","
    "\"unit_of_meas\":\"V\","
    "\"dev_cla\":\"voltage\","
    "\"uniq_id\":\"photoframe_battery\","
    "\"dev\":{\"ids\":[\"esp32_photoframe\"],\"name\":\"ESP32 Photo Frame\",\"mf\":\"DIY\"}"
    "}";
  mqtt.publish(MQTT_TOPIC_CONFIG_B, cfgBatt.c_str(), true);

  return true;
}

// --------------------------------------------------
// Report: read sensor → publish via MQTT
// --------------------------------------------------

void sensorReport() {
  float tempC = 0, humidity = 0;
  bool hasSensor = false;

  if (shtc3Ready) {
    if (shtc3Read(tempC, humidity)) {
      hasSensor = true;
      Serial.printf("SHTC3: %.1f°C  %.1f%% RH\n", tempC, humidity);
    } else {
      Serial.println("SHTC3 read failed");
    }
  }

  // Read battery voltage from PMU
  float vbat = 0;
  if (pmuReady) {
    vbat = pmu.getBattVoltage();
    if (vbat > 100.0f) vbat /= 1000.0f;  // auto-scale if mV
    Serial.printf("Battery: %.2fV\n", vbat);
  }

  // Ensure WiFi is up
  bool weConnectedWifi = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiConnect()) {
      Serial.println("WiFi failed, skipping MQTT report");
      return;
    }
    weConnectedWifi = true;
  }

  if (!mqtt.connected()) {
    if (!mqttConnect()) {
      if (weConnectedWifi) wifiDisconnect();
      return;
    }
  }

  char buf[16];

  if (hasSensor) {
    snprintf(buf, sizeof(buf), "%.1f", tempC);
    mqtt.publish(MQTT_TOPIC_TEMP, buf);

    snprintf(buf, sizeof(buf), "%.1f", humidity);
    mqtt.publish(MQTT_TOPIC_HUMID, buf);
  }

  if (pmuReady) {
    snprintf(buf, sizeof(buf), "%.2f", vbat);
    mqtt.publish(MQTT_TOPIC_BATTERY, buf);
  }

  mqtt.loop();  // flush

  Serial.println("MQTT sensor data published");

  // Don't disconnect WiFi here — updateDisplay() manages it,
  // and we may be mid-wait in loop().
  // But if we brought WiFi up just for this, tear it down.
  if (weConnectedWifi) {
    mqtt.disconnect();
    wifiDisconnect();
  }
}
