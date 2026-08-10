#!/usr/bin/env bash
# Populate tanmatsu/components/ with the MeshCore core + the Arduino libraries wadamesh needs,
# sourced from PlatformIO's already-resolved libdeps. Run an S3 build first to populate them:
#   /Users/kaj/Library/Python/3.9/bin/pio run -e LilyGo_TDeck_companion_radio_touch
# Vendored payloads are gitignored (regenerate with this script); the committed component
# wrappers (components/meshcore/CMakeLists.txt, components/ardlibs/CMakeLists.txt, the
# esp_hosted override) are tracked.
set -euo pipefail
cd "$(dirname "$0")"
LIBDEPS="../.pio/libdeps/LilyGo_TDeck_companion_radio_touch"
[ -d "$LIBDEPS/MeshCore" ] || { echo "!! $LIBDEPS/MeshCore missing — run an S3 'pio run' first"; exit 1; }

mkdir -p components

# --- MeshCore core: copy src/ + variants/ under the committed meshcore wrapper ---
rm -rf components/meshcore/core
mkdir -p components/meshcore/core
cp -R "$LIBDEPS/MeshCore/src"      components/meshcore/core/src
cp -R "$LIBDEPS/MeshCore/variants" components/meshcore/core/variants 2>/dev/null || true
mkdir -p components/meshcore/core/lib
cp -R "$LIBDEPS/MeshCore/lib/ed25519" components/meshcore/core/lib/ed25519   # bundled C ed25519 (Identity.cpp)
echo "vendored: meshcore core ($(find components/meshcore/core/src -name '*.cpp' | wc -l | tr -d ' ') cpp)"

# --- LVGL 8.4 (own component; big; uses the repo's include/lv_conf.h) ---
rm -rf components/lvgl/upstream
mkdir -p components/lvgl
cp -R "$LIBDEPS/lvgl" components/lvgl/upstream
echo "vendored: lvgl ($(find components/lvgl/upstream/src -name '*.c' | wc -l | tr -d ' ') c)"

# --- Arduino libraries: ALL go into ONE 'ardlibs' component so inter-library #includes
#     resolve without per-lib REQUIRES (e.g. RTClib -> Adafruit BusIO). Payload gitignored. ---
rm -rf components/ardlibs/upstream
mkdir -p components/ardlibs/upstream
vend() {  # vend <libdeps-subdir>
  local src="$1"; local dst="${src// /_}"   # spaces in dir names break IDF component-req matching
  [ -d "$LIBDEPS/$src" ] || { echo "  skip ($src not in libdeps)"; return; }
  cp -R "$LIBDEPS/$src" "components/ardlibs/upstream/$dst"
  echo "  + $src -> $dst"
}
# the core's own lib_deps (minus RadioLib — replaced by the TanmatsuLoraRadio bridge) + their deps:
vend "Crypto"
vend "RTClib"
vend "Adafruit BusIO"        # RTClib + RV3028 need Adafruit_I2CDevice.h
vend "Adafruit Unified Sensor"
vend "Melopero RV3028"
vend "CayenneLPP"
vend "ArduinoJson"           # header-only; some core/app files include it
vend "base64"                # base64.hpp (companion / QR code paths)
# app-layer libs (validate they build on P4; used once wadamesh_app is wired):
vend "MicroNMEA"             # GPS NMEA parsing
vend "AsyncTCP"              # the core's ESP32Board HTTP-OTA + companion-over-IP transport
vend "ESPAsyncWebServer"     # WebSocket/HTTP companion server (needs the task WDT on — sdkconfigs/wadamesh)
vend "AsyncElegantOTA"       # OTA web UI (the core's ESP32Board HTTP-OTA)
# NOTE: PubSubClient is deliberately NOT vended — the MQTT bridge is compiled out on the Tanmatsu
# (HAS_TANMATSU no-op stub in MqttBridge.h); the P4's esp-hosted Wi-Fi isn't wired for it and it
# crashed the app at boot. Re-add this vend if MQTT is ever brought up on the P4.
# Bump the component CMakeLists so the next build re-globs (IDF component scripts can't use
# file(GLOB CONFIGURE_DEPENDS) — they run in script mode during requirement expansion).
touch components/meshcore/CMakeLists.txt components/ardlibs/CMakeLists.txt 2>/dev/null || true
echo "done."

# --- 1.17 core: ESP32Board.cpp includes <target.h> for its powerOff/enterDeepSleep
# (board globals: radio_driver, sensors, P_LORA_NSS). The IDF apps compile the core
# without the monorepo target headers, and the Tanmatsu has no P_LORA_NSS at all
# (LoRa sits behind the C6; power teardown is the launcher/BSP's job on both boards).
# Vendor-time patch: drop the include, swap both functions for self-contained
# minimal deep-sleep. Hard-fails if the upstream shape changes.
python3 - <<'PYEOF'
import re, sys
p = 'components/meshcore/core/src/helpers/ESP32Board.cpp'
s = open(p).read()
s2, n1 = re.subn(r'#include <target\.h>\n', '', s)
pat = re.compile(r'void ESP32Board::powerOff\(\).*?esp_deep_sleep_start\(\);.*?\n\}\n', re.S)
minimal = '''// wadamesh(fetch-deps patch): IDF builds have no monorepo <target.h>; radio/display
// teardown belongs to the launcher/BSP on these boards. Minimal deep sleep only.
void ESP32Board::powerOff() { enterDeepSleep(0); }

void ESP32Board::enterDeepSleep(uint32_t secs) {
  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }
  esp_deep_sleep_start(); // never returns
}
'''
s2, n2 = pat.subn(minimal, s2)
if n1 != 1 or n2 != 1:
    sys.exit(f"!! ESP32Board.cpp patch mismatch (include={n1}, funcs={n2}) -- core shape changed, fix fetch-deps.sh")
