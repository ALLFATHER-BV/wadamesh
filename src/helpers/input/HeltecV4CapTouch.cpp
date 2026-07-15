#if defined(HAS_HELTEC_V4_CAP_TOUCH) && defined(ESP32)

#include "HeltecV4CapTouch.h"
#include <Arduino.h>
#include <Wire.h>
#include <chsc6x.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_SDA
  #error PIN_TOUCH_SDA required for HAS_HELTEC_V4_CAP_TOUCH
#endif
#ifndef PIN_TOUCH_SCL
  #error PIN_TOUCH_SCL required for HAS_HELTEC_V4_CAP_TOUCH
#endif
#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST -1
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT -1
#endif

// --- Which I2C controller the CHSC6x lives on -------------------------------
// Plain V4: the touch has its OWN controller (Wire1) on dedicated pins 47/48,
// physically separate from the board bus (Wire on 4/3) — two controllers on two
// pad-pairs is fine, so it stays on Wire1.
// V4-R8 (Expansion Kit V2): touch + RTC + env-sensors are all unified onto
// GPIO17/18. Running the touch on a SECOND controller (Wire1) bound to the SAME
// pads as the board's Wire let the core-0 touch-poll task and the core-1 RTC/
// sensor reads collide on the wire — SDA stuck low = an unrecoverable I2C wedge,
// i.e. the "random freezes that need a reset". Share the ONE board controller
// (Wire) instead: arduino-esp32's per-instance TwoWire mutex then serialises
// every transaction (repeated-starts included) across both cores — exactly the
// proven T-Deck pattern (GT911 touch + keyboard + sensors all on one Wire).
#if defined(HELTEC_LORA_V4_R8)
  #define V4CAP_WIRE Wire
#else
  #define V4CAP_WIRE Wire1
#endif

#ifndef HELTEC_V4_TOUCH_LONG_MS
  #define HELTEC_V4_TOUCH_LONG_MS 1000
#endif
#ifndef HELTEC_V4_TOUCH_LONG_MOVE_MAX
  #define HELTEC_V4_TOUCH_LONG_MOVE_MAX 18
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_MIN
  #define HELTEC_V4_TOUCH_SWIPE_MIN 36
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_INVERT
  #define HELTEC_V4_TOUCH_SWIPE_INVERT 0
#endif

static chsc6x* s_tp;
static bool s_init_attempted = false;
static bool s_init_ok = false;
static unsigned long s_last_init_try_ms = 0;
static bool s_tap_pending = false;
static uint16_t s_tap_x = 0, s_tap_y = 0;
static bool s_swipe_pending = false;
static int8_t s_swipe_x = 0, s_swipe_y = 0;
static bool s_live = false;
static uint16_t s_live_x = 0, s_live_y = 0;
// Set during a gesture once displacement exceeds the swipe threshold AND the
// dominant axis is horizontal — LVGL uses this to cancel any pending click on
// the originally pressed widget so a side-swipe across a button row doesn't
// both switch tabs AND click the row. Vertical drags are NOT marked, so LVGL
// can run its native scroll on lists (natural drag-follows-finger behaviour).
static bool s_swiping_now = false;

// Current UI rotation (LVGL lv_disp_rot_t: 0/1/2/3). Swipe-direction detection
// rotates the raw panel-space delta into the user-visible (logical) frame so a
// horizontal swipe in landscape is recognised as horizontal. Coordinates fed
// to LVGL are unaffected (LVGL's sw_rotate handles those).
static uint8_t s_ui_rotation = 0;
void heltecV4CapTouchSetRotation(uint8_t lvgl_rot) { s_ui_rotation = lvgl_rot & 3; }

// Rotate a panel-space (dx, dy) into the logical/visible frame.
// NOTE: the raw CHSC6x frame is already 180 deg relative to the panel's
// rotation-0 (portrait uses panel rotation 2, and mapTouchToDisplay is a
// no-op for that case), so the landscape orientations use the "+180" math:
// ROT_90 (1) gets the 270-degree formula and vice-versa. Portrait stays
// identity. (Verified on hardware — without the flip, taps landed at the
// diagonally-opposite point.)
static void rotateSwipeDelta(int dx, int dy, int* odx, int* ody) {
  switch (s_ui_rotation) {
    case 1:  *odx = -dy; *ody =  dx; break;  // ROT_90  (panel-rot-2 baseline)
    case 2:  *odx = -dx; *ody = -dy; break;  // 180
    case 3:  *odx =  dy; *ody = -dx; break;  // ROT_270
    default: *odx =  dx; *ody =  dy; break;  // portrait
  }
}

