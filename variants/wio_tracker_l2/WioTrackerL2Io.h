// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

namespace WioTrackerL2Io {

bool begin();
bool ready();
bool readWakeButton(bool& pressed);
bool readSdPresent(bool& present);

bool setLcdPower(bool enabled);
bool setLcdResetReleased(bool released);
bool setTouchResetReleased(bool released);
bool setGnssPower(bool enabled);
bool setGnssResetReleased(bool released);
bool setSdPower(bool enabled);
bool setBatterySense(bool enabled);
bool setAudioPaPower(bool enabled);

}  // namespace WioTrackerL2Io