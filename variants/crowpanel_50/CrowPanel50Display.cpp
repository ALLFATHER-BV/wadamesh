// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrowPanel50Display.h"

#include <Arduino.h>
#include <Wire.h>

// Bus pins, timings and the board controller follow Elecrow's 5.0" factory code
// (LovyanGFX_Driver.h, lesson-05) and Meshtastic's LGFX_ELECROW70, as collected in
// Camillia-MT's hw_crowpanel_50.h / crowpanel_50_display.h.
namespace {
constexpr int kPanelW = 800, kPanelH = 480;
constexpr int kTouchSda = 15, kTouchScl = 16, kTouchInt = 1;

// Pixel clock. Elecrow's factory code runs 21 MHz; Meshtastic and Camillia-MT
// run 14 MHz to leave PSRAM bandwidth for LVGL, WiFi and the radio. 12 MHz is
// too slow: the panel drops into its own colour-cycling self-test, which only a
// power cycle clears. The panel timings can be overridden with -D for bring-up
// on other panel revisions.
#ifndef CROWPANEL50_PCLK_HZ
#define CROWPANEL50_PCLK_HZ 14000000
#endif
// Clock edge (LovyanGFX's pclk_idle_high sets the edge the data changes on).
// Elecrow's 1 is right: with 0 the data changes as the panel samples it, so
// single-pixel detail goes dark and gradients pick up stray lines.
#ifndef CROWPANEL50_PCLK_IDLE_HIGH
#define CROWPANEL50_PCLK_IDLE_HIGH 1
#endif
#ifndef CROWPANEL50_HPORCH
#define CROWPANEL50_HPORCH 8
#endif
constexpr uint32_t kPclkHz = CROWPANEL50_PCLK_HZ;

// V1.1+ board MCU: single-byte commands at 0x30. 0 brightest .. 244 dimmest,
// 245 backlight off, 250 wakes the touch panel (V1.2/V1.3 values).
constexpr uint8_t kMcuAddr = 0x30;
constexpr uint8_t kMcuBlMax = 0, kMcuBlMin = 244, kMcuBlOff = 245, kMcuTouchOn = 250;

// V1.0 PCA9557 (TCA9534 register layout).
constexpr uint8_t kIoexAddr = 0x18;
constexpr uint8_t kIoexBl = 1, kIoexTpRst = 2, kIoexAmpMute = 3, kIoexAmpShut = 4;
constexpr uint8_t kRegOutput = 0x01, kRegConfig = 0x03;
inline uint8_t ioBit(uint8_t pin) { return (uint8_t)(1u << pin); }
}  // namespace

// ---- Board controller -------------------------------------------------------

bool CrowPanel50BoardCtl::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

CrowPanel50BoardCtl::Kind CrowPanel50BoardCtl::begin() {
  if (probe(kIoexAddr)) {
    _kind = PCA9557;
    // Levels first, then direction, so no pin glitches on the way to output.
    // Elecrow's start-up state: backlight on, touch out of reset, amplifier off.
    _out = ioBit(kIoexBl) | ioBit(kIoexTpRst) | ioBit(kIoexAmpShut);
    pcaWrite(kRegOutput, _out);
    pcaWrite(kRegConfig, (uint8_t)~(ioBit(kIoexBl) | ioBit(kIoexTpRst) | ioBit(kIoexAmpMute) | ioBit(kIoexAmpShut)));
    Serial.printf("[crowpanel-50] V1.0 board: PCA9557 at 0x%02X\n", kIoexAddr);
    if (P_LORA_NSS != 0)
      Serial.println("[crowpanel-50] V1.0 boards wire the radio's CS to GPIO0; this build uses "
                     "GPIO8 (V1.2+), so the radio will not answer");
  } else if (probe(kMcuAddr)) {
    _kind = MCU;
    Serial.printf("[crowpanel-50] V1.1+ board: MCU at 0x%02X\n", kMcuAddr);
  } else {
    _kind = NONE;
    Serial.printf("[crowpanel-50] no backlight controller at 0x%02X or 0x%02X\n", kIoexAddr, kMcuAddr);
  }
  return _kind;
}

