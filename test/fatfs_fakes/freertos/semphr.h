// SPDX-License-Identifier: GPL-3.0-or-later
// What ffconf.h needs from FreeRTOS: the type of FatFs' volume lock. Host tests only.
#pragma once
typedef void* SemaphoreHandle_t;
