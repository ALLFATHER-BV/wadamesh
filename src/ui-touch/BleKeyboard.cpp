// SPDX-License-Identifier: GPL-3.0-or-later
//
// External Bluetooth LE keyboard: a HID-over-GATT host on NimBLE's central
// role. See BleKeyboard.h for the threading contract.
//
// Keys are read the way phones and computers read them: the keyboard's Report
// Map says which report carries the keys and how they are laid out (a six-key
// array, or one bit per key), so only those reports are subscribed and media
// keys or a touchpad on the same keyboard are left alone. A keyboard whose map
// cannot be used is switched to the boot protocol, whose 8-byte report has one
// fixed layout; one that ignores that request is read in the same layout on
// its report characteristics. The keys are then translated with the layout
// the user picks, because a keyboard only sends key positions: an AZERTY
// keyboard and a QWERTY keyboard send the same code for the key left of Z/W.
#include "BleKeyboard.h"

#if CAP_BLE_KEYBOARD

#include <Arduino.h>
#include <NimBLEDevice.h>
#if defined(CONFIG_NIMBLE_CPP_IDF)
#  include "host/ble_hs.h"
#else
#  include "nimble/nimble/host/include/host/ble_hs.h"
#endif
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_random()
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <helpers/input/HidReportMap.h>