// GT911 address strap: INT held low through the reset gives 0x5D. Elecrow's V1.2
// code repeats the MCU's wake until the GT911 answers, so this does too, a few
// times. If the strap is missed it comes up at 0x14 instead; either is accepted.
uint8_t CrowPanel50BoardCtl::resetTouch() {
  if (_kind == NONE) return probe(0x5D) ? 0x5D : (probe(0x14) ? 0x14 : 0);
  for (int attempt = 1; attempt <= 5; ++attempt) {
    pinMode(kTouchInt, OUTPUT);
    digitalWrite(kTouchInt, LOW);
    if (_kind == PCA9557) {
      pcaSet(kIoexTpRst, false);
      delay(20);
      pcaSet(kIoexTpRst, true);
      delay(100);
    } else {
      mcuSendAlways(kMcuTouchOn);
      delay(120);
    }
    pinMode(kTouchInt, INPUT);
    delay(100);
    const bool at5d = probe(0x5D), at14 = probe(0x14);
    if (at5d || at14) {
      Serial.printf("[crowpanel-50] GT911 at 0x%02X after %d attempt%s\n",
                    at5d ? 0x5D : 0x14, attempt, attempt == 1 ? "" : "s");
      return at5d ? 0x5D : 0x14;
    }
  }
  Serial.println("[crowpanel-50] GT911 did not answer at 0x5D or 0x14");
  return 0;
}

// 0 is off. V1.0's backlight only switches; the MCU dims (255 brightest). Below
// full the MCU's PWM can make the backlight driver whine, so 100% is steady DC.
void CrowPanel50BoardCtl::setBacklight(uint8_t level) {
  if (_kind == PCA9557) {
    pcaSet(kIoexBl, level > 0);
  } else if (_kind == MCU) {
    if (level == 0) { mcuSend(kMcuBlOff); return; }
    const uint8_t span = kMcuBlMin - kMcuBlMax;
    mcuSend((uint8_t)(kMcuBlMin - (uint32_t)(level - 1) * span / 254));
  }
}

void CrowPanel50BoardCtl::pcaSet(uint8_t pin, bool high) {
  const uint8_t next = high ? (uint8_t)(_out | ioBit(pin)) : (uint8_t)(_out & ~ioBit(pin));
  if (next == _out) return;
  _out = next;
  pcaWrite(kRegOutput, _out);
}

void CrowPanel50BoardCtl::pcaWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kIoexAddr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void CrowPanel50BoardCtl::mcuSend(uint8_t cmd) {
  if (cmd == _lastMcu) return;
  if (mcuSendAlways(cmd)) _lastMcu = cmd;
}

bool CrowPanel50BoardCtl::mcuSendAlways(uint8_t cmd) {
  Wire.beginTransmission(kMcuAddr);
  Wire.write(cmd);
  return Wire.endTransmission() == 0;
}

// ---- Display ------------------------------------------------------------------

CrowPanel50Display::CrowPanel50Display() : DisplayDriver(kPanelW, kPanelH) {
  {
    auto cfg = _panel.config();
    cfg.memory_width = kPanelW;
    cfg.memory_height = kPanelH;
    cfg.panel_width = kPanelW;
    cfg.panel_height = kPanelH;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 0;
    cfg.readable = true;   // the framebuffer is the only copy of the screen
    _panel.config(cfg);
  }
  {
    auto cfg = _panel.config_detail();
    cfg.use_psram = 1;     // 800x480x2 = 750 KB
    _panel.config_detail(cfg);
  }
  {
    auto cfg = _bus.config();
    cfg.panel = &_panel;
    cfg.pin_d0 = GPIO_NUM_21;   // B0
    cfg.pin_d1 = GPIO_NUM_47;   // B1
    cfg.pin_d2 = GPIO_NUM_48;   // B2
    cfg.pin_d3 = GPIO_NUM_45;   // B3
    cfg.pin_d4 = GPIO_NUM_38;   // B4
    cfg.pin_d5 = GPIO_NUM_9;    // G0
    cfg.pin_d6 = GPIO_NUM_10;   // G1
    cfg.pin_d7 = GPIO_NUM_11;   // G2
    cfg.pin_d8 = GPIO_NUM_12;   // G3
    cfg.pin_d9 = GPIO_NUM_13;   // G4
    cfg.pin_d10 = GPIO_NUM_14;  // G5
    cfg.pin_d11 = GPIO_NUM_7;   // R0
    cfg.pin_d12 = GPIO_NUM_17;  // R1
    cfg.pin_d13 = GPIO_NUM_18;  // R2
    cfg.pin_d14 = GPIO_NUM_3;   // R3
    cfg.pin_d15 = GPIO_NUM_46;  // R4
    cfg.pin_henable = GPIO_NUM_42;
    cfg.pin_vsync = GPIO_NUM_41;
    cfg.pin_hsync = GPIO_NUM_40;
    cfg.pin_pclk = GPIO_NUM_39;
    cfg.freq_write = kPclkHz;
    cfg.hsync_polarity = 0;
    cfg.hsync_front_porch = CROWPANEL50_HPORCH;
    cfg.hsync_pulse_width = 4;
    cfg.hsync_back_porch = CROWPANEL50_HPORCH;
    cfg.vsync_polarity = 0;
    cfg.vsync_front_porch = 8;
    cfg.vsync_pulse_width = 4;
    cfg.vsync_back_porch = 8;
    cfg.pclk_idle_high = CROWPANEL50_PCLK_IDLE_HIGH;
    _bus.config(cfg);
  }
  _panel.setBus(&_bus);

  _light.setController(&_ctl);
  _panel.setLight(&_light);

  {
    auto cfg = _touch.config();
    cfg.x_min = 0;
    cfg.x_max = kPanelW - 1;
    cfg.y_min = 0;
    cfg.y_max = kPanelH - 1;
    cfg.pin_int = -1;        // polled; INT is only the address strap
    cfg.pin_rst = -1;        // reset belongs to the board controller (resetTouch)
    cfg.bus_shared = true;   // the controller is on the same I2C pins
    cfg.offset_rotation = 0;
    cfg.i2c_port = 0;
    cfg.i2c_addr = 0x5D;
    cfg.pin_sda = kTouchSda;
    cfg.pin_scl = kTouchScl;
    cfg.freq = 400000;
    _touch.config(cfg);
    _panel.setTouch(&_touch);
  }
  _lcd.setPanel(&_panel);
}

