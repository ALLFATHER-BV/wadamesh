// LilyGo T-Deck trackball driver — see TDeckTrackball.h.
#if defined(HAS_TDECK_TRACKBALL) && defined(ESP32)

#include "TDeckTrackball.h"
#include <Arduino.h>

// Direction GPIOs (LilyGo T-Deck): G01=15 up, G02=3 down, G03=2 left, G04=1
// right, in the device's native PORTRAIT frame. Overridable from build flags.
#ifndef PIN_TB_UP
  #define PIN_TB_UP 15
#endif
#ifndef PIN_TB_DOWN
  #define PIN_TB_DOWN 3
#endif
#ifndef PIN_TB_LEFT
  #define PIN_TB_LEFT 2
#endif
#ifndef PIN_TB_RIGHT
  #define PIN_TB_RIGHT 1
#endif
#ifndef PIN_TB_CLICK
  #define PIN_TB_CLICK 0   // BOARD_BOOT / PIN_USER_BTN
#endif

static volatile uint32_t s_cnt_up = 0, s_cnt_down = 0, s_cnt_left = 0, s_cnt_right = 0;

static void IRAM_ATTR isrUp()    { ++s_cnt_up; }
static void IRAM_ATTR isrDown()  { ++s_cnt_down; }
static void IRAM_ATTR isrLeft()  { ++s_cnt_left; }
static void IRAM_ATTR isrRight() { ++s_cnt_right; }

static bool    s_inited = false;
static bool    s_enabled = true;    // Settings > Keyboard > Trackball
static bool    s_isr_on  = false;   // the direction interrupts are attached
static uint8_t s_rot = 0;

static void attachIsrs(bool on) {
  if (on == s_isr_on) return;
  s_isr_on = on;
  if (on) {
    attachInterrupt(digitalPinToInterrupt(PIN_TB_UP),    isrUp,    FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TB_DOWN),  isrDown,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TB_LEFT),  isrLeft,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TB_RIGHT), isrRight, FALLING);
  } else {
    detachInterrupt(digitalPinToInterrupt(PIN_TB_UP));
    detachInterrupt(digitalPinToInterrupt(PIN_TB_DOWN));
    detachInterrupt(digitalPinToInterrupt(PIN_TB_LEFT));
    detachInterrupt(digitalPinToInterrupt(PIN_TB_RIGHT));
  }
  noInterrupts();
  s_cnt_up = s_cnt_down = s_cnt_left = s_cnt_right = 0;   // nothing counted while off carries over
  interrupts();
}

void tdeckTrackballBegin() {
  if (s_inited) return;
  s_inited = true;
  pinMode(PIN_TB_UP, INPUT_PULLUP);
  pinMode(PIN_TB_DOWN, INPUT_PULLUP);
  pinMode(PIN_TB_LEFT, INPUT_PULLUP);
  pinMode(PIN_TB_RIGHT, INPUT_PULLUP);
  pinMode(PIN_TB_CLICK, INPUT_PULLUP);
  attachIsrs(s_enabled);
}

void tdeckTrackballSetEnabled(bool on) {
  s_enabled = on;
  if (s_inited) attachIsrs(on);
}

bool tdeckTrackballEnabled() { return s_enabled; }

void tdeckTrackballSetRotation(uint8_t lvgl_rot) { s_rot = lvgl_rot & 3; }

bool tdeckTrackballReadMotion(int* dx, int* dy) {
  if (!s_enabled) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return false;
  }
  // Atomically snapshot + clear the ISR counters.
  noInterrupts();
  uint32_t u = s_cnt_up, d = s_cnt_down, l = s_cnt_left, r = s_cnt_right;
  s_cnt_up = s_cnt_down = s_cnt_left = s_cnt_right = 0;
  interrupts();

  // Portrait-frame delta: +x right, +y down.
  int pdx = (int)r - (int)l;
  int pdy = (int)d - (int)u;
  if (pdx == 0 && pdy == 0) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return false;
  }

  // Orient into the visible frame. The ROT_270 case is calibrated against the
  // hardware: on the T-Deck the encoder axes line up with the landscape screen
  // but are inverted on BOTH axes (a 180°), so left/right/up/down map directly
  // once negated — confirmed on-device. The other cases are derived guesses for
  // completeness (the T-Deck only ever runs ROT_270).
  int sx, sy;
  switch (s_rot) {
    case 1:  sx =  pdy; sy = -pdx; break;  // ROT_90
    case 2:  sx = -pdy; sy =  pdx; break;  // 180
    case 3:  sx = -pdx; sy = -pdy; break;  // ROT_270 (T-Deck default, measured)
    default: sx =  pdx; sy =  pdy; break;  // portrait
  }
  if (dx) *dx = sx;
  if (dy) *dy = sy;
  return true;
}

bool tdeckTrackballClickHeld() {
  if (!s_enabled) return false;
  return digitalRead(PIN_TB_CLICK) == LOW;  // active-low
}

#endif
