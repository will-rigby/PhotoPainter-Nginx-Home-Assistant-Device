# ESP32-S3 E-Ink Photo Frame

[![Watch the video](https://img.youtube.com/vi/A8poN4a6Aiw/0.jpg)](https://www.youtube.com/watch?v=A8poN4a6Aiw)
_YouTube Overview (covers the earlier nginx-based version)_

Custom firmware for the [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter), a battery-powered e-ink photo frame featuring an ESP32-S3 and a 7.3" Spectra ACeP 6-colour display (800×480). This project replaces the stock firmware with an Arduino-based sketch.

The frame is **self-contained**: when plugged into USB-C power it runs a built-in **web server** where you browse a **gallery** of photos on the SD card and **upload** new ones (one or many at a time) from your phone or computer. Browser uploads store an **800x480 frame-ready copy**, with optional crop and rotation editing; the untouched original stays on your phone or computer. The first time a photo is displayed it is Floyd–Steinberg dithered to the 6-colour panel format **on the fly**, and the result is cached on the SD card so every later display is a fast load. On battery the frame deep-sleeps, rotating through your photos and reporting temperature, humidity, and battery voltage to Home Assistant via MQTT. **No external image server is required.**

> **Note:** This firmware is **not** based on the official Waveshare ESP-IDF demo. It is written from scratch using the Arduino framework. The stock firmware is not required, but an SD card **is** (it stores your photos).

## How it works

The frame has two modes, chosen automatically at boot based on whether USB-C power is present (detected via the AXP2101 PMU):

- **Server mode (USB-C plugged in)** — the frame stays awake and runs the web UI. It joins your home WiFi if it can (see below), otherwise it hosts its own setup hotspot. While powered it also rotates the displayed photo and keeps reporting sensors to MQTT.
- **Battery mode (unplugged)** — the frame deep-sleeps to conserve battery. Every ~5 minutes it wakes to report sensors via MQTT; every ~30 minutes it picks a random stored photo and refreshes the display. The web server does **not** run on battery.

### WiFi behaviour

By default (no saved network) the frame **hosts its own WPA2 access point** named `PhotoFrame-Setup`. Connect to it and open the web page to set your home WiFi. Once saved and restarted, the frame connects to your network as a normal client. Open `http://photoframe.local/` or its IP address on your LAN. The WiFi page lets you choose a unique hostname for each frame. If the saved network can't be found, it falls back to hosting its own hotspot again so you can always reconfigure it.

## Features

- Built-in web server (USB-powered): photo **gallery** (shows stored photos), multi-file **upload**, **delete**, and "show now"
- Optional crop, zoom, and rotation before batch upload; browser uploads store 800x480 JPEGs and matching thumbnails
- Safe streamed uploads with per-file progress, explicit errors, retry tokens, and interruption cleanup
- Versioned dither caches built **lazily at display time**, with independent identities for each complete filename
- Responsive HTTP during decoding/panel refresh, queued display requests, and device status
- Gallery pagination (24 photos per page) and `photoframe.local` discovery
- Full **progressive JPEG** decoding via a JPEGDEC fork that buffers the multi-scan coefficients in PSRAM (with automatic 1/8-thumbnail fallback for very large progressive files)
- Self-hosting WPA2 access point with web-based WiFi configuration; STA with AP fallback
- Web-based MQTT configuration (separate settings page)
- Settings persisted to flash (NVS) — no recompiling to change WiFi/MQTT
- Deep sleep on battery (~5 min wake cycle, display refresh every 30 min)
- GPIO4 button press triggers immediate photo change (battery mode)
- SHTC3 temperature & humidity + battery voltage reported to Home Assistant via MQTT auto-discovery

## Hardware

This project targets the [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/esp32-s3-photopainter.htm) which includes:

- ESP32-S3 with 16MB flash and OPI PSRAM
- 7.3" Spectra ACeP e-ink display (800×480, 6-colour: Black/White/Yellow/Red/Blue/Green)
- AXP2101 PMU with LiPo battery management (I2C) — also used to detect USB-C power
- SHTC3 temperature/humidity sensor (I2C, same bus as AXP2101)
- MicroSD card slot (SPI) — **required**; this is where your photos are stored
- BOOT button on GPIO4 (used to trigger an immediate photo change)
- PWR button for power on/off

## Setup

The Arduino sketch lives in the [esp32_photo-frame/](esp32_photo-frame/) folder. Open `esp32_photo-frame/esp32_photo-frame.ino` in the Arduino IDE (the folder name matches the main sketch, so all the `.ino`/`.h` files load as one sketch).

### 1. Arduino IDE Board Settings

| Setting       | Value              |
|---------------|--------------------|
| Board         | ESP32S3 Dev Module |
| Flash Size    | **16MB**           |
| PSRAM         | **OPI PSRAM**      |
| USB CDC On Boot | Enabled          |
| Partition Scheme | Default          |

### 2. Required Libraries

Install the following via the Arduino Library Manager:

- **XPowersLib** — AXP2101 PMU driver
- **PubSubClient** — MQTT client by Nick O'Leary

**JPEGDEC** must be the fork with improved progressive JPEG support, **not** the Library Manager version (make sure the stock JPEGDEC is *not* also installed — two copies of the same library conflict). Clone the fork, switch to its `Progressive-JPEG` branch, and link it into your sketchbook libraries folder, e.g. on Windows:

```powershell
git clone https://github.com/will-rigby/JPEGDEC.git
git -C JPEGDEC switch Progressive-JPEG

New-Item -ItemType Junction `
  -Path "$env:USERPROFILE\Documents\Arduino\libraries\JPEGDEC" `
  -Target (Resolve-Path JPEGDEC)
```

The fork decodes progressive JPEGs in full by buffering the multi-scan DCT coefficients in PSRAM (~3 bytes/pixel of the full image for 4:2:0, 6 bytes/pixel for 4:4:4). If a progressive file is too large for the coefficient buffer, it automatically falls back to the old 1/8-scale DC-only thumbnail decode instead of failing.

`WiFi`, `WebServer`, `DNSServer`, `Preferences`, `SD`, `SPI`, and `Wire` ship with the ESP32 Arduino core — nothing extra to install.

### 3. (Optional) factory defaults

Configuration now happens in the browser, so you do **not** need to edit any files to build. The defaults live in [config_defaults.h](esp32_photo-frame/config_defaults.h) — most importantly the setup hotspot:

```cpp
#define AP_SSID       "PhotoFrame-Setup"
#define AP_PASSWORD   "photoframe"        // WPA2 — must be >= 8 characters
#define ROTATE_MINUTES 30
```

If you'd like a device to come up already knowing your WiFi/MQTT (so you can skip the browser step), copy `esp32_photo-frame/secrets_template.h` to `esp32_photo-frame/secrets.h` (gitignored) and uncomment the values you want to pre-seed. Anything you set there is overridden the moment you save settings in the web UI.

### 4. Flash & first-time configuration

1. Flash the sketch and **keep the frame on USB-C power**.
2. On a phone/laptop, join the WiFi network **`PhotoFrame-Setup`** (password `photoframe` unless you changed it).
3. Open a browser to **`http://192.168.4.1/`** (most devices pop up the captive portal automatically).
4. Go to the **WiFi** page, enter your home network, and **Save**. Unplug and replug the frame to join it.
5. Reconnect your phone/laptop to your home WiFi and open **`http://photoframe.local/`** (or your configured hostname). If your network/client does not support mDNS, use the frame's IP from the router or serial console.

### 5. Using the gallery

- **Choose photos** - select JPEG/BMP files. Each gets a centered preview; optionally use **Edit crop** to drag, zoom, rotate by 90 degrees, or reset it. The browser handles EXIF orientation.
- **Upload** - files are converted and uploaded sequentially as **800x480 JPEGs**. Only this prepared copy is stored in `/originals`; keep your original photos on your phone/computer. Conversion failures are shown instead of silently sending unprepared files.
- **Retry** - each file reports its own result. Retry after interruption safely within the same page: its token prevents duplicating a committed upload. A failed thumbnail retries the thumbnail only. Reloading the page starts a new batch, so check the gallery before uploading again.
- **Gallery** - Previous/Next browse 24 photos at a time. Thumbnails load lazily, falling back to the stored source. Cards report cached, uncached, or processing-failed state.
- **Show** - returns immediately. Status progresses through preparing and refreshing; a panel refresh takes roughly 30 seconds. One request can wait behind the active refresh; a newer Show replaces that pending request. The displayed-photo label changes only after successful refresh.
- **Device status** - current/pending photo, next rotation, free SD space, WiFi signal/IP/hostname, MQTT status, and the most recent error. Observation ages distinguish sampled telemetry from current readings.
- **Clear dither cache** - removes generated panel buffers and clears processing failures; images rebuild when displayed. Busy displays/uploads prevent conflicting maintenance.
- **Delete** - removes a source photo and its generated files. Active or queued photos are protected from deletion.

WiFi and MQTT password fields are blank when loaded. **Blank means keep the saved password**; use **Clear saved password** explicitly to remove it. WiFi/hostname changes require restarting the frame. MQTT settings apply after current control work finishes.

### 6. MQTT / Home Assistant

Open the **MQTT** settings page in the web UI and enter your broker host, port, and (optionally) username/password, then Save. Leave the host blank to disable MQTT. Sensor reporting starts on the next cycle.

#### 6a. MQTT Broker (Mosquitto)

If you haven't already, install the Mosquitto MQTT broker. On Home Assistant OS the easiest way is the official **Mosquitto broker** add-on:

1. In Home Assistant go to **Settings → Add-ons → Add-on Store**.
2. Search for **Mosquitto broker** and install it.
3. Start the add-on.

#### 6b. Create a Home Assistant user for the device

1. Go to **Settings → People → Users**.
2. Click **Add User** — e.g. display name `Photo Frame`, username `photoframe`, a strong password.
3. Toggle **"Can only log in from the local network"** on (recommended) and leave **Administrator** off.
4. Enter that username/password on the frame's **MQTT** settings page.

The Mosquitto add-on automatically allows any HA local user to authenticate, so no extra Mosquitto config is needed.

#### 6c. Auto-Discovery

The device publishes MQTT auto-discovery messages on connection. Once it reports its first reading, three entities appear automatically in Home Assistant:

- **Photo Frame Temperature** — °C from the SHTC3 sensor
- **Photo Frame Humidity** — % RH from the SHTC3 sensor
- **Photo Frame Battery** — battery voltage in V from the AXP2101 PMU

No manual MQTT entity configuration is needed — just ensure the HA MQTT integration is set up against the same broker.

## File Overview

| File                    | Description                                                        |
|-------------------------|--------------------------------------------------------------------|
| `esp32_photo-frame.ino` | Main sketch — power-mode selection, setup/loop, display rotation, lazy render + dither cache |
| `spectra73.ino`         | Spectra 7.3" ACeP e-ink panel driver + framebuffer ⇄ SD `.bin` I/O |
| `image_processing.ino`  | JPEG/BMP decode, bilinear resize, Floyd–Steinberg dither           |
| `network.ino`           | WiFi station connect + WPA2 access-point fallback                  |
| `webserver.ino`         | Dedicated HTTP task, paginated gallery, uploads, status, settings |
| `runtime.ino`           | Display scheduling, shared status, safe shutdown, control-task maintenance |
| `photo_store.cpp` / `storage.h` | Revisioned metadata, upload receipts, migration, and serialized SD access |
| `web/` / `web_assets.h` | Editable browser UI and generated flash-resident assets |
| `config.ino` / `config.h` | Persistent configuration (Preferences/NVS)                       |
| `config_defaults.h`     | Compile-time factory defaults (AP name/password, rotate interval)  |
| `sdcard.ino`            | SD card initialisation                                             |
| `sensor.ino`            | SHTC3 sensor + MQTT reporting with HA auto-discovery               |
| `secrets_template.h`    | Optional template to pre-seed WiFi/MQTT defaults                   |

## Storage layout (SD card)

- `/originals/<filename>` - display sources. Browser uploads are prepared 800x480 JPEGs; existing JPEG/BMP files copied directly to the card remain supported without renaming.
- `/photo-meta/<filename-hash>` - fixed-size, checksummed source/thumbnail revisions and source size/modification time. The full filename **including extension** identifies a photo; FAT names are treated case-insensitively.
- `/dithered/<filename-hash>-<source-revision>-r2.bin` - 192,000-byte display buffers, keyed by source and rendering version.
- `/thumbs/<filename-hash>-<thumbnail-revision>.jpg|.bmp` - browser JPEG thumbnails or device-generated BMP fallbacks.
- `/upload-temp/` - incomplete uploads, excluded from the gallery and rotation. Removed after failure or on next boot.
- `/upload-receipts/<token>` - persistent retry receipts. A receipt is valid only while its matching source/revision exists.

On the first upgraded SD initialization, **legacy generated dither buffers and thumbnails are discarded** and rebuilt lazily. Source photos and their filenames are preserved. `.bak` metadata records support recovery if power is lost between renames. Back up the SD card before firmware upgrades.

Files added/changed outside the web UI are registered when enumerated or selected, using their complete name, size, and modification time. If an external editor preserves both size and timestamp, remove the corresponding metadata and generated files while the card is offline to force a fresh import. A full card can still serve existing photos using reproducible import revisions, even if their sidecar cannot be saved.

## Image pipeline and task ownership

1. Browser conversion produces the selected 800x480 crop plus a matching 400x240 JPEG thumbnail.
2. The HTTP task streams the payload into a temporary file, checks every write and image signature, persists its receipt/metadata, then renames the completed source. Maximum raw HTTP upload: **20 MiB/photo**, **256 KiB/thumbnail**, one file per request. Full decoding still happens at display time.
3. The control task selects a photo and exclusively owns JPEGDEC, image buffers, panel SPI, sensors/PMU, and MQTT. Cache misses use the existing JPEG/BMP resize/dither pipeline; hits load the panel buffer.
4. The control task refreshes the panel while the separate HTTP task continues serving requests. SD operations are serialized individually; locks are not held during panel waits, CPU rendering, or socket writes.
5. GPIO4, rotation, startup display, Show, and the diagnostic pattern share the display scheduler. Automatic rotation cannot replace manual pending work. Conflicting deletion/cache-clear/thumbnail writes return a busy error.
6. USB removal stops new work, closes/cleans uploads, finishes the active panel operation, stops HTTP/DNS/mDNS, and enters battery sleep. Battery wakeups do not start an HTTP task.

The synchronous HTTP library still serves **one client at a time**. Another HTTP transfer can delay status polling; moving the panel work off HTTP removes the long refresh stall, not this library limit.

## HTTP interface

JSON responses use `Cache-Control: no-store`; failures return `{ "ok": false, "error": "..." }` with a non-2xx status. Image URLs include revision tokens and cache for a week. Unversioned image requests revalidate with ETags.

| Route | Behavior |
|---|---|
| `GET /api/photos?limit=24&cursor=...` | `{photos, next_cursor, generation}`; limit 1-48. Photo entries include `name`, `revision`, `thumbnail_revision`, `cached`, and `failed`. A changed generation returns HTTP 409 with `restart:true`. |
| `GET /api/status` | Display stage, active/pending jobs, current photo, next rotation, storage, WiFi, MQTT, observation times, and last error. Time fields are milliseconds since boot. |
| `GET /api/config` | Non-secret settings and booleans indicating whether passwords are saved. |
| `GET /original?file=...&v=...` / `GET /thumb?file=...&v=...` | Source/thumbnail; expired revisions return 404. Thumbnail absence permits source fallback. |
| `POST /upload?token=<32 lowercase hex characters>` | Multipart `file`; HTTP 200 includes `ok`, stored `name`, and `revision`. Reuse the token only to retry the same payload. Legacy requests without a token still work but cannot deduplicate a lost response. |
| `POST /thumb?name=...` | Multipart JPEG thumbnail; independent commit/result from the source upload. |
| `POST /show?file=...` | HTTP **202** with `job_id`; observe completion in `/api/status`. Latest pending request wins. |
| `POST /test` / `POST /redither` | HTTP 202 diagnostic/maintenance job; HTTP 409 when conflicting work is active. |
| `POST /delete?file=...` | Deletes source and derived files; HTTP 409 for a protected photo. |
| `POST /api/wifi` | Form fields `ssid`, `pass`, `hostname`, optional `clear_password=true`. |
| `POST /api/mqtt` | Form fields `host`, `port`, `user`, `pass`, optional `clear_password=true`. Port must be 1-65535; omission defaults to 1883. |

**Compatibility changes:** `/api/photos` now returns a paginated object instead of a bare array. Show/test/cache-clear are asynchronous. Clients must check HTTP status and poll job state rather than treating acceptance as display completion. Settings remain in NVS; older individual keys are read until a new settings group is saved successfully.

## Development and verification

The editable browser sources are in `web/`. Generate the checked-in PROGMEM header after editing them:

```powershell
python tools/embed_web.py
python tools/embed_web.py --check
npm.cmd ci
npm.cmd test
npm.cmd run test:browser
```

Arduino IDE builds use the committed header and require no Node/Python tooling. Browser tests use headless Microsoft Edge with desktop and mobile viewports. See [tests/README.md](tests/README.md) for firmware logic tests and the physical-device acceptance checklist.

Reproducible firmware configuration used for validation:

- ESP32 Arduino core **3.3.8**
- PubSubClient **2.8**, XPowersLib **0.3.3**
- JPEGDEC `Progressive-JPEG`, commit **d09cf27644cefdc5efb63d19e95de43559938cd4**
- ESP32S3 Dev Module, **16 MB flash**, **OPI PSRAM**, **USB CDC on boot**, default partition scheme

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc" --build-path .build/firmware esp32_photo-frame
```

Compilation does not flash a connected device. Keep build output and local test dependencies out of source control.

## Power / mode behaviour

| Condition | Behaviour |
|-----------|-----------|
| **USB-C connected** | Web server runs; WiFi joins home network or hosts `PhotoFrame-Setup`; display rotates and MQTT reports on timers; never deep-sleeps. |
| **On battery** | Deep sleep. Wakes every ~5 min to report sensors via MQTT; every ~30 min rotates to a random stored photo. Button (GPIO4) forces an immediate refresh. |

## Third-Party Libraries & Licenses

This project uses the following third-party libraries. Full license texts are available in each library's repository.

| Library | Author | License | Repository |
|---------|--------|---------|------------|
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | Larry Bank / BitBank Software, Inc. | Apache-2.0 | [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC) (used via the [will-rigby fork](https://github.com/will-rigby/JPEGDEC), `Progressive-JPEG` branch) |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | Lewis He / LilyGO | MIT | [lewisxhe/XPowersLib](https://github.com/lewisxhe/XPowersLib) |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | Nick O'Leary | MIT | [knolleary/pubsubclient](https://github.com/knolleary/pubsubclient) |

The WiFi, WebServer, DNSServer, Preferences, SD, SPI, and Wire libraries are part of the [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32) by Espressif Systems, licensed under the **GNU Lesser General Public License v2.1 (LGPL-2.1)**.

## License

This project is licensed under the **BSD Zero Clause License (0BSD)**. See [LICENSE](LICENSE).

In short: you can use, modify, and redistribute this project for any purpose, including commercial use, without attribution. The software is provided "as is" without warranty.

Third-party libraries remain under their own licenses (MIT, Apache-2.0, LGPL-2.1). If you distribute binaries/firmware, keep the relevant third-party notices and provide source/build information as required by those licenses.
