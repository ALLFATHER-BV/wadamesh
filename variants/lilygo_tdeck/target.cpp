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
// Wadamesh's provider for its UART probe: T-Deck Plus receivers talk 38400 or 9600
// depending on the unit, so the speed is found while the user has not picked one in
// Settings (CAP_GPS_BAUD_FIND). No enable or reset line on this board; the ESP
// receives on PIN_GPS_TX (the core's naming).
WadaNmeaLocationProvider gps(Serial1, &rtc_clock, -1 /*reset*/, -1 /*enable*/,
                             PIN_GPS_TX, PIN_GPS_RX, GPS_BAUD_RATE, true /*keep_choice*/);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

#define PIN_TOUCH_SDA 18
#define PIN_TOUCH_SCL 8

bool radio_init() {
  fallback_clock.begin();

  pinMode(PIN_TOUCH_SDA, INPUT);
  pinMode(PIN_TOUCH_SCL, INPUT);
  delayMicroseconds(50);
  int lvl_sda = digitalRead(PIN_TOUCH_SDA);
  int lvl_scl = digitalRead(PIN_TOUCH_SCL);

  // I2C bus recovery: if a peripheral is mid-byte and holding SDA low, clock SCL
  // up to 9 times so it can finish, then leave the lines idle-high.
  pinMode(PIN_TOUCH_SCL, OUTPUT);
  digitalWrite(PIN_TOUCH_SCL, HIGH);
  pinMode(PIN_TOUCH_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9 && digitalRead(PIN_TOUCH_SDA) == LOW; ++i) {
    digitalWrite(PIN_TOUCH_SCL, LOW);  delayMicroseconds(6);
    digitalWrite(PIN_TOUCH_SCL, HIGH); delayMicroseconds(6);
  }

  // Force a clean re-route onto 18/8. ESP32 Arduino's Wire was already begun on
  // DEFAULT pins in ESP32Board::begin(); a second begin() with new pins does not
  // always re-assign them, so end() first guarantees the pins actually move.
  Wire.end();
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
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
