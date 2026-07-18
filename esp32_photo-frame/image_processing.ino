// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Image Processing: JPEG/BMP decode → resize → dither
// --------------------------------------------------
//
// Requires the JPEGDEC fork with improved progressive JPEG support
// (Progressive-JPEG branch of https://github.com/will-rigby/JPEGDEC),
// junctioned/copied into the Arduino sketchbook libraries folder — NOT the
// Library Manager version. See README "Required Libraries".
//

#include <esp_heap_caps.h>

// --------------------------------------------------
// 6-colour palette (approximate RGB values for ACeP panel)
// Tweak these to match your specific e-ink panel.
// --------------------------------------------------

static const uint8_t paletteRGB[][3] = {
  {   0,   0,   0 },  // Black
  { 255, 255, 255 },  // White
  { 255, 236,   0 },  // Yellow
  { 200,  30,  30 },  // Red
  {  30,  30, 200 },  // Blue
  {  60, 150,  60 },  // Green
};

static const uint8_t paletteEPD[] = {
  EPD_BLACK, EPD_WHITE, EPD_YELLOW, EPD_RED, EPD_BLUE, EPD_GREEN
};

static const int PALETTE_SIZE = 6;

// --------------------------------------------------
// JPEG decoder globals
// --------------------------------------------------

static JPEGDEC    jpeg;
static uint16_t*  jpgDecodeBuf  = nullptr;   // RGB565 decode target
static int        jpgDecodeW    = 0;         // buffer width (memory stride)
static int        jpgDecodeH    = 0;         // buffer height
static int        jpgFillW      = 0;         // actual width JPEGDEC filled
static int        jpgFillH      = 0;         // actual height JPEGDEC filled
static File       jpgFile;                   // kept open during decode

// Progressive-JPEG coefficient buffer hooks: the fork allocates its multi-scan
// coefficient store (~3 B/px full-res for 4:2:0, 6 B/px for 4:4:4) through
// these. Must live in PSRAM; if the alloc fails (image too large) the library
// gracefully falls back to a 1/8 DC-only thumbnail decode.
static void* jpgCoefAlloc(uint32_t size) {
  Serial.printf("JPEGDEC coeff buffer request: %u bytes (PSRAM)\n", (unsigned)size);
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void jpgCoefFree(void* p) {
  heap_caps_free(p);
}

// --------------------------------------------------
// JPEGDEC file-I/O callbacks (SD card)
// --------------------------------------------------

static void* jpgOpen(const char* filename, int32_t* pSize) {
  jpgFile = SD.open(filename);
  if (!jpgFile) return nullptr;
  *pSize = jpgFile.size();
  return &jpgFile;
}

static void jpgClose(void* handle) {
  if (jpgFile) jpgFile.close();
}

static int32_t jpgRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  if (!jpgFile) return 0;
  return jpgFile.read(pBuf, len);
}

static int32_t jpgSeek(JPEGFILE* pFile, int32_t pos) {
  if (!jpgFile) return 0;
  return jpgFile.seek(pos);
}

// Copy decoded MCU blocks into the linear RGB565 buffer, tracking the actual
// region JPEGDEC fills (it may not match origW/scale exactly).
static int jpgDrawCB(JPEGDRAW* pDraw) {
  if (!jpgDecodeBuf) return 0;

  // The callback fires many times during a decode; yield occasionally so the
  // decode (which runs on the dither task, core 0) doesn't starve the idle task
  // and trip the watchdog.
  static uint16_t cbCount = 0;
  if ((++cbCount & 15) == 0) vTaskDelay(1);

  for (int y = 0; y < pDraw->iHeight; y++) {
    int dstY = pDraw->y + y;
    if (dstY >= jpgDecodeH) continue;

    int srcOff = y * pDraw->iWidth;
    int dstOff = dstY * jpgDecodeW + pDraw->x;
    int w      = min((int)pDraw->iWidth, jpgDecodeW - pDraw->x);
    if (w > 0) {
      memcpy(&jpgDecodeBuf[dstOff], &pDraw->pPixels[srcOff], w * 2);
      if (pDraw->x + w > jpgFillW) jpgFillW = pDraw->x + w;
      if (dstY + 1     > jpgFillH) jpgFillH = dstY + 1;
    }
  }
  return 1;
}

// --------------------------------------------------
// JPEG decode (with automatic down-scaling)
// --------------------------------------------------

