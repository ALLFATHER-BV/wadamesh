# wadamesh in the browser (WEB_BUILD)

The real wadamesh touch UI (`src/ui-touch`) built for a computer instead of an
ESP32, driving a mesh it reaches through a backend. Phase 1 is a native
macOS/Linux build in an SDL2 window with a demo backend; phase 2 is the same
build in a web page (Emscripten, `web/dist/`). Phase 3 connects it to a real
MeshCore companion node.

```
web/build.sh host     # build web/build-host/wadamesh-host
web/build.sh run      # build and start it (data in web/data/)
web/build.sh shots    # run the acceptance tour headless -> web/docs/*.png
web/build.sh smoke    # the same tour, exit status only (CI, release gate)
web/build.sh web      # the browser build -> web/dist/ (static files)
web/build.sh web-debug  # -g + runtime assertions -> web/dist-debug/
web/build.sh serve    # serve web/dist on http://localhost:8080 with COOP/COEP
web/build.sh webtest  # the same tour in headless Chromium/Firefox/WebKit (Playwright)
```

Needs cmake, ninja, SDL2 (`brew install cmake ninja sdl2`); the browser build
also needs Emscripten (`brew install emscripten`, built with 6.0.12) and the
test needs Playwright (`npm install --prefix web playwright`, then
`cd web && npx playwright install`; `web/node_modules/` is gitignored). The first build
runs `web/fetch-deps.sh`, which fetches pinned third-party sources into
`web/third_party/` (gitignored). Window: 800x480, resizable; mouse = finger,
the computer keyboard types into the focused field, Enter sends, Esc is Back,
F12 saves a screenshot.

## The idea: one source, a new board

There is no fork of the UI. `web/CMakeLists.txt` compiles every `.cpp`/`.c` in
`src/ui-touch/` (UITask, LuaAppHost, LuaHost, i18n, the fonts and emoji,
KeyboardLayouts, Constellation, SnakeGame, ReaderContent, ...) plus the helpers
the UI links (`TouchPrefsStore`, `SdNvsPrefs`, `WifiRuntimeStore`, `WebMirror`,
`TouchDiagTrace`, `LvglPsramAlloc`), with the same `include/lv_conf.h` the
firmware uses and the same LVGL 8.4 with the same two source patches. A new UI
file joins the web build automatically.

The web build is a board like any other: `-DWADA_WEB` selects a block in
`src/ui-touch/device_caps.h`, and the UI adapts through its `CAP_*` flags.

| Capability | Web board | Why |
|---|---|---|
| CAP_TOUCH | 1 | mouse / finger |
| CAP_LARGE_SCREEN | 1 | 800x480 landscape |
| CAP_ROTATABLE | 0 | fixed canvas |
| CAP_SD, CAP_FILESYSTEM | 0 | storage is the host directory behind SPIFFS |
| CAP_GPS, CAP_BATTERY, CAP_LOCK_SCREEN, CAP_OTA, CAP_USB_FILES | 0 | no such hardware; a page updates by reloading |
| CAP_RADIO_TOOLS (new) | 0 | no own radio: no Signal / Discover / Spectrum tiles |
| CAP_SCREEN_MIRROR (new) | 0 | it is the browser view: no Remote / VNC tiles |
| CAP_WEB_BROWSER | 0 | a page cannot fetch arbitrary sites (CORS) |
| CAP_BUILTIN_LANGS, CAP_BUILTIN_LUA_APPS | 1 | no Store download yet: translations and catalog apps are baked in |
| CAP_EXT_KEYBOARD, CAP_KEYPAD_NAV | 1 (derived) | the computer keyboard is the UI's external keyboard (`HAS_CARDKB`) |

**Why 800x480.** It is the size of the two shipping large-screen layouts (the
CrowPanel 5.0 with touch, the Tanmatsu with keys), so the web build runs a
layout path real devices already exercise, with the UI-size fonts. It also
fits a laptop browser window at 1:1 and scales cleanly to phones and big screens
(the window/canvas scales; LVGL renders at 800x480).

