// SPDX-License-Identifier: GPL-3.0-or-later
#include "WioTrackerL2Display.h"

#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t kLightAddress = 0x2C;
constexpr uint8_t kLightUpdate = 0x0F;
constexpr uint8_t kLightPwm0 = 0x18;
}

bool WioTrackerL2Light::writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kLightAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool WioTrackerL2Light::init(uint8_t brightness) {
  bool ok = true;
  ok = writeRegister(0x00, 0x01) && ok;
  ok = writeRegister(0x01, 0x01) && ok;
  ok = writeRegister(0x02, 0x00) && ok;
  ok = writeRegister(0x04, 0x4E) && ok;
  ok = writeRegister(0x05, 0xF0) && ok;
  for (uint8_t channel = 0; channel < 4; ++channel) ok = writeRegister(0x14 + channel, 200) && ok;
  ok = writeRegister(0x02, 0x0F) && ok;
  setBrightness(brightness);
  return ok;
}

void WioTrackerL2Light::setBrightness(uint8_t brightness) {
  for (uint8_t channel = 0; channel < 4; ++channel) (void)writeRegister(kLightPwm0 + channel, brightness);
  (void)writeRegister(kLightUpdate, 0x55);
  _brightness = brightness;
}

WioTrackerL2Display::WioTrackerL2Display() : DisplayDriver(320, 240) {
  {
    auto cfg = _bus.config();
    cfg.spi_host = SPI3_HOST;
    cfg.spi_mode = 3;
    cfg.freq_write = 75000000;
    cfg.freq_read = 16000000;
    cfg.spi_3wire = false;
    cfg.use_lock = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk = 42;
    cfg.pin_miso = -1;
    cfg.pin_mosi = -1;
    cfg.pin_dc = -1;
    cfg.pin_io0 = 41;
    cfg.pin_io1 = 40;
    cfg.pin_io2 = 39;
    cfg.pin_io3 = 38;
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }
  {
    auto cfg = _panel.config();
    cfg.pin_cs = 46;
    cfg.pin_rst = -1;
    cfg.pin_busy = -1;
    cfg.panel_width = 240;
    cfg.panel_height = 320;
    cfg.memory_width = 240;
    cfg.memory_height = 320;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 1;
    cfg.readable = false;
    cfg.invert = true;
    cfg.rgb_order = true;
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    _panel.config(cfg);
  }
  _panel.setLight(&_light);
  {
    auto cfg = _touch.config();
    cfg.x_min = 0; cfg.x_max = 239;
    cfg.y_min = 0; cfg.y_max = 319;
    cfg.pin_int = -1;
    cfg.bus_shared = false;
    cfg.offset_rotation = 2;
    cfg.i2c_port = 0;
    cfg.i2c_addr = 0x5D;
    cfg.pin_sda = 47;
    cfg.pin_scl = 48;
    cfg.freq = 100000;
    _touch.config(cfg);
    _panel.setTouch(&_touch);
  }
  _lcd.setPanel(&_panel);
}

bool WioTrackerL2Display::begin() {
  if (_isOn) return true;
  (void)_light.init(160);
  _lcd.init();
  Wire.end();
  Wire.begin(47, 48, 100000);
  _lcd.setSwapBytes(true);
  _lcd.setRotation(0);
  _lcd.setBrightness(160);
  _lcd.fillScreen(0);
  setLogicalSize(_lcd.width(), _lcd.height());
  _isOn = true;
  Serial.printf("[wio-l2] display %dx%d\n", width(), height());
  return true;
}

void WioTrackerL2Display::turnOn() { if (!_isOn) { _lcd.setBrightness(160); _isOn = true; } }
void WioTrackerL2Display::turnOff() { if (_isOn) { _lcd.setBrightness(0); _isOn = false; } }
void WioTrackerL2Display::clear() { _lcd.fillScreen(0); }
void WioTrackerL2Display::startFrame(ColorVal bkg) { _lcd.fillScreen(bkg); }
void WioTrackerL2Display::setTextSize(int sz) { _lcd.setTextSize(sz); }
void WioTrackerL2Display::setColor(ColorVal c) { _color = c; _lcd.setTextColor(c); }
void WioTrackerL2Display::setCursor(int x, int y) { _lcd.setCursor(x, y); }
void WioTrackerL2Display::print(const char* str) { _lcd.print(str); }
void WioTrackerL2Display::fillRect(int x, int y, int w, int h) { _lcd.fillRect(x, y, w, h, _color); }
void WioTrackerL2Display::drawRect(int x, int y, int w, int h) { _lcd.drawRect(x, y, w, h, _color); }
void WioTrackerL2Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) { _lcd.drawXBitmap(x, y, bits, w, h, _color); }
uint16_t WioTrackerL2Display::getTextWidth(const char* str) { return _lcd.textWidth(str); }
void WioTrackerL2Display::endFrame() {}

void WioTrackerL2Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  _lcd.startWrite();
  _lcd.setAddrWindow(x, y, w, h);
  _lcd.writePixels(const_cast<uint16_t*>(pixels), (uint32_t)w * h);
  _lcd.endWrite();
}

void WioTrackerL2Display::setDisplayRotation(uint8_t) {
  _lcd.setRotation(0);
  setLogicalSize(_lcd.width(), _lcd.height());
}

void WioTrackerL2Display::setBrightness(uint8_t brightness) {
  _lcd.setBrightness(brightness);
}

bool WioTrackerL2Display::getTouchPoint(uint16_t& x, uint16_t& y) {
  int32_t tx = 0, ty = 0;
  if (!_lcd.getTouch(&tx, &ty)) return false;
  x = (uint16_t)tx;
  y = (uint16_t)ty;
  return true;
}