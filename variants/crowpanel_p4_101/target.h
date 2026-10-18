#pragma once
#ifdef __cplusplus
extern "C" void set_boot_phase(int phase);
#endif

// Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI (1024x600 IPS, EK79007 MIPI-DSI, GT911 touch).
// EXPERIMENTAL port, built from Elecrow's schematics (Elecrow-RD/CrowPanel-Advanced-10.1inch-
// ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen, Eagle_SCH&PCB/1.0, 1.1, 1.2) and their
// idf-code lessons; not yet run on hardware.
//
// Pin map. [S] = traced in the schematic netlist, [E] = used by Elecrow's ESP-IDF lesson code.
//   Display   EK79007, MIPI-DSI 2 lanes (dedicated pins), 900 Mbps, DPI 51 MHz      [E]
//             LCD_RESET GPIO41 [S], unused like Elecrow (reset_gpio_num = -1)        [E]
//   Backlight LCD_BK_EN GPIO31 (MT9201 EN, LEDC 30 kHz)                              [S][E]
//             LCD_BK_POWER GPIO29 (Q11 gate; Q11 not fitted, R184 0R bypass)          [S]
//   LDOs      LDO3 2.5 V (DSI PHY), LDO4 3.3 V (VDDPST_5: touch, LCD reset, SD pads)  [S][E]
//   Touch     GT911 I2C0 SDA45 SCL46, RST40, INT42                                   [S][E]
//   C6        SDIO slot 1 CLK18 CMD19; C6_EN GPIO32                                   [S][E]
//             V1.1/V1.2: D0..D3 = GPIO17,16,15,14 (4-bit)                            [S][E]
//             V1.0:      D0..D3 = GPIO14,15,16,17 (run 1-bit like Elecrow's V1.0 lesson) [S][E]
//   LoRa      SX1262 module on J9/J11: SCK8 MISO7 MOSI6 NSS10 BUSY9                    [S][E]
//             V1.0/V1.1: DIO1 = GPIO53 (J9.1), NRST = GPIO54 (J11.1)                  [S][E]
//             V1.2:      DIO1 = GPIO27 (J9.1), NRST = GPIO28 (J11.1)                  [S][E]
//             TCXO 1.6 V, DIO2 RF switch (Elecrow Lesson14 begin() values)            [E]
//             (V1.2 net names call GPIO10 "W_BUSY"; the lesson code uses 9 = BUSY, 10 = NSS)
//   microSD   SDMMC slot 0, 1-bit: CLK43 CMD44 D0 39                                  [S][E]
//   BOOT key  GPIO35 (SPI_BOOT, K3), not used yet                                    [S]
//   Audio     I2S LRCK21 BCLK22 DOUT23, amp shutdown GPIO30 (held off), not used yet  [S][E]
//   Battery   TP4059 charger + VBAT ADC on the STC8 helper MCU, no P4 link            [S]
//
// Unlike the T-Display P4 there is no I/O expander: every control line is a plain P4 GPIO. The
// SX1262 sits on the external wireless header (J9/J11) with a real DIO1 interrupt line, so the
// stock RadioLib ISR path is used (no MESH_RADIO_DIO1_POLLED).
#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include "EK79007Display.h"
#include <esp_system.h>

// Power the board's on-chip LDO rails (LDO3 = DSI PHY, LDO4 = the 3.3 V pad domain that carries the
// touch I2C, LCD reset and microSD lines) and park the backlight dark. Call first in app_main, before
// Wire / the display / the SD card touch any of those pads.
void crowpanelP4PowerOn();
// True once crowpanelP4PowerOn() holds LDO4 (the GT911 / SD pad supply).
bool crowpanelP4PadPowerOk();
// One plain line about the C6's esp-hosted firmware version vs the host's (from the transport INIT
// event). Returns -1 while the transport has not come up yet, 0 when the C6 matches the host,
// 1 when the C6 is older than the host (major.minor), which is when RPCs can time out.
int crowpanelC6Describe(char* out, size_t cap);
// Quiet one-pass microSD identification (boot only, before the C6 starts). ESP_OK = a card answered,
// ESP_ERR_TIMEOUT = no card or it does not respond, anything else = a card that answered but failed.
esp_err_t crowpanelSdProbe();

// Makes every software restart a full system reset, so the MIPI-DSI host comes back clean (same
// P4 silicon behaviour, and the same fix, as the T-Display P4). Call first thing in app_main.
void crowpanelP4InstallFullRestart();
// esp_reset_reason(), except that a restart made by the above reports ESP_RST_SW, not ESP_RST_WDT.
esp_reset_reason_t crowpanelP4ResetReason();

class CrowPanelP4Board : public ESP32Board {
public:
  void begin();
  // The battery charger and its ADC belong to the on-board STC8 helper MCU, which has no link to the
  // P4 in the schematic, so there is no battery reading to report (CAP_BATTERY 0).
  const char* getManufacturerName() const override { return "Elecrow CrowPanel P4 10.1"; }
};

extern CrowPanelP4Board board;
extern WRAPPER_CLASS    radio_driver;
extern RADIO_CLASS      radio;   // raw SX1262 (also driven directly by the Spectrum sweep)
extern ClockFloorRTC    rtc_clock;
extern EnvironmentSensorManager sensors;
extern DISPLAY_CLASS    display;   // EK79007 MIPI-DSI, LVGL flush target

bool radio_init();
mesh::LocalIdentity radio_new_identity();
