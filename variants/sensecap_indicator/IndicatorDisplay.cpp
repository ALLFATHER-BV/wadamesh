// SPDX-License-Identifier: GPL-3.0-or-later
#include "IndicatorDisplay.h"

#include <Arduino.h>

#include "IndicatorIo.h"

#ifndef INDICATOR_PCLK_HZ
#error "INDICATOR_PCLK_HZ comes from the sensecap_indicator env in platformio.ini"
#endif

namespace {
constexpr uint8_t kBacklightPin = 45;
constexpr uint32_t kBacklightHz = 12000;
// Nothing else claims an LEDC channel on this board: PIN_TFT_LEDA_CTL is -1, so
// UITask's generic backlight PWM (channel 6) is compiled out.
constexpr uint8_t kBacklightChannel = 7;
constexpr uint8_t kDefaultBrightness = 160;
}  // namespace

// List 1 comes from Meshtastic device-ui (GPL-3.0, Copyright (c) Meshtastic),
// include/graphics/LGFX/LGFX_INDICATOR.h @ 62ed2aa2, Panel_Indicator::getInitCommands.
// The closing 0xFF,0xFF ends the list; without it the panel comes up with
// inverted colours.
const uint8_t* IndicatorPanel::getInitCommands(uint8_t listno) const {
  static constexpr const uint8_t list1[] = {
      0x36, 1, 0x10,                         // MADCTL for vertical flip
      0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10, // Command2 BK0 SEL
      0xC7, 1, 0x04,                         // SDIR: X-direction Control (Horizontal Flip)
      0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00, // Command2 BK0 DIS
      0xFF, 0xFF
  };
  switch (listno) {
    case 1:
      return list1;
    default:
      return lgfx::Panel_ST7701::getInitCommands(listno);
  }
}

IndicatorDisplay::IndicatorDisplay() : DisplayDriver(480, 480) {
  {
    auto cfg = _panel.config();
    cfg.pin_cs = -1;
    cfg.pin_rst = -1;
    cfg.pin_busy = -1;
    cfg.memory_width = 480;
    cfg.memory_height = 480;
    cfg.panel_width = 480;
    cfg.panel_height = 480;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 0;
    _panel.config(cfg);
  }
  {
    // CS is P0.4 on the expander; begin() drives it around _lcd.init().
    auto cfg = _panel.config_detail();
    cfg.pin_cs = -1;
    cfg.pin_sclk = 41;
    cfg.pin_mosi = 48;
    cfg.use_psram = 1;
    _panel.config_detail(cfg);
  }
  {
    auto cfg = _bus.config();
    cfg.panel = &_panel;

    cfg.pin_d0 = GPIO_NUM_15;  // B0
    cfg.pin_d1 = GPIO_NUM_14;  // B1
    cfg.pin_d2 = GPIO_NUM_13;  // B2
    cfg.pin_d3 = GPIO_NUM_12;  // B3
    cfg.pin_d4 = GPIO_NUM_11;  // B4

    cfg.pin_d5 = GPIO_NUM_10;  // G0
    cfg.pin_d6 = GPIO_NUM_9;   // G1
    cfg.pin_d7 = GPIO_NUM_8;   // G2
    cfg.pin_d8 = GPIO_NUM_7;   // G3
    cfg.pin_d9 = GPIO_NUM_6;   // G4
    cfg.pin_d10 = GPIO_NUM_5;  // G5

    cfg.pin_d11 = GPIO_NUM_4;  // red bit 0
    cfg.pin_d12 = GPIO_NUM_3;  // red bit 1
    cfg.pin_d13 = GPIO_NUM_2;  // red bit 2
    cfg.pin_d14 = GPIO_NUM_1;  // red bit 3
    cfg.pin_d15 = GPIO_NUM_0;  // red bit 4

    cfg.pin_henable = GPIO_NUM_18;
    cfg.pin_vsync = GPIO_NUM_17;
    cfg.pin_hsync = GPIO_NUM_16;
    cfg.pin_pclk = GPIO_NUM_21;
    cfg.freq_write = INDICATOR_PCLK_HZ;

    cfg.hsync_pulse_width = 8;
    cfg.hsync_back_porch = 50;
    cfg.hsync_front_porch = 10;
    cfg.vsync_pulse_width = 8;
    cfg.vsync_back_porch = 20;
    cfg.vsync_front_porch = 10;
    cfg.hsync_polarity = 0;
    cfg.vsync_polarity = 0;
    cfg.pclk_active_neg = 0;
    cfg.de_idle_high = 1;
    cfg.pclk_idle_high = 0;

    _bus.config(cfg);
  }
  _panel.setBus(&_bus);
  {
    auto cfg = _light.config();
    cfg.pin_bl = kBacklightPin;
    cfg.freq = kBacklightHz;
    cfg.pwm_channel = kBacklightChannel;
    cfg.invert = false;
    _light.config(cfg);
  }
  _panel.setLight(&_light);
  _lcd.setPanel(&_panel);
}

