// SPDX-License-Identifier: GPL-3.0-or-later
// The T-LoRa Pager's charger (BQ25896) and fuel gauge (BQ27220) decisions, run on
// the production TLoraPagerBoard.cpp against fake chips (test/pager_hw_fakes/):
// the USB / charge-status decoding of REG0B, the temperature guard and battery care
// (both switch the charger through one place, with hysteresis), the charge limits
// rounded to the chip's steps, the gauge capacity written once per target, and the
// ship-mode power-off refused on USB.
//
// Build: g++ -std=c++17 -I test/pager_hw_fakes -I src test/test_pager_power.cpp
#include <cassert>
#include <cstdio>
#include "pager_hw_fakes/Arduino.h"
#include "pager_hw_fakes/Wire.h"
#define TLORA_PAGER 1
#define ESP32 1
#define P_LORA_NSS 36
#define P_LORA_RESET 47
#define P_LORA_DIO_1 14
#define PIN_USER_BTN 0
uint32_t testMillis = 1000;
TestSerial Serial;
TwoWire Wire;
static int s_haptics_begun = 0;
void pagerHapticsBegin() { ++s_haptics_begun; }
#include "../variants/lilygo_tlora_pager/TLoraPagerBoard.cpp"

static void advance(uint32_t ms) { testMillis += ms; }

// A fresh board, charger and gauge, as after a power-on. NVS survives unless wiped.
static void powerOn(TLoraPagerBoard& b, bool wipe_nvs) {
  if (wipe_nvs) testNvs().clear();
  s_charger = PowersBQ25896{};
  b = TLoraPagerBoard{};
  advance(5000);
}

static void testBootLimitsAndCapacity() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.gauge.design = 3000;                       // as the gauge leaves the factory
  b.begin();
  assert(s_charger.vreg_mv == 4288 && s_charger.ichg_ma == 704);   // LilyGoLib's figures, not the 2 A default
  assert(s_charger.chargeEnabled() && s_charger.enable_calls == 1); // a reboot never leaves charging off
  assert(b.gauge.capacity_writes == 1 && b.gauge.design == 1500);
  assert(testNvs()["pagerhw/cap_try"] == 1500);

  // Next boot: the gauge already reads the target, nothing is written.
  GaugeBQ27220 kept = b.gauge;
  powerOn(b, false);
  b.gauge = kept; b.gauge.capacity_writes = 0;
  b.begin();
  assert(b.gauge.capacity_writes == 0);

  // A sealed gauge that refuses: tried once, then left alone on later boots.
  powerOn(b, true);
  b.gauge.design = 3000; b.gauge.accept_capacity = false;
  b.begin();
  assert(b.gauge.capacity_writes == 1);
  for (int boot = 0; boot < 3; ++boot) {
    GaugeBQ27220 g = b.gauge; g.capacity_writes = 0;
    powerOn(b, false);
    b.gauge = g;
    b.begin();
    assert(b.gauge.capacity_writes == 0);
  }
  // The user picks another pack: a new target, so it is tried again.
  b.gauge.accept_capacity = true;
  assert(b.setBatteryCapacity(2000));
  assert(b.gauge.design == 2000 && b.batteryCapacitySetting() == 2000);
  assert(!b.setBatteryCapacity(100) && !b.setBatteryCapacity(30000));   // out of range: refused
  assert(b.batteryCapacitySetting() == 2000);

  // Charger not answering: no limits, ship mode refused, power state unknown.
  powerOn(b, true);
  s_charger.present = false;
  b.begin();
  assert(b.externalPowerPresent() == -1);
  assert(b.chargeState() == TLoraPagerBoard::ChargeState::Unknown);
  assert(!b.enterShipMode() && s_charger.shutdown_calls == 0);
}