static bool decodeJPEG(const char* path) {
  int rc = jpeg.open(path, jpgOpen, jpgClose, jpgRead, jpgSeek, jpgDrawCB);
  if (!rc) {
    Serial.println("JPEG open failed");
    return false;
  }

  // Must be set after open() — open() resets the decoder state.
  jpeg.setBufferHooks(jpgCoefAlloc, jpgCoefFree);

  int origW = jpeg.getWidth();
  int origH = jpeg.getHeight();
  Serial.printf("JPEG %dx%d\n", origW, origH);

  // Pick the most aggressive scale where BOTH decoded dims >= EPD dims
  // (cover mode — we'll crop after resize)
  struct { int div; int flag; } scales[] = {
    { 8, JPEG_SCALE_EIGHTH  },
    { 4, JPEG_SCALE_QUARTER },
    { 2, JPEG_SCALE_HALF    },
    { 1, 0                  }
  };

  int best = 3;  // fallback: no scaling
  for (int i = 0; i < 4; i++) {
    if (origW / scales[i].div >= EPD_WIDTH &&
        origH / scales[i].div >= EPD_HEIGHT) {
      best = i;
      break;
    }
  }

  jpgDecodeW = origW / scales[best].div;
  jpgDecodeH = origH / scales[best].div;
  Serial.printf("Decode at 1/%d → %dx%d\n",
                scales[best].div, jpgDecodeW, jpgDecodeH);

  size_t bufBytes = (size_t)jpgDecodeW * jpgDecodeH * 2;
  jpgDecodeBuf = (uint16_t*)ps_malloc(bufBytes);
  if (!jpgDecodeBuf) {
    Serial.println("Decode buffer alloc failed");
    jpeg.close();
    return false;
  }
  memset(jpgDecodeBuf, 0, bufBytes);

  jpgFillW = 0;
  jpgFillH = 0;

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  rc = jpeg.decode(0, 0, scales[best].flag);
  int decodeMode = jpeg.getDecodeMode();   // read before close()
  jpeg.close();

  if (!rc) {
    Serial.println("JPEG decode failed");
    free(jpgDecodeBuf);
    jpgDecodeBuf = nullptr;
    return false;
  }

  if (decodeMode == JPEG_DECODE_PROGRESSIVE_FULL) {
    Serial.println("Progressive JPEG: full multi-scan decode");
  } else if (decodeMode == JPEG_DECODE_PROGRESSIVE_THUMBNAIL) {
    // The library forces 1/8 scale in this mode; the smaller filled region is
    // handled by the jpgFillW/H tracking and upscaled by resizeToDisplay().
    Serial.println("WARN: progressive JPEG fell back to 1/8 DC-only thumbnail "
                   "(coefficient alloc failed or unsupported subsampling) — "
                   "output will be soft");
  }

  Serial.printf("JPEG decoded OK — filled %dx%d of %dx%d buffer\n",
                jpgFillW, jpgFillH, jpgDecodeW, jpgDecodeH);
  return true;
}

// --------------------------------------------------
// BMP decode (24-bit uncompressed only)
// --------------------------------------------------

