#pragma once
// Pointer + keyboard state shared by the SDL event pump (host_main.cpp) and the
// board input drivers the UI calls (WebTouch.cpp: the heltecV4CapTouch* touch API,
// WebKeyboard.cpp: the CardKB external-keyboard API). All on the UI thread.
#include <stdint.h>
void webPointer(bool down, int x, int y);   // in canvas pixels
void webKeyPush(uint8_t cardkb_code);        // CardKB codes: ASCII, 0x08 BS, 0x0D Enter, 0x1B Esc, 0xB4-0xB7 arrows
