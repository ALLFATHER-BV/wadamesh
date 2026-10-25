// SPDX-License-Identifier: GPL-3.0-or-later
//
// T-LoRa Pager IMU — Bosch BHI260AP smart sensor hub on the shared I2C bus
// (Wire, SDA 3 / SCL 2) at 0x28, interrupt on GPIO 8 (unused here: polled).
//
// The BHI260AP is a microcontroller of its own and boots from RAM: until the
// host uploads its firmware (~120 KB, the Klio image LilyGoLib uses on this
// board), it reports nothing. That upload takes a second or two, so it is NOT
// done at boot — nothing on the device needs the IMU unless an app asks — but on
// the first read. The chip then keeps its firmware until power-off.
//
// Same contract as the ThinkNode M9's accelerometer (M9Imu.h), so the Lua
// bridge (wada.sys.accel()) needs nothing board-specific: acceleration in g.
// Unlike the M9, the axes here are mapped to the device by the hub itself
// (LilyGoLib's own remap for this board: BOTTOM_LAYER_TOP_LEFT_CORNER).
#pragma once

#if defined(TLORA_PAGER) && defined(ESP32)

/** Latest acceleration in g. The first call uploads the hub firmware (blocks
 *  for a second or two, once per boot) and starts the accelerometer; later
 *  calls are cheap. False = no chip, upload failed, or nothing fresh yet. */
bool pagerImuRead(float* x_g, float* y_g, float* z_g);

/** Call periodically (cheap until it acts). Stops the accelerometer a few
 *  seconds after the last read, so the hub idles while no app wants tilt. */
void pagerImuIdleTick();

/** True once the hub firmware upload has been tried this boot, i.e. the next
 *  pagerImuRead() will not block for it. */
bool pagerImuStarted();

#endif
