#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
struct WadaHostSem;
typedef WadaHostSem* SemaphoreHandle_t;
struct StaticSemaphore_t { void* reserved[8]; };
SemaphoreHandle_t wadaSemCreate(UBaseType_t max_count, UBaseType_t initial, bool recursive);
#define xSemaphoreCreateMutex()             wadaSemCreate(1, 1, false)
#define xSemaphoreCreateRecursiveMutex()    wadaSemCreate(1, 1, true)
#define xSemaphoreCreateBinary()            wadaSemCreate(1, 0, false)
#define xSemaphoreCreateCounting(mx, init)  wadaSemCreate((mx), (init), false)
#define xSemaphoreCreateMutexStatic(buf)    ((void)(buf), wadaSemCreate(1, 1, false))
#define xSemaphoreCreateBinaryStatic(buf)   ((void)(buf), wadaSemCreate(1, 0, false))
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
#define xSemaphoreTakeRecursive(s, w)   xSemaphoreTake((s), (w))
#define xSemaphoreGiveRecursive(s)      xSemaphoreGive(s)
#define xSemaphoreTakeFromISR(s, woken) ((void)(woken), xSemaphoreTake((s), 0))
#define xSemaphoreGiveFromISR(s, woken) ((void)(woken), xSemaphoreGive(s))
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
#endif
