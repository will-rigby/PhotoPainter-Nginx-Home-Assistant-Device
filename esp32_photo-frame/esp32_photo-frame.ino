// SPDX-License-Identifier: 0BSD

#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SD.h>
#include <XPowersLib.h>
#include <JPEGDEC.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <driver/rtc_io.h>
#include "config.h"

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
#define PHOTO_DIR_PATH      "/photos"

// Server-mode (USB powered) timers
#define SENSOR_INTERVAL_MS  (5UL * 60UL * 1000UL)             // 5 minutes
#define ROTATE_INTERVAL_MS  ((uint32_t)ROTATE_MINUTES * 60UL * 1000UL)
#define POWER_CHECK_MS      10000UL                           // re-check USB every 10s

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

// Server-mode loop timers
static uint32_t lastSensor     = 0;
static uint32_t lastRotate     = 0;
static uint32_t lastPowerCheck = 0;

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

// True when running from USB-C power (VBUS present).
bool pmuVbusPresent() {
  if (!pmuReady) return false;
  return pmu.isVbusIn();
}

// --------------------------------------------------
// Display a random pre-processed photo from the SD card
// --------------------------------------------------

static bool isBinFile(File& e) {
  if (e.isDirectory()) return false;
  String low = String(e.name());
  low.toLowerCase();
  return low.endsWith(".bin");
}

bool displayRandomFromSD() {
  // Pass 1: count the .bin photos.
  File dir = SD.open(PHOTO_DIR_PATH);
  if (!dir) {
    Serial.println("No /photos directory on SD card");
    return false;
  }
  int count = 0;
  File e;
  while ((e = dir.openNextFile())) {
    if (isBinFile(e)) count++;
    e.close();
  }
  dir.close();

  if (count == 0) {
    Serial.println("No photos on SD card yet");
    return false;
  }

  int target = esp_random() % count;

  // Pass 2: walk to the target-th .bin and resolve its path.
  dir = SD.open(PHOTO_DIR_PATH);
  String chosen;
  int i = 0;
  while ((e = dir.openNextFile())) {
    if (isBinFile(e)) {
      if (i == target) {
        String nm = String(e.name());
        int s = nm.lastIndexOf('/');
        if (s >= 0) nm = nm.substring(s + 1);
        chosen = String(PHOTO_DIR_PATH "/") + nm;
        e.close();
        break;
      }
      i++;
    }
    e.close();
  }
  dir.close();

  if (chosen.length() == 0) return false;
  Serial.printf("Displaying [%d/%d]: %s\n", target, count, chosen.c_str());

  if (!epdLoadBufferFromFile(chosen.c_str())) {
    Serial.println("Failed to load photo buffer");
    return false;
  }

  epdDisplayCurrentBuffer();
  return true;
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
  (void)timerWake;

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

  // ----- Load persistent config -----

  configLoad();

  // ----- Choose mode based on power source -----

  bool usbPowered = pmuVbusPresent();
  Serial.printf("Power source: %s\n",
                usbPowered ? "USB-C (server mode)" : "battery (sleep mode)");

  if (usbPowered) {
    // ===== SERVER MODE — stay awake, run the web UI =====
    serverMode = true;

    if (!epdInit()) Serial.println("WARN: framebuffer alloc failed");
    if (!sdInit())  Serial.println("WARN: SD card init failed");

    wifiBringUp();        // STA if saved network reachable, else host AP
    webServerBegin();

    // Show a photo immediately so the panel isn't blank.
    displayRandomFromSD();

    // Initial sensor report (no-op in AP mode or if MQTT unconfigured).
    sensorReport();

    uint32_t now = millis();
    lastSensor = now;
    lastRotate = now;
    lastPowerCheck = now;
    return;  // fall through to loop()
  }

  // ===== BATTERY MODE — low-power deep-sleep cycle =====

  bool doDisplayUpdate = false;

  if (firstBoot) {
    doDisplayUpdate = true;
    firstBoot = false;
    wakeCount = 0;
  } else if (buttonWake) {
    doDisplayUpdate = true;
    wakeCount = 0;
  } else {
    wakeCount++;
    if (wakeCount >= WAKES_PER_UPDATE) {
      doDisplayUpdate = true;
      wakeCount = 0;
    }
  }

  Serial.printf("Wake count: %d/%d  display=%s\n",
                wakeCount, WAKES_PER_UPDATE, doDisplayUpdate ? "yes" : "no");

  // Sensor report (quick — WiFi + MQTT)
  sensorReport();

  // Display update (rotate a stored photo from the SD card)
  if (doDisplayUpdate) {
    if (epdInit() && sdInit()) {
      displayRandomFromSD();
    } else {
      Serial.println("Skipping display update (EPD/SD init failed)");
    }
  }

  enterDeepSleep();
}

// --------------------------------------------------
// LOOP (only runs in server mode; battery mode never returns from setup)
// --------------------------------------------------

void loop() {
  if (!serverMode) {
    enterDeepSleep();
    return;
  }

  webServerHandle();

  uint32_t now = millis();

  if (now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;
    sensorReport();
  }

  if (now - lastRotate >= ROTATE_INTERVAL_MS) {
    lastRotate = now;
    displayRandomFromSD();
  }

  if (now - lastPowerCheck >= POWER_CHECK_MS) {
    lastPowerCheck = now;
    if (!pmuVbusPresent()) {
      Serial.println("USB power removed — switching to battery sleep mode");
      enterDeepSleep();
    }
  }

  delay(2);
}
