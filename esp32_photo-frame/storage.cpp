// SPDX-License-Identifier: 0BSD
#include "storage.h"
SemaphoreHandle_t storageMutex = nullptr;
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t catalogMutex = nullptr;
SemaphoreHandle_t configMutex = nullptr;
Storage storage;
void synchronizationBegin() {
  storageMutex = xSemaphoreCreateRecursiveMutex();
  stateMutex = xSemaphoreCreateRecursiveMutex();
  catalogMutex = xSemaphoreCreateRecursiveMutex();
  configMutex = xSemaphoreCreateRecursiveMutex();
  configASSERT(storageMutex && stateMutex && catalogMutex && configMutex);
}