open(p, 'w').write(s2)
print("patched: ESP32Board.cpp deep-sleep (IDF-self-contained)")
PYEOF

# --- SerialBLEInterface: the core targets NimBLE-Arduino 1.4.x (the S3 PIO builds);
# the IDF apps compile it against esp-nimble-cpp 2.x, whose callback signatures and
# advertising API differ. Port the vendored copy at vendor time (the S3 libdeps copy
# stays untouched). Hard-fails when the upstream shape changes.
python3 - <<'PYEOF'
import sys
H = 'components/meshcore/core/src/helpers/esp32/SerialBLEInterface.h'
C = 'components/meshcore/core/src/helpers/esp32/SerialBLEInterface.cpp'
def sub(path, pairs):
    s = open(path).read()
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"!! BLE 2.x patch mismatch in {path} (needle x{s.count(old)}): {old[:70]!r}")
        s = s.replace(old, new)
    open(path, 'w').write(s)
sub(H, [
  ("""  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
  void onDisconnect(NimBLEServer* pServer) override;
  void onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) override;
  uint32_t onPassKeyRequest() override;
  bool onConfirmPIN(uint32_t pass_key) override;
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override;""",
   """  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override;
  uint32_t onPassKeyDisplay() override;
  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;"""),
  ("  void onWrite(NimBLECharacteristic* pCharacteristic) override;",
   "  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;"),
])
sub(C, [
  ('#include "SerialBLEInterface.h"',
   '#include "SerialBLEInterface.h"\n#include <esp_mac.h>   // esp_efuse_mac_get_default (IDF 5.x moved it; Arduino S3 gets it transitively)'),
  ("""uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");""",
   """uint32_t SerialBLEInterface::onPassKeyDisplay() {
  BLE_DEBUG_PRINTLN("onPassKeyDisplay()");"""),
  ("""bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}""",
   """void SerialBLEInterface::onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPassKey(%u)", pass_key);
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}"""),
  ("""void SerialBLEInterface::onAuthenticationComplete(ble_gap_conn_desc* desc) {
  if (desc && desc->sec_state.encrypted) {""",
   """void SerialBLEInterface::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  if (connInfo.isEncrypted()) {"""),
  ("pServer->disconnect(desc ? desc->conn_handle : last_conn_id);",
   "pServer->disconnect(connInfo.getConnHandle());"),
  ("""void SerialBLEInterface::onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d", desc ? desc->conn_handle : -1);
  if (desc) {
    last_conn_id = desc->conn_handle;
    memcpy(_peer_bda, desc->peer_ota_addr.val, 6);   // NimBLE addr is little-endian
    _peer_bda_valid = true;
  }
}""",
   """void SerialBLEInterface::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d", connInfo.getConnHandle());
  last_conn_id = connInfo.getConnHandle();
  memcpy(_peer_bda, connInfo.getAddress().getVal(), 6);   // NimBLE addr is little-endian
  _peer_bda_valid = true;
}"""),
  ("void SerialBLEInterface::onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) {",
   "void SerialBLEInterface::onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {"),
  ("void SerialBLEInterface::onDisconnect(NimBLEServer* pServer) {",
   "void SerialBLEInterface::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {"),
  ("void SerialBLEInterface::onWrite(NimBLECharacteristic* pCharacteristic) {",
   "void SerialBLEInterface::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {"),
  ("adv->setScanResponse(true);", "adv->enableScanResponse(true);"),
])
print("patched: SerialBLEInterface -> esp-nimble-cpp 2.x")
PYEOF

# --- Guard: the vendored tree is gitignored and this script rm -rf's it, so any
# hand-edit made in there is destroyed with no git history to recover it. That
# has now cost us twice: the esp-nimble-cpp 2.x BLE port, and the T-Display P4's
# polled receive (its DIO1 reaches no host GPIO, so polling is the ONLY way it
# ever sees a packet — losing it left the board able to transmit and completely
# unable to receive). Both are upstream in the core now; verify they survived the
# copy, so a regression fails here instead of on a user's device.
for probe in \
  "components/meshcore/core/src/helpers/radiolib/RadioLibWrappers.cpp:pollRxIfNoIrq" \
  "components/meshcore/core/src/helpers/radiolib/CustomSX1262Wrapper.h:pollRxDone" \
  "components/meshcore/core/src/helpers/esp32/SerialBLEInterface.h:NimBLEConnInfo"; do
  f="${probe%%:*}"; sym="${probe##*:}"
  grep -q "$sym" "$f" || { echo "!! $f is missing '$sym' — the vendored core is stale or the core changed shape. FIX BEFORE BUILDING."; exit 1; }
done
echo "verified: polled-RX + BLE 2.x present in the vendored core"
