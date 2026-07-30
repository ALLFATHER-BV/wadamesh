// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// XL9535 — 16-bit I2C GPIO expander (TCA9535/PCA9535-class) on the LilyGo T-Display P4.
// It is the board's linchpin: it gates the power rails and the enable/reset lines for the
// ESP32-C6, the SD card, the AMOLED, the touch panel, the SX1262 (RESET + DIO1) and the
// RF switch. NOTHING comes up until begin() runs, so app_main must call it first.
//
// LilyGo names the pins IO0..IO7 (port 0) and IO10..IO17 (port 1); we keep that numbering.
// Register map (standard 9535): 0=in0 1=in1 2=out0 3=out1 6=cfg0 7=cfg1 (cfg bit: 1=input).
//
// Runs on the Arduino `Wire` bus already used for the board I2C (SDA=7/SCL=8), so the RTC,
// gauge and touch share it. Pins/roles from LilyGo t_display_p4_config.h (see TDISPLAY_P4_PORT.md).

#include <stdint.h>

class Xl9535 {
public:
  // LilyGo IO indices (0-7 = port0 bit0-7, 10-17 = port1 bit0-7).
  enum Io : uint8_t {
    // port 0
    IO_3V3_EN        = 0,   // 3.3V rail enable
    IO_RF_SWITCH     = 1,   // SKY13453 VCTL (LoRa TX/RX path)
    IO_SCREEN_RST    = 2,
    IO_TOUCH_RST     = 3,
    IO_TOUCH_INT     = 4,   // input
    IO_ETHERNET_RST  = 5,
    IO_5V_EN         = 6,
    IO_EXT_SENSOR_INT= 7,   // input
    // port 1
    IO_VCCA_EN       = 10,
    IO_GPS_WAKE      = 11,
    IO_RTC_INT       = 12,  // input
    IO_C6_WAKE       = 13,
    IO_C6_EN         = 14,
    IO_SD_EN         = 15,
    IO_SX1262_RST    = 16,
    IO_SX1262_DIO1   = 17,  // input (the LoRa IRQ)
  };

  // addr defaults to 0x20 (A0-A2 low). sda/scl default to the board I2C_1 pins.
  bool begin(uint8_t sda = 7, uint8_t scl = 8, uint8_t addr = 0x20);
  bool ok() const { return _ok; }

  // Per-pin control (io = an Io value). dir: true=input.
  void setDir(uint8_t io, bool input);
  void write(uint8_t io, bool high);
  bool read(uint8_t io);

  // Board power-on sequence: enable rails, bring up the C6 + SD, release the screen/touch
  // resets, park the SX1262 RESET high. Conservative delays — TUNE against LilyGo on-device.
  void powerOnSequence();

  // Convenience for the SX1262 glue (RESET + DIO1 live here).
  void sx1262Reset();            // active-low pulse on IO_SX1262_RST
  bool sx1262Dio1() { return read(IO_SX1262_DIO1); }
  // IO1 drives the board's SKY13453. We ASSUME it is a TX/RX path switch and flip it around every
  // transmit, but that was never validated on hardware, and this expander has NO separate
  // internal/external antenna line — while other P4 firmware DOES offer an internal/external
  // choice. If IO1 is really the ANTENNA select, toggling it per TX means transmitting on one
  // antenna and receiving on the other, which is exactly what a field report of -10 dB outbound
  // against +12 dB inbound looks like on a unit with an external antenna fitted (and symmetric on
  // that same hardware under other firmware). Until it is confirmed on a device, let the user PIN
  // the line instead of toggling it:
  //   0 = auto  -> legacy behaviour, toggle per TX (default; nothing changes)
  //   1 = LOW   -> hold low  for both TX and RX
  //   2 = HIGH  -> hold high for both TX and RX
  // Whichever pinned setting gives symmetric SNR in both directions is the right antenna — and
  // that answer also tells us what IO1 actually is.
  void setAntennaMode(uint8_t mode) {
    _ant_mode = (mode > 2) ? 0 : mode;
    if (_ant_mode) write(IO_RF_SWITCH, _ant_mode == 2);   // pinned: set once, never toggled again
    else           write(IO_RF_SWITCH, false);            // auto: back to the idle state
  }
  uint8_t antennaMode() const { return _ant_mode; }
  void rfSwitchTx(bool tx) {
    if (_ant_mode) return;      // pinned — the path must NOT differ between TX and RX
    write(IO_RF_SWITCH, tx);    // auto (legacy): assumes a TX/RX switch; polarity unverified
  }

private:
  uint8_t _addr = 0x20;
  bool _ok = false;
  uint8_t _ant_mode = 0;   // 0 = auto/legacy per-TX toggle, 1 = pinned LOW, 2 = pinned HIGH
  uint8_t _out[2]  = {0xFF, 0xFF};   // shadow of output regs (port0, port1)
  uint8_t _cfg[2]  = {0xFF, 0xFF};   // shadow of config  regs (1=input)
  void writeReg(uint8_t reg, uint8_t val);
  uint8_t readReg(uint8_t reg);
  static void split(uint8_t io, uint8_t& port, uint8_t& bit) { port = io < 8 ? 0 : 1; bit = io < 8 ? io : io - 10; }
};

extern Xl9535 xl9535;   // the board-global instance (defined in target.cpp)