// Hardware-rotation touch-point transform. When the panel is rotated in
// HARDWARE (the global landscape orientation), LVGL no longer rotates the
// touch point, so the driver must map the raw panel-space coordinate (portrait
// DISPLAY_WIDTH x DISPLAY_HEIGHT) into the rotated logical frame before handing
// it to LVGL. Stays 0 (identity) in portrait — there the keyboard's transient
// landscape uses LVGL sw_rotate, which transforms the point itself.
static uint8_t s_point_rotation = 0;
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }

static void applyPointRotation(uint16_t* x, uint16_t* y) {
  if (s_point_rotation == 0) return;
  const int W = 240;    // V4 panel portrait width
  const int H = 320;    // V4 panel portrait height
  const int px = *x, py = *y;
  int lx, ly;
  // Same +180 baseline as rotateSwipeDelta: ROT_90 uses the 270-degree map.
  switch (s_point_rotation) {
    case 1:  lx = (H - 1) - py;  ly = px;           break;  // ROT_90  -> 320x240
    case 2:  lx = (W - 1) - px;  ly = (H - 1) - py; break;  // 180
    case 3:  lx = py;            ly = (W - 1) - px; break;  // ROT_270 -> 320x240
    default: lx = px;            ly = py;           break;
  }
  if (lx < 0) lx = 0;
  if (ly < 0) ly = 0;
  *x = (uint16_t)lx;
  *y = (uint16_t)ly;
}

static void mapTouchToDisplay(uint16_t in_x, uint16_t in_y, uint16_t* out_x, uint16_t* out_y) {
#ifndef DISPLAY_WIDTH
  #define DISPLAY_WIDTH 240
#endif
#ifndef DISPLAY_HEIGHT
  #define DISPLAY_HEIGHT 320
#endif
#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 0
#endif

  int x = in_x;
  int y = in_y;
  int w = DISPLAY_WIDTH;
  int h = DISPLAY_HEIGHT;
  switch (DISPLAY_ROTATION & 3) {
    case 1: {
      int nx = h - 1 - y;
      int ny = x;
      x = nx;
      y = ny;
    } break;
    case 2:
#if defined(UI_LVGL)
      // LVGL touch UI path already uses the ST7789 rotated coordinate space.
      // Applying another 180-degree transform here mirrors taps incorrectly.
      break;
#else
      x = w - 1 - x;
      y = h - 1 - y;
      break;
#endif
    case 3: {
      int nx = y;
      int ny = w - 1 - x;
      x = nx;
      y = ny;
    } break;
    default:
      break;
  }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w) x = w - 1;
  if (y >= h) y = h - 1;
  if (out_x) *out_x = (uint16_t)x;
  if (out_y) *out_y = (uint16_t)y;
}

static bool probeTouchControllerAddress() {
  // Known CHSC6x addresses seen across board variants/libraries.
  static const uint8_t kAddrCandidates[] = {0x2E, 0x15, 0x14};
  for (uint8_t i = 0; i < sizeof(kAddrCandidates); i++) {
    V4CAP_WIRE.beginTransmission(kAddrCandidates[i]);
    uint8_t rc = V4CAP_WIRE.endTransmission();
    if (rc == 0) return true;
  }
  return false;
}

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;
  unsigned long now = millis();
  if (s_init_attempted && (unsigned long)(now - s_last_init_try_ms) < 400) return false;
  s_init_attempted = true;
  s_last_init_try_ms = now;

  // Ensure bus is configured and bounded so failed probes never wedge boot/runtime.
  V4CAP_WIRE.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 100000);
#if defined(ESP32)
  V4CAP_WIRE.setTimeOut(20);  // milliseconds
