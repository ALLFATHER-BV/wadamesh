// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(HAS_ATTAKY_MESH_KEYBOARD) && defined(ESP32)

#include <stdint.h>

void attakyKeyboardPoll(bool active);
int attakyKeyboardReadKey();
bool attakyKeyboardPresent();

#endif