Build defines (all in `web/CMakeLists.txt`): `WADA_WEB ESP32 HAS_TOUCH_UI UI_LVGL
MULTI_TRANSPORT_COMPANION HAS_CARDKB WADAMESH_FORK_BUILD MAX_CONTACTS=500
MAX_GROUP_CHANNELS=40`. `ESP32` is on because the UI's ESP32 code paths are the
device code paths; the compat layer backs them. `MULTI_TRANSPORT_COMPANION` is
on because every touch board builds that UI (the non-companion path does not
compile any more). The version strings are read from `src/MyMesh.h`, so the
web build's About page names the same firmware as this commit's device build.

## Changes to shared source (all of them)

Every change is behaviour-neutral for the device builds (they never define
`WADA_WEB`, and the new caps default to what every board had).

| File:line | Change | Why |
|---|---|---|
| `src/ui-touch/UITask.cpp:5` | `#if defined(WADA_WEB)` include `WebMesh.h`, else `../MyMesh.h` | the mesh stand-in (a relative include cannot be redirected by include paths) |
| `src/ui-touch/UITask.cpp:212` | `WADA_WEB` branch of the display-class include chain: `<WebDisplay.h>` | the board's display, as every board adds its own here |
| `src/ui-touch/UITask.cpp:288` | `WADA_WEB` branch of the `extern ... display` chain | same |
| `src/ui-touch/UITask.cpp:34556` | `WADA_BOARD_ID "web"` | the chain ends in `#error` for unknown boards (on purpose) |
| `src/ui-touch/UITask.cpp:73461` | `WADA_WEB` branch of the LVGL resolution chain: `WADA_WEB_W x WADA_WEB_H` | the `#else` is the V4's 320x240 |
| `src/ui-touch/UITask.cpp:55216-55224` | Signal/Discover/Spectrum tiles under `CAP_RADIO_TOOLS`; Remote/VNC also under `CAP_SCREEN_MIRROR` | capability gates, not board names |
| `src/ui-touch/device_caps.h:28` | the `WADA_WEB` board block | a new board |
| `src/ui-touch/device_caps.h:503` (CAP_WEB_BROWSER) | wrapped in `#ifndef` so a board block can preset it | like CAP_LUA_APPS |
| `src/ui-touch/device_caps.h:515-520` (new) | `CAP_RADIO_TOOLS`, `CAP_SCREEN_MIRROR`, default 1 | see above |
| `src/helpers/esp32/MqttBridge.h:32` | `|| defined(WADA_WEB)` on the P4 no-op bridge | no sockets of its own yet |
| `src/ui-touch/ui_font_18.c`, `_20.c`, `_24.c` (guard lines) + `scripts/build/gen-touch-fonts.sh:418` | `|| defined(WADA_WEB)` in the big-font board list | an 800x480 board carries the UI-size fonts; the generator and its output stay in step |

That is five `WADA_WEB` sites in UITask.cpp, all at places where every board
adds a line (display, board id, resolution) or the one unavoidable include.

## The mesh stand-in and the drift alarm

`web/mesh/WebMesh.h` declares a class named `MyMesh` with exactly the members
the touch UI uses (100 distinct `the_mesh.` members across 466 call sites),
with the same signatures. The plain data types are the real MeshCore and
wadamesh headers (`ContactInfo`, `ChannelDetails`, `mesh::Identity`,
`NodePrefs`, `RegionRegistry`, `DataStore`, `ADV_TYPE_*`, `OUT_PATH_UNKNOWN`,
`MSG_SEND_*`, `TxtTxDebugInfo`). There are no templates, macros or catch-alls,
so:

> When a firmware change adds `the_mesh.someNewThing()` (or a new
> `radio_driver.` / `display.` / `heltecV4CapTouch*()` call) to the UI, the
> web build stops compiling. That is the alarm. Fix it by deciding what the
> call means in the browser: implement it on the backend, or add it as
> unavailable here and to the table below.

The stand-in keeps the same local mirror a device's MyMesh keeps (prefs,
contact table, channel table, counters, recently heard, repeats heard) with
BaseChatMesh's semantics, and calls the UI back in the same order
`src/MyMesh.cpp` does (`notify()` before `newMsgFromPubWithMeta()`, etc.).
Everything that needs the network goes through one interface,
`web/mesh/MeshBackend.h`:

- **DemoBackend** (now): the cast of `scripts/doc/web-demo/demo.py` (Sanne,
  Diego, Marta, Hilltop Repeater, Harbor Room, ...), its channels, heard nodes
  and conversations (played on the first boot of a data dir). DMs are ACKed
  after a short delay and sometimes answered, channel posts are heard
  repeated, the repeater answers status pings, someone talks on Public every
  minute or so, nodes re-advert.
