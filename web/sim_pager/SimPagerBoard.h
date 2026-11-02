// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdlib.h>
// T-LoRa Pager simulator: the Pager board the UI's TLORA_PAGER branches talk to, on
// top of the web board. A pack at 76 %, no USB, no card, the radio of the web board.
#include "../platform/WebBoard.h"
#include "../../src/helpers/HardwareRtcClock.h"

#define PAGER_PIN_SD_CS 21
#ifndef PIN_I2S_MCLK
  #define PIN_I2S_MCLK 10
  #define PIN_I2S_BCK  11
  #define PIN_I2S_WS   18
  #define PIN_I2S_DOUT 45
  #define PIN_I2S_SDIN 17
#endif

class SPIClass;

class TLoraPagerBoard : public WebBoard {
public:
  enum class SdCardState : uint8_t { Absent, Present, Unknown };
  enum class ChargeState : uint8_t { NotCharging = 0, PreCharge = 1, FastCharge = 2, Done = 3, Unknown = 0xFF };
  uint16_t getBattMilliVolts() override { return 3950; }
  const char* getManufacturerName() const override { return "LilyGo T-LoRa Pager (sim)"; }
  // WADA_SIM_SOC=<0..100>: the gauge's state of charge (default 76), e.g. to
  // try the low-battery warnings; WADA_SIM_SOC_DROP=1 lets it fall 1 % per 5 s.
  int  getBattStateOfCharge() {
    static const char* e = getenv("WADA_SIM_SOC");
    static const bool drop = getenv("WADA_SIM_SOC_DROP") != nullptr;
    int s = e ? atoi(e) : 76;
    if (drop) s -= (int)(millis() / 5000);
    return s < 0 ? 0 : s > 100 ? 100 : s;
  }
  struct GaugeInfo {
    bool valid; int16_t current_ma, power_mw; uint16_t remaining_mah, full_mah, to_empty_min, to_full_min;
    int16_t temp_c10; uint16_t cycles, health_pct;
  };
  // WADA_SIM_CHARGING=1 in the environment: on USB, charging at ~480 mA.
  static bool simCharging() { static const bool c = getenv("WADA_SIM_CHARGING") != nullptr; return c; }
  GaugeInfo getGaugeInfo() {   // a pack drawing ~42 mA with the screen on
    if (simCareHold())   // on USB, held at 80 %: the board runs from VBUS, the pack rests
      return { true, -2, -8, 1190, 1488, 0xFFFF, 0xFFFF, 262, 12, 99 };
    if (simCharging())
      return { true, (int16_t)(476 + (int)(millis() / 1000 % 9)), 1980, 1131, 1488, 0xFFFF, 47, 263, 12, 99 };
    return { true, (int16_t)(-38 - (int)(millis() / 1000 % 9)), -152, 1131, 1488, 1615, 0xFFFF, 241, 12, 99 };
  }
  uint16_t sim_cap_ = 1500;
  bool     setBatteryCapacity(uint16_t mah) { sim_cap_ = mah; return true; }
  uint16_t batteryCapacitySetting() { return sim_cap_; }
  uint16_t batteryDesignCapacity() const { return sim_cap_; }
  int  externalPowerPresent() { return (simCharging() || simCareHold()) ? 1 : 0; }
  static bool simCareHold() { static const bool h = getenv("WADA_SIM_CARE_HOLD") != nullptr; return h; }
  ChargeState chargeState() { return ChargeState::NotCharging; }
  // WADA_SIM_CHARGE_HOT=1: the temperature guard has charging paused (too hot).
  void setChargeLimits(uint16_t, uint16_t) {}
  void setChargeTempMax(int16_t) {}
  void setChargeTempMin(int16_t) {}
  int chargeTempBlock() const { static const bool h = getenv("WADA_SIM_CHARGE_HOT") != nullptr; return h ? 1 : 0; }
  // WADA_SIM_CARE_HOLD=1: battery care is holding the charger off at 80 %.
  void setChargeCare(bool) {}
  bool chargeCareHold() const { return simCareHold(); }
  bool enterShipMode() { return false; }
  SdCardState sdCardState() { return SdCardState::Absent; }
  bool sdCardPresent() { return false; }
  bool setSdCardPower(bool) { return true; }
  bool resetSdCardPower() { return true; }
  void setAmpEnabled(bool) {}
  bool setGpsPower(bool) { return true; }
  bool gpsPowerIsOn() const { return true; }
};

extern HardwareRtcClock hw_rtc;
SPIClass* tloraPagerSharedSPI();
void tloraPagerGpsPowerDown();
