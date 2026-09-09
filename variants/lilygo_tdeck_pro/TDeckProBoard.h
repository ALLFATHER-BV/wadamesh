// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <helpers/ESP32Board.h>

class TDeckProBoard : public ESP32Board {
public:
  void begin();
  uint16_t getBattMilliVolts();

  void enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const gpio_num_t dio = (gpio_num_t)P_LORA_DIO_1;
    rtc_gpio_init(dio);
    rtc_gpio_set_direction(dio, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(dio);
    esp_sleep_enable_ext1_wakeup(1ULL << P_LORA_DIO_1, ESP_EXT1_WAKEUP_ANY_HIGH);

    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
    if (pin_wake_btn >= 0) {
      const gpio_num_t wake = (gpio_num_t)pin_wake_btn;
      rtc_gpio_init(wake);
      rtc_gpio_set_direction(wake, RTC_GPIO_MODE_INPUT_ONLY);
      rtc_gpio_pullup_en(wake);
      rtc_gpio_pulldown_dis(wake);
      esp_sleep_enable_ext0_wakeup(wake, 0);
    }
    if (secs > 0) esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    esp_deep_sleep_start();
  }

  const char* getManufacturerName() const { return "LilyGo T-Deck Pro"; }

private:
  bool bqRead(uint8_t reg, uint8_t& value);
  bool bqWrite(uint8_t reg, uint8_t value);

  bool _bq_present = false;
  bool _bq_pending = false;
  uint16_t _batt_mv = 3700;
  uint32_t _bq_retry_ms = 0;
};