// SPDX-License-Identifier: GPL-3.0-or-later
//
// HID-usage maps for the external Bluetooth keyboard. See BleKeyMap.h.
//
// The non-Bulgarian tables are the ones the Bluetooth keyboard shipped with;
// kBG follows the traditional Bulgarian phonetic layout (the one where q is
// я), with the four letters the 26 Latin positions cannot hold on the
// punctuation keys a real keyboard has: ` = ч, [ = ш, ] = щ, \ = ю.

#include "BleKeyMap.h"
#include "device_caps.h"

#if CAP_BLE_KEYBOARD

namespace BleKeyMap {
namespace {

constexpr uint32_t DK = 0x8000;   // table flag: dead key (accent waits for the next key)

struct KeyMap { uint8_t usage; uint32_t base, shift, altgr; };

// US: every non-letter key. Other layouts list what differs; unlisted
// non-letters fall back to this table and letters to their US position.
constexpr KeyMap kUS[] = {
  {0x1E,'1','!',0}, {0x1F,'2','@',0}, {0x20,'3','#',0}, {0x21,'4','$',0}, {0x22,'5','%',0},
  {0x23,'6','^',0}, {0x24,'7','&',0}, {0x25,'8','*',0}, {0x26,'9','(',0}, {0x27,'0',')',0},
  {0x2D,'-','_',0}, {0x2E,'=','+',0}, {0x2F,'[','{',0}, {0x30,']','}',0}, {0x31,'\\','|',0},
  {0x32,'\\','|',0}, {0x33,';',':',0}, {0x34,'\'','"',0}, {0x35,'`','~',0}, {0x36,',','<',0},
  {0x37,'.','>',0}, {0x38,'/','?',0}, {0x64,'\\','|',0},
};

constexpr KeyMap kUK[] = {
  {0x1F,'2','"',0}, {0x20,'3',0xA3,0}, {0x21,'4','$',0x20AC},
  {0x31,'#','~',0}, {0x32,'#','~',0}, {0x34,'\'','@',0}, {0x35,'`',0xAC,0xA6},
  {0x64,'\\','|',0},
};

constexpr KeyMap kDE[] = {
  {0x1C,'z','Z',0}, {0x1D,'y','Y',0},
  {0x14,'q','Q','@'}, {0x08,'e','E',0x20AC}, {0x10,'m','M',0xB5},
  {0x1E,'1','!',0}, {0x1F,'2','"',0xB2}, {0x20,'3',0xA7,0xB3}, {0x21,'4','$',0},
  {0x22,'5','%',0}, {0x23,'6','&',0}, {0x24,'7','/','{'}, {0x25,'8','(','['},
  {0x26,'9',')',']'}, {0x27,'0','=','}'},
  {0x2D,0xDF,'?','\\'}, {0x2E,DK|0xB4,DK|'`',0}, {0x2F,0xFC,0xDC,0}, {0x30,'+','*','~'},
  {0x31,'#','\'',0}, {0x32,'#','\'',0}, {0x33,0xF6,0xD6,0}, {0x34,0xE4,0xC4,0},
  {0x35,DK|'^',0xB0,0}, {0x36,',',';',0}, {0x37,'.',':',0}, {0x38,'-','_',0},
  {0x64,'<','>','|'},
};

constexpr KeyMap kFR[] = {
  {0x04,'q','Q',0}, {0x14,'a','A',0}, {0x1A,'z','Z',0}, {0x1D,'w','W',0}, {0x33,'m','M',0},
  {0x08,'e','E',0x20AC},
  {0x10,',','?',0}, {0x36,';','.',0}, {0x37,':','/',0}, {0x38,'!',0xA7,0},
  {0x1E,'&','1',0}, {0x1F,0xE9,'2',DK|'~'}, {0x20,'"','3','#'}, {0x21,'\'','4','{'},
  {0x22,'(','5','['}, {0x23,'-','6','|'}, {0x24,0xE8,'7',DK|'`'}, {0x25,'_','8','\\'},
  {0x26,0xE7,'9','^'}, {0x27,0xE0,'0','@'},
  {0x2D,')',0xB0,']'}, {0x2E,'=','+','}'}, {0x2F,DK|'^',DK|0xA8,0}, {0x30,'$',0xA3,0xA4},
  {0x31,'*',0xB5,0}, {0x32,'*',0xB5,0}, {0x34,0xF9,'%',0}, {0x35,0xB2,0,0},
  {0x64,'<','>',0},
};

constexpr KeyMap kBE[] = {
  {0x04,'q','Q',0}, {0x14,'a','A',0}, {0x1A,'z','Z',0}, {0x1D,'w','W',0}, {0x33,'m','M',0},
  {0x08,'e','E',0x20AC},
  {0x10,',','?',0}, {0x36,';','.',0}, {0x37,':','/',0}, {0x38,'=','+',DK|'~'},
  {0x1E,'&','1','|'}, {0x1F,0xE9,'2','@'}, {0x20,'"','3','#'}, {0x21,'\'','4',0},
  {0x22,'(','5',0}, {0x23,0xA7,'6','^'}, {0x24,0xE8,'7',0}, {0x25,'!','8',0},
  {0x26,0xE7,'9','{'}, {0x27,0xE0,'0','}'},
  {0x2D,')',0xB0,0}, {0x2E,'-','_',0}, {0x2F,DK|'^',DK|0xA8,'['}, {0x30,'$','*',']'},
  {0x31,0xB5,0xA3,DK|'`'}, {0x32,0xB5,0xA3,DK|'`'}, {0x34,0xF9,'%',DK|0xB4},
  {0x35,0xB2,0xB3,0}, {0x64,'<','>','\\'},
};

// Bulgarian (traditional phonetic). Every letter position is here, so the
// Latin fallback in mapKey() is never reached for a letter; the punctuation
// keys a full keyboard has carry the four extra letters.
constexpr KeyMap kBG[] = {
  {0x04,0x0430,0x0410,0}, {0x05,0x0431,0x0411,0}, {0x06,0x0446,0x0426,0},
  {0x07,0x0434,0x0414,0}, {0x08,0x0435,0x0415,0}, {0x09,0x0444,0x0424,0},
  {0x0A,0x0433,0x0413,0}, {0x0B,0x0445,0x0425,0}, {0x0C,0x0438,0x0418,0},
  {0x0D,0x0439,0x0419,0}, {0x0E,0x043A,0x041A,0}, {0x0F,0x043B,0x041B,0},
  {0x10,0x043C,0x041C,0}, {0x11,0x043D,0x041D,0}, {0x12,0x043E,0x041E,0},
  {0x13,0x043F,0x041F,0}, {0x14,0x044F,0x042F,0}, {0x15,0x0440,0x0420,0},
  {0x16,0x0441,0x0421,0}, {0x17,0x0442,0x0422,0}, {0x18,0x0443,0x0423,0},
  {0x19,0x0436,0x0416,0}, {0x1A,0x0432,0x0412,0}, {0x1B,0x044C,0x042C,0},
  {0x1C,0x044A,0x042A,0}, {0x1D,0x0437,0x0417,0},
  // The number row keeps its digits; Shift is the traditional phonetic row.
  {0x1E,'1','!',0}, {0x1F,'2','@',0}, {0x20,'3','#',0}, {0x21,'4','$',0},
  {0x22,'5','%',0}, {0x23,'6',0x20AC,0}, {0x24,'7',0xA7,0}, {0x25,'8','*',0},
  {0x26,'9','(',0}, {0x27,'0',')',0},
  {0x2D,'-',0x2014,0},
  {0x36,',',0x201E,0}, {0x37,'.',0x201C,0},   // Shift+, = „, Shift+. = “
  // The four letters that have no Latin position in the 26-letter rows.
  {0x2F,0x0448,0x0428,0}, {0x30,0x0449,0x0429,0},   // [ = ш, ] = щ
  {0x31,0x044E,0x042E,0}, {0x32,0x044E,0x042E,0},   // \ = ю
  {0x35,0x0447,0x0427,0},                           // ` = ч
  {0x64,0x044E,0x042E,0},                           // ISO extra key = ю
};

struct LayoutDef { const KeyMap* keys; size_t count; };

LayoutDef tableFor(BleKbd::Layout layout) {
  switch (layout) {
    case BleKbd::Layout::UK: return {kUK, sizeof(kUK) / sizeof(kUK[0])};
    case BleKbd::Layout::DE: return {kDE, sizeof(kDE) / sizeof(kDE[0])};
    case BleKbd::Layout::FR: return {kFR, sizeof(kFR) / sizeof(kFR[0])};
    case BleKbd::Layout::BE: return {kBE, sizeof(kBE) / sizeof(kBE[0])};
    case BleKbd::Layout::BG: return {kBG, sizeof(kBG) / sizeof(kBG[0])};
    case BleKbd::Layout::US:
    default:                 return {kUS, sizeof(kUS) / sizeof(kUS[0])};
  }
}

const KeyMap* findKey(const KeyMap* table, size_t count, uint8_t usage) {
  for (size_t i = 0; i < count; i++)
    if (table[i].usage == usage) return &table[i];
  return nullptr;
}

// A key whose shifted value is its own capital is a letter, so Caps Lock
// flips it. Cyrillic а-я is contiguous, like Latin a-z.
bool isCasePair(uint32_t lower, uint32_t upper) {
  if (lower >= 'a' && lower <= 'z') return upper == lower - 32;
  if (lower >= 0xE0 && lower <= 0xFE && lower != 0xF7) return upper == lower - 0x20;
  if (lower >= 0x0430 && lower <= 0x044F) return upper == lower - 0x20;
  return false;
}

struct Compose { uint32_t accent; char base; uint32_t out; };
constexpr Compose kCompose[] = {
  {'^','a',0xE2}, {'^','e',0xEA}, {'^','i',0xEE}, {'^','o',0xF4}, {'^','u',0xFB},
  {'^','A',0xC2}, {'^','E',0xCA}, {'^','I',0xCE}, {'^','O',0xD4}, {'^','U',0xDB},
  {0xA8,'a',0xE4}, {0xA8,'e',0xEB}, {0xA8,'i',0xEF}, {0xA8,'o',0xF6}, {0xA8,'u',0xFC},
  {0xA8,'y',0xFF}, {0xA8,'A',0xC4}, {0xA8,'E',0xCB}, {0xA8,'I',0xCF}, {0xA8,'O',0xD6},
  {0xA8,'U',0xDC},
  {0xB4,'a',0xE1}, {0xB4,'e',0xE9}, {0xB4,'i',0xED}, {0xB4,'o',0xF3}, {0xB4,'u',0xFA},
  {0xB4,'y',0xFD}, {0xB4,'A',0xC1}, {0xB4,'E',0xC9}, {0xB4,'I',0xCD}, {0xB4,'O',0xD3},
  {0xB4,'U',0xDA}, {0xB4,'Y',0xDD},
  {'`','a',0xE0}, {'`','e',0xE8}, {'`','i',0xEC}, {'`','o',0xF2}, {'`','u',0xF9},
  {'`','A',0xC0}, {'`','E',0xC8}, {'`','I',0xCC}, {'`','O',0xD2}, {'`','U',0xD9},
  {'~','a',0xE3}, {'~','n',0xF1}, {'~','o',0xF5}, {'~','A',0xC3}, {'~','N',0xD1},
  {'~','O',0xD5},
};

}  // namespace

uint32_t mapKey(BleKbd::Layout layout, uint8_t usage, bool shift, bool altgr, bool caps) {
  const LayoutDef def = tableFor(layout);
  const KeyMap* km = findKey(def.keys, def.count, usage);
  if (!km && def.keys != kUS) km = findKey(kUS, sizeof(kUS) / sizeof(kUS[0]), usage);
  if (km) {
    uint32_t v = altgr ? km->altgr : (shift ? km->shift : km->base);
    if (!v) return 0;
    if (caps && !altgr && !(v & DK) && isCasePair(km->base, km->shift))
      v = shift ? km->base : km->shift;
    return (v & DK) ? (kDeadFlag | (v & 0x7FFF)) : v;
  }
  if (usage >= 0x04 && usage <= 0x1D) {
    if (altgr) return 0;
    const char c = (char)('a' + (usage - 0x04));
    return (shift != caps) ? (uint32_t)(c - 32) : (uint32_t)c;
  }
  return 0;
}

uint32_t compose(uint32_t accent, uint32_t cp) {
  if (cp == ' ') return accent;
  for (const Compose& c : kCompose)
    if (c.accent == accent && (uint8_t)c.base == cp) return c.out;
  return 0;
}

bool toKeyboardLayout(BleKbd::Layout layout, KeyboardLayoutId* out) {
  if (!out) return false;
  switch (layout) {
    case BleKbd::Layout::US: *out = KeyboardLayoutId::EN; return true;
    case BleKbd::Layout::BG: *out = KeyboardLayoutId::BG; return true;
    case BleKbd::Layout::DE: *out = KeyboardLayoutId::DE; return true;
    case BleKbd::Layout::FR: *out = KeyboardLayoutId::FR; return true;
    default: return false;   // UK (British) and BE have no shared on-screen id
  }
}

bool fromKeyboardLayout(KeyboardLayoutId id, BleKbd::Layout* out) {
  if (!out) return false;
  switch (id) {
    case KeyboardLayoutId::EN: *out = BleKbd::Layout::US; return true;
    case KeyboardLayoutId::BG: *out = BleKbd::Layout::BG; return true;
    case KeyboardLayoutId::DE: *out = BleKbd::Layout::DE; return true;
    case KeyboardLayoutId::FR: *out = BleKbd::Layout::FR; return true;
    default: return false;
  }
}

const char* layoutCode(BleKbd::Layout layout) {
  switch (layout) {
    case BleKbd::Layout::US: return "EN";
    case BleKbd::Layout::UK: return "UK";
    case BleKbd::Layout::DE: return "DE";
    case BleKbd::Layout::FR: return "FR";
    case BleKbd::Layout::BE: return "BE";
    case BleKbd::Layout::BG: return "BG";
    default:                 return "EN";
  }
}

}  // namespace BleKeyMap

#endif  // CAP_BLE_KEYBOARD
