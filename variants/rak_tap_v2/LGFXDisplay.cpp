// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(RAK_WISMESH_TAP_V2)

#include "LGFXDisplay.h"
#include <Arduino.h>

#ifndef LGFX_INVERT_COLOR
  #define LGFX_INVERT_COLOR true  // default: black bg (ST7789 INVON)
#endif

LGFXDisplay::LGFXDisplay(RefCountedDigitalPin* peripher_power)
  : DisplayDriver(240, 320),
    _periph_power(peripher_power)
{
  _isOn  = false;
  _color = 0xFFFF;

  // ---- SPI bus config ----
  {
    auto cfg = _bus.config();
    cfg.spi_host    = SPI2_HOST;     // FSPI, already begun by SPI.begin() in radio_init()
    cfg.spi_mode    = 0;
    cfg.freq_write  = 20000000;      // 20 MHz — conservative for this PCB
    cfg.freq_read   = 16000000;
    cfg.spi_3wire   = false;
    cfg.use_lock    = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk    = PIN_TFT_SCL;   // 13
    cfg.pin_mosi    = PIN_TFT_SDA;   // 11
    cfg.pin_miso    = -1;
    cfg.pin_dc      = PIN_TFT_DC;    // 42
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }

  // ---- ST7789 panel config ----
  {
    auto cfg = _panel.config();
    cfg.pin_cs          = PIN_TFT_CS;   // 12
    cfg.pin_rst         = PIN_TFT_RST;  // -1
    cfg.pin_busy        = -1;
    cfg.panel_width     = 240;
    cfg.panel_height    = 320;
    cfg.offset_x        = 0;
    cfg.offset_y        = 0;
    cfg.offset_rotation = 0;
    cfg.readable        = false;
    cfg.invert          = LGFX_INVERT_COLOR;  // configurable via -D LGFX_INVERT_COLOR in platformio.ini
    cfg.rgb_order       = false;
    cfg.dlen_16bit      = false;
    cfg.bus_shared      = true;
    _panel.config(cfg);
  }

  // ---- Backlight ----
  {
    auto cfg = _light.config();
    cfg.pin_bl = PIN_TFT_LEDA_CTL; // 41
    cfg.invert = false;
    cfg.freq   = 44000;
    cfg.pwm_channel = 7;
    _light.config(cfg);
    _panel.setLight(&_light);
  }

  _lcd.setPanel(&_panel);
}

bool LGFXDisplay::begin() {
  if (_isOn) return true;
  if (_periph_power) _periph_power->claim();

  _lcd.init();
  _lcd.setSwapBytes(true);                         // LVGL pixels are LE; ST7789 needs BE
  _lcd.setRotation(1);                             // landscape 320×240
  _lcd.setBrightness(255);
  _lcd.fillScreen(0x0000);
  setLogicalSize(_lcd.width(), _lcd.height());

  _isOn = true;
  Serial.printf("[TFT] LGFXDisplay %dx%d rotation=1\n", _lcd.width(), _lcd.height());
  return true;
}

void LGFXDisplay::turnOn() {
  if (_isOn) return;
  if (_periph_power) _periph_power->claim();
  _lcd.setBrightness(255);
  _isOn = true;
}
void LGFXDisplay::turnOff() {
  if (!_isOn) return;
  _lcd.setBrightness(0);
  _isOn = false;
  if (_periph_power) _periph_power->release();
}
void LGFXDisplay::clear()                     { _lcd.fillScreen(0x0000); }
void LGFXDisplay::startFrame(Color)           { _lcd.fillScreen(0x0000); }
void LGFXDisplay::setTextSize(int sz)         { _lcd.setTextSize(sz); }

static inline uint16_t _c16(DisplayDriver::Color c) {
  switch (c) {
    case DisplayDriver::DARK:   return 0x0000;
    case DisplayDriver::LIGHT:  return 0xFFFF;
    case DisplayDriver::RED:    return 0xF800;
    case DisplayDriver::GREEN:  return 0x07E0;
    case DisplayDriver::BLUE:   return 0x001F;
    case DisplayDriver::YELLOW: return 0xFFE0;
    case DisplayDriver::ORANGE: return 0xFD20;
    default:                    return 0xFFFF;
  }
}
void LGFXDisplay::setColor(Color c) {
  _color = _c16(c);
  _lcd.setTextColor(_color);
}
void LGFXDisplay::setCursor(int x, int y)     { _lcd.setCursor(x, y); }
void LGFXDisplay::print(const char* s)         { _lcd.print(s); }
void LGFXDisplay::fillRect(int x,int y,int w,int h) { _lcd.fillRect(x,y,w,h,_color); }
void LGFXDisplay::drawRect(int x,int y,int w,int h) { _lcd.drawRect(x,y,w,h,_color); }
void LGFXDisplay::drawXbm(int x,int y,const uint8_t* b,int w,int h) { _lcd.drawXBitmap(x,y,b,w,h,_color); }
uint16_t LGFXDisplay::getTextWidth(const char* s) { return _lcd.textWidth(s); }
void LGFXDisplay::endFrame()                  {}

void LGFXDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  _lcd.startWrite();
  _lcd.setAddrWindow(x, y, w, h);
  _lcd.writePixels(const_cast<uint16_t*>(pixels), (uint32_t)(w * h));
  _lcd.endWrite();
}

void LGFXDisplay::setDisplayRotation(uint8_t) {
  _lcd.setRotation(1);
  setLogicalSize(_lcd.width(), _lcd.height());
}

#endif
