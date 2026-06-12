#pragma once

/* Implementation lives in src/helpers/esp32/WifiRuntimeStore (NVS namespace meshcomod). */
#if defined(ESP32)
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
#include <helpers/esp32/WifiRuntimeStore.h>
#endif
#endif
