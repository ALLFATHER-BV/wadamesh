#pragma once
// The web board: mesh::MainBoard for a host. No battery, no GPIO, no radio front end.
#include <MeshCore.h>
class WebBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 0; }      // CAP_BATTERY 0: never shown
  const char* getManufacturerName() const override { return "wadamesh web"; }
  void reboot() override;                                   // web/platform/host_main.cpp
  void powerOff() override;
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};
// CAP_NATIVE_KEYBOARD: the phone's own keyboard types (web/platform/WebKeyboard.cpp).
bool boardNativeKeyboard();
