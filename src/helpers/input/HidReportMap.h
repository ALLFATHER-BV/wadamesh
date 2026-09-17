// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Keyboard reports described by a HID report descriptor (the "Report Map" of a
// Bluetooth LE keyboard).
//
// In report protocol a keyboard lays its reports out the way its own
// descriptor says: modifier bits, then a key array or a one-bit-per-key
// bitmap, often next to media-key or touchpad reports of other lengths. This
// finds the keyboard input reports and the Caps Lock / Num Lock LED bits of
// the output report, and turns an input report into the fixed boot layout the
// key decoder works with: one modifier byte and up to six key usages.
//
// Plain C++ without Arduino dependencies, so the host tests can run it.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct HidKeyboardReport {
  uint8_t  report_id;     // 0 when the descriptor uses no report ids
  int16_t  mod_bit;       // first of the 8 modifier bits (usages 0xE0-0xE7), -1 if absent
  int16_t  keys_bit;      // first bit of the key array, -1 if absent
  uint8_t  keys_count;    // entries in the key array
  uint8_t  keys_size;     // bits per entry
  int32_t  keys_lmin;     // entry values outside [keys_lmin, keys_lmax] mean "no key"
  int32_t  keys_lmax;
  uint16_t keys_umin;     // usage of the value keys_lmin
  int16_t  bitmap_bit;    // first bit of the key bitmap, -1 if absent
  uint16_t bitmap_first;  // usage of the bitmap's first bit
  uint16_t bitmap_count;  // bits in the bitmap
  uint16_t bits;          // input report length in bits, without the id
};

struct HidLedReport {
  uint8_t  report_id;
  int16_t  num_bit;       // Num Lock, -1 if absent
  int16_t  caps_bit;      // Caps Lock, -1 if absent
  uint16_t bits;          // output report length in bits, without the id
};

struct HidReportMapInfo {
  static constexpr int kMaxKeyboards = 4;
  HidKeyboardReport kbd[kMaxKeyboards];
  int               kbd_count;
  HidLedReport      leds;       // leds.caps_bit < 0 and leds.num_bit < 0: no LEDs
  bool              have_leds;
};

