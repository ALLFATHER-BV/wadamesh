# wadamesh — split plan

**Status: planning.** No firmware code has moved yet. This file is the tracked
roadmap for splitting the LVGL touch-UI firmware out of
[meshcomod](https://github.com/ALLFATHER-BV/meshcomod) into **wadamesh**.

## Goal

Split the touch-UI firmware (LilyGo T-Deck / T-Deck Plus + Heltec V4 TFT) out of
the meshcomod MeshCore fork into its own repo, **wadamesh**, depending on a clean
MeshCore fork via PlatformIO `lib_deps`. The result must be **behavior-identical
to the shipping betas** — this is mechanical repackaging, **not a refactor**.

## Repos & licensing — done

| Repo | Visibility | License | Role |
|---|---|---|---|
| `ALLFATHER-BV/wadamesh` | private (for now) | **GPL-3.0** | the app: `ui-touch` + companion app glue + variants. Copyleft so distributed builds/forks stay open. |
| `ALLFATHER-BV/MeshCore` | public | **MIT** | fork of `meshcore-dev/MeshCore`; the core wadamesh depends on. Stays MIT so the Wi-Fi/BLE hooks are upstreamable. |

Dependency arrow: **GPL app → `lib_deps` @ git tag → MIT core.** MIT is
GPL-compatible; MeshCore-derived files inside wadamesh keep their MIT headers
(see `NOTICE`).

## Decisions locked

- **Fork scope = minimal & upstreamable.** The fork carries only the genuine core
  hooks; everything touch/app-specific moves to wadamesh.
- **Consumption = `lib_deps` @ git tag.** Matches the existing `library.json` +
  `build_as_lib.py` — the MeshCore repo is already a PlatformIO library.
- **No refactor.** Explicitly OUT of scope (regression risk): no `IMeshController`,
  no `MyMesh` slimming, no `ContactInfo` DTOs, no capability `#ifdef`s. The
  existing `MyMesh` ↔ `ui-touch` coupling is preserved exactly as-is.
- **"Both cores" is a maintenance property, not a runtime layer.** `MyMesh`
  extends `BaseChatMesh`, so it recompiles against whichever core `lib_deps`
  resolves (the fork, or `kkazakov/meshcomod`). Not built now; deferred.

## The boundary

Draw the repo line where the code already separates:

- **Core → MeshCore fork:** the genuine mesh/transport hooks in `src/`.
- **App → wadamesh:** `examples/companion_radio/` (MyMesh, main, DataStore,
  AbstractUITask, NodePrefs) + `ui-touch/` + touch helpers +
  `variants/{lilygo_tdeck, heltec_v4_tft}`.

Core→UI is already a clean seam (`AbstractUITask`, 16 virtuals). The UI→core
coupling (~52 `the_mesh.*` calls) is **left as-is** — `MyMesh` moves into wadamesh
*with* the UI, so that coupling stays in-repo and needs no abstraction layer.

## Where each core change goes (the ~4.2k-line `src/` diff vs upstream)

**KEEP in the fork** (minimal hooks, upstreamable):
- `helpers/BaseChatMesh.{cpp,h}` — `sendMessage` / `expected_ack` / `composeMsgPacket` hooks
- `helpers/BaseSerialInterface.h` — transport abstraction surface
- `helpers/esp32/SerialBLEInterface.*` — NimBLE port (ideally behind a build flag)
- small touches: `CommonCLI`, `IdentityStore`, `EnvironmentSensorManager` (evaluate each)

**MOVE to wadamesh:**
- `LvglPsramAlloc.cpp`, `helpers/esp32/{SdNvsPrefs,TouchPrefsStore,WifiRuntimeStore}.*` → app helpers
- `helpers/input/{HeltecV4CapTouch,TDeckTouch,TDeckKeyboard,TDeckTrackball}.*` → `hal/` (prefer pulled libs where they exist)
- `helpers/ui/ST7789LCDDisplay.*` → `hal/`
- `helpers/esp32/{MultiTransportCompanionInterface,TCPCompanionServer,WebSocketCompanionServer}.*` → `app/` (companion transports)
- OTA helpers (`HttpOta*.h`, `RepeaterTcpOtaEmit.h`) → `app/`

**EVALUATE per-file during extraction** (the judgment calls):
- `helpers/ESP32Board.cpp` (+836) — split general board support (stays) vs touch/display init (moves). Biggest call.
- `helpers/ui/{DisplayDriver.h,UIScreen.h}` (+1/+2) — tiny; likely stay in fork.
- `MeshTouchTxTrace.h`, `TouchDiagTrace.*` — dev instrumentation; move or drop.

## Sequence (one topic per commit, behind a build gate)

1. **Fork baseline.** Branch `ALLFATHER-BV/MeshCore`; tag the vanilla baseline.
   Apply the minimal hook set above; tag `v1.16.0-wada.0`.
2. **wadamesh tree.** Add app + `ui-touch` + helpers + `hal/` + `variants/` + a
   root `platformio.ini` with `lib_deps = ALLFATHER-BV/MeshCore@<tag>`.
3. **Build gate.** Both touch envs build green against the fork; firmware matches
   beta_17 behavior on-device.
4. **Release pipeline.** Re-home the release process to wadamesh (new repo/domain,
   same `beta_N` mechanics). Tile-proxy + flasher: shared with meshcomod or
   forked — a separate decision.

Throughout steps 1–4 the **shipping meshcomod checkout is untouched**; the beta
cadence is unaffected.

## Risks / watch-items

- **Minimal extraction is iterative.** If the touch build needs a core symbol, add
  it to the fork as a discrete hook — don't drag in unrelated code.
- **`ESP32Board` split** is the biggest judgment call — review before moving.
- **"Both cores"** validation (building wadamesh on `kkazakov/meshcomod`) is
  deferred until step 3 proves the boundary.
- **Security gate every wadamesh push** — no `CLAUDE.md`, no `.claude/`, no infra
  details.