- **CompanionBackend** (phase 3): the same interface over the companion frame
  protocol.

Of the 100 members, 61 have the device's semantics (contacts, channels,
sending, ACKs, repeats, RX metadata, signal, status/telemetry/trace requests
via the backend, prefs, a terminal subset). 39 are inert because nothing in a
browser can answer them yet:

| Unavailable / inert | Returns |
|---|---|
| `setBLEPin` | false (no Bluetooth) |
| `uiShareContact` | false |
| `uiExportBackup`, `uiImportBackup` | `{}` / false |
| `uiSendAdminLogin`, `uiSendAdminCommand`, `uiRoomRelogin`, `sendRegionsRequestForUI` | MSG_SEND_FAILED (phase 3: companion login/CLI) |
| `uiSendSignalProbe`, `uiStartDiscoverScan`, `discoverCount/Get/Clear` | 0 / empty (no own radio) |
| `uiRxLogCount`, `uiRxLogGet` | empty (no raw frames) |
| `getAdvertPath`, `lastRxPath` | unknown / empty |
| `uiRepeatHopCount`, `uiRepeatHop` | 0 (repeat counts work, repeater identities are not reported) |
| `getTotalAirTime`, `getReceiveAirTime`, `getRemainingTxBudget`, `isRadioReceiving`, `getProtoNumClients` | 0 / false |
| `getStore`, `getOrphanedBlobs` | nullptr / 0 (no local DataStore) |
| `lastRxScope`, `lastRxScopeSlot`, `lastRxScopeIsHome`, `pushChannelScope`, `popChannelScope`, `setScopeDirectFloods` | no region scope on the backend link yet |
| `uiHasConnectionTo`, `uiIsMeshcomodRecipient`, `cancelUIDeferredLogin` | false / no-op |
| `uiRegisterExpectedAck` | no-op (the backend matches its own ACKs) |
| `getNextCompanionRetryWakeDelay`, `hasPendingWork`, `persistSyncHistoryNow` | false / no-op (no companion retry or sync ring here) |
| `runLocalCli` | `ver`, `name`, `contacts`; anything else says it is not available |

The other board globals follow the same rule: `radio_driver` is
`web/platform/WebRadio.h` (only the members the UI calls; every reading says
"no radio"), `board` is `WebBoard`, `display` is `WebDisplay` (the RGB565
canvas LVGL flushes into), and the touch API (`heltecV4CapTouch*`) is
`WebTouch.cpp`, the CHSC6x driver's gesture rules over the SDL pointer.

## The compat layer (`web/compat/`)

Small and honest; what is faked says so in its header.

- **Arduino**: `Arduino.h`, `esp32-hal.h` (millis/micros/delay from a monotonic
  clock, GPIO no-ops, PSRAM = heap), `random()`, `Serial` to stdout. `String`,
  `Print`, `Stream`, `IPAddress`, `Client`, `FS`/`File` and the VFS file layer
  are the real arduino-esp32 2.0.17 sources (`compat/arduino/`, LGPL-2.1),
  with one const-correctness cast in `vfs_api.cpp`.
- **FreeRTOS** (`compat/freertos/`): tasks are `std::thread`s (vTaskDelete(NULL)
  ends the thread; deleting another task is logged and ignored), semaphores,
  mutexes, queues and task notifications on mutex + condition variable,
  critical sections = a recursive mutex per `portMUX_TYPE`. Priorities and core
  pinning are ignored.
- **Storage**: `SPIFFS` and `LittleFS` are the VFS layer over
  `$WADA_DATA_DIR/spiffs` and `/littlefs` (SPIFFS's flat names: a write creates
  missing folders). `Preferences`/NVS is one key-value file,
  `$WADA_DATA_DIR/nvs.bin`, rewritten atomically. `SD` has no card.
- **ESP-IDF**: `esp_heap_caps` (malloc; size queries report a fixed 8 MB free),
  `esp_timer` (monotonic clock + callback threads), `esp_system`
  (`esp_random` from the OS CSPRNG, restart = exit), and inert
  `esp_partition`, `esp_ota_ops`, `esp_core_dump`, `esp_sleep`, `nvs`,
  `esp_task_wdt`, `rtc_io`, `esp_chip_info`.
