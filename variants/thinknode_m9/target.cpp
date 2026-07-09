#include "target.h"
#include <Arduino.h>

ThinkNodeM9Board board;

// LR1110 has no std_init() helper in the core (unlike CustomSX1262/SX1268/
// LLCC68) — confirmed against MeshCore's own LR1110 boards (thinknode_m3,
// minewsemi_me25ls01). Module() takes the same (NSS, IRQ/DIO1, RESET, BUSY,
// spi) signature either way. M9: NSS=39, DIO1=42, RESET=45, BUSY=41,
// SCLK=40, MISO=38, MOSI=47 — all on the SAME physical SPI bus as the LCD
// (CS=16) and the microSD slot (CS=36), so radio and display share the
// literal global `SPI` object — matching how T-Deck/Heltec V4's radio (a
// plain default-constructed `static SPIClass spi;`, not an explicit-host
// instance) and the LR1110 reference boards (thinknode_m3, me25ls01, which
// also pass the global `SPI` to their Module()) both do it. The bus itself
// is begun once, in ThinkNodeM9Board::begin() (see M9Board.cpp for why that
// has to happen there rather than mirroring T-Deck/Heltec's "begin it inside
// radio_init()" exactly — their displays self-init on a separate dedicated
// bus, M9's display is on ST7789LCDDisplay's "default" branch and needs the
// global SPI already begun before display.begin() runs).
RADIO_CLASS radio =
    new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(
    fallback_clock); // wraps AutoDiscover: monotonic send-timestamp floor (#89)
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
// periph_power gates the LCD/GPS/sensor rail (GPIO18, active-low P-MOS).
// Passing &board.periph_power lets ST7789LCDDisplay::begin()/turnOff()
// claim/release it alongside the panel's own lifecycle. The backlight
// (GPIO17, PNP) is handled separately by ThinkNodeM9Board — see M9Board.h
// for why it's NOT routed through PIN_TFT_LEDA_CTL.
DISPLAY_CLASS display(&board.periph_power);
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

#ifndef LORA_CR
#define LORA_CR 5
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire); // peripheral I2C bus (7/6) — RTC PCF8563 @ 0x51

#if defined(HAS_M9_KEYBOARD)
  m9KeyboardBegin(); // own bus, Wire1 (20/21) — no contention with Wire
#endif

#ifdef LR11X0_DIO3_TCXO_VOLTAGE
  float tcxo = LR11X0_DIO3_TCXO_VOLTAGE;
#else
  // 0 = disable RadioLib's internal TCXO-bias feature. M9's LR1110 clock
  // comes from Y1, a self-powered active oscillator (schematic-confirmed:
  // VCC/GND/GND/OUT, output -> XTA directly) — not a passive crystal the
  // chip needs to bias itself, so this must stay 0. See the build-flag
  // comment in platformio.ini for the full explanation.
  float tcxo = 0.0f;
#endif

  // SPI bus itself was already begun once in ThinkNodeM9Board::begin() (the
  // display needs it before display.begin() runs) — this is the manual
  // equivalent of what CustomSX1262/SX1268's std_init() would otherwise do,
  // since CustomLR1110 has no std_init() helper.
  int status = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                           RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                           LORA_TX_POWER, 16, tcxo);
  if (status != RADIOLIB_ERR_NONE) {
    // One retry after a settle: the first begin() enables the TCXO supply, and
    // if Y1's startup outruns RadioLib's fixed internal wait the first
    // calibration can fail while the second attempt finds a stable clock.
    delay(150);
    status = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                         RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER,
                         16, tcxo);
  }
  // Print the chip's own identification in BOTH outcomes: device type,
  // transceiver FW revision and pending error flags. Preprod M9 units ship
  // with old LR1110 firmware (pre-0x0308), which is exactly what this line
  // catches — see scripts/build/patch_radiolib_lr11x0.py for the matching
  // old-FW tolerance in RadioLib's config(). Safe to call even after a
  // failed begin(): the SPI link is configured before the failing step.
  {
    LR11x0VersionInfo_t vinfo;
    uint16_t chip_errors = 0;
    if (radio.getVersionInfo(&vinfo) == RADIOLIB_ERR_NONE) {
      radio.getErrors(&chip_errors);
      Serial.printf("LR1110: hw=0x%02X device=0x%02X fw=%u.%u wifi=%u.%u "
                    "gnss=%u.%u errors=0x%04X\n",
                    vinfo.hardware, vinfo.device, vinfo.fwMajor, vinfo.fwMinor,
                    vinfo.fwMajorWiFi, vinfo.fwMinorWiFi, vinfo.fwGNSS,
                    vinfo.almanacGNSS, chip_errors);
    } else {
      Serial.println("LR1110: getVersionInfo failed (chip not answering)");
    }
  }

  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: radio init failed: ");
    Serial.println(status);
    return false;
  }

  radio.setCRC(2);
  radio.explicitHeader();

  // RF-switch DIO table: pin assignment is SCHEMATIC-CONFIRMED
  // (Think_Node_M9_V1_0.pdf, the LR1110/U7 block) — DIO5 (pin 20) -> R20 -> net
  // RFSW0_V1 -> switch IC (U8) V1; DIO6 (pin 19) -> R19 -> net RFSW1_V2 -> U8
  // V2. DIO7/DIO8 are unconnected on this board (no net, dangling stubs) —
  // unlike t1000-e/me25ls01's 4-pin DIO5-8 scheme, this is a plain 2-pin switch
  // into a single antenna (U8 RFC -> C74 -> L12 -> ANT1), matching
  // thinknode_m3's table shape exactly. What's NOT independently re-derived:
  // the per-mode HIGH/LOW truth table below — U8's part number isn't printed on
  // the schematic, so this reuses the conventional STBY/RX/TX/TX_HP polarity
  // every other MeshCore LR1110 board's 2-pin table uses (thinknode_m3, same
  // shape). If TX/RX work but seem swapped or dead, flip the RX/TX_HP rows here
  // first.
#ifdef RF_SWITCH_TABLE
  static const uint32_t rfswitch_dios[Module::RFSWITCH_MAX_PINS] = {
      RADIOLIB_LR11X0_DIO5, // -> RFSW0_V1 (U8 V1)
      RADIOLIB_LR11X0_DIO6, // -> RFSW1_V2 (U8 V2)
      RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};
  static const Module::RfSwitchMode_t rfswitch_table[] = {
      {LR11x0::MODE_STBY, {LOW, LOW}},  {LR11x0::MODE_RX, {HIGH, LOW}},
      {LR11x0::MODE_TX, {HIGH, HIGH}},  {LR11x0::MODE_TX_HP, {LOW, HIGH}},
      {LR11x0::MODE_TX_HF, {LOW, LOW}}, {LR11x0::MODE_GNSS, {LOW, LOW}},
      {LR11x0::MODE_WIFI, {LOW, LOW}},  END_OF_MODE_TABLE,
  };
  radio.setRfSwitchTable(rfswitch_dios, rfswitch_table);
#endif
#ifdef RX_BOOSTED_GAIN
  radio.setRxBoostedGainMode(RX_BOOSTED_GAIN);
#endif

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}

SPIClass *m9SharedSPI() {
  return &SPI; // global instance, shared by radio + display + SD; begun once in
               // M9Board::begin()
}

void m9SetBacklight(bool on) {
  if (on)
    board.backlight.claim();
  else
    board.backlight.release();
}
