#pragma once

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <helpers/ESP32Board.h>
#include <helpers/RefCountedDigitalPin.h>

// ---------------------------------------------------------------------------
// ThinkNode M9 — ESP32-S3R8 / LR1110 / 2.4" 240x320 ST7789 / QWERTY keyboard.
//
// Pin map verified against the V1.0 schematic + Meshtastic boot log during
// Specter (ESP-IDF) bring-up; carried over here unchanged. See M9_PORT.md
// for the full table and the hardware-verify checklist.
//
// Two independently switched power rails, BOTH active-low:
//   - PIN_PERIPH_POWER (18, P-MOS): gates the LCD / GPS / sensor rail.
//   - PIN_TFT_BL_EN    (17, PNP):   backlight, separate from the rail above.
// ST7789LCDDisplay (vendored core) hardcodes PIN_TFT_LEDA_CTL active-HIGH, and
// references the macro unconditionally (no #ifdef guard — leaving it undefined
// is a compile error, not a no-op), so platformio.ini sets PIN_TFT_LEDA_CTL=-1
// to make the class's own `!= -1` check skip the pin entirely, and we drive
// BL_EN (active-LOW) ourselves here instead.
// ---------------------------------------------------------------------------

class ThinkNodeM9Board : public ESP32Board {
public:
  RefCountedDigitalPin periph_power; // GPIO18, active LOW — LCD/GPS/sensor rail
  RefCountedDigitalPin backlight;    // GPIO17, active LOW — PNP backlight

  ThinkNodeM9Board()
      : periph_power(PIN_PERIPH_POWER, LOW), backlight(PIN_TFT_BL_EN, LOW) {}

  void begin();

  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1);
  void powerOff() override;

  uint16_t getBattMilliVolts() override;

  const char *getManufacturerName() const override {
    return "Elecrow ThinkNode M9";
  }
};