- **Network**: `WiFi` reports no Wi-Fi hardware; `HTTPClient` goes through
  one hook, `wadaHttpRequest()` (`wada_http.h`), which fails every request
  today (tiles, update check, Store and Reader take their offline paths).
- **Misc**: `mbedtls/md.h` (SHA-1/SHA-256 + HMAC for the PIN hash and the Lua
  crypto API), `rom/miniz.h` (upstream miniz's tinfl for the baked language
  and app blobs), `Wire`/`SPI` with nothing attached, rweather/Crypto's `RNG`
  on the OS CSPRNG.

## Platform (`web/platform/`)

`board.cpp` is the board's main.cpp: it defines the board globals, starts
SPIFFS, the mesh stand-in and the UI in `src/main.cpp`'s order
(`wadaBoardBegin()`), and runs one loop step at a time (`wadaBoardStep()`: UI
loop, mesh loop). `host_main.cpp` is the native front end (SDL2 events, the
scripted tour, present); `web_main.cpp` is the browser one (phase 2 below). Options: `--headless` (no window,
for CI), `--script FILE` (wait / tap / hold / drag / type / key / shot / quit;
`web/scripts/tour.wada` is the acceptance tour), `--shots DIR`, `--scale N`.

## Phase 2: the browser build

Same `web/CMakeLists.txt` under `emcmake`; only the front end differs.
`web/platform/board.cpp` owns the board globals and src/main.cpp's
setup()/loop() order; `host_main.cpp` (SDL2 window, scripted tour) and
`web_main.cpp` (a `<canvas>`) are the two front ends, one per target.
No SDL in the browser build: the page is the window.

**Threads.** Emscripten pthreads (`-pthread` on every object,
`PTHREAD_POOL_SIZE=6`, non-strict). LVGL and the UI loop run on the browser
main thread via `emscripten_set_main_loop` (one board step per animation
frame; no PROXY_TO_PTHREAD). The UI's background FreeRTOS tasks are pthreads
through the unchanged compat shim: `hist_flush` always, `tile_fetch`, the
Reader and the prefs writer on demand; file I/O from them is proxied to the
main thread by Emscripten. Waits on the main thread spin (no Atomics.wait
there) and are short. `vTaskDelete(NULL)` ends the pthread with `pthread_exit`
(the browser build has no C++ exceptions; FreeRTOS does not unwind either).
Stacks: 2 MB main, 256 KB per task. Memory: 64 MB initial, growth to 1 GB
(the tour peaks at ~17 MB RSS natively); the growable-heap cost of pthreads
only touches JS-side heap reads, which is one rectangle upload per frame,
made inside the module (`EM_JS`) where HEAPU8 is kept current.

Threads need SharedArrayBuffer, so the page must be cross-origin isolated:
`Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy:
require-corp`. `web/build.sh serve` (`web/scripts/serve.py`) sends them and
serves the precompressed `.br`/`.gz`; `web/deploy/nginx-snippet.conf` is the
same for our host. Without isolation the page says so and does not start.

**Display and input.** The LVGL flush writes the RGB565 framebuffer
(`WebDisplay`), which tracks the changed rectangle; each frame that rectangle
is converted to RGBA and put on the canvas. `web/shell/app.js` scales the
800x480 canvas to the window with the aspect kept: whole device pixels with
nearest-neighbour when that loses at most 20 % of the space (crisp on HiDPI),
smooth fractional scaling otherwise. Pointer events (mouse, pen, one finger)
map to canvas pixels; press/release edges are queued and handed to the touch
driver one per step, so a click faster than a frame is never lost. The
keyboard maps to the same CardKB codes as the host build (ASCII, Enter,
Backspace, Esc, Tab, arrows); Ctrl/Cmd/Alt shortcuts stay the browser's.

**Storage.** `/data` is an IDBFS mount (IndexedDB database `/data`), loaded in
`preRun` before `main()`, so the UI boots on the saved prefs, chat history,
NVS store and Lua app data. The compat file layer bumps one counter on every
write, close, remove, rename, mkdir, format and NVS commit
(`wadaHostFsTouched()`); the page persists once writes settle (0.8 s quiet,
at least every 5 s), and on `visibilitychange` (hidden) and `pagehide` after
flushing the lazily written chat history. `esp_restart()` (Reboot, factory
reset, ...) persists and reloads the page. "Reset demo data" deletes the
database before the next boot (`?reset=1`). Contacts are not a stand-in file:
the demo backend replays its cast at every boot, as a companion node will.

