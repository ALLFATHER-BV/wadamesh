// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

void attakyI2cInit();

bool attakyI2cLock(uint32_t timeout_ms = 50);
void attakyI2cUnlock();
