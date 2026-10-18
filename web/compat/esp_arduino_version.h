#pragma once
// The arduino-esp32 release whose String/Print/Stream sources web/compat/arduino/ carries.
#define ESP_ARDUINO_VERSION_MAJOR 2
#define ESP_ARDUINO_VERSION_MINOR 0
#define ESP_ARDUINO_VERSION_PATCH 17
#define ESP_ARDUINO_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(2, 0, 17)
