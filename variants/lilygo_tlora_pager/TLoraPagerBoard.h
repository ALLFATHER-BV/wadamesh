// SPDX-License-Identifier: GPL-3.0-or-later
//
// Board class for the LilyGo T-LoRa Pager (ESP32-S3). Unlike the T-Deck/
// Heltec V4, power-rail control and battery reporting go through I2C chips —
// an XL9555 GPIO expander gates LoRa/GPS/keyboard/SD power, a BQ27220
// fuel gauge replaces the usual VBAT ADC divider (this board doesn't have
// one), and a BQ25896 charger owns USB input and the battery switch.
// begin() brings up just enough of that I2C chain for the rest of the
// app to run.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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

#define PAGER_XL9555_ADDR  0x20
#define PAGER_BQ25896_ADDR 0x6B   // charger / power path (power key = its QON input)

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
  enum class SdCardState : uint8_t { Absent, Present, Unknown };

  // BQ25896 REG0B CHRG_STAT, plus Unknown when the charger does not answer.
  enum class ChargeState : uint8_t { NotCharging = 0, PreCharge = 1, FastCharge = 2, Done = 3, Unknown = 0xFF };

  void begin();
  SdCardState sdCardState();
  bool sdCardPresent();
  bool setSdCardPower(bool on);
  bool resetSdCardPower();

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

  // Battery voltage from the BQ27220. Shares one gauge refresh with the state of
  // charge below (see refreshGauge()).
  uint16_t getBattMilliVolts() {
    refreshGauge();
    return batt_mv_;
  }

  // The gauge's own StateOfCharge(), in percent; -1 when it has not answered with
  // an in-range value yet. The gauge coulomb-counts against its learned pack
  // profile, so this is the real charge level: terminal voltage is charger-driven
  // and reads near full the moment USB goes in, whatever the pack holds (the
  // T-Display P4, same gauge, had exactly that bug: #273).
  int getBattStateOfCharge() {
    refreshGauge();
    return batt_soc_;
  }

  // Everything else the BQ27220 measures, from the same 2 s refresh. It counts the
  // charge in and out of the pack, so these are measurements, not estimates from the
  // voltage. valid is false until the gauge has answered once.
  struct GaugeInfo {
    bool     valid;
    int16_t  current_ma;      // + charging, - drawing from the pack
    int16_t  power_mw;        // average power, same sign
    uint16_t remaining_mah;
    uint16_t full_mah;        // what the gauge has learned the pack holds now
    uint16_t to_empty_min;    // 0xFFFF = not discharging
    uint16_t to_full_min;     // 0xFFFF = not charging
    int16_t  temp_c10;        // pack temperature, 0.1 °C
    uint16_t cycles;
    uint16_t health_pct;      // StateOfHealth, %
  };
  GaugeInfo getGaugeInfo() {
    refreshGauge();
    return gauge_info_;
  }

  // The fitted pack's capacity (Settings > Battery; LilyGo's pack is 1500 mAh).
  // setBatteryCapacity() stores it and writes it to the gauge as its design and
  // full-charge capacity (config-update mode, about a second); true when the
  // gauge reads it back. batteryCapacitySetting() is the stored choice.
  bool     setBatteryCapacity(uint16_t mah);
  uint16_t batteryCapacitySetting();
  uint16_t batteryDesignCapacity() const { return gauge.getDesignCapacity(); }   // as last read, 0 = never

  const char* getManufacturerName() const{
    return "LilyGo T-LoRa Pager";
  }

  // Mute/unmute the NS4150B amp via its XL9555 enable pin: the runtime toggle
  // the sound code brackets each chime/WAV with, so the amp is only live while
  // something is actually playing. begin() drives it HIGH with the other rails
  // and then leaves boot with it off.
  void setAmpEnabled(bool on);

  // GPS module power (XL9555 GPS_EN + GPS_RST), switched by PagerGps.
  bool setGpsPower(bool on);
  bool gpsPowerIsOn() const { return gps_on_; }

  // ---- BQ25896 charger -------------------------------------------------------
  // External power (USB) present: 1 yes, 0 no, -1 charger not answering. Cached
  // for a second; the status bar asks several times a second.
  int  externalPowerPresent();
  ChargeState chargeState();

  // Charging held off for the pack's temperature (gauge reading): 1 too hot,
  // -1 too cold, 0 charging allowed. See chargeThermalCheck().
  int chargeTempBlock() const { return charge_temp_block_; }

  // Battery care (Settings > Battery): stop charging at 80 %, resume below 75 %.
  // chargeCareHold() is true while it is holding the charger off.
  void setChargeCare(bool on);
  bool chargeCareHold() const { return care_hold_; }

  // The limits from Settings > Battery > Charging: fast-charge current (mA, 64 mA
  // steps) and charge voltage (mV, 16 mV steps), and the temperature guard's upper
  // limit in 0.1 deg C (0 = guard off). begin() starts at the safe defaults; the
  // UI applies the stored choice once it has loaded the prefs.
  void setChargeLimits(uint16_t ma, uint16_t mv);
  void setChargeTempMax(int16_t c10);
  void setChargeTempMin(int16_t c10);   // lower limit, 0.1 deg C (default 0)

  // Real power-off: opens the charger's battery switch (BQ25896 ship mode,
  // BATFET_DIS), which takes the whole board down to the charger's own leakage,
  // about 26 µA against about 530 µA in deep sleep. Only the power key (the
  // charger's QON input) or plugging in USB switches it back on. Refused (false)
  // while USB is connected: VBUS would keep the system powered anyway, and the
  // caller then falls back to deep sleep. On success it does not return.
  bool enterShipMode();

  ExtensionIOXL9555 io_expander;
  GaugeBQ27220 gauge;

private:
  void refreshGauge();
  void chargeThermalCheck(int16_t temp_c10);
  void chargeCareCheck(int soc);
  void chargeEnableApply();

  bool expander_ready_ = false;
  bool charger_ready_ = false;
  bool gps_on_ = true;     // GPS_EN as last driven (on from the boot rail bring-up)
  uint16_t batt_mv_ = PAGER_BATT_MILLIVOLTS_FALLBACK;   // the last gauge voltage
  int      batt_soc_ = -1;  // the last in-range gauge state of charge, -1 = none yet
  GaugeInfo gauge_info_ = {};
  uint32_t batt_ms_ = 0;    // when the gauge was last read, 0 = never
  int8_t   ext_pwr_ = -1;   // externalPowerPresent() cache
  uint8_t  sys_stat_ = 0;   // REG0B as last read
  uint32_t chg_ms_ = 0;     // when the charger was last read, 0 = never
  int8_t   charge_temp_block_ = 0;   // chargeTempBlock()
  int16_t  chg_tmax_c10_ = 450;      // setChargeTempMax(); 0 = guard off
  int16_t  chg_tmin_c10_ = 0;        // setChargeTempMin()
  bool     care_on_ = false;         // setChargeCare()
  bool     care_hold_ = false;       // chargeCareHold()
  bool     chg_enabled_ = true;      // what chargeEnableApply() last told the charger
  SemaphoreHandle_t expander_mutex_ = nullptr;
};
