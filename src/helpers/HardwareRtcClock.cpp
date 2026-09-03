// SPDX-License-Identifier: GPL-3.0-or-later
#include "HardwareRtcClock.h"

#include "ClockFloorRTC.h"   // MIN_VALID_EPOCH / MAX_PLAUSIBLE_EPOCH — one set of sane bounds

// Both parts live at 0x51. That collision is the whole reason the core's
// address-only probe cannot tell them apart — see the header.
static constexpr uint8_t RTC_I2C_ADDR = 0x51;

static constexpr uint8_t REG_CTRL1    = 0x00;   // same address on both chips
static constexpr uint8_t CTRL1_STOP   = 0x20;   // bit 5, same meaning on both
static constexpr uint8_t REG_RAM_85063 = 0x03;  // PCF85063A scratch RAM; PCF8563 has minutes here

// First register of the 7-byte calendar block. This two-register shift is the
// entire register-map incompatibility between the two parts.
uint8_t HardwareRtcClock::timeBase() const {
  return (_chip == Chip::PCF85063A) ? 0x04 : 0x02;
}

const char* HardwareRtcClock::chipName() const {
  switch (_chip) {
    case Chip::PCF85063A: return "PCF85063A";
    case Chip::PCF8563:   return "PCF8563";
    default:              return "none";
  }
}

bool HardwareRtcClock::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  if (!_wire) return false;
  _wire->beginTransmission(RTC_I2C_ADDR);
  _wire->write(reg);
  // Repeated start, not stop-then-start: keeps the pointer write and the read
  // inside one driver-locked transaction on a bus the keyboard, codec, gauge
  // and expander all share.
  if (_wire->endTransmission(false) != 0) return false;
  if (_wire->requestFrom((int)RTC_I2C_ADDR, (int)len) != (int)len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!_wire->available()) return false;
    buf[i] = (uint8_t)_wire->read();
  }
  return true;
}

bool HardwareRtcClock::writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len) {
  if (!_wire) return false;
  _wire->beginTransmission(RTC_I2C_ADDR);
  _wire->write(reg);
  for (uint8_t i = 0; i < len; i++) _wire->write(buf[i]);
  return _wire->endTransmission() == 0;
}

// Positive chip-identity proof for the PCF85063A: register 0x03 is a byte of
// general-purpose RAM there, so bit 7 can be set and cleared and read back.
// On a PCF8563 that address is the MINUTES register, whose bit 7 is unused and
// reads back 0 — so the test fails cleanly rather than mis-identifying, and the
// original byte is restored either way (a same-value write to 8563 minutes is
// benign: it does not touch the seconds counter or the divider chain).
//
// This is SensorLib's own PCF85063 discriminator (SensorPCF85063::initImpl),
// reimplemented over plain Wire rather than pulled in via SensorPCF85063. Two
// reasons: this adapter also builds on the M9, where SensorLib is not a
// dependency; and SensorLib's high-level getDateTime() hides both the transfer
// status and the raw bytes, so the time read had to be hand-rolled regardless
// (issue #383 — validating what the chip actually returned is the entire point).
bool HardwareRtcClock::identify() {
  if (_chip != Chip::PCF85063A) {
    // PCF8563 boards: no write-probe at all. 0x03 is the live MINUTES register
    // there, and the part is already declared by the board; a PCF85063A
    // answering in its place lands month 0 / day > 31 in readHardware() and is
    // rejected there rather than silently believed.
    return true;
  }

  uint8_t original = 0;
  if (!readRegs(REG_RAM_85063, &original, 1)) return false;

  uint8_t probe = (uint8_t)(original | 0x80);
  bool ok = false;
  if (writeRegs(REG_RAM_85063, &probe, 1)) {
    uint8_t back = 0;
    if (readRegs(REG_RAM_85063, &back, 1) && (back & 0x80)) {
      probe = (uint8_t)(back & ~0x80);
      if (writeRegs(REG_RAM_85063, &probe, 1) &&
          readRegs(REG_RAM_85063, &back, 1) && (back & 0x80) == 0) {
        ok = true;
      }
    }
  }
  writeRegs(REG_RAM_85063, &original, 1);   // restore whatever was there
  return ok;
}

// A halted oscillator holds the calendar registers at their last value forever,
// which reads back as "the clock froze at the moment of the shutdown" — the
// exact M9 symptom. Neither RTClib's begin() nor the core's probe ever clears
// this bit, so do it here, on every boot, for both parts.
bool HardwareRtcClock::clearStopBit(bool& was_stopped) {
  was_stopped = false;
  uint8_t ctrl1 = 0;
  if (!readRegs(REG_CTRL1, &ctrl1, 1)) return false;
  was_stopped = (ctrl1 & CTRL1_STOP) != 0;
  if (!was_stopped) return true;
  const uint8_t running = (uint8_t)(ctrl1 & ~CTRL1_STOP);
  return writeRegs(REG_CTRL1, &running, 1);
}

