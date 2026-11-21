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
    _pmu.setVbusVoltageLimit(4000);
    _pmu.setVbusCurrentLimit(1000);
    _pmu.setSysPowerDownVoltage(2600);

    // Peripheral 3.3 V rails (display + touch + sensors).
    _pmu.setALDO1Voltage(3300);  _pmu.enableALDO1();
    _pmu.setALDO2Voltage(3300);  _pmu.enableALDO2();
    _pmu.setALDO3Voltage(3300);  _pmu.enableALDO3();
    _pmu.setALDO4Voltage(3300);  _pmu.enableALDO4();
    _pmu.setBLDO1Voltage(3300);  _pmu.enableBLDO1();
    _pmu.setBLDO2Voltage(3300);  _pmu.enableBLDO2();
    _pmu.setDC1Voltage(3300);    _pmu.enableDC1();
    _pmu.setDC2Voltage(3300);    _pmu.enableDC2();
    _pmu.setDC3Voltage(3300);    _pmu.enableDC3();

    // ESP32-S3 core supply (CPU SLDO domain).
    _pmu.setCPUSLDOVoltage(1100); _pmu.enableCPUSLDO();

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
