// SPDX-License-Identifier: MIT
// Adapted from MeshCore's PortableLoRaChipProbe; license in PagerRadioProbe.h.
#if defined(TLORA_PAGER_UNIFIED)
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "PagerRadioProbe.h"

// Runs only after display.begin() has started the shared SPI bus. Transactions
// are deliberately slow identification reads; normal initialization resets
// the selected chip again and owns TCXO, RF switch, and all transmit settings.
class PagerProbeIO {
  SPIClass& spi_;
  bool waitReady() {
    const uint32_t start = millis();
    while (!ready()) {
      if (uint32_t(millis() - start) >= 250) return false;
      delay(1);
    }
    return true;
  }
  void start() {
    spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(P_LORA_NSS, LOW);
  }
  void finish() {
    digitalWrite(P_LORA_NSS, HIGH);
    spi_.endTransaction();
    delayMicroseconds(1);
  }
public:
  explicit PagerProbeIO(SPIClass& spi) : spi_(spi) {}
  uint32_t now() const { return millis(); }
  void pause(uint32_t ms) { delay(ms); }
  bool ready() const { return digitalRead(P_LORA_BUSY) == LOW; }
  void reset() {
    digitalWrite(P_LORA_NSS, HIGH);
    pinMode(P_LORA_NSS, OUTPUT);
    pinMode(P_LORA_BUSY, INPUT);
    digitalWrite(P_LORA_RESET, HIGH);
    pinMode(P_LORA_RESET, OUTPUT);
    digitalWrite(P_LORA_RESET, LOW);
    delay(1);
    digitalWrite(P_LORA_RESET, HIGH);
    delay(10);
  }
  bool readSX126Version(char* data, size_t length) {
    if (!data || length != 6 || !ready()) return false;
    start();
    spi_.transfer(RADIOLIB_SX126X_CMD_READ_REGISTER);
    spi_.transfer(RADIOLIB_SX126X_REG_VERSION_STRING >> 8);
    spi_.transfer(RADIOLIB_SX126X_REG_VERSION_STRING & 0xff);
    spi_.transfer(0);
    for (size_t i = 0; i < length; ++i) data[i] = spi_.transfer(0);
    finish();
    return waitReady();
  }
  bool readLR1121Version(uint8_t* data, size_t length) {
    if (!data || length != 4 || !ready()) return false;
    // GetVersion is a 16-bit command followed by a separate read transaction.
    start();
    spi_.transfer(0x01);
    spi_.transfer(0x01);
    finish();
    if (!waitReady()) return false;
    start();
    const uint8_t status = spi_.transfer(0);
    for (size_t i = 0; i < length; ++i) data[i] = spi_.transfer(0);
    finish();
    const uint8_t command_status = status & 0x0e;
    return status != 0xff && (command_status == 0x04 || command_status == 0x06) && waitReady();
  }
};

PagerRadioFamily pagerDetectRadio(SPIClass& spi) {
  PagerProbeIO io(spi);
  return probePagerRadio(io);
}
#endif