static bool decodeBMP(const char* path) {
  File f = SD.open(path);
  if (!f) { Serial.println("BMP open failed"); return false; }

  // Read BMP header
  uint8_t hdr[54];
  if (f.read(hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
    Serial.println("Not a valid BMP");
    f.close();
    return false;
  }

  uint32_t dataOffset = *(uint32_t*)&hdr[10];
  int32_t  bmpW       = *(int32_t*)&hdr[18];
  int32_t  bmpH       = *(int32_t*)&hdr[22];
  uint16_t bpp        = *(uint16_t*)&hdr[28];
  uint32_t compress   = *(uint32_t*)&hdr[30];

  bool bottomUp = (bmpH > 0);
  if (bmpH < 0) bmpH = -bmpH;

  Serial.printf("BMP %dx%d %dbpp\n", bmpW, bmpH, bpp);
  if (bpp != 24 || compress != 0) {
    Serial.println("Only 24-bit uncompressed BMP supported");
    f.close();
    return false;
  }

  jpgDecodeW = bmpW;
  jpgDecodeH = bmpH;
  jpgFillW   = bmpW;   // BMP path fills the whole buffer
  jpgFillH   = bmpH;

  size_t bufBytes = (size_t)bmpW * bmpH * 2;
  jpgDecodeBuf = (uint16_t*)ps_malloc(bufBytes);
  if (!jpgDecodeBuf) {
    Serial.println("BMP buffer alloc failed");
    f.close();
    return false;
  }

  int rowPad = (4 - (bmpW * 3) % 4) % 4;
  uint8_t* rowBuf = (uint8_t*)malloc(bmpW * 3 + rowPad);
  if (!rowBuf) {
    Serial.println("BMP row buffer alloc failed");
    free(jpgDecodeBuf); jpgDecodeBuf = nullptr;
    f.close();
    return false;
  }

  f.seek(dataOffset);

  for (int y = 0; y < bmpH; y++) {
    if ((y & 31) == 0) vTaskDelay(1);   // yield so core-0 idle/WDT stays happy
    int dstY = bottomUp ? (bmpH - 1 - y) : y;
    f.read(rowBuf, bmpW * 3 + rowPad);

    for (int x = 0; x < bmpW; x++) {
      uint8_t b = rowBuf[x * 3 + 0];
      uint8_t g = rowBuf[x * 3 + 1];
      uint8_t r = rowBuf[x * 3 + 2];
      // Pack to RGB565 (little-endian on ESP32 = native uint16_t)
      jpgDecodeBuf[dstY * bmpW + x] =
          ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
  }

  free(rowBuf);
  f.close();
  Serial.println("BMP decoded OK");
  return true;
}

// --------------------------------------------------
// RGB565 helpers
// --------------------------------------------------

static inline void rgb565to888(uint16_t c, int& r, int& g, int& b) {
  r = (c >> 11) & 0x1F; r = (r << 3) | (r >> 2);
  g = (c >>  5) & 0x3F; g = (g << 2) | (g >> 4);
  b =  c        & 0x1F; b = (b << 3) | (b >> 2);
}

// Bilinear sample from an RGB565 buffer. srcW/srcH are the valid content size;
// srcStride is the buffer's row stride in pixels (may be larger than srcW).
static void sampleBilinear(float sx, float sy,
                           int srcW, int srcH, int srcStride,
                           const uint16_t* src,
                           int& r, int& g, int& b) {
  int x0 = (int)sx;
  int y0 = (int)sy;
  int x1 = min(x0 + 1, srcW - 1);
  int y1 = min(y0 + 1, srcH - 1);
  float fx = sx - x0;
  float fy = sy - y0;

  int r00, g00, b00, r10, g10, b10;
  int r01, g01, b01, r11, g11, b11;
  rgb565to888(src[y0 * srcStride + x0], r00, g00, b00);
  rgb565to888(src[y0 * srcStride + x1], r10, g10, b10);
  rgb565to888(src[y1 * srcStride + x0], r01, g01, b01);
  rgb565to888(src[y1 * srcStride + x1], r11, g11, b11);

  float w00 = (1 - fx) * (1 - fy);
  float w10 = fx       * (1 - fy);
  float w01 = (1 - fx) * fy;
  float w11 = fx       * fy;

  r = (int)(r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11);
  g = (int)(g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11);
  b = (int)(b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11);
}

// --------------------------------------------------
// Bilinear resize to EPD_WIDTH × EPD_HEIGHT (cover + centre crop)
// Returns RGB888 buffer (3 bytes / pixel) in PSRAM.
// --------------------------------------------------

static uint8_t* resizeToDisplay(int srcW, int srcH, int srcStride,
                                const uint16_t* src) {
  size_t outBytes = (size_t)EPD_WIDTH * EPD_HEIGHT * 3;
  uint8_t* out = (uint8_t*)ps_malloc(outBytes);
  if (!out) {
    Serial.println("Resize buffer alloc failed");
    return nullptr;
  }

  // Cover: scale so both dims fill, then centre-crop the excess
  float scale = max((float)EPD_WIDTH  / srcW,
                    (float)EPD_HEIGHT / srcH);
  float cropW = EPD_WIDTH  / scale;
  float cropH = EPD_HEIGHT / scale;
  float startX = (srcW - cropW) / 2.0f;
  float startY = (srcH - cropH) / 2.0f;

  Serial.printf("Resize: scale=%.3f  crop origin=(%.0f,%.0f)\n",
                scale, startX, startY);

  for (int dy = 0; dy < EPD_HEIGHT; dy++) {
    if ((dy & 31) == 0) vTaskDelay(1);   // yield so core-0 idle/WDT stays happy
    for (int dx = 0; dx < EPD_WIDTH; dx++) {
      float sx = startX + dx / scale;
      float sy = startY + dy / scale;
      sx = max(0.0f, min(sx, (float)(srcW - 1)));
      sy = max(0.0f, min(sy, (float)(srcH - 1)));

      int r, g, b;
      sampleBilinear(sx, sy, srcW, srcH, srcStride, src, r, g, b);

      int idx = (dy * EPD_WIDTH + dx) * 3;
      out[idx + 0] = r;
      out[idx + 1] = g;
      out[idx + 2] = b;
    }
  }

  Serial.println("Resize complete");
  return out;
}

// --------------------------------------------------
// Nearest palette colour (weighted Euclidean distance)
// --------------------------------------------------

static int nearestPaletteColor(int r, int g, int b) {
  int bestIdx  = 0;
  int bestDist = INT_MAX;

  for (int i = 0; i < PALETTE_SIZE; i++) {
    int dr = r - paletteRGB[i][0];
    int dg = g - paletteRGB[i][1];
    int db = b - paletteRGB[i][2];
    // Perceptual weighting: green > red > blue
    int dist = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
    if (dist < bestDist) {
      bestDist = dist;
      bestIdx  = i;
    }
  }
  return bestIdx;
}

// --------------------------------------------------
// Floyd–Steinberg dither (RGB888 → 6-colour EPD buffer)
// --------------------------------------------------

static bool floydSteinbergDither(uint8_t* dst, uint8_t* rgb, int w, int h) {
  Serial.println("Floyd-Steinberg dithering...");

  // Two rows of signed error accumulators (R, G, B per pixel)
  int16_t* errCur  = (int16_t*)calloc(w * 3, sizeof(int16_t));
  int16_t* errNext = (int16_t*)calloc(w * 3, sizeof(int16_t));
  if (!errCur || !errNext) {
    Serial.println("Dither error-buffer alloc failed");
    free(errCur); free(errNext);
    return false;
  }

  // Seed first row
  for (int x = 0; x < w; x++) {
    int si = x * 3;
    errCur[si + 0] = rgb[si + 0];
    errCur[si + 1] = rgb[si + 1];
    errCur[si + 2] = rgb[si + 2];
  }

  for (int y = 0; y < h; y++) {
    if ((y & 31) == 0) vTaskDelay(1);   // yield so core-0 idle/WDT stays happy

    // Prepare next-row base values
    if (y + 1 < h) {
      int off = (y + 1) * w * 3;
      for (int x = 0; x < w; x++) {
        int si = x * 3;
        errNext[si + 0] = rgb[off + si + 0];
        errNext[si + 1] = rgb[off + si + 1];
        errNext[si + 2] = rgb[off + si + 2];
      }
    } else {
      memset(errNext, 0, w * 3 * sizeof(int16_t));
    }

    for (int x = 0; x < w; x++) {
      int si = x * 3;

      // Clamp to [0..255]
      int r = max(0, min(255, (int)errCur[si + 0]));
      int g = max(0, min(255, (int)errCur[si + 1]));
      int b = max(0, min(255, (int)errCur[si + 2]));

      int palIdx = nearestPaletteColor(r, g, b);
      epdSetPixelBuf(dst, x, y, paletteEPD[palIdx]);

      int eR = r - paletteRGB[palIdx][0];
      int eG = g - paletteRGB[palIdx][1];
      int eB = b - paletteRGB[palIdx][2];

      // Distribute error  →  7/16  ↙ 3/16  ↓ 5/16  ↘ 1/16
      if (x + 1 < w) {
        errCur[(x+1)*3+0] += eR * 7 / 16;
        errCur[(x+1)*3+1] += eG * 7 / 16;
        errCur[(x+1)*3+2] += eB * 7 / 16;
      }
      if (x > 0 && y + 1 < h) {
        errNext[(x-1)*3+0] += eR * 3 / 16;
        errNext[(x-1)*3+1] += eG * 3 / 16;
        errNext[(x-1)*3+2] += eB * 3 / 16;
      }
      if (y + 1 < h) {
        errNext[x*3+0] += eR * 5 / 16;
        errNext[x*3+1] += eG * 5 / 16;
        errNext[x*3+2] += eB * 5 / 16;
      }
      if (x + 1 < w && y + 1 < h) {
        errNext[(x+1)*3+0] += eR / 16;
        errNext[(x+1)*3+1] += eG / 16;
        errNext[(x+1)*3+2] += eB / 16;
      }
    }

    // Swap rows
    int16_t* tmp = errCur;
    errCur  = errNext;
    errNext = tmp;
  }

  free(errCur);
  free(errNext);
  Serial.println("Dithering complete");
  return true;
}

// --------------------------------------------------
// Two-phase pipeline (so the background task can hold the SD lock only while
// it actually touches the card):
//   imgDecode()         — reads the source file from SD into jpgDecodeBuf  [SD]
//   imgRenderToBuffer() — resize + dither into the caller's buffer         [CPU]
// The JPEGDEC globals (jpgDecodeBuf, jpgFillW/H, …) are touched only by the
// dither task, so they need no locking themselves.
// --------------------------------------------------

bool imgDecode(const char* filepath) {
  Serial.print("Decoding: ");
  Serial.println(filepath);

  String path = String(filepath);
  path.toLowerCase();

  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    return decodeJPEG(filepath);
  } else if (path.endsWith(".bmp")) {
    return decodeBMP(filepath);
  }
  Serial.println("Unsupported image format (only JPEG/BMP)");
  return false;
}

bool imgRenderToBuffer(uint8_t* target) {
  // Resize from the region actually filled by the decoder (jpgFillW/H), using
  // the buffer width (jpgDecodeW) as the row stride. Robust even if the decoder
  // filled less than the full allocated buffer.
  int contentW = (jpgFillW > 0 && jpgFillW <= jpgDecodeW) ? jpgFillW : jpgDecodeW;
  int contentH = (jpgFillH > 0 && jpgFillH <= jpgDecodeH) ? jpgFillH : jpgDecodeH;
  Serial.printf("Resizing from content %dx%d (stride %d)\n",
                contentW, contentH, jpgDecodeW);
  uint8_t* resized = resizeToDisplay(contentW, contentH, jpgDecodeW, jpgDecodeBuf);

  free(jpgDecodeBuf);
  jpgDecodeBuf = nullptr;

  if (!resized) return false;

  bool ok = floydSteinbergDither(target, resized, EPD_WIDTH, EPD_HEIGHT);

  free(resized);
  if (!ok) return false;
  Serial.println("Image rendering complete");
  return true;
}

// --------------------------------------------------
// Gallery thumbnail backfill (device side)
// --------------------------------------------------
//
// New uploads get a JPEG thumbnail from the browser; photos that predate that
// get one here the first time they are displayed. The dithered framebuffer is
// box-averaged 4x4 back into smooth RGB (the dither noise averages out) and
// written as an uncompressed 200x120 24bpp BMP (~72 KB) — no JPEG encoder
// needed, and the thumbnail shows exactly what the panel shows.

#define THUMB_W (EPD_WIDTH / 4)
#define THUMB_H (EPD_HEIGHT / 4)

static void bmpPutU32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void bmpPutU16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void thumbEnsureFromBuffer(const String& base) {
  if (!epdBuffer) return;
  if (SD.exists(String(THUMBS_DIR "/") + base + ".jpg")) return;
  String bp = String(THUMBS_DIR "/") + base + ".bmp";
  if (SD.exists(bp)) return;
  if (!SD.exists(THUMBS_DIR)) SD.mkdir(THUMBS_DIR);

  // EPD colour code -> RGB (codes outside the palette render white)
  uint8_t lut[16][3];
  memset(lut, 255, sizeof(lut));
  for (int i = 0; i < PALETTE_SIZE; i++)
    memcpy(lut[paletteEPD[i]], paletteRGB[i], 3);

  File f = SD.open(bp.c_str(), FILE_WRITE);
  if (!f) { Serial.printf("Thumb write failed: %s\n", bp.c_str()); return; }

  const uint32_t rowBytes = THUMB_W * 3;   // 600, already a multiple of 4
  uint8_t hdr[54] = { 0 };
  hdr[0] = 'B'; hdr[1] = 'M';
  bmpPutU32(hdr + 2,  54 + rowBytes * THUMB_H);   // file size
  bmpPutU32(hdr + 10, 54);                        // pixel data offset
  bmpPutU32(hdr + 14, 40);                        // BITMAPINFOHEADER size
  bmpPutU32(hdr + 18, THUMB_W);
  bmpPutU32(hdr + 22, THUMB_H);                   // positive height = bottom-up
  bmpPutU16(hdr + 26, 1);                         // planes
  bmpPutU16(hdr + 28, 24);                        // bits per pixel
  bmpPutU32(hdr + 34, rowBytes * THUMB_H);        // image size
  f.write(hdr, sizeof(hdr));

  uint8_t row[THUMB_W * 3];
  for (int ty = THUMB_H - 1; ty >= 0; ty--) {     // BMP rows are bottom-up
    uint8_t* o = row;
    for (int tx = 0; tx < THUMB_W; tx++) {
      uint16_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 4; dx++) {
          const uint8_t* c = lut[epdGetPixel(tx * 4 + dx, ty * 4 + dy) & 0x0F];
          r += c[0]; g += c[1]; b += c[2];
        }
      *o++ = b >> 4; *o++ = g >> 4; *o++ = r >> 4;   // BGR, /16
    }
    f.write(row, sizeof(row));
  }
  f.close();
  Serial.printf("Thumbnail written: %s\n", bp.c_str());
}
