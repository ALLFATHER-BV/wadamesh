// SPDX-License-Identifier: GPL-3.0-or-later
#include "TDeckProDisplay.h"

#include <Arduino.h>
#include <cstring>

TDeckProDisplay::BusyHook TDeckProDisplay::_busy_hook = nullptr;

TDeckProDisplay::TDeckProDisplay()
    : DisplayDriver(WIDTH, HEIGHT),
      _canvas(nullptr),
      _epd(Panel(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BUSY)),
      _cst328(WIDTH, HEIGHT, &Wire, PIN_TOUCH_RST, PIN_TOUCH_INT) {}

bool TDeckProDisplay::begin() {
  const uint8_t selects[] = { PIN_TFT_CS, P_LORA_NSS, PIN_SD_CS };
  for (uint8_t pin : selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  _canvas.setPsram(true);
  _canvas.setColorDepth(16);
  if (!_canvas.createSprite(WIDTH, HEIGHT)) return false;
  _canvas.fillScreen(0xFFFF);

  _mono = (uint8_t*)ps_malloc(MONO_BYTES);
  _sent = (uint8_t*)ps_malloc(MONO_BYTES);
  if (!_mono) _mono = (uint8_t*)malloc(MONO_BYTES);
  if (!_sent) _sent = (uint8_t*)malloc(MONO_BYTES);
  if (!_mono || !_sent) return false;
  memset(_mono, 0xFF, MONO_BYTES);
  memset(_sent, 0xFF, MONO_BYTES);

  SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  _epd.init(115200, true, 2, false, SPI,
            SPISettings(2000000, MSBFIRST, SPI_MODE0));
  _epd.setRotation(0);
  _epd.epd2.setBusyCallback(&TDeckProDisplay::busyCallback);

  ledcSetup(TDECK_PRO_FRONTLIGHT_CHANNEL, 12000, 8);
  ledcAttachPin(PIN_TFT_LEDA_CTL, TDECK_PRO_FRONTLIGHT_CHANNEL);
  writeBrightness(0);

  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);
  resetTouch();
  _touch_is_cst3530 = probeCst3530();
  if (_touch_is_cst3530) {
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    _touch_ready = initCst3530();
  } else {
    _touch_ready = _cst328.begin();
    _cst328.setRotation(0);
  }

  _is_on = true;
  Serial.printf("[BOOT] T-Deck Pro e-paper ready touch=%s\n",
                _touch_ready ? (_touch_is_cst3530 ? "CST3530" : "CST328") : "missing");
  return true;
}

void TDeckProDisplay::turnOn() {
  _sleeping = false;
  _is_on = true;
  requestRefresh(true);
  writeBrightness(_brightness);
}

void TDeckProDisplay::turnOff() {
  writeBrightness(0);
  _epd.powerOff();
  _sleeping = true;
  _is_on = false;
}

void TDeckProDisplay::clear() {
  _canvas.fillScreen(0xFFFF);
  memset(_mono, 0xFF, MONO_BYTES);
  requestRefresh(true);
}

void TDeckProDisplay::startFrame(ColorVal background) {
  _canvas.fillScreen(background);
  _canvas.setTextColor(_color);
}

void TDeckProDisplay::setTextSize(int size) { _canvas.setTextSize(size); }
void TDeckProDisplay::setColor(ColorVal color) { _color = color; _canvas.setTextColor(color); }
void TDeckProDisplay::setCursor(int x, int y) { _canvas.setCursor(x, y); }
void TDeckProDisplay::print(const char* text) { _canvas.print(text ? text : ""); }
void TDeckProDisplay::fillRect(int x, int y, int w, int h) { _canvas.fillRect(x, y, w, h, _color); }
void TDeckProDisplay::drawRect(int x, int y, int w, int h) { _canvas.drawRect(x, y, w, h, _color); }
void TDeckProDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _canvas.drawXBitmap(x, y, bits, w, h, _color);
}
uint16_t TDeckProDisplay::getTextWidth(const char* text) { return (uint16_t)_canvas.textWidth(text ? text : ""); }

void TDeckProDisplay::endFrame() {
  canvasToMono();
  requestRefresh(true);
  serviceRefresh(true);
}

bool TDeckProDisplay::isDark(uint16_t color) {
  const uint16_t red = (uint16_t)(((color >> 11) & 0x1F) * 255u / 31u);
  const uint16_t green = (uint16_t)(((color >> 5) & 0x3F) * 255u / 63u);
  const uint16_t blue = (uint16_t)((color & 0x1F) * 255u / 31u);
  return ((red * 54u + green * 183u + blue * 19u) >> 8) < 144u;
}

