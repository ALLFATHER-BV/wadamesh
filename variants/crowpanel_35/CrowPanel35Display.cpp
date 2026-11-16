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
  if (!_touch.ready()) {
    Serial.println("[crowpanel-35] GT911 did not answer, resetting it");
    recoverTouch();
  }
  _isOn = true;
  Serial.printf("[crowpanel-35] display %dx%d\n", width(), height());
  return true;
}

// The address the GT911 answers at, 0 while it does not. LovyanGFX's own reset does
// not hold INT, so after a software restart the chip can come up at 0x14 instead of
// 0x5D; LovyanGFX tries both, and the touch probe has to follow it. While the chip is
// silent this tries the full reset again now and then rather than stay dead.
uint8_t CrowPanel35Display::touchAddr() {
  if (!_touch.ready()) {
    const uint32_t now = millis();
    if (_touch_retries >= 5 || now - _touch_retry_ms < 3000) return 0;
    _touch_retry_ms = now;
    ++_touch_retries;
    if (!recoverTouch()) return 0;
  }
  return (uint8_t)_touch.config().i2c_addr;
}

// After a software restart the GT911 sometimes did not answer the few milliseconds
// LovyanGFX gives it after its 5 ms reset pulse, and LovyanGFX then leaves touch off
// for the whole session (kc9seb, Discord: dead touch after a restart from Settings,
// fine after the next one). A restart in the middle of a touch read can also leave
// the chip holding SDA low. So: clock the bus free, give the chip a full reset with
// INT held low (address 0x5D) and its boot time, then try again without a reset.
// Wire is released first and opened again last, in the order begin() uses: the pin
// writes below take GPIO15/16 off the I2C peripheral.
bool CrowPanel35Display::recoverTouch() {
  Wire.end();
  pinMode(15, INPUT_PULLUP);
  pinMode(16, OUTPUT);
  for (int i = 0; i < 9 && digitalRead(15) == LOW; ++i) {
    digitalWrite(16, LOW);  delayMicroseconds(5);
    digitalWrite(16, HIGH); delayMicroseconds(5);
  }
  pinMode(47, OUTPUT); digitalWrite(47, LOW);
  pinMode(48, OUTPUT); digitalWrite(48, LOW);
  delay(20);
  digitalWrite(48, HIGH);
  delay(10);
  pinMode(47, INPUT);
  delay(60);
  auto cfg = _touch.config();
  cfg.pin_rst = -1;   // the reset above stands; another short pulse would undo it
  cfg.i2c_addr = 0x5D;
  _touch.config(cfg);
  for (int i = 0; i < 3 && !_touch.init(); ++i) delay(20);
  Wire.begin(15, 16, 400000);
  Serial.printf("[crowpanel-35] GT911 %s after reset\n", _touch.ready() ? "answers" : "still silent");
  return _touch.ready();
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
  // The UI's portrait 180 (2) is then the panel's own 0; the landscapes pass through.
  static const uint8_t kPanelRot[4] = { 2, 1, 0, 3 };
  _lcd.setRotation(kPanelRot[rotation & 3]);
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
