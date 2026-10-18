// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_CROWPANEL_P4_101)
#include <Arduino.h>
#include <Wire.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>   // RadioNoiseListener / StdRNG
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_hosted_host_fw_ver.h"   // ESP_HOSTED_VERSION_MAJOR_1/MINOR_1/PATCH_1 (host side)

CrowPanelP4Board board;

// SX1262 module on the external wireless header J9/J11, SPI3 SCLK=8 / MISO=7 / MOSI=6, NSS=10,
// BUSY=9. DIO1 and NRST moved between board revisions (see main/CMakeLists.txt, CP_REV): GPIO53/54 on
// V1.0/V1.1, GPIO27/28 on V1.2. All are direct P4 GPIOs, so RadioLib takes a real DIO1 interrupt.
static SPIClass spi(FSPI);
RADIO_CLASS   radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);

DISPLAY_CLASS display;

// No battery-backed RTC on this board: ESP32 system time, guarded by the monotonic floor.
ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

EnvironmentSensorManager sensors;   // no GPS fitted (an external UART GPS could be added later)

// ---- Board power ---------------------------------------------------------------------------------
// Elecrow's every display/wireless lesson acquires LDO3 = 2.5 V and LDO4 = 3.3 V before touching any
// peripheral. LDO3 feeds the MIPI D-PHY; LDO4 (net ESP32_LDO4 -> VDDPST_5 in the schematic) is the pad
// supply for the GT911 I2C/RST/INT lines, LCD_RESET and the microSD lines. Both are held for the life
// of the session. Because LDO4 is ours, SD_MMC is told to use "external" pad power (main.cpp:
// SD_MMC.setPowerChannel(-1)); otherwise Arduino's SD driver would try to acquire the same channel
// as an adjustable one and fail.
static esp_ldo_channel_handle_t s_ldo3 = nullptr;
static esp_ldo_channel_handle_t s_ldo4 = nullptr;

void crowpanelP4PowerOn() {
  esp_ldo_channel_config_t ldo4 = {};
  ldo4.chan_id = 4;
  ldo4.voltage_mv = 3300;
  if (esp_ldo_acquire_channel(&ldo4, &s_ldo4) != ESP_OK) printf("[BOOT] LDO4 (3.3 V pads) acquire FAILED\n");
  esp_ldo_channel_config_t ldo3 = {};
  ldo3.chan_id = 3;
  ldo3.voltage_mv = 2500;
  if (esp_ldo_acquire_channel(&ldo3, &s_ldo3) != ESP_OK) printf("[BOOT] LDO3 (DSI PHY 2.5 V) acquire FAILED\n");

  // GPIO29 = LCD_BK_POWER, gate of the optional P-MOSFET Q11 in front of the backlight boost. Q11 is
  // marked "AO3401_NC" (not fitted) with R184 = 0R bypassing it on V1.1/V1.2, so this is normally a
  // no-op; LOW is the "on" level should a unit have Q11 fitted.
  gpio_reset_pin(GPIO_NUM_29);
  gpio_set_direction(GPIO_NUM_29, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_29, 0);
  // GPIO31 = LCD_BK_EN: hold the MT9201 off until the display has blanked its framebuffer.
  gpio_reset_pin(GPIO_NUM_31);
  gpio_set_direction(GPIO_NUM_31, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_31, 0);
  // GPIO30 = AUDIO_OUT_SD, the speaker amplifier's shutdown (active low in Elecrow's code: LOW = on).
  // No audio support yet, so keep the amplifier OFF to avoid hiss and pops.
  gpio_reset_pin(GPIO_NUM_30);
  gpio_set_direction(GPIO_NUM_30, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_30, 1);
  delay(20);
}

bool crowpanelP4PadPowerOk() { return s_ldo4 != nullptr; }

