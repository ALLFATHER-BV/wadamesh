// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Wire.h>
#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include <helpers/RefCountedDigitalPin.h>
#include <driver/rtc_io.h>

// LilyGo T-Watch S3 power path is the AXP2101 PMU (I2C 0x34 on the sensor bus
// Wire SDA=10/SCL=11), NOT a GPIO rail. Battery voltage also comes from the
// PMU's fuel-gauge ADC, so there is no PIN_VBAT_READ divider.
#define PIN_ADC_CTRL     -1

// XPowersLib powers the PMU. The chip-select macro must be set before the
// header is included (the header dispatches on it).
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

class TWatchS3Board : public ESP32Board {
private:
  XPowersAXP2101 _pmu;
  bool _pmu_ok = false;

public:
  TWatchS3Board() { }

  void begin();

  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);
    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
    } else {
      esp_sleep_enable_ext1_wakeup((1ULL << P_LORA_DIO_1) | (1ULL << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    esp_deep_sleep_start();
  }

  // Power off for real: the AXP2101 cuts every rail and the crown turns the watch
  // back on. Deep sleep (the fallback) would keep waking on every LoRa packet.
  void powerOff() override {
    if (_pmu_ok) { _pmu.shutdown(); delay(100); }
    enterDeepSleep(0);
  }

  uint16_t getBattMilliVolts() override {
    if (!_pmu_ok) return 0;
    return _pmu.getBattVoltage();   // mV from the AXP2101 fuel gauge
  }

  // Crown (PWR) button short press via the AXP2101 PEK IRQ. Poll-and-clear:
  // returns true exactly once per short press (the IRQ is latched). The 6 s
  // long-press power-off is handled by the AXP2101 itself, not this.
  bool pollPowerKeyShortPress();

  const char* getManufacturerName() const override {
    return "LilyGo T-Watch S3";
  }
};
