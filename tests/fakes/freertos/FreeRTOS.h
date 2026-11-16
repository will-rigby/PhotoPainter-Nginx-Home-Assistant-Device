#pragma once
#include <assert.h>
#define portMAX_DELAY 0xffffffffu
#define configASSERT assert
inline void vTaskDelay(unsigned) {}