void TDeckProDisplay::prepareMapTileRGB565(uint16_t* pixels, int width, int height) {
  if (!pixels || width <= 0 || height <= 0) return;

  // A fixed Bayer screen preserves mid-tone map regions without carrying
  // diffusion error across independently decoded tiles. Map tiles are 256 px,
  // a multiple of eight, so the pattern also remains continuous at tile edges.
  static constexpr uint8_t bayer[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 },
    {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 },
    {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 },
    {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 },
    {42, 26, 38, 22, 41, 25, 37, 21 },
  };
  constexpr int black_point = 72;
  constexpr int white_point = 246;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint16_t color = pixels[(size_t)y * width + x];
      const int red = (int)(((color >> 11) & 0x1F) * 255u / 31u);
      const int green = (int)(((color >> 5) & 0x3F) * 255u / 63u);
      const int blue = (int)((color & 0x1F) * 255u / 31u);
      int maximum = red > green ? red : green;
      if (blue > maximum) maximum = blue;
      int minimum = red < green ? red : green;
      if (blue < minimum) minimum = blue;

      // Hue differences carry map meaning even when their luminance is close.
      // Darkening saturated fills slightly keeps water, parks, and major roads
      // distinguishable from neutral land after conversion to one bit.
      int tone = ((red * 54 + green * 183 + blue * 19) >> 8) -
                 (maximum - minimum) / 5;
      bool black;
      if (tone <= black_point) {
        black = true;
      } else if (tone >= white_point) {
        black = false;
      } else {
        const int white_level =
            ((tone - black_point) * 64 + (white_point - black_point) / 2) /
            (white_point - black_point);
        black = bayer[y & 7][x & 7] >= white_level;
      }
      pixels[(size_t)y * width + x] = black ? 0x0000u : 0xFFFFu;
    }
  }
}

void TDeckProDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_mono || !pixels || w <= 0 || h <= 0) return;
  if (x >= 0 && y >= 0 && x + w <= WIDTH && y + h <= HEIGHT)
    _canvas.pushImage(x, y, w, h, const_cast<uint16_t*>(pixels));
  for (int row = 0; row < h; ++row) {
    const int py = y + row;
    if (py < 0 || py >= HEIGHT) continue;
    for (int col = 0; col < w; ++col) {
      const int px = x + col;
      if (px < 0 || px >= WIDTH) continue;
      uint8_t& destination = _mono[(size_t)py * (WIDTH / 8) + ((size_t)px >> 3)];
      const uint8_t mask = (uint8_t)(0x80u >> (px & 7));
      if (isDark(pixels[(size_t)row * w + col])) destination &= (uint8_t)~mask;
      else destination |= mask;
    }
  }
  requestRefresh(false);
}

void TDeckProDisplay::setDisplayRotation(uint8_t) { setLogicalSize(WIDTH, HEIGHT); }

void TDeckProDisplay::setBrightness(uint8_t brightness) {
  _brightness = brightness;
  if (_sent_valid && !_sleeping) writeBrightness(brightness);
}

void TDeckProDisplay::writeBrightness(uint8_t brightness) {
  ledcWrite(TDECK_PRO_FRONTLIGHT_CHANNEL, brightness);
}

void TDeckProDisplay::requestRefresh(bool full) {
  _refresh_pending = true;
  _full_refresh_pending = _full_refresh_pending || full;
}

void TDeckProDisplay::serviceRefresh(bool force) {
  if (!_refresh_pending || _sleeping || !_mono) return;
  const uint32_t now = millis();
  if (!force && _last_refresh_ms && now - _last_refresh_ms < 250) return;

  const bool full = _full_refresh_pending || _partial_refreshes >= 9;
  if (!full && _sent_valid && memcmp(_mono, _sent, MONO_BYTES) == 0) {
    _refresh_pending = false;
    return;
  }

  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);
  if (full) _epd.setFullWindow();
  else _epd.setPartialWindow(0, 0, WIDTH, HEIGHT);
  _epd.firstPage();
  do {
    _epd.drawInvertedBitmap(0, 0, _mono, WIDTH, HEIGHT, GxEPD_BLACK);
  } while (_epd.nextPage());
  _epd.powerOff();

  memcpy(_sent, _mono, MONO_BYTES);
  _sent_valid = true;
  writeBrightness(_brightness);
  _last_refresh_ms = millis();
  _refresh_pending = false;
  _full_refresh_pending = false;
  _partial_refreshes = full ? 0 : (uint8_t)(_partial_refreshes + 1);
}