static void testUsbDecoding() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.begin();
  struct { uint8_t reg0b; int ext; int chg; } cases[] = {
    { 0x00, 0, 0 },          // nothing in
    { 0x20 | 0x04, 1, 0 },   // USB host, power good
    { 0x40 | 0x04 | 0x10, 1, 2 },   // adapter, fast charging
    { 0x40 | 0x04 | 0x18, 1, 3 },   // adapter, charge done
    { 0x04, 1, 0 },          // power good alone counts
    { 0xE0, 0, 0 },          // OTG: the Pager is the source, not on USB
    { 0x20, 1, 0 },          // VBUS seen, PG not yet
  };
  for (auto& c : cases) {
    s_charger.reg[0x0B] = c.reg0b;
    advance(1000);                                // past the 1 s cache
    assert(b.externalPowerPresent() == c.ext);
    assert((int)b.chargeState() == c.chg);
  }
  // Cached for a second: a change inside that window is seen after it.
  s_charger.reg[0x0B] = 0x00; advance(1000); assert(b.externalPowerPresent() == 0);
  s_charger.reg[0x0B] = 0x24; advance(400);  assert(b.externalPowerPresent() == 0);
  advance(700); assert(b.externalPowerPresent() == 1);
}

// Feed one gauge reading through the 2 s refresh, as the battery callers do.
static void gaugeAt(TLoraPagerBoard& b, float temp_c, int soc) {
  b.gauge.temp_c = temp_c;
  b.gauge.soc = (uint16_t)soc;
  advance(2000);
  (void)b.getGaugeInfo();
}

static void testTemperatureGuard() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.begin();
  const int en0 = s_charger.enable_calls;
  gaugeAt(b, 44.9f, 50); assert(b.chargeTempBlock() == 0 && s_charger.chargeEnabled());
  gaugeAt(b, 45.2f, 50); assert(b.chargeTempBlock() == 1 && !s_charger.chargeEnabled());
  assert(s_charger.disable_calls == 1);
  gaugeAt(b, 46.0f, 50); assert(s_charger.disable_calls == 1);       // no repeat writes
  gaugeAt(b, 43.0f, 50); assert(b.chargeTempBlock() == 1);           // inside the 3 C band: still off
  gaugeAt(b, 42.1f, 50); assert(b.chargeTempBlock() == 1);
  gaugeAt(b, 42.0f, 50); assert(b.chargeTempBlock() == 0 && s_charger.chargeEnabled());
  assert(s_charger.enable_calls == en0 + 1);
  // Cold: below 0 off, back on from 3 C.
  gaugeAt(b, -0.5f, 50); assert(b.chargeTempBlock() == -1 && !s_charger.chargeEnabled());
  gaugeAt(b, 2.9f, 50);  assert(b.chargeTempBlock() == -1);
  gaugeAt(b, 3.0f, 50);  assert(b.chargeTempBlock() == 0 && s_charger.chargeEnabled());
  // Implausible readings change nothing, either way.
  gaugeAt(b, 50.0f, 50); assert(b.chargeTempBlock() == 1);
  gaugeAt(b, -273.1f, 50); assert(b.chargeTempBlock() == 1);         // a gauge read as 0 K
  gaugeAt(b, 120.0f, 50);  assert(b.chargeTempBlock() == 1);
  gaugeAt(b, 30.0f, 50);   assert(b.chargeTempBlock() == 0);
  // The user's limits: 50 C upper, 5 C lower; and the guard off.
  b.setChargeTempMax(500); b.setChargeTempMin(50);
  gaugeAt(b, 49.0f, 50); assert(b.chargeTempBlock() == 0);
  gaugeAt(b, 50.5f, 50); assert(b.chargeTempBlock() == 1);
  gaugeAt(b, 47.0f, 50); assert(b.chargeTempBlock() == 0);
  gaugeAt(b, 4.0f, 50);  assert(b.chargeTempBlock() == -1);
  b.setChargeTempMin(-100);                                          // never below freezing
  gaugeAt(b, 8.5f, 50); assert(b.chargeTempBlock() == 0);
  gaugeAt(b, -1.0f, 50); assert(b.chargeTempBlock() == -1);
  b.setChargeTempMax(0);                                             // guard off: always allowed
  assert(b.chargeTempBlock() == 0 && s_charger.chargeEnabled());
  gaugeAt(b, 60.0f, 50); assert(b.chargeTempBlock() == 0);
  // A gauge that stops answering marks the reading invalid, keeps the last SOC.
  b.gauge.answers = false; advance(2000);
  assert(!b.getGaugeInfo().valid && b.getBattStateOfCharge() == 50);
}