void CrowPanelP4Board::begin() {
  ESP32Board::begin();   // Wire on PIN_BOARD_SDA/SCL = 45/46 (GT911, codec, HY2.0 port)
  printf("[BOOT] i2c bus 0 on SDA%d/SCL%d: %s\n", PIN_BOARD_SDA, PIN_BOARD_SCL,
         i2cIsInit(0) ? "ok" : "FAIL (Wire.begin refused; touch will retry)");
}

bool radio_init() {
  fallback_clock.begin();
  // No battery-backed RTC chip on this board, so no generic rtc_clock.begin(Wire) probe (#383):
  // CAP_HARDWARE_RTC is 0 and the opt-in cold-boot network time sync covers it.
  rtc_clock.noteHardwareClock(false);
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI, P_LORA_NSS);
  // std_init resets the module through P_LORA_RESET, sets DIO3 TCXO (SX126X_DIO3_TCXO_VOLTAGE,
  // 1.6 V as in Elecrow's Lesson14) and DIO2 as the RF switch.
  if (!radio.std_init(&spi)) {
    printf("[crowpanel-p4] SX1262 not found (NSS%d BUSY%d DIO1%d RST%d). Seat the SX1262 module in "
           "J9/J11, and check the build matches the board revision (CP_REV).\n",
           P_LORA_NSS, P_LORA_BUSY, P_LORA_DIO_1, P_LORA_RESET);
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}

// ---- ESP32-C6 (esp-hosted) reset -----------------------------------------------------------------
// C6_EN = GPIO32 on every revision (schematic net C6_EN -> IC1.EN, 10k pull-up R77 + 100 nF C15).
// Elecrow's V1.0/V1.1 Wi-Fi lessons reset the slave through GPIO32; the V1.2 lesson's sdkconfig names
// GPIO54, which the V1.2 schematic routes to header J7 pin 12, not to the C6, so GPIO32 is used here
// for all revisions. Pulse LOW 50 ms, release, give the hosted firmware time to boot.
extern "C" int hosted_reset_slave_callback() {
  gpio_reset_pin(GPIO_NUM_32);
  gpio_set_direction(GPIO_NUM_32, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_32, 1);
  delay(10);
  gpio_set_level(GPIO_NUM_32, 0);
  delay(50);
  gpio_set_level(GPIO_NUM_32, 1);
  delay(1000);
  return 0;
}

