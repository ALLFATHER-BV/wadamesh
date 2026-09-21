// SPDX-License-Identifier: GPL-3.0-or-later
#include "TDeckMaxBoard.h"

// Boot order matters on this board and mirrors Meck's proven sequence:
//   1. I2C up (XL9555, BQ27220, TCA8418, CST328, ES8311, SY6970 all share it)
//   2. XL9555 up and its boot state applied -- until this runs the LoRa module
//      is unpowered and the touch and keyboard controllers are unreset
//   3. Touch reset pulse, then keyboard reset pulse
//   4. Fuel gauge
// display.begin() runs after this and probes the touch controller, so the
// touch reset must already have happened here. The Pro's own resetTouch()
// pulse is a no-op on the Max (PIN_TOUCH_RST is -1 in this env).
void TDeckMaxBoard::begin() {
  ESP32Board::begin();

  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);
  pinMode(PIN_USER_BTN, INPUT_PULLUP);

  // Park every shared-SPI select/reset OUTPUT-HIGH before any bus traffic,
  // exactly as the Pro board does.
  const uint8_t selects[] = { PIN_TFT_CS, P_LORA_NSS, P_LORA_RESET, PIN_SD_CS };
  for (uint8_t pin : selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  if (!expander_mutex_) expander_mutex_ = xSemaphoreCreateMutex();
  expander_ready_ = io_expander.begin(Wire, TDMAX_XL9555_ADDR);
  // Not a nicety: LoRa power and both input-controller resets hang off this
  // chip. If it is missing the radio probe fails and touch/keyboard are dead.
  Serial.printf("[BOOT] T-Deck Max xl9555 %s\n", expander_ready_ ? "ok" : "PROBE FAILED");

  if (expander_ready_) {
    // Output latches first, then direction. The XL9555 powers up with every
    // line as an input and its output latches at 1, so writing direction
    // first would put the modem enable, motor and amplifier lines HIGH for
    // the few milliseconds until the levels were corrected.
    io_expander.writePort(ExtensionIOXL9555::PORT0, TDMAX_XL_BOOT_PORT0);
    io_expander.writePort(ExtensionIOXL9555::PORT1, TDMAX_XL_BOOT_PORT1);
    io_expander.configPort(ExtensionIOXL9555::PORT0, 0x00);
    io_expander.configPort(ExtensionIOXL9555::PORT1, 0x00);
    delay(10);   // LoRa rail settle before anything downstream probes the radio

    touchReset();
    keyboardReset();
  }
  gps_on_ = false;

  const bool gauge_ok = gauge.begin(Wire);
  Serial.printf("[BOOT] T-Deck Max bq27220 %s\n", gauge_ok ? "ok" : "PROBE FAILED");

  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    if (esp_sleep_get_ext1_wakeup_status() & (1ULL << P_LORA_DIO_1))
      startup_reason = BD_STARTUP_RX_PACKET;
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
    rtc_gpio_deinit((gpio_num_t)PIN_USER_BTN);
  }
}

bool TDeckMaxBoard::xlWrite(uint8_t line, bool high) {
  if (!expander_ready_ || !expander_mutex_) return false;
  if (xSemaphoreTake(expander_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  io_expander.digitalWrite(line, high ? HIGH : LOW);
  xSemaphoreGive(expander_mutex_);
  return true;
}

void TDeckMaxBoard::touchReset() {
  if (!expander_ready_) return;
  xlWrite(TDMAX_XL_TOUCH_RST, false);
  delay(20);
  xlWrite(TDMAX_XL_TOUCH_RST, true);
  delay(50);
}

void TDeckMaxBoard::keyboardReset() {
  if (!expander_ready_) return;
  xlWrite(TDMAX_XL_KEY_RST, false);
  delay(20);
  xlWrite(TDMAX_XL_KEY_RST, true);
  delay(50);
}

void TDeckMaxBoard::gpsPowerOn() {
  if (xlWrite(TDMAX_XL_GPS_EN, true)) {
    gps_on_ = true;
    delay(100);   // let the module's supply settle before the UART is nudged
  }
}

void TDeckMaxBoard::gpsPowerOff() {
  if (xlWrite(TDMAX_XL_GPS_EN, false)) gps_on_ = false;
}

uint16_t TDeckMaxBoard::getBattMilliVolts() {
  if (!gauge.refresh()) return TDMAX_BATT_MILLIVOLTS_FALLBACK;
  const uint16_t mv = gauge.getVoltage();
  return mv > 0 ? mv : TDMAX_BATT_MILLIVOLTS_FALLBACK;
}
