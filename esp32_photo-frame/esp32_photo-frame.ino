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

// Session-only set of complete filenames that failed to decode/render, so the display
// paths skip them instead of retrying in a tight loop.
static String   failedBases[MAX_FAILED];
static int      failedCount = 0;

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
// Low-battery indicator
// --------------------------------------------------
//
// Overlaid on the framebuffer at refresh time (epdDisplayCurrentBuffer),
// after the dither cache and thumbnail are generated from the buffer — so
// cached .bins and thumbs stay clean and the icon disappears on the first
// refresh after charging.

#define LOW_BATT_ICON_PERCENT  20     // show icon at/below this charge level
#define LOW_BATT_ICON_VOLTS    3.45f  // fallback when the fuel gauge is n/a

static bool batteryIsLow() {
  if (!pmuReady) return false;
  if (pmu.isVbusIn() || pmu.isCharging()) return false;  // externally powered
  int pct = pmu.getBatteryPercent();
  if (pct >= 0) return pct <= LOW_BATT_ICON_PERCENT;
  float vbat = pmu.getBattVoltage();
  if (vbat > 100.0f) vbat /= 1000.0f;
  return vbat > 0.1f && vbat < LOW_BATT_ICON_VOLTS;     // 0V = no battery
}

static void fillRectFb(int x, int y, int w, int h, uint8_t color) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      epdSetPixel(xx, yy, color);
}

// Small red nearly-empty battery, bottom-right, on a white pad for contrast.
void overlayLowBatteryIcon() {
  if (!batteryIsLow()) return;
  Serial.println("Battery low - drawing indicator");

  const int W = 48, H = 24, T = 3;   // body size, border thickness
  const int x = EPD_WIDTH - W - 20;  // leaves room for the terminal nub
  const int y = EPD_HEIGHT - H - 16;

  fillRectFb(x - 4, y - 4, W + 12, H + 8, EPD_WHITE);          // backdrop
  fillRectFb(x, y, W, T, EPD_RED);                             // top edge
  fillRectFb(x, y + H - T, W, T, EPD_RED);                     // bottom edge
  fillRectFb(x, y, T, H, EPD_RED);                             // left edge
  fillRectFb(x + W - T, y, T, H, EPD_RED);                     // right edge
  fillRectFb(x + W, y + 7, 4, H - 14, EPD_RED);                // terminal nub
  fillRectFb(x + T + 2, y + T + 2, 6, H - 2 * T - 4, EPD_RED); // last sliver
}

// --------------------------------------------------
// Lazy dither cache (rendered at display time)
// --------------------------------------------------
//
// Originals live in /originals. Nothing is processed at upload time; instead,
// the first time a photo is displayed it is decoded, resized + dithered
// straight into the panel framebuffer (epdBuffer) and the result is saved as
// a source/revision-specific dither cache. Later displays of the same photo are a pure cache
// load. Failures are parked in a session-only set so a bad file isn't retried
// in a tight loop. The decode/resize/dither loops yield periodically
// (vTaskDelay) so long jobs don't starve the idle task / trip the watchdog.
// --------------------------------------------------

bool isFailedBase(const String& base) {
  MutexGuard guard(stateMutex);
  for (int i = 0; i < failedCount; i++) {
    if (failedBases[i] == base) return true;
  }
  return false;
}

static void markFailedBase(const String& base) {
  MutexGuard guard(stateMutex);
  if (isFailedBase(base)) return;
  if (failedCount < MAX_FAILED) failedBases[failedCount++] = base;
}

// Forget a base (e.g. on delete) so a re-upload of the same name is retried.
void clearFailedBase(const String& base) {
  MutexGuard guard(stateMutex);
  for (int i = 0; i < failedCount; i++) {
    if (failedBases[i] == base) { failedBases[i] = failedBases[--failedCount]; return; }
  }
}

// Forget all failures (e.g. on "re-dither all") so everything is retried.
void clearAllFailed() {
  MutexGuard guard(stateMutex);
  failedCount = 0;
}