**Network.** Still off, behind one hook: `HTTPClient` sends every request
through `wadaHttpRequest()` (`web/compat/wada_http.h`) and serves the reply
from memory (getString, getSize, getStreamPtr, writeToStream). It returns
"connection refused" today. Map tiles later: implement it with a synchronous
`emscripten_fetch` from the calling pthread (the UI's `tile_fetch` task
already is one), make the tile server answer with CORS + CORP (COEP) or proxy
it same-origin, and let `WiFi` report connected when the hook is live.

**Size and speed** (release, `-O2`, Emscripten 6.0.12): wadamesh.wasm 2.45 MB
(gzip 1.28 MB, brotli 1.14 MB), wadamesh.js 98 KB (brotli 23 KB); `-Os` saves
only 2 % (most of the wasm is fonts, emoji and the baked languages/apps), so
release stays `-O2`. First frame (Chromium, cold cache, brotli): ~0.4 s local,
~0.5 s at 50 Mbit/s, ~1.5 s at 10 Mbit/s, ~7.4 s on "Fast 3G"; the boot splash
then plays as on a device. The debug variant is 12.3 MB (DWARF).

**Checked** with `web/build.sh webtest` in Chromium 156, Firefox 157 and
WebKit 27 (headless, Playwright), plus the Claude desktop browser pane: the
acceptance tour, send + reload (the sent DM is in IndexedDB and on screen after
reload), Reboot from the Power menu, Reset demo data, a 390x844 touch phone
(Chromium, WebKit), the no-isolation message. Scrolling a list: 60 fps
requestAnimationFrame, ~40 UI frames/s presented (LVGL's refresh period).

## Known gaps (phase 2)

- The mouse wheel does not scroll (drag does, as on the panel).
- A background tab pauses the UI loop (requestAnimationFrame); the demo mesh
  catches up when the tab is shown again.
- Saving on page close is best effort (IndexedDB is asynchronous); normal use
  is covered by the debounced save.
- In a private window IndexedDB may be refused: the page says the demo resets
  on reload.
- iPhone Safari has no element fullscreen; the button hides there.
- The Power menu still offers "Download mode" (no CAP gate for it yet).

## Known gaps (phase 1)

- Typing on the computer keyboard is ASCII (the UI's CardKB path); other
  scripts and emoji use the on-screen keyboard and the emoji picker.
- The Wi-Fi and Bluetooth settings pages still show (Wi-Fi even says "On")
  and do nothing; a `CAP_WIFI` / `CAP_BLE` pair would hide them.
- No map tiles (HTTP is off); contacts still plot on the empty map.
- The baked radio Lua apps (RF Monitor, Airtime, Wardrive) open but have no
  radio data.
- The Home RAM widget shows a synthetic 50 % (fixed heap report).
- The demo history is stamped with the first-boot time (the UI stamps
  outgoing bubbles with the system clock), not demo.py's "minutes ago".
- No sound, no console mode (no `DISPLAY_CLASS` text renderer), no lock screen.
- `ClockFloorRTC::pushSystemClock` calls `settimeofday`, which a normal user
  is not allowed to do; it fails harmlessly and the host clock is already right.

## Phase plan

1. **Native host + demo** (done): this document.
2. **Emscripten + IDBFS + demo deploy** (done, see "Phase 2: the browser
   build"; left: put `web/dist/` on the site with `web/deploy/nginx-snippet.conf`).
3. **Companion backend.** `CompanionBackend : MeshBackend` speaking the
   companion frames that `src/MyMesh.cpp`'s `handleCmdFrame()` serves
   (APP_START, DEVICE_QUERY, GET_CONTACTS, GET_CHANNEL, SEND_TXT_MSG,
   SEND_CHANNEL_TXT_MSG, SYNC_NEXT_MESSAGE, the PUSH codes, status/telemetry/
   login/trace requests), over Web Serial, Web Bluetooth or WebSocket, with the
   transports in JS and the framing in C++. Most of the "unavailable" table
   becomes available here.
4. **Release gate + deploy.** `scripts/release.sh` runs `web/build.sh smoke`
   (a failed compile is the drift alarm; a failed tour is a broken UI) and
   publishes the wasm build next to the flasher.
