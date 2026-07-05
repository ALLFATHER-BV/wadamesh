#pragma once

// Tanmatsu target wiring. Unlike the S3 boards (RadioLib over SPI), the radio here
// is the coprocessor LoRa reached via TanmatsuLoraRadio, so there is no RADIO_CLASS/
// Module/SPI — radio_driver IS the bridge. Everything the app pulls off `radio_driver`
// (MyMesh.cpp) is implemented by TanmatsuLoraRadio.

#include <TanmatsuBoard.h>
#include "TanmatsuLoraRadio.h"
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)

extern TanmatsuBoard        board;
extern TanmatsuLoraRadio    radio_driver;
extern ClockFloorRTC rtc_clock;

#include <TanmatsuDisplay.h>
#include <helpers/SensorManager.h>
extern DISPLAY_CLASS  display;   // LVGL flush target (badge-bsp)
extern SensorManager  sensors;   // telemetry (base/no-op for now)
// TODO(device): wire the keyboard/touch as an lv_indev from bsp/input.h.

bool radio_init();
mesh::LocalIdentity radio_new_identity();
