// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "TWatchS3Board.h"

// AXP2101 I2C slave address on the sensor bus (Wire SDA=10/SCL=11).
#ifndef AXP2101_ADDR
  #define AXP2101_ADDR 0x34
#endif

void TWatchS3Board::begin() {
  ESP32Board::begin();

  // --- AXP2101 PMU ----------------------------------------------------------
  // The display (ST7789), touch controller (FT6x36) and GPS/IMU/audio rails are
  // all switched by the AXP2101. Bring it up and enable the rails they need
  // BEFORE any peripheral probe. Rail/voltage choices follow LilyGo's T-Watch S3
  // example; the exact set needs on-hardware confirmation.
  _pmu_ok = _pmu.init(Wire, PIN_BOARD_SDA, PIN_BOARD_SCL, AXP2101_ADDR);
  if (_pmu_ok) {
    // Crown (PWR) key presses are reported as PEK IRQs; they are masked by
    // default, so isPekeyShortPressIrq()/isPekeyLongPressIrq() stay false until
    // we enable them here.
    _pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    // These two take the AXP2101 register codes, not millivolts / milliamps: a plain
    // 1000 truncated to code 0 = a 100 mA USB input limit (slow charge, brown-outs
    // on TX when the cell is flat).
    _pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    _pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_900MA);
    _pmu.setSysPowerDownVoltage(2600);

    // Rails as LilyGo's own T-Watch S3 library sets them. DC1 is the ESP32-S3's
    // 3.3 V: left alone.
    _pmu.setALDO1Voltage(3300);  _pmu.enableALDO1();   // RTC (PCF8563) VBAT
    _pmu.setALDO2Voltage(3300);  _pmu.enableALDO2();   // TFT backlight VDD
    _pmu.setALDO3Voltage(3300);  _pmu.enableALDO3();   // screen + touch VDD
    _pmu.setALDO4Voltage(3300);  _pmu.enableALDO4();   // SX1262 VDD
    _pmu.setBLDO2Voltage(3300);  _pmu.enableBLDO2();   // DRV2605 haptics enable
#if ENV_INCLUDE_GPS
    _pmu.setDC3Voltage(3300);    _pmu.enableDC3();     // GNSS (T-Watch S3 Plus)
#else
    _pmu.disableDC3();
#endif
    // Not connected on this board: off, they only cost battery.
    _pmu.disableDC2(); _pmu.disableDC4(); _pmu.disableDC5();
    _pmu.disableBLDO1(); _pmu.disableCPUSLDO(); _pmu.disableDLDO1(); _pmu.disableDLDO2();

    // RTC coin-cell charge + battery presence.
    _pmu.setButtonBatteryChargeVoltage(3300);
    _pmu.enableButtonBatteryCharge();
    _pmu.enableBattDetection();
    _pmu.enableVbusVoltageMeasure();
    _pmu.enableBattVoltageMeasure();

    Serial.printf("[BOOT] AXP2101 ok, batt=%d mV charging=%d\n",
                  _pmu.getBattVoltage(), _pmu.isCharging() ? 1 : 0);
  } else {
    Serial.println("[BOOT] AXP2101 PROBE FAILED — display/touch rails may be off");
  }

  // --- Button / LoRa pins / deep-sleep wake reason --------------------------
#if PIN_USER_BTN >= 0
  pinMode(PIN_USER_BTN, INPUT);
#endif

  pinMode(P_LORA_MISO, INPUT_PULLUP);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET;
    }
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}

bool TWatchS3Board::pollPowerKeyShortPress() {
  if (!_pmu_ok) return false;
  _pmu.getIrqStatus();       // refresh the cached status from hardware (else isPekey* reads stale 0)
  if (_pmu.isPekeyShortPressIrq()) {
    _pmu.clearIrqStatus();   // latched: clear so the next press is a fresh edge
    return true;
  }
  return false;
}
