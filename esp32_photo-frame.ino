// SPDX-License-Identifier: 0BSD

#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <SD.h>
#include <XPowersLib.h>
#include <JPEGDEC.h>
#include <driver/rtc_io.h>
#include "secrets.h"

// --------------------------------------------------
// Pin Definitions
// --------------------------------------------------

// EPD SPI
#define PIN_DC     8
#define PIN_CS     9
#define PIN_SCK    10
#define PIN_MOSI   11
#define PIN_RST    12
#define PIN_BUSY   13

// I2C (PMU)
#define I2C_SDA    47
#define I2C_SCL    48

// SD Card SPI
#define SD_PIN_CS   38
#define SD_PIN_CLK  39
#define SD_PIN_MISO 40
#define SD_PIN_MOSI 41

// Button (active-low, pulled to GND)
#define PIN_BUTTON  4

// --------------------------------------------------
// EPD Constants
// --------------------------------------------------

#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_ROW_BYTES  (EPD_WIDTH / 2)   // 4bpp, 2 pixels per byte
#define EPD_BUF_SIZE   (EPD_ROW_BYTES * EPD_HEIGHT)  // 192,000 bytes

// Colour indices per panel datasheet
#define EPD_BLACK   0x0
#define EPD_WHITE   0x1
#define EPD_YELLOW  0x2
#define EPD_RED     0x3
#define EPD_BLUE    0x5
#define EPD_GREEN   0x6

// --------------------------------------------------
// Configuration
// --------------------------------------------------

#define SLEEP_DURATION_US   (5ULL * 60ULL * 1000000ULL) // 5 minutes
#define WAKES_PER_UPDATE    6                            // 6 × 5 min = 30 min
#define TEMP_IMAGE_PATH     "/photo_frame_temp"

// --------------------------------------------------
// RTC-persistent state (survives deep sleep)
// --------------------------------------------------

RTC_DATA_ATTR int  wakeCount    = 0;   // counts up each timer wake
RTC_DATA_ATTR bool firstBoot    = true;

// --------------------------------------------------
// Globals (re-initialised each boot)
// --------------------------------------------------

XPowersAXP2101 pmu;
SPIClass spi(FSPI);
bool pmuReady = false;

WiFiClient    mqttWifi;
PubSubClient  mqtt(mqttWifi);

#define MAX_IMAGES  512
String imageList[MAX_IMAGES];
int    imageCount = 0;

// --------------------------------------------------
// PMU
// --------------------------------------------------

void logPmu() {
  if (!pmuReady) {
    return;
  }
  float vbat = pmu.getBattVoltage();
  float vsys = pmu.getSystemVoltage();
  float valdo4 = pmu.getALDO4Voltage();
  if (vbat > 100.0f) vbat /= 1000.0f;
  if (vsys > 100.0f) vsys /= 1000.0f;
  if (valdo4 > 100.0f) valdo4 /= 1000.0f;
  Serial.print("PMU VBAT=");
  Serial.print(vbat, 3);
  Serial.print("V VSYS=");
  Serial.print(vsys, 3);
  Serial.print("V ALDO4=");
  Serial.print(valdo4, 3);
  Serial.print("V CHG=");
  Serial.println(pmu.isCharging() ? "yes" : "no");
}

// --------------------------------------------------
// UPDATE DISPLAY
// --------------------------------------------------

