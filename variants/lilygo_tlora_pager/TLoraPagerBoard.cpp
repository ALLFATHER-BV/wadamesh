// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "TLoraPagerBoard.h"

void TLoraPagerBoard::begin() {

  ESP32Board::begin();

  // XL9555/BQ27220 sit on the shared I2C bus (SDA 3 / SCL 2). ESP32Board::begin()
  // only calls the pin-less Wire.begin() unless PIN_BOARD_SDA/SCL are defined
  // for this env, so re-init explicitly with this board's pins before probing
  // either chip.
  Wire.begin(SDA, SCL);

  // Configure user button
  pinMode(PIN_USER_BTN, INPUT);

  // Park every shared-SPI select/reset OUTPUT-HIGH before ANY bus traffic
  // (display.begin() runs right after board.begin() in main.cpp, and the
  // radio's NSS/RESET stay unclaimed until radio.begin() well after that).
  // A floating select can leave that chip half-listening on the live bus;
  // LilyGoLib parks this same set before its display init (initShareSPIPins()).
  const uint8_t spi_selects[] = { P_LORA_NSS, P_LORA_RESET, PAGER_PIN_SD_CS, PAGER_PIN_NFC_CS };
  for (uint8_t pin : spi_selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  const bool expander_ok = io_expander.begin(Wire, PAGER_XL9555_ADDR);
  // Not just a nicety: every power rail AND the display's hardware reset hang
  // off this chip — if the probe fails silently, the symptom downstream is a
  // black screen with an otherwise clean boot log.
  Serial.printf("[BOOT] xl9555 %s\n", expander_ok ? "ok" : "PROBE FAILED");
  if (expander_ok) {
    // Drive EVERY channel the vendor drives (LilyGoLib LilyGo_LoRa_Pager.cpp
    // begin()), not just the rails this app uses: DRV2605 (haptics) and the
    // ES8311/amp are on the shared I2C bus and the ST25R3916 (NFC) is on the
    // shared SPI bus — an UNPOWERED chip's pads can clamp a shared bus through
    // its ESD diodes, so "off because unused" isn't safe here. DISP_RST first,
    // same as the vendor list, so the panel's reset line is driven (not
    // floating) as early as possible.
    const uint8_t rails[] = {
      PAGER_EXPAND_DISP_RST,
      PAGER_EXPAND_KB_RST,
      PAGER_EXPAND_LORA_EN,
      PAGER_EXPAND_GPS_EN,
      PAGER_EXPAND_DRV_EN,
      PAGER_EXPAND_AMP_EN,
      PAGER_EXPAND_NFC_EN,
      PAGER_EXPAND_GPS_RST,
      PAGER_EXPAND_KB_EN,
      PAGER_EXPAND_GPIO_EN,
      PAGER_EXPAND_SD_EN,
    };
    for (uint8_t ch : rails) {
      io_expander.pinMode(ch, OUTPUT);
      io_expander.digitalWrite(ch, HIGH);
      delay(1); // stagger rail turn-on, mirrors the vendor's bring-up order
    }

    io_expander.pinMode(PAGER_EXPAND_SD_DET, INPUT);
    io_expander.pinMode(PAGER_EXPAND_SD_PULLEN, INPUT);

    // Hardware-reset the ST7796 panel (XL9555 ch6 — see TLoraPagerBoard.h for
    // why this line was invisible to the first bring-up pass). Same pulse shape
    // as the vendor's: LOW 50ms, back HIGH. display.begin()'s own software
    // reset + its mandated 120ms wait then run against a freshly-reset panel.
    io_expander.digitalWrite(PAGER_EXPAND_DISP_RST, LOW);
    delay(50);
    io_expander.digitalWrite(PAGER_EXPAND_DISP_RST, HIGH);

    delay(50); // let rails + panel settle before anything downstream probes them
  }

  gauge.begin(Wire);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET; // received a LoRa packet (while in deep sleep)
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}