static void testBatteryCare() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.begin();
  b.setChargeCare(true);
  gaugeAt(b, 25.0f, 79); assert(!b.chargeCareHold() && s_charger.chargeEnabled());
  gaugeAt(b, 25.0f, 80); assert(b.chargeCareHold() && !s_charger.chargeEnabled());
  gaugeAt(b, 25.0f, 76); assert(b.chargeCareHold());
  gaugeAt(b, 25.0f, 75); assert(b.chargeCareHold());
  gaugeAt(b, 25.0f, 74); assert(!b.chargeCareHold() && s_charger.chargeEnabled());
  // Both holds: charging only comes back when neither holds it off.
  gaugeAt(b, 25.0f, 85); assert(!s_charger.chargeEnabled());
  gaugeAt(b, 46.0f, 85); assert(b.chargeTempBlock() == 1 && b.chargeCareHold());
  b.setChargeCare(false);
  assert(!b.chargeCareHold() && !s_charger.chargeEnabled());         // still too hot
  gaugeAt(b, 40.0f, 85); assert(s_charger.chargeEnabled());
  // Care on with no SOC yet: no decision.
  powerOn(b, true);
  b.begin();
  b.setChargeCare(true);
  gaugeAt(b, 25.0f, 255);                                            // out of range: SOC stays unknown
  assert(b.getBattStateOfCharge() == -1);
  assert(!b.chargeCareHold() && s_charger.chargeEnabled());
}

static void testChargeLimits() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.begin();
  b.setChargeLimits(1000, 4300);  assert(s_charger.ichg_ma == 960 && s_charger.vreg_mv == 4288);
  b.setChargeLimits(1536, 4352);  assert(s_charger.ichg_ma == 1536 && s_charger.vreg_mv == 4352);
  b.setChargeLimits(10, 3000);    assert(s_charger.ichg_ma == 64 && s_charger.vreg_mv == 3840);
  for (uint16_t ma : { 256, 512, 704, 1024, 1536 }) { b.setChargeLimits(ma, 4288); assert(s_charger.ichg_ma == ma); }
  for (uint16_t mv : { 4096, 4208, 4288, 4352 }) { b.setChargeLimits(704, mv); assert(s_charger.vreg_mv == mv); }
}

static void testShipMode() {
  TLoraPagerBoard b;
  powerOn(b, true);
  b.begin();
  // On USB: refused, the battery switch is not touched (the caller deep-sleeps).
  s_charger.reg[0x0B] = 0x44;
  assert(!b.enterShipMode());
  assert(s_charger.shutdown_calls == 0 && !(s_charger.reg[0x09] & 0x20));
  // The cache must not hide a plug-in that just happened: enterShipMode reads afresh.
  s_charger.reg[0x0B] = 0x00; advance(1000); assert(b.externalPowerPresent() == 0);
  s_charger.reg[0x0B] = 0x44;                          // plugged in 10 ms before the power-off
  advance(10);
  assert(!b.enterShipMode() && s_charger.shutdown_calls == 0);
  // On battery: BATFET_DLY cleared (open at once), BATFET_DIS set, then it waits for
  // the power to go and, still running, hands back to the deep-sleep fallback.
  s_charger.reg[0x0B] = 0x00;
  s_charger.reg[0x09] = 0x08;
  const uint32_t t0 = testMillis;
  assert(!b.enterShipMode());
  assert(s_charger.shutdown_calls == 1);
  assert((s_charger.reg[0x09] & 0x20) && !(s_charger.reg[0x09] & 0x08));
  assert(testMillis - t0 >= 2000);
}

int main() {
  testBootLimitsAndCapacity();
  testUsbDecoding();
  testTemperatureGuard();
  testBatteryCare();
  testChargeLimits();
  testShipMode();
  std::printf("test_pager_power: OK\n");
  return 0;
}
