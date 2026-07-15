#pragma once
#ifdef __cplusplus
extern "C" void set_boot_phase(int phase);
#endif

// LilyGo T-Display P4 target wiring. Unlike the Tanmatsu (coprocessor LoRa bridge), this board has
// a RAW SX1262 on the P4's SPI, so it uses the core's RadioLib path like the S3 boards — except the
// SX1262 RESET + DIO1 and the RF-path switch (SKY13453) live on the XL9535 expander.
#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include "RM69A10Display.h"
#include "Xl9535.h"

class TDisplayP4Board : public ESP32Board {
public:
  void begin();
  // LoRa TX/RX antenna path is the SKY13453, driven via the XL9535 (polarity TBD on-device).
  void onBeforeTransmit(void) override { xl9535.rfSwitchTx(true); }
  void onAfterTransmit(void)  override { xl9535.rfSwitchTx(false); }
  uint16_t getBattMilliVolts() override;   // BQ27220 gauge on I2C_1 — TODO(device)
  const char* getManufacturerName() const override { return "LilyGo T-Display P4"; }
};

extern TDisplayP4Board board;
extern WRAPPER_CLASS   radio_driver;
extern RADIO_CLASS     radio;   // raw SX1262 (also driven directly by the Spectrum sweep)
extern ClockFloorRTC   rtc_clock;
extern EnvironmentSensorManager sensors;
extern DISPLAY_CLASS   display;   // RM69A10 MIPI-DSI, LVGL flush target

bool radio_init();
mesh::LocalIdentity radio_new_identity();
