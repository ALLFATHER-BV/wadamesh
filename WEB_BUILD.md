# wadamesh in the browser (WEB_BUILD)

The real wadamesh touch UI (`src/ui-touch`) built for a computer instead of an
ESP32, driving a mesh it reaches through a backend. Phase 1 (this branch) is a
native macOS/Linux build in an SDL2 window with a demo backend. Later phases put
the same build in a web page (Emscripten) and connect it to a real MeshCore
companion node.

```
web/build.sh host     # build web/build-host/wadamesh-host
web/build.sh run      # build and start it (data in web/data/)
web/build.sh shots    # run the acceptance tour headless -> web/docs/*.png
web/build.sh smoke    # the same tour, exit status only (CI, release gate)
web/build-host/wadamesh-host --serial /dev/cu.usbmodem101   # drive a real node (see "The companion backend")
web/build.sh web      # phase 2 (says so and exits)
```

Needs cmake, ninja, SDL2 (`brew install cmake ninja sdl2`). The first build
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
- **CompanionBackend**: a real MeshCore companion node, over the protocol the
  phone apps use (next section).

Of the 100 members, 77 have the device's semantics with a companion node behind
them (contacts, channels, sending, ACKs, repeats heard with the repeaters' names,
RX metadata and route, signal, the RX log, advert routes, status / telemetry /
trace / regions requests, admin login and CLI, room sessions, prefs, the node's
CLI in the Terminal). The demo backend leaves the network-only ones (login, CLI,
RX log, advert routes) unanswered. 23 are inert because nothing behind a
companion link can answer them:

| Unavailable / inert | Returns |
|---|---|
| `uiExportBackup`, `uiImportBackup` | `{}` / false (could be built from the contact list + `CMD_EXPORT_CONTACT`; not yet) |
| `uiSendSignalProbe`, `uiStartDiscoverScan`, `discoverCount/Get/Clear` | 0 / empty (no own radio; `CMD_SEND_CONTROL_DATA` could carry discovery later) |
| `getTotalAirTime`, `getReceiveAirTime`, `getRemainingTxBudget`, `isRadioReceiving`, `getProtoNumClients` | 0 / false |
| `getStore`, `getOrphanedBlobs` | nullptr / 0 (no local DataStore) |
| `lastRxScopeIsHome`, `pushChannelScope`, `popChannelScope`, `setScopeDirectFloods` | the node's region key and per-send scope are not in the protocol (a wadamesh node applies each channel's scope itself) |
| `uiIsMeshcomodRecipient` | false |
| `uiRegisterExpectedAck` | no-op (the backend matches its own ACKs) |
| `getNextCompanionRetryWakeDelay`, `hasPendingWork`, `persistSyncHistoryNow` | false / no-op (no companion retry or sync ring here) |

Moved from unavailable to working by the companion backend (16): `setBLEPin`,
`uiShareContact`, `uiSendAdminLogin`, `uiSendAdminCommand`, `uiRoomRelogin`,
`sendRegionsRequestForUI`, `uiHasConnectionTo`, `cancelUIDeferredLogin`,
`uiRxLogCount`, `uiRxLogGet`, `getAdvertPath`, `lastRxPath`, `uiRepeatHopCount`,
`uiRepeatHop`, `lastRxScope`, `lastRxScopeSlot`; and `runLocalCli` now runs every
command on the node (`CMD_RUN_CLI_COMMAND`, MeshCore 1.18+), the old subset as
the fallback.

The other board globals follow the same rule: `radio_driver` is
`web/platform/WebRadio.h` (only the members the UI calls; every reading says
"no radio"), `board` is `WebBoard`, `display` is `WebDisplay` (the RGB565
canvas LVGL flushes into), and the touch API (`heltecV4CapTouch*`) is
`WebTouch.cpp`, the CHSC6x driver's gesture rules over the SDL pointer.

## The companion backend (`web/mesh/Companion*`)

```
wadamesh-host --serial /dev/cu.usbmodem101          # USB (the node's companion over USB)
wadamesh-host --tcp 192.168.1.50:5000               # the node's TCP companion server (Wi-Fi on)
wadamesh-host --serial /dev/cu.usbmodem101 --read-only
wadamesh --companion web-serial | ble | ws:ws://192.168.1.50:8765/   # browser (Module.arguments)
```

The demo is the default; naming a transport selects the companion backend
(`web/mesh/BackendSelect.cpp`, one line in `host_main.cpp`). `--read-only` never
sends a command that changes the node (settings, contacts, channels, clock, PIN,
CLI other than `get`/`ver`/`board`/`clock`); messages, adverts and requests still
go out. `WADA_COMPANION_DEBUG=1` logs every frame.

