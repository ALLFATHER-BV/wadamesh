// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(HAS_ATTAKY_MESH_KEYBOARD) && defined(ESP32)

#include <stdint.h>

enum : uint8_t {
	ATTAKY_NOTIFY_OFF   = 0,
	ATTAKY_NOTIFY_BLUE  = 1u << 0,
	ATTAKY_NOTIFY_GREEN = 1u << 1,
	ATTAKY_NOTIFY_RED   = 1u << 2,
};

void attakyKeyboardPoll(bool active);
int attakyKeyboardReadKey();
bool attakyKeyboardPresent();
bool attakyKeyboardSetNotificationColor(uint8_t color_mask);

#endif
