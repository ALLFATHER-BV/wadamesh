#include <Arduino.h>
#include <Wire.h>                     // BQ27220 fuel gauge (getBattMilliVolts)
#include "target.h"
#include <helpers/ArduinoHelpers.h>   // RadioNoiseListener / StdRNG

TDisplayP4Board board;
Xl9535 xl9535;                        // board-global expander (declared extern in Xl9535.h)

// SX1262 on the P4 SPI (SCLK=2/MOSI=3/MISO=4, CS=24, BUSY=6). RESET + DIO1 are on the XL9535
// (P_LORA_RESET/DIO_1 == -1 = RADIOLIB_NC): reset is pulsed via the expander in radio_init(); with
// DIO1 == NC RadioLib polls for RX/TX-done instead of taking a hardware IRQ.
// TODO(device): if polling RX is unreliable, route DIO1 through the XL9535 INT (GPIO5).
static SPIClass spi(FSPI);
RADIO_CLASS   radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);

DISPLAY_CLASS display;                 // RM69A10 MIPI-DSI

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  // Same approach as camillia-mt's working P4 port: the L76K is woken through the XL9535
  // (Xl9535::powerOnSequence drives GPS_WAKE high before the UART opens), UART1 listens on
  // GPIO22 / transmits on 23, and the link starts at 9600 and PROBES other rates until a
  // checksummed NMEA sentence arrives. The receiver's rate depends on what last configured
  // it (the chip's 9600 default, or 115200 persisted by LilyGo's factory firmware), and the
  // core provider listened at one fixed rate, so a mismatch meant an open, silent UART.
  // WadaNmeaLocationProvider saves the rate that works. No reset/enable GPIOs on this board.
  #include "../../src/helpers/WadaNmeaLocationProvider.h"
  WadaNmeaLocationProvider nmea(Serial1, &rtc_clock, -1 /*reset*/, -1 /*enable*/,
                                PIN_GPS_TX, PIN_GPS_RX, GPS_BAUD_RATE);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

void TDisplayP4Board::begin() {
  ESP32Board::begin();
}

// BQ27220 fuel gauge (I2C_1 @0x55, same shared Wire bus as the XL9535/RTC/touch — TwoWire's
// internal lock makes each transaction atomic across tasks). Standard command reads: 16-bit
// little-endian at Voltage()=0x08 (mV). Reads are rate-limited and sanity-windowed; on any I2C
// hiccup the last good value is kept so the battery UI never flickers to nonsense.
// Returns false when the gauge did not answer. This USED to return 0 on an I2C
// failure, which is indistinguishable from a real reading of zero — and the gauge
// shares this bus with the XL9535 expander, the RTC and the touch controller, so a
// lost transaction now and then is normal rather than exceptional. The voltage
// reader below always range-checked its result and kept the last good value, but
// the state-of-charge reader took 0 at face value and reported 0%, which is what
// made the battery flicker between 0 and the real figure. Report the failure
// instead of encoding it as data.
static bool bq27220ReadU16(uint8_t cmd, uint16_t& out) {
  Wire.beginTransmission(0x55);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x55, 2) != 2) return false;
  uint16_t lo = Wire.read(), hi = Wire.read();
  out = (uint16_t)(lo | (hi << 8));
  return true;
}

// StateOfCharge() = REG 0x2C, a percentage the gauge computes by coulomb counting
// against its learned pack profile. This is the number to trust, and the reason the
// battery readout was wrong (#273): we were reporting terminal VOLTAGE through a
// generic 3.3-4.2 V linear curve instead. Voltage is charger-driven, so plugging USB
// in pinned the display at 100% while the pack was nearly flat (one reporter read 71%
// here and 11% under Meck-P4, another watched it jump 51% -> 100% on plugging in), a
// resting-but-full pack never quite reached 100%, and "calibrate 100% here" captured a
// charging rail as full and made the error permanent.
//
// Returns -1 when the gauge does not answer or reports out of range, so the caller can
// fall back to the voltage curve rather than show a made-up number.
int TDisplayP4Board::getBattStateOfCharge() {
  static uint32_t last_ms = 0;
  static int      last_pct = -1;
  const uint32_t now = millis();
  if (last_ms == 0 || now - last_ms >= 5000) {
    last_ms = now;
    // Keep the previous good percentage when the gauge does not answer, exactly as
    // the voltage read does. Only a genuine in-range reply moves the number.
    uint16_t soc = 0;
    if (bq27220ReadU16(0x2C, soc) && soc <= 100) last_pct = (int)soc;
  }
  return last_pct;
}

