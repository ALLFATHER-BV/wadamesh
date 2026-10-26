#pragma once
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static int m; return &m; }
inline int xSemaphoreTake(SemaphoreHandle_t, int) { return 1; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}
