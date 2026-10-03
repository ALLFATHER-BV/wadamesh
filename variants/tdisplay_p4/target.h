#pragma once
#ifdef __cplusplus
extern "C" void set_boot_phase(int phase);
#endif

// LilyGo T-Display P4 target wiring. Unlike the Tanmatsu (coprocessor LoRa bridge), this board has
// a raw SX1262 or LR2021 on the P4's SPI, so it uses the core's RadioLib path like the S3 boards —
// except RESET + DIO1 and the antenna switch (SKY13453) live on the XL9535 expander.
#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/ESP32Board.h>
#if defined(USE_LR2021)
#include <helpers/radiolib/CustomLR2021Wrapper.h>
#else
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#endif
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#if defined(HAS_TDP4_LCD)
#include "HI8561Display.h"                // HI8561 TFT-LCD (LCD SKU) — DISPLAY_CLASS=HI8561Display
#else
#include "RM69A10Display.h"               // RM69A10 AMOLED (default SKU) — DISPLAY_CLASS=RM69A10Display
#endif
#include "Xl9535.h"
#include <esp_system.h>

#if defined(USE_LR2021)
class TDisplayP4LR2021Wrapper : public CustomLR2021Wrapper {
public:
  TDisplayP4LR2021Wrapper(CustomLR2021& radio, mesh::MainBoard& board)
      : CustomLR2021Wrapper(radio, board) {}

  bool pollRxDone() override {
    return (static_cast<CustomLR2021*>(_radio)->getIrqFlags() &
            RADIOLIB_LR2021_IRQ_RX_DONE) != 0;
  }

  bool setRxBoostedGainMode(bool enabled) override {
    const bool applied = CustomLR2021Wrapper::setRxBoostedGainMode(enabled);
    if (applied) _rx_boosted = enabled;
    return applied;
  }

  bool getRxBoostedGainMode() const override { return _rx_boosted; }

private:
#if defined(LR2021_RX_BOOSTED_GAIN) && LR2021_RX_BOOSTED_GAIN
  bool _rx_boosted = true;
#else
  bool _rx_boosted = false;
#endif
};
#endif

// Makes every software restart a full system reset, so the MIPI-DSI host comes back clean and the
// panel is not left dark after a settings reboot. Call first thing in app_main. See target.cpp.
void tdisplayP4InstallFullRestart();
// esp_reset_reason(), except that a restart made by the above reports ESP_RST_SW, not ESP_RST_WDT.
esp_reset_reason_t tdisplayP4ResetReason();

class TDisplayP4Board : public ESP32Board {
public:
  void begin();
  // LoRa TX/RX antenna path is the SKY13453, driven via the XL9535 (polarity TBD on-device).
  void onBeforeTransmit(void) override { xl9535.rfSwitchTx(true); }
  void onAfterTransmit(void)  override { xl9535.rfSwitchTx(false); }
  uint16_t getBattMilliVolts() override;   // BQ27220 gauge on I2C_1 — TODO(device)
  int      getBattStateOfCharge();         // BQ27220 StateOfCharge(), %; -1 = unavailable (#273)
  const char* getManufacturerName() const override { return "LilyGo T-Display P4"; }
};

extern TDisplayP4Board board;
extern WRAPPER_CLASS   radio_driver;
extern RADIO_CLASS     radio;   // raw SX1262/LR2021 (also driven directly by the Spectrum sweep)
extern ClockFloorRTC   rtc_clock;
extern EnvironmentSensorManager sensors;
extern DISPLAY_CLASS   display;   // RM69A10 MIPI-DSI, LVGL flush target

bool radio_init();
mesh::LocalIdentity radio_new_identity();
