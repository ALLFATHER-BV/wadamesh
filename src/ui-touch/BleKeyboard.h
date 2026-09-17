// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// External Bluetooth LE keyboard (HID over GATT host).
//
// Bluetooth serves either the phone app or a keyboard, never both at once; the
// UI owns that choice and calls setActive(). The work that blocks (connecting,
// pairing, service discovery) runs on this module's own task, and key reports
// arrive on the NimBLE host task, so the UI only ever polls: readKey() for
// input, generation() to know when to redraw, and the take*() calls to persist
// what changed.
//
// Only Bluetooth LE keyboards can work: the ESP32-S3 radio has no classic
// Bluetooth.

#include <stddef.h>
#include <stdint.h>
#include "device_caps.h"

// Serial trace of raw reports, the Report Map and every key handed to the UI.
// Off by default: typed text would land on the USB serial port the phone app
// also uses.
#ifndef BLE_KBD_TRACE
#define BLE_KBD_TRACE 0
#endif

// Key codes handed to the UI. Printable text arrives as BLE_KEY_TEXT | code
// point, already mapped to the keyboard's own layout, so the UI must insert it
// as is. Enter and Backspace keep the codes the built-in keyboards use.
constexpr int BLE_KEY_TEXT       = 0x40000000;
constexpr int BLE_KEY_SPECIAL    = 0x20000000;
constexpr int BLE_KEY_UP         = BLE_KEY_SPECIAL | 1;
constexpr int BLE_KEY_DOWN       = BLE_KEY_SPECIAL | 2;
constexpr int BLE_KEY_LEFT       = BLE_KEY_SPECIAL | 3;
constexpr int BLE_KEY_RIGHT      = BLE_KEY_SPECIAL | 4;
constexpr int BLE_KEY_HOME       = BLE_KEY_SPECIAL | 5;
constexpr int BLE_KEY_END        = BLE_KEY_SPECIAL | 6;
constexpr int BLE_KEY_PAGE_UP    = BLE_KEY_SPECIAL | 7;
constexpr int BLE_KEY_PAGE_DOWN  = BLE_KEY_SPECIAL | 8;
constexpr int BLE_KEY_DELETE     = BLE_KEY_SPECIAL | 9;
constexpr int BLE_KEY_ESC        = BLE_KEY_SPECIAL | 10;
constexpr int BLE_KEY_TAB        = BLE_KEY_SPECIAL | 11;
constexpr int BLE_KEY_SHIFT_TAB  = BLE_KEY_SPECIAL | 12;
constexpr int BLE_KEY_EMOJI      = BLE_KEY_SPECIAL | 13;   // Command (Windows) key tapped on its own
constexpr int BLE_KEY_ENTER      = 0x0D;
constexpr int BLE_KEY_BACKSPACE  = 0x08;

#if CAP_BLE_KEYBOARD
namespace BleKbd {

enum class State : uint8_t {
  Off,         // keyboard mode not active
  Idle,        // active, nothing paired yet
  Waiting,     // active, paired keyboard not connected; retrying in the background
  Scanning,    // looking for keyboards in pairing mode
  Connecting,  // opening the link to the keyboard the user picked
  Pairing,     // link open, pairing in progress (a code may be on screen)
  Connected,   // typing works
  Failed,      // the last pairing attempt failed; see lastError()
};

// Why the last attempt failed; the UI turns it into a translated sentence.
enum class Error : uint8_t {
  None,
  NotRunning,     // the Bluetooth stack is not up
  Connect,        // the link could not be opened
  PairingCode,    // pairing with a code failed
  PairingNoCode,  // pairing without a code failed
  NotKeyboard,    // linked, but the device offers no keyboard input
  NeedsPairing,   // this side lost the bond; pair the keyboard again
};

enum class Layout : uint8_t { US = 0, UK, DE, FR, BE, Count };

struct Device {
  char    name[32];
  uint8_t addr[6];
  uint8_t addr_type;
  int8_t  rssi;
  bool    keyboard;   // advertises the keyboard appearance, not just HID
};

// Keyboard mode on/off. Starts the worker the first time it is switched on.
void   setActive(bool on);
bool   active();
State  state();
// Changes whenever anything the pairing screen shows changes.
uint32_t generation();
Error  lastError();

// The keyboard to reconnect to, loaded from settings at boot. nullptr clears it.
void   setPaired(const Device* dev);
bool   paired(Device* out);

// Pairing screen.
void   scan(bool on);
int    deviceCount();
bool   deviceAt(int i, Device* out);
void   pair(int i, bool with_code);
// The 6-digit code to type on the keyboard, or 0 when none is waiting.
uint32_t pairingCode();
// Drop the current keyboard and delete its bond.
void   forget();

void        setLayout(Layout l);
Layout      layout();
const char* layoutName(Layout l);

// A key that goes back besides Esc (its HID usage, 0 = none), for keyboards
// whose Esc key sends another code.
void    setBackKey(uint8_t usage);
uint8_t backKey();
// Picking that key: while on, the next key pressed is kept for
// takeCapturedKey() instead of being typed.
void    captureKey(bool on);
bool    capturingKey();
bool    takeCapturedKey(uint8_t* usage);
// A short name for a key: "Esc", "F1", or what it types in the current layout.
void    keyName(uint8_t usage, char* out, size_t cap);

// UI loop.
int    readKey();                // next key, 0 when none
bool   takeNewPairing(Device* out);   // true once after a successful pairing
bool   takeForgotten();               // true once after forget()

// Call right before NimBLEDevice::deinit(), which deletes the client.
void   stackGoingDown();
// Take the keyboard off the air for a while (the Pager's Wi-Fi handoff), then
// let it reconnect. linkUp() tells when the link is really gone.
void   suspend(bool on);
bool   linkUp();

}  // namespace BleKbd
#endif  // CAP_BLE_KEYBOARD
