// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#if defined(USE_SX1262)
  // Milestone 10 (SX1262-variant env): CustomSX1262/CustomSX1262Wrapper are
  // generic core-lib classes (angle include), unlike CustomLR1121 below --
  // T-Deck/Heltec V4's target.h include the exact same header.
  #include <helpers/radiolib/CustomSX1262Wrapper.h>
#else
  #include "CustomLR1121Wrapper.h"
#endif
#include "TLoraPagerBoard.h"
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  #include <helpers/ui/ST7796LCDDisplay.h>
  #include <helpers/ui/MomentaryButton.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

extern TLoraPagerBoard board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