namespace BleKbd {
namespace {

constexpr uint16_t kUuidHid          = 0x1812;
constexpr uint16_t kUuidReport       = 0x2A4D;
constexpr uint16_t kUuidProtocolMode = 0x2A4E;
constexpr uint16_t kUuidBootKbdIn    = 0x2A22;
constexpr uint16_t kUuidBootKbdOut   = 0x2A32;
constexpr uint16_t kUuidReportRef    = 0x2908;
constexpr uint16_t kUuidReportMap    = 0x2A4B;

constexpr uint32_t kRepeatDelayMs = 500;
constexpr uint32_t kRepeatRateMs  = 40;
constexpr uint32_t kRetryMs       = 3000;
constexpr uint32_t kScanSeconds   = 30;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Shared between the UI, the worker and the NimBLE host task (under s_mux).
bool     s_active = false;
State    s_state = State::Off;
Error    s_error = Error::None;
uint32_t s_gen = 0;
uint32_t s_code = 0;
Device   s_peer = {};
bool     s_have_peer = false;
bool     s_new_pairing = false;
bool     s_forgotten = false;
Layout   s_layout = Layout::US;
uint8_t  s_back_usage = 0;       // a second Back key, 0 = none
bool     s_capture = false;      // the next key pressed is kept, not typed
bool     s_have_captured = false;
uint8_t  s_captured = 0;

constexpr int kMaxDevices = 12;
Device s_devices[kMaxDevices];
int    s_device_count = 0;

// Key ring: filled by the NimBLE host task, drained by the UI loop.
constexpr int kRing = 64;
int     s_ring[kRing];
uint8_t s_head = 0;
uint8_t s_tail = 0;

// Typematic repeat of the last key still held (under s_mux).
int      s_held_code = 0;
uint8_t  s_held_usage = 0;
uint32_t s_held_since = 0;
uint32_t s_next_repeat = 0;

// This link's reports. The worker fills these in setupHid() before it
// subscribes, and the NimBLE host task only reads them afterwards.
HidReportMapInfo s_map = {};
bool     s_map_ok = false;
uint16_t s_boot_handle = 0;         // boot keyboard input report, 0 when not subscribed
constexpr int kMaxReportChars = 8;
struct ReportChar {
  uint16_t handle;
  int8_t   layout;   // index into s_map.kbd, or -1: no usable map, read as a boot report
};
ReportChar s_report_chars[kMaxReportChars];
uint8_t    s_report_char_count = 0;

// Report decoding (NimBLE host task only).
// When a keyboard ignores the switch to the boot protocol, the same keys
// could arrive on both channels; the first one that delivers keys is used for
// the rest of the link.
enum : uint8_t { kSrcNone, kSrcBoot, kSrcReport };
uint8_t  s_src = kSrcNone;
#if BLE_KBD_TRACE
uint32_t s_report_logs = 0;   // the first reports of each link are logged
#endif
uint8_t  s_prev_keys[6] = {};
// Command (GUI) tapped on its own, with nothing else pressed while it was down.
bool     s_gui_down = false;
bool     s_gui_alone = false;
uint32_t s_gui_since = 0;
constexpr uint32_t kGuiTapMs = 1000;   // held longer than this it was not a tap
bool     s_caps = false;
uint32_t s_dead = 0;   // pending dead-key accent, 0 = none

// Worker.
enum class CmdType : uint8_t { Activate, Deactivate, ScanOn, ScanOff, Pair, Forget, SyncLeds };
struct Cmd { CmdType type; int8_t index; bool with_code; };
TaskHandle_t  s_task = nullptr;
QueueHandle_t s_cmdq = nullptr;
NimBLEClient* s_client = nullptr;
// Caps Lock LED: the boot output report, or the output report of the report
// protocol, whichever matches the channel the keys arrive on. Both point into
// the client's attribute list, which connect() rebuilds, so they are cleared
// before every connect.
NimBLERemoteCharacteristic* s_led_boot = nullptr;
NimBLERemoteCharacteristic* s_led_report = nullptr;
volatile bool s_connecting = false;   // a connect() is waiting; ble_gap_conn_cancel() ends it
volatile bool s_suspended = false;    // off the air for now; the user's choice is unchanged
volatile bool s_busy = false;

void bumpLocked() { s_gen++; }

void setState(State st) {
  portENTER_CRITICAL(&s_mux);
  s_state = st;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

void setError(Error e) {
  portENTER_CRITICAL(&s_mux);
  s_error = e;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

State getState() {
  portENTER_CRITICAL(&s_mux);
  const State st = s_state;
  portEXIT_CRITICAL(&s_mux);
  return st;
}

bool havePeer(Device* out) {
  portENTER_CRITICAL(&s_mux);
  const bool have = s_have_peer;
  if (have && out) *out = s_peer;
  portEXIT_CRITICAL(&s_mux);
  return have;
}

bool isLinkUp() { return s_client && s_client->isConnected(); }

// ---- key output -------------------------------------------------------------

void clearHeld() {
  portENTER_CRITICAL(&s_mux);
  s_held_code = 0;
  s_held_usage = 0;
  portEXIT_CRITICAL(&s_mux);
}

void pushKey(int code) {
  const uint8_t next = (uint8_t)((s_head + 1) % kRing);
  if (next == s_tail) return;   // full: drop rather than block the host task
  s_ring[s_head] = code;
  s_head = next;
}

void emit(int code, uint8_t usage, bool repeatable) {
  pushKey(code);
  const uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  if (repeatable) {
    s_held_code = code;
    s_held_usage = usage;
    s_held_since = now;
    s_next_repeat = now + kRepeatDelayMs;
  } else {
    s_held_code = 0;
    s_held_usage = 0;
  }
  portEXIT_CRITICAL(&s_mux);
}

// ---- layouts ------------------------------------------------------------------

constexpr uint16_t DK = 0x8000;           // table flag: dead key (accent waits for the next key)
constexpr uint32_t kDeadFlag = 0x80000000;

struct KeyMap { uint8_t usage; uint16_t base, shift, altgr; };

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

struct LayoutDef { const char* name; const KeyMap* keys; size_t count; };
constexpr LayoutDef kLayouts[] = {
  {"US", kUS, sizeof(kUS) / sizeof(kUS[0])},
  {"UK", kUK, sizeof(kUK) / sizeof(kUK[0])},
  {"DE", kDE, sizeof(kDE) / sizeof(kDE[0])},
  {"FR", kFR, sizeof(kFR) / sizeof(kFR[0])},
  {"BE", kBE, sizeof(kBE) / sizeof(kBE[0])},
};

const KeyMap* findKey(const KeyMap* table, size_t count, uint8_t usage) {
  for (size_t i = 0; i < count; i++)
    if (table[i].usage == usage) return &table[i];
  return nullptr;
}

// A key whose shifted value is its own capital is a letter, so Caps Lock flips it.
bool isCasePair(uint16_t lower, uint16_t upper) {
  if (lower >= 'a' && lower <= 'z') return upper == lower - 32;
  if (lower >= 0xE0 && lower <= 0xFE && lower != 0xF7) return upper == lower - 0x20;
  return false;
}

// Code point for a key, kDeadFlag | accent for a dead key, 0 for nothing.
uint32_t mapKey(Layout layout, uint8_t usage, bool shift, bool altgr, bool caps) {
  const LayoutDef& def = kLayouts[(size_t)layout < (size_t)Layout::Count ? (size_t)layout : 0];
  const KeyMap* km = findKey(def.keys, def.count, usage);
  if (!km && def.keys != kUS) km = findKey(kUS, sizeof(kUS) / sizeof(kUS[0]), usage);
  if (km) {
    uint16_t v = altgr ? km->altgr : (shift ? km->shift : km->base);
    if (!v) return 0;
    if (caps && !altgr && !(v & DK) && isCasePair(km->base, km->shift))
      v = shift ? km->base : km->shift;
    return (v & DK) ? (kDeadFlag | (uint32_t)(v & 0x7FFF)) : v;
  }
  if (usage >= 0x04 && usage <= 0x1D) {
    if (altgr) return 0;
    const char c = (char)('a' + (usage - 0x04));
    return (shift != caps) ? (uint32_t)(c - 32) : (uint32_t)c;
  }
  return 0;
}

struct Compose { uint16_t accent; char base; uint16_t out; };
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

// Accent + key. Space gives the accent itself; 0 means "no such letter".
uint32_t compose(uint32_t accent, uint32_t cp) {
  if (cp == ' ') return accent;
  for (const Compose& c : kCompose)
    if (c.accent == accent && (uint8_t)c.base == cp) return c.out;
  return 0;
}

// ---- reports ------------------------------------------------------------------

void requestLedSync() {
  if (!s_cmdq) return;
  const Cmd c{CmdType::SyncLeds, 0, false};
  xQueueSend(s_cmdq, &c, 0);
}

void pressKey(uint8_t usage, uint8_t mod) {
  portENTER_CRITICAL(&s_mux);
  const bool capture = s_capture;
  if (capture) {
    s_capture = false;
    s_captured = usage;
    s_have_captured = true;
    bumpLocked();
  }
  const uint8_t back = s_back_usage;
  portEXIT_CRITICAL(&s_mux);
  if (capture) return;   // the settings page is picking a key
  if (back && usage == back) {
    s_dead = 0;
    emit(BLE_KEY_ESC, usage, false);
    return;
  }

  const bool shift = (mod & 0x22) != 0;
  const bool altgr = (mod & 0x40) != 0 || (mod & 0x05) == 0x05;   // right Alt, or Ctrl+Alt
  const bool ctrl  = (mod & 0x11) != 0 && !altgr;
  const bool alt   = (mod & 0x04) != 0 && !altgr;
  const bool gui   = (mod & 0x88) != 0;

  int special = 0;
  bool repeat = true;
  switch (usage) {
    case 0x28: case 0x58: special = BLE_KEY_ENTER; repeat = false; break;
    case 0x29: special = BLE_KEY_ESC; repeat = false; break;
    case 0x2A: special = BLE_KEY_BACKSPACE; break;
    case 0x2B: special = shift ? BLE_KEY_SHIFT_TAB : BLE_KEY_TAB; repeat = false; break;
    case 0x39: s_caps = !s_caps; requestLedSync(); return;
    case 0x4A: special = BLE_KEY_HOME; repeat = false; break;
    case 0x4B: special = BLE_KEY_PAGE_UP; break;
    case 0x4C: special = BLE_KEY_DELETE; break;
    case 0x4D: special = BLE_KEY_END; repeat = false; break;
    case 0x4E: special = BLE_KEY_PAGE_DOWN; break;
    case 0x4F: special = BLE_KEY_RIGHT; break;
    case 0x50: special = BLE_KEY_LEFT; break;
    case 0x51: special = BLE_KEY_DOWN; break;
    case 0x52: special = BLE_KEY_UP; break;
    default: break;
  }
  if (special) {
    s_dead = 0;
    emit(special, usage, repeat);
    return;
  }
  if (gui || ctrl || alt) return;   // shortcuts, not text

  portENTER_CRITICAL(&s_mux);
  const Layout layout = s_layout;
  portEXIT_CRITICAL(&s_mux);

  uint32_t cp = 0;
  if (usage == 0x2C) {
    cp = ' ';
  } else if (usage >= 0x54 && usage <= 0x63) {
    static constexpr char kPad[] = "/*-+\n1234567890.";   // 0x58 (keypad Enter) is handled above
    cp = (uint8_t)kPad[usage - 0x54];
  } else {
    cp = mapKey(layout, usage, shift, altgr, s_caps);
  }
  if (!cp) return;
  if (cp & kDeadFlag) {
    if (s_dead) emit(BLE_KEY_TEXT | (int)s_dead, usage, false);   // a second accent: type the first
    s_dead = cp & ~kDeadFlag;
    return;
  }
  if (s_dead) {
    const uint32_t accent = s_dead;
    s_dead = 0;
    const uint32_t composed = compose(accent, cp);
    if (composed) {
      cp = composed;
    } else {
      pushKey(BLE_KEY_TEXT | (int)accent);   // no such letter: the accent, then the key
    }
  }
  emit(BLE_KEY_TEXT | (int)cp, usage, true);
}

// The fixed boot layout: modifiers, a reserved byte, six key usages.
bool bootReport(const uint8_t* data, size_t len, uint8_t* mod, uint8_t keys[6]) {
  if (len < 8) return false;
  if (data[2] >= 0x01 && data[2] <= 0x03) return false;   // rollover error: the state is unknown
  *mod = data[0];
  memcpy(keys, data + 2, 6);
  return true;
}

// -2: not a keyboard report of this link.
int reportLayout(uint16_t handle) {
  for (uint8_t i = 0; i < s_report_char_count; i++)
    if (s_report_chars[i].handle == handle) return s_report_chars[i].layout;
  return -2;
}

void onReport(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
  if (!chr) return;
  const uint16_t handle = chr->getHandle();
  const bool from_boot = s_boot_handle != 0 && handle == s_boot_handle;
#if BLE_KBD_TRACE
  if (s_report_logs < 400) {
    s_report_logs++;
    char hex[3 * 12 + 1] = "";
    for (size_t i = 0; i < len && i < 12; i++) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    Serial.printf("[blekbd] report %u len=%u: %s\n", (unsigned)handle, (unsigned)len, hex);
  }
#endif

  uint8_t mod = 0;
  uint8_t keys[6] = {};
  bool ok = false;
  if (from_boot) {
    ok = bootReport(data, len, &mod, keys);
  } else {
    const int layout = reportLayout(handle);
    if (layout >= 0) ok = hidDecodeKeyboard(s_map.kbd[layout], data, len, &mod, keys);
    else if (layout == -1 && len == 8) ok = bootReport(data, len, &mod, keys);
  }
  if (!ok) return;

  const uint8_t src = from_boot ? kSrcBoot : kSrcReport;
  if (s_src == kSrcNone) s_src = src;
  else if (s_src != src) return;   // the same keys on the other channel

  bool any_key = false;
  for (int i = 0; i < 6; i++) any_key |= keys[i] >= 0x04;
  const bool gui = (mod & 0x88) != 0;
  const bool others = any_key || (mod & ~0x88) != 0;
  if (gui && !s_gui_down) {
    s_gui_alone = !others;
    s_gui_since = millis();
  } else if (gui && others) {
    s_gui_alone = false;   // used as a modifier
  } else if (!gui && s_gui_down && s_gui_alone && millis() - s_gui_since < kGuiTapMs) {
    emit(BLE_KEY_EMOJI, 0, false);
  }
  s_gui_down = gui;

  portENTER_CRITICAL(&s_mux);
  const uint8_t held = s_held_usage;
  portEXIT_CRITICAL(&s_mux);
  bool held_down = false;
  for (int i = 0; i < 6; i++) held_down |= (held != 0 && keys[i] == held);
  if (!held_down) clearHeld();

  for (int i = 0; i < 6; i++) {
    const uint8_t k = keys[i];
    if (k < 0x04) continue;
    bool was_down = false;
    for (int j = 0; j < 6; j++) was_down |= (s_prev_keys[j] == k);
    if (!was_down) pressKey(k, mod);
  }
  memcpy(s_prev_keys, keys, sizeof s_prev_keys);
}

// ---- link -----------------------------------------------------------------------

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*) override {
    memset(s_prev_keys, 0, sizeof s_prev_keys);
    s_gui_down = false;
    s_gui_alone = false;
    s_dead = 0;
    clearHeld();
    portENTER_CRITICAL(&s_mux);
    if (s_state == State::Connected) s_state = s_active ? State::Waiting : State::Off;
    bumpLocked();
    portEXIT_CRITICAL(&s_mux);
  }
};
ClientCallbacks s_client_cb;

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* adv) override {
    const bool hid = adv->isAdvertisingService(NimBLEUUID(kUuidHid));
    const uint16_t appearance = adv->haveAppearance() ? adv->getAppearance() : 0;
    const bool hid_appearance = appearance >= 0x03C0 && appearance <= 0x03CF;
    if (!hid && !hid_appearance) return;

    Device dev = {};
    if (adv->haveName()) {
      const std::string n = adv->getName();
      strncpy(dev.name, n.c_str(), sizeof dev.name - 1);
    }
    const NimBLEAddress addr = adv->getAddress();
    memcpy(dev.addr, addr.getNative(), 6);
    dev.addr_type = addr.getType();
    const int rssi = adv->getRSSI();
    dev.rssi = (int8_t)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
    dev.keyboard = appearance == 0x03C1;

    portENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < s_device_count; i++)
      if (memcmp(s_devices[i].addr, dev.addr, 6) == 0) { slot = i; break; }
    const bool fresh = slot < 0;
    if (slot < 0 && s_device_count < kMaxDevices) slot = s_device_count++;
    if (slot >= 0) {
      if (!dev.name[0]) memcpy(dev.name, s_devices[slot].name, sizeof dev.name);
      s_devices[slot] = dev;
      bumpLocked();
    }
    portEXIT_CRITICAL(&s_mux);
    if (fresh && slot >= 0)
      Serial.printf("[blekbd] found %s %s appearance=0x%04X hid=%d rssi=%d\n",
                    dev.name[0] ? dev.name : "?", addr.toString().c_str(), appearance, hid ? 1 : 0,
                    (int)dev.rssi);
  }
};
ScanCallbacks s_scan_cb;

