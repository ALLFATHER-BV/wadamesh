// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" void set_boot_phase(int phase);
#endif

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <RakTapV2Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#if defined(RAK_WISMESH_TAP_V2)
  #include "LGFXDisplay.h"
  #define DISPLAY_CLASS LGFXDisplay
  #include <helpers/ui/MomentaryButton.h>
#elif defined(DISPLAY_CLASS)
  #include <helpers/ui/ST7789LCDDisplay.h>
  #include <helpers/ui/MomentaryButton.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

extern RAKTapV2Board board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

// Shared SPI bus instance (SCLK=5/MISO=3/MOSI=6), already begun for the LoRa
// radio. The microSD slot (CS=2) reuses this so it doesn't fight the radio for
// the bus. Returns nullptr if the build has no LoRa SPI pins.
SPIClass* rakTapV2SharedSPI();

bool radio_init();
mesh::LocalIdentity radio_new_identity();
