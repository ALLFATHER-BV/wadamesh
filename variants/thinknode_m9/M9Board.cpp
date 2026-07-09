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
  // the S3's PACKAGE pin 36 (SPICLK_N = GPIO48) as GPIO36. SD support returns
  // with PIN_SD_CS=48 once the keyboard/radio bring-up settles (M9_PORT.md).
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);

  // Bring up both rails at boot and leave them claimed for now. Display
  // power-down (turnOff()) releases periph_power via the pointer passed into
  // ST7789LCDDisplay's constructor in target.cpp; the backlight is ours to
  // manage explicitly until UI-side sleep/backlight-timeout wiring exists for
  // this board (TODO — see M9_PORT.md).
  periph_power.begin();
  backlight.begin();
  periph_power.claim();
  backlight.claim();

  pinMode(PIN_USER_BTN, INPUT_PULLUP); // BOOT button, GPIO0, active LOW

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    uint64_t wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1ULL << P_LORA_DIO_1)) {
      startup_reason =
          BD_STARTUP_RX_PACKET; // woke from an incoming LoRa packet
    }
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}

void ThinkNodeM9Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // Hold DIO1 / NSS at their required levels through sleep (LR1110 IRQ wake).
  rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

  if (pin_wake_btn < 0) {
    esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1),
                                 ESP_EXT1_WAKEUP_ANY_HIGH);
  } else {
    esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1) |
                                     (1ULL << pin_wake_btn),
                                 ESP_EXT1_WAKEUP_ANY_HIGH);
  }

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }

  esp_deep_sleep_start(); // CPU halts here and never returns
}

void ThinkNodeM9Board::powerOff() { enterDeepSleep(0); }

uint16_t ThinkNodeM9Board::getBattMilliVolts() {
#if defined(PIN_VBAT_READ)
  analogReadResolution(12);
  uint32_t sum = 0;
  const int kSamples = 8;
  for (int i = 0; i < kSamples; i++) {
    sum += analogReadMilliVolts(PIN_VBAT_READ);
  }
  return (uint16_t)(2 * (sum / kSamples));
#else
  return 0;
#endif
}
