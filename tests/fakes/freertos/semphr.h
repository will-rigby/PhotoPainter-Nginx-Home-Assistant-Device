#pragma once
#include <mutex>
using SemaphoreHandle_t = std::recursive_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new std::recursive_mutex; }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m,unsigned) { m->lock(); }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); }
