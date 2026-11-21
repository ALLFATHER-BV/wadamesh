#include "M9Board.h"
#include "M9Keyboard.h"
#include "soc/usb_serial_jtag_reg.h"
#if !defined(M9_HOST_TEST)
#include "target.h" // radio_driver / sensors / display externs for enterDeepSleep
                    // (same source the base ESP32Board.cpp pulls them from)
#endif
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

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
  // PCF8563 (0x51), QMI8658A (0x6B), and QMC6309 (0x7C) all support 400 kHz Fast Mode.
  Wire.setClock(400000);
  Wire.setTimeOut(20);

#if defined(PIN_USB_5V_SENSE)
  pinMode(PIN_USB_5V_SENSE, INPUT);
#endif
#if defined(PIN_CHG_DONE)
  // U6 (LGS4056HDA-4.35) DONE pin is an open-drain NMOS output without an
  // external pull-up on the V1.0 schematic; enable the internal pull-up so
  // charging-in-progress reads HIGH and charge-complete reads LOW.
  pinMode(PIN_CHG_DONE, INPUT_PULLUP);
#endif
#if defined(PIN_KB_WAKEUP)
  pinMode(PIN_KB_WAKEUP, INPUT);
#endif
#if defined(THINKNODE_M9_BUZZER_PIN)
  pinMode(THINKNODE_M9_BUZZER_PIN, OUTPUT);
  digitalWrite(THINKNODE_M9_BUZZER_PIN, LOW);
#endif

  // Undo the pad holds enterDeepSleep() arms (backlight/rail/NSS/key-LED): a held pad
  // silently ignores every pinMode/digitalWrite below, so a timer wake would
  // otherwise come up with both rails latched OFF and the radio's NSS stuck.
  // The holds live in the RTC domain and survive ANY reset short of the power
  // slider actually cutting VBAT — the RST button included — so release them
  // on every boot rather than gating on ESP_RST_DEEPSLEEP like HeltecV4Board
  // does (harmless no-ops on a cold boot).
  rtc_gpio_hold_dis((gpio_num_t)PIN_TFT_BL_EN);
  rtc_gpio_hold_dis((gpio_num_t)PIN_PERIPH_POWER);
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)P_LORA_NSS); // digital pad (39 > RTC range), own API
#if defined(PIN_KEY_LED)
  gpio_hold_dis((gpio_num_t)PIN_KEY_LED);
#endif

#if defined(PIN_VBAT_READ)
  // Explicitly set 11 dB attenuation (full 0..3.1 V range) on GPIO13 (ADC2_CH2)
  // and prime s_last_good_mv before Wi-Fi is initialized so ADC2 contention can
  // never cause an initial 0 mV reading.
  analogSetPinAttenuation(PIN_VBAT_READ, ADC_11db);
  (void)getBattMilliVolts();
#endif

  // Shared-SPI bus discipline: park ALL three bus devices' chip-selects HIGH
  // BEFORE calling SPI.begin() (TFT CS=16, LR1110 NSS=39, SD CS=48) so no slave
  // sees SCK/MOSI edges with a floating CS line during bus initialization.
  // Pass -1 for SS so GPIO16 is not momentarily bound to the hardware SPI CS matrix.
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  SPI.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI, -1);

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

  // Everything below mirrors the base ESP32Board::enterDeepSleep sequence this
  // override hides — the rewrite exists only to drop the base's impossible
  // rtc_gpio/ext1 wake calls (see above), not its power-downs.
  //
  // Display first: ST7789LCDDisplay::begin() holds its own claim on
  // periph_power (via the pointer passed in target.cpp), so without turnOff()
  // the release below would only drop the refcount 2->1 and GPIO18 would
  // still be driven LOW (rail ON) when the CPU halts.
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif

  // The LR1110 hangs off the always-on 3V3 rail, NOT the GPIO18-gated one,
  // and with no wake source that could ever answer a packet, RX through deep
  // sleep is pure drain (the exact failure V4-R8's power-off path fixed) —
  // put the radio to sleep and park NSS high. GPIO39 is beyond the S3's RTC
  // pads (GPIO0-21), so the base class's rtc_gpio_hold_en would return
  // ESP_ERR_INVALID_ARG here; the digital-pad hold (latched through the sleep
  // by gpio_deep_sleep_hold_en below) is the S3 equivalent.
  radio_driver.powerOff();
  digitalWrite(P_LORA_NSS, HIGH);
  gpio_hold_en((gpio_num_t)P_LORA_NSS);

  // Tell the GPS to stop while its rail is still up (the rail cut below kills
  // it regardless, but stop() parks the provider's state cleanly).
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  Serial.flush();

  // Darken the keyboard backlight and silence the buzzer before halting.
