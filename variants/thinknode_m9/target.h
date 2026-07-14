#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include "../../src/helpers/ClockFloorRTC.h" // monotonic send-timestamp floor (issue #89)
#include "M9Board.h"
#include <RadioLib.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/radiolib/CustomLR1110Wrapper.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#ifdef DISPLAY_CLASS
#include <helpers/ui/MomentaryButton.h>
#include <helpers/ui/ST7789LCDDisplay.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"
#if defined(HAS_M9_KEYBOARD)
#include "M9Keyboard.h"
#endif

extern ThinkNodeM9Board board;
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

// Shared SPI bus accessor for the microSD mount code in UITask.cpp (mirrors
// tdeckSharedSPI()). M9's radio/display use the global `SPI` instance
// directly (no dedicated local SPIClass — see target.cpp), so this just
// hands that same instance to SD.begin().
SPIClass *m9SharedSPI();

void m9SetBacklight(bool on);
