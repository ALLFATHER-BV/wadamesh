#include <Arduino.h>
#include "target.h"
#include <helpers/HardwareRtcClock.h>

TDeckBoard board;

#if defined(P_LORA_SCLK)
  static SPIClass spi;
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

SPIClass* tdeckSharedSPI() {
#if defined(P_LORA_SCLK)
  return &spi;     // already begun (SCLK/MISO/MOSI) by radio.std_init
#else
  return nullptr;
#endif
}

ESP32RTCClock fallback_clock;
// The T-Deck ships with no battery-backed clock, but a DS3231 module on the I2C
// bus is a common self-fit (#378). The adapter is transparent when nothing is
// there: reads come from the software clock and writes stop at it, so a board
// without the module behaves exactly as before.
HardwareRtcClock     hw_rtc(fallback_clock);
ClockFloorRTC        rtc_clock(hw_rtc);
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  Wire.begin(18, 8);
  // A DS3231 fitted to this bus, if there is one. This is NOT the generic
  // address probe the comment below warns about: it is a declared chip type at
  // its own address with a positive identity proof, and a bus with nothing at
  // 0x68 simply logs and carries on. CAP_HARDWARE_RTC stays 0 for the board,
  // since the module is an aftermarket addition rather than something a T-Deck
  // has, which keeps the cold-boot network time sync on offer for everyone else.
  hw_rtc.begin(Wire, HardwareRtcClock::Chip::DS3231);
  rtc_clock.noteHardwareClock(hw_rtc.present());
  if (hw_rtc.adopted()) rtc_clock.noteHardwareTime();
  // NO generic rtc_clock.begin(Wire) here (issue #383). This board has no declared
  // battery-backed clock, and the call was doubly wrong: it ran BEFORE the bus
  // was even begun, so "the probe found nothing" was never evidence of anything.
  // A board either drives a documented chip through HardwareRtcClock (see the
  // Pager and the M9) or has none — "the generic probe happened to be quiet" is
  // not a capability test. CAP_HARDWARE_RTC is 0 here, which is what makes the
  // opt-in cold-boot network time sync worth offering on this board.

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}