uint16_t TDisplayP4Board::getBattMilliVolts() {
  static uint32_t last_ms = 0;
  static uint16_t last_mv = 3800;      // pre-first-read placeholder
  static bool     logged  = false;
  const uint32_t now = millis();
  if (last_ms == 0 || now - last_ms >= 5000) {
    last_ms = now;
    uint16_t mv = 0;
    const bool mv_ok = bq27220ReadU16(0x08, mv);      // Voltage(), mV
    if (mv_ok && mv >= 2500 && mv <= 4600) {
      last_mv = mv;
      if (!logged) { printf("[BATT] BQ27220 alive: %u mV\n", mv); logged = true; }
    } else if (!logged) {
      printf("[BATT] BQ27220 read invalid (%u) — placeholder in use\n", mv);
      logged = true;
    }
  }
  return last_mv;
}

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);            // PCF8563 RTC is on I2C_1 (shared Wire, SDA=7/SCL=8)
  // This board's chip IS documented, so declare it rather than leaving the
  // clock-source diagnostics to guess (#383). Unlike the Pager and the M9 this
  // one still goes through the core's probe, so a read that loses the chip's VL
  // bit is still only caught by ClockFloorRTC's plausibility bounds — which is
  // what already catches this board's known "came back asserting 2043".
  rtc_clock.noteHardwareClock(true);
  const uint32_t retained = rtc_clock.getCurrentTime();
  if (retained > ClockFloorRTC::MIN_VALID_EPOCH &&
      retained <= ClockFloorRTC::MAX_PLAUSIBLE_EPOCH) {
    rtc_clock.noteHardwareTime();
  }

  // SX1262 RESET is on the XL9535 — pulse it before RadioLib init.
  xl9535.sx1262Reset();
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI, P_LORA_NSS);
  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);   // fresh random identity from SX1262 RSSI noise
}

extern "C" bool tdisplay_p4_reset_c6() {
  if (!xl9535.ok()) return false;
  // Match the factory-era Meck/LilyGo Esp_At driver exactly: release first,
  // assert for 50 ms, then release and allow one second for ESP-AT to boot.
  xl9535.write(Xl9535::IO_C6_EN, true);
  delay(50);
  xl9535.write(Xl9535::IO_C6_EN, false);
  delay(50);
  xl9535.write(Xl9535::IO_C6_EN, true);
  delay(1000);
  return true;
}

extern "C" int hosted_reset_slave_callback() {
  return tdisplay_p4_reset_c6() ? 0 : -1;
}

// ---- Software restart = full system reset --------------------------------------------------------
// esp_restart() on the ESP32-P4 is SW_CPU_RESET: esp_system_reset_modules_on_exit()
// (esp_system/port/soc/esp32p4/system_internal.c) resets the DMA engines, UARTs, SDMMC and crypto,
// but not the MIPI-DSI host or its DPI path. The display driver then initialises over a host still
// half-configured from the session that restarted, and the panel stays dark. A power cycle or the
// reset button is a full chip reset and always recovers it -- which is exactly what users saw: a
// setting that reboots came up with the screen off, and the NEXT reboot by hand was fine.
// (Same defect, same fix as camillia-mt's T-Display P4 port.)
//
// So a software restart becomes that full reset: the RTC watchdog's system reset, which resets the
// CPU and all peripherals and spares only the RTC domain. Done as a shutdown handler so it covers
// every restart path (rebootDevice, OTA, console, factory reset) without touching any of them.
//
// esp_restart() runs shutdown handlers newest first and this one never returns, so it is installed
// before anything else registers one (Wi-Fi's among them) to run LAST, after the others have done
// their work. RTC memory survives the system reset, which is how the next boot knows its watchdog
// reset was a deliberate restart rather than a hang.
#include <esp_system.h>
#include <hal/wdt_hal.h>

static constexpr uint32_t kFullRestartMagic = 0x50345253u;   // "P4RS"
RTC_NOINIT_ATTR static uint32_t s_full_restart_marker;
static bool s_boot_was_full_restart = false;

static void tdisplayP4FullRestart() {
  s_full_restart_marker = kFullRestartMagic;
  wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
  wdt_hal_write_protect_disable(&rwdt);
  // Ticks of the slow clock: ~130 ms at 150 kHz, longer on a 32 kHz crystal. It only has to be short.
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, 20000, WDT_STAGE_ACTION_RESET_SYSTEM);
  wdt_hal_enable(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
  while (true) {
  }
}

void tdisplayP4InstallFullRestart() {
  static bool s_installed = false;
  if (s_installed) return;
  // Latch (and clear) the marker left by the previous session's restart before anything can read it.
  s_boot_was_full_restart = (s_full_restart_marker == kFullRestartMagic)
                            && esp_reset_reason() == ESP_RST_WDT;
  s_full_restart_marker = 0;
  s_installed = (esp_register_shutdown_handler(tdisplayP4FullRestart) == ESP_OK);
}

esp_reset_reason_t tdisplayP4ResetReason() {
  // Deliberate restarts arrive as RTC-watchdog resets on this board; report them as what they were,
  // so the "restarted after a crash" prompt and Settings -> About do not call every reboot a crash.
  return s_boot_was_full_restart ? ESP_RST_SW : esp_reset_reason();
}
