// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Direct-to-panel drawing for the T-Display P4's two DSI panels (RM69A10 AMOLED, HI8561 LCD).
//
// LVGL owns these panels almost all the time, and it reaches them only through
// writePixelsRGB565(), which takes HALF-resolution LVGL coordinates and upscales 2x. The
// rest of the DisplayDriver API (fills, rectangles, text) used to be empty stubs. That was
// harmless until something drew WITHOUT LVGL: Remote UI mode renders the UI off-screen for
// the browser and paints a status placeholder on the physical panel through this API. On
// the P4 the text and boxes vanished, and the logo landed at 2x its coordinates, which is
// the "misdraw in the top-right corner, nothing else" report.
//
// This class implements that API at the panel's NATIVE resolution (width()/height()),
// writing through writeNativeRGB565(), which each panel implements without the LVGL
// upscale. Text is rasterised from LVGL's built-in Montserrat fonts (already linked for
// the UI), drawn at 2x like the rest of the UI on these panels:
//   text size 1 -> Montserrat 14 at 2x (28 px, matching the UI's body text)
//   text size 2 -> Montserrat 28 at 2x (56 px titles)
// There is no framebuffer readback, so glyph edges are blended against the colour of the
// last fill under the text (or the frame background), which covers every caller today.
#include <stdint.h>
#include <string.h>
#include <helpers/ui/DisplayDriver.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

class P4PanelPainter : public DisplayDriver {
protected:
  P4PanelPainter(int w, int h) : DisplayDriver(w, h) {}

  // Blit native-resolution RGB565 (no LVGL upscale). Implemented by each panel driver.
  virtual void writeNativeRGB565(int x, int y, int w, int h, const uint16_t* px) = 0;

private:
  static constexpr int kScale = 2;          // glyph pixel -> kScale x kScale panel pixels
  static constexpr int kStripRows = 32;     // fill granularity (one reusable buffer)
  ColorVal _color = 0xFFFF;
  ColorVal _frame_bg = 0x0000;
  int _text_size = 1;
  int _cx = 0, _cy = 0;
  // Last solid fill, so text drawn on top of it blends against the right colour.
  int _fx = 0, _fy = 0, _fw = 0, _fh = 0;
  ColorVal _fill_bg = 0x0000;
  uint16_t* _buf = nullptr;
  size_t _buf_px = 0;

  // Internal DMA RAM first, PSRAM as the fallback: see the RM69A10 flush note on why a
  // PSRAM scratch competes with the DSI framebuffer stream.
  uint16_t* scratch(size_t px) {
    if (px > _buf_px) {
      if (_buf) heap_caps_free(_buf);
      _buf = (uint16_t*)heap_caps_malloc(px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
      if (!_buf) _buf = (uint16_t*)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
      _buf_px = _buf ? px : 0;
    }
    return _buf;
  }

  const lv_font_t* font() const {
    return _text_size >= 2 ? &lv_font_montserrat_28 : &lv_font_montserrat_14;
  }

  static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {   // a: 0..255
    const uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
    const uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    const uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
  }

  static uint8_t glyphAlpha(const uint8_t* bmp, uint8_t bpp, uint32_t idx) {
    const uint32_t bit = idx * bpp;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t shift = (uint8_t)(8 - bpp - (bit & 7));
    const uint8_t v = (uint8_t)((byte >> shift) & ((1u << bpp) - 1));
    switch (bpp) {
      case 1: return v ? 255 : 0;
      case 2: return (uint8_t)(v * 85);
      case 4: return (uint8_t)(v * 17);
      default: return v;
    }
  }

  // Clip to the panel and blit rect of one colour, in strips.
  void fillNative(int x, int y, int w, int h, ColorVal c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width())  w = width() - x;
    if (y + h > height()) h = height() - y;
    if (w <= 0 || h <= 0) return;
    const int rows = h < kStripRows ? h : kStripRows;
    uint16_t* b = scratch((size_t)w * rows);
    if (!b) return;
    for (size_t i = 0; i < (size_t)w * rows; i++) b[i] = c;
    for (int yy = y; yy < y + h; yy += rows) {
      const int n = (y + h - yy) < rows ? (y + h - yy) : rows;
      writeNativeRGB565(x, yy, w, n, b);
    }
  }