State restingState() {
  if (isLinkUp()) return State::Connected;
  portENTER_CRITICAL(&s_mux);
  const State st = !s_active ? State::Off : (s_have_peer ? State::Waiting : State::Idle);
  portEXIT_CRITICAL(&s_mux);
  return st;
}

void onScanDone(NimBLEScanResults) {
  if (getState() == State::Scanning) setState(restingState());
}

void stopScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();
}

// Device::addr holds the stack's own byte order (NimBLEAddress::getNative()).
// Rebuild through ble_addr_t: the uint8_t[6] constructor expects display order
// and would reverse it.
NimBLEAddress toAddress(const Device& dev) {
  ble_addr_t a;
  a.type = dev.addr_type;
  memcpy(a.val, dev.addr, sizeof a.val);
  return NimBLEAddress(a);
}

bool ensureClient() {
  if (!NimBLEDevice::getInitialized()) {
    s_client = nullptr;
    return false;
  }
  if (!s_client) {
    s_client = NimBLEDevice::createClient();
    if (!s_client) return false;
    s_client->setClientCallbacks(&s_client_cb, false);
    // 15-30 ms interval, 4 s supervision timeout; while connecting, listen
    // 30 ms of every 100 ms so the radio still has room for Wi-Fi.
    s_client->setConnectionParams(12, 24, 0, 400, 160, 48);
  }
  return true;
}

