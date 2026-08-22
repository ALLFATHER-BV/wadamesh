#pragma once

// ThinkNode M9 magnetometer — QST QMC6309 on the peripheral I2C bus (Wire,
// SDA=7 / SCL=6) at 7-bit address 0x7C. The chip sits on the GPIO18 peripheral
// rail with the LCD/GPS, which ThinkNodeM9Board::begin() claims for the whole
// runtime, so it is powered whenever the firmware runs.
//
// This is deliberately a dumb sensor reader: it hands out the raw field vector
// in Gauss, sensor frame, uncalibrated. Hard-iron calibration, the sensor-to-
// screen axis mapping (not documented anywhere — Meshtastic's own M9 driver
// marks its heading offset "must be verified on real hardware" and never uses
// it) and the heading maths live in the consumer (the GPS Compass Lua app),
// where they can be adjusted and persisted per user without a firmware cut.
//
// Register map (QMC6309 datasheet Rev A, cross-checked against SlimeVR,
// madflight and the emfcamp Tildagon drivers — NOT against SensorLib, whose
// setOutputDataRate() writes the ODR into the wrong register):
//   0x00 chip id (0x90)      0x01..0x06 X/Y/Z int16 little-endian
//   0x09 status: bit0 DRDY (cleared by reading 0x09), bit1 OVFL
//   0x0A CTRL1: OSR2[7:5] OSR1[4:3] MODE[1:0]   (00 suspend, 01 normal, 11 cont.)
//   0x0B CTRL2: SOFT_RST[7] ODR[6:4] RNG[3:2] SET/RESET[1:0]
// Sensitivity at the ±32 G range used here: 1000 LSB/G (1 mG per count).
#if defined(HAS_M9_COMPASS) && defined(ESP32)

#include <stdint.h>

class TwoWire;

/** Probe + configure the chip on `w`. Safe to call when the chip is absent or
 *  not yet out of power-on reset: the read path re-probes on its own. Logs one
 *  line to Serial either way (matches the keyboard bring-up style). */
void m9CompassBegin(TwoWire& w);

/** True once the chip has answered with its id and taken the configuration. */
bool m9CompassPresent();

/** Latest field vector in Gauss, sensor frame, uncalibrated. Reads the chip
 *  synchronously when a new sample is ready (three short I2C transactions,
 *  well under 1 ms at 100 kHz on a healthy bus), otherwise returns the cached
 *  sample while it is younger than a second. False = no chip, bus error, or
 *  nothing fresh. `overflow` (optional) is set when the chip flagged the
 *  sample as saturated (an axis beyond ±32000 counts): the values are still
 *  returned so a consumer can show "away from magnets" rather than "no
 *  compass", but they are not a usable heading. */
bool m9CompassRead(float* x_gauss, float* y_gauss, float* z_gauss, bool* overflow = nullptr);

/** Call periodically (cheap: one comparison until it acts). Suspends the chip
 *  a couple of seconds after the last read, so it only draws its ~1 mA while
 *  something is actually using the compass. The next read wakes it. */
void m9CompassIdleTick();

#endif