bool HardwareRtcClock::readHardware(uint32_t& out_epoch, Status& why) {
  if (!_present) { why = Status::Absent; return false; }

  uint8_t r[7] = {0};
  if (!readRegs(timeBase(), r, 7)) { why = Status::Absent; return false; }

  // Every believability rule lives in the codec, against the SAME lower/upper
  // bounds the send-timestamp floor enforces — so a chip that decodes cleanly
  // but asserts 1970 or 2043 is refused here rather than latching the ratchet
  // later. See test/test_hw_rtc_codec.cpp for the cases.
  return hwRtcDecodeTime(r, ClockFloorRTC::MIN_VALID_EPOCH,
                         ClockFloorRTC::MAX_PLAUSIBLE_EPOCH, out_epoch, why);
}

bool HardwareRtcClock::writeHardware(uint32_t epoch) {
  uint8_t r[7];
  if (!hwRtcEncodeTime(epoch, r)) return false;
  return writeRegs(timeBase(), r, 7);
}

bool HardwareRtcClock::begin(TwoWire& wire, Chip chip) {
  _wire   = &wire;
  _chip   = chip;
  _present = false;
  _status = Status::NotProbed;
  if (chip == Chip::None) return false;

  wire.beginTransmission(RTC_I2C_ADDR);
  if (wire.endTransmission() != 0) {
    _status = Status::Absent;
    Serial.printf("[rtc] %s: no device at 0x%02X\n", chipName(), RTC_I2C_ADDR);
    return false;
  }
  if (!identify()) {
    _status = Status::WrongChip;
    Serial.printf("[rtc] 0x%02X answered but is not a %s — ignoring\n", RTC_I2C_ADDR, chipName());
    return false;
  }
  _present = true;
  bool was_stopped = false;
  if (!clearStopBit(was_stopped)) {
    _status = Status::Absent;
    Serial.printf("[rtc] %s control read/write failed\n", chipName());
    return false;
  }
  if (was_stopped) {
    // Resume the oscillator, but do not adopt the calendar it held while
    // stopped. Only an external sync can make that frozen value trustworthy.
    _status = Status::IntegrityLost;
    Serial.printf("[rtc] %s oscillator was stopped; time NOT adopted\n", chipName());
    return false;
  }

  uint32_t epoch = 0;
  Status why = Status::NotProbed;
  if (!readHardware(epoch, why)) {
    _status = why;
    // Not fatal, and deliberately silent about the value: the software clock
    // and the persisted floor stay exactly as they were.
    Serial.printf("[rtc] %s present, time NOT adopted (%s)\n", chipName(), statusName(why));
    return false;
  }

  // The one boot-time copy that makes powered-off retention real.
  _sw.setCurrentTime(epoch);
  _status = Status::Ok;
  Serial.printf("[rtc] %s retained time adopted: %lu\n", chipName(), (unsigned long)epoch);
  return true;
}

void HardwareRtcClock::setCurrentTime(uint32_t time) {
  _sw.setCurrentTime(time);            // system clock first: the UI reads that
  if (!_present) return;

  bool was_stopped = false;
  if (!clearStopBit(was_stopped)) {
    _write_ok = false;
    Serial.println("[rtc] oscillator start failed");
    return;
  }
  if (!writeHardware(time)) {
    _write_ok = false;
    Serial.println("[rtc] hardware write failed");
    return;
  }
  // Read back once. A chip that ACKs but does not retain (dead backup cell,
  // held in reset) must not be reported as a working retention path.
  uint32_t back = 0;
  Status why = Status::NotProbed;
  if (!readHardware(back, why)) {
    _write_ok = false;
    Serial.printf("[rtc] write read-back failed (%s)\n", statusName(why));
    return;
  }
  // One second of slack: the write and the read-back straddle the chip's own
  // seconds tick often enough that an exact compare would flap.
  const uint32_t skew = (back > time) ? (back - time) : (time - back);
  _write_ok = (skew <= 1);
  if (!_write_ok) {
    Serial.printf("[rtc] write read-back mismatch: wrote %lu, read %lu\n",
                  (unsigned long)time, (unsigned long)back);
  } else if (_status != Status::Ok) {
    // A chip that had lost integrity at boot is trustworthy again now that a
    // real time source has written it.
    _status = Status::Ok;
  }
}
