// SPDX-License-Identifier: 0BSD
#include <WebServer.h>
#include <DNSServer.h>
#include "config.h"
#include "policies.h"
#include "web_assets.h"

static WebServer server(80);
static DNSServer dnsServer;
static TaskHandle_t httpTask = nullptr;
static SemaphoreHandle_t httpStopped = nullptr;

static void sendJson(int status, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json", body);
}
static void sendError(int status, const String& message) {
  if (status >= 500) operationError(message);
  sendJson(status, "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}");
}
static bool storageAvailable() {
  RuntimeSnapshot state = runtimeSnapshot();
  if (!state.sdReady || state.stopping) { sendError(503, "Storage unavailable or device stopping"); return false; }
  return true;
}
static String imageType(const String& name) {
  String lower = name; lower.toLowerCase(); return lower.endsWith(".bmp") ? "image/bmp" : "image/jpeg";
}
static bool unsignedNumber(const String& text, uint32_t& result) {
  if (!text.length() || text.length() > 10) return false;
  uint64_t n = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    n = n * 10 + text[i] - '0'; if (n > UINT32_MAX) return false;
  }
  result = n; return true;
}
static void handleApiPhotos() {
  if (!storageAvailable()) return;
  uint32_t limit = 24, offset = 0, generation = catalogGeneration();
  if (server.hasArg("limit") && (!unsignedNumber(server.arg("limit"), limit) || !limit || limit > 48)) {
    sendError(400, "Page size must be 1-48"); return;
  }
  String cursor = server.arg("cursor");
  if (cursor.length()) {
    int split = cursor.indexOf(':'); uint32_t requested;
    if (split < 0 || !unsignedNumber(cursor.substring(0, split), requested) ||
        !unsignedNumber(cursor.substring(split + 1), offset)) { sendError(400, "Invalid cursor"); return; }
    if (requested != generation) { sendJson(409, "{\"ok\":false,\"restart\":true,\"error\":\"Gallery changed\"}"); return; }
  }
  StorageFile dir = storage.open(ORIGINALS_DIR);
  if (!dir) { sendError(503, "Cannot read photo directory"); return; }
  uint32_t index = 0, count = 0; bool more = false;
  String body = "{\"photos\":[";
  StorageFile f;
  while ((f = dir.openNextFile())) {
    String name = photoBaseName(f.name()); bool isPhoto = !f.isDirectory() && validPhotoName(name); f.close();
    if (!isPhoto || index++ < offset) continue;
    if (count == limit) { more = true; break; }
    PhotoMeta m;
    if (!photoMeta(name, m)) { sendError(503, "Cannot read or save photo metadata"); return; }
    if (count++) body += ',';
    bool cached = storage.exists(photoCachePath(m));
    body += "{\"name\":\"" + jsonEscape(name) + "\",\"revision\":\"" + m.revision +
      "\",\"thumbnail_revision\":\"" + m.thumbnail + "\",\"cached\":" + (cached ? "true" : "false") +
      ",\"failed\":" + (!cached && isFailedBase(name) ? "true" : "false") + "}";
    vTaskDelay(1);
  }
  dir.close();
  if (generation != catalogGeneration()) { sendJson(409, "{\"ok\":false,\"restart\":true}"); return; }
  body += "],\"generation\":" + String(generation) + ",\"next_cursor\":";
  body += more ? "\"" + String(generation) + ":" + String(offset + count) + "\"" : "null";
  sendJson(200, body + "}");
}
static void handleApiConfig() {
  ConfigSnapshot c = configSnapshot();
  sendJson(200, "{\"wifi_ssid\":\"" + jsonEscape(c.ssid) + "\",\"hostname\":\"" + jsonEscape(c.hostname) +
    "\",\"mqtt_host\":\"" + jsonEscape(c.host) + "\",\"mqtt_port\":" + String(c.port) +
    ",\"mqtt_user\":\"" + jsonEscape(c.user) + "\",\"wifi_password_saved\":" + (c.wifiPass.length() ? "true" : "false") +
    ",\"mqtt_password_saved\":" + (c.mqttPass.length() ? "true" : "false") + ",\"ap_mode\":" + (apMode ? "true" : "false") + "}");
}
static String jobJson(const DisplayJob& j) {
  if (!j.id) return "null";
  return "{\"id\":" + String(j.id) + ",\"name\":\"" + jsonEscape(j.name) + "\"}";
}
static void handleApiStatus() {
  RuntimeSnapshot s = runtimeSnapshot();
  uint32_t remaining = (int32_t)(s.nextRotation - millis()) > 0 ? (s.nextRotation - millis()) / 1000 : 0;
  String body = "{\"now\":" + String(millis()) + ",\"observed_at\":" + String(s.observedAt) +
    ",\"stage\":\"" + jsonEscape(s.stage) + "\",\"active\":" + jobJson(s.active) + ",\"pending\":" + jobJson(s.pending) +
    ",\"displayed\":\"" + jsonEscape(s.displayed) + "\",\"last_job\":" + String(s.lastJob) +
    ",\"display_at\":" + String(s.displayAt) + ",\"next_rotation_seconds\":" + String(remaining) +
    ",\"storage_total\":" + String((unsigned long long)s.storageTotal) +
    ",\"storage_free\":" + String((unsigned long long)s.storageFree) + ",\"storage_at\":" + String(s.storageAt) +
    ",\"sd_ready\":" + (s.sdReady ? "true" : "false") + ",\"mqtt_connected\":" + (s.mqttConnected ? "true" : "false") +
    ",\"mqtt_state\":" + String(s.mqttState) + ",\"mqtt_at\":" + String(s.mqttAt) +
    ",\"ap_mode\":" + (apMode ? "true" : "false") + ",\"ip\":\"" + (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString() +
    "\",\"rssi\":" + String(WiFi.RSSI()) + ",\"hostname\":\"" + String(WiFi.getHostname() ? WiFi.getHostname() : "photoframe") +
    "\",\"stopping\":" + (s.stopping ? "true" : "false") + ",\"last_error\":\"" + jsonEscape(s.lastError) + "\"}";
  sendJson(200, body);
}
static void serveImage(bool thumbnail) {
  if (!storageAvailable()) return;
  String name = server.arg("file");
  if (!validPhotoName(name)) { sendError(400, "Invalid filename"); return; }
  PhotoMeta meta;
  if (!photoMeta(name, meta)) { sendError(404, "Photo not found"); return; }
  String revision = thumbnail ? meta.thumbnail : meta.revision;
  String version = server.arg("v");
  if (version.length() && version != revision) { sendError(404, "Image revision expired"); return; }
  String path = thumbnail ? photoThumbPath(meta, true) : String(ORIGINALS_DIR "/") + name;
  String type = thumbnail ? "image/jpeg" : imageType(name);
  StorageFile f = storage.open(path);
  if (thumbnail && !f) { f = storage.open(photoThumbPath(meta, false)); type = "image/bmp"; }
  if (!f) { sendError(404, "Image not available"); return; }
  String etag = "\"" + revision + "\"";
  server.sendHeader("ETag", etag);
  server.sendHeader("Cache-Control", version.length() ? "private, max-age=604800, immutable" : "no-cache");
  if (server.header("If-None-Match") == etag) { server.send(304); return; }
  server.setContentLength(f.size()); server.send(200, type, "");
  WiFiClient client = server.client();
  static uint8_t buffer[16384];
  size_t n;
  while (!deviceStopping() && (n = f.read(buffer, sizeof(buffer)))) {
    size_t offset = 0;
    while (offset < n) {
      if (deviceStopping()) { client.stop(); return; }
      size_t wrote = client.write(buffer + offset, n - offset);
      if (!wrote) { f.close(); return; }
      offset += wrote;
    }
    vTaskDelay(1);
  }
  f.close();
}

struct UploadSession {
  StorageFile file;
  UploadAccounting accounting;
  PhotoMeta meta;
  String temporary, name, token, error;
  uint8_t header[54] = {};
  size_t headerBytes = 0;
  int status = 400, parts = 0;
  bool ended = false, duplicate = false, thumbnail = false, leased = false, aborted = false;
};
static UploadSession incoming;

static void resetUpload() {
  incoming.file.close();
  if (incoming.temporary.length()) storage.remove(incoming.temporary);
  if (incoming.leased) endUploadLease();
  incoming = UploadSession{};
}
static void uploadError(int status, const String& error) {
  if (!incoming.error.length()) { incoming.status = status; incoming.error = error; }
  incoming.accounting.failed = true;
  incoming.file.close();
  if (incoming.temporary.length()) storage.remove(incoming.temporary);
}
static String uploadName(const String& original) {
  String name = photoBaseName(original), lower = name; lower.toLowerCase();
  String extension = lower.endsWith(".jpeg") ? ".jpeg" : lower.endsWith(".jpg") ? ".jpg" : lower.endsWith(".bmp") ? ".bmp" : "";
  if (!extension.length()) return "";
  String base;
  for (int i = 0; i < name.lastIndexOf('.') && base.length() < 40; ++i) {
    char c = name[i]; base += isalnum((uint8_t)c) && (uint8_t)c < 128 ? c : '_';
  }
  if (!base.length()) base = "photo";
  String candidate = base + extension;
  for (uint32_t i = 1; storage.exists(String(ORIGINALS_DIR "/") + candidate); ++i) candidate = base + "_" + String(i) + extension;
  return candidate;
}
static void receiveUpload(bool thumbnail) {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (incoming.aborted) resetUpload();
    if (++incoming.parts != 1) { uploadError(400, "Send one file per request"); return; }
    incoming.thumbnail = thumbnail;
    if (!runtimeSnapshot().sdReady || deviceStopping()) { uploadError(503, "Storage unavailable"); return; }
    String leaseError;
    String name = thumbnail ? server.arg("name") : "";
    if (!beginUploadLease(name, leaseError)) { uploadError(409, leaseError); return; }
    incoming.leased = true;
    if (thumbnail) {
      if (!validPhotoName(name) || !photoMeta(name, incoming.meta)) { uploadError(404, "Photo not found"); return; }
      incoming.name = name;
    } else {
      incoming.token = server.arg("token");
      if (!incoming.token.length()) incoming.token = newRevision(); // legacy clients
      if (!validToken(incoming.token)) { uploadError(400, "Invalid upload token"); return; }
      incoming.duplicate = findUploadToken(incoming.token, incoming.meta);
      incoming.name = incoming.duplicate ? String(incoming.meta.name) : uploadName(up.filename);
      if (!incoming.name.length()) { uploadError(415, "Supported types: JPEG and BMP"); return; }
    }
    if (!incoming.duplicate) {
      incoming.temporary = "/upload-temp/" + newRevision() + ".part";
      incoming.file = storage.open(incoming.temporary, FILE_WRITE);
      if (!incoming.file) uploadError(507, "Cannot create upload file on SD card");
    }
  } else if (up.status == UPLOAD_FILE_WRITE && !incoming.error.length()) {
    if (deviceStopping()) { uploadError(503, "USB power removed"); server.client().stop(); return; }
    size_t limit = thumbnail ? 256u * 1024 : 20u * 1024 * 1024;
    if (up.currentSize > limit || incoming.accounting.received > limit - up.currentSize) { uploadError(413, "Image exceeds upload limit"); return; }
    size_t copied = min(up.currentSize, sizeof(incoming.header) - incoming.headerBytes);
    memcpy(incoming.header + incoming.headerBytes, up.buf, copied); incoming.headerBytes += copied;
    size_t wrote = incoming.duplicate ? up.currentSize : incoming.file.write(up.buf, up.currentSize);
    if (!incoming.accounting.add(up.currentSize, wrote, limit)) uploadError(507, "SD card write failed or card is full");
  } else if (up.status == UPLOAD_FILE_END && !incoming.error.length()) {
    String lower = incoming.name; lower.toLowerCase();
    if (!incoming.accounting.complete(up.totalSize) ||
        !imageSignature(incoming.header, incoming.headerBytes, !thumbnail && lower.endsWith(".bmp"))) {
      uploadError(415, "Incomplete file or image signature does not match its type"); return;
    }
    if (incoming.duplicate) {
      if (incoming.accounting.received != incoming.meta.size) { uploadError(409, "Upload token was already used for a different file"); return; }
    } else {
      incoming.file.flush();
      if (incoming.file.size() != incoming.accounting.received) { uploadError(507, "SD file size verification failed"); return; }
      incoming.file.close();
    }
    incoming.ended = true;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    uploadError(400, "Upload interrupted"); resetUpload(); incoming.aborted = true;
  }
}
static void finishUpload() {
  if (!incoming.ended || incoming.error.length()) {
    sendError(incoming.status, incoming.error.length() ? incoming.error : "No complete image received"); resetUpload(); return;
  }
  bool committed = incoming.duplicate;
  {
    MutexGuard catalog(catalogMutex);
    if (!committed && incoming.thumbnail) {
      PhotoMeta previous = incoming.meta;
      strlcpy(incoming.meta.thumbnail, newRevision().c_str(), sizeof(incoming.meta.thumbnail));
      String finalPath = photoThumbPath(incoming.meta, true);
      committed = storage.rename(incoming.temporary, finalPath) && savePhotoMeta(incoming.meta);
      if (committed) {
        storage.remove(photoThumbPath(previous, true)); storage.remove(photoThumbPath(previous, false));
      } else storage.remove(finalPath);
    } else if (!committed) {
      incoming.meta = PhotoMeta{};
      StorageFile f = storage.open(incoming.temporary);
      incoming.meta.size = f.size(); incoming.meta.modified = f.getLastWrite(); f.close();
      strlcpy(incoming.meta.name, incoming.name.c_str(), sizeof(incoming.meta.name));
      strlcpy(incoming.meta.revision, newRevision().c_str(), sizeof(incoming.meta.revision));
      strlcpy(incoming.meta.thumbnail, newRevision().c_str(), sizeof(incoming.meta.thumbnail));
      strlcpy(incoming.meta.token, incoming.token.c_str(), sizeof(incoming.meta.token));
      // Receipt and metadata precede the final source rename. A retry is only
      // considered committed when the source exists with the recorded stats.
      committed = saveUploadReceipt(incoming.meta) && savePhotoMeta(incoming.meta) &&
                  storage.rename(incoming.temporary, String(ORIGINALS_DIR "/") + incoming.name);
      if (committed) clearFailedBase(incoming.name);
    }
    if (committed) catalogChanged();
  }
  if (committed) sendJson(200, "{\"ok\":true,\"name\":\"" + jsonEscape(incoming.name) + "\",\"revision\":\"" + incoming.meta.revision + "\"}");
  else sendError(507, "Could not commit upload to SD card");
  resetUpload();
}
static void handleDelete() {
  if (!storageAvailable()) return;
  String name = server.arg("file"), error;
  if (!validPhotoName(name)) { sendError(400, "Invalid filename"); return; }
  if (deletePhoto(name, error)) { clearFailedBase(name); sendJson(200, "{\"ok\":true}"); }
  else sendError(photoInUse(name) || maintenanceBusy() ? 409 : 500, error);
}
static void handleDisplayRequest(DisplayKind kind) {
  if (kind != DisplayKind::Test && !storageAvailable()) return;
  String error; uint32_t id;
  if (queueDisplay(kind, server.arg("file"), true, id, error))
    sendJson(202, "{\"ok\":true,\"job_id\":" + String(id) + "}");
  else sendError(error == "Photo not found" ? 404 : 409, error);
}
static void handleSaveWifi() {
  if (deviceStopping()) { sendError(503, "Device stopping"); return; }
  String error, hostname = server.hasArg("hostname") ? server.arg("hostname") : configSnapshot().hostname;
  if (configSaveWifi(server.arg("ssid"), server.arg("pass"), server.arg("clear_password") == "true", hostname, error)) sendJson(200, "{\"ok\":true}");
  else sendError(error.startsWith("Invalid") ? 400 : 500, error);
}
static void handleSaveMqtt() {
  if (deviceStopping()) { sendError(503, "Device stopping"); return; }
  String error;
  if (configSaveMqtt(server.arg("host"), server.arg("port"), server.arg("user"), server.arg("pass"), server.arg("clear_password") == "true", error)) sendJson(200, "{\"ok\":true}");
  else sendError(error.startsWith("Invalid") ? 400 : 500, error);
}
static void httpService(void*) {
  const char* headers[] = {"If-None-Match"}; server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-cache"); server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX); });
  server.on("/wifi", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-cache"); server.send_P(200, "text/html; charset=utf-8", PAGE_WIFI); });
  server.on("/mqtt", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-cache"); server.send_P(200, "text/html; charset=utf-8", PAGE_MQTT); });
  server.on("/app.js", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-cache"); server.send_P(200, "text/javascript", ASSET_APP_JS); });
  server.on("/app.css", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-cache"); server.send_P(200, "text/css", ASSET_APP_CSS); });
  server.on("/api/photos", HTTP_GET, handleApiPhotos);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/original", HTTP_GET, [] { serveImage(false); });
  server.on("/thumb", HTTP_GET, [] { serveImage(true); });
  server.on("/upload", HTTP_POST, finishUpload, [] { receiveUpload(false); });
  server.on("/thumb", HTTP_POST, finishUpload, [] { receiveUpload(true); });
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/show", HTTP_POST, [] { handleDisplayRequest(DisplayKind::Photo); });
  server.on("/test", HTTP_POST, [] { handleDisplayRequest(DisplayKind::Test); });
  server.on("/redither", HTTP_POST, [] { handleDisplayRequest(DisplayKind::ClearCache); });
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/mqtt", HTTP_POST, handleSaveMqtt);
  server.onNotFound([] {
    if (apMode && server.method() == HTTP_GET && !server.uri().startsWith("/api/")) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/"); server.send(302, "text/plain", "");
    } else sendError(404, "Not found");
  });
  if (apMode) dnsServer.start(53, "*", WiFi.softAPIP());
  server.begin();
  while (!deviceStopping()) {
    server.handleClient();
    // Multipart parsing is synchronous in WebServer. A parser failure after
    // FILE_END can bypass both the completion and abort callbacks; never let
    // that leave a temporary file or photo lease attached to the next request.
    if (incoming.parts) {
      operationError("Upload interrupted before the request completed");
      resetUpload();
    }
    if (apMode) dnsServer.processNextRequest();
    networkDiscoveryTick();
    vTaskDelay(1);
  }
  resetUpload(); server.stop(); dnsServer.stop(); networkDiscoveryStop();
  xSemaphoreGive(httpStopped);
  vTaskDelete(nullptr);
}
void webServerBegin() {
  httpStopped = xSemaphoreCreateBinary();
  if (!httpStopped || xTaskCreate(httpService, "photo-http", 12288, nullptr, 1, &httpTask) != pdPASS) {
    httpTask = nullptr; operationError("Could not start HTTP task");
  }
}
void webServerStop() {
  { MutexGuard g(stateMutex); runtimeState.stopping = true; runtimeState.pending = DisplayJob{}; }
  if (httpTask) { xSemaphoreTake(httpStopped, portMAX_DELAY); httpTask = nullptr; }
}
