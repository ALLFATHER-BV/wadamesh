// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HELTEC_LORA_V4_R8)

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
  // SPI2_HOST (FSPI), already begun by SPI.begin() in radio_init(). The micro-SD
  // slot reuses this same bus (bus_shared=true releases it between transactions).
  {
    auto cfg = _bus.config();
    cfg.spi_host    = SPI2_HOST;
    cfg.spi_mode    = 0;
    cfg.freq_write  = 40000000;      // 40 MHz — the old 20 MHz made a full-screen flush
                                     // ~61 ms of pure bus time (~4x the plain V4's 80 MHz
                                     // driver); ST7789 typically takes 62.5-80 MHz, so 40
                                     // is still the conservative choice. If hardware shows
                                     // tearing/garbled bands, fall back to 26.6 MHz.
    cfg.freq_read   = 16000000;
    cfg.spi_3wire   = false;
    cfg.use_lock    = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk    = PIN_TFT_SCL;   // 16
    cfg.pin_mosi    = PIN_TFT_SDA;   // 15
    // MISO=45 MUST be routed here even though the ST7789 panel is write-only: the
    // shared micro-SD (CS=3) READS its data back over this same pin. LovyanGFX owns
    // SPI2 (it runs first in setup — the display renders fine), so if LGFX leaves
    // MISO unrouted (-1), the later Arduino SPI.begin() can't retrofit it onto the
    // already-initialised bus and every SD read returns nothing. That is exactly the
    // tester signature: a known-good FAT32 card "wants formatting" (FAT can't be read)
    // and f_mkfs hangs (writes go out on MOSI, the read-back verify never lands).
    // readable=false in the panel config still keeps LGFX from ever reading the panel.
    cfg.pin_miso    = PIN_TFT_MISO;  // 45 — needed for shared micro-SD reads
    cfg.pin_dc      = PIN_TFT_DC;    // 48
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }

  // ---- ST7789 panel config ----
  {
    auto cfg = _panel.config();
    cfg.pin_cs          = PIN_TFT_CS;   // 47
    cfg.pin_rst         = PIN_TFT_RST;  // 21
    cfg.pin_busy        = -1;
    cfg.panel_width     = 240;
    cfg.panel_height    = 320;
    cfg.offset_x        = 0;
    cfg.offset_y        = 0;
    cfg.offset_rotation = 0;
    cfg.readable        = false;
    cfg.invert          = LGFX_INVERT_COLOR;  // configurable via -D LGFX_INVERT_COLOR
    cfg.rgb_order       = false;
    cfg.dlen_16bit      = false;
    cfg.bus_shared      = true;
    _panel.config(cfg);
  }

  // ---- Backlight ----
  // Deliberately NOT routed through LGFX's Light_PWM: UITask's applyBrightness()
  // owns GPIO44 on LEDC channel 6 (20 kHz / 8-bit), and a second owner
  // (Light_PWM ch7, 44 kHz / 9-bit, sharing LEDC timer 3) was a latent
  // conflict — any future _lcd.setBrightness() would write 9-bit duty onto a
  // channel that no longer owns the pin. begin()/turnOn()/turnOff() drive the
  // pad directly for the boot-logo window; UITask takes over once the UI is up.

  _lcd.setPanel(&_panel);
}

bool LGFXDisplay::begin() {
  if (_isOn) return true;
  if (_periph_power) _periph_power->claim();

  _lcd.init();
  _lcd.setSwapBytes(true);                         // LVGL pixels are LE; ST7789 needs BE
  // PORTRAIT as-inited (240x320) — the V4-shared UI contract: portrait never calls
  // setDisplayRotation() ("portrait leaves the panel as inited"), landscape calls it with 1/3.
  // The old hardcoded setRotation(1) here put the panel in landscape under portrait frames —
  // the tester's "boot logo in the wrong orientation".
  _lcd.setRotation(0);
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);       // boot-logo backlight (UITask's LEDC takes over later)
  digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  _lcd.fillScreen(0x0000);
  setLogicalSize(_lcd.width(), _lcd.height());

  _isOn = true;
  Serial.printf("[TFT] LGFXDisplay %dx%d rotation=0 (portrait)\n", _lcd.width(), _lcd.height());
  return true;
}

void LGFXDisplay::turnOn() {
  if (_isOn) return;
  if (_periph_power) _periph_power->claim();
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);       // re-route from any LEDC attach back to plain GPIO
  digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  _isOn = true;
}
void LGFXDisplay::turnOff() {
  if (!_isOn) return;
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
  digitalWrite(PIN_TFT_LEDA_CTL, LOW);
  _isOn = false;
  if (_periph_power) _periph_power->release();
}
void LGFXDisplay::clear()                     { _lcd.fillScreen(0x0000); }
void LGFXDisplay::startFrame(ColorVal)        { _lcd.fillScreen(0x0000); }
void LGFXDisplay::setTextSize(int sz)         { _lcd.setTextSize(sz); }

void LGFXDisplay::setColor(ColorVal c) {
  _color = c;   // ColorVal IS RGB565 (1.17 UIColor)
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

// UI contract (see UITask applyRotation): called with 1 for ROT_90, 3 for ROT_270; portrait
// stays as-inited (rotation 0). Honor the argument — the old hardcoded 1 ignored it.
void LGFXDisplay::setDisplayRotation(uint8_t r) {
  _lcd.setRotation(r & 3);
  setLogicalSize(_lcd.width(), _lcd.height());
}

// Anti-burn-in panel sleep over LGFX's own SPI2 bus (SLPIN stops the oscillator/
// booster/LC drive; panel RAM is retained so wake = SLPOUT and the old frame is
// back with no redraw). Using LGFX's bus — not a second HSPI SPIClass on the same
// pins — is what keeps the shared FSPI display bus intact across a wake.
void LGFXDisplay::panelSleep(bool sleep) {
  // Mirror the hardware-proven V4/T-Deck shim's datasheet timing — LovyanGFX's
  // setSleep writes the bare command with no delay: t_SLPIN wants 5 ms of bus
  // quiet after SLPIN, and >=5 ms must pass after SLPOUT before the next
  // command (the wake path fires backlight + LVGL flushes immediately after).
  if (sleep) { _lcd.sleep();  delay(5); }   // SLPIN
  else       { _lcd.wakeup(); delay(6); }   // SLPOUT
}

#endif  // HELTEC_LORA_V4_R8
