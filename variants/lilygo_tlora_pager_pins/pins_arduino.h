// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lives in its own folder, separate from variants/lilygo_tlora_pager/ (our
// board glue: TLoraPagerBoard.*, target.*, CustomLR1121*), because PlatformIO's
// arduino-esp32 build script unconditionally compiles every source file found
// under board_build.variants_dir/<board.variant>/ as a standalone "framework
// variant" library (platformio-build.py's corelib_env.BuildSources call) --
// a build context with none of our own lib_deps include paths. Since this
// board has no framework-bundled variant, we must point variants_dir at our
// own repo; keeping this pin map alone in board.variant's folder means that
// auto-compile step finds nothing but a header (a no-op), while our .cpp
// files stay reachable only through our own build_src_filter, exactly once.
#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// USB descriptor — matches boards/lilygo-t-lora-pager.json's hwids (0x303A/0x82D4).
#define USB_VID 0x303A
#define USB_PID 0x82D4
#define USB_MANUFACTURER "LilyGo"
#define USB_PRODUCT "wadamesh T-LoRa Pager"

static const uint8_t LED_BUILTIN = 255; // no onboard status LED
#define BUILTIN_LED  LED_BUILTIN
#define LED_BUILTIN LED_BUILTIN

static const uint8_t TX = 43;
static const uint8_t RX = 44;

// I2C bus: XL9555 expander, BQ25896 PMU, BQ27220 gauge, PCF85063 RTC, BHI260AP
// IMU, DRV2605 haptics, ES8311 codec, TCA8418 keyboard all share this bus.
static const uint8_t SDA = 3;
static const uint8_t SCL = 2;

// Shared SPI bus: ST7796 display, LR1121/SX1262 radio, and microSD all share
// this bus (like the T-Deck's 40/41/38) — CS discipline + SPI transactions
// keep them from fighting each other. SS defaults to the microSD CS.
static const uint8_t SS   = 21;
static const uint8_t MOSI = 34;
static const uint8_t MISO = 33;
static const uint8_t SCK  = 35;

static const uint8_t A0 = 1;
static const uint8_t A1 = 2;
static const uint8_t A2 = 3;
static const uint8_t A3 = 4;
static const uint8_t A4 = 5;
static const uint8_t A5 = 6;
static const uint8_t A6 = 7;
static const uint8_t A7 = 8;
static const uint8_t A8 = 9;
static const uint8_t A9 = 10;
static const uint8_t A10 = 11;
static const uint8_t A11 = 12;
static const uint8_t A12 = 13;
static const uint8_t A13 = 14;
static const uint8_t A14 = 15;
static const uint8_t A15 = 16;
static const uint8_t A16 = 17;
static const uint8_t A17 = 18;
static const uint8_t A18 = 19;
static const uint8_t A19 = 20;

static const uint8_t T1 = 1;
static const uint8_t T2 = 2;
static const uint8_t T3 = 3;
static const uint8_t T4 = 4;
static const uint8_t T5 = 5;
static const uint8_t T6 = 6;
static const uint8_t T7 = 7;
static const uint8_t T8 = 8;
static const uint8_t T9 = 9;
static const uint8_t T10 = 10;
static const uint8_t T11 = 11;
static const uint8_t T12 = 12;
static const uint8_t T13 = 13;
static const uint8_t T14 = 14;

#endif /* Pins_Arduino_h */
