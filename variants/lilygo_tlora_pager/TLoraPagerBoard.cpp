// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "TLoraPagerBoard.h"
#include <Preferences.h>
#include "PagerHaptics.h"
#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>   // PowersBQ25896: the charger

// One board, one charger; kept out of the header so nothing else that includes
// TLoraPagerBoard.h (PagerGps, the UI) needs XPowersLib on its include path.
static PowersBQ25896 s_charger;

// The pack LilyGo fits (3.7 V / 1500 mAh). The BQ27220 leaves the factory set up
// for 3000 mAh, and its state of charge is only as good as that figure.
#define PAGER_GAUGE_DESIGN_MAH 1500

// The capacity the gauge is set to: the fitted pack (1500 mAh) unless the user
// set another in Settings > Battery (NVS "pagerhw/cap_mah"). The gauge sits on
// the battery, so the setting outlives reflashes and power-off; boot only writes
// it when the gauge reads something else. Each target is tried once ("cap_try"),
// so a gauge that refuses the write (sealed with a custom key) or reads back
// something else is not put through config-update mode on every boot.
static uint16_t pagerCapTarget(Preferences& p) {
  const uint16_t v = p.getUShort("cap_mah", PAGER_GAUGE_DESIGN_MAH);
  return (v >= 300 && v <= 20000) ? v : PAGER_GAUGE_DESIGN_MAH;
}
static void pagerConfigureGauge(GaugeBQ27220& gauge) {
  if (!gauge.refresh()) return;                 // not answering: try next boot
  Preferences p;
  if (!p.begin("pagerhw", false)) return;
  const uint16_t want = pagerCapTarget(p);
  if (gauge.getDesignCapacity() != want && p.getUShort("cap_try", 0) != want) {
    const bool ok = gauge.setNewCapacity(want, want);
    Serial.printf("[BOOT] bq27220 capacity -> %u mAh %s\n", (unsigned)want, ok ? "ok" : "FAILED");
    p.putUShort("cap_try", want);
  }
  p.end();
}