void updateDisplay() {
  Serial.println("=== Starting display update ===");

  if (!wifiConnect()) {
    Serial.println("WiFi failed, skipping update");
    return;
  }

  if (!fetchImageList()) {
    Serial.println("No images found on server");
    wifiDisconnect();
    return;
  }

  // Pick a random image
  int idx = esp_random() % imageCount;
  Serial.print("Selected image [");
  Serial.print(idx);
  Serial.print("]: ");
  Serial.println(imageList[idx]);

  // Download to SD card
  String localPath = String(TEMP_IMAGE_PATH) + getExtension(imageList[idx]);
  if (!downloadImageToSD(imageList[idx], localPath)) {
    Serial.println("Download failed");
    wifiDisconnect();
    return;
  }

  wifiDisconnect();

  // Process image: decode → resize → dither → write to EPD buffer
  if (!processImage(localPath.c_str())) {
    Serial.println("Image processing failed");
    SD.remove(localPath);
    return;
  }

  // Refresh the display
  epdWriteBuffer();
  epdTurnOnDisplay();

  // Clean up temp file
  SD.remove(localPath);

  Serial.println("=== Display update complete ===");
}

// Return file extension including the dot, e.g. ".jpg"
String getExtension(const String& filename) {
  int dot = filename.lastIndexOf('.');
  if (dot < 0) return "";
  return filename.substring(dot);
}

// --------------------------------------------------
// DEEP SLEEP
// --------------------------------------------------

void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  Serial.flush();

  // Timer wakeup — 5 minutes
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);

  // Button wakeup — GPIO4 active-low (wake on LOW)
  // Enable RTC pull-up so the pin stays HIGH during deep sleep
  rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);

  esp_deep_sleep_start();
  // — never returns —
}

// --------------------------------------------------
// SETUP (runs on every wake)
// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  // Determine why we woke up
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool buttonWake = (wakeReason == ESP_SLEEP_WAKEUP_EXT0);
  bool timerWake  = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);

  if (firstBoot) {
    Serial.println("=== First boot ===");
  } else if (buttonWake) {
    Serial.println("=== Woke: BUTTON ===");
  } else if (timerWake) {
    Serial.println("=== Woke: TIMER ===");
  } else {
    Serial.printf("=== Woke: reason %d ===\n", wakeReason);
  }

  // ----- Hardware init (required every wake) -----

  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  digitalWrite(PIN_CS, HIGH);

  spi.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  // ----- AXP2101 -----

  Wire.begin(I2C_SDA, I2C_SCL);
  if (pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    pmuReady = true;
    Serial.println("AXP2101 detected");
    pmu.enableSystemVoltageMeasure();
    pmu.setALDO4Voltage(3300);
    pmu.enableALDO4();
    logPmu();
  } else {
    Serial.println("AXP2101 init failed");
  }

  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();
  delay(200);

  // ----- SHTC3 -----

  sensorInit();

  // ----- Decide what to do this wake -----

  bool doDisplayUpdate = false;
  bool doSensorReport  = true;  // always report sensor

  if (firstBoot) {
    doDisplayUpdate = true;
    firstBoot = false;
    wakeCount = 0;
  } else if (buttonWake) {
    doDisplayUpdate = true;
    wakeCount = 0;  // reset timer so next display update is 30 min from now
  } else {
    // Timer wake
    wakeCount++;
    if (wakeCount >= WAKES_PER_UPDATE) {
      doDisplayUpdate = true;
      wakeCount = 0;
    }
  }

  Serial.printf("Wake count: %d/%d  display=%s  sensor=%s\n",
                wakeCount, WAKES_PER_UPDATE,
                doDisplayUpdate ? "yes" : "no",
                doSensorReport  ? "yes" : "no");

  // ----- Sensor report (quick — WiFi + MQTT) -----

  if (doSensorReport) {
    sensorReport();
  }

  // ----- Display update (heavy — WiFi + download + decode + refresh) -----

  if (doDisplayUpdate) {
    epdPortInit();

    if (!epdInit()) {
      Serial.println("FATAL: no framebuffer");
      enterDeepSleep();
    }

    if (!sdInit()) {
      Serial.println("SD card init failed, skipping display update");
    } else {
      updateDisplay();
    }

    epdDeepSleep();
  }

  // ----- Sleep -----

  enterDeepSleep();
}

// --------------------------------------------------
// LOOP (never reached — deep sleep resets to setup)
// --------------------------------------------------

void loop() {
  // Should never get here
}