#if defined(HAS_M9_KEYBOARD)
  m9KeyboardSetBacklight(0);
#endif
#if defined(PIN_KEY_LED)
  pinMode(PIN_KEY_LED, OUTPUT);
  digitalWrite(PIN_KEY_LED, LOW);
  gpio_hold_en((gpio_num_t)PIN_KEY_LED);
#endif
#if defined(THINKNODE_M9_BUZZER_PIN)
  pinMode(THINKNODE_M9_BUZZER_PIN, OUTPUT);
  digitalWrite(THINKNODE_M9_BUZZER_PIN, LOW);
#endif

  // Clear stale wake sources before arming the timer/keyboard wake, so a leftover from
  // PM/auto-light-sleep can't ghost-wake us — same guard as the base class.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }
#if defined(PIN_KB_WAKEUP)
  // GPIO12 (ESP32_WAKEUP from ESP32-S2 keyboard MCU) is an RTC GPIO (GPIO0-21).
  // Only arm keyboard wake for timed/hibernating sleep (secs > 0 || pin_wake_btn >= 0)
  // so powerOff() (secs == 0, pin_wake_btn == -1) stays off until the power slider cycles.
  if (secs > 0 || pin_wake_btn >= 0) {
    const int idle_lvl = digitalRead(PIN_KB_WAKEUP);
    if (idle_lvl == LOW) {
      rtc_gpio_pullup_dis((gpio_num_t)PIN_KB_WAKEUP);
      rtc_gpio_pulldown_en((gpio_num_t)PIN_KB_WAKEUP);
    } else {
      rtc_gpio_pulldown_dis((gpio_num_t)PIN_KB_WAKEUP);
      rtc_gpio_pullup_en((gpio_num_t)PIN_KB_WAKEUP);
    }
    esp_sleep_enable_ext1_wakeup(
        1ULL << PIN_KB_WAKEUP,
        idle_lvl == LOW ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW);
  }
#endif

  // Turn both active-LOW rails off. The backlight pad needs a re-route first:
  // UITask's applyBrightness attached LEDC ch7 to GPIO17, and a bare
  // digitalWrite doesn't take the pad back from the LEDC matrix signal (which
  // is why RefCountedDigitalPin's release() alone can't darken it) — same
  // pinMode dance as the V4-R8 power-off path.
  pinMode(PIN_TFT_BL_EN, OUTPUT); // re-route from LEDC ch7 back to plain GPIO
  backlight.release();            // refcount 1->0: drives HIGH = backlight off
  periph_power.release();         // 1->0 now the display let go: rail off

  // Latch the OFF levels through the sleep — the pads tristate the moment
  // esp_deep_sleep_start() runs and both active-LOW gates would drift back
  // on. GPIO17/18 ARE RTC pads, so their hold lives in the RTC domain and
  // survives deep sleep on its own; NSS (held above) additionally needs the
  // global digital-pad latch. All three are released again in begin().
  rtc_gpio_hold_en((gpio_num_t)PIN_TFT_BL_EN);
  rtc_gpio_hold_en((gpio_num_t)PIN_PERIPH_POWER);
  gpio_deep_sleep_hold_en();

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

#if defined(PIN_USB_5V_SENSE)
static bool usbSensePresent() { return digitalRead(PIN_USB_5V_SENSE) == LOW; }
#endif

int ThinkNodeM9Board::externalPowerPresent() {
#if defined(PIN_USB_5V_SENSE)
  // 5V_TEST (USB VBUS sense) on GPIO1 reads LOW while USB power is present, the
  // same as Meshtastic's M9 variant (EXT_PWR_DETECT 1, EXT_PWR_DETECT_VALUE LOW).
  // beta_103 read it active-HIGH, so every M9 showed charging on battery and
  // battery on USB (the screensaver followed, and the 3% switch-off never fired).
  return usbSensePresent() ? 1 : 0;
#else
  return -1;
#endif
}

int ThinkNodeM9Board::isCharging() const {
#if defined(PIN_USB_5V_SENSE)
  if (!usbSensePresent()) return 0; // USB VBUS absent -> discharging
#if defined(PIN_CHG_DONE)
  // U6 (LGS4056HDA-4.35) DONE# pin (GPIO8) is open-drain active-LOW with internal pull-up:
  // HIGH (1, pulled up while open-drain off) = actively charging (1),
  // LOW (0, pulled low by U6 when charge terminates) = charge complete / top-off (2).
  return digitalRead(PIN_CHG_DONE) == LOW ? 2 : 1;
#else
  return 1;
#endif
#else
  return -1;
#endif
}

bool ThinkNodeM9Board::isExternalPowered() {
  const int ext = externalPowerPresent();
  if (ext >= 0) return ext != 0;
  return getBattMilliVolts() >= 4250;
}
