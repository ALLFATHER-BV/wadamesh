// SPDX-License-Identifier: GPL-3.0-or-later

#include "AttakyMeshSeriesBoard.h"
#include "AttakySharedI2C.h"
#include <SPI.h>
#include <Wire.h>

#define MAX17048_ADDR        0x36
#define MAX17048_REG_VCELL   0x02

void AttakyMeshSeriesBoard::begin() {
    ESP32Board::begin();

    attakyI2cInit();

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_DEEPSLEEP) {
      long wakeup_source = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_source & (1 << P_LORA_DIO_1)) {
        startup_reason = BD_STARTUP_RX_PACKET;
      }

      rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
      rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
    }

    // The global-SPI display path does not initialize the bus.
#if defined(PIN_TFT_LEDA_CTL) && (PIN_TFT_LEDA_CTL >= 0)
    pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
    digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
#endif
    SPI.begin(PIN_TFT_SCL, -1, PIN_TFT_SDA, PIN_TFT_CS);
  }

  void AttakyMeshSeriesBoard::onBeforeTransmit(void) { }

  void AttakyMeshSeriesBoard::onAfterTransmit(void) { }

  void AttakyMeshSeriesBoard::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
    } else {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1) | (1L << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    esp_deep_sleep_start();
  }

  void AttakyMeshSeriesBoard::powerOff()  {
    enterDeepSleep(0);
  }

  // MAX17048 VCELL is 78.125 uV/LSB.
  uint16_t AttakyMeshSeriesBoard::getBattMilliVolts()  {
    uint32_t now = millis();
    if (batt_cache_mv != 0 && (uint32_t)(now - batt_last_ms) < 2000) return batt_cache_mv;
    if (!attakyI2cLock(30)) return batt_cache_mv ? batt_cache_mv : 4000;
    uint16_t raw = 0;
    bool ok = false;
    Wire.beginTransmission(MAX17048_ADDR);
    Wire.write(MAX17048_REG_VCELL);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)MAX17048_ADDR, 2) == 2) {
      raw = ((uint16_t)Wire.read() << 8) | Wire.read();
      ok = true;
    }
    attakyI2cUnlock();
    if (ok) {
      batt_cache_mv = (uint16_t)(((uint32_t)raw * 5) / 64);
      batt_last_ms  = now;
    }
    return batt_cache_mv ? batt_cache_mv : 4000;
  }

  const char* AttakyMeshSeriesBoard::getManufacturerName() const {
    return "Attaky Core";
  }
