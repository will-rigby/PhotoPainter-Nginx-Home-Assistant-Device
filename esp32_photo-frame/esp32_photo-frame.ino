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

// Server-mode (USB powered) timers
#define SENSOR_INTERVAL_MS  (5UL * 60UL * 1000UL)             // 5 minutes
#define ROTATE_INTERVAL_MS  ((uint32_t)ROTATE_MINUTES * 60UL * 1000UL)
#define POWER_CHECK_MS      10000UL                           // re-check USB every 10s
#define WORK_QUIET_MS       1500UL                            // don't process within this of an upload
#define WORK_GAP_MS         100UL                             // min gap between processing two images
#define MAX_FAILED          32                                // session failed-set size

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
static uint32_t lastWork       = 0;

// Background dithering queue state. workPending and lastUploadMs are also set
// from the web upload handler in webserver.ino. The queue is drained one image
// at a time in loop() (server mode).
bool       workPending  = false;
uint32_t   lastUploadMs = 0;
static String   failedBases[MAX_FAILED];
static int      failedCount = 0;
static uint8_t* ditherBuf   = nullptr;   // scratch panel buffer for dithering

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
  File dir = SD.open(DITHERED_DIR);
  if (!dir) {
    Serial.println("No /dithered directory on SD card");
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
  dir = SD.open(DITHERED_DIR);
  String chosen;
  int i = 0;
  while ((e = dir.openNextFile())) {
    if (isBinFile(e)) {
      if (i == target) {
        String nm = String(e.name());
        int s = nm.lastIndexOf('/');
        if (s >= 0) nm = nm.substring(s + 1);
        chosen = String(DITHERED_DIR "/") + nm;
        e.close();
        break;
      }
      i++;
    }
    e.close();
  }
  dir.close();

  if (chosen.length() == 0 || !epdLoadBufferFromFile(chosen.c_str())) {
    Serial.println("Failed to load photo buffer");
    return false;
  }
  Serial.printf("Displaying [%d/%d]: %s\n", target, count, chosen.c_str());

  epdDisplayCurrentBuffer();
  return true;
}

// --------------------------------------------------
// Background dithering queue (drained in loop(), server mode)
// --------------------------------------------------
//
// Uploads drop originals into /originals and set workPending. loop() drains the
// queue one image at a time: find an original with no matching .bin, decode,
// resize + dither into ditherBuf, and save it. Processing one image briefly
// blocks the web server (~seconds); the next image is handled on a later loop
// pass. Failures are parked in a session-only set so they aren't retried in a
// tight loop. The decode/resize/dither loops yield periodically (vTaskDelay) so
// long jobs don't starve the idle task / trip the watchdog.
// --------------------------------------------------

bool isFailedBase(const String& base) {
  for (int i = 0; i < failedCount; i++) {
    if (failedBases[i] == base) return true;
  }
  return false;
}

static void markFailedBase(const String& base) {
  if (isFailedBase(base)) return;
  if (failedCount < MAX_FAILED) failedBases[failedCount++] = base;
}

// Forget a base (e.g. on delete) so a re-upload of the same name is retried.
void clearFailedBase(const String& base) {
  for (int i = 0; i < failedCount; i++) {
    if (failedBases[i] == base) { failedBases[i] = failedBases[--failedCount]; return; }
  }
}

// Forget all failures (e.g. on "re-dither all") so everything is retried.
void clearAllFailed() {
  failedCount = 0;
}

// True for filenames we can actually decode (ignores stray files on the card).
static bool hasImageExt(const String& nm) {
  String low = nm;
  low.toLowerCase();
  return low.endsWith(".jpg") || low.endsWith(".jpeg") || low.endsWith(".bmp");
}

// Strip directory and extension → base name.
static String baseOf(const String& filename) {
  String b = filename;
  int s = b.lastIndexOf('/');
  if (s >= 0) b = b.substring(s + 1);
  int dot = b.lastIndexOf('.');
  if (dot > 0) b = b.substring(0, dot);
  return b;
}

// Process at most one pending original. Returns true if it did work (so the
// caller keeps draining), false when nothing is pending.
bool processNextPending() {
  if (!ditherBuf) return false;

  File dir = SD.open(ORIGINALS_DIR);
  if (!dir) return false;

  String srcPath, base;
  File e;
  while ((e = dir.openNextFile())) {
    if (!e.isDirectory()) {
      String nm = String(e.name());
      int s = nm.lastIndexOf('/');
      if (s >= 0) nm = nm.substring(s + 1);
      String b = baseOf(nm);
      if (hasImageExt(nm) &&
          !SD.exists(String(DITHERED_DIR "/") + b + ".bin") &&
          !isFailedBase(b)) {
        srcPath = String(ORIGINALS_DIR "/") + nm;
        base = b;
        e.close();
        break;
      }
    }
    e.close();
  }
  dir.close();

  if (srcPath.length() == 0) return false;  // nothing pending

  String binPath = String(DITHERED_DIR "/") + base + ".bin";
  Serial.printf("Dithering: %s -> %s\n", srcPath.c_str(), binPath.c_str());

  if (!imgDecode(srcPath.c_str()) ||
      !imgRenderToBuffer(ditherBuf) ||
      !epdSaveBufferToFile(binPath.c_str(), ditherBuf)) {
    Serial.printf("Processing failed for %s (parked)\n", srcPath.c_str());
    markFailedBase(base);
  }
  return true;  // advanced the queue either way
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

    // Scratch buffer the background dithering renders into (kept allocated).
    ditherBuf = (uint8_t*)ps_malloc(EPD_BUF_SIZE);
    if (!ditherBuf) ditherBuf = (uint8_t*)malloc(EPD_BUF_SIZE);
    if (!ditherBuf) Serial.println("WARN: dither buffer alloc failed");

    // Dither any originals left unprocessed (e.g. power lost mid-batch).
    workPending = true;

    // Show a photo immediately so the panel isn't blank.
    displayRandomFromSD();

    // Initial sensor report (no-op in AP mode or if MQTT unconfigured).
    sensorReport();

    uint32_t now = millis();
    lastSensor = now;
    lastRotate = now;
    lastPowerCheck = now;
    lastWork = now;
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

  // GPIO4 button cycles to another image (edge-triggered + debounced so a held
  // button is one image, not a tight refresh loop).
  static bool btnPrev = HIGH;
  bool btnNow = digitalRead(PIN_BUTTON);     // LOW = pressed (active-low)
  if (btnPrev == HIGH && btnNow == LOW) {
    delay(20);                               // debounce
    if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("Button pressed — cycling image");
      displayRandomFromSD();
      lastRotate = millis();                 // restart the auto-rotate timer
    }
  }
  btnPrev = btnNow;

  // Drain the dithering queue one image at a time, but only when the device is
  // idle (no upload in the last WORK_QUIET_MS) so active uploads stay snappy.
  if (workPending &&
      (now - lastUploadMs >= WORK_QUIET_MS) &&
      (now - lastWork >= WORK_GAP_MS)) {
    if (!processNextPending()) workPending = false;
    lastWork = millis();  // account for the processing time just spent
  }

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
