// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Spectra 7.3" ACeP E-Paper Driver
// (EPD constants defined in esp32_photo-frame.ino)
// --------------------------------------------------

const uint8_t colorValues[] = { EPD_BLACK, EPD_WHITE, EPD_YELLOW, EPD_RED, EPD_BLUE, EPD_GREEN };
const char* colorNames[]    = { "Black",   "White",   "Yellow",   "Red",   "Blue",   "Green" };
const uint8_t colorCount = 6;

// Framebuffer — allocated in epdInit()
uint8_t* epdBuffer = nullptr;

// --------------------------------------------------
// BUSY HANDLING
// --------------------------------------------------

bool epdWaitBusy(uint32_t timeout_ms = 60000) {
  uint32_t start = millis();

  while (digitalRead(PIN_BUSY) == LOW) {
    if (millis() - start > timeout_ms) {
      Serial.println("EPD BUSY timeout!");
      return false;
    }
    delay(5);
  }
  return true;
}

// --------------------------------------------------
// LOW LEVEL SPI
// --------------------------------------------------

bool epdSendCommand(uint8_t cmd) {
  if (!epdWaitBusy()) return false;  // Bail if controller hung

  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  spi.transfer(cmd);
  digitalWrite(PIN_CS, HIGH);
  return true;
}

void epdSendData(uint8_t data) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  spi.transfer(data);
  digitalWrite(PIN_CS, HIGH);
}

void epdBeginData() {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
}

void epdEndData() {
  digitalWrite(PIN_CS, HIGH);
}

// --------------------------------------------------
// PANEL CONTROL
// --------------------------------------------------

void epdHardwareReset() {
  digitalWrite(PIN_RST, LOW);
  delay(20);
  digitalWrite(PIN_RST, HIGH);
  delay(50);
  epdWaitBusy();
}

void epdPortInit() {
  Serial.println("EPD Init sequence");

  epdHardwareReset();
  delay(50);

  epdSendCommand(0xAA);      // CMDH
  epdSendData(0x49);
  epdSendData(0x55);
  epdSendData(0x20);
  epdSendData(0x08);
  epdSendData(0x09);
  epdSendData(0x18);

  epdSendCommand(0x01);
  epdSendData(0x3F);

  epdSendCommand(0x00);
  epdSendData(0x5F);
  epdSendData(0x69);

  epdSendCommand(0x03);
  epdSendData(0x00);
  epdSendData(0x54);
  epdSendData(0x00);
  epdSendData(0x44);

  epdSendCommand(0x05);      // Booster soft start
  epdSendData(0x40);
  epdSendData(0x1F);
  epdSendData(0x1F);
  epdSendData(0x2C);

  epdSendCommand(0x06);      // Booster setting
  epdSendData(0x6F);
  epdSendData(0x1F);
  epdSendData(0x17);
  epdSendData(0x49);

  epdSendCommand(0x08);
  epdSendData(0x6F);
  epdSendData(0x1F);
  epdSendData(0x1F);
  epdSendData(0x22);

  epdSendCommand(0x30);      // PLL
  epdSendData(0x03);

  epdSendCommand(0x50);      // VCOM / data interval
  epdSendData(0x3F);

  epdSendCommand(0x60);      // TCON
  epdSendData(0x02);
  epdSendData(0x00);

  epdSendCommand(0x61);      // Resolution: 800 x 480
  epdSendData(0x03);         // 0x0320 = 800
  epdSendData(0x20);
  epdSendData(0x01);         // 0x01E0 = 480
  epdSendData(0xE0);

  epdSendCommand(0x84);
  epdSendData(0x01);

  epdSendCommand(0xE3);
  epdSendData(0x2F);

  epdSendCommand(0x04);      // Power ON
  epdWaitBusy();

  Serial.println("EPD Init complete");
}

void epdDeepSleep() {
  Serial.println("EPD Deep Sleep");
  epdSendCommand(0x07);      // Deep Sleep
  epdSendData(0xA5);         // Check byte — exit requires HWRESET
}

