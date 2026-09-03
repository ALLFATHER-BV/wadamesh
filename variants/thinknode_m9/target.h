#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include "../../src/helpers/ClockFloorRTC.h" // monotonic send-timestamp floor (issue #89)
#include "../../src/helpers/HardwareRtcClock.h" // PCF8563 retention across power-off (issue #383)
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
// Wadamesh-owned NMEA provider (the core one plus speed/course for wada.sys.gps)
#include "../../src/helpers/WadaNmeaLocationProvider.h"
#if defined(HAS_M9_KEYBOARD)
#include "M9Keyboard.h"
#endif
#if defined(HAS_M9_COMPASS)
#include "M9Compass.h"
#endif
#if defined(HAS_M9_IMU)
#include "M9Imu.h"
#endif

extern ThinkNodeM9Board board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;
extern HardwareRtcClock hw_rtc;   // the board's PCF8563; ClockFloorRTC's fallback
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
extern DISPLAY_CLASS display;
// (no user_btn: the M9 has no user/BOOT button — see target.cpp)
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();

// Shared SPI bus accessor for the microSD mount code in UITask.cpp (mirrors
// tdeckSharedSPI()). M9's radio/display use the global `SPI` instance
// directly (no dedicated local SPIClass — see target.cpp), so this just
// hands that same instance to SD.begin().
SPIClass *m9SharedSPI();

// Speed/course over ground from the GPS for the Lua host (HAS_GPS_MOTION).
// Plain function so UITask.cpp needs no provider type: false = no fix / no
// RMC yet; course_deg is NAN while not moving (see WadaNmeaLocationProvider).
bool wadaGpsMotion(float *speed_kmh, float *course_deg);
