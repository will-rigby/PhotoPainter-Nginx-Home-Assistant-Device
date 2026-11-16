// SPDX-License-Identifier: 0BSD
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t storageMutex;
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t catalogMutex;
extern SemaphoreHandle_t configMutex;
void synchronizationBegin();

class MutexGuard {
  SemaphoreHandle_t mutex;
public:
  explicit MutexGuard(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGiveRecursive(mutex); }
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;
};

// Each filesystem operation is serialized. Locks never span a socket write,
// a decode/dither loop, or a panel wait. File copies retain FS reference semantics.
class StorageFile {
  File file;
public:
  StorageFile() = default;
  explicit StorageFile(File f) : file(f) {}
  StorageFile(const StorageFile& other) { MutexGuard g(storageMutex); file = other.file; }
  StorageFile& operator=(const StorageFile& other) {
    MutexGuard g(storageMutex); file = other.file; return *this;
  }
  ~StorageFile() { if (storageMutex) { MutexGuard g(storageMutex); file = File(); } }
  explicit operator bool() const { MutexGuard g(storageMutex); return bool(file); }
  size_t read(uint8_t* b, size_t n) { MutexGuard g(storageMutex); return file.read(b, n); }
  size_t write(const uint8_t* b, size_t n) { MutexGuard g(storageMutex); return file.write(b, n); }
  bool seek(uint32_t p) { MutexGuard g(storageMutex); return file.seek(p); }
  size_t size() const { MutexGuard g(storageMutex); return file.size(); }
  time_t getLastWrite() { MutexGuard g(storageMutex); return file.getLastWrite(); }
  bool isDirectory() { MutexGuard g(storageMutex); return file.isDirectory(); }
  String name() const { MutexGuard g(storageMutex); return String(file.name()); }
  StorageFile openNextFile() { MutexGuard g(storageMutex); return StorageFile(file.openNextFile()); }
  void flush() { MutexGuard g(storageMutex); file.flush(); }
  void close() { MutexGuard g(storageMutex); file.close(); }
};

class Storage {
public:
  StorageFile open(const String& p, const char* mode = FILE_READ) {
    MutexGuard g(storageMutex); return StorageFile(SD.open(p, mode));
  }
  bool exists(const String& p) { MutexGuard g(storageMutex); return SD.exists(p); }
  bool remove(const String& p) { MutexGuard g(storageMutex); return SD.remove(p); }
  bool rename(const String& a, const String& b) { MutexGuard g(storageMutex); return SD.rename(a, b); }
  bool mkdir(const String& p) { MutexGuard g(storageMutex); return SD.exists(p) || SD.mkdir(p); }
  uint64_t totalBytes() { MutexGuard g(storageMutex); return SD.totalBytes(); }
  uint64_t usedBytes() { MutexGuard g(storageMutex); return SD.usedBytes(); }
};
extern Storage storage;
