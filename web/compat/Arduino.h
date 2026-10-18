// SPDX-License-Identifier: GPL-3.0-or-later
// web/compat/Arduino.h -- the Arduino-ESP32 surface the touch UI uses, on a host OS.
//
// Scope: exactly what src/ui-touch + the helpers it pulls in need, no more. Strings,
// Print and Stream are the REAL arduino-esp32 2.0.17 sources (web/compat/arduino/,
// LGPL-2.1, unmodified), so String formatting behaves like on the device. Time comes
// from a monotonic clock started at boot. GPIO is a no-op (there are no pins).
//
// Must stay plain C/C++17 + POSIX: the same header builds under Emscripten (phase 2).
#ifndef Arduino_h
#define Arduino_h

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>

#include "esp_arduino_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp32-hal.h"
#include "arduino/stdlib_noniso.h"

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352

#define LSBFIRST 0
#define MSBFIRST 1
#define RISING    0x01
#define FALLING   0x02
#define CHANGE    0x03

#ifndef __STRINGIFY
#define __STRINGIFY(a) #a
#endif
#define _min(a,b) ((a)<(b)?(a):(b))
#define _max(a,b) ((a)>(b)?(a):(b))
#define _abs(x) ((x)>0?(x):-(x))
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#define _round(x)     ((x)>=0?(long)((x)+0.5):(long)((x)-0.5))
#define radians(deg) ((deg)*DEG_TO_RAD)
#define degrees(rad) ((rad)*RAD_TO_DEG)
#define sq(x) ((x)*(x))
#define interrupts()
#define noInterrupts()
#define lowByte(w) ((uint8_t) ((w) & 0xff))
#define highByte(w) ((uint8_t) ((w) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))
#define _BV(b) (1UL << (b))
#define NOT_A_PIN -1
#define digitalPinToInterrupt(p) (p)

typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

#ifdef __cplusplus
long random(long);
long random(long, long);
void randomSeed(unsigned long);
#endif
long map(long, long, long, long, long);

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include "arduino/WCharacter.h"
#include "arduino/WString.h"
#include "arduino/Stream.h"
#include "arduino/Printable.h"
#include "arduino/Print.h"
#include "HardwareSerial.h"
#include "Esp.h"
using std::abs;
using std::isinf;
using std::isnan;
using std::max;
using std::min;
using std::round;

extern "C" bool getLocalTime(struct tm* info, uint32_t ms = 5000);
void tone(uint8_t pin, unsigned int frequency, unsigned long duration = 0);
void noTone(uint8_t pin);
#endif

#include "pins_arduino.h"

#endif
