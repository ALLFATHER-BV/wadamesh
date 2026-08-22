#pragma once

// ThinkNode M9 IMU — QST QMI8658 on the peripheral I2C bus (Wire, SDA=7 /
// SCL=6) at 7-bit address 0x6B (SA0 grounded). Accelerometer only: it exists
// here to tell the compass which way is DOWN, so a heading can be corrected
// for tilt. Nothing needs the gyroscope, and leaving it off is most of the
// part's power budget.
//
// Register map (QMI8658A datasheet Rev D; the QMI8658C shares all of it,
// differing only in the post-reset delay and some tolerances):
//   0x00 WHO_AM_I (0x05)      0x01 REVISION_ID (0x7C on the A, other on the C)
//   0x02 CTRL1: bit6 ADDR_AI (burst reads need it), bit5 BE, bit0 SensorDisable
//   0x03 CTRL2: aFS[6:4] full scale, aODR[3:0] rate
//   0x05 CTRL5: accel low-pass    0x08 CTRL7: bit0 aEN
//   0x2E STATUS0: bit0 aDA        0x35..0x3A AX/AY/AZ int16 little-endian
//   0x60 RESET (write 0xB0)       0x4D reset-success flag (0x80)
// At ±2 g the scale is 16384 LSB/g.
//
// The part's own axes are right-handed and documented; how the PACKAGE is
// rotated on the M9's board is not documented anywhere, and no firmware has
// verified it (Meshtastic passes all three axes through untransformed with a
// comment saying the handedness is unverified). So this driver reports the
// sensor frame as-is and the mapping to the device is measured and applied by
// the consumer, exactly as was done for the magnetometer.
#if defined(HAS_M9_IMU) && defined(ESP32)

#include <stdint.h>

class TwoWire;

/** Probe + configure on `w`. Safe when the chip is absent or still in reset:
 *  the read path re-probes on its own. Logs one line to Serial either way. */
void m9ImuBegin(TwoWire& w);

/** True once the chip has answered with its id and taken the configuration. */
bool m9ImuPresent();

/** Latest acceleration in g, sensor frame. Held level and still, one axis
 *  reads about ±1 and the others about 0 — which is what identifies the axes.
 *  False = no chip, bus error, or nothing fresh. */
bool m9ImuRead(float* x_g, float* y_g, float* z_g);

/** Call periodically (cheap until it acts). Puts the part in power-down a
 *  couple of seconds after the last read, so it costs nothing while no app
 *  wants tilt; the next read wakes it. */
void m9ImuIdleTick();

#endif