namespace hid_report_map_detail {

constexpr uint32_t kPageKeyboard = 0x07;
constexpr uint32_t kPageLeds = 0x08;
constexpr int kMaxIds = 16;
constexpr int kMaxUsages = 16;
constexpr int kMaxPush = 4;

struct Globals {
  uint32_t usage_page;
  int32_t  logical_min;
  int32_t  logical_max;
  uint32_t report_size;
  uint32_t report_count;
  uint8_t  report_id;
};

struct Locals {
  uint32_t usages[kMaxUsages];   // explicit usages, page included when given
  int      usage_count;
  uint32_t usage_min;
  uint32_t usage_max;
  bool     have_min;
  bool     have_max;
};

struct Offsets {
  uint8_t  id[kMaxIds];
  uint32_t in_bits[kMaxIds];
  uint32_t out_bits[kMaxIds];
  int      count;
};

inline int offsetSlot(Offsets& o, uint8_t id) {
  for (int i = 0; i < o.count; i++)
    if (o.id[i] == id) return i;
  if (o.count >= kMaxIds) return -1;
  o.id[o.count] = id;
  o.in_bits[o.count] = 0;
  o.out_bits[o.count] = 0;
  return o.count++;
}

// Usage of the i-th field of a main item, with its page in the upper 16 bits.
inline uint32_t fieldUsage(const Locals& l, const Globals& g, uint32_t i, bool* ok) {
  *ok = true;
  uint32_t u;
  if (l.have_min) {
    const uint32_t max = l.have_max ? l.usage_max : l.usage_min;
    u = l.usage_min + i;
    if (u > max) u = max;
  } else if (l.usage_count > 0) {
    u = l.usages[(int)i < l.usage_count ? (int)i : l.usage_count - 1];
  } else {
    *ok = false;
    return 0;
  }
  if (u <= 0xFFFF) u |= g.usage_page << 16;
  return u;
}

inline HidKeyboardReport* keyboardFor(HidReportMapInfo* out, uint8_t id) {
  for (int i = 0; i < out->kbd_count; i++)
    if (out->kbd[i].report_id == id) return &out->kbd[i];
  if (out->kbd_count >= HidReportMapInfo::kMaxKeyboards) return nullptr;
  HidKeyboardReport& k = out->kbd[out->kbd_count++];
  k.report_id = id;
  k.mod_bit = -1;
  k.keys_bit = -1;
  k.keys_count = 0;
  k.keys_size = 0;
  k.keys_lmin = 0;
  k.keys_lmax = 0;
  k.keys_umin = 0;
  k.bitmap_bit = -1;
  k.bitmap_first = 0;
  k.bitmap_count = 0;
  k.bits = 0;
  return &k;
}

inline void inputItem(HidReportMapInfo* out, const Globals& g, const Locals& l, uint32_t flags,
                      uint32_t bit) {
  const bool constant = (flags & 0x01) != 0;
  const bool variable = (flags & 0x02) != 0;
  if (constant || g.report_size == 0 || g.report_count == 0) return;
  if (bit > 0x7FFF) return;
  bool ok = false;
  const uint32_t first = fieldUsage(l, g, 0, &ok);
  if (!ok || (first >> 16) != kPageKeyboard) return;

  if (!variable) {
    // Key array: each entry holds a usage index.
    if (g.report_size > 16 || g.report_count > 32) return;
    HidKeyboardReport* k = keyboardFor(out, g.report_id);
    if (!k || k->keys_bit >= 0) return;
    k->keys_bit = (int16_t)bit;
    k->keys_count = (uint8_t)g.report_count;
    k->keys_size = (uint8_t)g.report_size;
    k->keys_lmin = g.logical_min;
    k->keys_lmax = g.logical_max;
    k->keys_umin = (uint16_t)(first & 0xFFFF);
    return;
  }
  if (g.report_size != 1) return;

  // One bit per usage: the modifier byte, or a whole key bitmap.
  const uint32_t first_usage = first & 0xFFFF;
  bool sequential = true;
  for (uint32_t i = 1; i < g.report_count && sequential; i++) {
    bool field_ok = false;
    const uint32_t u = fieldUsage(l, g, i, &field_ok);
    sequential = field_ok && (u & 0xFFFF) == first_usage + i && (u >> 16) == kPageKeyboard;
  }
  if (!sequential) return;
  HidKeyboardReport* k = keyboardFor(out, g.report_id);
  if (!k) return;
  if (first_usage == 0xE0 && g.report_count == 8) {
    if (k->mod_bit < 0) k->mod_bit = (int16_t)bit;
  } else if (k->bitmap_bit < 0 && g.report_count <= 0x400) {
    k->bitmap_bit = (int16_t)bit;
    k->bitmap_first = (uint16_t)first_usage;
    k->bitmap_count = (uint16_t)g.report_count;
  }
}

inline void outputItem(HidReportMapInfo* out, const Globals& g, const Locals& l, uint32_t flags,
                       uint32_t bit) {
  if ((flags & 0x01) || !(flags & 0x02) || g.report_size != 1) return;
  if (bit > 0x7FFF) return;
  for (uint32_t i = 0; i < g.report_count && i < 64; i++) {
    bool ok = false;
    const uint32_t u = fieldUsage(l, g, i, &ok);
    if (!ok || (u >> 16) != kPageLeds) continue;
    const uint32_t led = u & 0xFFFF;
    if (led != 0x01 && led != 0x02) continue;
    if (out->have_leds && out->leds.report_id != g.report_id) continue;
    if (!out->have_leds) {
      out->have_leds = true;
      out->leds.report_id = g.report_id;
      out->leds.num_bit = -1;
      out->leds.caps_bit = -1;
      out->leds.bits = 0;
    }
    if (led == 0x01) out->leds.num_bit = (int16_t)(bit + i);
    else             out->leds.caps_bit = (int16_t)(bit + i);
  }
}

inline void clearLocals(Locals& l) {
  l.usage_count = 0;
  l.have_min = false;
  l.have_max = false;
  l.usage_min = 0;
  l.usage_max = 0;
}

}  // namespace hid_report_map_detail

// Parses a report descriptor. Returns false when it describes no keyboard
// input report (or is malformed before one was found).
inline bool hidParseReportMap(const uint8_t* desc, size_t len, HidReportMapInfo* out) {
  using namespace hid_report_map_detail;
  memset(out, 0, sizeof *out);
  Globals g = {};
  Globals stack[kMaxPush];
  int depth = 0;
  Locals l = {};
  Offsets off = {};

  size_t i = 0;
  while (i < len) {
    const uint8_t prefix = desc[i];
    if (prefix == 0xFE) {   // long item: never used by keyboards, skip it
      if (i + 2 >= len) break;
      i += 3 + desc[i + 1];
      continue;
    }
    static constexpr uint8_t kSizes[4] = {0, 1, 2, 4};
    const size_t size = kSizes[prefix & 0x03];
    if (i + 1 + size > len) break;
    uint32_t value = 0;
    for (size_t b = 0; b < size; b++) value |= (uint32_t)desc[i + 1 + b] << (8 * b);
    int32_t svalue = (int32_t)value;
    if (size == 1) svalue = (int8_t)value;
    else if (size == 2) svalue = (int16_t)value;
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag = prefix >> 4;
    i += 1 + size;

    if (type == 0) {   // main
      int slot = offsetSlot(off, g.report_id);
      const uint32_t bits = g.report_size * g.report_count;
      if (tag == 0x8 && slot >= 0) {        // Input
        inputItem(out, g, l, value, off.in_bits[slot]);
        off.in_bits[slot] += bits;
      } else if (tag == 0x9 && slot >= 0) { // Output
        outputItem(out, g, l, value, off.out_bits[slot]);
        off.out_bits[slot] += bits;
      }
      clearLocals(l);
    } else if (type == 1) {   // global
      switch (tag) {
        case 0x0: g.usage_page = value; break;
        case 0x1: g.logical_min = svalue; break;
        // "Logical Maximum (255)" is often written as the single byte 0xFF;
        // with a non-negative minimum it can only mean the unsigned value.
        case 0x2: g.logical_max = g.logical_min >= 0 ? (int32_t)value : svalue; break;
        case 0x7: g.report_size = value; break;
        case 0x8: g.report_id = (uint8_t)value; break;
        case 0x9: g.report_count = value; break;
        case 0xA: if (depth < kMaxPush) stack[depth++] = g; break;
        case 0xB: if (depth > 0) g = stack[--depth]; break;
        default: break;
      }
    } else if (type == 2) {   // local
      switch (tag) {
        case 0x0:
          if (l.usage_count < kMaxUsages) l.usages[l.usage_count++] = size == 4 ? value : value & 0xFFFF;
          break;
        case 0x1: l.usage_min = size == 4 ? value & 0xFFFF : value; l.have_min = true; break;
        case 0x2: l.usage_max = size == 4 ? value & 0xFFFF : value; l.have_max = true; break;
        default: break;
      }
    }
  }

  // Report lengths, and drop entries that carry no keys (modifiers alone).
  int kept = 0;
  for (int k = 0; k < out->kbd_count; k++) {
    HidKeyboardReport r = out->kbd[k];
    if (r.keys_bit < 0 && r.bitmap_bit < 0) continue;
    for (int s = 0; s < off.count; s++)
      if (off.id[s] == r.report_id) r.bits = (uint16_t)off.in_bits[s];
    out->kbd[kept++] = r;
  }
  out->kbd_count = kept;
  if (out->have_leds) {
    for (int s = 0; s < off.count; s++)
      if (off.id[s] == out->leds.report_id) out->leds.bits = (uint16_t)off.out_bits[s];
  }
  return kept > 0;
}