**Protocol.** STOCK MeshCore 1.17 / 1.18 companion behaviour (`CompanionBackend.h`
has the codes), with the wadamesh differences handled: wadamesh also pushes
message frames live (so 16/17 count as the reply only while
`CMD_SYNC_NEXT_MESSAGE` is in flight, and a message seen both ways is dropped
once), keeps a per-client sync cursor (we send client id `wadamesh-web` in
`APP_START`, which stock ignores), and appends a "Meshcomod" pseudo contact
(filtered). One command is in flight at a time; pushes arrive any time.

| On link up | Frames |
|---|---|
| identity, radio, node prefs | `DEVICE_QUERY(3)` -> 13, `APP_START` -> 5 (`beSelfInfo`: the node's settings replace the stand-in's) |
| clock | `GET_DEVICE_TIME`; an UNSET node clock is set from the computer (as the phone apps do) |
| contacts | `GET_CONTACTS` -> 2, 3 x N, 4 (contacts the node no longer has are dropped) |
| channels | `GET_CHANNEL i` for every slot `DEVICE_INFO` reports |
| waiting messages | `SYNC_NEXT_MESSAGE` until 10; again on every `PUSH 0x83` |

| UI feature | Frames | Event |
|---|---|---|
| DM | `SEND_TXT_MSG` -> 6 (node's ack -> the UI's tag), `PUSH 0x82` | `onMessageAcked` (Delivered) |
| channel post | `SEND_CHANNEL_TXT_MSG` -> 0 | repeats: below |
| incoming DM / channel / room post / CLI reply | 16 / 17 (or v1 7 / 8), txt_type 0 / 2 / 1 | `newMsgFromPubWithMeta`, `newRoomMsgFromPubWithMeta`, `onAdminCommandReply` |
| status / telemetry | `SEND_STATUS_REQ` / `SEND_TELEMETRY_REQ`, `PUSH 0x87` / `0x8B`; no answer from a repeater/room/sensor in 6 s (12 s flood): guest `SEND_LOGIN`, the request again 2 s after `0x85` (the device's #683 order) | `onPingReply`, `onTelemetryReply` |
| trace ping / route | `SEND_TRACE_PATH` (our tag), `PUSH 0x89` | `onTracePingResult` (per-hop SNRs) |
| admin login, room re-login | `SEND_LOGIN`, `PUSH 0x85/0x86` | `onServerClock`, `onAdminLoginResult` |
| regions | `SEND_ANON_REQ [01 00]`, `PUSH 0x8C` with the request tag | `onRegionListReply` |
| adverts | `PUSH 0x80/0x81` -> `GET_CONTACT_BY_KEY`; `0x8A` (not added) | `discoveredContact` (Discovered list), thread refresh |
| raw RX | `PUSH 0x88` | RX log ring, 0-hop signal, advert routes, message route + scope, repeats heard |
| edits | `ADD_UPDATE_CONTACT` (favourite, telemetry perms, position, manual add), `REMOVE_CONTACT`, `RESET_PATH`, `SHARE_CONTACT`, `SET_CHANNEL`, `SEND_SELF_ADVERT`, `SET_ADVERT_NAME` / `SET_RADIO_PARAMS` / `SET_RADIO_TX_POWER` / `SET_OTHER_PARAMS` (only fields that differ from the node's), `SET_DEVICE_PIN`, `RUN_CLI_COMMAND` (Terminal) | |

**Repeats heard** work as on a device: a channel post's flood payload is
deterministic (`sendGroupMessage` + `createGroupDatagram`), so the backend
computes it, and the stand-in fingerprints every raw RX push the same way the
device's `logRxRaw` does (FNV-1a over the payload, last path hop = the
repeater). A DM is encrypted with a key only the node holds, so its echo is
recognised by destination + source hash (the first flooded TXT_MSG from us to that
contact after the send), then by that payload.

**Transports** (`CompanionTransport.h`): the backend works in frames; each
transport owns its framing.

| Transport | host -> node | node -> host | Where |
|---|---|---|---|
| USB serial, TCP | `'<'` len16 LE payload | `'>'` len16 LE payload in a byte stream (log text skipped) | `SerialTransport.cpp` (termios; DTR+RTS released in one ioctl, HUPCL off; reconnects) |
| WebSocket (port 8765) | `'<'` len16 payload in ONE binary message | the raw payload, one message per frame | `BrowserTransport.cpp` + `js/companion-transports.js` |
| BLE (Nordic UART `6e400001-b5a3-f393-e0a9-e50e24dcca9e`) | the payload, written to RX `...0002...` | one TX `...0003...` notification per frame | same |
| Web Serial | as USB | as USB | same |

The browser glue is `--pre-js` (one `if(EMSCRIPTEN)` line in CMakeLists.txt): JS
moves bytes (`WadaCompanion.open/send/close`, and `WadaCompanion.connect(kind)`
for the page's Connect button, since Web Serial / Bluetooth need a user gesture
the first time; afterwards a granted device reconnects by itself), C++ frames
them; JS reaches C++ through three `EMSCRIPTEN_KEEPALIVE` functions, no
`EXPORTED_FUNCTIONS`. Received bytes are queued under a mutex and delivered on
the UI thread, so it works with or without pthreads. Verified: an Emscripten
test program with this glue against a mock WS node (DEVICE_QUERY round trip in
node.js); the page shell that offers Connect is phase 2's.

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
- **Network**: `WiFi` reports no Wi-Fi hardware; `HTTPClient` fails every
  request (tiles, update check, Store and Reader take their offline paths).
- **Misc**: `mbedtls/md.h` (SHA-1/SHA-256 + HMAC for the PIN hash and the Lua
  crypto API), `rom/miniz.h` (upstream miniz's tinfl for the baked language
  and app blobs), `Wire`/`SPI` with nothing attached, rweather/Crypto's `RNG`
  on the OS CSPRNG.

## Platform (`web/platform/`)

`host_main.cpp` is the board's main.cpp: it defines the board globals, starts
SPIFFS, the mesh stand-in and the UI in `src/main.cpp`'s order, and runs one
loop step at a time (`wadaHostStep()`: events, UI loop, mesh loop, present),
the shape Emscripten's main loop needs. Only SDL2 APIs are used, so the same
file builds against Emscripten's SDL2 port. Options: `--headless` (no window,
for CI), `--script FILE` (wait / tap / hold / drag / type / key / shot / quit;
`web/scripts/tour.wada` is the acceptance tour), `--shots DIR`, `--scale N`.

