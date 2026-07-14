#pragma once

#ifdef __cplusplus
extern "C" void set_boot_phase(int phase);
#endif

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <HeltecV4Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#ifdef DISPLAY_CLASS
#ifdef HELTEC_LORA_V4_OLED
    #include <helpers/ui/SSD1306Display.h>
#elif defined(HELTEC_LORA_V4_R8)
    // V4-R8 + Expansion Kit V2: TFT on hardware SPI (shared with micro-SD). Must
    // come before the HELTEC_LORA_V4_TFT branch — the R8 build defines both.
    #include "LGFXDisplay.h"
#elif defined(HELTEC_LORA_V4_TFT)
    #include <helpers/ui/ST7789LCDDisplay.h>
#endif
  #include <helpers/ui/MomentaryButton.h>
#endif

extern HeltecV4Board board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;   // raw SX1262 — driven directly by the Spectrum analyzer sweep
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();

#if defined(HELTEC_LORA_V4_R8)
  // Shared SPI2/FSPI bus (SCK=16, MOSI=15, MISO=45) driving the TFT and the
  // Expansion Kit V2 micro-SD (CS=3). Begun in radio_init(); the SD mount in
  // main.cpp reuses it. Returns nullptr if the build has no TFT SPI pins.
  SPIClass* heltecV4R8SharedSPI();
#endif

