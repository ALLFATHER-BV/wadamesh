# Lua apps — plan + tracker

Goal: non-core apps become **Lua scripts** installed from a store catalog on
firmware.wadamesh.com — installable, uninstallable and updatable without cutting
a beta, one app artifact for every board, and writable by the community without
forking the firmware. Runtime target: **Lua 5.4** (vanilla, no JIT — smallest
serious VM, trivial to sandbox, portable Xtensa/RISC-V).

Non-goals (v1): native dynamic loading (ELF), Lua access to LoRa TX or protocol
frames, multi-file app packages, app signing (the curated catalog is the trust
layer, same as firmware bins today).

## Execution model — the decision that keeps this safe

Lua runs **on the UI task**, event-driven. No app thread, no preemption:

- The host calls the app's callbacks: `on_open(w, h)`, `on_tick(dt_ms)`,
  `on_input(ev)`, `on_close()`. All LVGL work stays on the LVGL thread by
  construction (LVGL is not thread-safe).
- Every callback runs under a `lua_sethook(LUA_MASKCOUNT)` instruction budget
  (~100k instructions ≈ low single-digit ms). Budget exceeded → error → app
  closed with a toast. A buggy app can never stall the mesh loop or trip the WDT.
- Every entry is wrapped in `lua_pcall`. A Lua error NEVER reboots the device:
  toast "App error: <msg>", close the app, free the state. Crash containment is
  the whole point.
