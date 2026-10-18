#pragma once
// The ESP object: chip facts the About page shows, reported for the "web" target.
#include <stdint.h>
#include <stddef.h>
#include "esp_heap_caps.h"
class EspClass {
public:
  uint32_t getHeapSize() { return WADA_HOST_HEAP_REPORT * 2; }
  uint32_t getFreeHeap() { return WADA_HOST_HEAP_REPORT; }
  uint32_t getMinFreeHeap() { return WADA_HOST_HEAP_REPORT; }
  uint32_t getMaxAllocHeap() { return WADA_HOST_HEAP_REPORT; }
  uint32_t getPsramSize() { return WADA_HOST_HEAP_REPORT * 2; }
  uint32_t getFreePsram() { return WADA_HOST_HEAP_REPORT; }
  uint32_t getMinFreePsram() { return WADA_HOST_HEAP_REPORT; }
  uint32_t getMaxAllocPsram() { return WADA_HOST_HEAP_REPORT; }
  uint16_t getChipRevision() { return 0; }
  const char* getChipModel() { return "Web"; }
  uint8_t getChipCores() { return 1; }
  uint32_t getCpuFreqMHz() { return 240; }
  uint32_t getCycleCount();
  const char* getSdkVersion() { return "host"; }
  uint32_t getFlashChipSize() { return 16u * 1024 * 1024; }
  uint32_t getFlashChipSpeed() { return 80000000; }
  uint32_t getSketchSize() { return 0; }
  uint32_t getFreeSketchSpace() { return 0; }
  uint64_t getEfuseMac();
  void restart();          // web/platform: ends the process (the shell restarts it)
  void deepSleep(uint64_t) { restart(); }
};
extern EspClass ESP;
