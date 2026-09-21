// SPDX-License-Identifier: GPL-3.0-or-later
//
// Board class for the LilyGo T-Deck Max (ESP32-S3). Same SoC, GDEQ031T10
// e-paper panel, SX1262, TCA8418 keyboard and CST328/CST3530 touch as the
// T-Deck Pro, so the Pro's display and touch drivers are reused unchanged.
// What differs is power and reset routing: on the Max the peripheral power
// enables, resets and switches go through an XL9555 I/O expander on the shared
// I2C bus instead of direct GPIOs, and battery reporting comes from the
// BQ27220 fuel gauge (the Pro's BQ25896 charger at 0x6B is not fitted; the Max
// carries an SY6970 at 0x6A). Line map, boot state and reset timings follow
// the Meck firmware's working bring-up of this exact board.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/rtc_io.h>
#include <helpers/ESP32Board.h>
#include <ExtensionIOXL9555.hpp>
#include <GaugeBQ27220.hpp>

#define TDMAX_XL9555_ADDR 0x20

// XL9555 lines. 0-7 are port 0, 8-15 are port 1.
#define TDMAX_XL_6609_EN     0   // HIGH: A7682E modem supply (SGM6609 boost)
#define TDMAX_XL_LORA_EN     1   // HIGH: SX1262 supply (the Pro drives GPIO 46 for this)
#define TDMAX_XL_GPS_EN      2   // HIGH: GPS supply (the Pro drives GPIO 39 for this)
#define TDMAX_XL_1V8_EN      3   // HIGH: BHI260AP 1.8 V supply
#define TDMAX_XL_LORA_SEL    4   // HIGH: internal antenna, LOW: external (SKY13453)
#define TDMAX_XL_MOTOR_EN    5   // HIGH: DRV2605 supply
#define TDMAX_XL_AMP_EN      6   // HIGH: NS4150B speaker amplifier
#define TDMAX_XL_TOUCH_RST   7   // LOW: touch controller held in reset (the Pro uses GPIO 38)
#define TDMAX_XL_PWRKEY      8   // A7682E POWERKEY
#define TDMAX_XL_KEY_RST     9   // LOW: keyboard controller held in reset
#define TDMAX_XL_AUDIO_SEL  10   // HIGH: modem audio out, LOW: ES8311 audio out

// Boot state, written to both output ports in one go before the lines are
// switched to outputs: LoRa on, 1.8 V on, internal antenna, touch and keyboard
// resets released; modem, motor and amplifier off; audio mux to the ES8311.
//
// GPS is deliberately OFF here. On the T-Deck Pro the GPS rail is GPIO 39 and
// the location provider drives it: off at boot, on when GPS is enabled, off
// again when it is disabled. The Max keeps that behaviour by routing the same
// calls to this expander line (see TDeckMaxGps.h).
#define TDMAX_XL_BOOT_PORT0 0x9A   // 1001 1010: T_RST=1 AMP=0 MOT=0 LSEL=1 1V8=1 GPS=0 LORA=1 6609=0
#define TDMAX_XL_BOOT_PORT1 0x02   // 0000 0010: ASEL=0 KRST=1 PKEY=0

// Gauge probe/refresh failed -- never let the UI divide by zero.
#define TDMAX_BATT_MILLIVOLTS_FALLBACK 3700

class TDeckMaxBoard : public ESP32Board {
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

  const char* getManufacturerName() const { return "LilyGo T-Deck Max"; }

  // True once the XL9555 answered on the bus and the boot state was applied.
  bool expanderReady() const { return expander_ready_; }

  // Drive one expander line. Returns false if the expander is unavailable or
  // the guard could not be taken; the line is then left as it was.
  bool xlWrite(uint8_t line, bool high);

  // Reset pulses, same shape as Meck's on this board: LOW 20 ms, HIGH, then
  // 50 ms for the controller to come out of reset.
  void touchReset();
  void keyboardReset();

  // GPS rail, called by the Max location provider around the core provider's
  // begin()/stop() so GPS on/off in the UI powers the module on and off.
  void gpsPowerOn();
  void gpsPowerOff();
  bool gpsPowerIsOn() const { return gps_on_; }

  ExtensionIOXL9555 io_expander;
  GaugeBQ27220 gauge;

private:
  bool expander_ready_ = false;
  bool gps_on_ = false;
  SemaphoreHandle_t expander_mutex_ = nullptr;
};