- `lua_newstate` gets a custom allocator on **PSRAM** (`heap_caps_malloc`
  SPIRAM) with a hard per-app cap (256 KB to start). The V4's tight internal
  DRAM (#192) is never touched by app heaps.
- One app at a time (matches today's overlay model). The app root is one
  `k_popup_registry` row — red-X/Esc/status-bar close come free.

## The `wada.*` API (v1) — deliberately small

We do NOT bind LVGL wholesale. We bind a stable, versioned surface we own
(`manifest.min_api` gates compatibility). ~30 functions total:

- `wada.ui` — the app's root container + widgets: `canvas(w,h)` (draw_rect,
  draw_line, draw_text, draw_circle, fill, blit-from-table), `label`, `chart`
  (the Airtime/Monitor primitive), `button`, `list`. Theme colours provided
  (`wada.ui.colors`), fonts by size class only (12/14/16 → g_font_*).
- `wada.input` — events into `on_input`: touch x/y down/up, key codes,
  trackball dirs. (Snake's swipe steering maps here; the existing
  SnakeGame gesture path shows the shape.)
- `wada.mesh` (READ-ONLY v1) — `contacts()` snapshot (name/type/last_seen/
  rssi), `rx_log()` ring (the RF Monitor feed), `stats()` (airtime used,
  duty budget, tx/rx counters, noise floor, current rssi), `self()` (name,
  lat/lon, battery_mv). No TX of any kind in v1 — mesh safety line.
- `wada.net` — `http_get(url, cb, max_bytes)` async on the existing tile-fetch
  worker; plain HTTP (V4 cannot TLS); response size capped (64 KB default).
- `wada.store` — per-app KV: `get/set(key, value)` under an `app.<id>.` prefix
  in the file-mode prefs. Quota 2 KB/app (respects the SdNvsPrefs blob cap).
- `wada.sys` — `millis()`, `board()` (id, screen w/h, caps: keyboard,
  trackball, gps), `version()`, `toast(msg)`, `tr(s)` (the firmware's own
  translation table, so an app is not stuck in English).
- `wada.timer` — `every(ms)` drives `on_tick` cadence (min 33 ms).

Sandbox env (no `io`, `os`, `require`, `dofile`, `load` of new chunks;
whitelisted stdlib: `math`, `string`, `table`, `pairs/ipairs/select/pcall/
tostring/tonumber`). API-level bounds are the second fence, the curated
catalog is the third.

## App format + store

- On device: `/apps/<id>.lua` + `/apps/<id>.json` (manifest: name, version,
  min_api, boards allow-list, icon glyph, description) on the active storage
  root (SD where adopted, internal FS otherwise) — same resolution as chat
  history.
- Catalog: `https://firmware.wadamesh.com/apps/apps.json` + one `.lua` +
  `.json` per app in `apps/<id>/<version>/`. Served like firmware: VPS rsync,
  immutable per-version paths, mutable index. Device store page = fetch
  index → list → install (download 2 files) → appears in the drawer.
  Uninstall = delete 2 files. Update = version compare against the index.
- The Store page also lists the **built-in removable apps** (Snake, Airtime,
  RF Monitor, later more) with the same install/uninstall verbs — for
  built-ins that just toggles drawer visibility + services (MQTT bridge gets
  this too). One UX for both worlds from day one.

## Budgets + board gates (measure in Phase 0, numbers are targets)

- Flash: Lua core + bindings ≤ **250 KB** (-Os, no coroutines trimmed? keep
  coroutines, trim io/os libs at compile). Gate: `CAP_LUA_APPS` in
  device_caps.h. V4 (77.6%) and T-Deck (81.8%) are fine. **The Pager is the
  risk board** (84.3% now, 87.7% if #198 fonts land) — if Lua + fonts don't
  both fit, the Pager ships CAP_LUA_APPS=0 first and we revisit; measure
  before deciding. P4/Tanmatsu: plenty of room, plain C portability.
- RAM: state base ≤ 40 KB PSRAM, Snake running ≤ 100 KB PSRAM, ZERO new
  internal-DRAM allocations attributable to apps.
- Latency: worst on_tick under budget ≤ 4 ms on the V4 @ 240 MHz (measure
  with the STALL_SCOPE harness already in loop()).

## Conversion inventory

Tier 1 — converts first (proves the API):
- **Snake** (SnakeGame.h — already a self-contained module): canvas + input +
  timer + store (high score). The reference app; native version compiled out
  once parity is confirmed on all boards.
- **Airtime** (ChannelUtil, jacobpretorius): chart + `wada.mesh.stats()`.
  Invite @jacobpretorius to do this port against the SDK — the first
  external Lua app author, with credit.

Tier 2 — converts once the API stabilises:
- **RF Monitor**: rx_log + stats + chart + recolor-safe labels.
- **Telemetry charts** (Sensors view): needs a `wada.mesh.telemetry()` read.
- **LOS analyzer**: http_get (SRTM), contacts picker, canvas plot.

Stays native, with reasons (do not creep):
- **Spectrum** — owns the raw SX1262 with ms-level timing; a Lua radio API is
  a different (dangerous) project.
- **Discover** — transmits protocol frames; revisit only if a consent-gated
  TX API ever exists.
- **Web browser, VNC/Remote/web mirror, Terminal, Files** — infrastructure,
  not apps.
- **MQTT bridge** — background service woven into the main loop; gets the
  catalog uninstall-toggle only.

## Delivery: ALL of it ships in ONE beta — beta_59 (Kaj, 2026-08-04)

The phases below are the build order inside the beta_59 branch of work, not
separate releases. Ship list for beta_59: runtime + host + wada.* v1 + the
Store page + VPS catalog + Snake/Airtime/RF Monitor converted to Lua (seeded
to storage at first boot so they work offline, native versions compiled out
under CAP_LUA_APPS) + built-in hide/show toggles + an SDK docs page. The
Pager go/no-go measurement happens mid-flight and only decides that board's
CAP_LUA_APPS value, not the project.

## Phases (build order within beta_59)

- **Phase 0 — spike + go/no-go (behind an experimental flag, T-Deck only).**
  Vendor Lua 5.4 under `lib/lua/` (PIO) + an IDF component wrapper for
  P4/Tanmatsu (the ed25519/shared-src pattern). PSRAM allocator, sandbox env,
  instruction hook, pcall containment. A 20-line hello app drawing to a
  canvas. Deliverable: measured flash/RAM/latency vs the budgets above.
  STOP here if the Pager numbers say the whole idea needs rethinking.
- **Phase 1 — API v1 + Snake.** The app host (lifecycle, popup-registry row,
  error toasts), wada.ui/input/timer/sys/store, Snake ported + shipped as a
  bundled Lua app (still pre-store: file baked into the FS image or seeded).
  Beta-tested by the community like any feature.
- **Phase 2 — the store.** /apps scan, drawer integration, manifest checks,
  the Store page (browse/install/uninstall/update), VPS catalog + curation
  flow (a PR to a catalog repo dir = submission, we review = curation).
  Built-in-app hide/show toggles land here too.
- **Phase 3 — mesh/net APIs + Tier-1/2 conversions.** wada.mesh + wada.net,
  Airtime and RF Monitor converted, their native versions compiled out where
  CAP_LUA_APPS=1. Flash reclaimed is a bonus, not the goal.
- **Phase 4 — SDK launch.** Docs page on the site (existing docs workflow),
  an annotated example app, an "apps wanted" note in the release body.
  PixPMusic and jacobpretorius get first pings.

Parked for later: multi-file apps, app signing, TX-capable APIs with on-device
consent prompts, a desktop simulator for app development (lv_port_pc + the
same wada.* bindings — nice for contributors, not needed to start).

## Risks, named

- **Pager flash ceiling** — measured in Phase 0, gated per-board; worst case
  the Pager waits.
- **API regret** — once apps exist in the wild, wada.* is an ABI. Mitigation:
  v1 stays tiny, everything else waits until two real apps demand it;
  `min_api` versioning from day one.
- **V4 PSRAM pressure** — app heaps compete with the tile pool; the 256 KB
  cap + tile-pool priority keeps maps working. Watch #192's findings.
- **Store curation load** — catalog PRs need review; that is deliberate
  (curation = the trust model) but it is maintainer time.

## Status log

- 2026-08-04: plan written.
- 2026-08-04: Phase 0 GO — measured on the T-Deck: flash +96 KB (budget 250),
  state+libs 7.5 KB PSRAM / 2 ms, busy-script peak 31 KB, hostile while-true
  contained in 30 ms, zero leak after close. Hook cadence: 100k instructions
  (10k costs +111% on call-heavy code; 100k is ~free and still contains).
  Pager fits (+2.4%, ~87% even with #198 fonts).
- 2026-08-04: Phase 1 DONE — LuaAppHost (AppPage overlay, guarded pcall +
  100k-instruction budget, deferred close from inside callbacks), wada.ui
  (canvas/label/button + theme colors) + input (hw swipes, trackball, touch)
  + timer + sys + store (per-app KV file, root-prefix aware), Snake ported
  to Lua as the embedded+seedable reference app. Verified on the T-Deck:
  open at 19 KB PSRAM, plays via hw-swipe steering, an early bug (pre-layout
  size read) was contained exactly as designed (toast + clean close, 0 leak,
  4 retries, no crash). Host layer costs 13 KB on top of the VM (85.1%%
  T-Deck total). Next: Phase 2 store page + catalog.

**2026-08-04 — Store v2: three tabs + languages-as-files.** The store page is
now segmented: Apps (catalog + sideloads), Built-in (show/hide switches),
Languages (new). Store opens instantly — the card re-listing moved to the net
worker (s_luascan_* flags) and pre-warms at boot. Card text is width-capped
clear of the action button. Languages: gen-lang-files.py exports every i18n
column to apps/lang/<code>.lang + langs.json (serving on the VPS); the device
downloads to <data>/lang/, prefs v48 stores the active code, and at boot the
file loads into PSRAM and overlays TR() (bsearch; file → built-in column →
EN). Users can sideload/edit their own .lang files. Built-in table stays as
fallback this beta; dropping its columns for the flash win is the follow-up
once files are field-proven.

- 2026-08-22: **wada.sys.gps() widened + wada.sys.compass() + GPS Compass
  app.** gps() now carries `alt` (m) everywhere and `speed_kmh`/`course` on
  boards that build their GPS on `src/helpers/WadaNmeaLocationProvider.h`
  (a Wadamesh-owned copy of the core provider exposing the RMC motion fields
  it keeps private; M9 first, flag `HAS_GPS_MOTION`), and it returns nil while
  the user has GPS switched off. compass() is on a new HARDWARE gate
  `CAP_COMPASS` (the M9's QMC6309, driver `variants/thinknode_m9/M9Compass.*`,
  exposed in `caps().compass`) and hands out the raw field vector in Gauss —
  calibration, axis mapping and the heading maths live in the app so they can
  be adjusted per user without a firmware cut. `deploy/apps/gpscompass/1.0`
  is that app: rotating rose, live fix readout, bearing/range to a contact,
  C to calibrate (a 3D sphere fit that also calibrates the accelerometer's
  zero-g offset from the same sweep), tilt compensation off the M9's QMI8658,
  GPS-course fallback on every other board. It carries a real WMM2025
  declination model (`scripts/wmm/`) so the dial and the bearings share a
  north: the magnetometer measures magnetic north, every bearing computed from
  coordinates is true, and drawing one against the other put every waypoint
  ~22° out. Headings and bearings are labelled `T` or an amber `M` accordingly.
  Verified on the host harness in `scripts/lua-harness` (same vendored Lua,
  same LUA_32BITS numeric model): calibration recovers a simulated hard-iron
  bias exactly, heading error 0° at six test angles, bearings checked against
  absolute compass directions, declination within 0.0002° of NOAA, worst tick
  ≈12k of the 100k instruction budget. Hardware validation (axis orientation,
  real bias magnitude) is in M9_PORT.md. Not seeded into lua_builtin.h on
  purpose: CAP_BUILTIN_LUA_APPS also hides the Store > Apps tab.

- 2026-08-22: **manifest `icon` implemented.** The drawer gave every Lua app
  the same generic glyph while `LUA_APPS.md` had promised manifests an icon
  field since the plan was written. `icon` is now read from `<id>.json` and
  mapped to a glyph by NAME (`gps`, `radio`, `chart`, `game`, ...) — names,
  not codepoints, so a store submission stays reviewable and an app can never
  ship a missing-glyph box; anything unrecognised falls back to the generic
  symbol. `deploy/site/sdk.html`'s manifest table was corrected at the same
  time: it documented `version` / `min_api` / `description` / `boards`, none
  of which the device has ever parsed.