void epdStartTransmission() {
  epdSendCommand(0x10);      // Data Start Transmission
}

void epdTurnOnDisplay() {
  Serial.println("EPD Turn on display");

  epdSendCommand(0x04);      // Power ON
  epdWaitBusy();

  epdSendCommand(0x06);      // Booster setting
  epdSendData(0x6F);
  epdSendData(0x1F);
  epdSendData(0x17);
  epdSendData(0x49);

  epdSendCommand(0x12);      // Display Refresh
  epdSendData(0x00);

  uint32_t start = millis();
  epdWaitBusy();
  uint32_t duration = millis() - start;

  Serial.print("Refresh complete in ");
  Serial.print(duration / 1000.0);
  Serial.println(" seconds");

  epdSendCommand(0x02);      // Power OFF
  epdSendData(0x00);
  epdWaitBusy();
}

// --------------------------------------------------
// FRAMEBUFFER INIT
// --------------------------------------------------

bool epdInit() {
  epdBuffer = (uint8_t*)ps_malloc(EPD_BUF_SIZE);
  if (!epdBuffer) {
    Serial.println("PSRAM alloc failed, trying heap");
    epdBuffer = (uint8_t*)malloc(EPD_BUF_SIZE);
  }
  if (!epdBuffer) {
    Serial.println("Framebuffer allocation failed!");
    return false;
  }
  Serial.print("Framebuffer allocated: ");
  Serial.print(EPD_BUF_SIZE);
  Serial.println(" bytes");

  epdClear(EPD_WHITE);
  return true;
}

// --------------------------------------------------
// FRAMEBUFFER DRAWING
// --------------------------------------------------

void epdClear(uint8_t color) {
  if (!epdBuffer) return;
  uint8_t packed = (color << 4) | color;
  memset(epdBuffer, packed, EPD_BUF_SIZE);
}

void epdSetPixel(int x, int y, uint8_t color) {
  if (!epdBuffer) return;
  if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;

  uint32_t idx = (uint32_t)y * EPD_ROW_BYTES + (x / 2);
  if (x & 1) {
    // Low nibble (right pixel)
    epdBuffer[idx] = (epdBuffer[idx] & 0xF0) | (color & 0x0F);
  } else {
    // High nibble (left pixel)
    epdBuffer[idx] = (epdBuffer[idx] & 0x0F) | ((color & 0x0F) << 4);
  }
}

uint8_t epdGetPixel(int x, int y) {
  if (!epdBuffer) return 0;
  if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return 0;

  uint32_t idx = (uint32_t)y * EPD_ROW_BYTES + (x / 2);
  if (x & 1) {
    return epdBuffer[idx] & 0x0F;
  } else {
    return (epdBuffer[idx] >> 4) & 0x0F;
  }
}

