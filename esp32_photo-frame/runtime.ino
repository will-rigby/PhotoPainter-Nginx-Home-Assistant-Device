// SPDX-License-Identifier: 0BSD
#include "config.h"
#include "transactions.h"

static RuntimeSnapshot runtimeState;
static uint32_t nextJobId = 1;
static bool storageDirty = true;
static bool uploadInProgress = false;
static String uploadingPhoto;

bool photoBeingUploaded(const String& name) {
  MutexGuard g(stateMutex);
  return uploadInProgress && uploadingPhoto.length() && uploadingPhoto.equalsIgnoreCase(name);
}
bool beginUploadLease(const String& name, String& error) {
  MutexGuard catalog(catalogMutex);
  MutexGuard state(stateMutex);
  if (maintenanceBusy() || uploadInProgress || (name.length() && photoInUse(name))) {
    error = "Photo or storage is busy"; return false;
  }
  uploadInProgress = true; uploadingPhoto = name; return true;
}
void endUploadLease() {
  MutexGuard g(stateMutex); uploadInProgress = false; uploadingPhoto = "";
}

RuntimeSnapshot runtimeSnapshot() { MutexGuard g(stateMutex); return runtimeState; }
bool deviceStopping() { MutexGuard g(stateMutex); return runtimeState.stopping; }
void operationError(const String& message) {
  MutexGuard g(stateMutex); runtimeState.lastError = message; runtimeState.observedAt = millis();
}
void displayFailure(const String& message) {
  MutexGuard g(stateMutex); runtimeState.jobError = message; runtimeState.lastError = message;
}
void storageChanged() { MutexGuard g(stateMutex); storageDirty = true; }
void displayStage(const String& stage) {
  MutexGuard g(stateMutex); runtimeState.stage = stage; runtimeState.displayAt = millis();
}
bool maintenanceBusy() {
  MutexGuard g(stateMutex);
  return runtimeState.stopping ||
    (runtimeState.active.id && runtimeState.active.kind == DisplayKind::ClearCache) ||
    (runtimeState.pending.id && runtimeState.pending.kind == DisplayKind::ClearCache);
}
bool photoInUse(const String& name) {
  MutexGuard g(stateMutex);
  return photoBeingUploaded(name) || (runtimeState.active.id && runtimeState.active.name.equalsIgnoreCase(name)) ||
         (runtimeState.pending.id && runtimeState.pending.name.equalsIgnoreCase(name));
}
bool queueDisplay(DisplayKind kind, const String& name, bool manual, uint32_t& id, String& error) {
  // Lock ordering: catalog -> state. No SD operation while holding stateMutex.
  MutexGuard catalog(catalogMutex);
  if (kind == DisplayKind::Photo && (!validPhotoName(name) || !storage.exists(String(ORIGINALS_DIR "/") + name))) {
    error = "Photo not found"; return false;
  }
  MutexGuard state(stateMutex);
  if (maintenanceBusy()) { error = "Device is stopping or clearing its cache"; return false; }
  if (photoBeingUploaded(name) || (kind == DisplayKind::ClearCache && uploadInProgress)) {
    error = "Upload in progress"; return false;
  }
  bool busy = runtimeState.active.id || runtimeState.pending.id;
  if ((kind == DisplayKind::Test || kind == DisplayKind::ClearCache || !manual) && busy) {
    error = "Display busy"; return false;
  }
  DisplayJob job; job.id = nextJobId++; job.name = name; job.kind = kind; job.manual = manual;
  enqueueLatest(runtimeState.active, runtimeState.pending, job, manual);
  id = job.id;
  runtimeState.observedAt = millis();
  return true;
}

void runtimeBegin(bool sdReady) {
  runtimeState.sdReady = sdReady;
  runtimeState.observedAt = millis();
  runtimeState.nextRotation = millis() + ROTATE_INTERVAL_MS;
  if (!sdReady) runtimeState.lastError = "SD card unavailable";
}