void TLoraPagerBoard::begin() {

  ESP32Board::begin();

  // Power-off holds the radio's select HIGH through deep sleep (a falling edge would
  // wake the radio). Let go of it before anything drives the pin.
  gpio_hold_dis((gpio_num_t)P_LORA_NSS);
  gpio_deep_sleep_hold_dis();

  // XL9555/BQ27220 sit on the shared I2C bus (SDA 3 / SCL 2). ESP32Board::begin()
  // only calls the pin-less Wire.begin() unless PIN_BOARD_SDA/SCL are defined
  // for this env, so re-init explicitly with this board's pins before probing
  // either chip.
  Wire.begin(SDA, SCL);

  // Configure user button
  pinMode(PIN_USER_BTN, INPUT);

  // Park every shared-SPI select/reset OUTPUT-HIGH before ANY bus traffic
  // (display.begin() runs right after board.begin() in main.cpp, and the
  // radio's NSS/RESET stay unclaimed until radio.begin() well after that).
  // A floating select can leave that chip half-listening on the live bus;
  // LilyGoLib parks this same set before its display init (initShareSPIPins()).
  const uint8_t spi_selects[] = { P_LORA_NSS, P_LORA_RESET, PAGER_PIN_SD_CS, PAGER_PIN_NFC_CS };
  for (uint8_t pin : spi_selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  if (!expander_mutex_) expander_mutex_ = xSemaphoreCreateMutex();
  const bool expander_ok = io_expander.begin(Wire, PAGER_XL9555_ADDR);
  expander_ready_ = expander_ok;
  // Not just a nicety: every power rail AND the display's hardware reset hang
  // off this chip — if the probe fails silently, the symptom downstream is a
  // black screen with an otherwise clean boot log.
  Serial.printf("[BOOT] xl9555 %s\n", expander_ok ? "ok" : "PROBE FAILED");
  if (expander_ok) {
    // Drive EVERY channel the vendor drives (LilyGoLib LilyGo_LoRa_Pager.cpp
    // begin()), not just the rails this app uses: DRV2605 (haptics) and the
    // ES8311/amp are on the shared I2C bus and the ST25R3916 (NFC) is on the
    // shared SPI bus — an UNPOWERED chip's pads can clamp a shared bus through
    // its ESD diodes, so "off because unused" isn't safe here. DISP_RST first,
    // same as the vendor list, so the panel's reset line is driven (not
    // floating) as early as possible.
    const uint8_t rails[] = {
      PAGER_EXPAND_DISP_RST,
      PAGER_EXPAND_KB_RST,
      PAGER_EXPAND_LORA_EN,
      PAGER_EXPAND_GPS_EN,
      PAGER_EXPAND_DRV_EN,
      PAGER_EXPAND_AMP_EN,
      PAGER_EXPAND_NFC_EN,
      PAGER_EXPAND_GPS_RST,
      PAGER_EXPAND_KB_EN,
      PAGER_EXPAND_GPIO_EN,
      PAGER_EXPAND_SD_EN,
    };
    for (uint8_t ch : rails) {
      io_expander.pinMode(ch, OUTPUT);
      io_expander.digitalWrite(ch, HIGH);
      delay(1); // stagger rail turn-on, mirrors the vendor's bring-up order
    }

    // A software power-off leaves the XL9555 alive in deep sleep. If a card is
    // removed, used by a host, and reinserted while asleep, SD_EN can therefore
    // stay HIGH across the whole cycle and the card never sees a clean power-on
    // reset. Pulse only its dedicated rail before the first mount attempt.
    resetSdCardPower();

    io_expander.pinMode(PAGER_EXPAND_SD_DET, INPUT);
    io_expander.pinMode(PAGER_EXPAND_SD_PULLEN, INPUT);

    // Hardware-reset the ST7796 panel (XL9555 ch6 — see TLoraPagerBoard.h for
    // why this line was invisible to the first bring-up pass). Same pulse shape
    // as the vendor's: LOW 50ms, back HIGH. display.begin()'s own software
    // reset + its mandated 120ms wait then run against a freshly-reset panel.
    io_expander.digitalWrite(PAGER_EXPAND_DISP_RST, LOW);
    delay(50);
    io_expander.digitalWrite(PAGER_EXPAND_DISP_RST, HIGH);

    delay(50); // let rails + panel settle before anything downstream probes them

    // The speaker amplifier is the sound code's to switch: on for each chime, off
    // after it (setAmpEnabled). Off is its idle state, so leave boot in it rather
    // than live until the first sound, a few mA for nothing. Only the amp: the
    // codec and the other chips keep their rails (see the list above).
    io_expander.digitalWrite(PAGER_EXPAND_AMP_EN, LOW);
  }

  // ES8311 audio codec presence check (address only -- no register writes
  // yet, that's the sound code's job on first use). Every power rail this
  // chip depends on is driven above; a missing ack here means the sound
  // path won't work and is worth knowing from the boot log alone.
  Wire.beginTransmission(0x18);
  Serial.printf("[BOOT] es8311 %s\n", Wire.endTransmission() == 0 ? "ok" : "PROBE FAILED");

  // DRV2605 vibration motor driver: its rail (DRV_EN) is up from the list above.
  // Configured once here and left in standby until a pattern plays.
  if (expander_ok) pagerHapticsBegin();

  gauge.begin(Wire);
  pagerConfigureGauge(gauge);

  // BQ25896 charger. XPowersLib's init checks the chip id and switches the I2C
  // watchdog off, so what is set here stays set (with the watchdog on, every
  // expiry put the chip back to its defaults). The chip default charge current is
  // 2 A, too much for the 1500 mAh pack: use LilyGoLib's figures for this board,
  // 704 mA (under C/2) up to 4288 mV.
  charger_ready_ = s_charger.init(Wire, SDA, SCL, PAGER_BQ25896_ADDR);
  if (charger_ready_) {
    s_charger.setChargeTargetVoltage(4288);
    s_charger.setChargerConstantCurr(704);
    // CHG_CONFIG survives an ESP reset: a reboot while the temperature guard
    // had charging off must not leave it off. The guard re-checks within 2 s.
    s_charger.enableCharge();
    const int stat = s_charger.readRegister(0x0B);
    if (stat >= 0) sys_stat_ = (uint8_t)stat;
  }
  Serial.printf("[BOOT] bq25896 %s\n", charger_ready_ ? "ok" : "PROBE FAILED");

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET; // received a LoRa packet (while in deep sleep)
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}

TLoraPagerBoard::SdCardState TLoraPagerBoard::sdCardState() {
  // If the expander probe failed, don't turn an unreadable detect input into a
  // permanent "empty slot" result. The non-formatting SD.begin() probe is the
  // safe fallback arbiter (and will simply fail if the SD rail is unavailable).
  if (!expander_ready_ || !expander_mutex_) return SdCardState::Unknown;
  if (xSemaphoreTake(expander_mutex_, pdMS_TO_TICKS(50)) != pdTRUE)
    return SdCardState::Unknown;
  // LilyGo's pager implementation treats the XL9555 SD_DET input as
  // active-low: HIGH is an empty slot, LOW is a seated card. Read the whole
  // port so SensorLib's -1 I2C error remains distinguishable; digitalRead()
  // narrows that error to 0xFF and would falsely report a live card removed.
  const int port = io_expander.readPort(ExtensionIOXL9555::PORT1);
  xSemaphoreGive(expander_mutex_);
  if (port < 0) return SdCardState::Unknown;
  return (port & (1 << (PAGER_EXPAND_SD_DET - 8))) == 0
      ? SdCardState::Present : SdCardState::Absent;
}

bool TLoraPagerBoard::sdCardPresent() {
  // Unknown is deliberately fail-present for normal operation: never tear down
  // a live VFS because card-detect I2C was temporarily unavailable. A real,
  // non-formatting SD transaction remains the final mount/alive arbiter.
  return sdCardState() != SdCardState::Absent;
}

bool TLoraPagerBoard::setSdCardPower(bool on) {
  if (!expander_ready_ || !expander_mutex_) return false;
  if (xSemaphoreTake(expander_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  io_expander.pinMode(PAGER_EXPAND_SD_EN, OUTPUT);
  io_expander.digitalWrite(PAGER_EXPAND_SD_EN, on ? HIGH : LOW);
  xSemaphoreGive(expander_mutex_);
  return true;
}

bool TLoraPagerBoard::resetSdCardPower() {
  if (!setSdCardPower(false)) return false;
  delay(100);
  if (!setSdCardPower(true)) return false;
  delay(250);
  return true;
}

// The u-blox module's rail (XL9555 GPS_EN). The GPS is a UART-only device, not on
// the shared I2C/SPI buses the rail list above protects, so it can go dark: with
// GPS off it was otherwise searching for satellites around the clock. Its reset
// line goes LOW with the rail (a HIGH pin would feed the unpowered module) and is
// released a moment after power returns, a clean power-on reset. The location
// provider (PagerGps) switches it.
bool TLoraPagerBoard::setGpsPower(bool on) {
  if (!expander_ready_) return false;
  if (expander_mutex_ && xSemaphoreTake(expander_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (on) {
    io_expander.digitalWrite(PAGER_EXPAND_GPS_EN, HIGH);
    delay(5);
    io_expander.digitalWrite(PAGER_EXPAND_GPS_RST, HIGH);
  } else {
    io_expander.digitalWrite(PAGER_EXPAND_GPS_RST, LOW);
    io_expander.digitalWrite(PAGER_EXPAND_GPS_EN, LOW);
  }
  if (expander_mutex_) xSemaphoreGive(expander_mutex_);
  gps_on_ = on;
  return true;
}

void TLoraPagerBoard::setAmpEnabled(bool on) {
  if (!expander_ready_) return;
  if (!expander_mutex_) {
    io_expander.digitalWrite(PAGER_EXPAND_AMP_EN, on ? HIGH : LOW);
    return;
  }
  // Audio must never pin the SD lifecycle gate forever if the I2C expander is
  // unhealthy. A missed amp toggle is preferable to wedging the notify task.
  if (xSemaphoreTake(expander_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return;
  io_expander.digitalWrite(PAGER_EXPAND_AMP_EN, on ? HIGH : LOW);
  xSemaphoreGive(expander_mutex_);
}

// One gauge refresh (the BQ27220's whole register block, about 6 ms over 100 kHz
// I2C) serves every battery caller for 2 s: the status bar alone asks 4 times a
// second, and a pack moves slowly. Short enough that plugging the charger in
// still shows at once (the UI publishes a charge flip immediately).
void TLoraPagerBoard::refreshGauge() {
  const uint32_t now = millis();
  if (batt_ms_ && now - batt_ms_ < 2000) return;
  batt_ms_ = now ? now : 1;
  if (!gauge.refresh()) {                        // keep the last good voltage/SOC, but do
    gauge_info_.valid = false;                   // not show stale currents as live
    return;
  }
  const uint16_t mv = gauge.getVoltage();
  if (mv >= 2500 && mv <= 4600) batt_mv_ = mv;
  const uint16_t soc = gauge.getStateOfCharge();
  if (soc <= 100) batt_soc_ = (int)soc;
  GaugeInfo g;
  g.valid         = true;
  g.current_ma    = gauge.getCurrent();
  g.power_mw      = gauge.getAveragePower();
  g.remaining_mah = gauge.getRemainingCapacity();
  g.full_mah      = gauge.getFullChargeCapacity();
  g.to_empty_min  = gauge.getTimeToEmpty();
  g.to_full_min   = gauge.getTimeToFull();
  g.temp_c10      = (int16_t)(gauge.getTemperature() * 10.0f);
  g.cycles        = gauge.getCycleCount();
  g.health_pct    = gauge.getStateOfHealth();
  gauge_info_ = g;
  chargeThermalCheck(g.temp_c10);
  chargeCareCheck(batt_soc_);
}

// Charging only between 0 and 45 °C pack temperature (the usual Li-ion window;
// LilyGo's sheet allows up to 60 °C, which ages a cell fast). The BQ25896 has
// its own NTC input, but nothing in this firmware relies on it being wired, so
// the gauge's reading decides: charging switches off outside the window and
// back on 3 °C inside it, so it does not flap at the edge. The power path is
// untouched: on USB the board keeps running from VBUS either way.
// Both limits can be changed in Settings > Battery > Charge limits: the upper
// one (or the guard switched off) with a warning, the lower one only upwards
// from 0 deg C, which is the safe direction.
#define PAGER_CHG_TEMP_HYST_C10   30
void TLoraPagerBoard::chargeThermalCheck(int16_t t) {
  if (!charger_ready_) return;
  int8_t want = charge_temp_block_;
  if (chg_tmax_c10_ <= 0) {
    want = 0;                                    // guard off: charge whatever the temperature
  } else {
    if (t < -400 || t > 1000) return;            // not a plausible reading: change nothing
    if (charge_temp_block_ == 0) {
      if (t > chg_tmax_c10_)               want = 1;
      else if (t < chg_tmin_c10_) want = -1;
    } else if (t <= chg_tmax_c10_ - PAGER_CHG_TEMP_HYST_C10 &&
               t >= chg_tmin_c10_ + PAGER_CHG_TEMP_HYST_C10) {
      want = 0;
    }
  }
  if (want == charge_temp_block_) return;
  charge_temp_block_ = want;
  Serial.printf("[POWER] temperature guard: %s (pack %d.%d C)\n",
                want > 0 ? "too hot" : want < 0 ? "too cold" : "ok",
                t / 10, (t < 0 ? -t : t) % 10);
  chargeEnableApply();
}

// Battery care: with it on, charging stops once the gauge reports 80 % and only
// starts again below 75 %, so the pack does not sit at 100 % on the charger all
// day (the main thing that ages a Li-ion cell besides heat). Like the
// temperature guard it only gates charging; the board still runs from USB.
#define PAGER_CARE_STOP_PCT    80
#define PAGER_CARE_RESUME_PCT  75
void TLoraPagerBoard::chargeCareCheck(int soc) {
  bool want = care_hold_;
  if (!care_on_)                         want = false;
  else if (soc < 0 || soc > 100)         return;      // no reading yet: change nothing
  else if (!care_hold_ && soc >= PAGER_CARE_STOP_PCT)  want = true;
  else if (care_hold_ && soc < PAGER_CARE_RESUME_PCT)  want = false;
  if (want == care_hold_) return;
  care_hold_ = want;
  Serial.printf("[POWER] battery care: %s at %d %%\n", want ? "holding" : "charging", soc);
  chargeEnableApply();
}

// The one place that switches the charger: on only when neither the temperature
// guard nor battery care holds it off.
void TLoraPagerBoard::chargeEnableApply() {
  if (!charger_ready_) return;
  const bool on = charge_temp_block_ == 0 && !care_hold_;
  if (on == chg_enabled_) return;
  chg_enabled_ = on;
  if (on) s_charger.enableCharge();
  else    s_charger.disableCharge();
  chg_ms_ = 0;                                   // re-read the charge status next time
  Serial.printf("[POWER] charging %s\n", on ? "on" : "off");
}

void TLoraPagerBoard::setChargeCare(bool on) {
  care_on_ = on;
  chargeCareCheck(batt_soc_);
}

bool TLoraPagerBoard::setBatteryCapacity(uint16_t mah) {
  if (mah < 300 || mah > 20000) return false;
  Preferences p;
  if (!p.begin("pagerhw", false)) return false;
  p.putUShort("cap_mah", mah);
  bool ok = false;
  if (gauge.refresh()) {
    if (gauge.getDesignCapacity() == mah) {
      ok = true;
    } else {
      gauge.setNewCapacity(mah, mah);           // design and full charge: the gauge relearns from there
      p.putUShort("cap_try", mah);
      ok = gauge.refresh() && gauge.getDesignCapacity() == mah;
    }
  }
  p.end();
  batt_ms_ = 0;                                  // the next battery call reads the gauge afresh
  Serial.printf("[POWER] battery capacity %u mAh %s\n", (unsigned)mah, ok ? "ok" : "NOT TAKEN");
  return ok;
}

uint16_t TLoraPagerBoard::batteryCapacitySetting() {
  Preferences p;
  if (!p.begin("pagerhw", true)) return PAGER_GAUGE_DESIGN_MAH;
  const uint16_t v = pagerCapTarget(p);
  p.end();
  return v;
}

void TLoraPagerBoard::setChargeLimits(uint16_t ma, uint16_t mv) {
  if (!charger_ready_) return;
  ma = (uint16_t)(ma / 64 * 64);                 // the BQ25896's steps: 64 mA, 16 mV
  mv = (uint16_t)(mv / 16 * 16);
  if (ma < 64) ma = 64;
  if (mv < 3840) mv = 3840;
  s_charger.setChargerConstantCurr(ma);
  s_charger.setChargeTargetVoltage(mv);
  Serial.printf("[POWER] charge limits %u mA, %u mV\n", (unsigned)ma, (unsigned)mv);
}

void TLoraPagerBoard::setChargeTempMin(int16_t c10) {
  chg_tmin_c10_ = c10 > 0 ? c10 : 0;             // never below freezing
  chargeThermalCheck(gauge_info_.valid ? gauge_info_.temp_c10 : (int16_t)250);
}

void TLoraPagerBoard::setChargeTempMax(int16_t c10) {
  chg_tmax_c10_ = c10 > 0 ? c10 : 0;
  chargeThermalCheck(gauge_info_.valid ? gauge_info_.temp_c10 : (int16_t)250);   // re-decide now
}

// REG0B (system status): VBUS_STAT[7:5] (000 no input, 001 USB host, 010
// adapter, 111 OTG), CHRG_STAT[4:3], PG_STAT[2] (input power good).
int TLoraPagerBoard::externalPowerPresent() {
  if (!charger_ready_) return -1;
  const uint32_t now = millis();
  if (!chg_ms_ || now - chg_ms_ >= 1000) {
    chg_ms_ = now ? now : 1;
    const int r = s_charger.readRegister(0x0B);
    if (r >= 0) {
      const uint8_t stat = (uint8_t)r;
      sys_stat_ = stat;
      const uint8_t vbus = (stat >> 5) & 0x07;
      const bool pg = (stat & 0x04) != 0;
      ext_pwr_ = (pg || (vbus != 0 && vbus != 7)) ? 1 : 0;   // OTG = we are the source
    }
  }
  return ext_pwr_;
}

TLoraPagerBoard::ChargeState TLoraPagerBoard::chargeState() {
  if (externalPowerPresent() < 0) return ChargeState::Unknown;
  return (ChargeState)((sys_stat_ >> 3) & 0x03);
}

// REG09: BATFET_DIS is bit 5, BATFET_DLY bit 3 (0 = open at once). The switch
// stays open until the power key (QON) is pressed or VBUS returns, so this
// needs no wake source of our own.
bool TLoraPagerBoard::enterShipMode() {
  if (!charger_ready_) return false;
  chg_ms_ = 0;                                   // read the input state fresh
  if (externalPowerPresent() != 0) return false; // on USB (or unknown): would not switch off
  Serial.println("[POWER] ship mode");
  Serial.flush();
  s_charger.setTurnOffDelay(0);                  // BATFET_DLY off: open at once
  s_charger.shutdown();                          // BATFET_DIS
  delay(2000);                                   // power goes away here
  return false;                                  // still running: the caller falls back
}
