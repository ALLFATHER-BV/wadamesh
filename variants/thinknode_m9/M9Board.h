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
  int externalPowerPresent();
  int isCharging() const;
  bool isExternalPowered() override;

  const char *getManufacturerName() const override {
    return "Elecrow ThinkNode M9";
  }
};

// Battery EMA + 20 s publish hold + charge-state transition reset.
struct M9BatterySmoother {
  float         ema      = 0.0f;
  uint16_t      pub      = 0;
  unsigned long pub_ms   = 0;
  unsigned long flip_ms  = 0;
  int           ext_prev = -2;

  uint16_t sample(uint16_t raw) {
    if (raw == 0) return 0;
    if (ema < 1.0f) {
      ema = (float)raw;
    } else {
      const float d = (float)raw - ema;
      if (d > 70.0f || d < -70.0f) ema = (float)raw;
      else                         ema += d * 0.15f;
    }
    return (uint16_t)(ema + 0.5f);
  }

  uint16_t smooth(uint16_t raw, int ext_present, int chg_state, unsigned long now_ms) {
    const uint16_t cur = sample(raw);
    if (cur == 0) return 0;
    const int ext_now = ((ext_present & 1) << 2) | (chg_state & 3);
    const bool charge_flip = (pub != 0) && ext_prev != -2 && ext_now != ext_prev;
    ext_prev = ext_now;
    if (pub == 0 || charge_flip || (unsigned long)(now_ms - pub_ms) >= 20000UL) {
      pub     = cur;
      pub_ms  = now_ms;
      flip_ms = charge_flip ? (now_ms ? now_ms : 1UL) : 0UL;
    } else if (flip_ms && (unsigned long)(now_ms - flip_ms) >= 3000UL) {
      pub     = cur;
      pub_ms  = now_ms;
      flip_ms = 0UL;
    }
    return pub;
  }
};
