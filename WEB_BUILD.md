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
web/build.sh web      # the browser build -> web/dist/ (static files), and the no-threads bundle
web/build.sh web-nothreads  # only the no-threads bundle -> web/dist-nothreads/ (served at nt/)
web/build.sh web-debug  # -g + runtime assertions -> web/dist-debug/
web/build.sh serve    # serve web/dist on http://localhost:8080 with COOP/COEP
web/build.sh webtest  # the same tour in headless Chromium/Firefox/WebKit (Playwright)
web/build.sh mobiletest  # phones and tablets: iPhone 14, Pixel 7, iPad (Playwright device emulation)
web/build-host/wadamesh-host --serial /dev/cu.usbmodem101   # drive a real node (see "The companion backend")
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
| CAP_LARGE_SCREEN | 1 | the large-panel layouts and UI-size fonts, at every size the page picks (800x480 on a computer, the screen's own size on a phone or tablet) |
| CAP_ROTATABLE | 0 | no rotation setting: the page picks portrait or landscape from the window, and a turn restarts the UI in the other layout |
| CAP_NATIVE_KEYBOARD (new) | 1 | on a touch device the phone's own keyboard types (`boardNativeKeyboard()`): the UI's on-screen keys, focus ring and tab hotkeys stay away |
| CAP_SD, CAP_FILESYSTEM | 0 | storage is the host directory behind SPIFFS |
| CAP_GPS | 1 | the computer's or phone's location (see "Location") |
| CAP_GPS_RECEIVER (new) | 0 | not a serial receiver: Settings > GPS has no baud rate and no rest-between-checks saver |
| CAP_BATTERY, CAP_LOCK_SCREEN, CAP_OTA, CAP_USB_FILES | 0 | no such hardware; a page updates by reloading |
| CAP_WIFI_SETUP, CAP_BT_SETUP (new) | 0 | the page's bar picks the node and the link: Settings > Wi-Fi / Bluetooth explain that instead of showing switches that change nothing |
| CAP_USB_FLASH (new) | 0 | no bootloader: the Power menu has no "Download mode" |
| CAP_RADIO_TOOLS (new) | 0 | no own radio: no Signal / Discover / Spectrum tiles |
| CAP_SCREEN_MIRROR (new) | 0 | it is the browser view: no Remote / VNC tiles |
| CAP_WEB_BROWSER | 0 | a page cannot fetch arbitrary sites (CORS) |
| CAP_BUILTIN_LANGS, CAP_BUILTIN_LUA_APPS | 1 | translations and catalog apps are baked in: the offline first load |
| CAP_STORE_DOWNLOADS (new) | 1 | ...and the Store installs and updates anyway (Apps tab, language downloads) |
| CAP_FW_UPDATE_CHECK (new) | 0 | a reload is the update: no firmware check, no beta report, no install count; the Store badge fetches the catalog itself |
| CAP_FILE_HANDOFF (new) | 1 | an exported backup is downloaded by the browser; "Import from this computer" opens its file picker |
| CAP_EXT_KEYBOARD, CAP_KEYPAD_NAV | 1 (derived) | the computer keyboard is the UI's external keyboard (`HAS_CARDKB`) |

**Why 800x480 on a computer.** It is the size of the two shipping large-screen layouts (the
CrowPanel 5.0 with touch, the Tanmatsu with keys), so the web build runs a
layout path real devices already exercise, with the UI-size fonts, and it
fits a laptop browser window at 1:1. Phones and tablets get the size of their own
screen instead (see "Phones and tablets").

Build defines (all in `web/CMakeLists.txt`): `WADA_WEB ESP32 HAS_TOUCH_UI UI_LVGL
MULTI_TRANSPORT_COMPANION HAS_CARDKB WADAMESH_FORK_BUILD MAX_CONTACTS=500
MAX_GROUP_CHANNELS=40 HAS_GPS_MOTION` and `WADA_TILE_SERVER` (read from
`DEFAULT_TILE_SERVER` in `src/helpers/esp32/TouchPrefsStore.cpp`, like the version). `ESP32` is on because the UI's ESP32 code paths are the
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
| `src/ui-touch/UITask.cpp:34584` | `WADA_BOARD_ID "web"` | the chain ends in `#error` for unknown boards (on purpose) |
| `src/ui-touch/UITask.cpp:73542` | `WADA_WEB` branch of the LVGL resolution chain: `WADA_WEB_W x WADA_WEB_H` (the size the page picked at boot) | the `#else` is the V4's 320x240 |
| `src/ui-touch/UITask.cpp:9855-9859` | `CAP_NATIVE_KEYBOARD`: a second tap on a field does not summon the UI's keys while the phone's keyboard types | the page reopens the phone keyboard instead |
| `src/ui-touch/UITask.cpp:49627` | `cardKbUiKey` passes `BLE_KEY_TEXT \| code point` through | text in any script and emoji from the page (a CardKB only ever sends bytes, never that bit) |
| `src/ui-touch/UITask.cpp:51733-51748` | `CAP_NATIVE_KEYBOARD`: the phone's keyboard counts as an external keyboard for typing, but turns on no focus ring and no tab hotkeys or their letters | a phone keyboard has no keys to navigate by |
| `src/ui-touch/UITask.cpp:55254-55262` | Signal/Discover/Spectrum tiles under `CAP_RADIO_TOOLS`; Remote/VNC also under `CAP_SCREEN_MIRROR` | capability gates, not board names |
| `src/ui-touch/UITask.cpp:4189-4193` | the GPS "searching" hint: sky + cold start under `CAP_GPS_RECEIVER`, else "allow this page to use your location" | the browser's GPS has no sky |
| `src/ui-touch/UITask.cpp:17639, 17679` | Settings > GPS: the Receiver (baud) and Battery (saver) cards under `CAP_GPS_RECEIVER` | settings of a serial receiver |
| `src/ui-touch/UITask.cpp:19243-19251` | Settings > Bluetooth: `!CAP_BT_SETUP` shows where the connection is set | no switches that change nothing |
| `src/ui-touch/UITask.cpp:20290-20298` | Settings > Wi-Fi: `!CAP_WIFI_SETUP` likewise | same |
| `src/ui-touch/UITask.cpp:40894` | the Bluetooth row's On/Off state under `CAP_BT_SETUP` | same |
| `src/ui-touch/UITask.cpp:52628-52634, 52689` | Power menu: "Download mode" under `CAP_USB_FLASH`, the card sized by its rows | no bootloader in a browser |
| `src/ui-touch/UITask.cpp:33922, 53974, 54679` | Store: the catalog's `"radio": "own"` field, honoured with `CAP_RADIO_TOOLS` | apps that need the board's own radio (below) |
| `src/ui-touch/lua_builtin.h` (generated) + `scripts/build/gen-lua-builtin.py` | apps with `"radio": "own"` baked under `#if CAP_RADIO_TOOLS` | same |
| `deploy/apps/apps.json` | `"radio": "own"` on RF Monitor, Airtime, Wardrive | same |
| `src/ui-touch/UITask.cpp:9153-9165` | `!CAP_FW_UPDATE_CHECK`: no version check; the Store badge fetches the catalog every 6 h | Store badge without the firmware check |
| `src/ui-touch/UITask.cpp:54501, 54511, 54825, 54884` | the Store's picker-only mode is `CAP_BUILTIN_* && !CAP_STORE_DOWNLOADS` | baked apps AND a working Store |
| `src/ui-touch/UITask.cpp:49385, 49516, 49529, 77989` | `CAP_FILE_HANDOFF`: the export is handed to the board (download), "Import from this computer" (file picker), the picked file goes to the usual import confirm | backups in and out of a browser |
| `src/ui-touch/device_caps.h:28` | the `WADA_WEB` board block | a new board |
| `src/ui-touch/device_caps.h:561-565` (new) | `CAP_NATIVE_KEYBOARD` default 0 | every device board's value |
| `src/ui-touch/device_caps.h:507` (CAP_WEB_BROWSER) | wrapped in `#ifndef` so a board block can preset it | like CAP_LUA_APPS |
| `src/ui-touch/device_caps.h:519-555, 657-675` (new) | `CAP_RADIO_TOOLS`, `CAP_SCREEN_MIRROR`, `CAP_WIFI_SETUP`, `CAP_BT_SETUP`, `CAP_USB_FLASH`, `CAP_GPS_RECEIVER`, `CAP_FW_UPDATE_CHECK` default 1; `CAP_FILE_HANDOFF` default 0; `CAP_STORE_DOWNLOADS` default `!CAP_BUILTIN_LUA_APPS` | each a board's value today (behaviour-neutral) |
| `src/helpers/esp32/MqttBridge.h:32` | `|| defined(WADA_WEB)` on the P4 no-op bridge | no sockets of its own yet |
| `src/ui-touch/ui_font_18.c`, `_20.c`, `_24.c` (guard lines) + `scripts/build/gen-touch-fonts.sh:418` | `|| defined(WADA_WEB)` in the big-font board list | an 800x480 board carries the UI-size fonts; the generator and its output stay in step |

That is five `WADA_WEB` sites in UITask.cpp, all at places where every board
adds a line (display, board id, resolution) or the one unavoidable include; every
other difference is a capability flag that a future board can use too.

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
| private key in a backup | the export writes `"private_key": ""` and the import never sends one: the demo has no key, a node keeps its own (export it on the node, behind its own setting) |
| `uiSendSignalProbe`, `uiStartDiscoverScan`, `discoverCount/Get/Clear` | 0 / empty (no own radio; `CMD_SEND_CONTROL_DATA` could carry discovery later) |
| `getTotalAirTime`, `getReceiveAirTime`, `getRemainingTxBudget`, `isRadioReceiving`, `getProtoNumClients` | 0 / false |
| `getStore`, `getOrphanedBlobs` | nullptr / 0 (no local DataStore) |
| `lastRxScopeIsHome`, `pushChannelScope`, `popChannelScope`, `setScopeDirectFloods` | the node's region key and per-send scope are not in the protocol (a wadamesh node applies each channel's scope itself) |
| `uiIsMeshcomodRecipient` | false |
| `uiRegisterExpectedAck` | no-op (the backend matches its own ACKs) |
| `getNextCompanionRetryWakeDelay`, `hasPendingWork`, `persistSyncHistoryNow` | false / no-op (no companion retry or sync ring here) |