bool IndicatorDisplay::begin() {
  if (_isOn) return true;
  if (!IndicatorIo::ready()) {
    Serial.println("[indicator] display: expander not ready, panel left off");
    return false;
  }
  auto& io = IndicatorIo::chip();
  bool ok = true;
  {
    IndicatorIo::BusLock lock;
    ok = io.setOutput(indicator::pins::kLoraNss, true) && ok;
  }
  {
    IndicatorIo::BusLock lock;
    ok = io.setOutput(indicator::pins::kLcdCs, true) && ok;
  }
  {
    IndicatorIo::BusLock lock;
    ok = io.setOutput(indicator::pins::kLcdReset, false) && ok;
  }
  delay(10);
  {
    IndicatorIo::BusLock lock;
    ok = io.setOutput(indicator::pins::kLcdReset, true) && ok;
  }
  delay(120);
  bool lcdOk = false;
  {
    // One lock from CS low to CS high: no other task may move the radio's NSS
    // while LovyanGFX bit-bangs the panel's init over the shared GPIO41/48.
    IndicatorIo::BusLock lock;
    ok = io.setOutput(indicator::pins::kLcdCs, false) && ok;
    lcdOk = _lcd.init();
    ok = io.setOutput(indicator::pins::kLcdCs, true) && ok;
  }
  // A failed init leaves the backlight at the 0 the panel init wrote. A
  // successful init lights it, so a missed expander write turns it off again:
  // the panel may not have seen its reset or init commands.
  if (!lcdOk) {
    Serial.println("[indicator] display: lcd.init failed");
    return false;
  }
  if (!ok) {
    _lcd.setBrightness(0);
    Serial.println("[indicator] display: expander write failed during panel init");
    return false;
  }

  // Pixels go in as the RAK/Wio LovyanGFX drivers pass them (LV_COLOR_16_SWAP 0
  // plus setSwapBytes).
  _lcd.setSwapBytes(true);
  _lcd.setRotation(0);
  _lcd.fillScreen(0);
  setLogicalSize(_lcd.width(), _lcd.height());
  _panelOk = true;
  _isOn = true;
  setBrightness(kDefaultBrightness);
  Serial.printf("[indicator] display %dx%d\n", width(), height());
  return true;
}

void IndicatorDisplay::turnOn() {
  if (!_panelOk || _isOn) return;
  _lcd.setBrightness(kDefaultBrightness);
  _isOn = true;
}
void IndicatorDisplay::turnOff() { if (_isOn) { _lcd.setBrightness(0); _isOn = false; } }
void IndicatorDisplay::clear() { _lcd.fillScreen(0); }
void IndicatorDisplay::startFrame(ColorVal bkg) { _lcd.fillScreen(bkg); }
void IndicatorDisplay::setTextSize(int sz) { _lcd.setTextSize(sz); }
void IndicatorDisplay::setColor(ColorVal c) { _color = c; _lcd.setTextColor(c); }
void IndicatorDisplay::setCursor(int x, int y) { _lcd.setCursor(x, y); }
void IndicatorDisplay::print(const char* str) { _lcd.print(str); }
void IndicatorDisplay::fillRect(int x, int y, int w, int h) { _lcd.fillRect(x, y, w, h, _color); }
void IndicatorDisplay::drawRect(int x, int y, int w, int h) { _lcd.drawRect(x, y, w, h, _color); }
void IndicatorDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) { _lcd.drawXBitmap(x, y, bits, w, h, _color); }
uint16_t IndicatorDisplay::getTextWidth(const char* str) { return _lcd.textWidth(str); }
void IndicatorDisplay::endFrame() {}

void IndicatorDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* px) {
  if (!_isOn || !px || w <= 0 || h <= 0) return;
  _lcd.startWrite();
  _lcd.setAddrWindow(x, y, w, h);
  _lcd.writePixels(const_cast<uint16_t*>(px), (uint32_t)w * h);
  _lcd.endWrite();
}

// The panel is square and the UI never rotates it.
void IndicatorDisplay::setDisplayRotation(uint8_t) {
  _lcd.setRotation(0);
  setLogicalSize(_lcd.width(), _lcd.height());
}

// Light_PWM writes duty 0 for brightness 0, so 0 switches the backlight off.
// UITask drives the backlight through the global display even when begin()
// failed; a panel that never came up stays dark.
void IndicatorDisplay::setBrightness(uint8_t brightness) {
  if (!_panelOk) return;
  _lcd.setBrightness(brightness);
}
