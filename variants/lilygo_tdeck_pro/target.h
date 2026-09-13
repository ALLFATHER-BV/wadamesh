// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/ui/MomentaryButton.h>

#include "TDeckProBoard.h"
#include "TDeckProDisplay.h"
#include "../../src/helpers/ClockFloorRTC.h"
#include "../../src/helpers/WadaNmeaLocationProvider.h"
#include "helpers/sensors/EnvironmentSensorManager.h"

extern TDeckProBoard board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;
extern TDeckProDisplay display;
extern MomentaryButton user_btn;

SPIClass* tdeckSharedSPI();
bool radio_init();
mesh::LocalIdentity radio_new_identity();