# Verification

Run from the repository root:

```powershell
python tools/embed_web.py --check
npm.cmd ci
npm.cmd test
npm.cmd run test:browser
```

Browser tests run headless Microsoft Edge against the real `web/` assets and mocked HTTP responses, at desktop and mobile sizes. They cover crop geometry/conversion, large-gallery paging, unsafe filename rendering, partial batch failures, thumbnail-only retries, password handling, persistence errors, and pending display status. They do not simulate the physical ESP32 or SD driver.

The portable C++ tests exercise the same upload accounting, settings persistence, record-replacement, and queue helpers used by firmware, with short writes, failed renames, interrupted commits, and failed NVS writes:

```sh
c++ -std=c++17 tests/firmware_logic.cpp -o firmware_logic
./firmware_logic
c++ -std=c++17 -Itests/fakes tests/photo_store_test.cpp esp32_photo-frame/photo_store.cpp esp32_photo-frame/storage.cpp -o photo_store_test
./photo_store_test
```

On Windows, a local Zig compiler also works:

```powershell
zig c++ -std=c++17 tests/firmware_logic.cpp -o .build/firmware_logic.exe
& .build/firmware_logic.exe
zig c++ -std=c++17 -Itests/fakes tests/photo_store_test.cpp esp32_photo-frame/photo_store.cpp esp32_photo-frame/storage.cpp -o .build/photo_store_test.exe
& .build/photo_store_test.exe
```

Compile the hardware firmware with the versions/configuration documented in the main README. No test command flashes the frame.

The photo-store suite executes production storage code with a host filesystem adapter. It checks legacy migration, filename/extension identity, upload receipts before/after commit, metadata corruption and backups, full-card access, temporary-file cleanup, and display leases. The adapter substitutes hashing and filesystem operations; physical FAT durability and the ESP32 SPI driver still need device testing.

## Physical-device acceptance checklist

These checks require the PhotoPainter, a disposable/test SD card, and a browser. They are not replaced by the automated tests.

| Scenario | Expected result |
|---|---|
| Upgrade a card with legacy photos and stem-based caches | Original bytes/names remain intact; generated caches/thumbs rebuild. |
| Copy `holiday.jpg` and `holiday.bmp` to the card | Independent previews, dither buffers, and failed-image state. |
| Delete and upload different bytes under the same name | New browser image/revision; old URLs do not return the replacement. |
| Interrupt an upload or remove USB during it | No incomplete source in gallery/rotation; temporary data closes/cleans; retry is safe. |
| Lose the successful upload response and retry its token | Same committed filename, no duplicate source. |
| Full SD, card removal, or failed metadata write | Explicit failure; no false success; existing readable photos remain accessible. |
| Fail only thumbnail upload, then retry | Source is uploaded once; thumbnail is retried independently. |
| Cached/uncached Show while polling status and browsing | HTTP remains responsive during decode/panel waits; status normally responds within 1 second on healthy local WiFi, excluding another HTTP file transfer. |
| A refreshing, request B then C | A finishes, then C; B is never displayed. |
| Delete active/queued photo or clear cache during display/upload | HTTP 409; active input/framebuffer remains intact. |
| Panel BUSY stuck low | Bounded timeout and failed job; current-photo label is not advanced. |
| Empty and 1,000-photo cards | Bounded 24-card pages; generation changes restart stale cursors. |
| Blank password save / explicit clear / invalid port | Preserve / clear / HTTP 400 respectively; saved groups survive reboot. |
| Broker settings changed during a refresh | Applied after current control work; MQTT continues to be serviced during panel waits. |
| Router disconnect/reconnect, AP setup, hostname change | mDNS advertisement returns; IP fallback and captive-portal setup still work. |
| GPIO4, automatic rotation, USB unplug during a refresh | Serial display ownership; safe shutdown after refresh; battery wake/sleep cycle unchanged. |
| Battery-only boot and progressive JPEG fallback | No HTTP task; existing decoder behavior and low-battery overlay retained. |

Record panel refresh duration, status latency, free heap/PSRAM, HTTP task stack high-water mark, and any serial errors during a sustained upload/rotation session before deploying broadly.
