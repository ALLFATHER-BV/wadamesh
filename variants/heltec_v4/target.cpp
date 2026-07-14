#include <Arduino.h>
#include "target.h"

// Set phase 1 when target.cpp globals start (no Serial - can crash before setup). Read g_boot_phase in setup().
namespace {
struct BootTrace {
  BootTrace() { set_boot_phase(1); }
} _boot_trace;
}

HeltecV4Board board;

#if defined(P_LORA_SCLK)
  #if defined(HELTEC_LORA_V4_R8)
    static SPIClass spi(HSPI);   // R8: LoRa on HSPI; the TFT + micro-SD own FSPI (SPI2)
  #else
    static SPIClass spi;
  #endif
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC        rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  #if defined(HELTEC_LORA_V4_R8)
    DISPLAY_CLASS display(&board.periph_power);   // LGFXDisplay claims Vext(40) on turnOn
  #else
    DISPLAY_CLASS display(NULL);
  #endif
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(HELTEC_LORA_V4_R8)
  // V4-R8 + Expansion Kit V2: bring up the shared TFT/SD bus on FSPI (SPI2) so
  // LovyanGFX (bus_shared=true) finds an already-begun bus and skips a double
  // spi_bus_initialize, and so the micro-SD mount (main.cpp, CS=3) can share it.
  // MISO=45 is needed for SD reads even though the ST7789 panel is write-only.
  #if PIN_TFT_LEDA_CTL >= 0
    pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
    digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    delay(50);
  #endif
  SPI.begin(PIN_TFT_SCL, PIN_TFT_MISO, PIN_TFT_SDA, PIN_TFT_CS);
#endif

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

#if defined(HELTEC_LORA_V4_R8)
SPIClass* heltecV4R8SharedSPI() {
  return &SPI;   // FSPI (SPI2), begun in radio_init() for the TFT + micro-SD
}
#endif