The repeater admin app (main's `RepeaterAdminApp.h`) added `sendStatusPingForUI`,
`sendTelemetryRequestForUI`, `uiSendBinaryRequest` (-> `AbstractUITask::onBinaryReply`),
`UiReqKind::Binary` and `uiLastLoginInfo()` / `UiLoginInfo`: over a node they are
`SEND_STATUS_REQ` / `SEND_TELEMETRY_REQ`, `CMD_SEND_BINARY_REQ` answered by
`PUSH_BINARY_RESPONSE` with the request's tag, and the ACL + firmware-level bytes of a
modern `PUSH_LOGIN_SUCCESS` (`beLoginInfo`); the demo's Hilltop Repeater and Harbor Room
take any password as the admin's (empty: guest) and answer status, neighbours, the
access list, owner info and a few CLI lines (screenshots `web/docs/admin-demo-*.png`;
the mock node answers neighbours and owner info for `nodetest`-style runs).

Moved from unavailable to working: `uiExportBackup`, `uiImportBackup` (see
"Backups"); by the companion backend (16): `setBLEPin`,
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
```

In the browser, the page's Connect button picks the node (see "Connecting the
page to a node" below).

The demo is the default. A front end fills a `WadaBackendChoice`
(`web/mesh/BackendSelect.h`: host_main.cpp from `--serial` / `--tcp` /
`--read-only`, web_main.cpp from the page) and the shared boot in `board.cpp`
creates the backend from it, so both front ends select the same way. `--read-only` never
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

### Connecting the page to a node

The bar's **Connect** opens a chooser with the transports this browser has: USB
cable (Web Serial) and Bluetooth (Web Bluetooth) on Chrome / Edge, Wi-Fi
(WebSocket to `ws://HOST:8765/`, the node's companion server) everywhere; Safari
and Firefox get Wi-Fi and a short note. Wi-Fi from an `https://` page is blocked
by the browser (mixed content), which the chooser says.

- The choice (`{kind, arg, key}`) is kept in localStorage; the page passes it to
  `web_main.cpp` (`Module.wadaBackend`) and the UI boots on that backend.
  Switching (Connect, Disconnect) saves and reloads; a reload reconnects.
- **Data never mixes**: each backend has its own data dir under the IndexedDB
  mount: `/data/demo`, `/data/node-<first 4 key bytes>`. A node's first session
  runs on `/data/node-new` until its key is known and moves there on the next
  start; a different node on the same cable or address switches to its own
  dir. The old top-level demo data moves into `/data/demo` once; "Reset demo
  data" clears only that.
- A node is already set up, so its first boot skips the demo's setup wizard
  (it would rename and re-tune the node).
- The badge says `Demo`, `Connecting`, `Connected: <node name> over USB /
  Bluetooth / Wi-Fi` or `Reconnecting` (from `g_wada_link_hook`). A lost link
  is retried every 3 s; a granted USB device reconnects when it is plugged in.
- Web Serial reopens the granted port (matched by USB VID:PID) without a
  prompt; Web Bluetooth does where the browser has `bluetooth.getDevices()`.
  Otherwise the bar shows **Reconnect** / **Pair again** (one click, the
  browser's picker).
- `web/build.sh nodetest` runs it end to end in Chromium against
  `web/mesh/test/mock_ws_node.py`: chooser, connect over Wi-Fi, the node's
  contacts / channels / waiting messages, a DM that gets Delivered and answered,
  a live message, reload reconnects with the history, Disconnect returns to an
  untouched demo (`web/docs/node-*.png`).

## Location (the browser's position is the board's GPS)

The web board has `CAP_GPS 1`. The UI gets a position here exactly as on a device:
`SensorManager::getLocationProvider()`, switched by `setSettingValue("gps", ...)`
(UITask::toggleGPS, setGpsPower, the boot resume), copied into
`sensors.node_lat/lon` by `UITask::updateGpsLocation`. `web/platform/WebLocation.*`
is that SensorManager + LocationProvider, fed by the host instead of NMEA, so the
GPS switch (Settings > GPS, the Control Center chip), the status bar pin, the GPS
status text, map centring and follow, distances and bearings, `wada.sys.gps()` (with
speed and course: `HAS_GPS_MOTION`, `wadaGpsMotion`) all work unchanged.

- **Browser**: `navigator.geolocation.watchPosition` (`web/shell/app.js`, through
  `Module.wadaGeoStart/Stop`), running only while the UI has its GPS on. Nothing asks
  on page load: when the visitor has not decided yet (permission state `prompt`) the
  request waits for a tap (the GPS switch itself, opening the map, any tap); when the
  GPS was left on, a granted page simply resumes. Refused or unavailable = no fix (the
  UI says "searching", with a note to allow the location), one toast, no retry loop;
  switching the GPS off and on asks again. Accuracy is kept (the provider's
  diagnostics line); the UI shows no HDOP, and satellites read 0.
- **Native**: `WADA_GPS="lat,lon"` gives a fixed position; unset = no fix.
- **Privacy rule.** A device advertises its position only while `advert_loc_policy`
  is "share". In the browser the advert is the node's, so the same setting decides
  whether this computer's position is handed to the node at all
  (`WebMesh.cpp syncAdvertPosition`): share off = the position stays in this page
  (map, distances, Lua), nothing is sent; share on = `CMD_SET_ADVERT_LATLON` with the
  Location privacy displacement applied (the device's `advertPosition()` math, so a
  stock node advertises the displaced point), when the UI's position differs from the
  node's by more than ~11 m, at most every 2 minutes. The demo has no node to tell;
  `--read-only` refuses the command. While this computer has a fix it wins over the
  node's stored position on the map, as a device's fix does. Stricter than a device in
  one way: a device's location telemetry also answers with its GPS position; here
  nothing but the share setting sends it. `web/build.sh nodetest` checks both halves
  against the mock node.
- **Demo**: when the visitor allows the location, the demo's cast moves around it
  (demo.py's offsets, so distances look right) and keeps that place for the next
  visit (`/web_demo_home`, cleared by "Reset demo data"); otherwise Amsterdam.

## Map tiles

The tile fetch task (`tile_fetch`, a pthread) downloads with the firmware's own code;
`wadaHttpRequest()` lets through only URLs under the firmware's default tile server
(`WADA_TILE_SERVER`, read from `DEFAULT_TILE_SERVER` at configure time):

- **Browser**: a synchronous `emscripten_fetch` (`-sFETCH`) on that pthread, to the
  same-origin path `tiles/<z>/<x>/<y>.jpg` next to the page (and `tiles/elev?...`,
  line of sight). Same origin needs no CORS and satisfies COEP. A call from the main
  thread fails at once. The page's host proxies it: `web/scripts/serve.py` (path
  allow-list, identifying User-Agent, `Cache-Control: public, max-age=1209600`) and,
  for the public host, the `location ^~ /web/tiles/` block in
  `web/deploy/nginx-snippet.conf` (proxy_cache 14 days).
- **Native**: plain HTTP/1.0 over a socket straight to the tile server; off in
  `--headless` runs unless `WADA_NET=1` (the smoke tour stays offline).
  `WADA_HTTP_DEBUG=1` logs every request.
- Tiles are cached as on a device, in the LittleFS "tiles" volume (here under the
  backend's data dir, so in IndexedDB: the map works offline afterwards), within the
  device's 4.75 MB budget.
- The UI paces its downloads (OSM etiquette), so a fresh area fills in over a few
  seconds. `web/build.sh webtest` checks the map shows tiles around Ghent when the
  server can reach the tile server (skipped without a network).

## Two copies: https (threads) and http (no threads)

The public page is served twice (app.wadamesh.com): **https** is the full client (Web
Serial, Web Bluetooth and the location need a secure context); **http** is the one that
can reach a node over Wi-Fi, because a node's companion server speaks plain `ws://`,
which a secure page may not open (mixed content). On http (other than localhost) a page
is never cross-origin isolated, so there is no SharedArrayBuffer and no pthreads.

- Two bundles, one page: `app.js` loads `wadamesh.js` next to itself when
  `crossOriginIsolated`, else `nt/wadamesh.js` (`web/dist-nothreads`, deployed at
  `/nt/`). `web/build.sh web` builds both (the page's `?v=` hash covers both);
  `web/build.sh serve` serves `nt/` too. Tile and Store URLs are relative, so both
  bundles use the same `tiles/` and `fw/apps/` next to the page. The page (shell) is
  one: phones get the same portrait layout and phone keyboard on either copy.
- **No-threads bundle** (`-DWADA_NO_THREADS=ON`, `-sASYNCIFY`): the FreeRTOS tasks
  (hist_flush, tile_fetch, the prefs writer, ...) are Emscripten fibers on the page's one
  thread, `web/compat/src/freertos_coop.cpp`, run once per frame after the UI step.
  Every wait is a yield point (vTaskDelay, semaphore / queue / notification waits,
  `delay()`, `yield()`); a wait on the UI thread runs the tasks meanwhile and, inside
  the main loop's step only, lets the browser in with `emscripten_sleep` (a call from
  the page, such as the flush before a reload, must not unwind: there the wait runs
  the tasks, and gives up after 3 s). HTTP from a task is an async `emscripten_fetch`
  the task waits for (the synchronous one would block the only thread); from the UI
  thread it fails at once, as in the threaded build. `esp_timer` callbacks are small
  tasks. Size: wasm 3.76 MB (gzip 1.61 MB) against 2.56 MB (1.32 MB) threaded (Asyncify
  instruments the code); the scroll probe measures the same ~40 frames/s.
- **Messages**: on http the chooser offers Wi-Fi (USB and Bluetooth do not exist there)
  and links the https copy "for USB, Bluetooth and your location"; on https the Wi-Fi
  row has "Open the Wi-Fi version", which opens the http copy with the address
  (`#wifi=<address>`: the http page opens the chooser with it filled in). A Wi-Fi
  connect tried from https anyway fails as mixed content (or silently times out): the
  help under the bar says so, with the same button. A browser that is not isolated
  on https either gets the no-threads bundle; if a bundle cannot load the user sees a
  plain message, developer hints only on localhost.
- Checked: `web-tour.mjs` and `node-tour.mjs` against `http://<LAN IP>:8081/`
  (`serve.py --bind <IP>`; Chrome treats only localhost as secure): demo tour,
  persistence, map tiles, Store install + run, backups, mock node connect / DM /
  reload. Location is unavailable on an insecure page (skipped there).

## Store and Lua apps

The Store works as on an 8 MB device: the Apps tab (catalog, Get / Update / Remove,
the app's own `"files"`), the Built-in tab, the Languages tab (catalog + downloads),
and the update badge. `CAP_STORE_DOWNLOADS 1` keeps the baked apps and languages as
the offline first load and turns the Store on; a downloaded copy wins, as everywhere.

- **Path**: the firmware's own install code over `wadaHttpRequest()`: every
  `http://firmware.wadamesh.com/apps/...` URL becomes the same-origin `fw/apps/...`
  next to the page. `web/scripts/serve.py` proxies exactly the catalog, app, file
  and `.lang` paths (`STORE_PATH`; catalogs `no-cache`, version folders a week), and
  `location ^~ /web/fw/apps/` in `web/deploy/nginx-snippet.conf` does the same on the
  site. The rest of that server (update check, OTA images, beta reports, the install
  count) is refused in the compat layer and has no proxy: `CAP_OTA 0`,
  `CAP_FW_UPDATE_CHECK 0`. `web/CMakeLists.txt` fails the configure if the catalog URL
  moves in UITask.cpp (`WADA_STORE_BASE`).
- **Storage**: installed apps (`apps/<id>.lua/.json`, `apps/<id>.d/`), app data
  (`wada.store`, `wada.fs`, saves, widgets) and languages (`lang/`) live in the
  backend's own data dir (IndexedDB), so they survive reloads and never mix between
  the demo and a node. The firmware's install writes a `.tmp` through its bounce
  buffer and renames it; rename is a plain VFS rename on MEMFS/IDBFS and bumps the
  write counter, so the page persists after an install like after any write.
- **wada.net** (HTTP from apps): no open proxy. A request to any other server is a
  direct browser fetch: it works when that server sends CORS headers and fails
  cleanly otherwise (the app sees an error). No published app uses wada.net today.
- Apps that need the board's own radio are left out (below). The live catalog shows
  them greyed only once the `"radio"` field is published (deploy-apps).

## Backups

Settings > Backups works with the firmware's own code:

- **Export**: `MyMesh::uiExportBackup` in the stand-in writes the device's file
  (`src/MyMesh.cpp`'s fields, the MeshCore app's shape plus wadamesh's blocks and the
  app preferences) without the private key; the UI saves it in the data dir as on a
  device, and `CAP_FILE_HANDOFF` hands it to the browser, which downloads it
  (`web/platform/WebFiles.cpp boardHandOffFile`, a Blob + `<a download>`; the UI's
  button press is the gesture).
- **Import**: "Import backup" lists the saved ones as on a device; "Import from this
  computer" opens the browser's file picker (`boardPickFile`), the page writes the
  file into this backend's SPIFFS, and the UI's usual confirm + import + restart
  follow. The stand-in's import applies name, radio, wadamesh and app settings,
  position, channels and contacts (a node: `SET_ADVERT_NAME`, `SET_RADIO_PARAMS`,
  `SET_OTHER_PARAMS`, `SET_CHANNEL`, `ADD_UPDATE_CONTACT`; contacts are added and
  updated, never deleted from a node over the link). A restart waits up to 20 s for
  those commands to reach the node (`web_main.cpp drainThenReboot`). The demo replays
  its cast and channels at every start, so there an import keeps the settings.
- **This browser**: the bar's More menu has "Back up this browser" (one `.tar` of the
  active backend's data dir: chats, settings, installed apps and their data,
  languages; not the map tile cache) and "Restore a browser backup" (replaces that
  data dir, then restarts), for moving to another browser or computer.

## Lua apps that need the board's own radio

RF Monitor (noise floor, margin), Airtime (channel load) and Wardrive (discovery
scans) read the device's own LoRa radio, which the browser does not have. They are
left out through the catalog: `"radio": "own"` in `deploy/apps/apps.json`, baked under
`#if CAP_RADIO_TOOLS` by `gen-lua-builtin.py`, and greyed in the Store's Apps tab.
A separate field, not a `"requires": "radio"` value, because `requires` holds one
name and Wardrive already needs `sdk_ext`: older firmware compares it as a whole
string and treats an unknown value as runnable, so `"sdk_ext,radio"` would have
offered Wardrive on the 2 MB V4 again. Older firmware ignores the new field (every
device has a radio anyway). Nearby and Ping work from contacts, messages and the
position and stay.

## Input

Mouse and finger are the touch panel; the keyboard is the external keyboard. The mouse
wheel and two-finger trackpad scrolling scroll the list or page under the pointer
(`web/platform/WebLvglInput.cpp`: LVGL's own hit test, the first ancestor that can still
scroll that way, `lv_obj_scroll_by`, SCROLL_END once the wheel stops; vertical only).
The tour scripts have `wheel X Y DY`.

Typing on a computer keyboard: ASCII, Enter, Backspace, Esc, Tab and arrows are CardKB
codes (hotkeys and navigation work); any other character (accents, other scripts) goes
in as text, UTF-8, through `cardKbUiKey`'s `BLE_KEY_TEXT` path.

## Phones and tablets

**The size.** `web/shell/app.js` picks the board's resolution once per boot from the
window (`Module.wadaSize`, read by `web_main.cpp` before the display exists;
`WebDisplay` takes any size; natively `wadamesh-host --size WxH`):

| Window | Canvas | Shown |
|---|---|---|
| a computer (fine pointer), landscape | 800x480 | the framed device, scaled to the window, whole device pixels when that costs at most 20 % (unchanged; the tours and docs shots are at this size) |
| a phone or tablet (coarse pointer), or a computer window clearly taller than wide | the space below the bar, safe areas out, at about one CSS pixel per canvas pixel | edge to edge, no frame |

The phone width is the phone's own CSS width (360-430), so the UI's text is as big as a
web page's, with a whole number of device pixels per canvas pixel when one is within
16 % (crisp, nearest-neighbour): iPhone 14 390x616 (Safari, at 3:1), Pixel 7 360x692
(3:1; 412 CSS px wide), iPad 540x686 (3:1, 1.5 CSS px each; tablets stay near 600
wide). The height fills the rest. Landscape picks the height the same way (up to 600)
and the width from the window's aspect: iPhone 14 664x358, Pixel 7 734x330, iPad
720x506. Why not 800x480 scaled on a tablet or a phone on its side: the bars around a
fixed aspect waste up to a third of a phone screen, and a phone's 390 CSS px would
draw a 480- or 800-wide UI at 0.8 or 0.5 scale (small text). The computer keeps
800x480 because a desktop window is resized freely (each new size would mean a
restart) and the frame at 1:1 is the intended look there.

**Layouts reused.** No new layout code: the large-screen layouts (`CAP_LARGE_SCREEN`,
the UI-size fonts, `PSC`/`PCW` popup sizing clamped to the screen) already size
themselves from `lv_disp_get_hor_res/ver_res`, as on the CrowPanel 5.0 in portrait
(480x800), the Tanmatsu, and the T-Display P4 (284x616 portrait, 616x284 landscape,
also `CAP_LARGE_SCREEN`). Checked at 360-540 wide portrait and 330-506 high landscape:
Home widgets, chat list, conversation + composer, contacts, the contact popup, map,
Settings, the app drawer.

**Turning the device.** A turn that really flips the aspect (and changes the window's
width: a keyboard or a toolbar only changes its height) saves the UI's data
(`_wadaWebFlush` + IndexedDB) and reloads into the other layout, 600 ms after the
last resize, never while the phone keyboard is open (it checks again when the keyboard
closes). The UI comes back on Home; chats, sent messages and settings are kept.

**The phone's own keyboard** (`CAP_NATIVE_KEYBOARD`). On a touch device the UI's
on-screen keys stay down (`wadaWebNativeKeyboard(1)`: the phone keyboard counts as an
external keyboard, without the focus ring and tab hotkeys a real keyboard gets). A tap on
an LVGL text field (`wadaWebTextareaAt`, asked inside the tap's own `pointerup`, since
iOS opens a keyboard only for a focus() made in the user's gesture) focuses a hidden
`<textarea id="ime">`: autocorrect, emoji, every language, voice typing. Its changes are
forwarded as a diff (Backspaces + UTF-8 text) to the bound field, so a composing keyboard
(Gboard, CJK IMEs) is followed live and an autocorrection replaces the word; a leading
sentinel space lets Backspace work on an empty field; Enter (and the keyboard's Send key,
`enterkeyhint="send"`) is the UI's Enter; the keyboard closes when the edit ends in the UI
(`wadaWebEditing`). While it is up, `visualViewport` slides the screen up so the
composer sits on the keyboard.

**The page on a phone.** One slim bar row (logo, status badge, Connect, the More menu;
labels shorten and never wrap; slimmer still on a phone on its side); `viewport-fit=cover`
with the safe areas (notch, home indicator, rounded corners) kept clear; no page zoom
(`maximum-scale=1`, 16 px hidden field); `overscroll-behavior: none` (no
pull-to-refresh). "Add to Home Screen" opens it standalone: `manifest.webmanifest`
(`display: standalone`, theme and background colours, 192/512 + maskable icons) and
the Apple tags (`apple-mobile-web-app-capable`, a black-translucent status bar,
`icons/icon-180.png`). The icons are `web/shell/icons/app-icon.svg` rendered by
`web/shell/icons/render.mjs`. The Connect chooser shows only what the browser has: iPhone
and iPad get Wi-Fi with one line ("On iPhone and iPad the browser has no Bluetooth or
USB: connect over Wi-Fi."), Android Chrome gets Bluetooth.

**Tests.** `web/build.sh mobiletest` (`web/scripts/mobile-tour.mjs`, Playwright device
emulation: iPhone 14 and iPad in WebKit, Pixel 7 in Chromium): the canvas covers more
than 90 % of the window below a one-row bar; tap into a chat (taps go by label text,
`wadamesh.findText`, or by the canvas's own size), type "Grüße vom Handy 👋" through the
hidden field, the LVGL field holds exactly that (`wadamesh.editText`), the UI's keys stay
down, Enter sends, it is saved as UTF-8; on the Pixel an IME composition grows, commits
and is deleted; turn to landscape (fills, the message is there) and back (same size,
the message is in the chat); the chooser; the manifest and icons; no page errors.
Screenshots `web/docs/mobile-*.png`. The desktop tour (`webtest`) still runs at 800x480;
its phone step checks the portrait canvas.

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
- **Network**: `WiFi.status()` is `WL_CONNECTED` while the browser is online
  (`navigator.onLine`; natively `WADA_NET`), with no address of its own
  (`localIP()` 0.0.0.0, so the version check, File Transfer and Remote stay off);
  `HTTPClient` goes through one hook, `wadaHttpRequest()` (`wada_http.h`): the
  tile server and the Store same-origin (see "Map tiles", "Store"), other servers
  directly (CORS), never the update check, OTA images or reports.
- **Misc**: `mbedtls/md.h` (SHA-1/SHA-256 + HMAC for the PIN hash and the Lua
  crypto API), `rom/miniz.h` (upstream miniz's tinfl for the baked language
  and app blobs), `Wire`/`SPI` with nothing attached, rweather/Crypto's `RNG`
  on the OS CSPRNG.

## Platform (`web/platform/`)

`board.cpp` is the board's main.cpp: it defines the board globals, starts
SPIFFS, the mesh stand-in and the UI in `src/main.cpp`'s order
(`wadaBoardBegin(choice)`), and runs one loop step at a time (`wadaBoardStep()`: UI
loop, mesh loop). `host_main.cpp` is the native front end (SDL2 events, the
scripted tour, present); `web_main.cpp` is the browser one (phase 2 below). Options: `--headless` (no window,
for CI), `--script FILE` (wait / tap / hold / drag / type / key / shot / quit;
`web/scripts/tour.wada` is the acceptance tour), `--shots DIR`, `--scale N`.
`key NAME [COUNT]` takes enter, esc, bs, tab, up, down, left, right, a single
character, or a raw byte as `0x82`; COUNT pushes it that many times at once, the
way a busy device drains several queued keys before it renders.

**ThinkNode M9 flavour.** `cmake -S web -B web/build-host-m9 -DWADA_HOST_BOARD=m9`
builds the same app with the M9's key path (`HAS_M9_KEYBOARD`: the d-pad, the
function keys, the Back ladder) in place of the external keyboard, so M9
navigation can be scripted headless. Run it with `--size 320x240`. The host keys
arrive as the controller's bytes (`platform/WebM9Keyboard.cpp`): the arrows and
Enter as they are, Esc as BACK, and a script sends the others raw (`key 0x81`
MSG, `0x82` HOME, `0x83` @, `0x84` ADV, `0x85` MAP, `0x88` triangle, `0x90` CTRL,
`0xA3` held OK). The rest of the board stays the web board (its caps, no SD).

The demo backend takes `WADA_DEMO_CHATTER_MS=N` (a line on Public every N ms, so
messages arrive at known times) and `WADA_DEMO_CHATTER_BURST=N` (the first N lines
20 ms apart, a long history).

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
canvas (800x480 on a computer; phones: "Phones and tablets") to the window with the aspect kept: whole device pixels with
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

**Network.** One hook: `HTTPClient` sends every request through
`wadaHttpRequest()` (`web/compat/wada_http.h`) and serves the reply from memory
(getString, getSize, getStreamPtr, writeToStream). See "Map tiles" and "Store".

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

- A background tab pauses the UI loop (requestAnimationFrame); the demo mesh
  catches up when the tab is shown again.
- Saving on page close is best effort (IndexedDB is asynchronous); normal use
  is covered by the debounced save.
- In a private window IndexedDB may be refused: the page says the demo resets
  on reload.
- iPhone Safari has no element fullscreen; the button hides there.

## Known gaps

- A computer keyboard's IME composition (Chinese, Japanese, ...) does not reach the
  canvas; on a phone it does (the hidden field). Dead keys and other scripts work.
- Phones: the phone keyboard has no cursor movement inside the LVGL field (it types and
  deletes at the end); a turn restarts the UI on Home.
- The Control Center's Wi-Fi chip still toggles the (inert) Wi-Fi preference;
  Settings > Wi-Fi explains.
- A custom tile server (Map options) is not reachable from a page: only the
  default one is proxied.
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
2. **Emscripten + IDBFS + demo deploy** (done, see "Phase 2: the browser
   build"; left: put `web/dist/` on the site with `web/deploy/nginx-snippet.conf`).
3. **Companion backend** (done; see "The companion backend"). `CompanionBackend : MeshBackend` speaking the
   companion frames that `src/MyMesh.cpp`'s `handleCmdFrame()` serves
   (APP_START, DEVICE_QUERY, GET_CONTACTS, GET_CHANNEL, SEND_TXT_MSG,
   SEND_CHANNEL_TXT_MSG, SYNC_NEXT_MESSAGE, the PUSH codes, status/telemetry/
   login/trace requests), over Web Serial, Web Bluetooth or WebSocket, with the
   transports in JS and the framing in C++. Most of the "unavailable" table
   becomes available here.
4. **Release gate + deploy.** `scripts/release.sh` runs `web/build.sh smoke`
   (a failed compile is the drift alarm; a failed tour is a broken UI) and
   publishes the wasm build next to the flasher.
