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
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
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
static uint16_t bq27220ReadU16(uint8_t cmd) {
  Wire.beginTransmission(0x55);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(0x55, 2) != 2) return 0;
  uint16_t lo = Wire.read(), hi = Wire.read();
  return (uint16_t)(lo | (hi << 8));
}

uint16_t TDisplayP4Board::getBattMilliVolts() {
  static uint32_t last_ms = 0;
  static uint16_t last_mv = 3800;      // pre-first-read placeholder
  static bool     logged  = false;
  const uint32_t now = millis();
  if (last_ms == 0 || now - last_ms >= 5000) {
    last_ms = now;
    const uint16_t mv = bq27220ReadU16(0x08);          // Voltage(), mV
    if (mv >= 2500 && mv <= 4600) {
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

  // SX1262 RESET is on the XL9535 — pulse it before RadioLib init.
  xl9535.sx1262Reset();
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI, P_LORA_NSS);
  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);   // fresh random identity from SX1262 RSSI noise
}
