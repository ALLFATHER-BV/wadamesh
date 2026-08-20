#include "M9Board.h"
#include "soc/usb_serial_jtag_reg.h"
#include <Arduino.h>
#include <SPI.h>

void ThinkNodeM9Board::begin() {
  // Release the S3's native USB pads. GPIO19/20 are USB D-/D+ and the ROM's
  // USB-Serial-JTAG peripheral owns those pads from reset, including a D+
  // pull-up ON GPIO20. The M9's console goes through an external UART bridge
  // (U3) instead, and the schematic reuses GPIO20/21 as the KEYBOARD I2C bus
  // (nets ESP32-2_SDA/SCL) and GPIO19 as LCD_TE — so without this the
  // keyboard SDA line is clamped by the USB PHY and the controller never
  // answers (bring-up #8: probe found nothing on a correctly-wired bus).
  REG_CLR_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

  ESP32Board::begin(); // attaches PIN_VBAT_READ, brings up Wire on
                       // PIN_BOARD_SDA/SCL (7/6)

  // The one place this board's init genuinely has to differ from T-Deck/
  // Heltec V4's pattern: their displays use ST7789LCDDisplay's dedicated-
  // instance branch (LILYGO_TDECK / HELTEC_LORA_V4_TFT), which begins its
  // own SPI bus internally — so neither board's board.begin() touches SPI
  // at all, and the radio's bus only gets begun later, inside radio_init().
  // M9's display is on the class's "default" branch (display(&SPI, ...)) —
  // the only safe branch available without borrowing another board's
  // identity macro (HELTEC_LORA_V4_TFT is also checked in shared UITask.cpp
  // against HeltecV4Board-only methods, which would be a compile error here)
  // — and that branch does NOT begin the bus itself, it assumes the caller
  // already has. So a single bare SPI.begin() has to happen here, before
  // display.begin() runs (next, in main.cpp). Everything else in this
  // function mirrors TDeckBoard::begin()/HeltecV4Board::begin() as closely
  // as M9's actual hardware allows — no CS-deselect dance, no NSS pre-drive:
  // neither working board does either, and they don't need it.
  SPI.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI, PIN_TFT_CS);

  // Shared-SPI bus discipline: park the bus devices' chip-selects HIGH before
  // any traffic flows (LR1110 NSS=39, TFT CS=16). NOTE: do NOT touch GPIO35-37
  // on this board — the S3R8's octal PSRAM owns them (driving GPIO36 as a GPIO
  // wedged the PSRAM bus and hung boot right after prefs init, bring-up #3).
  // SCHEMATIC TRUTH (read from the V1.0 sheet, bring-up #9): the microSD *is*
  // on this shared SPI bus with CS = GPIO48 — the patch's "SD CS = 36" misread
  // the S3's PACKAGE pin 36 (SPICLK_N = GPIO48) as GPIO36.
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);
  // Park the SD's CS HIGH too: the card mounts lazily from the UI, so from
  // reset until then it would otherwise watch 80 MHz display + radio traffic
  // with its CS floating (no pull at reset on GPIO48) — a card that samples CS
  // low can drive MISO into the LR1110's reads. SD.begin later re-runs the
  // same pinMode, so this is purely the boot-window fix.
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  // Bring up both rails at boot and leave them claimed for now. Display
  // power-down (turnOff()) releases periph_power via the pointer passed into
  // ST7789LCDDisplay's constructor in target.cpp; the backlight is ours to
  // manage explicitly until UI-side sleep/backlight-timeout wiring exists for
  // this board (TODO — see M9_PORT.md).
  periph_power.begin();
  backlight.begin();
  periph_power.claim();
  backlight.claim();

  // NB: no user/BOOT button pinMode here — this board has NO user button at
  // all (schematic-confirmed: only a power-cut slider and a reset button,
  // neither a GPIO). PIN_USER_BTN is deliberately not defined for this env;
  // the old GPIO0 pull-up + UITask's screen-lock poll on it risked phantom
  // locks from a floating strapping pin.
}

void ThinkNodeM9Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
  // Deep-sleep wake on this board is TIMER-ONLY. The earlier "LR1110 DIO1
  // (GPIO42) wake" was electrically impossible: ESP32-S3 RTC pads are GPIO0-21,
  // so every rtc_gpio_* call on GPIO42/39 returned ESP_ERR_INVALID_ARG
  // (unchecked) and esp_sleep_enable_ext1_wakeup rejected the mask — sleep was
  // entered with NO wake source at all when secs==0. The only RTC-capable wake
  // candidate is ESP_WAKEUP (GPIO12, pulsed by the keyboard MCU), whose
  // edge/polarity/pulse width are undocumented — wire ext0/ext1 to it only
  // after characterizing it on hardware (M9_PORT.md Deferred #6).
  (void)pin_wake_btn;   // no wakeable button exists on this board
  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }
  // Release the active-LOW rails before sleeping: without an explicit hold
  // their driven levels are lost in deep sleep anyway, and releasing them
  // turns the LCD/GPS/sensor rail and backlight off deliberately.
  backlight.release();
  periph_power.release();

  esp_deep_sleep_start(); // CPU halts here and never returns
}

// secs=0 -> no timer, and no GPIO wake exists on this board: deep sleep with
// no wake source, i.e. genuinely off until the physical power slider cycles.
// (The touch UI's Power menu hides its Power-off row on M9 for this reason;
// nothing in the companion build calls this — it exists as the board API.)
void ThinkNodeM9Board::powerOff() { enterDeepSleep(0); }

uint16_t ThinkNodeM9Board::getBattMilliVolts() {
#if defined(PIN_VBAT_READ)
  analogReadResolution(12);
  // GPIO13 is ADC2_CH2 on the S3 (not ADC1 — the port doc's table was wrong),
  // and ADC2 is arbitrated against Wi-Fi: a sample taken while Wi-Fi owns the
  // unit times out and the HAL returns raw 0, which the calibration converts
  // to a small offset-mV value. Unfiltered, those samples dragged the average
  // toward 0 whenever Wi-Fi was busy (status bar snapping to 0%, garbage in
  // the battery log). Filter per-sample — half of 8 good samples still
  // averages fine — and hold the last good reading when every sample failed.
  static uint16_t s_last_good_mv = 0;
  uint32_t sum = 0;
  int n = 0;
  const int kSamples = 8;
  for (int i = 0; i < kSamples; i++) {
    const uint32_t mv = analogReadMilliVolts(PIN_VBAT_READ);
    if (mv >= 1250) { sum += mv; n++; }   // < 2.5 V at the pack through the 2:1 divider is impossible — ADC2-blocked read
  }
  if (n == 0) return s_last_good_mv;
  s_last_good_mv = (uint16_t)(2 * (sum / n));
  return s_last_good_mv;
#else
  return 0;
#endif
}