void TDeckProDisplay::busyCallback(const void*) {
  if (_busy_hook) _busy_hook();
  else delay(1);
}

void TDeckProDisplay::canvasToMono() {
  memset(_mono, 0xFF, MONO_BYTES);
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      if (isDark((uint16_t)_canvas.readPixelValue(x, y)))
        _mono[(size_t)y * (WIDTH / 8) + ((size_t)x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
    }
  }
}

void TDeckProDisplay::resetTouch() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(80);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
}

bool TDeckProDisplay::probeCst3530() {
  const uint8_t command[] = { 0xD0, 0x03, 0x00, 0x00 };
  uint8_t response[7] = {};
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(command, sizeof(command));
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)sizeof(response)) == sizeof(response)) {
      Wire.readBytes(response, sizeof(response));
      if (response[2] == 0xCA && response[3] == 0xCA) return true;
    }
    const uint8_t wake[] = { 0xD0, 0x00, 0x04, 0x00 };
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(wake, sizeof(wake));
    (void)Wire.endTransmission();
    delay(50);
  }
  return false;
}

bool TDeckProDisplay::writeCst3530Command(uint32_t command) {
  const uint8_t bytes[] = {
    (uint8_t)(command >> 24), (uint8_t)(command >> 16),
    (uint8_t)(command >> 8), (uint8_t)command,
  };
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(bytes, sizeof(bytes));
  return Wire.endTransmission() == 0;
}

bool TDeckProDisplay::initCst3530() {
  bool ok = writeCst3530Command(0xD0000400);
  delay(20);
  ok = writeCst3530Command(0xD0000400) && ok;
  delay(20);
  ok = writeCst3530Command(0xD0000000) && ok;
  ok = writeCst3530Command(0xD0000C00) && ok;
  ok = writeCst3530Command(0xD0000100) && ok;
  return ok;
}

bool TDeckProDisplay::readCst3530(int16_t& x, int16_t& y) {
  const uint8_t read_command[] = { 0xD0, 0x07, 0x00, 0x00 };
  const uint8_t clear_command[] = { 0xD0, 0x00, 0x02, 0xAB };
  uint8_t response[50] = {};

  if (digitalRead(PIN_TOUCH_INT) != LOW) return false;
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(read_command, sizeof(read_command));
  if (Wire.endTransmission() != 0 || Wire.requestFrom((int)PIN_TOUCH_ADDR, 9) != 9) return false;
  size_t received = Wire.readBytes(response, 9);
  const uint8_t fingers = response[3] & 0x0F;
  const uint8_t keys = (response[3] >> 4) & 0x0F;
  const uint8_t total = (uint8_t)(fingers + keys);
  if (total > 1) {
    size_t extra = (size_t)(total - 1) * 5u;
    if (extra > sizeof(response) - received) extra = sizeof(response) - received;
    if (Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)extra) == extra)
      received += Wire.readBytes(response + received, extra);
  }
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(clear_command, sizeof(clear_command));
  (void)Wire.endTransmission();

  if (fingers == 0 || (response[8] >> 4) == 0) return false;
  const size_t index = (size_t)keys * 5u;
  if (index + 7 >= received) return false;
  x = (int16_t)(response[index + 4] | ((uint16_t)(response[index + 7] & 0x0F) << 8));
  y = (int16_t)(response[index + 5] | ((uint16_t)(response[index + 7] & 0xF0) << 4));
  return true;
}

bool TDeckProDisplay::getTouchPoint(uint16_t& x, uint16_t& y) {
  if (!_touch_ready) return false;
  int16_t raw_x = 0, raw_y = 0;
  if (_touch_is_cst3530) {
    if (!readCst3530(raw_x, raw_y)) return false;
  } else {
    if (_cst328.getTouches() == 0) return false;
    const CSE_TouchPoint point = _cst328.getPoint(0);
    raw_x = point.x;
    raw_y = point.y;
  }
  if (raw_x < 0 || raw_y < 0 || raw_x >= WIDTH || raw_y >= HEIGHT) return false;
  x = (uint16_t)raw_x;
  y = (uint16_t)raw_y;
  return true;
}