#endif
  if (PIN_TOUCH_INT >= 0) pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

  // If controller is absent or bus is unhealthy, skip init and keep UI alive.
  if (!probeTouchControllerAddress()) {
    s_tp = nullptr;
    s_init_ok = false;
    return false;
  }

  // Use our own INT pin handling above; avoid library-level attachInterrupt side effects.
  // Keep reset pin support so controller can still be hard-reset.
  static chsc6x instance_safe(&V4CAP_WIRE, PIN_TOUCH_SDA, PIN_TOUCH_SCL, -1, PIN_TOUCH_RST);
  s_tp = &instance_safe;
  s_tp->chsc6x_init();
  // chsc6x_init() re-runs Wire begin internally, so re-apply bounded bus settings.
  V4CAP_WIRE.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 100000);
#if defined(ESP32)
  V4CAP_WIRE.setTimeOut(20);
#endif
  s_init_ok = true;
  return true;
}

int heltecV4CapTouchCheck() {
  if (!s_tp) {
    s_live = false;
    return BUTTON_EVENT_NONE;
  }

  static bool down = false;
  static unsigned long down_at = 0;
  static bool long_dispatched = false;
  static uint16_t start_x = 0, start_y = 0;
  static uint16_t last_x = 0, last_y = 0;
  // Release debounce: at 125 Hz polling the CHSC6X occasionally returns an
  // I2C-flaky "no touch" mid-gesture even while the finger is on screen.
  // Without debouncing this fires a release (and swipe detection!) followed
  // by a new press when the next poll succeeds — symptom is one finger
  // gesture registering as two swipes / two taps. Require N consecutive
  // misses before treating the finger as released. With 8 ms polling, N=2
  // tolerates a single flaky read = 16 ms blip.
  static uint8_t release_misses = 0;
  constexpr uint8_t RELEASE_DEBOUNCE = 2;

  uint16_t x = 0, y = 0;
  int r = s_tp->chsc6x_read_touch_info(&x, &y);
  bool now = (r == 0);

  // If we're currently down and the poll says "no touch", give the chip one
  // or two more cycles to confirm before declaring a release.
  if (!now && down && release_misses < RELEASE_DEBOUNCE) {
    ++release_misses;
    return BUTTON_EVENT_NONE;
  }
  if (now) release_misses = 0;

  if (now && !down) {
    down = true;
    down_at = millis();
    long_dispatched = false;
    start_x = last_x = x;
    start_y = last_y = y;
    s_swiping_now = false;
    mapTouchToDisplay(start_x, start_y, &s_live_x, &s_live_y);
    applyPointRotation(&s_live_x, &s_live_y);
    s_live = true;
  } else if (now && down) {
    last_x = x;
    last_y = y;
    mapTouchToDisplay(last_x, last_y, &s_live_x, &s_live_y);
    applyPointRotation(&s_live_x, &s_live_y);
    s_live = true;
    // Flag a HORIZONTAL swipe-in-progress as soon as horizontal displacement
    // crosses the swipe threshold AND is the dominant axis. Vertical drags
    // are deliberately NOT flagged so LVGL keeps its native vertical scroll
    // on lists (drag-follows-finger).
    if (!s_swiping_now) {
      int rdx = static_cast<int>(last_x) - static_cast<int>(start_x);
      int rdy = static_cast<int>(last_y) - static_cast<int>(start_y);
      int dx, dy;
      rotateSwipeDelta(rdx, rdy, &dx, &dy);   // panel-space -> logical/visible
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      if (adx >= HELTEC_V4_TOUCH_SWIPE_MIN && adx > ady) {
        s_swiping_now = true;
      }
    }
  } else if (!now && down) {
    s_live = false;
    down = false;
    s_swiping_now = false;
    unsigned long dur = (down_at > 0) ? (unsigned long)(millis() - down_at) : 0;
    down_at = 0;
    uint16_t sx = 0, sy = 0, ex = 0, ey = 0;
    mapTouchToDisplay(start_x, start_y, &sx, &sy);
    mapTouchToDisplay(last_x, last_y, &ex, &ey);
    int rdx = (int)ex - (int)sx;
    int rdy = (int)ey - (int)sy;
    int dx, dy;
    rotateSwipeDelta(rdx, rdy, &dx, &dy);   // panel-space -> logical/visible
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (!long_dispatched && adx >= HELTEC_V4_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool swipe_left = dx < 0;
#if HELTEC_V4_TOUCH_SWIPE_INVERT
      swipe_left = !swipe_left;
#endif
      s_swipe_x = swipe_left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      // Reuse button events as directional gestures:
      // DOUBLE_CLICK -> swipe left, TRIPLE_CLICK -> swipe right.
      return swipe_left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (!long_dispatched && ady >= HELTEC_V4_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = (dy < 0) ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    // Tap minimum 12 ms (was 30 ms): real screen taps can be very brief and
    // anything tighter than ~10 ms is debounced by the 2-poll release miss
    // counter anyway, so accidental brushes won't slip through.
    if (!long_dispatched && dur >= 12 && dur < (unsigned long)HELTEC_V4_TOUCH_LONG_MS) {
      s_tap_x = ex;
      s_tap_y = ey;
      applyPointRotation(&s_tap_x, &s_tap_y);
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    long_dispatched = false;
  } else if (down && !long_dispatched && down_at > 0 &&
             (unsigned long)(millis() - down_at) >= (unsigned long)HELTEC_V4_TOUCH_LONG_MS) {
    int dx = (int)last_x - (int)start_x;
    int dy = (int)last_y - (int)start_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx > HELTEC_V4_TOUCH_LONG_MOVE_MAX || ady > HELTEC_V4_TOUCH_LONG_MOVE_MAX) {
      return BUTTON_EVENT_NONE;
    }
    long_dispatched = true;
    return BUTTON_EVENT_LONG_PRESS;
  }

  return BUTTON_EVENT_NONE;
}

bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tap_pending) return false;
  s_tap_pending = false;
  if (x) *x = s_tap_x;
  if (y) *y = s_tap_y;
  return true;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false;
  if (x) *x = s_live_x;
  if (y) *y = s_live_y;
  return true;
}

bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir) {
  if (!s_swipe_pending) return false;
  s_swipe_pending = false;
  if (x_dir) *x_dir = s_swipe_x;
  if (y_dir) *y_dir = s_swipe_y;
  s_swipe_x = 0;
  s_swipe_y = 0;
  return true;
}

// ============================================================
// Async background polling
//
// Decouples touch I/O from LVGL's render cadence. The task is the sole owner
// of the chsc6x driver state machine while it runs (callers that previously
// invoked heltecV4CapTouchCheck() inline must skip when heltecV4CapTouchIsAsyncPolling
// is true).
//
// Pinned to core 0 because Arduino's loop() — which runs LVGL and the mesh
// stack — runs on core 1 by default. Separating them means a 100 ms mesh
// burst no longer starves touch sampling.
// ============================================================
static TaskHandle_t s_poll_task = nullptr;
static uint32_t     s_poll_period_ms = 8;
static volatile bool s_async_active = false;

static void touchPollTaskFn(void* /*arg*/) {
  s_async_active = true;
  const TickType_t period = pdMS_TO_TICKS(s_poll_period_ms ? s_poll_period_ms : 8);
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    (void)heltecV4CapTouchCheck();
    vTaskDelayUntil(&last_wake, period);
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_poll_task != nullptr) return false;
  if (!s_init_ok) return false;
  if (period_ms < 4) period_ms = 4;
  if (period_ms > 100) period_ms = 100;
  s_poll_period_ms = period_ms;
  BaseType_t ok = xTaskCreatePinnedToCore(
      touchPollTaskFn,
      "touch_poll",
      4096,            // 4 KB stack — only calls heltecV4CapTouchCheck
      nullptr,
      1,                // low priority; we want to yield to LVGL/mesh easily
      &s_poll_task,
      0);               // core 0 (Arduino loop runs on core 1)
  if (ok != pdPASS) {
    s_poll_task = nullptr;
    return false;
  }
  return true;
}

bool heltecV4CapTouchIsAsyncPolling() {
  return s_async_active;
}

bool heltecV4CapTouchIsSwiping() {
  return s_swiping_now;
}

// The CHSC6x V4 driver has no bus-scan diagnostic to report; the symbol exists
// only so the shared touch-UI overlay (which prints heltecV4CapTouchDebug())
// links on every board.
const char* heltecV4CapTouchDebug() { return ""; }

// No raw-coordinate calibration readout on the V4; stub so the shared overlay links.
void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = 0;
  if (ry) *ry = 0;
}

#endif
