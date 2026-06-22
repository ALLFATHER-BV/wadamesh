#pragma once
// Compatibility shims for Arduino-ESP32 3.x / ESP32-P4 API gaps that the vendored MeshCore core
// still expects. Force-included on the Tanmatsu build (see tanmatsu/main/CMakeLists.txt).
#include <stdint.h>

// adcAttachPin() was removed in Arduino-ESP32 3.x. The core's ESP32Board calls it for the VBAT
// pin; on the Tanmatsu that pin is -1/none (battery comes from badge-bsp/power), so a no-op is fine.
static inline void adcAttachPin(uint8_t) {}
