// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrowPanel35Display.h"

#include <Arduino.h>
#include <Wire.h>

CrowPanel35Display::CrowPanel35Display() : DisplayDriver(480, 320) {
  {
    auto cfg = _bus.config();
    cfg.spi_host = SPI3_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = 40000000;
    cfg.freq_read = 16000000;
    cfg.spi_3wire = false;
    cfg.use_lock = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk = 42;
    cfg.pin_mosi = 39;
    cfg.pin_miso = -1;
    cfg.pin_dc = 41;
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }
  {
    auto cfg = _panel.config();
    cfg.pin_cs = 40;
    cfg.pin_rst = -1;
    cfg.pin_busy = -1;
    cfg.panel_width = 320;
    cfg.panel_height = 480;
    cfg.memory_width = 320;
    cfg.memory_height = 480;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.readable = false;
    cfg.invert = true;
    cfg.rgb_order = false;
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    _panel.config(cfg);
  }
  {
    auto cfg = _light.config();
    cfg.pin_bl = 38;
    cfg.invert = false;
    cfg.freq = 44000;
    cfg.pwm_channel = 7;
    _light.config(cfg);
    _panel.setLight(&_light);
  }
  {
    auto cfg = _touch.config();
    cfg.i2c_port = 0;
    cfg.i2c_addr = 0x5D;
    cfg.pin_sda = 15;
    cfg.pin_scl = 16;
    cfg.pin_int = 47;
    cfg.pin_rst = 48;
    cfg.bus_shared = false;
    cfg.freq = 400000;
    cfg.x_min = 0;
    cfg.x_max = 319;
    cfg.y_min = 0;
    cfg.y_max = 479;
    cfg.offset_rotation = 0;
    _touch.config(cfg);
    _panel.setTouch(&_touch);
  }
  _lcd.setPanel(&_panel);
}

bool CrowPanel35Display::begin() {
  if (_isOn) return true;
  if (!_lcd.init()) {
    Serial.println("[crowpanel-35] ILI9488 display init failed");
    return false;
  }
  _lcd.setSwapBytes(true);
  setDisplayRotation(3);
  _lcd.setBrightness(160);
  _lcd.fillScreen(0);
  Wire.end();
  if (!Wire.begin(15, 16, 400000)) {
    Serial.println("[crowpanel-35] GT911 I2C bus init failed");
    return false;
  }
  _isOn = true;
  Serial.printf("[crowpanel-35] display %dx%d\n", width(), height());
  return true;
}

void CrowPanel35Display::turnOn() {
  if (!_isOn) { _lcd.setBrightness(160); _isOn = true; }
}
void CrowPanel35Display::turnOff() {
  if (_isOn) { _lcd.setBrightness(0); _isOn = false; }
}
void CrowPanel35Display::clear() { _lcd.fillScreen(0); }
void CrowPanel35Display::setDisplayRotation(uint8_t rotation) {
  // Keep the saved portrait mode at 0; rotate the panel and touch together by 180 degrees.
  _lcd.setRotation(rotation == 0 ? 2 : rotation);
  setLogicalSize(_lcd.width(), _lcd.height());
}

void CrowPanel35Display::startFrame(ColorVal bkg) { _lcd.fillScreen(bkg); }
void CrowPanel35Display::setTextSize(int sz) { _lcd.setTextSize(sz); }
void CrowPanel35Display::setColor(ColorVal c) { _color = c; _lcd.setTextColor(c); }
void CrowPanel35Display::setCursor(int x, int y) { _lcd.setCursor(x, y); }
void CrowPanel35Display::print(const char* str) { _lcd.print(str); }
void CrowPanel35Display::fillRect(int x, int y, int w, int h) {
  _lcd.fillRect(x, y, w, h, _color);
}
void CrowPanel35Display::drawRect(int x, int y, int w, int h) {
  _lcd.drawRect(x, y, w, h, _color);
}
void CrowPanel35Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _lcd.drawXBitmap(x, y, bits, w, h, _color);
}
uint16_t CrowPanel35Display::getTextWidth(const char* str) { return _lcd.textWidth(str); }
void CrowPanel35Display::endFrame() {}
void CrowPanel35Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  _lcd.startWrite();
  _lcd.setAddrWindow(x, y, w, h);
  _lcd.writePixels(const_cast<uint16_t*>(pixels), (uint32_t)w * h);
  _lcd.endWrite();
}
void CrowPanel35Display::setBrightness(uint8_t value) { _lcd.setBrightness(value); }
bool CrowPanel35Display::getTouchPoint(uint16_t& x, uint16_t& y) {
  int32_t tx = 0, ty = 0;
  if (!_lcd.getTouch(&tx, &ty)) return false;
  x = (uint16_t)tx;
  y = (uint16_t)ty;
  return true;
}
