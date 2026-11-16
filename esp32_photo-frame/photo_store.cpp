// SPDX-License-Identifier: 0BSD
#include "config.h"
#include <mbedtls/sha256.h>
#include <stddef.h>
#include "transactions.h"

static uint32_t generation = 1;
static const char* META_DIR = "/photo-meta";
static const char* RECEIPT_DIR = "/upload-receipts";

String jsonEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    unsigned char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
    else out += char(c);
  }
  return out;
}
String photoBaseName(const String& path) { return path.substring(path.lastIndexOf('/') + 1); }
bool validPhotoName(const String& name) {
  if (!name.length() || name.length() > 255 || name.indexOf('/') >= 0 ||
      name.indexOf('\\') >= 0 || name.indexOf("..") >= 0 || name[0] == '.') return false;
  for (size_t i = 0; i < name.length(); ++i) if ((uint8_t)name[i] < 32) return false;
  String lower = name; lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".bmp");
}
String photoKey(const String& name) {
  String canonical = name; canonical.toLowerCase(); // FAT is case-insensitive.
  uint8_t hash[32]; char hex[65];
  mbedtls_sha256((const uint8_t*)canonical.c_str(), canonical.length(), hash, 0);
  for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
  return String(hex);
}
String newRevision() {
  char text[33];
  for (int i = 0; i < 4; ++i) snprintf(text + 8 * i, 9, "%08lx", (unsigned long)esp_random());
  return String(text);
}
bool validToken(const String& token) {
  if (token.length() != 32) return false;
  for (size_t i = 0; i < token.length(); ++i)
    if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return false;
  return true;
}
static uint32_t recordChecksum(const PhotoMeta& m) {
  const uint8_t* bytes = (const uint8_t*)&m;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < offsetof(PhotoMeta, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}
static bool readRecordFile(const String& path, PhotoMeta& m) {
  StorageFile f = storage.open(path);
  if (!f || f.size() != sizeof(m)) return false;
  if (f.read((uint8_t*)&m, sizeof(m)) != sizeof(m)) return false;
  return m.version == 2 && m.name[255] == 0 && m.revision[32] == 0 && m.thumbnail[32] == 0 &&
         m.token[32] == 0 && validPhotoName(m.name) && validToken(m.revision) &&
         validToken(m.thumbnail) && m.checksum == recordChecksum(m);
}
static bool readRecord(const String& path, PhotoMeta& m) {
  return readRecordFile(path, m) || readRecordFile(path + ".bak", m);
}
static bool writeRecord(const String& path, const PhotoMeta& value) {
  PhotoMeta m = value; m.checksum = recordChecksum(m);
  String temp = path + ".tmp", backup = path + ".bak";
  StorageFile f = storage.open(temp, FILE_WRITE);
  if (!f) return false;
  bool ok = f.write((const uint8_t*)&m, sizeof(m)) == sizeof(m);
  f.flush(); ok = ok && f.size() == sizeof(m); f.close();
  if (!ok) { storage.remove(temp); return false; }
  PhotoMeta previous, recovered;
  // Never replace the only good backup with a corrupt primary record.
  if (storage.exists(path) && !readRecordFile(path, previous) && readRecordFile(backup, recovered)) {
    if (!storage.remove(path)) return false;
  }
  return replaceRecord(storage, temp, path, backup);
}
static String metaPath(const String& name) { return String(META_DIR) + "/" + photoKey(name); }
bool savePhotoMeta(const PhotoMeta& m) {
  MutexGuard guard(catalogMutex);
  return writeRecord(metaPath(m.name), m);
}
String photoCachePath(const PhotoMeta& m) {
  return String(DITHERED_DIR "/") + photoKey(m.name) + "-" + m.revision + "-r2.bin";
}
String photoThumbPath(const PhotoMeta& m, bool jpeg) {
  return String(THUMBS_DIR "/") + photoKey(m.name) + "-" + m.thumbnail + (jpeg ? ".jpg" : ".bmp");
}
bool removeDerived(const PhotoMeta& m) {
  bool ok = true;
  for (const String& p : {photoCachePath(m), photoThumbPath(m, true), photoThumbPath(m, false)})
    if (storage.exists(p) && !storage.remove(p)) ok = false;
  return ok;
}
bool photoMeta(const String& name, PhotoMeta& m) {
  MutexGuard guard(catalogMutex);
  if (!validPhotoName(name)) return false;
  StorageFile source = storage.open(String(ORIGINALS_DIR "/") + name);
  if (!source || source.isDirectory()) return false;
  uint32_t size = source.size(); int64_t modified = source.getLastWrite(); source.close();
  if (readRecord(metaPath(name), m) && String(m.name).equalsIgnoreCase(name)) {
    if (m.size == size && m.modified == modified) return true;
    removeDerived(m);
  }
  m = PhotoMeta{};
  m.size = size; m.modified = modified;
  strlcpy(m.name, name.c_str(), sizeof(m.name));
  // Imports use a reproducible fingerprint so existing photos remain readable
  // even when a full/read-only card cannot persist their first sidecar. Web
  // uploads always receive a fresh random revision in finishUpload().
  String fingerprint = photoKey(name + ":" + String(size) + ":" + String((long long)modified)).substring(0, 32);
  strlcpy(m.revision, fingerprint.c_str(), sizeof(m.revision));
  strlcpy(m.thumbnail, fingerprint.c_str(), sizeof(m.thumbnail));
  if (!savePhotoMeta(m)) operationError("Photo metadata could not be saved; SD card may be full or read-only");
  return true;
}
bool saveUploadReceipt(const PhotoMeta& m) {
  MutexGuard guard(catalogMutex);
  return validToken(m.token) && writeRecord(String(RECEIPT_DIR) + "/" + m.token, m);
}
bool findUploadToken(const String& token, PhotoMeta& m) {
  MutexGuard guard(catalogMutex);
  if (!validToken(token) || !readRecord(String(RECEIPT_DIR) + "/" + token, m) || String(m.token) != token) return false;
  StorageFile f = storage.open(String(ORIGINALS_DIR "/") + m.name);
  if (!f || f.size() != m.size || f.getLastWrite() != m.modified) return false;
  PhotoMeta current;
  if (readRecord(metaPath(m.name), current) && String(current.revision) != m.revision) return false;
  return true;
}
uint32_t catalogGeneration() { MutexGuard g(catalogMutex); return generation; }
void catalogChanged() { MutexGuard g(catalogMutex); ++generation; storageChanged(); }

static bool clearDirectoryFiles(const String& path, bool temporaryOnly) {
  // Closing and reopening after each deletion avoids invalid FAT iterators.
  while (true) {
    StorageFile dir = storage.open(path);
    if (!dir) return false;
    String victim;
    StorageFile f;
    while ((f = dir.openNextFile())) {
      String name = photoBaseName(f.name());
      if (!f.isDirectory() && (!temporaryOnly || name.endsWith(".part") || name.endsWith(".tmp"))) {
        victim = path + "/" + name; break;
      }
    }
    f.close(); dir.close();
    if (!victim.length()) return true;
    if (!storage.remove(victim)) return false;
    vTaskDelay(1);
  }
}
void cleanupTemporaryFiles() {
  clearDirectoryFiles("/upload-temp", false);
  clearDirectoryFiles(META_DIR, true);
  clearDirectoryFiles(RECEIPT_DIR, true);
  clearDirectoryFiles(DITHERED_DIR, true);
  clearDirectoryFiles(THUMBS_DIR, true);
}
bool photoStoreBegin() {
  generation = esp_random() | 1; // cursors from a previous boot cannot be reused
  for (const char* p : {ORIGINALS_DIR, DITHERED_DIR, THUMBS_DIR, META_DIR, RECEIPT_DIR, "/upload-temp"})
    if (!storage.mkdir(p)) return false;
  cleanupTemporaryFiles();
  if (!storage.exists("/photo-meta/layout-v2")) {
    if (!clearDirectoryFiles(DITHERED_DIR, false) || !clearDirectoryFiles(THUMBS_DIR, false)) return false;
    StorageFile marker = storage.open("/photo-meta/layout-v2", FILE_WRITE);
    const uint8_t version = 2;
    if (!marker || marker.write(&version, 1) != 1) return false;
    marker.close();
  }
  return true;
}
bool deletePhoto(const String& name, String& error) {
  MutexGuard guard(catalogMutex);
  if (photoInUse(name) || maintenanceBusy()) { error = "Photo is queued, displaying, or maintenance is running"; return false; }
  PhotoMeta m;
  if (!photoMeta(name, m)) { error = "Photo or metadata unavailable"; return false; }
  if (!storage.remove(String(ORIGINALS_DIR "/") + name)) { error = "Could not delete photo"; return false; }
  bool ok = removeDerived(m);
  for (const String& p : {metaPath(name), String(metaPath(name) + ".bak")})
    if (storage.exists(p) && !storage.remove(p)) ok = false;
  if (validToken(m.token)) {
    String p = String(RECEIPT_DIR) + "/" + m.token;
    if (storage.exists(p) && !storage.remove(p)) ok = false;
  }
  catalogChanged();
  if (!ok) error = "Photo deleted; some generated files could not be removed";
  return ok;
}
bool clearDitherCache() { return clearDirectoryFiles(DITHERED_DIR, false); }