void disconnectAndWait() {
  if (!isLinkUp()) return;
  s_client->disconnect();
  for (int i = 0; i < 40 && s_client->isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(50));
}

struct SavedSecurity { uint8_t bonding, mitm, sc, io_cap; uint32_t passkey; };

SavedSecurity saveSecurity() {
  return SavedSecurity{(uint8_t)ble_hs_cfg.sm_bonding, (uint8_t)ble_hs_cfg.sm_mitm,
                       (uint8_t)ble_hs_cfg.sm_sc, (uint8_t)ble_hs_cfg.sm_io_cap,
                       NimBLEDevice::getSecurityPasskey()};
}

// The phone-app link configures these globally; put its values back so a later
// phone pairing still asks for its own code.
void restoreSecurity(const SavedSecurity& s) {
  NimBLEDevice::setSecurityAuth(s.bonding != 0, s.mitm != 0, s.sc != 0);
  NimBLEDevice::setSecurityIOCap(s.io_cap);
  NimBLEDevice::setSecurityPasskey(s.passkey);
}

// Report Reference descriptor: [report id, type] with type 1 = input, 2 = output.
bool reportRef(NimBLERemoteCharacteristic* chr, uint8_t* id, uint8_t* type) {
  NimBLERemoteDescriptor* ref = chr->getDescriptor(NimBLEUUID(kUuidReportRef));
  if (!ref) return false;
  const NimBLEAttValue v = ref->readValue();
  if (v.length() < 2) return false;
  *id = v.data()[0];
  *type = v.data()[1];
  return true;
}