// ---- microSD presence probe ------------------------------------------------------------------------
// One quiet card-identification pass on SDMMC slot 0, 1-bit, IOMUX pins (CLK43 CMD44 D0 39), at the
// 400 kHz probing clock. Card identification always runs at 400 kHz, so when the card does not
// answer the first command (ESP_ERR_TIMEOUT, 0x107) a 20/10/5 MHz mount ladder can only fail the same
// way three times, each with an IDF error and an Arduino error. The IDF sdmmc tags are muted for this
// pass only. Boot use only: it inits and deinits the whole SDMMC host, so it must run before the C6
// (SDMMC slot 1) is started.
esp_err_t crowpanelSdProbe() {
  static const char* const kTags[] = {"sdmmc_common", "sdmmc_init", "sdmmc_sd", "sdmmc_cmd",
                                      "sdmmc_req", "sdmmc_periph", "sdmmc_mmc"};
  esp_log_level_t prev[sizeof kTags / sizeof kTags[0]];
  for (size_t i = 0; i < sizeof kTags / sizeof kTags[0]; i++) {
    prev[i] = esp_log_level_get(kTags[i]);
    esp_log_level_set(kTags[i], ESP_LOG_NONE);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_PROBING;
  // All pins 0 = "not set" = the slot's dedicated IOMUX pins (same trick as Arduino's SD_MMC slot-0
  // branch: the IDF's pin-not-set check is !pin, which GPIO_NUM_NC would fail).
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = slot.cmd = slot.d0 = slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_0;
  slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_0;
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.width = 1;
  slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t err = sdmmc_host_init();
  if (err == ESP_OK) {
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    if (err == ESP_OK) {
      sdmmc_card_t* card = (sdmmc_card_t*)calloc(1, sizeof(sdmmc_card_t));
      err = card ? sdmmc_card_init(&host, card) : ESP_ERR_NO_MEM;
      free(card);
      sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
    }
    sdmmc_host_deinit();
  }

  for (size_t i = 0; i < sizeof kTags / sizeof kTags[0]; i++) esp_log_level_set(kTags[i], prev[i]);
  return err;
}

// ---- ESP32-C6 firmware version ----------------------------------------------------------------------
// Written by esp-hosted's transport driver when the C6's INIT event arrives (build.sh patch to
// transport_drv.c), read by crowpanelC6Describe(). 0 / unknown until the transport has come up.
extern "C" {
volatile uint32_t wadamesh_c6_fw_version = 0;
volatile int      wadamesh_c6_fw_known   = 0;
}

int crowpanelC6Describe(char* out, size_t cap) {
  if (!out || !cap) return -1;
  if (!wadamesh_c6_fw_known) {
    snprintf(out, cap, "C6 firmware version not reported (transport not up)");
    return -1;
  }
  const uint32_t v = wadamesh_c6_fw_version;
  const unsigned maj = ESP_HOSTED_VERSION_MAJOR(v), min = ESP_HOSTED_VERSION_MINOR(v), pat = ESP_HOSTED_VERSION_PATCH(v);
  if (v == 0) {
    snprintf(out, cap, "C6 esp-hosted firmware did not report a version (very old); host is %d.%d.%d. "
             "Wi-Fi/BLE may be slow or fail; update the C6 with Elecrow's V2.12.3 image",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    return 1;
  }
  const bool older = (maj < ESP_HOSTED_VERSION_MAJOR_1) ||
                     (maj == ESP_HOSTED_VERSION_MAJOR_1 && min < ESP_HOSTED_VERSION_MINOR_1);
  if (older) {
    snprintf(out, cap, "C6 esp-hosted firmware %u.%u.%u, host %d.%d.%d: the C6 is older. Wi-Fi/BLE "
             "may be slow or time out (Elecrow's V2.12.3 C6 image matches this host)",
             maj, min, pat, ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
  } else {
    snprintf(out, cap, "C6 esp-hosted firmware %u.%u.%u (host %d.%d.%d): ok", maj, min, pat,
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
  }
  return older ? 1 : 0;
}

// ---- Software restart = full system reset ---------------------------------------------------------
// esp_restart() on the ESP32-P4 does not reset the MIPI-DSI host or its DPI path, so the panel can
// come back dark after a settings reboot (seen on the T-Display P4; same silicon here). A shutdown
// handler turns every software restart into an RTC-watchdog system reset; an RTC_NOINIT marker lets
// the next boot report it as a software restart rather than a watchdog crash.
#include <hal/wdt_hal.h>

static constexpr uint32_t kFullRestartMagic = 0x50345253u;   // "P4RS"
RTC_NOINIT_ATTR static uint32_t s_full_restart_marker;
static bool s_boot_was_full_restart = false;

static void crowpanelP4FullRestart() {
  s_full_restart_marker = kFullRestartMagic;
  wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
  wdt_hal_write_protect_disable(&rwdt);
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, 20000, WDT_STAGE_ACTION_RESET_SYSTEM);
  wdt_hal_enable(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
  while (true) {
  }
}

void crowpanelP4InstallFullRestart() {
  static bool s_installed = false;
  if (s_installed) return;
  s_boot_was_full_restart = (s_full_restart_marker == kFullRestartMagic) && esp_reset_reason() == ESP_RST_WDT;
  s_full_restart_marker = 0;
  s_installed = (esp_register_shutdown_handler(crowpanelP4FullRestart) == ESP_OK);
}

esp_reset_reason_t crowpanelP4ResetReason() {
  return s_boot_was_full_restart ? ESP_RST_SW : esp_reset_reason();
}

#endif  // HAS_CROWPANEL_P4_101
