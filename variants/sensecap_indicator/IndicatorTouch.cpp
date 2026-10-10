// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_SENSECAP_INDICATOR) && defined(ESP32)

#include <helpers/input/HeltecV4CapTouch.h>
#include <helpers/ui/MomentaryButton.h>

// Placeholder until the FT6336 reader lands: the touch API the UI calls, with no
// touch behind it.

bool heltecV4CapTouchBegin() { return false; }
int heltecV4CapTouchCheck() { return BUTTON_EVENT_NONE; }
bool heltecV4CapTouchPopTap(uint16_t*, uint16_t*) { return false; }
bool heltecV4CapTouchGetLive(uint16_t*, uint16_t*) { return false; }
bool heltecV4CapTouchPopSwipe(int8_t*, int8_t*) { return false; }
bool heltecV4CapTouchStartBackgroundPoll(uint32_t) { return false; }
bool heltecV4CapTouchIsAsyncPolling() { return false; }
bool heltecV4CapTouchIsSwiping() { return false; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool) {}
const char* heltecV4CapTouchDebug() { return "SenseCAP Indicator (no touch yet)"; }
void heltecV4CapTouchGetRaw(uint16_t* x, uint16_t* y) { if (x) *x = 0; if (y) *y = 0; }

#endif
