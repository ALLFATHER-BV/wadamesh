// SPDX-License-Identifier: GPL-3.0-or-later
#include "ST7796LCDDisplay.h"

#if defined(TLORA_PAGER) && defined(ESP32)

// This panel's 222px glass is narrower than the ST7796 controller's native
// (320px) GRAM. TFT_eSPI's own ST7796_Rotation.h already has the fix
// (colstart=49/rowstart=49 depending on rotation, applied automatically by
// every setAddrWindow() call) -- but only when CGRAM_OFFSET is defined.
// Unlike ST7789_Defines.h (which self-defines it), ST7796_Defines.h does not.
// Without this flag the build succeeds but every frame renders shifted/
// cropped by 49px with no error -- turn that into a build failure instead.
#if defined(ST7796_DRIVER) && !defined(CGRAM_OFFSET)
#error "ST7796LCDDisplay requires -D CGRAM_OFFSET=1 in this env's platformio.ini build_flags -- \
this panel's 222px glass is narrower than the ST7796 controller's 320px GRAM, and TFT_eSPI's \
ST7796_Rotation.h only applies the required 49px column/row offset when CGRAM_OFFSET is defined."
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3   // landscape, matches the other boards' default
#endif
#ifndef DISPLAY_SCALE_X
  #define DISPLAY_SCALE_X 1.0f // native res == logical res; kept for shape-parity with ST7789LCDDisplay
#endif
#ifndef DISPLAY_SCALE_Y
  #define DISPLAY_SCALE_Y 1.0f
#endif

bool ST7796LCDDisplay::begin() {
  if (!_isOn) {
    display.init();   // reads TFT_WIDTH/HEIGHT/ST7796_DRIVER/TFT_* pins from build flags; brings up its own SPI bus
    display.setRotation(DISPLAY_ROTATION);
    setLogicalSize((int)(display.width() / DISPLAY_SCALE_X), (int)(display.height() / DISPLAY_SCALE_Y));
    display.setAttribute(CP437_SWITCH, true);
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE);
    display.setTextSize((uint8_t)(2 * DISPLAY_SCALE_X));

    _backlight.begin(PIN_TFT_LEDA_CTL);   // claims the pin itself (pinMode + initial LOW) -- don't also drive it here
    setBrightness(_brightness_pct);

    _isOn = true;
  }
  return true;
}

void ST7796LCDDisplay::turnOn() { ST7796LCDDisplay::begin(); }

void ST7796LCDDisplay::turnOff() {
  if (_isOn) {
    setBrightness(0);
    display.writecommand(TFT_DISPOFF);   // TFT_RST=-1 on this board (not wired), so sleep via command, not reset pin
    _isOn = false;
  }
}

void ST7796LCDDisplay::clear() { display.fillScreen(TFT_BLACK); }

void ST7796LCDDisplay::startFrame(Color bkg) { (void)bkg; display.fillScreen(TFT_BLACK); }

void ST7796LCDDisplay::setTextSize(int sz) { display.setTextSize((uint8_t)(sz * DISPLAY_SCALE_X)); }

void ST7796LCDDisplay::setColor(Color c) {
  switch (c) {
    case DisplayDriver::DARK:   _color = TFT_BLACK;  break;
    case DisplayDriver::LIGHT:  _color = TFT_WHITE;  break;
    case DisplayDriver::RED:    _color = TFT_RED;    break;
    case DisplayDriver::GREEN:  _color = TFT_GREEN;  break;
    case DisplayDriver::BLUE:   _color = TFT_BLUE;   break;
    case DisplayDriver::YELLOW: _color = TFT_YELLOW; break;
    case DisplayDriver::ORANGE: _color = TFT_ORANGE; break;
    default:                    _color = TFT_WHITE;  break;
  }
  display.setTextColor(_color);
}

void ST7796LCDDisplay::setCursor(int x, int y) {
  display.setCursor((int16_t)(x * DISPLAY_SCALE_X), (int16_t)(y * DISPLAY_SCALE_Y));
}

void ST7796LCDDisplay::print(const char* str) { display.print(str); }

void ST7796LCDDisplay::fillRect(int x, int y, int w, int h) {
  display.fillRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
}

void ST7796LCDDisplay::drawRect(int x, int y, int w, int h) {
  display.drawRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
}

void ST7796LCDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  // TFT_eSPI has no native XBM primitive either -- same manual bit-unpack loop as ST7789LCDDisplay.
  uint8_t byteWidth = (w + 7) / 8;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      if (byte & (0x80 >> (i & 7))) {
        for (int dy = 0; dy < DISPLAY_SCALE_Y; dy++) {
          for (int dx = 0; dx < DISPLAY_SCALE_X; dx++) {
            display.drawPixel((int32_t)(x * DISPLAY_SCALE_X + i * DISPLAY_SCALE_X + dx),
                               (int32_t)(y * DISPLAY_SCALE_Y + j * DISPLAY_SCALE_Y + dy), _color);
          }
        }
      }
    }
  }
}

uint16_t ST7796LCDDisplay::getTextWidth(const char* str) {
  return (uint16_t)(display.textWidth(str) / DISPLAY_SCALE_X);
}

void ST7796LCDDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  display.startWrite();
  display.setAddrWindow(x, y, w, h);   // colstart/rowstart (CGRAM_OFFSET) applied here automatically
  display.pushColors(const_cast<uint16_t*>(pixels), (uint32_t)(w * h));   // default swap=true matches LVGL's RGB565 buffers
  display.endWrite();
}

void ST7796LCDDisplay::endFrame() { /* no-op: pushColors' own endWrite() already closed the SPI transaction */ }

void ST7796LCDDisplay::setDisplayRotation(uint8_t r) {
  display.setRotation(r);
  setLogicalSize((int)(display.width() / DISPLAY_SCALE_X), (int)(display.height() / DISPLAY_SCALE_Y));
}

void ST7796LCDDisplay::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  // AW9364LedDriver::setBrightness() is relative/circular pulse-stepping (no absolute-set wire
  // command on this chip) -- round to the nearest of its 17 discrete steps (0..16), not floor,
  // so e.g. 50% lands on step 8 rather than 7.
  uint8_t steps = (uint8_t)(((uint16_t)pct * 16 + 50) / 100);
  _backlight.setBrightness(steps);
  _brightness_pct = pct;
}

#endif