bool CrowPanel50Display::begin() {
  if (_isOn) return true;
  // board.begin() has opened Wire on GPIO15/16 already.
  _ctl.begin();
  _touchAddr = _ctl.resetTouch();
  if (_touchAddr) {
    auto cfg = _touch.config();
    cfg.i2c_addr = _touchAddr;
    _touch.config(cfg);
  }
  if (!_lcd.init()) {
    Serial.println("[crowpanel-50] RGB panel init failed");
    return false;
  }
  _lcd.setSwapBytes(true);
  setDisplayRotation(3);
  _lcd.fillScreen(0);
  // LovyanGFX's touch init reprograms I2C0 under Wire; take the bus back for the
  // backlight controller and the RTC probe, as the CrowPanel 3.5 does.
  Wire.end();
  if (!Wire.begin(kTouchSda, kTouchScl, 400000)) {
    Serial.println("[crowpanel-50] I2C bus init failed");
    return false;
  }
  _ctl.setBacklight(_brightness);
#ifdef CROWPANEL50_TESTPATTERN
  // Bring-up aid: colour ramps straight to the panel, no LVGL. Clean, even steps mean
  // the bus is fine; stray lines or flecks mean data bits are mis-sampled.
  {
    const int w = _lcd.width(), bh = _lcd.height() / 6;
    for (int x = 0; x < w; ++x) {
      const uint32_t r5 = x * 32 / w, g6 = x * 64 / w;
      _lcd.fillRect(x, 0 * bh, 1, bh, (uint16_t)(r5 << 11));
      _lcd.fillRect(x, 1 * bh, 1, bh, (uint16_t)(g6 << 5));
      _lcd.fillRect(x, 2 * bh, 1, bh, (uint16_t)r5);
      _lcd.fillRect(x, 3 * bh, 1, bh, (uint16_t)((r5 << 11) | (g6 << 5) | r5));
      _lcd.fillRect(x, 4 * bh, 1, bh, (uint16_t)((x & 1) ? 0xFFFF : 0x0000));
      const uint8_t r = 0x2D + (0xF4 - 0x2D) * x / w, g = 0xD4 + (0x72 - 0xD4) * x / w,
                    b = 0xBF + (0xB6 - 0xBF) * x / w;
      _lcd.fillRect(x, 5 * bh, 1, _lcd.height() - 5 * bh, _lcd.color565(r, g, b));
    }
    Serial.println("[crowpanel-50] test pattern (15 s)");
    delay(15000);
  }
#endif
  _isOn = true;
  Serial.printf("[crowpanel-50] display %dx%d, pclk %lu Hz edge %d hporch %d, touch %s\n",
                width(), height(), (unsigned long)kPclkHz, CROWPANEL50_PCLK_IDLE_HIGH,
                CROWPANEL50_HPORCH, _lcd.touch() ? "up" : "not found");
  return true;
}

