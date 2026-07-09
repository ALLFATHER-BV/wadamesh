// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Wire.h>
#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include <helpers/RefCountedDigitalPin.h>
#include <driver/rtc_io.h>

// Battery sense: PIN=1 sits behind RAK's voltage divider.
// ADC_MULTIPLIER is calibrated for the RAK3312 module's divider
// (3.0 * 1.73 * 1.187 * 1000 ≈ 6167) matching RAK3112Board.
#ifndef PIN_VBAT_READ
  #define PIN_VBAT_READ    1
#endif
// GPIO36 (SPID4) is used by the Octal PSRAM bus on ESP32-S3 — must NOT be
// touched as GPIO. The RAK TAP V2 baseboard's ADC divider is always-connected
// (no mux switch), so ADC_CTRL is not needed. Meshtastic does not define it
// for this board either.
#ifndef PIN_ADC_CTRL
  #define PIN_ADC_CTRL     -1
#endif
#define PIN_ADC_CTRL_ACTIVE    LOW
#define PIN_ADC_CTRL_INACTIVE  HIGH
#define ADC_MULTIPLIER   (3.0f * 1.73f * 1.187f * 1000.0f)
#define BATTERY_SAMPLES 8

class RAKTapV2Board : public ESP32Board {
private:
  bool adc_active_state;

public:
  // Peripherals 3.3V rail: GPIO14 (PIN_VEXT_EN).
  // The TFT panel, touch controller, and other I2C sensors are all
  // powered through this rail. It MUST be claimed before any of them
  // are initialised.
  RefCountedDigitalPin periph_power;

  RAKTapV2Board() : periph_power(PIN_VEXT_EN) { }

  void begin() {
    Serial.println("[BOOT] RAKTapV2Board::begin() entering"); Serial.flush();
    ESP32Board::begin();
    Serial.println("[BOOT] ESP32Board::begin() done"); Serial.flush();

    // Enable the 3.3V peripheral rail BEFORE any I2C or SPI peripheral
    // probe — without this the TFT panel has no power and will never
    // respond to init commands.
    periph_power.begin();
    Serial.println("[BOOT] periph_power.begin() done"); Serial.flush();

    // Auto-detect correct ADC_CTRL pin polarity (boards may vary).
    // When PIN_ADC_CTRL < 0 the ADC mux is not used — the battery
    // divider is always-connected (Meshtastic does not define it).
    if (PIN_ADC_CTRL >= 0) {
      pinMode(PIN_ADC_CTRL, INPUT);
      adc_active_state = !digitalRead(PIN_ADC_CTRL);

      pinMode(PIN_ADC_CTRL, OUTPUT);
      digitalWrite(PIN_ADC_CTRL, !adc_active_state); // Initially inactive
    }
    Serial.println("[BOOT] ADC_CTRL done"); Serial.flush();

    // Power-up the TFT backlight rail early so the panel is lit as soon
    // as ST7789LCDDisplay::begin() initialises the controller.
    if (PIN_TFT_LEDA_CTL >= 0) {
      pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
      digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
      delay(50);
    }

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_DEEPSLEEP) {
      long wakeup_source = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_source & (1 << P_LORA_DIO_1)) {  // received a LoRa packet (while in deep sleep)
        startup_reason = BD_STARTUP_RX_PACKET;
      }

      rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
      rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
    }
  }

  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Make sure the DIO1 and NSS GPIOs are held on required levels during deep sleep
    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet
    } else {
      esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1) | (1ULL << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet OR wake btn
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    // Finally set ESP32 into sleep
    esp_deep_sleep_start();   // CPU halts here and never returns!
  }

  void powerOff() override {
    enterDeepSleep(0);
  }

  uint16_t getBattMilliVolts() override {
    analogReadResolution(12);
    // Enable ADC control pin if applicable
    if (PIN_ADC_CTRL >= 0) {
      pinMode(PIN_ADC_CTRL, OUTPUT);
      digitalWrite(PIN_ADC_CTRL, adc_active_state);
      delayMicroseconds(50);
    }

    uint32_t raw = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
      raw += analogRead(PIN_VBAT_READ);
    }
    raw = raw / BATTERY_SAMPLES;

    if (PIN_ADC_CTRL >= 0) {
      digitalWrite(PIN_ADC_CTRL, !adc_active_state);  // Disable ADC control
    }
    return (uint16_t)((ADC_MULTIPLIER * raw) / 4096);
  }

  const char* getManufacturerName() const override {
    return "RAK WisMesh Tap V2";
  }
};
