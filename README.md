# ESP32-S3 E-Ink Photo Frame
[![Watch the video](https://img.youtube.com/vi/A8poN4a6Aiw/0.jpg)](https://www.youtube.com/watch?v=A8poN4a6Aiw)

Custom firmware for the [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter), a battery-powered e-ink photo frame featuring an ESP32-S3 and a 7.3" Spectra ACeP 6-colour display (800×480). This project replaces the stock firmware with an Arduino-based sketch that fetches random images from an nginx server over WiFi, resizes and Floyd–Steinberg dithers them to the 6-colour palette, and displays them on the panel. It also reports temperature, humidity, and battery voltage to Home Assistant via MQTT.

> **Note:** This firmware is **not** based on the official Waveshare ESP-IDF demo. It is written from scratch using the Arduino framework. The stock firmware and SD card contents are not required.

## Features

- Fetches random images (JPEG/BMP) from a configured HTTP server
- Bilinear resize with cover-crop to 800×480
- Floyd–Steinberg dithering to 6-colour ACeP palette (Black, White, Yellow, Red, Blue, Green)
- Deep sleep between updates (~5 min wake cycle, display refresh every 30 min)
- GPIO4 button press triggers immediate photo change
- SHTC3 temperature & humidity sensor reported to Home Assistant via MQTT
- Battery voltage reported to Home Assistant via MQTT
- MQTT auto-discovery for Home Assistant

## Hardware

This project targets the [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/esp32-s3-photopainter.htm) which includes:

- ESP32-S3 with 16MB flash and OPI PSRAM
- 7.3" Spectra ACeP e-ink display (800×480, 6-colour: Black/White/Yellow/Red/Blue/Green)
- AXP2101 PMU with LiPo battery management (I2C)
- SHTC3 temperature/humidity sensor (I2C, same bus as AXP2101)
- MicroSD card slot (SPI)
 - MicroSD card slot (SPI) — must stay populated because the firmware streams every downloaded photo onto the card as scratch storage before any processing happens
- BOOT button on GPIO4 (used to trigger immediate photo change)
- PWR button for power on/off

## Setup

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
- **JPEGDEC** — JPEG decoder by Larry Bank
- **PubSubClient** — MQTT client by Nick O'Leary

### 3. Configure Secrets

1. Copy `secrets_template.h` to `secrets.h`:
   ```
   cp secrets_template.h secrets.h
   ```
2. Open `secrets.h` and fill in your credentials:

   ```cpp
   #pragma once

   #define WIFI_SSID     "YourWiFiSSID"
   #define WIFI_PASSWORD "YourWiFiPassword"
   #define SERVER        "server:port/"

   #define MQTT_SERVER   "mqttserver_url_or_ip"
   #define MQTT_PORT     1883
   #define MQTT_USER     ""          // leave empty if no auth
   #define MQTT_PASSWORD ""          // leave empty if no auth
   ```

   - **WIFI_SSID / WIFI_PASSWORD** — Your WiFi network credentials.
   - **SERVER** — The HTTP address of your nginx image server.
   - **MQTT_SERVER** — The IP or hostname of your MQTT broker (e.g. Mosquitto on your Home Assistant host).
   - **MQTT_PORT** — MQTT broker port (default `1883`).
   - **MQTT_USER / MQTT_PASSWORD** — MQTT credentials. Leave empty strings if your broker doesn't require authentication.

> **Note:** `secrets.h` is gitignored to avoid committing credentials. Never commit this file.

### 4. Nginx Image Server

The photo frame fetches images over HTTP from an nginx server with `autoindex` enabled. The server simply points at a folder on disk and lists whatever images are inside it. The key advantage of this approach is that the photo directory can live on a Samba (SMB) network share — anyone on the network can drag-and-drop new photos into the shared folder and the frame will pick them up automatically on its next refresh cycle. There is no need to physically remove the SD card from the frame to update the photo collection.

An example config is included in `photoframe.conf`.

#### Fresh Nginx Install

1. Install nginx:
   ```bash
   sudo apt update
   sudo apt install nginx
   ```

2. Copy the included site configuration into nginx's `sites-available` directory:
   ```bash
   sudo cp photoframe.conf /etc/nginx/sites-available/photoframe
   ```

3. Edit the config to set your LAN IP, port, and photo directory:
   ```bash
   sudo nano /etc/nginx/sites-available/photoframe
   ```
   - Replace `lanip:port` with your server's LAN address and desired port (e.g. `192.168.1.50:8080`).
   - Replace `/your/photo/directory` with the absolute path to the folder containing your photos.

   > **Why bind to a LAN IP, not just a port?**
   > The `listen` directive in `photoframe.conf` uses `lanip:port` rather than just `port`. This binds nginx to your private LAN interface only (e.g. `192.168.1.50:8080`), so the server will not accept connections on the public-facing interface or on `localhost`. This effectively makes it a LAN-only service — devices outside your local network cannot reach it without additional routing.
   >
   > For extra protection you can also add a firewall rule to restrict the port to your local subnet:
   > ```bash
   > sudo ufw allow from 192.168.1.0/24 to any port 8080 proto tcp
   > ```
   > This ensures only devices on your local network can connect, even if the machine has a public IP.

4. Enable the site by creating a symlink in `sites-enabled`:
   ```bash
   sudo ln -s /etc/nginx/sites-available/photoframe /etc/nginx/sites-enabled/photoframe
   ```

5. Optionally remove the default site to avoid port conflicts:
   ```bash
   sudo rm /etc/nginx/sites-enabled/default
   ```

6. Test and reload nginx:
   ```bash
   sudo nginx -t
   sudo systemctl reload nginx
   ```

7. Verify the image listing is accessible by visiting `http://your-lan-ip:port/` in a browser — you should see an autoindex directory listing of your photos.

Set the `SERVER` value in `secrets.h` to match (e.g. `"192.168.1.50:8080/"`).

### 5. Home Assistant & MQTT

#### 5a. MQTT Broker (Mosquitto)

If you haven't already, install the Mosquitto MQTT broker. The easiest way on Home Assistant OS is via the official **Mosquitto broker** add-on:

1. In Home Assistant go to **Settings → Add-ons → Add-on Store**.
2. Search for **Mosquitto broker** and install it.
3. Start the add-on.

#### 5b. Create a Home Assistant User for the Device

The photo frame needs MQTT credentials to authenticate with the broker. Create a dedicated local user in Home Assistant for this purpose:

1. Go to **Settings → People → Users** (tab at the top).
2. Click **Add User**.
3. Fill in the details:
   - **Display Name:** e.g. `Photo Frame`
   - **Username:** e.g. `photoframe`
   - **Password:** choose a strong password
4. Toggle **"Can only log in from the local network"** on (recommended).
5. Set **Administrator** to off — the device only needs basic access.
6. Click **Create**.

Now put these credentials into `secrets.h`:

```cpp
#define MQTT_USER     "photoframe"
#define MQTT_PASSWORD "the-password-you-chose"
```

The Mosquitto add-on in Home Assistant automatically allows any HA local user to authenticate against the broker, so no extra Mosquitto configuration is needed.

#### 5c. MQTT Integration

1. Go to **Settings → Devices & Services → Integrations**.
2. If MQTT is not already configured, click **Add Integration**, search for **MQTT**, and follow the prompts to connect to your Mosquitto broker (usually `localhost:1883`).

#### 5d. Auto-Discovery

The device publishes MQTT auto-discovery messages on first connection. Once the ESP32 reports its first sensor reading, three entities will appear automatically in Home Assistant:

- **Photo Frame Temperature** — °C from the SHTC3 sensor
- **Photo Frame Humidity** — % RH from the SHTC3 sensor
- **Photo Frame Battery** — Battery voltage in V from the AXP2101 PMU

No manual MQTT entity configuration is needed — just ensure the MQTT integration is set up and the broker address in Home Assistant matches `MQTT_SERVER` in `secrets.h`.

## File Overview

| File                    | Description                                          |
|-------------------------|------------------------------------------------------|
| `esp32_photo-frame.ino` | Main sketch — setup, deep sleep, update orchestration |
| `spectra73.ino`         | Spectra 7.3" ACeP e-ink panel driver                 |
| `image_processing.ino`  | JPEG/BMP decode, bilinear resize, Floyd–Steinberg dither |
| `network.ino`           | WiFi, HTTP image list parsing, image download to SD  |
| `sdcard.ino`            | SD card initialisation                               |
| `sensor.ino`            | SHTC3 sensor + MQTT reporting with HA auto-discovery |
| `secrets.h`             | WiFi, server, and MQTT credentials (not committed)   |
| `secrets_template.h`    | Template for `secrets.h`                             |
| `photoframe.conf`       | Example nginx site configuration                     |

## Image Pipeline

1. Wake & connect — [esp32_photo-frame.ino](esp32_photo-frame.ino) brings the ESP32 out of deep sleep and calls the WiFi helpers in [network.ino](network.ino) to associate with your access point.
2. Build the candidate list — `fetchImageList()` in [network.ino](network.ino) pulls the nginx autoindex page, filters it down to JPEG/PNG/BMP files, and picks a random entry.
3. Cache to SD — the chosen image is streamed over HTTP and written to the mounted card via `downloadImageToSD()` in [network.ino](network.ino). This temporary SD storage is why the hardware requires the card to stay inserted.
4. Decode & resize — `processImage()` in [image_processing.ino](image_processing.ino) opens the SD file, selects the most aggressive downscale that still covers 800×480, and bilinear-resizes (cover + centered crop) into RGB888 buffers allocated in PSRAM.
5. Dither & refresh — the Floyd–Steinberg stage in [image_processing.ino](image_processing.ino) converts the RGB data into the 6-color ACeP palette before [spectra73.ino](spectra73.ino) pushes the final frame to the e-ink panel.
6. Report & sleep — [sensor.ino](sensor.ino) publishes temperature, humidity, and battery via MQTT, after which the main sketch schedules the next wake cycle and returns to deep sleep.

## Deep Sleep Behaviour

The ESP32 deep sleeps between tasks to conserve battery:

- **Every 5 minutes** — Wakes, reads the SHTC3 sensor and battery voltage, publishes to MQTT, then goes back to sleep.
- **Every 30 minutes** (6th wake) — Additionally fetches a new random photo from the server, processes it, and refreshes the display.
- **Button press** (GPIO4) — Immediately wakes and refreshes the display with a new random photo.

## Third-Party Libraries & Licenses

This project uses the following third-party libraries. Full license texts are available in each library's repository.

| Library | Author | License | Repository |
|---------|--------|---------|------------|
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | Larry Bank / BitBank Software, Inc. | Apache-2.0 | [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC) |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | Lewis He / LilyGO | MIT | [lewisxhe/XPowersLib](https://github.com/lewisxhe/XPowersLib) |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | Nick O'Leary | MIT | [knolleary/pubsubclient](https://github.com/knolleary/pubsubclient) |

The WiFi, HTTPClient, SD, SPI, and Wire libraries are part of the [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32) by Espressif Systems, licensed under the **GNU Lesser General Public License v2.1 (LGPL-2.1)**.
