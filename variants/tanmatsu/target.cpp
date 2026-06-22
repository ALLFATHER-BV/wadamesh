#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>   // StdRNG lives here (there is no StdRNG.h)
#include <TanmatsuDisplay.h>
#include <helpers/SensorManager.h>

TanmatsuBoard     board;
TanmatsuLoraRadio radio_driver;     // the coprocessor LoRa bridge (no RadioLib Module)
DISPLAY_CLASS     display;          // global LVGL flush target + boot/shutdown DisplayDriver
SensorManager     sensors;          // base (no telemetry yet); EnvironmentSensorManager later

ESP32RTCClock        fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

bool radio_init() {
  fallback_clock.begin();
  // TODO(device): if the Tanmatsu exposes a hardware RTC over I2C, do
  // rtc_clock.begin(Wire) + Wire.begin(<sda>,<scl>) here like the S3 boards.
  return radio_driver.init();       // lora_init_remote() + push PHY config
}

mesh::LocalIdentity radio_new_identity() {
  // S3 boards seed from RadioLib radio noise (RadioNoiseListener). The remote LoRa
  // has no equivalent, so seed a fresh identity from the same StdRNG the app uses,
  // keyed off getRngSeed() (esp_random ^ instantaneous RSSI noise).
  StdRNG rng;
  rng.begin(radio_driver.getRngSeed());
  return mesh::LocalIdentity(&rng);
}
