// SPDX-License-Identifier: GPL-3.0-or-later
// The web board's globals and boot order: what a variants/*/target.cpp plus
// src/main.cpp's setup()/loop() are on a device. Shared by host_main.cpp (SDL2) and
// web_main.cpp (Emscripten); neither front end duplicates any of this.
#include <Arduino.h>
#include <SPIFFS.h>
#include <LittleFS.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <helpers/BaseSerialInterface.h>
#include "UITask.h"
#include "WebMesh.h"
#include "WebPlatform.h"
#include "WebInput.h"
#include "BackendSelect.h"
#include "helpers/esp32/TouchPrefsStore.h"
#include "helpers/esp32/SdNvsPrefs.h"
#include "wada_host.h"
#include <time.h>

namespace {
// The mesh clock: the host's wall clock (a browser always knows the time).
class HostRTCClock : public mesh::RTCClock {
public:
  uint32_t getCurrentTime() override { return (uint32_t)time(nullptr); }
  void setCurrentTime(uint32_t) override {}   // the OS owns the clock
};
HostRTCClock s_host_clock;

// No companion transport is served BY the web board (it is a companion's CLIENT, see
// web/mesh/CompanionBackend.cpp), so the UI's companion-link controls see a disabled, unconnected interface.
class WebSerialInterface : public BaseSerialInterface {
public:
  void enable() override { _en = true; }
  void disable() override { _en = false; }
  bool isEnabled() const override { return _en; }
  bool isConnected() const override { return false; }
  bool isWriteBusy() const override { return false; }
  size_t writeFrame(const uint8_t src[], size_t len) override { (void)src; return len; }
  size_t checkRecvFrame(uint8_t dest[]) override { (void)dest; return 0; }
  bool isTcpEnabled() const override { return false; }
private:
  bool _en = false;
};
WebSerialInterface s_serial;
}  // namespace

WebBoard board;
WebRadio radio_driver;
ClockFloorRTC rtc_clock(s_host_clock);
WebSensorManager sensors;
WebDisplay display;
TwoWire Wire(0), Wire1(1);
SPIClass SPI;
WiFiClass WiFi;
UITask ui_task(&board, &s_serial);
static MyMesh s_mesh(rtc_clock, &ui_task);
MyMesh& the_mesh = s_mesh;

// src/main.cpp / DataStore.cpp / MultiTransportCompanionInterface.cpp symbols the UI reads.
bool g_identity_restored_from_copy = false;
uint8_t bleLastFailReason() { return 0; }   // BLE_FAIL_NONE: there is no Bluetooth stack to fail

// AutoDiscoverRTCClock (core): no I2C RTC chips on a host, the fallback clock is it.
void AutoDiscoverRTCClock::begin(TwoWire&) {}
uint32_t AutoDiscoverRTCClock::getCurrentTime() { return _fallback->getCurrentTime(); }
void AutoDiscoverRTCClock::setCurrentTime(uint32_t t) { _fallback->setCurrentTime(t); }

void WebBoard::reboot() { esp_restart(); }
void WebBoard::powerOff() { esp_restart(); }

void wadaBoardBegin(const WadaBackendChoice& choice) {
  Serial.printf("[web] wadamesh %s, data dir %s\n", FIRMWARE_VERSION, wadaHostDataDir().c_str());
  // Storage, then the mesh, then the UI: src/main.cpp's setup() order.
  SPIFFS.begin(true);
  LittleFS.begin(true);
  // Tests (WADA_PREFS_FILE=1): the devices' file-backed settings store, as src/main.cpp
  // selects it on a board without SD, instead of the host's NVS stand-in. Its A/B slot
  // files land in <data dir>/spiffs/prefs/ (web/scripts/prefs-slot.sh).
  if (const char* e = getenv("WADA_PREFS_FILE")) {
    if (e[0] == '1') { SdNvsPrefs::useFile((fs::FS*)&SPIFFS, "/prefs"); touchPrefsReload(); }
  }
  // The host clock is synced: a real time source. Tests (WADA_CLOCK_UNSET=1) leave it out,
  // so the UI runs as a device does before its first sync (rtc_clock.timeIsCurrent() false).
  if (!getenv("WADA_CLOCK_UNSET")) rtc_clock.setCurrentTime((uint32_t)time(nullptr));
  the_mesh.begin(wadaCreateBackend(choice));   // the demo, or a companion node (BackendSelect.h)
  // A node is set up already: the first-run wizard (name + LoRa region) would rename
  // and re-tune it. It is the demo's welcome only.
  if (choice.kind != "demo" && !touchPrefsGetSetupDone()) touchPrefsSetSetupDone(true);
  ui_task.begin(&display, &sensors, the_mesh.getNodePrefs());
  Serial.println("[BOOT] ui ready");
}

// UI first, then the mesh (same order as src/main.cpp, same reason).
void wadaBoardStep() {
  webWheelTick();
  ui_task.loop();
  the_mesh.loop();
}

void wadaBoardFlush() { ui_task.persistHistoryNow(); }