// Ensure a source/revision-specific dither cache exists for the given original (basename with
// extension) and leave the image in epdBuffer, ready for
// epdDisplayCurrentBuffer(). Cache hit: load the .bin (a corrupt/short .bin is
// deleted and re-rendered). Miss: decode /originals/<name>, resize + dither
// straight into epdBuffer, then save it as the cache file. A failed cache
// *write* still returns true (the render is displayable; caching retries next
// time); a failed decode or render marks the base failed and returns false.
bool ensureDitheredInBuffer(const String& origName) {
  if (!epdBuffer) return false;
  PhotoMeta meta;
  if (!photoMeta(origName, meta)) { displayFailure("Photo metadata unavailable"); return false; }
  String binPath = photoCachePath(meta);
  if (storage.exists(binPath)) {
    if (epdLoadBufferFromFile(binPath.c_str())) {
      thumbEnsureFromBuffer(origName);
      return true;
    }
    storage.remove(binPath);
  }
  String source = String(ORIGINALS_DIR "/") + origName;
  if (!imgDecode(source.c_str()) || !imgRenderToBuffer(epdBuffer)) {
    displayFailure("Could not decode or render " + origName);
    markFailedBase(origName); return false;
  }
  String temp = binPath + ".part";
  if (!epdSaveBufferToFile(temp.c_str(), epdBuffer) || !storage.rename(temp, binPath)) {
    storage.remove(temp);
    operationError("Displayed photo could not be cached; will retry next time");
  }
  clearFailedBase(origName);
  thumbEnsureFromBuffer(origName);
  storageChanged();
  return true;
}

// --------------------------------------------------
// Display a random photo from /originals (lazy render + cache)
// --------------------------------------------------

bool displayRandomFromSD() {
  const int MAX_ATTEMPTS = 4;

  for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    // Pass 1: count displayable originals (decodable extension, not parked).
    StorageFile dir = storage.open(ORIGINALS_DIR);
    if (!dir) {
      Serial.println("No /originals directory on SD card");
      return false;
    }
    int count = 0;
    StorageFile e;
    while ((e = dir.openNextFile())) {
      if (!e.isDirectory()) {
        String nm = String(e.name());
        int s = nm.lastIndexOf('/');
        if (s >= 0) nm = nm.substring(s + 1);
        if (validPhotoName(nm) && !isFailedBase(nm)) count++;
      }
      e.close();
    }
    dir.close();

    if (count == 0) {
      Serial.println("No displayable photos on SD card");
      return false;
    }

    int target = esp_random() % count;

    // Pass 2: walk to the target-th displayable original.
    dir = storage.open(ORIGINALS_DIR);
    String chosen;
    int i = 0;
    while ((e = dir.openNextFile())) {
      if (!e.isDirectory()) {
        String nm = String(e.name());
        int s = nm.lastIndexOf('/');
        if (s >= 0) nm = nm.substring(s + 1);
        if (validPhotoName(nm) && !isFailedBase(nm)) {
          if (i == target) {
            chosen = nm;
            e.close();
            break;
          }
          i++;
        }
      }
      e.close();
    }
    dir.close();

    if (chosen.length() == 0) return false;  // card changed under us

    // A failed render parks the filename, so the next attempt's scan excludes it.
    if (displayNamedPhoto(chosen)) {
      Serial.printf("Displaying [%d/%d]: %s\n", target, count, chosen.c_str());
      return true;
    }
  }

  Serial.println("Failed to display a photo (all attempts failed)");
  return false;
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
  synchronizationBegin();
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
    bool sdReady = sdInit();
    if (sdReady && !photoStoreBegin()) operationError("Could not prepare all storage directories; card may be full or read-only");
    runtimeBegin(sdReady);

    wifiBringUp();        // STA if saved network reachable, else host AP
    uint32_t startupJob; String startupError;
    queueDisplay(DisplayKind::Random, "", false, startupJob, startupError);
    webServerBegin();

    // The initial display is queued; HTTP remains available during startup work.

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
      photoStoreBegin(); // existing sources can still render on a full card
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

  controlTick();

  uint32_t now = millis();

  // GPIO4 button cycles to another image (edge-triggered + debounced so a held
  // button is one image, not a tight refresh loop).
  static bool btnPrev = HIGH;
  bool btnNow = digitalRead(PIN_BUTTON);     // LOW = pressed (active-low)
  if (btnPrev == HIGH && btnNow == LOW) {
    delay(20);                               // debounce
    if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("Button pressed — cycling image");
      uint32_t job; String error;
      queueDisplay(DisplayKind::Random, "", true, job, error);
      lastRotate = millis();                 // restart the auto-rotate timer
    }
  }
  btnPrev = btnNow;

  if (now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;
    sensorReport();
  }

  if (now - lastRotate >= ROTATE_INTERVAL_MS) {
    lastRotate = now;
    uint32_t job; String error;
    queueDisplay(DisplayKind::Random, "", false, job, error);
  }

  if (now - lastPowerCheck >= POWER_CHECK_MS) {
    lastPowerCheck = now;
    if (!pmuVbusPresent()) {
      Serial.println("USB power removed — switching to battery sleep mode");
      webServerStop();
      enterDeepSleep();
    }
  }

  delay(2);
}
