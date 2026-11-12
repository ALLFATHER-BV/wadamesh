// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the Bluetooth keyboard's HID-usage maps. The Bulgarian table is
// the traditional phonetic deck: q=я, w=в, ... and the four letters the 26
// Latin positions cannot hold on `, [, ], \. Build from the repo root:
//
//   g++ -std=c++17 -I src test/test_ble_keymap.cpp -o /tmp/test_ble_keymap && /tmp/test_ble_keymap
#include <assert.h>
#include <stdio.h>

// BleKeyMap.cpp is gated on CAP_BLE_KEYBOARD; the host has no board -D flags,
// so give device_caps.h the two the capability needs.
#define BLE_PIN_CODE 1
#define MULTI_TRANSPORT_COMPANION 1
#include "../src/ui-touch/BleKeyMap.cpp"

using BleKbd::Layout;

static uint32_t key(Layout l, uint8_t usage, bool shift = false, bool altgr = false, bool caps = false) {
  return BleKeyMap::mapKey(l, usage, shift, altgr, caps);
}

int main() {
  // ---- Bulgarian: every letter at its US-QWERTY position ----
  struct BgKey { uint8_t usage; uint32_t lower, upper; };
  const BgKey bg[] = {
    {0x14, 0x044F, 0x042F},  // q -> я
    {0x1A, 0x0432, 0x0412},  // w -> в
    {0x08, 0x0435, 0x0415},  // e -> е
    {0x15, 0x0440, 0x0420},  // r -> р
    {0x17, 0x0442, 0x0422},  // t -> т
    {0x1C, 0x044A, 0x042A},  // y -> ъ
    {0x18, 0x0443, 0x0423},  // u -> у
    {0x0C, 0x0438, 0x0418},  // i -> и
    {0x12, 0x043E, 0x041E},  // o -> о
    {0x13, 0x043F, 0x041F},  // p -> п
    {0x04, 0x0430, 0x0410},  // a -> а
    {0x16, 0x0441, 0x0421},  // s -> с
    {0x07, 0x0434, 0x0414},  // d -> д
    {0x09, 0x0444, 0x0424},  // f -> ф
    {0x0A, 0x0433, 0x0413},  // g -> г
    {0x0B, 0x0445, 0x0425},  // h -> х
    {0x0D, 0x0439, 0x0419},  // j -> й
    {0x0E, 0x043A, 0x041A},  // k -> к
    {0x0F, 0x043B, 0x041B},  // l -> л
    {0x1D, 0x0437, 0x0417},  // z -> з
    {0x1B, 0x044C, 0x042C},  // x -> ь
    {0x06, 0x0446, 0x0426},  // c -> ц
    {0x19, 0x0436, 0x0416},  // v -> ж
    {0x05, 0x0431, 0x0411},  // b -> б
    {0x11, 0x043D, 0x041D},  // n -> н
    {0x10, 0x043C, 0x041C},  // m -> м
  };
  for (const BgKey& k : bg) {
    assert(key(Layout::BG, k.usage) == k.lower);
    assert(key(Layout::BG, k.usage, true) == k.upper);
  }

  // ---- The four letters with no Latin position, and the traditional shift row ----
  assert(key(Layout::BG, 0x2F) == 0x0448);          // [ = ш
  assert(key(Layout::BG, 0x30) == 0x0449);          // ] = щ
  assert(key(Layout::BG, 0x31) == 0x044E);          // \ = ю
  assert(key(Layout::BG, 0x64) == 0x044E);          // ISO extra key = ю
  assert(key(Layout::BG, 0x35) == 0x0447);          // ` = ч
  assert(key(Layout::BG, 0x2F, true) == 0x0428);
  assert(key(Layout::BG, 0x30, true) == 0x0429);
  assert(key(Layout::BG, 0x31, true) == 0x042E);
  assert(key(Layout::BG, 0x35, true) == 0x0427);
  assert(key(Layout::BG, 0x1E, true) == '!');
  assert(key(Layout::BG, 0x23, true) == 0x20AC);    // 6 -> €
  assert(key(Layout::BG, 0x24, true) == 0xA7);      // 7 -> §
  assert(key(Layout::BG, 0x2D, true) == 0x2014);    // - -> em dash
  assert(key(Layout::BG, 0x36, true) == 0x201E);    // Shift+, -> „
  assert(key(Layout::BG, 0x37, true) == 0x201C);    // Shift+. -> “

  // ---- Caps Lock flips Cyrillic too ----
  assert(key(Layout::BG, 0x14, false, false, true) == 0x042F);   // q with Caps -> Я
  assert(key(Layout::BG, 0x14, true, false, true) == 0x044F);    // Shift+q with Caps -> я

  // ---- The existing decks still map the way they did ----
  assert(key(Layout::US, 0x14) == 'q');
  assert(key(Layout::US, 0x14, true) == 'Q');
  assert(key(Layout::US, 0x1E, true) == '!');
  assert(key(Layout::DE, 0x1C) == 'z');   // QWERTZ: the y position prints z
  assert(key(Layout::DE, 0x1D) == 'y');
  assert(key(Layout::FR, 0x14) == 'a');   // AZERTY
  assert(key(Layout::FR, 0x04) == 'q');

  // ---- Dead keys and composition ----
  assert(key(Layout::DE, 0x2E) == (BleKeyMap::kDeadFlag | 0xB4));
  assert(BleKeyMap::compose(0xB4, 'a') == 0xE1);
  assert(BleKeyMap::compose(0xB4, 'q') == 0);

  // ---- The shared on-screen ids ----
  KeyboardLayoutId id;
  assert(BleKeyMap::toKeyboardLayout(Layout::BG, &id) && id == KeyboardLayoutId::BG);
  assert(BleKeyMap::toKeyboardLayout(Layout::US, &id) && id == KeyboardLayoutId::EN);
  assert(!BleKeyMap::toKeyboardLayout(Layout::UK, &id));
  assert(!BleKeyMap::toKeyboardLayout(Layout::BE, &id));
  Layout l;
  assert(BleKeyMap::fromKeyboardLayout(KeyboardLayoutId::BG, &l) && l == Layout::BG);
  assert(BleKeyMap::fromKeyboardLayout(KeyboardLayoutId::FR, &l) && l == Layout::FR);
  assert(!BleKeyMap::fromKeyboardLayout(KeyboardLayoutId::RU, &l));

  printf("test_ble_keymap: ok\n");
  return 0;
}
