// SPDX-License-Identifier: GPL-3.0-or-later
#include "TDeckProBoard.h"

namespace {
constexpr uint8_t BQ25896_ADDR = 0x6B;
constexpr uint8_t BQ_ADC_CTRL = 0x02;
constexpr uint8_t BQ_WATCHDOG = 0x03;
constexpr uint8_t BQ_BATTERY_VOLTAGE = 0x0E;
constexpr uint16_t BQ_VBAT_BASE_MV = 2304;
constexpr uint16_t BQ_VBAT_STEP_MV = 20;
}

void TDeckProBoard::begin() {
  ESP32Board::begin();

  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);
  pinMode(PIN_USER_BTN, INPUT_PULLUP);

  const uint8_t selects[] = { PIN_TFT_CS, P_LORA_NSS, P_LORA_RESET, PIN_SD_CS };
  for (uint8_t pin : selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);
  delay(10);

  Wire.beginTransmission(BQ25896_ADDR);
  _bq_present = Wire.endTransmission() == 0;
  Serial.printf("[BOOT] T-Deck Pro BQ25896 %s\n", _bq_present ? "ok" : "not found");

  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    if (esp_sleep_get_ext1_wakeup_status() & (1ULL << P_LORA_DIO_1))
      startup_reason = BD_STARTUP_RX_PACKET;
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
    rtc_gpio_deinit((gpio_num_t)PIN_USER_BTN);
  }
}

bool TDeckProBoard::bqRead(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(BQ25896_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)BQ25896_ADDR, 1) != 1) return false;
  value = (uint8_t)Wire.read();
  return true;
}

bool TDeckProBoard::bqWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(BQ25896_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

uint16_t TDeckProBoard::getBattMilliVolts() {
  const uint32_t now = millis();
  if (!_bq_present) {
    if (_bq_retry_ms && (int32_t)(now - _bq_retry_ms) < 0) return _batt_mv;
    Wire.beginTransmission(BQ25896_ADDR);
    _bq_present = Wire.endTransmission() == 0;
    _bq_retry_ms = _bq_present ? 0 : now + 15000;
    if (!_bq_present) return _batt_mv;
  }

  uint8_t watchdog = 0;
  if (bqRead(BQ_WATCHDOG, watchdog)) bqWrite(BQ_WATCHDOG, watchdog | 0x40);

  if (!_bq_pending) {
    uint8_t control = 0;
    if (!bqRead(BQ_ADC_CTRL, control) || !bqWrite(BQ_ADC_CTRL, (control & ~0x40) | 0x80)) {
      _bq_present = false;
      _bq_retry_ms = now + 15000;
      return _batt_mv;
    }
    _bq_pending = true;
    return _batt_mv;
  }

  uint8_t control = 0;
  if (!bqRead(BQ_ADC_CTRL, control)) {
    _bq_present = _bq_pending = false;
    _bq_retry_ms = now + 15000;
    return _batt_mv;
  }
  if (control & 0x80) return _batt_mv;
  _bq_pending = false;

  uint8_t raw = 0;
  if (bqRead(BQ_BATTERY_VOLTAGE, raw) && (raw & 0x7F))
    _batt_mv = BQ_VBAT_BASE_MV + (uint16_t)(raw & 0x7F) * BQ_VBAT_STEP_MV;
  return _batt_mv;
}