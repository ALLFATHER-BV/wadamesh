#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
struct WadaHostTask;
typedef WadaHostTask* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
struct StaticTask_t { void* reserved; };
typedef enum { eNoAction = 0, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* param,
                                   UBaseType_t prio, TaskHandle_t* out, BaseType_t core);
inline BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* param,
                              UBaseType_t prio, TaskHandle_t* out) {
  return xTaskCreatePinnedToCore(fn, name, stack_depth, param, prio, out, tskNO_AFFINITY);
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* param,
                                           UBaseType_t prio, StackType_t* stack, StaticTask_t* tcb, BaseType_t core);
inline TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* param,
                                      UBaseType_t prio, StackType_t* stack, StaticTask_t* tcb) {
  return xTaskCreateStaticPinnedToCore(fn, name, stack_depth, param, prio, stack, tcb, tskNO_AFFINITY);
}
// vTaskDelete(nullptr) ends the calling task (it unwinds the thread; never returns).
// Deleting ANOTHER task is not supported on a host and is logged + ignored.
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t* prev, TickType_t inc);
TickType_t xTaskGetTickCount();
inline TickType_t xTaskGetTickCountFromISR() { return xTaskGetTickCount(); }
TaskHandle_t xTaskGetCurrentTaskHandle();
inline TaskHandle_t xTaskGetCurrentTaskHandleForCPU(BaseType_t cpu) { (void)cpu; return xTaskGetCurrentTaskHandle(); }
inline TaskHandle_t xTaskGetIdleTaskHandleForCPU(BaseType_t cpu) { (void)cpu; return nullptr; }
const char* pcTaskGetName(TaskHandle_t t);
#define pcTaskGetTaskName pcTaskGetName
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
inline UBaseType_t uxTaskPriorityGet(TaskHandle_t t) { (void)t; return 1; }
inline void vTaskPrioritySet(TaskHandle_t t, UBaseType_t p) { (void)t; (void)p; }
inline void vTaskSuspendAll() {}
inline BaseType_t xTaskResumeAll() { return pdFALSE; }
inline eTaskState eTaskGetState(TaskHandle_t t) { return t ? eRunning : eDeleted; }
inline UBaseType_t uxTaskGetNumberOfTasks() { return 8; }
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait);
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t* value, TickType_t wait);
inline void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t* woken) { (void)woken; xTaskNotifyGive(t); }
#endif
