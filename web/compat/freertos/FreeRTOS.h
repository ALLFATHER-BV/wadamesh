#pragma once
// FreeRTOS on a host: tasks are std::threads, mutexes/semaphores/queues are built on
// std::mutex + std::condition_variable (web/compat/freertos_host.cpp). The tick is 1 ms,
// as in arduino-esp32 (configTICK_RATE_HZ 1000). Priorities and core affinity are
// accepted and ignored: the host scheduler decides. Critical sections are one
// recursive mutex per portMUX_TYPE (the ESP32 spinlock is recursive per core too).
//
// Faked, on purpose: a "mutex" is a binary semaphore without priority inheritance or an
// owner check (FreeRTOS would assert on a give from a non-owner; nothing in the UI does
// that). Stack sizes are ignored and uxTaskGetStackHighWaterMark reports a large value.
#include <stdint.h>
#include <stddef.h>
#include "esp_attr.h"

typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t  StackType_t;   // byte-sized so "bytes / sizeof(StackType_t)" stays bytes

#define pdFALSE  ((BaseType_t)0)
#define pdTRUE   ((BaseType_t)1)
#define pdPASS   (pdTRUE)
#define pdFAIL   (pdFALSE)
#define errQUEUE_EMPTY ((BaseType_t)0)
#define errQUEUE_FULL  ((BaseType_t)0)
#define errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY (-1)

#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 25
#define configMINIMAL_STACK_SIZE 768
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portTICK_RATE_MS portTICK_PERIOD_MS
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000U))
#define pdTICKS_TO_MS(t)  ((uint32_t)(t))
#define portNUM_PROCESSORS 2
#define tskNO_AFFINITY 0x7FFFFFFF
#define tskIDLE_PRIORITY 0
#define portYIELD() do {} while (0)
#define portYIELD_FROM_ISR(...) do {} while (0)
#define portENABLE_INTERRUPTS() do {} while (0)
#define portDISABLE_INTERRUPTS() do {} while (0)
#define configASSERT(x) do { (void)(x); } while (0)

#ifdef __cplusplus
#include <mutex>
struct portMUX_TYPE { std::recursive_mutex m; };
#define portMUX_INITIALIZER_UNLOCKED {}
inline void vPortEnterCritical(portMUX_TYPE* mux) { mux->m.lock(); }
inline void vPortExitCritical(portMUX_TYPE* mux)  { mux->m.unlock(); }
#define portENTER_CRITICAL(mux)        vPortEnterCritical(mux)
#define portEXIT_CRITICAL(mux)         vPortExitCritical(mux)
#define portENTER_CRITICAL_ISR(mux)    vPortEnterCritical(mux)
#define portEXIT_CRITICAL_ISR(mux)     vPortExitCritical(mux)
#define portENTER_CRITICAL_SAFE(mux)   vPortEnterCritical(mux)
#define portEXIT_CRITICAL_SAFE(mux)    vPortExitCritical(mux)
#define taskENTER_CRITICAL(mux)        vPortEnterCritical(mux)
#define taskEXIT_CRITICAL(mux)         vPortExitCritical(mux)
BaseType_t xPortGetCoreID();
inline bool xPortInIsrContext() { return false; }
#endif
