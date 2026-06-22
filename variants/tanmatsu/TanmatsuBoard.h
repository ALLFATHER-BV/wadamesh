#pragma once

#include <Arduino.h>
#include "helpers/ESP32Board.h"
extern "C" {
#include "bsp/power.h"   // badge-bsp battery/charger (no raw ADC pin on the Tanmatsu)
}

// Minimal board for the Tanmatsu. ESP32Board (-> mesh::MainBoard) gives the common
// ESP32/Arduino behaviour (millis, deep-sleep scaffolding, etc.). Battery/power and
// any board init come from the badge-bsp components, not raw ADC pins like the S3
// boards — left as TODO(device) until the hardware is in hand.
class TanmatsuBoard : public ESP32Board {
public:
  void begin() {
    ESP32Board::begin();
    // TODO(device): bsp_device_initialize() (display/input/power/led) is normally
    // done once in the variant's startup; decide whether it lives here or in main.
  }

  uint16_t getBattMilliVolts() override {
    // Battery comes from the CH32 power coprocessor via badge-bsp, not an ADC pin. The UI turns
    // this mV into a % (and "?" when it's 0), so returning a real reading fixes the "?".
    uint16_t mv = 0;
    if (bsp_power_get_battery_voltage(&mv) != ESP_OK) return 0;
    return mv;
  }

  const char* getManufacturerName() const override {
    return "Tanmatsu";
  }
};