  ColorVal bgAt(int x, int y) const {
    if (_fw > 0 && x >= _fx && x < _fx + _fw && y >= _fy && y < _fy + _fh) return _fill_bg;
    return _frame_bg;
  }

public:
  void clear() override { startFrame((ColorVal)0x0000); }
  void startFrame(ColorVal bkg) override {
    _frame_bg = bkg;
    _fw = 0;
    fillNative(0, 0, width(), height(), bkg);
  }
  void setTextSize(int sz) override { _text_size = sz < 1 ? 1 : sz; }
  void setColor(ColorVal c) override { _color = c; }
  void setCursor(int x, int y) override { _cx = x; _cy = y; }
  void fillRect(int x, int y, int w, int h) override {
    _fx = x; _fy = y; _fw = w; _fh = h; _fill_bg = _color;
    fillNative(x, y, w, h, _color);
  }
  void drawRect(int x, int y, int w, int h) override {
    const int t = kScale;   // outline weight matches the 2x UI
    fillNative(x, y, w, t, _color);
    fillNative(x, y + h - t, w, t, _color);
    fillNative(x, y, t, h, _color);
    fillNative(x + w - t, y, t, h, _color);
  }
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override {
    // 1-bpp XBM (LSB first, rows byte-padded), set bits in the current colour.
    if (!bits || w <= 0 || h <= 0) return;
    uint16_t* b = scratch((size_t)w * h);
    if (!b) return;
    const ColorVal bg = bgAt(x, y);
    const int stride = (w + 7) / 8;
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++)
        b[r * w + c] = ((bits[r * stride + c / 8] >> (c & 7)) & 1) ? _color : bg;
    writeNativeRGB565(x, y, w, h, b);
  }
  uint16_t getTextWidth(const char* str) override {
    if (!str) return 0;
    const lv_font_t* f = font();
    uint32_t i = 0, w = 0;
    while (str[i]) {
      const uint32_t cp = _lv_txt_encoded_next(str, &i);
      w += lv_font_get_glyph_width(f, cp, 0);   // same advance print() uses (no kerning)
    }
    return (uint16_t)(w * kScale);
  }
  void print(const char* str) override {
    if (!str || !str[0]) return;
    const lv_font_t* f = font();
    const int tw = getTextWidth(str);
    const int th = lv_font_get_line_height(f) * kScale;
    if (tw <= 0 || th <= 0) return;
    uint16_t* b = scratch((size_t)tw * th);
    if (!b) return;
    const ColorVal bg = bgAt(_cx, _cy);
    for (size_t i = 0; i < (size_t)tw * th; i++) b[i] = bg;
    const int baseline = lv_font_get_line_height(f) - f->base_line;   // glyph-space y of the baseline
    int pen = 0;
    uint32_t i = 0;
    while (str[i]) {
      const uint32_t cp = _lv_txt_encoded_next(str, &i);
      lv_font_glyph_dsc_t g;
      if (!lv_font_get_glyph_dsc(f, &g, cp, 0)) continue;
      const uint8_t* bmp = lv_font_get_glyph_bitmap(f, cp);
      if (bmp && g.box_w && g.box_h) {
        const int gx0 = pen + g.ofs_x;
        const int gy0 = baseline - g.box_h - g.ofs_y;
        for (int gy = 0; gy < g.box_h; gy++) {
          for (int gx = 0; gx < g.box_w; gx++) {
            const uint8_t a = glyphAlpha(bmp, g.bpp, (uint32_t)(gy * g.box_w + gx));
            if (!a) continue;
            const uint16_t px = blend565(_color, bg, a);
            for (int sy = 0; sy < kScale; sy++) {
              const int yy = (gy0 + gy) * kScale + sy;
              if (yy < 0 || yy >= th) continue;
              for (int sx = 0; sx < kScale; sx++) {
                const int xx = (gx0 + gx) * kScale + sx;
                if (xx >= 0 && xx < tw) b[yy * tw + xx] = px;
              }
            }
          }
        }
      }
      pen += g.adv_w;
    }
    // Off-panel text is dropped rather than clipped; every caller lays text out inside it.
    if (_cx < 0 || _cy < 0 || _cx + tw > width() || _cy + th > height()) return;
    writeNativeRGB565(_cx, _cy, tw, th, b);
    _cx += tw;
  }
};