// Runs only on the control task, including from long panel BUSY waits. The
// HTTP task never touches MQTT, Wire, PMU, JPEGDEC, or the panel framebuffer.
void controlMaintenance() {
  if (!serverMode) return;
  static uint32_t sampled = 0;
  static uint32_t powerSampled = 0;
  uint32_t now = millis();
  mqtt.loop();
  if (now - sampled >= 1000) {
    sampled = now;
    MutexGuard g(stateMutex);
    runtimeState.mqttConnected = mqtt.connected();
    runtimeState.mqttState = mqtt.state();
    runtimeState.mqttAt = now;
    runtimeState.observedAt = now;
  }
  if (now - powerSampled >= POWER_CHECK_MS) {
    powerSampled = now;
    if (!pmuVbusPresent()) {
      MutexGuard g(stateMutex);
      runtimeState.stopping = true;
      runtimeState.pending = DisplayJob{};
    }
  }
}

void controlTick() {
  controlMaintenance();
  if (deviceStopping()) { webServerStop(); enterDeepSleep(); return; }

  uint32_t revision = configMqttRevision();
  static uint32_t appliedMqttRevision = 0;
  if (revision != appliedMqttRevision) {
    mqtt.disconnect();
    appliedMqttRevision = revision;
    sensorReport();
    lastSensor = millis();
  }

  DisplayJob job;
  {
    MutexGuard g(stateMutex);
    if (startPending(runtimeState.active, runtimeState.pending, job)) {
      runtimeState.jobError = "";
      runtimeState.stage = job.kind == DisplayKind::ClearCache ? "clearing cache" : "preparing";
      runtimeState.displayAt = millis();
    }
  }
  if (job.id) {
    bool ok = false;
    if (job.kind == DisplayKind::Photo) ok = displayNamedPhoto(job.name);
    else if (job.kind == DisplayKind::Random) ok = displayRandomFromSD();
    else if (job.kind == DisplayKind::Test) {
      epdDrawHorizontalBars(); displayStage("refreshing"); ok = epdDisplayCurrentBuffer();
      if (ok) { MutexGuard g(stateMutex); runtimeState.displayed = "Test pattern"; }
    } else {
      ok = clearDitherCache(); clearAllFailed(); storageChanged();
    }
    uint32_t finished = millis();
    if (job.kind != DisplayKind::ClearCache) lastRotate = finished;
    {
      MutexGuard g(stateMutex);
      runtimeState.lastJob = job.id;
      runtimeState.stage = ok ? "complete" : "failed";
      runtimeState.displayAt = finished;
      if (!ok) runtimeState.lastError = runtimeState.jobError.length() ? runtimeState.jobError :
        (job.kind == DisplayKind::ClearCache ? "Could not clear all cached files" : "Photo preparation or panel refresh failed");
      runtimeState.active = DisplayJob{};
      runtimeState.nextRotation = lastRotate + ROTATE_INTERVAL_MS;
    }
  }

  bool updateStorage;
  { MutexGuard g(stateMutex); updateStorage = storageDirty; storageDirty = false; }
  if (updateStorage && runtimeSnapshot().sdReady) {
    uint64_t total = storage.totalBytes(), used = storage.usedBytes();
    MutexGuard g(stateMutex);
    runtimeState.storageTotal = total;
    runtimeState.storageFree = total >= used ? total - used : 0;
    runtimeState.storageAt = millis();
  }
}

bool displayNamedPhoto(const String& name) {
  {
    MutexGuard catalog(catalogMutex);
    if (photoBeingUploaded(name)) return false;
    if (!storage.exists(String(ORIGINALS_DIR "/") + name)) return false;
    MutexGuard state(stateMutex);
    // Random selection acquires the same lease as an explicit Show request.
    runtimeState.active.name = name;
  }
  displayStage("preparing");
  if (!ensureDitheredInBuffer(name)) return false;
  displayStage("refreshing");
  if (!epdDisplayCurrentBuffer()) return false;
  { MutexGuard g(stateMutex); runtimeState.displayed = name; }
  return true;
}
