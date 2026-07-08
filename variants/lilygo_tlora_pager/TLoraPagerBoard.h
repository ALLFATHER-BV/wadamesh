// SPDX-License-Identifier: GPL-3.0-or-later
//
// Board class for the LilyGo T-LoRa Pager (ESP32-S3). Unlike the T-Deck/
// Heltec V4, power-rail control and battery reporting go through I2C chips —
// an XL9555 GPIO expander gates LoRa/GPS/keyboard/SD power, and a BQ27220
// fuel gauge replaces the usual VBAT ADC divider (this board doesn't have
// one). begin() brings up just enough of that I2C chain for the rest of the
// app to run.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <helpers/ESP32Board.h>
#include <driver/rtc_io.h>
#include <ExtensionIOXL9555.hpp>
#include <GaugeBQ27220.hpp>

// XL9555 expander channels, matching LilyGoLib's LilyGo_LoRa_Pager.cpp begin()
// (the vendor's own bring-up — ground truth for this board). Channel 6 is the
// ST7796 panel's HARDWARE RESET, not a power rail: it's absent from both the
// LilyGoLib hardware doc's channel table and the canonical arduino-esp32
// pins_arduino.h on master, which is why the first bring-up pass concluded
// "TFT_RST isn't wired" (TFT_RST=-1) and left it floating — producing
// intermittent, cold-boot-worse black screens with a perfectly clean boot log
// (a panel held in hardware reset ignores all SPI, including TFT_eSPI's
// software-reset fallback).
#define PAGER_EXPAND_DRV_EN    0
#define PAGER_EXPAND_AMP_EN    1
#define PAGER_EXPAND_KB_RST    2
#define PAGER_EXPAND_LORA_EN   3
#define PAGER_EXPAND_GPS_EN    4
#define PAGER_EXPAND_NFC_EN    5
#define PAGER_EXPAND_DISP_RST  6
#define PAGER_EXPAND_GPS_RST   7
#define PAGER_EXPAND_KB_EN     8
#define PAGER_EXPAND_GPIO_EN   9
#define PAGER_EXPAND_SD_DET    10
#define PAGER_EXPAND_SD_PULLEN 11
#define PAGER_EXPAND_SD_EN     12

#define PAGER_XL9555_ADDR 0x20

// Shared-SPI chip selects/resets that are NOT otherwise claimed before first
// bus traffic. The display's CS is TFT_eSPI's; the radio's NSS/RESET belong to
// RadioLib — but only from radio.begin() onward, which runs AFTER the display
// has already been painting. Until every one of these is parked OUTPUT-HIGH,
// whichever chip's line floats low can sit half-selected on the live bus
// (LilyGoLib parks this exact set before its display init — initShareSPIPins()).
#define PAGER_PIN_SD_CS   21
#define PAGER_PIN_NFC_CS  39

// Gauge probe/refresh failed — never let the UI divide by zero.
#define PAGER_BATT_MILLIVOLTS_FALLBACK 3700

class TLoraPagerBoard : public ESP32Board {
public:
  void begin();

  void enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Make sure the DIO1 and NSS GPIOs are hold on required levels during deep sleep
    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH); // wake up on: recv LoRa packet
    } else {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1) | (1L << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH); // wake up on: recv LoRa packet OR wake btn
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    // Finally set ESP32 into sleep
    esp_deep_sleep_start(); // CPU halts here and never returns!
  }

  uint16_t getBattMilliVolts() {
    if (!gauge.refresh()) return PAGER_BATT_MILLIVOLTS_FALLBACK;
    uint16_t mv = gauge.getVoltage();
    return mv > 0 ? mv : PAGER_BATT_MILLIVOLTS_FALLBACK;
  }

  const char* getManufacturerName() const{
    return "LilyGo T-LoRa Pager";
  }

  // Mute/unmute the NS4150B amp via its XL9555 enable pin. Separate from the
  // boot-time rail bring-up in begin() (which drives this HIGH permanently,
  // for bus-integrity reasons unrelated to audio -- see begin()'s comment):
  // this is the runtime toggle the sound code brackets each chime/WAV with,
  // so the amp is only live while something is actually playing.
  void setAmpEnabled(bool on) {
    io_expander.digitalWrite(PAGER_EXPAND_AMP_EN, on ? HIGH : LOW);
  }

  // TODO: BQ25896 charger (XPowersLib) bring-up is out of scope for now — the
  // BQ27220 gauge alone covers battery %/mV for the UI. Add charge-status/
  // current reporting once the charger is wired in.

  ExtensionIOXL9555 io_expander;
  GaugeBQ27220 gauge;
};
