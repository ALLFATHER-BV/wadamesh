// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
extern uint32_t g_ms;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(uint32_t ticks) { g_ms += ticks; }
