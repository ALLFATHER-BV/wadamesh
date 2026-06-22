# Tanmatsu port — working tracker

Goal: run the **full** wadamesh UI + functionality on the **Tanmatsu** (Nicolai
Electronics), kept as **one codebase** with the existing S3 boards so a UI change
ships everywhere at once. This file is the running plan/status — update it as we go.

Status: **device IN HAND (2026-06-20); ENTIRE APP COMPILES + LINKS on P4** (`f04dba6`) · branch
`tanmatsu-port` on `beta_9` · the **full shared src/ — MyMesh + the 28k-line LVGL UITask + all
helpers + variant glue — compiles, and the project builds `application.bin`** · LVGL 8.4 + the
core + ~10 Arduino libs all build · **next = the real `app_main` bring-up (the spike main still
ships, so the bin isn't the real app yet), storage-on-FAT, the 800×480 layout, the bsp_input→LVGL
indev, then on-device install via BadgeLink/AppFS.**

---

## Decision (architecture) — UPDATED 2026-06-20: wadamesh is an APP on the launcher

On the Tanmatsu **everything is an app** (badge.team OS + GUI launcher + **AppFS** app
store). So wadamesh ships as a **launcher app, NOT a firmware that replaces the OS.**
AppFS apps are **standard ESP-IDF app binaries** installed into an AppFS flash partition
(via `appfs_add_file.py` / BadgeLink / the badge.team app store) and started from the
launcher; a running app owns the screen + input via `badge-bsp` and returns to the
launcher on exit (docs.tanmatsu.cloud → AppFS).

⇒ The Tanmatsu target is an **ESP-IDF app**, NOT the pioarduino/Arduino *standalone*
flash (that would overwrite the launcher). The PlatformIO route in the old plan is
**dropped**; the "IDF subproject" that was the fallback is now the **primary path** —
because the whole app model is IDF-native.

We still keep **one repo, one `src/`**: the shared app (UITask, MyMesh, i18n, fonts…)
compiles inside the IDF app via **arduino-esp32 as an ESP-IDF component**, alongside
`badge-bsp` (display/keyboard/power), `tanmatsu-lora` (LoRa — bridge already written in
`variants/tanmatsu/`) and `esp_wifi_remote` (C6 Wi-Fi/BLE). Our **LVGL 8.3** flushes to
the badge-bsp `esp_lcd` panel (not PAX; not the LVGL-9.3 `badge-bsp-lvgl`). **Distribution
= the badge.team app store** — the Tanmatsu analog of LauncherHub. `boards/tanmatsu.json`
+ the pioarduino env in the old plan are no longer the build; the LoRa bridge + variant
wiring carry over.

## Compile spike — RESULTS (2026-06-20) ✅ arduino-as-component PROVEN on P4

IDF-app build lives in **`tanmatsu/`** (badge.team Makefile + project-local IDF **v5.5.1**
in `tanmatsu/esp-idf`, gitignored). Build via **`./tanmatsu/build.sh <target>`** (wraps
`idf.py -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu;sdkconfigs/wadamesh" -DIDF_TARGET=esp32p4`).
Toolchain: PlatformIO already had the riscv32-esp P4 toolchain; IDF cloned via the Makefile
+ `idf_tools.py install cmake ninja` (the targeted `install.sh esp32p4` skips those).

Proven end-to-end:
- ✅ Stock badge.team template builds for P4 (baseline) — toolchain + component manager OK.
- ✅ **arduino-esp32 3.3.10** compiles + links **alongside badge-bsp** (241 arduino objs).
  esp32p4 first-class in arduino since 3.1.3. ⇒ the "one shared `src/` via arduino-on-P4"
  architecture is **validated** — this was the make-or-break risk.

**Fixes (all in `tanmatsu/sdkconfigs/wadamesh`, our overlay — badge.team files kept pristine):**
1. `CONFIG_FREERTOS_HZ=1000` — arduino-esp32 hard-requires it (badge.team defaults 100).
2. `CONFIG_BT_ENABLED=n` (+ `BT_NIMBLE_ENABLED=n`, `ESP_HOSTED_ENABLE_BT_NIMBLE=n`) — arduino's
   bundled BLE won't compile vs IDF 5.5.1 NimBLE (`ble_gap_read_local_irk` renamed). P4 has no
   native BLE + we use the separate NimBLE-Arduino lib, so BT-off compiles arduino
   BLE/SimpleBLE/BluetoothSerial out via their `#if` guards. **Revisit hosted BLE (phone
   companion over the C6) later.** NB: arduino's `CONFIG_ARDUINO_SELECTIVE_COMPILATION` does
   NOT filter in this setup (verified) — use feature disables, not the selective knob.

**Gotchas found (carry into the real src/ wiring):**
- **badge-bsp headers have NO `extern "C"` guards** → wrap every `#include "bsp/*.h"` in
  `extern "C" { }` from C++ or the bsp symbols mangle and fail to link.
- **Two esp-hosted stacks collide:** arduino + `espressif/esp_wifi_remote` pull
  `espressif/esp_hosted`, whose whole-archive C6 SDIO driver duplicates badge.team's
  `nicolaielectronics/esp-hosted-tanmatsu` → "multiple definition". **Fix:
  `tanmatsu/components/esp_hosted/` (empty forwarder, `REQUIRES esp-hosted-tanmatsu`) shadows
  the registry component by name** (local components win) → one esp-hosted, the hardware fork,
  and arduino's `WiFi.h` rides on it. Runtime TODO: the `ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE`
  config warning (C6 reset pin) — set it when WiFi is brought up live.

## Spike C1 — MeshCore core compiles on P4 (2026-06-20) ✅

The core is vendored by **`tanmatsu/fetch-deps.sh`** from `.pio/libdeps/.../MeshCore` into
`components/meshcore/core/` (gitignored) under a committed `components/meshcore/CMakeLists.txt`.
The Arduino libs it needs (Crypto, RTClib, Adafruit BusIO, Adafruit Unified Sensor, Melopero
RV3028, CayenneLPP, ArduinoJson) all go in ONE committed **`ardlibs`** component (so cross-lib
`#include`s resolve). Build: `./build.sh build`. What it took:
- vendored lib dir names must be **space-free** (spaces break IDF's component-req matching → `BUG:`).
- the core's bundled **C ed25519** (`lib/ed25519/`) must be added explicitly (PlatformIO
  auto-compiles it as a nested lib; IDF doesn't).
- `ardlibs` needs `nvs_flash` (Crypto's RNG seeds from NVS); `meshcore` needs `nvs_flash`+`efuse`.
- **strip IDF's `-Werror=all`** from `COMPILE_OPTIONS` — vendored code trips reorder/format/
  class-memaccess, and a bare `-Wno-error` does NOT cancel `-Werror=all` (done in main/CMakeLists).
- exclude over-globbed files vs `build_as_lib.py`: non-ESP32 platform helpers (nrf52/stm32/rp2040),
  `helpers/radiolib/*` (→ LoRa bridge), the S3 display drivers, `ESPNOWRadio`, `SerialBLEInterface`
  (BT off), and the `MC_VENDORED_TOUCH_APP` files (our own src/ ships them).

**Remaining for a running app — ordered worklist (next session):**
1. **Tanmatsu define set** (`main/CMakeLists.txt` build flags) — a *curated subset* of the S3 touch
   `build_flags`: KEEP `ESP32_PLATFORM`, `MC_VENDORED_TOUCH_APP`, `UI_LVGL=1`, `HAS_TOUCH_UI=1`,
   `MULTI_TRANSPORT_COMPANION`, `LORA_FREQ/BW/SF`, `LV_CONF_PATH=lv_conf.h`+`LV_CONF_INCLUDE_SIMPLE=1`
   +`LV_INDEV_DEF_SCROLL_THROW=7`, `MAX_CONTACTS`, `ENABLE_PRIVATE_KEY_*`, `ENV_INCLUDE_GPS`,
   `-I src -I src/ui-touch -I include`. DROP every S3-board define (`HELTEC_LORA_V4*`, `USE_SX1262`,
   `RADIO_CLASS`/`WRAPPER_CLASS`, all `P_LORA_*`/`PIN_*`/TFT/CHSC6x, `ST7789_*`, `RADIOLIB_*`). Give
   the Tanmatsu its own board/display identity (badge-bsp, NOT `DISPLAY_CLASS=ST7789`).
2. **`wadamesh_app` component** — glob `src/` MINUS `main.cpp` (own app_main) + the S3 input helpers
   (`HeltecV4CapTouch`, `TDeckKeyboard/Touch/Trackball` → Tanmatsu input is `bsp_input`); add
   `ui-touch/` once LVGL is in.
3. **LVGL component** — vendor lvgl 8.3 + the repo `lv_conf.h`.
4. **Transports** — re-add AsyncTCP+ESPAsyncWebServer with a WDT-config fix (`CONFIG_ESP_TASK_WDT_*`)
   or the ESP32Async fork; OR ship companion over USB-serial first (off the critical path).
5. **`variants/tanmatsu/` glue** — `TanmatsuBoard` (board API over bsp/power), the LVGL→badge-bsp
   display driver (`flush_cb`→`bsp_display_blit`, 800×480 RGB565), a `bsp_input`→`lv_indev`
   keyboard/pointer, `radio_init`→the `TanmatsuLoraRadio` bridge, storage→the FAT `locfd` partition.
6. **`app_main`** — replicate setup()/loop(): board.begin → radio_init → storage → the_mesh.begin →
   ui_task.begin(disp) → loop{ui_task.loop; the_mesh.loop}.
7. **Install** — `make build` → `application.bin` → BadgeLink/AppFS → the badge.team app store.

Biggest chunks: UITask (28k lines) vs the Tanmatsu defines + LVGL, and the 800×480 display flush
(768 KB/frame → PSRAM). All methodical integration on the proven foundation.

Dep status: vendored + compiling = MeshCore core, ed25519, Crypto, RTClib, Adafruit BusIO, Adafruit
Unified Sensor, Melopero RV3028, CayenneLPP, ArduinoJson, MicroNMEA. Still to add = LVGL, AsyncTCP +
ESPAsyncWebServer (WDT fix), and (only if telemetry wanted) the Adafruit sensor libs.

## Why this is feasible (verified 2026-06-18)

- **Arduino-ESP32 supports the ESP32-P4** — core **3.3.10** (2026-06-05) has P4
  boards incl. MIPI-display variants (`ESP32P4_4DS_MIPI`).
- **PlatformIO can target it** via the **pioarduino** platform (rel `55.03.39`),
  whose board list already includes **`m5stack-tab5-p4`** — a P4 + MIPI display +
  C6-coprocessor-radio handheld, i.e. a structural near-twin of the Tanmatsu. Crib
  display/WiFi bring-up from that board.
- **Radio maps cleanly:** `mesh::Radio` (7 pure virtuals) ↔ `tanmatsu-lora`'s
  `lora.h` (raw 256-byte tx/rx, settable freq/bw/sf/cr/sync/preamble/power,
  RSSI/SNR). Same SX1262/1268 family the S3 boards use. Bridge written (below).
- **No driver writing:** display/keyboard/power/wifi/lora all exist as IDF
  components (`badge-bsp`, `tanmatsu-wifi`, `tanmatsu-lora`).

## Hardware / platform facts

| | | (confirmed from docs.tanmatsu.cloud + hackster/CNX) |
|---|---|---|
| Main SoC | ESP32-**P4** dual-core RISC-V | **@ 400 MHz, 32 MB PSRAM, 16 MB flash**, microSD (SDIO) |
| Display | **3.97" MIPI-DSI, 800×480, RGB565** | `esp_lcd` panel from `badge-bsp`; we flush **LVGL 8.3** to it (not PAX) |
| Input | **69-key QWERTY** + 6 fn keys | events via `bsp_input_get_queue()` |
| Coprocessors | **WCH CH32V203** (keyboard + power) · **ESP32-C6** (WiFi/BLE/802.15.4 via `tanmatsu-wifi`) · **SX1262** LoRa 433/868-915 (via `tanmatsu-lora`, "remote") | the P4 has no native radio |
| Native SDK | ESP-IDF ≥ 5.5, CMake, C, PAX graphics | template: `Nicolai-Electronics/tanmatsu-template` |

## Research findings (2026-06-18, no hardware)

**Radio bridge — fully validated against `tanmatsu-lora/lora.c`; the code was already correct.**
- `lora_packet_stats_t = {rssi_pkt_raw u8, snr_pkt_raw i8, signal_rssi_pkt_raw u8}`; `rssi_dbm = -rssi_pkt_raw/2`, `snr_db = snr_pkt_raw/4`. ✓ matches the bridge.
- `lora_send_packet()` is **blocking** (waits ≤2 s on the transaction semaphore) → `isSendComplete()` latch is correct. ✓
- `config.bandwidth` = kHz, valid {7,10,15,20,31,41,62,125,250,500} (62.5→62); `config.coding_rate` = 5..8 → 4/5..4/8. `setParams()` is correct. ✓
- Only remaining radio tuning (small, MeshCore-side): `sync_word` / `preamble_length` / `power` to match `RadioLibWrappers.cpp` so it interoperates with the existing mesh.

**Display/input — DECISION: keep wadamesh's LVGL 8.3 and write our own flush; do NOT adopt `badge-bsp-lvgl`.**
- `badge-bsp` exposes the screen as an `esp_lcd` panel (`bsp_display_get_panel` / `_get_panel_io` / `_get_parameters` → h_res/v_res/color_format) and the keyboard as a FreeRTOS event queue (`bsp_input_get_queue`). Reference wiring: `badgeteam/konsool-template-lvgl`.
- BUT the badge LVGL component (`badgeteam/esp32-component-badge-bsp-lvgl`) targets **LVGL 9.3**, and wadamesh's UITask is **LVGL 8.3**. Adopting it forces an 8→9 migration of ~30k lines — not worth it. So: bring up our own LVGL 8.3 (esp_lvgl_port supports 8.x, or a manual `flush_cb`) flushing to the badge-bsp `esp_lcd` panel, + an `lv_indev` that reads the `bsp_input` queue.
- **UI layout target: 800×480 landscape** — make UITask responsive for it (it already branches on `hor_res`).

**Repos:** `badgeteam/esp32-component-badge-bsp` (BSP) · `badgeteam/konsool-template-lvgl` (LVGL-on-bsp reference) · `Nicolai-Electronics/esp32-component-tanmatsu-lora` (lora.c).

## Plan / status

- [x] Confirm framework gap is bridgeable (Arduino-P4 + pioarduino + Tab5 precedent)
- [x] Map `mesh::Radio` ↔ `lora.h`; write `TanmatsuLoraRadio` (full drop-in for `radio_driver`: setParams/setTxPower/setRxBoostedGainMode/idle/getPacketsRecvErrors/RSSI/SNR)
- [x] `idf_component.yml` for the BSP/wifi/lora components
- [x] `boards/tanmatsu.json` (based on pioarduino `m5stack-tab5-p4`)
- [x] `variants/tanmatsu/{target.h,target.cpp,TanmatsuBoard.h}` — wires `radio_driver`=bridge, `board`, `radio_init()`, `radio_new_identity()` (StdRNG)
- [ ] **Compile spike** — get `src/` to build for P4 under pioarduino (no HW needed; see below)
- [ ] `[env:tanmatsu_companion_radio_touch]` in `platformio.ini` — port the shared touch flags, drop S3 LoRa pins; **resolve the RadioLib build issue (next item)**
- [x] RadioLib build question — RESOLVED: keep RadioLib in `lib_deps` + the same `RADIOLIB_*` flags as the S3 envs. `RadioLibWrappers.cpp` still compiles, but since the Tanmatsu `target.cpp` never instantiates `WRAPPER_CLASS`/`RADIO_CLASS`, the unused code is linker-stripped. Net: the env mirrors the T-Deck's RadioLib deps/flags; only the `radio_driver` *type* differs (set in `variants/tanmatsu/target.h`). No `build_src_filter` gymnastics needed.
- [ ] Wire the IDF components into the pio build (component manager) ⚠️ main risk
- [ ] **Headless bring-up:** call `radio_init()` → `TanmatsuLoraRadio` joins the mesh (serial log)
- [ ] WiFi/BLE via `esp_wifi_remote`/`tanmatsu-wifi` (likely close to free in Arduino)
- [ ] LVGL display driver: flush → `bsp_display_blit`; LVGL indev ← `bsp/input` (keyboard)
- [ ] Make UITask responsive for the larger screen (it already branches on `hor_res`)
- [ ] Tune the `TanmatsuLoraRadio` TODO(device) items
- [ ] Release path (separate bin; not covered by the `wadamesh-release` skill yet)

## What's done this session (no hardware)

A structurally-complete variant skeleton — the radio + board wiring is real (mirrors
the T-Deck variant); display/input/wifi glue + the env are the device/spike work left.

- `variants/tanmatsu/TanmatsuLoraRadio.{h,cpp}` — the LoRa bridge, real code against
  the actual `lora.h` v0.3.0 API + `mesh::Radio` contract, reusing MeshCore's own
  airtime/packetScore math. A full drop-in for `radio_driver` (every method the app
  calls in `MyMesh.cpp`). Compiles-ready *pending* the `TODO(device)` items.
- `variants/tanmatsu/target.{h,cpp}` — wires `radio_driver` (the bridge), `board`,
  `radio_init()` (→ `lora_init_remote`), `radio_new_identity()` (seeded via StdRNG).
- `variants/tanmatsu/TanmatsuBoard.h` — `: public ESP32Board`; battery/power via
  badge-bsp left as TODO(device).
- `variants/tanmatsu/idf_component.yml` — the managed-component deps.
- `boards/tanmatsu.json` — P4/16MB/PSRAM board def (based on `m5stack-tab5-p4`;
  confirm flash size + f_cpu on the real unit).
- This tracker + the `tanmatsu-port` memory.

### `TanmatsuLoraRadio` — TODO(device) (confirm/tune on hardware)
- `lora_packet_stats_t` field names + raw→real RSSI/SNR conversion (assumed `-raw/2`, `raw/4`).
- `bandwidth` units (assumed kHz) and `coding_rate` encoding (assumed 4/x, x=5..8).
- `sync_word` / `preamble_length` — match `RadioLibWrappers.cpp` so it interoperates with the existing mesh.
- Whether `lora_send_packet()` is blocking (assumed yes → `isSendComplete()` latch is fine).
- Where the variant creates the radio object + calls `begin()`/`setParams()` (mirror the S3 radio glue in the variant target).

## Compile spike (run first, needs only the toolchain)

```bash
# add the env (below) to platformio.ini + a stub boards/tanmatsu.json, then:
PIO=/Users/kaj/Library/Python/3.9/bin/pio
$PIO run -e tanmatsu_companion_radio_touch     # expect: platform/toolchain resolves, src/ starts compiling
```
Outcome tells us: does pioarduino-P4 + Arduino build the shared `src/`? Compiles →
proceed with display/radio glue. Fails on the IDF-component wiring → that's the known
risk; fall back to the IDF-subproject layout.

### env block to add to `platformio.ini` (draft)
```ini
[env:tanmatsu_companion_radio_touch]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
framework = arduino
board = tanmatsu                      ; boards/tanmatsu.json — model on pioarduino m5stack-tab5-p4
build_flags =
  ${env.build_flags}                  ; reuse the shared touch flags
  -D HAS_TANMATSU=1
  -I variants/tanmatsu
; board_build.partitions = variants/tanmatsu/partitions_tanmatsu.csv   ; TODO
; the BSP/wifi/lora IDF components are pulled via variants/tanmatsu/idf_component.yml
```

## Risks / open questions
1. **IDF components inside PlatformIO** (`badge-bsp`/`tanmatsu-lora`) — the fiddliest part; pioarduino supports the component manager but expect friction. **Still the #1 risk.**
2. **Display = our own LVGL 8.3 flush onto the badge-bsp `esp_lcd` panel** (decided; don't use the LVGL-9.3 `badge-bsp-lvgl`). Work = the flush_cb + the `bsp_input`→`lv_indev` glue + an 800×480 layout pass on UITask. Bounded now that the BSP API + konsool reference are known.
3. **WiFi/BLE via the C6** (`esp_wifi_remote` + `esp_hosted` over SDIO). Arduino `WiFi.h` on the P4 *is* backed by `esp_wifi_remote` in arduino-esp32 3.x, so wadamesh's WiFi (tile fetch, setup wizard, network tab) can likely run **mostly unchanged** — but it needs the C6 on the right `esp_hosted` slave firmware + the components wired in, and it's a **known-finicky area** in practice (cf. esphome#10956). The template uses IDF wifi helpers (`wifi_remote.h`), not Arduino `WiFi.h`, so confirm Arduino WiFi coexists with `tanmatsu-wifi`'s C6 bring-up during the spike. A real task + risk, not free — but not a rewrite.
4. **Heap/PSRAM** — fine: **32 MB PSRAM**, and an 800×480 RGB565 buffer is only ~768 KB. Lots of headroom vs the S3 boards.

## References
- Template: https://github.com/Nicolai-Electronics/tanmatsu-template (ESP-IDF, P4, PAX, badge-bsp)
- LoRa component: https://github.com/Nicolai-Electronics/esp32-component-tanmatsu-lora (`include/lora.h`, v0.3.0)
- Precedent board: pioarduino `m5stack-tab5-p4` (P4 + MIPI + C6)
- `mesh::Radio` base: monorepo `src/Dispatcher.h`; example backend `src/helpers/esp32/ESPNOWRadio.h`
