// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Board-declared battery-backed RTC adapter (issue #383).
//
// WHY THIS EXISTS INSTEAD OF AutoDiscoverRTCClock
// ----------------------------------------------
// The core's AutoDiscoverRTCClock probes I2C 0x51 with a bare address ACK and
// then talks to whatever answered as if it were a PCF8563. Two things go wrong
// with that on the boards this firmware ships on:
//
//   * The T-LoRa Pager's chip at 0x51 is a PCF85063A, whose register map is
//     shifted by two (time starts at 0x04, not 0x02) — an 8563 driver reads its
//     OFFSET/RAM registers as seconds/minutes and returns garbage. That is why
//     the Pager deliberately skipped begin() and got NO retention at all, while
//     Ripple (which drives the real chip) keeps time through a power-down.
//
//   * Neither chip's "my time is untrustworthy" bit is consulted. The PCF8563's
//     VL bit and the PCF85063A's OS bit are both bit 7 of the seconds register,
//     both latch when VDD dips below the low-voltage threshold, and RTClib's
//     now() masks that bit off and hands back the stale register contents
//     anyway. On the ThinkNode M9 that reads back as "the wall clock froze at
//     the moment of the hard shutdown" — which is exactly what M9 users report.
//     (It is also the shape of the T-Display P4's "came back asserting 2043".)
//
// So: no address-only auto-detection. A board names the chip it actually has
// when it calls begin(Wire, Chip::...) from its own radio_init(), this adapter
// drives that chip's real register map, and every read is validated before it is
// allowed anywhere near system or protocol time. The PCF85063A additionally gets
// a positive identity proof (its RAM register is writable; the PCF8563's is the
// live minutes counter), so a mis-declared board fails closed instead of
// returning plausible nonsense.
//
// SHAPE
// -----
// The adapter is ClockFloorRTC's *fallback*, i.e. it stands where ESP32RTCClock
// used to, and it owns an ESP32RTCClock of its own:
//
//   * begin()            probe, integrity-check, decode, and — only if all three
//                        pass — copy hardware time into the ESP32 system clock,
//                        once, at boot. That is the retention fix.
//   * getCurrentTime()   reads the ESP32 clock, NEVER I2C. Timestamps are read on
//                        every message, every UI tick and every advert; the chip
//                        is here for retention across power-off, not as a hot
//                        path on a bus shared with the keyboard, codec, gauge and
//                        expander.
//   * setCurrentTime()   write-through: ESP32 clock AND chip, then one read-back
//                        to confirm the chip actually took it.
//
// Every boot-read failure (no ACK, wrong chip, stopped oscillator, integrity
// loss, bad BCD, implausible date) leaves the software clock / persisted floor
// unchanged. A later external sync still updates the software clock even if its
// hardware write cannot be verified; an invalid hardware value never becomes
// system time.

#include <Arduino.h>
#include <Mesh.h>
#include <Wire.h>

#include "HwRtcCodec.h"   // the Arduino-free decode/encode half (host-testable)

class HardwareRtcClock : public mesh::RTCClock {
public:
  // DS3231 is an aftermarket module people fit themselves (#378, T-Deck). It
  // lives at a different I2C address from the two soldered parts, so it can
  // never be confused with them; everything else about it differs too, see the
  // normalisation in the .cpp.
  enum class Chip : uint8_t { None, PCF8563, PCF85063A, DS3231 };

  // Why a boot-time read was refused. Surfaced by the `clock` CLI and the About
  // page so "the clock is wrong" is diagnosable without a serial console.
  // Defined in HwRtcCodec.h, next to the checks that produce it.
  using Status = HwRtcStatus;

  explicit HardwareRtcClock(mesh::RTCClock& software) : _sw(software) {}

  // Probe + adopt. `wire` must already be begun by the board (it is: every
  // board that has a chip brings its I2C bus up in board.begin(), before
  // radio_init()). Returns true only when hardware time was adopted.
  bool begin(TwoWire& wire, Chip chip);

  // Hot path: the ESP32 system clock, no I2C. See the shape note above.
  uint32_t getCurrentTime() override { return _sw.getCurrentTime(); }

  // Write-through from any accepted source (NTP, GPS, companion, CLI, mesh
  // bootstrap). ClockFloorRTC has already range-checked the value.
  void setCurrentTime(uint32_t time) override;

  void tick() override { _sw.tick(); }

  bool    present()   const { return _present; }         // a real, identified chip answered
  Status  status()    const { return _status; }          // why the boot read was/was not adopted
  bool    adopted()   const { return _status == Status::Ok; }
  // True once a set() has been written to the chip and read back this session.
  bool    writeConfirmed() const { return _write_ok; }
  const char* chipName() const;
  static const char* statusName(Status s) { return hwRtcStatusName(s); }

  // Raw, validated read of the chip's calendar. Public so diagnostics can ask
  // the chip directly without disturbing the system clock. Returns false (and
  // sets `why`) on any transfer, BCD, calendar or plausibility failure.
  bool readHardware(uint32_t& out_epoch, Status& why);

private:
  bool     writeHardware(uint32_t epoch);
  uint8_t  timeBase() const;   // first time register: 0x02 (8563) / 0x04 (85063) / 0x00 (DS3231)
  uint8_t  i2cAddr()  const;   // 0x51 for both PCF parts, 0x68 for the DS3231
  bool     readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
  bool     writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len);
  bool     identify();
  bool     ds3231Normalise(uint8_t r[7]);
  bool     clearStopBit(bool& was_stopped);

  mesh::RTCClock& _sw;
  TwoWire* _wire   = nullptr;
  Chip     _chip   = Chip::None;
  Status   _status = Status::NotProbed;
  bool     _present  = false;
  bool     _write_ok = false;
};
