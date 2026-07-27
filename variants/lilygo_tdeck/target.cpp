#include <Arduino.h>
#include "target.h"
#include <helpers/esp32/TouchPrefsStore.h>

namespace {

// Observe exactly the bytes consumed by MicroNMEA without draining Serial1 in
// a second diagnostic reader. Keeping this at the provider boundary makes the
// counters trustworthy while leaving the GPS parser's behavior unchanged.
class TDeckGpsDiagnosticStream : public Stream {
 public:
  explicit TDeckGpsDiagnosticStream(HardwareSerial& serial) : _serial(serial) {}

  int available() override { return _serial.available(); }
  int peek() override { return _serial.peek(); }
  void flush() override { _serial.flush(); }
  int availableForWrite() override { return _serial.availableForWrite(); }
  size_t write(uint8_t value) override { return _serial.write(value); }
  size_t write(const uint8_t* buffer, size_t size) override {
    return _serial.write(buffer, size);
  }

  int read() override {
    const int value = _serial.read();
    if (value >= 0) observe(static_cast<uint8_t>(value));
    return value;
  }

  void report(bool gps_enabled, MicroNMEALocationProvider& provider) {
    const uint32_t now = millis();
    if (!_announced) {
      _announced = true;
      _last_report_ms = now;
      Serial.printf("[GPS-DIAG] configured enabled=%u baud=%lu esp_rx=%d esp_tx=%d interval=5s\n",
                    gps_enabled ? 1U : 0U,
                    static_cast<unsigned long>(touchPrefsGetGpsBaud(GPS_BAUD_RATE)),
                    PIN_GPS_TX, PIN_GPS_RX);
      return;
    }
    if (now - _last_report_ms < 5000U) return;

    const bool ever_received = _last_byte_ms != 0;
    if (ever_received) {
      Serial.printf("[GPS-DIAG] enabled=%u bytes_5s=%lu sentences=%lu checksum_ok=%lu checksum_bad=%lu "
                    "last_byte_ms=%lu fix=%u sats=%ld\n",
                    gps_enabled ? 1U : 0U,
                    static_cast<unsigned long>(_bytes_window),
                    static_cast<unsigned long>(_sentences_window),
                    static_cast<unsigned long>(_checksum_ok_window),
                    static_cast<unsigned long>(_checksum_bad_window),
                    static_cast<unsigned long>(now - _last_byte_ms),
                    provider.isValid() ? 1U : 0U,
                    provider.satellitesCount());
    } else {
      Serial.printf("[GPS-DIAG] enabled=%u bytes_5s=%lu sentences=%lu checksum_ok=%lu checksum_bad=%lu "
                    "last_byte_ms=never fix=%u sats=%ld\n",
                    gps_enabled ? 1U : 0U,
                    static_cast<unsigned long>(_bytes_window),
                    static_cast<unsigned long>(_sentences_window),
                    static_cast<unsigned long>(_checksum_ok_window),
                    static_cast<unsigned long>(_checksum_bad_window),
                    provider.isValid() ? 1U : 0U,
                    provider.satellitesCount());
    }

    _bytes_window = 0;
    _sentences_window = 0;
    _checksum_ok_window = 0;
    _checksum_bad_window = 0;
    _last_report_ms = now;
  }

 private:
  static int hexValue(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  void finishSentence() {
    ++_sentences_window;
    if (_checksum_digits == 2) {
      if (_checksum_received == _checksum_calculated) ++_checksum_ok_window;
      else ++_checksum_bad_window;
    } else {
      ++_checksum_bad_window;
    }
    _in_sentence = false;
  }

  void observe(uint8_t c) {
    ++_bytes_window;
    _last_byte_ms = millis();

    if (c == '$') {
      _in_sentence = true;
      _reading_checksum = false;
      _checksum_calculated = 0;
      _checksum_received = 0;
      _checksum_digits = 0;
      return;
    }
    if (!_in_sentence) return;
    if (c == '\r' || c == '\n') {
      finishSentence();
      return;
    }
    if (c == '*' && !_reading_checksum) {
      _reading_checksum = true;
      return;
    }
    if (_reading_checksum) {
      const int nibble = hexValue(c);
      if (nibble < 0 || _checksum_digits >= 2) {
        _checksum_digits = 0;
        return;
      }
      _checksum_received = static_cast<uint8_t>((_checksum_received << 4) | nibble);
      ++_checksum_digits;
      return;
    }
    _checksum_calculated ^= c;
  }

  HardwareSerial& _serial;
  uint32_t _bytes_window = 0;
  uint32_t _sentences_window = 0;
  uint32_t _checksum_ok_window = 0;
  uint32_t _checksum_bad_window = 0;
  uint32_t _last_byte_ms = 0;
  uint32_t _last_report_ms = 0;
  uint8_t _checksum_calculated = 0;
  uint8_t _checksum_received = 0;
  uint8_t _checksum_digits = 0;
  bool _in_sentence = false;
  bool _reading_checksum = false;
  bool _announced = false;
};

TDeckGpsDiagnosticStream gps_serial(Serial1);

}  // namespace

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
ClockFloorRTC        rtc_clock(fallback_clock);
MicroNMEALocationProvider gps(gps_serial, &rtc_clock);
EnvironmentSensorManager sensors(gps);

void tdeckGpsDiagLoop(bool gps_enabled) {
  gps_serial.report(gps_enabled, gps);
}

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  Wire.begin(18, 8);

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