void CrowPanel50Display::turnOn() {
  if (!_isOn) { _ctl.setBacklight(_brightness); _isOn = true; }
}
void CrowPanel50Display::turnOff() {
  if (_isOn) { _ctl.setBacklight(0); _isOn = false; }
}
void CrowPanel50Display::clear() { _lcd.fillScreen(0); }

// The UI's portrait is 0 and its upright landscape 3 (ROT_270), as on the 3.5.
// The panel itself is native 800x480 landscape at LovyanGFX rotation 0, so map
// 3 to that, 1 to landscape turned 180 degrees, 0 to portrait and 2 to portrait
// turned 180 degrees. LovyanGFX rotates the framebuffer writes and the GT911
// coordinates together.
void CrowPanel50Display::setDisplayRotation(uint8_t rotation) {
  uint8_t r;
  switch (rotation) {
    case 3:  r = 0; break;
    case 1:  r = 2; break;
    case 2:  r = 3; break;
    default: r = 1; break;
  }
  _lcd.setRotation(r);
  setLogicalSize(_lcd.width(), _lcd.height());
}

void CrowPanel50Display::startFrame(ColorVal bkg) { _lcd.fillScreen(bkg); }
void CrowPanel50Display::setTextSize(int sz) { _lcd.setTextSize(sz); }
void CrowPanel50Display::setColor(ColorVal c) { _color = c; _lcd.setTextColor(c); }
void CrowPanel50Display::setCursor(int x, int y) { _lcd.setCursor(x, y); }
void CrowPanel50Display::print(const char* str) { _lcd.print(str); }
void CrowPanel50Display::fillRect(int x, int y, int w, int h) { _lcd.fillRect(x, y, w, h, _color); }
void CrowPanel50Display::drawRect(int x, int y, int w, int h) { _lcd.drawRect(x, y, w, h, _color); }
void CrowPanel50Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _lcd.drawXBitmap(x, y, bits, w, h, _color);
}
uint16_t CrowPanel50Display::getTextWidth(const char* str) { return _lcd.textWidth(str); }
void CrowPanel50Display::endFrame() {}

void CrowPanel50Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  _lcd.startWrite();
  _lcd.setAddrWindow(x, y, w, h);
  _lcd.writePixels(const_cast<uint16_t*>(pixels), (uint32_t)w * h);
  _lcd.endWrite();
}

void CrowPanel50Display::setBrightness(uint8_t value) {
  if (value) _brightness = value;
  _ctl.setBacklight(value);
}

// Reads the GT911 through Wire rather than LovyanGFX: Wire takes a lock, so the
// background touch task can share the bus with the backlight controller, and
// LovyanGFX drops a pending report whenever reads are more than 128 ms apart.
// Between reports the GT911 holds its last state, so that is what is returned.
bool CrowPanel50Display::getTouchPoint(uint16_t& x, uint16_t& y) {
  if (!_touchAddr) return false;
  uint8_t buf[9] = {0};
  Wire.beginTransmission(_touchAddr);
  Wire.write(0x81);
  Wire.write(0x4E);
  // A STOP, not a repeated start: Wire tracks a repeated start in shared fields
  // that are not task-safe, and loopTask's RTC reads use one.
  bool ok = Wire.endTransmission() == 0 &&
            Wire.requestFrom(_touchAddr, (uint8_t)sizeof(buf)) == sizeof(buf);
  if (ok) {
    for (auto& b : buf) b = Wire.read();
  }
  if (ok && (buf[0] & 0x80)) {
    const uint8_t points = buf[0] & 0x0F;
    if (points > 0 && points <= 5) {
      lgfx::touch_point_t tp;
      tp.id = buf[1];
      tp.x = (int16_t)(buf[2] | (buf[3] << 8));
      tp.y = (int16_t)(buf[4] | (buf[5] << 8));
      tp.size = (uint16_t)(buf[6] | (buf[7] << 8));
      _lcd.convertRawXY(&tp, 1);
      _touchX = (uint16_t)std::max<int>(0, tp.x);
      _touchY = (uint16_t)std::max<int>(0, tp.y);
      _touchDown = true;
    } else {
      _touchDown = false;
    }
    Wire.beginTransmission(_touchAddr);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0x00);
    Wire.endTransmission();
  }
  if (!_touchDown) return false;
  x = _touchX;
  y = _touchY;
  return true;
}
