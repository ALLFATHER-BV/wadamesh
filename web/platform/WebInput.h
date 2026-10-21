#pragma once
// Pointer + keyboard state shared by the SDL event pump (host_main.cpp) and the
// board input drivers the UI calls (WebTouch.cpp: the heltecV4CapTouch* touch API,
// WebKeyboard.cpp: the CardKB external-keyboard API). All on the UI thread.
#include <stdint.h>
void webPointer(bool down, int x, int y);   // in canvas pixels
// CardKB codes (ASCII, 0x08 BS, 0x0D Enter, 0x1B Esc, 0xB4-0xB7 arrows), or text as
// WADA_KEY_TEXT | code point (any script, emoji: UITask's cardKbUiKey passes it through).
constexpr int WADA_KEY_TEXT = 0x40000000;   // = BleKeyboard.h BLE_KEY_TEXT
void webKeyPush(int code);
void webTextPush(const char* utf8);          // each code point as typed text
// The phone's own keyboard types (web/shell/app.js): the UI keeps its on-screen keys
// down, as for any external keyboard.
void webKeyboardPresent(bool on);
// Mouse wheel / trackpad scroll at (x, y) in canvas pixels, dy > 0 = towards the end
// of a list (WebLvglInput.cpp). webWheelTick: once per board step.
void webWheel(int x, int y, int dy);
void webWheelTick();
// Text fields for the phone keyboard (WebLvglInput.cpp, LVGL side): a textarea under (x, y);
// whether the UI's keyboard is bound to a field.
bool webTextareaAt(int x, int y);
bool webEditing();
bool webOskShown();   // the UI's own on-screen keys are up (tests)
int webFindText(const char* text);   // tests: the middle of a visible label, (x << 16) | y, or -1
const char* webEditText();          // tests: the text of the field being edited, or ""
