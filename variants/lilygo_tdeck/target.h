#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TDeckBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  #include <helpers/ui/ST7789LCDDisplay.h>
  #include <helpers/ui/MomentaryButton.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

extern TDeckBoard board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;   // raw SX1262 — driven directly by the Spectrum analyzer sweep
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

// Shared SPI bus instance (SCLK40/MISO38/MOSI41), already begun for the LoRa
// radio. The microSD slot (CS=39) reuses this so it doesn't fight the radio for
// the bus. Returns nullptr if the build has no LoRa SPI pins.
SPIClass* tdeckSharedSPI();

bool radio_init();
mesh::LocalIdentity radio_new_identity();
