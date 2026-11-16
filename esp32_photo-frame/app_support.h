// SPDX-License-Identifier: 0BSD
#pragma once
#include "storage.h"

struct ConfigSnapshot {
  String ssid, wifiPass, hostname, host, user, mqttPass;
  uint16_t port = 1883;
  uint32_t mqttRevision = 0;
};
ConfigSnapshot configSnapshot();
uint32_t configMqttRevision();
bool configSaveWifi(const String&, const String&, bool, const String&, String&);
bool configSaveMqtt(const String&, const String&, const String&, const String&, bool, String&);

// Fixed-size records keep metadata work bounded. Sidecars are committed with
// rename, and a checksum distinguishes torn/corrupt records from valid ones.
struct PhotoMeta {
  uint32_t version = 2;
  uint32_t size = 0;
  int64_t modified = 0;
  char revision[33] = {};
  char thumbnail[33] = {};
  char token[33] = {};
  char name[256] = {};
  uint32_t checksum = 0;
};
String jsonEscape(const String&);
String photoBaseName(const String&);
bool validPhotoName(const String&);
String photoKey(const String&);
String newRevision();
bool validToken(const String&);
bool photoMeta(const String&, PhotoMeta&);
bool savePhotoMeta(const PhotoMeta&);
String photoCachePath(const PhotoMeta&);
String photoThumbPath(const PhotoMeta&, bool jpeg);
bool photoStoreBegin();
bool removeDerived(const PhotoMeta&);
bool deletePhoto(const String&, String&);
bool clearDitherCache();
uint32_t catalogGeneration();
void catalogChanged();
void storageChanged();
bool findUploadToken(const String&, PhotoMeta&);
bool saveUploadReceipt(const PhotoMeta&);
void cleanupTemporaryFiles();

enum class DisplayKind : uint8_t { Photo, Random, Test, ClearCache };
struct DisplayJob {
  uint32_t id = 0;
  DisplayKind kind = DisplayKind::Photo;
  String name;
  bool manual = true;
};
struct RuntimeSnapshot {
  DisplayJob active, pending;
  String stage = "idle", displayed, lastError, jobError;
  uint32_t observedAt = 0, displayAt = 0, lastJob = 0, nextRotation = 0;
  uint64_t storageTotal = 0, storageFree = 0;
  uint32_t storageAt = 0, mqttAt = 0;
  bool sdReady = false, mqttConnected = false, stopping = false;
  int mqttState = -1;
};
RuntimeSnapshot runtimeSnapshot();
bool deviceStopping();
bool queueDisplay(DisplayKind, const String&, bool, uint32_t&, String&);
bool photoInUse(const String&);
bool maintenanceBusy();
bool beginUploadLease(const String&, String&);
void endUploadLease();
bool photoBeingUploaded(const String&);
void displayStage(const String&);
void operationError(const String&);
void displayFailure(const String&);
void controlTick();
void controlMaintenance();
void runtimeBegin(bool);
void webServerStop();
void networkDiscoveryTick();
void networkDiscoveryStop();
bool displayNamedPhoto(const String&);
