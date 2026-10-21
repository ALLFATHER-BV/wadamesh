#pragma once

// LilyGo T-Deck trackball input. The trackball is a BlackBerry-style optical
// encoder: rolling it pulses one of four direction GPIOs (up/down/left/right),
// and the centre press is the BOARD_BOOT button (GPIO0, == PIN_USER_BTN). The
// pulse pins are edge-counted in ISRs so no movement is lost between polls.
//
// The UI layer (UITask) owns the on-screen cursor and turns the centre click
// into a touch event; this driver only reports oriented motion deltas.
#if defined(HAS_TDECK_TRACKBALL) && defined(ESP32)

#include <stdint.h>

/** Configure the four direction GPIOs + edge-count interrupts. One-shot. */
void tdeckTrackballBegin();

/**
 * Drain the motion accumulated since the last call into (*dx, *dy), already
 * rotated into the visible (UI) frame via tdeckTrackballSetRotation(). Units are
 * encoder "steps" (the UI scales these to pixels). Returns true if there was any
 * motion this interval.
 */
bool tdeckTrackballReadMotion(int* dx, int* dy);

/** True while the trackball centre button is held (active-low GPIO0). */
bool tdeckTrackballClickHeld();

/**
 * Orient motion to the UI rotation (LVGL lv_disp_rot_t: 0/1/2/3) so rolling the
 * ball moves the cursor the way the user sees it. The T-Deck UI is fixed
 * landscape (ROT_270). Call once at boot.
 */
void tdeckTrackballSetRotation(uint8_t lvgl_rot);

/**
 * Switch the whole trackball off or back on (Settings > Keyboard > Trackball), for
 * a damaged ball that rolls and clicks by itself. Off: the direction interrupts are
 * detached, tdeckTrackballReadMotion() reports nothing and the centre click reads
 * as released, so no motion or click reaches anything that reads them. The UI's
 * GPIO0 read (UITask::loop) checks tdeckTrackballEnabled() the same way.
 */
void tdeckTrackballSetEnabled(bool on);
bool tdeckTrackballEnabled();

#endif