// Internal RAM left once a keyboard is linked: the number that decides which
// boards can afford a keyboard next to everything else.
// Called on the worker, so the stack figure is its own least-free headroom.
void logLinked(const char* what) {
  Serial.printf("[blekbd] %s, internal heap %u free, largest block %u, worker stack %u unused\n", what,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void forgetAttributes() {
  s_led_boot = nullptr;
  s_led_report = nullptr;
}

bool setupHid() {
  forgetAttributes();
  s_src = kSrcNone;
#if BLE_KBD_TRACE
  s_report_logs = 0;
#endif
  s_boot_handle = 0;
  s_report_char_count = 0;
  s_map_ok = false;
  s_map.kbd_count = 0;
  s_map.have_leds = false;
  NimBLERemoteService* hid = s_client->getService(NimBLEUUID(kUuidHid));
  if (!hid) {
    Serial.println("[blekbd] HID: no HID service");
    return false;
  }
  // Discover every characteristic once and take them all from this list: a
  // later refresh deletes these objects, along with the notify callbacks set
  // on them.
  const std::vector<NimBLERemoteCharacteristic*>* chars = hid->getCharacteristics(true);
  if (!chars || chars->empty()) {
    Serial.println("[blekbd] HID: no characteristics");
    return false;
  }
  NimBLERemoteCharacteristic* boot = nullptr;
  NimBLERemoteCharacteristic* mode = nullptr;
  NimBLERemoteCharacteristic* boot_out = nullptr;
  NimBLERemoteCharacteristic* report_map = nullptr;
  for (NimBLERemoteCharacteristic* chr : *chars) {
    const NimBLEUUID uuid = chr->getUUID();
    if (uuid == NimBLEUUID(kUuidBootKbdIn)) boot = chr;
    else if (uuid == NimBLEUUID(kUuidProtocolMode)) mode = chr;
    else if (uuid == NimBLEUUID(kUuidBootKbdOut)) boot_out = chr;
    else if (uuid == NimBLEUUID(kUuidReportMap)) report_map = chr;
  }

  if (report_map) {
    const NimBLEAttValue desc = report_map->readValue();
    s_map_ok = hidParseReportMap(desc.data(), desc.length(), &s_map);
    Serial.printf("[blekbd] HID: report map %u bytes, keyboard reports %d, leds %d\n",
                  (unsigned)desc.length(), s_map.kbd_count, s_map.have_leds ? 1 : 0);
#if BLE_KBD_TRACE
    for (size_t off = 0; off < desc.length(); off += 32) {
      char hex[32 * 3 + 1] = "";
      for (size_t i = 0; i < 32 && off + i < desc.length(); i++)
        snprintf(hex + 3 * i, 4, "%02X ", desc.data()[off + i]);
      Serial.printf("[blekbd] HID:   map %s\n", hex);
    }
#endif
    for (int i = 0; i < s_map.kbd_count; i++) {
      const HidKeyboardReport& k = s_map.kbd[i];
      Serial.printf("[blekbd] HID:   id %u, %u bits: mods@%d keys@%d x%u bitmap@%d x%u\n",
                    (unsigned)k.report_id, (unsigned)k.bits, k.mod_bit, k.keys_bit,
                    (unsigned)k.keys_count, k.bitmap_bit, (unsigned)k.bitmap_count);
    }
  }

  // Input reports that carry keys, and the output report with the LEDs.
  int key_reports = 0;
  NimBLERemoteCharacteristic* led_report = nullptr;
  for (NimBLERemoteCharacteristic* chr : *chars) {
    if (chr->getUUID() != NimBLEUUID(kUuidReport)) continue;
    uint8_t id = 0;
    uint8_t type = 0;
    if (!reportRef(chr, &id, &type)) continue;
#if BLE_KBD_TRACE
    Serial.printf("[blekbd] HID: report handle=%u id=%u type=%u notify=%d\n", (unsigned)chr->getHandle(),
                  (unsigned)id, (unsigned)type, chr->canNotify() ? 1 : 0);
#endif
    if (type == 2) {
      if (s_map_ok ? (s_map.have_leds && id == s_map.leds.report_id) : !led_report) led_report = chr;
      continue;
    }
    if (type != 1 || !chr->canNotify()) continue;
    int layout = -1;
    for (int i = 0; i < s_map.kbd_count; i++)
      if (s_map.kbd[i].report_id == id) layout = i;
#if BLE_KBD_TRACE
    if (s_map_ok && layout < 0) layout = -3;   // media keys and the like: logged, never typed
#else
    if (s_map_ok && layout < 0) continue;   // media keys, a touchpad, ...
#endif
    if (s_report_char_count >= kMaxReportChars) break;
    // The entry exists before the first notification can arrive.
    s_report_chars[s_report_char_count] = ReportChar{chr->getHandle(), (int8_t)layout};
    s_report_char_count++;
    const bool sub = chr->subscribe(true, onReport, true);
    if (sub && layout != -3) key_reports++;
    Serial.printf("[blekbd] HID: report id %u on handle %u, layout %d, subscribed %d\n", (unsigned)id,
                  (unsigned)chr->getHandle(), layout, sub ? 1 : 0);
  }

  bool boot_ok = false;
  if (s_map_ok && key_reports > 0) {
    // Report protocol, as phones and computers use it.
    if (mode) {
      const uint8_t report_protocol = 1;
      mode->writeValue(&report_protocol, 1, false);
    }
  } else if (boot && boot->canNotify()) {
    if (mode) {
      const uint8_t boot_protocol = 0;
      mode->writeValue(&boot_protocol, 1, false);
    }
    s_boot_handle = boot->getHandle();
    boot_ok = boot->subscribe(true, onReport, true);
    if (!boot_ok) s_boot_handle = 0;
    else s_led_boot = boot_out;
  }
  if (key_reports > 0) s_led_report = led_report;
  Serial.printf("[blekbd] HID: %s protocol, key reports=%d boot=%d leds=%d/%d\n",
                s_map_ok && key_reports > 0 ? "report" : "boot", key_reports, boot_ok ? 1 : 0,
                s_led_boot ? 1 : 0, s_led_report ? 1 : 0);
  return boot_ok || key_reports > 0;
}

// Caps Lock as toggled here, Num Lock always on: the keypad always types digits.
void syncLeds() {
  if (!isLinkUp()) return;
  const bool via_report = s_src == kSrcReport || (s_src == kSrcNone && !s_led_boot);
  NimBLERemoteCharacteristic* chr = via_report ? s_led_report : s_led_boot;
  if (!chr) chr = s_led_boot ? s_led_boot : s_led_report;
  if (!chr) return;
  uint8_t buf[8] = {};
  size_t n = 0;
  if (chr == s_led_report && s_map_ok && s_map.have_leds) {
    n = hidEncodeLeds(s_map.leds, true, s_caps, buf, sizeof buf);
  } else {
    buf[0] = (uint8_t)(0x01 | (s_caps ? 0x02 : 0x00));   // boot layout: bit 0 Num Lock, bit 1 Caps Lock
    n = 1;
  }
  if (n) chr->writeValue(buf, n, !chr->canWriteNoResponse());
}

void doPair(int index, bool with_code) {
  Device dev = {};
  if (!deviceAt(index, &dev)) return;
  if (!ensureClient()) { setError(Error::NotRunning); setState(State::Failed); return; }
  stopScan();
  disconnectAndWait();

  const NimBLEAddress addr = toAddress(dev);
  // Pairing again has to start from scratch, or the old keys are reused silently.
  if (NimBLEDevice::isBonded(addr)) NimBLEDevice::deleteBond(addr);

  const SavedSecurity saved = saveSecurity();
  uint32_t code = 0;
  if (with_code) {
    code = 100000 + (esp_random() % 900000);
    NimBLEDevice::setSecurityPasskey(code);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  } else {
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }

  Serial.printf("[blekbd] pairing %s (%s, type %u) %s code\n", dev.name[0] ? dev.name : "?",
                addr.toString().c_str(), (unsigned)dev.addr_type, with_code ? "with" : "without");
  setError(Error::None);
  setState(State::Connecting);
  s_client->setConnectTimeout(12);
  forgetAttributes();
  s_connecting = true;
  bool ok = s_client->connect(addr, true);
  s_connecting = false;
  Serial.printf("[blekbd] connect: %s rc=%d\n", ok ? "ok" : "failed", s_client->getLastError());
  if (ok) {
    portENTER_CRITICAL(&s_mux);
    s_code = code;
    s_state = State::Pairing;
    bumpLocked();
    portEXIT_CRITICAL(&s_mux);
    ok = s_client->secureConnection();
    Serial.printf("[blekbd] pairing: %s rc=%d\n", ok ? "ok" : "failed", s_client->getLastError());
    portENTER_CRITICAL(&s_mux);
    s_code = 0;
    bumpLocked();
    portEXIT_CRITICAL(&s_mux);
    if (!ok) setError(with_code ? Error::PairingCode : Error::PairingNoCode);
  } else {
    setError(Error::Connect);
  }
  restoreSecurity(saved);

  if (ok && !setupHid()) {
    ok = false;
    setError(Error::NotKeyboard);
  }
  if (!ok) {
    disconnectAndWait();
    setState(State::Failed);
    return;
  }

  logLinked("paired");
  portENTER_CRITICAL(&s_mux);
  s_peer = dev;
  s_have_peer = true;
  s_new_pairing = true;
  s_state = State::Connected;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
  syncLeds();
}

// Background reconnect to the paired keyboard. The stored bond encrypts the
// link without any prompt; a keyboard that lost its half needs pairing again.
bool reconnectOnce() {
  Device dev = {};
  if (!havePeer(&dev) || !ensureClient()) return false;
  const NimBLEAddress addr = toAddress(dev);
  if (!NimBLEDevice::isBonded(addr)) {
    setError(Error::NeedsPairing);
    return false;
  }
  s_client->setConnectTimeout(10);
  forgetAttributes();
  s_connecting = true;
  bool ok = s_client->connect(addr, true);
  s_connecting = false;
  if (!ok) return false;
  if (!s_client->secureConnection()) {
    Serial.printf("[blekbd] reconnect: encryption failed rc=%d\n", s_client->getLastError());
    disconnectAndWait();
    return false;
  }
  if (!setupHid()) {
    disconnectAndWait();
    return false;
  }
  logLinked("reconnected");
  setError(Error::None);
  setState(State::Connected);
  syncLeds();
  return true;
}

bool wantReconnect() {
  if (isLinkUp()) return false;
  const State st = getState();
  if (st != State::Waiting) return false;
  portENTER_CRITICAL(&s_mux);
  const bool want = s_active && s_have_peer && s_error != Error::NeedsPairing;
  portEXIT_CRITICAL(&s_mux);
  return want && !s_suspended && NimBLEDevice::getInitialized();
}

void handle(const Cmd& c) {
  switch (c.type) {
    case CmdType::Activate:
      if (!ensureClient()) { setError(Error::NotRunning); setState(State::Failed); break; }
      setError(Error::None);
      setState(restingState());
      break;
    case CmdType::Deactivate:
      if (NimBLEDevice::getInitialized()) {
        stopScan();
        disconnectAndWait();
      }
      clearHeld();
      setState(State::Off);
      break;
    case CmdType::ScanOn: {
      if (!s_active || !ensureClient()) break;
      portENTER_CRITICAL(&s_mux);
      s_device_count = 0;
      bumpLocked();
      portEXIT_CRITICAL(&s_mux);
      NimBLEScan* scan = NimBLEDevice::getScan();
      scan->setAdvertisedDeviceCallbacks(&s_scan_cb, true);
      scan->setMaxResults(0);   // keep nothing; the callback copies what it needs
      scan->setActiveScan(true);
      scan->setInterval(100);
      scan->setWindow(60);
      scan->clearResults();
      setError(Error::None);
      if (scan->start(kScanSeconds, onScanDone, false)) setState(State::Scanning);
      break;
    }
    case CmdType::ScanOff: {
      // Also closes out a failed attempt: leaving the pairing screen resumes
      // reconnecting to the keyboard that was paired before.
      if (NimBLEDevice::getInitialized()) stopScan();
      const State st = getState();
      if (st == State::Scanning || st == State::Failed) {
        if (st == State::Failed) setError(Error::None);
        setState(restingState());
      }
      break;
    }
    case CmdType::Pair:
      doPair(c.index, c.with_code);
      break;
    case CmdType::Forget: {
      Device dev = {};
      const bool had = havePeer(&dev);
      if (NimBLEDevice::getInitialized()) {
        disconnectAndWait();
        if (had) {
          const NimBLEAddress addr = toAddress(dev);
          if (NimBLEDevice::isBonded(addr)) NimBLEDevice::deleteBond(addr);
        }
      }
      portENTER_CRITICAL(&s_mux);
      s_have_peer = false;
      s_peer = Device{};
      s_forgotten = true;
      s_error = Error::None;
      portEXIT_CRITICAL(&s_mux);
      setState(restingState());
      break;
    }
    case CmdType::SyncLeds:
      syncLeds();
      break;
  }
}

void worker(void*) {
  uint32_t next_try = 0;
  Cmd c;
  for (;;) {
    TickType_t wait = portMAX_DELAY;
    if (wantReconnect()) {
      const uint32_t now = millis();
      wait = ((int32_t)(now - next_try) >= 0) ? 0 : pdMS_TO_TICKS(next_try - now);
    }
    // Busy is raised before the command leaves the queue and before the
    // reconnect check, so stackGoingDown() never sees an idle worker that is
    // about to touch the client.
    if (xQueuePeek(s_cmdq, &c, wait) == pdTRUE) {
      s_busy = true;
      xQueueReceive(s_cmdq, &c, 0);
      handle(c);
      s_busy = false;
      continue;
    }
    s_busy = true;
    if (wantReconnect()) next_try = reconnectOnce() ? 0 : millis() + kRetryMs;
    s_busy = false;
  }
}

void post(CmdType type, int index = 0, bool with_code = false) {
  if (!s_cmdq) return;
  const Cmd c{type, (int8_t)index, with_code};
  xQueueSend(s_cmdq, &c, 0);
}

// A connect holds the worker for up to its timeout; anything the user asks for
// should not wait behind it.
void interruptConnect() {
  if (s_connecting) ble_gap_conn_cancel();
}

bool ensureWorker() {
  if (s_task) return true;
  s_cmdq = xQueueCreate(8, sizeof(Cmd));
  if (!s_cmdq) return false;
  // Internal-RAM stack: NimBLE may write bonds to flash from this task.
  if (xTaskCreatePinnedToCore(worker, "blekbd", 6144, nullptr, 1, &s_task, 0) != pdPASS) {
    vQueueDelete(s_cmdq);
    s_cmdq = nullptr;
    s_task = nullptr;
    return false;
  }
  return true;
}

}  // namespace

void setActive(bool on) {
  portENTER_CRITICAL(&s_mux);
  const bool changed = (s_active != on);
  s_active = on;
  if (changed) bumpLocked();
  portEXIT_CRITICAL(&s_mux);
  if (!changed) return;
  if (on) {
    if (ensureWorker()) post(CmdType::Activate);
  } else if (s_task) {
    interruptConnect();
    // Wake a pairing or reconnect that is waiting on the keyboard.
    if (NimBLEDevice::getInitialized() && isLinkUp()) s_client->disconnect();
    post(CmdType::Deactivate);
  }
}

bool active() {
  portENTER_CRITICAL(&s_mux);
  const bool on = s_active;
  portEXIT_CRITICAL(&s_mux);
  return on;
}

State state() { return getState(); }

Error lastError() {
  portENTER_CRITICAL(&s_mux);
  const Error e = s_error;
  portEXIT_CRITICAL(&s_mux);
  return e;
}

uint32_t generation() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t g = s_gen;
  portEXIT_CRITICAL(&s_mux);
  return g;
}