## Known gaps

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
- Companion: a send the node refuses (unknown contact, empty channel slot) is
  only logged; the bubble stays "Sent" without Delivered.
- Companion: the UI floods every DM (its "deterministic" send); over the link the
  node routes it by its own path, as for the phone app.
- Companion: messages the node itself sent (on its own screen or from another
  app) are not in the protocol's sync, so they do not appear here.
- Companion: a stock node's waiting-message queue is shared with the phone app
  (whoever syncs first gets the messages); wadamesh keeps a cursor per client.
- Companion: 256-bit channel keys cannot be written (the protocol carries 128).
- Companion: the guest-login fallback does not reset the route first (the device
  does); the contact list is refreshed on connect and by pushes, not polled.
- `ClockFloorRTC::pushSystemClock` calls `settimeofday`, which a normal user
  is not allowed to do; it fails harmlessly and the host clock is already right.

## Phase plan

1. **Native host + demo** (done): this document.
2. **Emscripten + IDBFS + demo deploy.** `emcmake cmake -S web` with SDL2 from
   the Emscripten port; the main loop via `emscripten_set_main_loop(wadaHostStep)`;
   `$WADA_DATA_DIR` on an IDBFS mount with a `syncfs` after Preferences and
   history writes; a small HTML shell that scales the canvas and maps touch.
   Decision to make: the UI's background tasks (history flush, prefs writer)
   need pthreads (`-pthread`, so the page must be served with COOP/COEP headers)
   or a fiber-based cooperative scheduler behind the same FreeRTOS shim
   (Asyncify). `UITask.cpp` already compiles front-end clean with `em++`
   (checked on this branch). HTTP over `fetch()` for the map tiles proxy.
3. **Companion backend** (done, branch `web-companion`; see above). `CompanionBackend : MeshBackend` speaking the
   companion frames that `src/MyMesh.cpp`'s `handleCmdFrame()` serves
   (APP_START, DEVICE_QUERY, GET_CONTACTS, GET_CHANNEL, SEND_TXT_MSG,
   SEND_CHANNEL_TXT_MSG, SYNC_NEXT_MESSAGE, the PUSH codes, status/telemetry/
   login/trace requests), over Web Serial, Web Bluetooth or WebSocket, with the
   transports in JS and the framing in C++. Most of the "unavailable" table
   becomes available here.
4. **Release gate + deploy.** `scripts/release.sh` runs `web/build.sh smoke`
   (a failed compile is the drift alarm; a failed tour is a broken UI) and
   publishes the wasm build next to the flasher.