inline uint32_t hidReportBits(const uint8_t* data, size_t len, uint32_t bit, uint32_t count) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < count && i < 32; i++) {
    const uint32_t b = bit + i;
    if (b / 8 >= len) break;
    if (data[b / 8] & (1u << (b % 8))) v |= 1u << i;
  }
  return v;
}

// Turns one input report (without its id) into boot form. Returns false when
// the report carries no usable key state: too short, or a rollover error.
inline bool hidDecodeKeyboard(const HidKeyboardReport& r, const uint8_t* data, size_t len,
                              uint8_t* mod, uint8_t keys[6]) {
  *mod = 0;
  memset(keys, 0, 6);
  const uint32_t have_bits = (uint32_t)len * 8;
  int n = 0;
  auto add = [&](int32_t usage) -> bool {
    if (usage >= 0x01 && usage <= 0x03) return false;   // ErrorRollOver, POSTFail, ErrorUndefined
    if (usage >= 0xE0 && usage <= 0xE7) {
      *mod |= (uint8_t)(1u << (usage - 0xE0));
    } else if (usage >= 0x04 && usage <= 0xFF && n < 6) {
      bool dup = false;
      for (int j = 0; j < n; j++) dup |= keys[j] == (uint8_t)usage;
      if (!dup) keys[n++] = (uint8_t)usage;
    }
    return true;
  };

  if (r.mod_bit >= 0) {
    if ((uint32_t)r.mod_bit + 8 > have_bits) return false;
    *mod = (uint8_t)hidReportBits(data, len, (uint32_t)r.mod_bit, 8);
  }
  if (r.keys_bit >= 0) {
    if ((uint32_t)r.keys_bit + (uint32_t)r.keys_count * r.keys_size > have_bits) return false;
    for (uint32_t i = 0; i < r.keys_count; i++) {
      const int32_t v =
          (int32_t)hidReportBits(data, len, (uint32_t)r.keys_bit + i * r.keys_size, r.keys_size);
      if (v < r.keys_lmin || v > r.keys_lmax) continue;
      const int32_t usage = (int32_t)r.keys_umin + (v - r.keys_lmin);
      if (usage == 0) continue;
      if (!add(usage)) return false;
    }
  }
  if (r.bitmap_bit >= 0) {
    if ((uint32_t)r.bitmap_bit + r.bitmap_count > have_bits) return false;
    for (uint32_t i = 0; i < r.bitmap_count; i++) {
      const uint32_t b = (uint32_t)r.bitmap_bit + i;
      if (!(data[b / 8] & (1u << (b % 8)))) continue;
      if (!add((int32_t)(r.bitmap_first + i))) return false;
    }
  }
  return true;
}

// Fills an output report (without its id) with the LED state; returns its
// length in bytes, 0 when it does not fit `cap`.
inline size_t hidEncodeLeds(const HidLedReport& r, bool num, bool caps, uint8_t* buf, size_t cap) {
  size_t n = (r.bits + 7) / 8;
  if (n == 0) n = 1;
  if (n > cap) return 0;
  memset(buf, 0, n);
  if (num && r.num_bit >= 0 && (size_t)(r.num_bit / 8) < n)
    buf[r.num_bit / 8] |= (uint8_t)(1u << (r.num_bit % 8));
  if (caps && r.caps_bit >= 0 && (size_t)(r.caps_bit / 8) < n)
    buf[r.caps_bit / 8] |= (uint8_t)(1u << (r.caps_bit % 8));
  return n;
}
