#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
struct WadaHostQueue;
typedef WadaHostQueue* QueueHandle_t;
struct StaticQueue_t { void* reserved[8]; };
#define queueSEND_TO_BACK  ((BaseType_t)0)
#define queueSEND_TO_FRONT ((BaseType_t)1)
#define queueOVERWRITE     ((BaseType_t)2)
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
inline QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t* storage, StaticQueue_t* q) {
  (void)storage; (void)q; return xQueueCreate(length, item_size);
}
BaseType_t xQueueGenericSend(QueueHandle_t q, const void* item, TickType_t wait, BaseType_t pos);
inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t wait) { return xQueueGenericSend(q, item, wait, queueSEND_TO_BACK); }
inline BaseType_t xQueueSendToBack(QueueHandle_t q, const void* item, TickType_t wait) { return xQueueGenericSend(q, item, wait, queueSEND_TO_BACK); }
inline BaseType_t xQueueSendToFront(QueueHandle_t q, const void* item, TickType_t wait) { return xQueueGenericSend(q, item, wait, queueSEND_TO_FRONT); }
inline BaseType_t xQueueOverwrite(QueueHandle_t q, const void* item) { return xQueueGenericSend(q, item, 0, queueOVERWRITE); }
inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void* item, BaseType_t* woken) { (void)woken; return xQueueSend(q, item, 0); }
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t wait);
BaseType_t xQueuePeek(QueueHandle_t q, void* out, TickType_t wait);
inline BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void* out, BaseType_t* woken) { (void)woken; return xQueueReceive(q, out, 0); }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t xQueueReset(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);
#endif