void setPaired(const Device* dev) {
  portENTER_CRITICAL(&s_mux);
  if (dev) {
    s_peer = *dev;
    s_peer.name[sizeof s_peer.name - 1] = '\0';
    s_have_peer = true;
  } else {
    s_peer = Device{};
    s_have_peer = false;
  }
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

bool paired(Device* out) { return havePeer(out); }

void scan(bool on) {
  if (!s_task) return;
  if (on) interruptConnect();
  post(on ? CmdType::ScanOn : CmdType::ScanOff);
}

int deviceCount() {
  portENTER_CRITICAL(&s_mux);
  const int n = s_device_count;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

bool deviceAt(int i, Device* out) {
  portENTER_CRITICAL(&s_mux);
  const bool ok = i >= 0 && i < s_device_count;
  if (ok && out) *out = s_devices[i];
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void pair(int i, bool with_code) {
  if (!s_task) return;
  interruptConnect();
  post(CmdType::Pair, i, with_code);
}

uint32_t pairingCode() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t code = s_code;
  portEXIT_CRITICAL(&s_mux);
  return code;
}

void forget() {
  if (!s_task) {
    portENTER_CRITICAL(&s_mux);
    s_have_peer = false;
    s_peer = Device{};
    s_forgotten = true;
    bumpLocked();
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  interruptConnect();
  post(CmdType::Forget);
}

void setLayout(Layout l) {
  if ((size_t)l >= (size_t)Layout::Count) l = Layout::US;
  portENTER_CRITICAL(&s_mux);
  s_layout = l;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

Layout layout() {
  portENTER_CRITICAL(&s_mux);
  const Layout l = s_layout;
  portEXIT_CRITICAL(&s_mux);
  return l;
}

const char* layoutName(Layout l) {
  const size_t i = (size_t)l < (size_t)Layout::Count ? (size_t)l : 0;
  return kLayouts[i].name;
}

void setBackKey(uint8_t usage) {
  portENTER_CRITICAL(&s_mux);
  s_back_usage = usage;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

uint8_t backKey() {
  portENTER_CRITICAL(&s_mux);
  const uint8_t u = s_back_usage;
  portEXIT_CRITICAL(&s_mux);
  return u;
}

void captureKey(bool on) {
  portENTER_CRITICAL(&s_mux);
  s_capture = on;
  if (on) s_have_captured = false;
  bumpLocked();
  portEXIT_CRITICAL(&s_mux);
}

bool capturingKey() {
  portENTER_CRITICAL(&s_mux);
  const bool on = s_capture;
  portEXIT_CRITICAL(&s_mux);
  return on;
}

bool takeCapturedKey(uint8_t* usage) {
  portENTER_CRITICAL(&s_mux);
  const bool have = s_have_captured;
  s_have_captured = false;
  if (have && usage) *usage = s_captured;
  portEXIT_CRITICAL(&s_mux);
  return have;
}

void keyName(uint8_t usage, char* out, size_t cap) {
  if (!out || !cap) return;
  switch (usage) {
    case 0x28: snprintf(out, cap, "Enter");     return;
    case 0x29: snprintf(out, cap, "Esc");       return;
    case 0x2A: snprintf(out, cap, "Backspace"); return;
    case 0x2B: snprintf(out, cap, "Tab");       return;
    case 0x2C: snprintf(out, cap, "Space");     return;
    case 0x49: snprintf(out, cap, "Insert");    return;
    case 0x4A: snprintf(out, cap, "Home");      return;
    case 0x4B: snprintf(out, cap, "Page Up");   return;
    case 0x4C: snprintf(out, cap, "Delete");    return;
    case 0x4D: snprintf(out, cap, "End");       return;
    case 0x4E: snprintf(out, cap, "Page Down"); return;
    default: break;
  }
  if (usage >= 0x3A && usage <= 0x45) { snprintf(out, cap, "F%u", (unsigned)(usage - 0x3A + 1)); return; }
  if (usage >= 0x68 && usage <= 0x73) { snprintf(out, cap, "F%u", (unsigned)(usage - 0x68 + 13)); return; }
  uint32_t cp = mapKey(layout(), usage, false, false, false) & ~kDeadFlag;
  if (cp >= 0x20 && cp != 0x7F) {   // the character it types, as UTF-8
    char u[5] = {};
    if (cp < 0x80) {
      u[0] = (char)cp;
    } else if (cp < 0x800) {
      u[0] = (char)(0xC0 | (cp >> 6));
      u[1] = (char)(0x80 | (cp & 0x3F));
    } else {
      u[0] = (char)(0xE0 | (cp >> 12));
      u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      u[2] = (char)(0x80 | (cp & 0x3F));
    }
    snprintf(out, cap, "%s", u);
    return;
  }
  snprintf(out, cap, "0x%02X", (unsigned)usage);
}

int readKey() {
  if (s_tail != s_head) {
    const int code = s_ring[s_tail];
    s_tail = (uint8_t)((s_tail + 1) % kRing);
    return code;
  }
  const uint32_t now = millis();
  int code = 0;
  portENTER_CRITICAL(&s_mux);
  if (s_held_code && (uint32_t)(now - s_held_since) >= kRepeatDelayMs &&
      (int32_t)(now - s_next_repeat) >= 0) {
    code = s_held_code;
    s_next_repeat = now + kRepeatRateMs;
  }
  portEXIT_CRITICAL(&s_mux);
  return code;
}

bool takeNewPairing(Device* out) {
  portENTER_CRITICAL(&s_mux);
  const bool fresh = s_new_pairing;
  if (fresh) {
    s_new_pairing = false;
    if (out) *out = s_peer;
  }
  portEXIT_CRITICAL(&s_mux);
  return fresh;
}

bool takeForgotten() {
  portENTER_CRITICAL(&s_mux);
  const bool gone = s_forgotten;
  s_forgotten = false;
  portEXIT_CRITICAL(&s_mux);
  return gone;
}

void suspend(bool on) {
  if (s_suspended == on) return;
  s_suspended = on;
  if (on) {
    interruptConnect();
    if (NimBLEDevice::getInitialized() && isLinkUp()) s_client->disconnect();
  } else if (s_cmdq) {
    post(CmdType::Activate);   // wakes the worker, which reconnects
  }
}

bool linkUp() { return NimBLEDevice::getInitialized() && isLinkUp(); }

void stackGoingDown() {
  if (!s_task) return;
  setActive(false);
  // Wait for the worker to let go of the client before NimBLE deletes it.
  for (int i = 0; i < 40 && (s_busy || uxQueueMessagesWaiting(s_cmdq) > 0); i++)
    vTaskDelay(pdMS_TO_TICKS(50));
  s_client = nullptr;
  forgetAttributes();
}

}  // namespace BleKbd

#endif  // CAP_BLE_KEYBOARD
