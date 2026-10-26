// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <assert.h>
typedef int* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new int(0); }
inline void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }
inline int xSemaphoreTake(SemaphoreHandle_t m, uint32_t) { assert(*m == 0); *m = 1; return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t m) { assert(*m == 1); *m = 0; return 1; }
