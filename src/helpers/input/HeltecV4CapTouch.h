#pragma once

// Touch-input API for the LVGL touch UI. The declarations are shared across
// touch boards (Heltec V4 CHSC6x, LilyGo T-Deck GT911, …); each board links its
// own implementation. Available for any touch-UI build.
#if (defined(HAS_HELTEC_V4_CAP_TOUCH) || defined(HAS_TOUCH_UI)) && defined(ESP32)

#include <stdint.h>

/** Initializes CHSC6x touch controller. Returns true if touch is available. */
bool heltecV4CapTouchBegin();
/** Returns BUTTON_EVENT_* (same as MomentaryButton::check). */
int heltecV4CapTouchCheck();
bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y);
/** True while finger is down; returns latest mapped display coordinates. */
bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y);
/**
 * Pop pending swipe gesture in display coordinates.
 * x_dir: -1 left, +1 right, 0 none
 * y_dir: -1 up, +1 down, 0 none
 */
bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir);

/**
 * Spawn a pinned FreeRTOS task that owns the touch I2C bus and polls
 * heltecV4CapTouchCheck() at `period_ms` intervals (clamped to [4, 100] ms),
 * independent of LVGL's render tick. Pinned to core 0 so it can't be starved
 * by mesh.loop() or the LVGL task (Arduino loop) on core 1.
 *
 * Once the task is running, lvglTouchRead (and any other caller) should
 * skip the inline heltecV4CapTouchCheck() and just consume cached state via
 * heltecV4CapTouchGetLive / heltecV4CapTouchPopTap / heltecV4CapTouchPopSwipe.
 * Query the state with heltecV4CapTouchIsAsyncPolling().
 *
 * Returns true on first successful start; false if already running or the
 * hardware isn't initialised yet.
 */
bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms);

/** True if heltecV4CapTouchStartBackgroundPoll() has spawned its task. */
bool heltecV4CapTouchIsAsyncPolling();

/**
 * True once an in-progress gesture's displacement has exceeded the swipe
 * threshold. Callers (LVGL indev) use this to abort any pending click on
 * the originally pressed widget — the gesture is a swipe, not a tap.
 * Resets to false when the finger lifts.
 */
bool heltecV4CapTouchIsSwiping();

/**
 * Tell the driver the current UI rotation (LVGL lv_disp_rot_t code:
 * 0 = portrait, 1 = 90, 2 = 180, 3 = 270) so swipe-direction detection
 * matches what the user sees. Touch coordinates reported to LVGL stay in the
 * raw panel space (LVGL's sw_rotate transforms them); only the swipe gesture's
 * dominant-axis / left-right decision is rotated by this value. Call whenever
 * the display rotation changes (boot + on a rotation toggle). Default 0.
 */
void heltecV4CapTouchSetRotation(uint8_t lvgl_rot);
void heltecV4CapTouchSetSlowPoll(bool slow);   // screen-off poll throttle (R8 shared-bus)

/**
 * Set the HARDWARE-rotation touch-point transform. When the panel is rotated
 * in hardware (the global landscape orientation), LVGL does not rotate the
 * touch point, so the driver maps the raw panel coordinate into the rotated
 * logical frame before LVGL sees it. Pass the LVGL rotation code (0/1/2/3).
 * Leave 0 in portrait (the keyboard's transient landscape uses LVGL sw_rotate,
 * which transforms the point itself — double-transforming would break touch).
 */
void heltecV4CapTouchSetPointRotation(uint8_t lvgl_rot);

/** Human-readable touch-init diagnostic (e.g. the I2C bus scan result on the
 *  T-Deck). Shown in the on-screen diag panel. Empty on boards that don't
 *  provide one. */
const char* heltecV4CapTouchDebug();

/** Last RAW touch point (pre-mapping), for on-screen coordinate calibration.
 *  Writes 0,0 on boards that don't expose raw coordinates. */
void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry);

#endif
