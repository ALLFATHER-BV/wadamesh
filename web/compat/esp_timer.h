#pragma once
// esp_timer: the monotonic microsecond clock LVGL's tick and the UI read, plus
// one-shot/periodic callbacks run on a host thread.
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;
typedef struct {
  esp_timer_cb_t callback;
  void* arg;
  esp_timer_dispatch_t dispatch_method;
  const char* name;
  bool skip_unhandled_events;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);
bool esp_timer_is_active(esp_timer_handle_t t);
#ifdef __cplusplus
}
#endif