void epdDrawLine(int x0, int y0, int x1, int y1, uint8_t color) {
  // Bresenham's line algorithm
  int dx = abs(x1 - x0);
  int dy = -abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;

  while (true) {
    epdSetPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void epdDrawRect(int x, int y, int w, int h, uint8_t color) {
  epdDrawLine(x, y, x + w - 1, y, color);          // Top
  epdDrawLine(x, y + h - 1, x + w - 1, y + h - 1, color);  // Bottom
  epdDrawLine(x, y, x, y + h - 1, color);          // Left
  epdDrawLine(x + w - 1, y, x + w - 1, y + h - 1, color);  // Right
}

void epdFillRect(int x, int y, int w, int h, uint8_t color) {
  // Clamp to screen bounds
  int x0 = max(0, x);
  int y0 = max(0, y);
  int x1 = min(EPD_WIDTH, x + w);
  int y1 = min(EPD_HEIGHT, y + h);

  for (int row = y0; row < y1; row++) {
    for (int col = x0; col < x1; col++) {
      epdSetPixel(col, row, color);
    }
  }
}

// --------------------------------------------------
// WRITE BUFFER TO PANEL
// --------------------------------------------------

void epdWriteBuffer() {
  if (!epdBuffer) {
    Serial.println("No framebuffer!");
    return;
  }

  Serial.println("Writing buffer to panel (180° rotated)");
  epdStartTransmission();
  epdBeginData();

  // Send rotated 180°: bottom-to-top, each row reversed with nibbles swapped
  uint8_t rowTmp[EPD_ROW_BYTES];
  for (int y = EPD_HEIGHT - 1; y >= 0; y--) {
    const uint8_t* src = &epdBuffer[(uint32_t)y * EPD_ROW_BYTES];
    for (int x = 0; x < EPD_ROW_BYTES; x++) {
      uint8_t b = src[EPD_ROW_BYTES - 1 - x];
      rowTmp[x] = (b << 4) | (b >> 4);  // swap nibbles (left↔right pixel)
    }
    spi.transfer(rowTmp, EPD_ROW_BYTES);
  }

  epdEndData();
}

// --------------------------------------------------
// FRAMEBUFFER <-> SD CARD (.bin = raw 4bpp panel buffer)
// --------------------------------------------------

bool epdSaveBufferToFile(const char* path) {
  if (!epdBuffer) return false;

  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.print("Failed to open for write: ");
    Serial.println(path);
    return false;
  }

  size_t written = f.write(epdBuffer, EPD_BUF_SIZE);
  f.close();

  if (written != EPD_BUF_SIZE) {
    Serial.printf("Short write (%u/%u) to %s\n",
                  (unsigned)written, (unsigned)EPD_BUF_SIZE, path);
    SD.remove(path);
    return false;
  }
  return true;
}

bool epdLoadBufferFromFile(const char* path) {
  if (!epdBuffer) return false;

  File f = SD.open(path);
  if (!f) {
    Serial.print("Failed to open for read: ");
    Serial.println(path);
    return false;
  }

  if (f.size() != EPD_BUF_SIZE) {
    Serial.printf("Bad .bin size (%u) for %s\n", (unsigned)f.size(), path);
    f.close();
    return false;
  }

  size_t got = f.read(epdBuffer, EPD_BUF_SIZE);
  f.close();
  return got == EPD_BUF_SIZE;
}

// Push the current framebuffer to the panel and put it back to sleep.
// Safe to call repeatedly (epdPortInit performs a full hardware reset).
void epdDisplayCurrentBuffer() {
  epdPortInit();
  epdWriteBuffer();
  epdTurnOnDisplay();
  epdDeepSleep();
}

// --------------------------------------------------
// TEST PATTERNS (draw into buffer)
// --------------------------------------------------

void epdFillSolid(uint8_t colorIdx) {
  Serial.print("Fill: ");
  Serial.println(colorNames[colorIdx]);
  epdClear(colorValues[colorIdx]);
}

void epdDrawCheckerboard(int squareSize = 10) {
  Serial.println("Drawing checkerboard");
  for (int y = 0; y < EPD_HEIGHT; y++) {
    for (int x = 0; x < EPD_WIDTH; x++) {
      bool isWhite = ((x / squareSize) + (y / squareSize)) % 2;
      epdSetPixel(x, y, isWhite ? EPD_WHITE : EPD_BLACK);
    }
  }
}

void epdDrawHorizontalBars() {
  Serial.println("Drawing horizontal colour bars");
  int barHeight = EPD_HEIGHT / colorCount;
  for (int i = 0; i < colorCount; i++) {
    epdFillRect(0, i * barHeight, EPD_WIDTH, barHeight, colorValues[i]);
  }
}

void epdDrawVerticalBars() {
  Serial.println("Drawing vertical colour bars");
  int barWidth = EPD_WIDTH / colorCount;
  for (int i = 0; i < colorCount; i++) {
    epdFillRect(i * barWidth, 0, barWidth, EPD_HEIGHT, colorValues[i]);
  }
}
