# Heltec ThinkNode M9 — wadamesh port

Fresh port against `main`, built alongside (not replacing) the Heltec V4 TFT and
LilyGo T-Deck envs. Env name: `ThinkNode_M9_companion_radio_touch`.

## Hardware summary

ESP32-S3R8, LR1110 radio, 2.4" 240x320 **ST7789** TFT (manufacturer-confirmed —
an earlier attempt assumed ILI9341 based on a Meshtastic boot log that, per the
manufacturer, was wrong), full QWERTY keyboard + d-pad + dedicated function
buttons all on one I2C keyboard controller, no touchscreen. CC1167Q GPS, QMI8658
IMU, QMC6309 compass, PCF8563 RTC, 8 MB PSRAM, 16 MB flash.

## Why this needed its own radio_init(), not std_init()

`CustomLR1110` (the core's LR1110 wrapper) has **no `std_init()`** — unlike
`CustomSX1262`/`SX1268`/`LLCC68`. Every existing LR1110 board in MeshCore
(`thinknode_m3`, `minewsemi_me25ls01`) drives the radio by hand: `SPI.setPins()`
→ `SPI.begin()` →
`radio.begin(freq, bw, sf, cr, sync_word, power, preamble, tcxo)` →
`setCRC()`/`explicitHeader()` → optional RF-switch table / boosted-gain.
`variants/thinknode_m9/target.cpp::radio_init()` follows that pattern exactly
(see MeshCore `variants/thinknode_m3/target.cpp` as the reference it's modelled
on).

The radio, the ST7789 panel, and the microSD slot all share one physical SPI
bus. Neither `LILYGO_TDECK` nor `HELTEC_LORA_V4_TFT` is defined for this board,
so `ST7789LCDDisplay` takes its default constructor branch
(`display(&SPI, ...)`) — meaning the display uses the **same** global `SPI`
instance as the radio, matching how the LR1110 reference boards do it (no
separate `HSPI`/local `SPIClass` the way the T-Deck/Heltec-TFT branch does).

## Verified pin map

(Originally cross-referenced from a Meshtastic `thinknode_m9` boot log against
the V1.0 schematic during early bring-up; several boot-log-derived entries
later turned out wrong on direct schematic inspection — microSD interface type
and the battery divider ratio both being corrected below are examples. Treat
this table as schematic-verified, not Meshtastic-derived, going forward.)

| Net                          | GPIO            | Notes                                                                                                                                                                                                                                                         |
| ---------------------------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| LR1110 NSS                   | 39              |                                                                                                                                                                                                                                                               |
| LR1110 IRQ (DIO1)            | 42              | also `getIRQGpio()` / sleep wake source                                                                                                                                                                                                                       |
| LR1110 RESET                 | 45              |                                                                                                                                                                                                                                                               |
| LR1110 BUSY                  | 41              |                                                                                                                                                                                                                                                               |
| SPI SCLK                     | 40              | shared: radio + LCD + microSD                                                                                                                                                                                                                                 |
| SPI MISO                     | 38              |                                                                                                                                                                                                                                                               |
| SPI MOSI                     | 47              |                                                                                                                                                                                                                                                               |
| LCD RESET                    | 14              |                                                                                                                                                                                                                                                               |
| LCD DC (RS)                  | 15              | repurposed XTAL_32K_P                                                                                                                                                                                                                                         |
| LCD CS                       | 16              | repurposed XTAL_32K_N                                                                                                                                                                                                                                         |
| Backlight (BL_EN)            | 17              | PNP transistor, **active-LOW**                                                                                                                                                                                                                                |
| Peripheral power rail        | 18              | P-MOS, **active-LOW** (LCD/GPS/sensors)                                                                                                                                                                                                                       |
| microSD                      | 48 (CS)         | Shared LoRa SPI bus (Arduino SD, same as T-Deck) — CS is GPIO48, schematic net `SPICLK_N`. NOT one of the GPIO33-37 octal-PSRAM lines; SPICLK_N/GPIO48 (and SPICLK_P/GPIO47) are only reserved on the R8V/R16V 1.8V-differential-clock variants — this board is a plain R8 (see Hardware summary), which uses the ordinary single-ended SPICLK instead, so GPIO48 is genuinely free. The earlier "CS=36 is octal-PSRAM-reserved" claim conflated physical package pin 36 with GPIO36 — they are not the same pin. Confirmed working on hardware. |
| Peripheral I2C SDA/SCL       | 7 / 6           | RTC PCF8563 0x51, IMU 0x6b, compass 0x7c. The RTC is driven by `src/helpers/HardwareRtcClock.*`, NOT by the core's `AutoDiscoverRTCClock` probe (issue #383): that path read the calendar registers without ever consulting the chip's VL bit, so after a hard shutdown the stale contents came back as real wall time and the clock looked frozen at the moment the slider was pulled. The adapter refuses an integrity-lost read, clears a latched STOP bit, and reads back every write                                                                                                                                                                                                                              |
| Keyboard I2C SDA/SCL         | 20 / 21         | controller @ 0x6c, **own bus** (Wire1)                                                                                                                                                                                                                        |
| Battery ADC                  | 13              | **ADC2**_CH2 (S3: ADC1=GPIO1-10, ADC2=GPIO11-20 — earlier "ADC1_CH2" label was wrong; ADC2 is Wi-Fi-arbitrated, see getBattMilliVolts's per-sample filter), **2:1 divider** (manufacturer-confirmed)                                                                                                                                                                                                            |
| GPS RX/TX                    | 2 / 3           | CC1167Q, UART1                                                                                                                                                                                                                                                |
| GPS EN / ON_OFF / RST / 1PPS | 11 / 10 / 5 / 4 | EN+RST wired, **both active-LOW** (confirmed on schematic: same P-MOS circuit as the peripheral power rail for EN; GPS_RST1 -> R46 -> NPN Q16 base, GPIO HIGH turns Q16 on and pulls the module's reset line to GND, so HIGH asserts reset / LOW releases it — inverted vs. the library defaults, both now overridden in platformio.ini). ON_OFF + 1PPS unused |
| Buzzer                       | 9               | wired — simple GPIO piezo (`THINKNODE_M9_BUZZER_PIN`), Arduino `tone()`/`noTone()`, shares the Heltec V4 buzzer code path |
| ESP_WAKEUP (from KB MCU)     | 12              | not wired; behaviour undocumented                                                                                                                                                                                                                             |
| KEY_LED                      | 46              | not wired                                                                                                                                                                                                                                                     |
| User button (BOOT)           | —               | **does not exist** (Deferred #6, schematic-confirmed): only a power-cut slider and a reset button, neither a GPIO. PIN_USER_BTN removed from the env — the old GPIO0 entry here contradicted Deferred #6 |                                                                                                                                                                                                                       |

This turn's corrections vs. the earlier (incorrect) attempt:

- Display is **ST7789**, not ILI9341.
- LR1110 clock: **TCXO mode at 3.3 V** (bring-up build #2). Empirically settled
  on the tester's preproduction unit: Meshtastic's variant logs "using DIO3 as
  TCXO reference voltage at 3.300000 V" + "LR1110 init result 0" on this exact
  device, and our build #1 with tcxo=0 (the earlier active-oscillator theory)
  failed radio init with -707: the chip boots on internal RC (SPI alive) but the
  first command needing the 32 MHz clock is rejected. The schematic's "VTCXO
  rail" powering Y1 is evidently the LR1110's own TCXO-supply output.
- Battery divider is **2:1**, not 1:1 — equal-value resistors still halve the
  voltage regardless of their absolute value; `getBattMilliVolts()`'s `2 *`
  multiplier was wrongly removed on the mistaken belief that "equal resistors"
  meant "no division." Restored.
- GPS EN and RESET are both **active-LOW**, not the library's active-HIGH
  defaults — confirmed on schematic (EN is the same P-MOS circuit as the
  peripheral power rail; RESET is GPS_RST1 -> R46 -> NPN Q16, where GPIO HIGH
  asserts reset). Both `PIN_GPS_EN_ACTIVE` and `PIN_GPS_RESET_ACTIVE` are now
  overridden in `platformio.ini`. Fixed a total loss of GPS fix (module was
  held disabled/in-reset on every boot).
- microSD CS is **GPIO48**, not "deferred pending SDMMC pins" — this board
  never needed SD_MMC at all; the original SPI-CS approach was correct, the
  pin number was just wrong (see the pin table above and "Bonus schematic
  finds" below). Confirmed working on hardware.

## Keyboard (confirmed on hardware — `M9Keyboard.h`)

Write reg 0x01 then read one byte (single latched key — see the
register-addressed-slave section below; the old "one raw byte per read" note
here was the exact protocol misunderstanding that made builds 1-7 see no
keys). The controller resolves shift/symbol layers itself:

| Key                  | Raw  | Key          | Raw  |
| -------------------- | ---- | ------------ | ---- |
| left                 | 0xB4 | left_message | 0x81 |
| up                   | 0xB5 | home         | 0x82 |
| down                 | 0xB6 | sub_message  | 0x83 |
| right                | 0xB7 | sub_map      | 0x84 |
| d-pad centre / enter | 0x0D | map          | 0x85 |
| del (backspace)      | 0x08 | hw_back      | 0x86 |
| mic (triangle)       | 0x88 | ctrl         | 0x90 |

Backspace (0x08) and Enter (0x0D) happen to match the values
`UITask.cpp::handleHwKey()` already special-cases for the T-Deck, so typing,
backspace, and Enter-to-send all work as-is. The d-pad/function-key bytes
(0x81–0x90, 0xB4–0xB7) currently fall through unhandled when no text field is
focused — see "Deferred" below.

## What's wired in this patch

- `boards/thinknode_m9.json` — ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
- `variants/thinknode_m9/M9Board.{h,cpp}` — board class: both active-low power
  rails (periph rail + backlight) via `RefCountedDigitalPin`, 2:1 battery
  divider. Deep sleep is TIMER-ONLY: the original "wake on LR1110 IRQ
  (GPIO42)" was electrically impossible (S3 RTC pads are GPIO0-21 — every
  rtc_gpio_*/ext1 call on 42/39 failed with ESP_ERR_INVALID_ARG, unchecked),
  so those calls were removed. The only RTC-capable wake candidate is
  ESP_WAKEUP GPIO12 from the keyboard MCU — needs hardware characterization
  (Deferred #6).
- `variants/thinknode_m9/target.{h,cpp}` — manual LR1110 `radio_init()`,
  GPS/RTC/sensor wiring, ST7789 display instantiation, `m9SharedSPI()` (mirrors
  the T-Deck's `tdeckSharedSPI()` — used by the SD mount code below).
- `variants/thinknode_m9/M9Keyboard.{h,cpp}` — Wire1 keyboard driver, ring
  buffer, hardware-confirmed keycode sentinels (`M9_KEY_*`).
- `variants/thinknode_m9/partitions_m9_touch.csv` — 16 MB, dual A/B OTA slots
  (copy of the T-Deck's layout; same flash size).
- `platformio.ini` — new `[env:ThinkNode_M9_companion_radio_touch]`.
- `src/ui-touch/device_caps.h` — new `HAS_THINKNODE_M9` capability block;
  `CAP_KEYPAD_NAV`/`CAP_SD`/`CAP_FILESYSTEM` all `1` (see below).
- `src/ui-touch/UITask.cpp`:
  - `HAS_M9_KEYBOARD` added alongside `HAS_TDECK_KEYBOARD` at every _generic_
    "is there a physical keyboard" gate (composer auto-focus, Enter-sends
    toggle, spacebar-lock, secondary-keyboard cycling hint, etc. — 15 sites),
    plus its own poll/drain branch in the per-tick loop (polls `Wire1` directly
    from the UI thread — no core-0 hand-off needed, since this bus has no other
    device on it, unlike the T-Deck's touch+keyboard-shared bus).
  - **microSD**, extended from the T-Deck's existing pattern: the include
    block, `fmIsSd()`, the mount/format helpers (`fmSdTryMount`/`fmSdDoFormat`/
    etc.), and the Files-manager settings row are all
    `#if defined(HAS_TDECK_GT911) || defined(HAS_THINKNODE_M9)`, swapping
    `tdeckSharedSPI()` for `m9SharedSPI()` where the bus accessor is used.
    `CAP_SD` is `1`. CS is GPIO48 (see pin table). Confirmed mounting,
    browsing, and reading on hardware. NOT yet extended: the wallpaper picker
    (in progress — its own implementation is fully board-agnostic already,
    just needs its enclosing guard split from the genuinely-T-Deck-only
    notification-sound chooser it currently shares a block with) and the
    notification-sound chooser itself (needs `tdeckPlayNotifySlot()`, T-Deck's
    I2S amp — M9 has a buzzer instead, real separate task).
  - **D-pad keypad navigation**, built on the SAME generic engine Tanmatsu and
    the T-Deck already share (navFifo, `navMoveDir`/`navSwitchTab`/
    `navPushTap`, the focus group, the secondary LVGL `KEYPAD` indev) —
    `CAP_KEYPAD_NAV` now also covers `HAS_THINKNODE_M9`, the secondary-indev
    registration and `s_kbd_nav`-always-on logic were broadened from
    `CAP_TRACKBALL`-only, and a new `#elif defined(HAS_M9_KEYBOARD)` block in
    `handleHwKey()` (parallel to the T-Deck's WASDZ-letter block) maps the M9's
    _fixed_ hardware d-pad/function-key bytes straight to those same primitives
    instead of going through the programmable letter table. UP/DOWN/LEFT/RIGHT
    move focus or pan the tab bar, the d-pad centre/Enter selects, the dedicated
    HW-back key backs out, and HOME/MAP/MESSAGE jump to those tabs. MIC and CTRL
    have no action bound yet.
- **Keypad nav corrected for cases the initial patch missed**: the wizard was
  unreachable by d-pad at all (handleHwKey()'s touch-only setup-root swallow ran
  before M9's key handling); Enter on a focused button after leaving a field via
  arrows silently no-op'd (stale on-screen-keyboard binding used instead of the
  live group focus); there was no way to leave an edit field via the d-pad; HOME
  didn't close overlays before jumping tabs; and there was no wake-from-idle
  path (M9 has no touch to wake it the way T-Deck/Heltec V4 do). All fixed in
  UITask.cpp — see git history for specifics.
- **Backlight control** (`touchScreenBacklight()`) had no M9 branch at all — the
  idle-off state tracked correctly but the physical backlight never dimmed.
  GPIO17 (`BL_EN`, PNP transistor) supports real PWM dimming despite being a
  simple digital-looking enable line — confirmed on hardware via LEDC, with
  **inverted duty** (PNP: lower duty on the base = more conduction = brighter).
  5 kHz confirmed clean. An earlier, incorrect on/off-only implementation
  (`m9SetBacklight()`, ref-counted `RefCountedDigitalPin`) was replaced with
  `applyBrightness()`/LEDC, matching the existing `HAS_BACKLIGHT_PWM` pattern.
- **Buzzer** (GPIO9) wired via the existing `HELTEC_V4_BUZZER_PIN` code path,
  widened to also accept `THINKNODE_M9_BUZZER_PIN` (same mechanism — Arduino
  `tone()`/`noTone()`, no separate enable line; `BUZZER_EN` is just GPIO9's
  net name, not a second pin). Confirmed working via the Settings > Sound
  toggle previews.
- **Commander (Home tab) landscape layout** — the TX/RX chart width and the
  5-button right-hand column's height-per-button math (Advert/Terminal/Files/
  Apps/Control) both had sizing gates that didn't include `HAS_THINKNODE_M9`,
  so the chart drew full-width over the buttons and the button-count math
  assumed 4 slots when M9 (like T-Deck) actually renders 5 — pushing the last
  button off the bottom of the screen. Both gates fixed.
- **Home-tab drawer toggle self-conflict**: `M9_KEY_HOME`'s own "close
  everything on top" dismiss loop was closing the drawer itself (once added to
  the popup registry), then immediately re-reading the now-mutated
  `s_home_drawer_mode` flag and reopening it in the same keypress. Fixed by
  snapshotting the flag before the dismiss loop runs. Also fixed: HOME
  stopping early after dismissing an overlay reached via the Settings tab
  (rather than the Home-tab drawer) instead of still jumping to Home
  afterward.
- **Scroll-into-view during keypad nav** used `lv_obj_scroll_to_view()`
  (checks only the immediate parent) instead of
  `lv_obj_scroll_to_view_recursive()` (walks every ancestor) — settings pages
  using the "grouped card" layout (`createSettingsModal`) nest controls 3+
  levels below the actual scrollable container, so focus moving off-screen
  there never scrolled. Fixed; shared code, benefits every board.
- **Textarea focus highlight** used the plain reverse-video fill instead of
  the bright outline+glow switches/sliders get, making it very hard to see
  which field was focused before entering edit mode. Added `lv_textarea_class`
  to that style branch. Shared code.
- **Dropdown-list keypad navigation** (`navOpenDropdown()`) was never wired
  into the arrow-key dispatch on any board (Tanmatsu's `navArrowAction`, T-Deck's
  CAP_TRACKBALL block, or M9's `m9HandleArrowKey`/`m9HandleNavKey`) — UP/DOWN/
  Enter/Back always fell through to page-level `navMoveDir`/popup-dismiss
  instead of moving the dropdown's own highlighted option. Added the missing
  checks to M9's handlers specifically (mirroring Tanmatsu's already-correct
  pattern for Enter/Back, which Tanmatsu never actually needed for UP/DOWN
  since it wasn't gapped there the same way). **Still not fully working on
  hardware** — traced the entire mechanism (group-focus detection, FIFO push,
  indev group assignment, LVGL's own dropdown `LV_EVENT_KEY` handler, loop
  ordering relative to `lv_timer_handler()`) and confirmed our dispatch code
  is correct up to and including the `navPushTap(LV_KEY_DOWN)` call itself —
  the dropdown still closes and focus moves to the next page element. Root
  cause not yet found; see Deferred list.

## Audit pass (2026-08-19) — Back key restored + feature/perf/data fixes

A full audit traced every M9 input path and closed the following (all
compile-verified; on-device validation still wanted for the UX items):

- **HW Back (0x86) restored in nav mode.** Commit `0ea242c` accidentally
  REPLACED `case M9_KEY_HW_BACK:` in `m9HandleNavKey` with the new
  `case M9_KEY_ENTER_LONG:` — since then Back only worked inside text-edit
  mode. Restored with a fuller ladder than the ad98f66 original: open
  dropdown list (ESC) → popup registry (incl. contacts select-mode) →
  full-screen app page (`s_apppage_close`: Lua apps, Snake, Web) → open chat →
  LV_KEY_ESC. This plus the registry-gate fixes below fully explains the old
  "some modals close via Back, some don't".
- **Lua-app hard trap fixed.** `isDismissKey()` had no M9 arm (returned false
  for everything), so any Lua app consumed Back/Home with no touch fallback —
  the only escape was the power slider. M9 arm added (Back + Home are always
  firmware keys); HOME also closes app pages now. Keys are additionally kept
  from apps while the screen is locked/dark, so `ENTER_LONG` (the board's only
  unlock) can't be eaten by an app after auto-lock.
- **Popup-registry gates widened to M9**: Files-manager overlays (image
  viewer, editor, prompts, actions, format overlay), Terminal command picker,
  fullscreen Files/Terminal view, wallpaper picker, and `drawerPopupOpen`'s
  fullscreen-view term. These compiled and ran on M9 but were invisible to
  Back/Home and `anyPopupOpen()`.
- **Dropdown keypad nav fixed**: no `navOpenDropdown()` capture existed in any
  M9 key path (the doc's earlier claim it was added never matched a commit) —
  arrows now FIFO LV_KEY_UP/DOWN into the open list instead of navMoveDir
  defocusing (and thereby closing) it; Back sends ESC to the list first.
- **Textarea 3-4-press focus escape**: edit-mode LEFT/RIGHT now fall through
  to navMoveDir when the caret is already at the text boundary.
- **Modal "breaks out" mitigation**: the M9 drain now runs `navMaybeRebuild()`
  BEFORE dispatching keys (they used to dispatch against a one-tick-stale
  focus mirror), and `closeSettingsModal()` gained the idiomatic
  `navDetachBeforeTreeMutation()`/`navMarkDirty()` pair.
- **Lua apps get the d-pad**: arrows → `luaAppSteer` (swipe events, T-Deck
  trackball parity), d-pad centre → synthetic centre tap (`luaAppPress`, new)
  + `ev.key=="enter"` (sendKey now maps `'\r'`). Snake and other store apps
  are playable, including "tap to retry".
- **New key bindings**: GPS_LONG 0x87 → `toggleGPS()` + alert (matches the CC
  chip; NB if hardware shows the latched slot also emitting the 0x84 tap
  first, the Advert page will open too — needs on-device check); CTRL 0x90 →
  Control Center. MIC 0x88 and 0x89 deliberately left unbound (documented in
  M9Keyboard.h).
- **Keyboard backlight wired**: `m9KeyboardSetBacklight()` (controller reg
  0x02) is now driven from the drain-loop tick — off/on/auto via the CC
  "Keyboard" chip, write-on-change only. The old drain comment claiming "no
  backlight control exists" was wrong. (Audit pass 2 hardened this: the setter
  reports success and the cache only latches written-and-ACKed duties — see
  below.)
- **Message flash/wake wired**: "Flash on new message" was a dead switch on M9
  (producer/consumer were T-Deck-only). M9 now wakes (or lock-reveals) on
  message and pulses the keyboard light; the 10 s notify re-dim applies.
- **Terminal/editor Enter**: the Enter-submit/newline gate excluded M9 —
  widened, so Enter runs the terminal command / inserts editor newlines.
- **"Store data on SD" honored**: the boot data-store path in main.cpp
  excluded M9, so the (shown, persisted) toggle silently did nothing —
  extended, with `m9SharedSPI()` arms in both selector chains, plus the truth
  label, recovery-copy button and migration machinery in Settings.
- **SD-backed chat history**: `uiDataFsReady()` had no M9 arm (fell into the
  V4 no-SD SPIFFS branch) — added the T-Deck-shaped SD arm plus the SD-history
  hardening gates (write-failure tell, remount re-flush).
- **Threads-index write made atomic (shared fix)**: `saveThreadsToStorage()`
  rewrote in place ("w"); a power-cut mid-write got the file quarantine-deleted
  at boot (chat list/unread/DM entries lost — the bulk of Deferred #9's
  symptom). Now tmp+rename via `uiDataReplaceFile`, orphan tmp swept at load.
- **Power menu**: the "Power off" row is hidden on M9 — it armed ext0 wake on
  a user button this board doesn't have, leaving the device unrecoverable
  until a slider cycle. The slider IS the power-off. Reboot/Download/Cancel
  remain. `PIN_USER_BTN` removed from the env (no button exists; GPIO0 was a
  floating strapping pin being polled by the screen-lock branch).
- **"Save update bin to SD"** (About) widened to M9 (OTA_BIN_NAME arm already
  existed).
- **Battery reads**: GPIO13 is ADC2 (Wi-Fi-arbitrated) — `getBattMilliVolts()`
  now discards ADC2-blocked samples (< 1250 mV at the pin) per-sample and
  holds the last good reading. The battery log now follows the resolved
  ui-data backend instead of bare card presence (Deferred #8's battLogOnSd fix
  landed earlier in 4846d4f; this closes the "any card with /meshcomod hijacks
  the log" residue).
- **Perf**: keyboard I2C poll throttled to 15 ms (was a blocking ~0.5 ms
  100 kHz transaction EVERY free-running loop iteration — hundreds/s), with a
  bounded 3-read drain per poll so a second key struck inside the window isn't
  lost; SD CS (GPIO48) parked HIGH at boot (was floating across 80 MHz shared-
  bus traffic until the first lazy mount); `RADIOLIB_DEBUG_BASIC` bring-up
  flag dropped per its own note.

**Follow-up fixes from the third on-device test round (same day) — "panning
doesn't reload tiles":** a multi-agent trace confirmed four contributors and
ruled out the tile-slot pool, the tab-bar removal, and the key-drain timing:

- **Tile cache now prefers the BUILT-IN 16 GB microSD** (every M9 has one).
  It used to cache into the 4.75 MB "tiles" partition, which fills after a few
  panned screens and has NO eviction — every later download then failed
  forever. Cache goes to SD /tiles (Launcher-T-Deck layout, merges with packs);
  the partition remains the fallback if the card wedges.
- **Auto-follow paused during pan mode**: mapAutoFollowTick recenters on the
  CENTER-vs-fix delta, so the pan itself tripped it (the old mapNudge comment
  claiming "recenters on the next GPS move" was wrong) — with follow on, every
  nudge snapped back within one 250 ms tick. Follow resumes when pan exits.
- **Two self-heal repaints (M9-gated)**: (1) the fetch worker's
  already-on-disk skip now arms the rate-capped repaint for visible-zoom tiles
  (a tile whose read was transiently blocked stayed blank "one pan behind");
  (2) clearing an SD fail-note (which blanks ALL tile reads for up to ~5 s per
  stamp) now arms the repaint too when the last render had gaps.
- **Offline UX**: entering pan mode and hitting uncached areas with Wi-Fi off
  shows a one-per-session alert — the fetch queue silently no-ops offline and
  only a small corner label said why.
- Still-relevant user guidance: a PNG tile pack on the card (/maps/osm or
  /tiles) is only read when Map options → "Tiles from SD card" is ON and the
  style is OSM; the toggle re-points and re-renders immediately.

**Follow-up fixes from the second on-device test round (same day):**

- **Back inside apps looked dead**: apps launch from the app DRAWER, which
  stays open BENEATH the full-screen app page by design (closing the app
  returns you to it) — and the drawer is a PF_COUNT popup-registry row, so the
  restored Back ladder's `anyPopupOpen()` rung dismissed the invisible drawer
  under the app instead of the app itself (Home worked because its handler
  closes the app page first). The Back ladder now closes an open app page
  before the registry dismiss, EXCEPT when the Control Center or power menu is
  open (the only popups a key can open OVER an app page on this board — CTRL
  falls through to openControlCenter() in display-only apps).
- **Map pan mode added** (see the resolved keypad-nav item above): Map key on
  the Map tab toggles arrows between focus-nav and mapNudge() panning.
- The Chats/Map jump keys now close an open app page before switching tabs —
  the jump used to land invisibly beneath the still-covering page.

**Follow-up fixes from the first on-device test round (same day):**

- **D-pad in display-only Lua apps (Airtime, RF Monitor)**: these apps declare
  no `on_input`, but `luaAppKey()` consumed every key anyway — the d-pad was
  dead inside them. Keys are now forwarded to a Lua app only when it actually
  declares `on_input` (new `luaAppHasOnInput()`); otherwise the d-pad keeps its
  native meaning: arrows move focus between the app page's own LVGL widgets
  (Airtime's Reset button), Enter clicks them, and when focus has nowhere to go
  UP/DOWN page-scroll the app body (new `luaAppScroll()` — RF Monitor's feed).
- **Bottom tab bar removed on M9** (user request): it was tap-only chrome —
  navMaybeRebuild deliberately never adds it to the focus group, so on a
  touchless board it was uncontrollable dead space. `TABBAR_H` is now 0 for
  M9 and the btnmatrix is hidden; every tab remains reachable via the dedicated
  HOME/MESSAGE/MAP keys and the app drawer's Chats/Contacts/Map/Settings tiles.
  Content gains the 30 px row. The M9 draws no notification badges there;
  unread messages and newly discovered contacts instead blink small accent-colour
  envelope/person glyphs in the former Chats and Contacts nav slots.
- **Spectrum sweep crawl fixed (LR1110-specific)**: `LR11x0::getRSSI(false)` is
  not the SX126x's cheap register read — it internally re-arms RX and drops to
  standby around EVERY call, so the 6-read peak-hold per bin paid ~960 extra
  arm/disarm cycles per 160-bin sweep (each a multi-command SPI exchange with
  BUSY waits) and sampled the unsettled post-arm RSSI default rather than live
  channel power. The M9 sweep now holds RX through the dwell
  (`getRSSI(false, /*skipReceive=*/true)` + one explicit `standby()` per bin)
  and skips the pointless image recalibration the 24 MHz wrap-around jump
  triggered every sweep (`setFrequency(f, /*skipCalibration=*/true)`).

## Audit pass 2 (2026-08-20) — second full sweep after the 08-19 fixes

A second 7-dimension multi-agent audit (each dimension adversarially verified:
30 findings, 29 confirmed, 1 refuted) over the tree WITH the 08-19 pass
applied. All 29 fixed, compile-verified. On-device retest list at the end.

**Key-trap / soft-lock class (all in UITask.cpp's M9 handlers):**

- **SUB_MAP over a display-only Lua app orphaned the app** (the worst find):
  0x84 opened the Advert page without closing the app page, the Advert page's
  close stole the single `s_apppage_close` slot, and closing it nulled the
  hook — the Lua app underneath became unreachable by ANY key (reboot/slider
  only). SUB_MAP and SUB_MESSAGE now close an open app page first, like the
  LEFT_MESSAGE/MAP jump keys always did.
- **Back-ladder z-order redesign.** The old ladder's app-page rung assumed
  "CC and power menu are the only popups that can stack over an app page" —
  false for the Lua send-permission confirm (which the app itself raises) and
  the registry walk closed the FIRST open row in declaration order, so Back
  with the CC open over e.g. a Files rename prompt silently discarded the
  prompt UNDERNEATH. New invariant: power menu and CC are always-frontmost
  rungs (power before CC — CTRL peels an open power menu before opening the
  CC so it holds in both stacking directions), the app-page rung defers to an
  open confirm modal, and SUB_MESSAGE/SUB_MAP run HOME's bounded
  dismiss-everything loop before opening their own overlay (refusing to
  stack over a null-close progress blocker) so nothing is ever left open
  beneath. HOME peels power/CC/confirm the same
  way. Arrows/Enter are also no longer forwarded to an `on_input` Lua app
  while a confirm modal is up — the send-permission dialog used to be
  UNANSWERABLE on M9 (keys went to the app; Back/Home killed the app under
  the dialog).
- **Map pan-mode flag could go stale**: no popup guard (arrows panned the map
  hidden under the CC) and no tab-jump clear (HOME with pan on left the flag
  armed — the next Back anywhere was eaten by "Map pan off", and re-entering
  the map via the drawer tile arrived still panning). Pan now self-clears on
  any popup or tab leave; a stale flag is cleared silently, the toast only
  fires for a genuine pan exit.
- **Null-closer "blocker" popup rows now actually block**:
  `popupRegistryDismissTop()` used to SKIP rows without a closer and dismiss
  whatever sat beneath the progress overlay (Back during bulk-delete exited
  select mode mid-operation; Back during SD format tore down the fullscreen
  Files view the format returns to). The walker now stops at the first open
  row; null-close rows refuse the dismiss. Shared fix (T-Deck had the same
  hole). Behavior note: a tab jump during a progress overlay now leaves the
  overlay up — intended blocker semantics.

**Gate parity (compiled on M9 but wired only for the T-Deck):**

- Terminal RX mirror — incoming mesh traffic never appeared in the M9's
  terminal chat mode (TX echo only). Gate widened. (TLORA_PAGER also compiles
  the terminal and still lacks the mirror — upstream follow-up, not M9's.)
- Accent + @-mention pickers popped up while typing but were UNPICKABLE
  (touch-only cells; the key-nav selection machinery is pager-only) — pure
  dead chrome on a touchless board, and the mention box could linger after
  leaving edit mode. Deliberate call: SUPPRESSED on M9 (auto-popups gated
  off, dead settings row compiled out, `mentionBoxHide()` added to Back's
  edit-mode branch). Porting the pager's key-nav selection is possible future
  work if accents are ever wanted on this keyboard.
- Fullscreen Terminal/Files title (status-bar borrow), wallpaper-set caption
  refresh in the Device modal, and the map storage-error message (M9 now gets
  the SD guidance, not "reflash the tiles partition") — all widened.
- Dead `HAS_M9_KEYBOARD` alternative removed from a CAP_TRACKBALL-only
  settings block (M9 nav is force-on at boot and must never grow an
  off-switch).

**Keyboard backlight cache**: `static uint8_t s_kb_bl_last = 0xFF` was meant
as a never-written sentinel — but M9 duties are only 0/255 and 255 == 0xFF,
so mode "On" restored from prefs skipped the first write EVERY BOOT (stuck on
the controller's keypress-auto default until the first dim/wake cycle). The
cache also latched duties whose I2C writes were dropped (controller still
booting, NACK). `m9KeyboardSetBacklight()` now returns success (false on no
bus / NACK), the cache is an int(-1) updated only on success — dropped writes
retry until ACKed, at a 250 ms cadence so a found-then-wedged bus never pays
an I2C transaction per free-running loop pass.

**Board API (latent — nothing calls powerOff/enterDeepSleep on M9 today, but
it shipped broken):** `enterDeepSleep()`'s single rail release left GPIO18
driven ON (refcount was 2: board + display — `display.turnOff()` now drops
the display's claim first), its backlight `digitalWrite` was inert (LEDC ch7
owned the pad since UI boot — `pinMode()` re-route first, V4-R8 idiom), and
no hold meant even correct levels were lost when pads tristate in sleep
(`rtc_gpio_hold_en` on 17/18, `gpio_hold_en` + `gpio_deep_sleep_hold_en` for
non-RTC NSS GPIO39; unconditional hold release at the top of `begin()` —
RTC-domain holds survive the RST button). It also now mirrors the base-class
sequence it was hiding: radio `powerOff()` + NSS parked HIGH, GPS provider
stop, `Serial.flush()`, stale wake sources cleared — previously a "powered
off" M9 kept the LR1110 in RX (mA-scale drain with no wake source that could
ever use it) until the slider cut the battery.

**Radio/build hardening:** the TCXO fallback in `radio_init()` still encoded
the DISPROVEN tcxo=0 theory with a comment saying it "must stay 0" (a trap:
the build only worked because platformio.ini defines the macro) — fallback is
now 3.3f with the disproof recorded. `patch_radiolib_lr11x0.py` fail-closes:
pattern drift with RadioLib present is a hard build error, and a pre-link
check verifies the patch marker after libdeps exist (a fresh checkout's first
`pio run` fails at link with a "patched — re-run" message rather than
shipping an unpatched -706 binary; the second run builds clean). M9 env
gained `ENV_SKIP_GPS_DETECT=1` (the cold-booting CC1167Q could miss the 1 s
NMEA probe → `gps_detected` false all session → GPS toggle + GPS_LONG key
dead; every sibling soldered-GPS env already set it) and `CORE_DEBUG_LEVEL=0`
(ARDUHAL [E] spam on UART0, which doubles as the companion frame stream).
Partition-CSV headroom prose refreshed (~2.99 MB firmware, ~0.88 MB headroom
per slot). Refuted by the verifier, deliberately NOT applied:
`RADIOLIB_EXCLUDE_SX126X`.

**On-device retest list for this pass:** Airtime/RF-Monitor + SUB_MAP then
Back (no orphan); CTRL over a Files rename prompt then Back (CC closes,
prompt survives); send-permission dialog from a store app (arrows/Enter
answer it); map pan → HOME → Back elsewhere (no eaten press); "Keyboard
light: On" applied immediately at boot; GPS working from a cold boot without
toggling; terminal chat shows the peer's replies; Back during an SD format
does nothing.

Still open / needs hardware (designed but deliberately NOT coded blind):
ST7789 SLPIN/DISPOFF panel sleep on screen-off (shared-bus variant of the
T-Deck's anti-burn-in path — a wrong sequence would look like a dead display);
raising the SD operating clock above 4 MHz; deferring hist-flush during active
input; Wire1 at 400 kHz; charging-detection (`batteryIsCharging` is
compile-time false on M9); GPIO12 ESP_WAKEUP characterization for a real
Power-off wake.

## Compass (QMC6309) + GPS motion for Lua apps (2026-08-22)

First use of the magnetometer. The chip was documented (peripheral bus, 0x7C)
but nothing ever talked to it; the QMI8658 IMU still has no driver.

- **Driver: `M9Compass.{h,cpp}`** (`HAS_M9_COMPASS=1` in the env). Probe =
  chip id 0x00 == 0x90; soft reset (0x0B=0x80 then the mandatory 0x0B=0x00 —
  the bit is not self-clearing); CTRL2 0x0B=0x30 (ODR 100 Hz, ±32 G, set/reset
  on), CTRL1 0x0A=0x41 (normal mode, OSR1 8, low-pass 4 — the datasheet's
  0x61 example is low-pass 8 at 50 Hz, which read as sluggish on the dial),
  both read back and re-written once if they did not stick.
  Read path: status 0x09 (bit0 DRDY, cleared by the read; bit1 OVFL → sample
  kept but flagged, logged at most every 10 s), then 6 bytes from 0x01
  little-endian int16, ×1/1000 → Gauss (±32 G chosen over ±8 G because of
  the bias magnitude question below; 1 mG/count is still ≈0.13° of heading).
  Synchronous on the UI thread from `luaHostCompass()` (three short
  transactions at 100 kHz, no poll hook), cached sample valid 1 s, re-probe
  every 2 s while absent (rail-powered part may still be in POR when
  `radio_init()` runs), eight consecutive bus errors → forget and re-probe.
  Boot log: `M9 compass: QMC6309 ok (id=0x90, 100 Hz, +/-32 G, OSR 8, LPF 4)` or the
  reason it is not. Register layout cross-checked against the Rev A datasheet
  and the SlimeVR/madflight/Tildagon drivers — NOT SensorLib, whose
  `setOutputDataRate()` writes the ODR into 0x0A (the OSR bits); Meshtastic
  inherits that bug and only works because it runs continuous mode.
- **Exposure: `CAP_COMPASS`** (device_caps.h, hardware gate, `caps().compass`)
  → `wada.sys.compass()` = `{x, y, z, ovfl}` Gauss, sensor frame,
  uncalibrated. No heading on purpose: see the two unknowns below.
- **GPS motion: `WadaNmeaLocationProvider`** (`src/helpers/`, `HAS_GPS_MOTION=1`)
  replaces the core `MicroNMEALocationProvider` in `target.cpp` — a line-for-
  line copy that also exposes RMC speed/course (the core keeps its parser
  private; patching libdeps is the build-fragile route this repo avoids).
  `wadaGpsMotion()` feeds `wada.sys.gps().speed_kmh/course`; course is absent
  under 1 km/h because MicroNMEA parses an empty course field as 0 (= north).
  `gps()` also returns nil while the GPS toggle is off.
- **App: `deploy/apps/gpscompass/1.0`** (Store catalog entry added; not baked
  into `lua_builtin.h` — `CAP_BUILTIN_LUA_APPS` also removes the Store > Apps
  tab). Keys: `C` start/finish calibration (auto-finishes after 20 s), `O`
  rotate the sensor frame 90°, `F` mirror it, `X` clear calibration, d-pad
  left/right or OK = cycle the target contact. Offsets/orientation persist in
  the app's KV store.

**Both unknowns are now MEASURED (2026-08-22), not guessed.** Held flat,
logging the raw vector (`M9_COMPASS_DEBUG` in M9Compass.cpp) at four headings
90° apart:

| heading | raw x | raw y | raw z | x−ox | y−oy |
|---|---|---|---|---|---|
| N | −0.395 | −3.068 | −2.768 | **−0.055** | **+0.310** |
| E | −0.072 | −3.396 | −2.704 | **+0.268** | **−0.018** |
| S | −0.327 | −3.653 | −2.752 | **+0.013** | **−0.275** |
| W | −0.565 | −3.394 | −2.754 | **−0.225** | **−0.016** |

1. **Axis orientation.** Hard-iron centre (ox, oy) = (−0.340, −3.378);
   `atan2(x−ox, y−oy)` then reads 350° / 94° / 177° / 266° at N/E/S/W —
   0/90/180/270 within a few degrees, counting UP clockwise. So **+Y points at
   the device's top edge, +X to its left**, and (right-handed) +Z into the
   screen. `deploy/apps/gpscompass` ships that as the default: correct after
   calibration alone, with no orientation press. Repeatability: returning to
   north landed within 0.066 G / 0.041 G of the first reading.
   NB the app's first auto-handedness rule had this INVERTED — it assumed a
   Z-out-of-screen sensor was the un-mirrored case — which is what made a
   correctly-defaulted device turn the wrong way. The dip test now reads:
   held flat, north of the magnetic equator, a Z-INTO-screen sensor sees the
   downward field as POSITIVE z.
2. **The hard-iron bias is real and large** — about −0.34 G on X, **−3.4 G on
   Y**, −2.8 G on Z: ~7× Earth's field, which vindicates Meshtastic's
   otherwise implausible hardcoded extrema. The horizontal signal riding on it
   is only ~0.27 G, so an UNCALIBRATED M9 barely moves the dial — that is the
   expected symptom, not a fault. It also confirms the ±32 G range: at ±8 G the
   Y axis sits within half a scale of the rail before the user's own
   environment is added. Expect `|B|` ≈ 0.25–0.65 G once calibrated;
   "Field saturated" (OVFL, raw counts logged every 10 s) means a magnet is
   nearby.

**Calibration must ROTATE THE DEVICE IN ONE PLACE.** Carrying it around while
turning it does not only rotate it, it also TRANSLATES it through the field of
a laptop, a desk frame, anything ferrous — and that corrupts the fit. Measured
consequence: a centre that moved 0.15 G between hand-tumbled sessions against a
0.26 G horizontal signal, i.e. tens of degrees of direction-dependent error,
reported on hardware as "it drifts when rotating". The app now measures
coverage from GRAVITY (the accelerometer, below) and refuses a sweep that never
turned the device over, naming the axis.

## IMU (QMI8658) + tilt compensation (2026-08-22)

`variants/thinknode_m9/M9Imu.{h,cpp}`, `HAS_M9_IMU=1` → `CAP_IMU` →
`wada.sys.accel()`. Accelerometer only — the gyro is most of the part's power
budget and nothing here needs it. QMI8658 at **0x6B** on the same peripheral
bus; this board carries the **A** die (`WHO_AM_I 0x05`, `REVISION_ID 0x7C`).
±2 g at 62.5 Hz with the accel low-pass on, soft reset (0x60←0xB0) then a
160 ms wait covering both die variants and a 0x4D==0x80 check, same idle
suspend as the compass.

**CTRL1 bit6 (ADDR_AI) must be set and read back.** With it clear the burst
read from 0x35 silently returns six copies of one byte — a sensor that probes
fine and reports nonsense.

**Axes MEASURED** (three attitudes, `M9_IMU_DEBUG` logging):

| attitude | reading | conclusion |
|---|---|---|
| flat, screen up | z = −1.02 | **+Z into the screen** (down) |
| on bottom edge, top edge up | x = +0.97 | **+X at the top edge** (forward) |
| on left edge, right edge up | y = +1.08 | **+Y at the right edge** |

So the IMU is already in the aerospace body frame (forward / right / down), and
it agrees independently with the magnetometer's measured +Z-into-screen. The
magnetometer maps into that frame as `(fwd, right, down) = (my−oy, −(mx−ox),
mz−oz)`.

**Why tilt compensation was needed at all:** the field dips ~60° here, so the
vertical component is 1.6× the horizontal one and tipping the device leaks it
into the pair the heading is made from — about **1.5° of heading per 1° of
tilt**. A hand-held reading wandered by tens of degrees. The app now rotates
the field back into the horizontal plane using gravity (NXP AN4248 / ST AN3192)
before taking the angle, shows the tilt angle, and says "too steep to read"
past 55° instead of lying. Confirmed working on hardware.

**Not copied from Meshtastic:** its M9 driver passes both sensors through
untransformed, carries a dead 180° constant, has its axis swaps commented out,
and mirrors the heading on the sign of accel Z — so its compass flips when the
device is turned over.


**Hardware-verify recipe:** flash; boot log shows the `M9 compass:` line;
push the app over the console — the M9's card is soldered on, so
`scripts/sideload_app.py --port /dev/cu.wchusbserial10 --reboot
deploy/apps/gpscompass/1.0` (the `fput`/`fadd`/`fend` CLI commands write into
the Store's own `/apps/` on the card; "Your own apps" lists it, the catalog
copy arrives once `deploy/apps` is published); open the app; `C`, TUMBLING (not just spinning) the
device through every orientation for 20 s; check `|B|`; set O/F against a
known north; walk with GPS on and confirm `Speed`/`Course` populate above
~1 km/h; pick a contact and sanity-check the bearing against the map.

Serial-console notes learned doing this: the CH34x bridge resets the board
whenever the port is opened (DTR/RTS, regardless of what pyserial asks), and
the console is not serviced until `[BOOT] ui ready`; the UART interrupt is
not IRAM-resident, so while the loop sits in a flash-cache pause only the
128-byte hardware FIFO buffers input and the middle of a longer line is
lost — hence the sideload's short, self-checking lines. Done on 2026-08-22:
`M9 compass: QMC6309 ok` on the first flash (0x7C answers, config sticks);
app files pushed and listed by `ls /apps`; calibration + rotation confirmed
working by Chris on the device the same night.

Two more M9 findings from that session: (1) the keypad-nav focus highlight
painted the whole Lua app body white — the body is a clickable object on the
top layer, so `navCollect` focused it and `navFocusCb`'s reverse-video fill
covered it (the canvas on top stayed dark). Fixed in the host with
`NAV_SKIP_FLAG` on the body, the same exclusion the map's touch catcher uses.
(2) Low-pass depth 8 at 50 Hz felt laggy; now 4 at 100 Hz.

## Map re-open cost (2026-08-22) — measured and fixed

`[STALL] ui:lvgl 2597ms` on EVERY map open, not just the first: leaving the
Map tab called `freeMapTiles()` (UITask.cpp, tab-change handler), so the next
visit re-read and re-decoded all nine 256x256 JPEGs. Boards with >=4 MB PSRAM
now leave the slots exactly as panning within the tab leaves them — the grid
costs 9 x 128 KB = 1.15 MB, which only matters on the 2 MB V4 (already capped
to a 4-tile pool by renderMapTiles), so that board keeps the old free.

Measured on the M9 after the change: cold open (first after boot) 2598 ms,
every re-open **245 ms**.

Gotchas for anyone revisiting this:
- `releaseMapTileSlot()` is NOT a substitute: it clears `in_use`, so the next
  render treats the tile as absent and decodes it again. Keeping the slots
  fully intact is what makes renderMapTiles' pass-1 match-and-reposition hit.
- The remaining cold-open cost is the SJPG decode, not the SD read. Raising the
  CPU to 240 MHz is already ruled out (RGB565 noise from the PSRAM bus, see
  onMapTabActivated). The open levers are decoding on core 0, or the M9's SD
  clock (still 4 MHz — see the deferred list below).

  **SUPERSEDED by measurement, 2026-08-28.** The decode-vs-read framing above is
  directionally right but was reasoning about the two SMALLER halves. Instrumented
  on hardware (`[MAPPROF]`, audit pass 3 below), a 9-tile cold open split:

  ```
  total=2189ms probe=0ms render=2080ms
        [read=497ms  decode=539ms  other=1042ms  tiles=9]  markers=10ms
  ```

  `other` — the per-tile `lv_refr_now()` progressive paint — cost **more than the
  SD reads and the JPEG decodes combined**. On the immersive full-screen map each
  call composites the whole 240x320 panel *and* alpha-blends the transparent
  status + tab bars over it: ~115 ms a call, nine times. Throttling that to one
  paint per 350 ms (`k_map_progressive_ms`) is a bigger win than either lever
  named above, and it is board-independent. Measure before optimising here: the
  intuition that "it must be the decode" was wrong by a factor of two.

## Audit pass 3 (2026-08-28) — cold map open + a real Back history

Third full sweep, prompted by two field reports: "the map doesn't load fast the
first time" and "Back should go back to the previous navigation". Both were
real; both had one identifiable cause. All eight PlatformIO envs compile-verified
(the shared `UITask.cpp` is the blast radius); on-device validation still wanted.

**Map, cold open.** The 2026-08-22 pass above fixed the *re-open* cost and left
cold open at 2598 ms, correctly attributing the bulk to the SJPG decode. What it
did not catch is what runs *in front* of that decode:

- **`bestAvailableZoom()` was called unconditionally** (`onMapTabActivated`) and
  its result discarded one line later whenever a zoom was persisted — which it
  is as soon as the user touches +/- once. It walks z19→z3 probing the card at
  each level, returning at the first hit. Now skipped when the answer cannot be
  used. It has no other caller, so this is pure dead-work removal.
- **The no-fix case was never first-open-only.** `s_map_view_inited` only latches
  once a non-zero centre exists, so with no GPS fix and no Profile location the
  recenter guard never closed: the full 17-level walk ran against lat/lon 0,0 —
  tiles that cannot exist — on *every* map open, forever, and `renderMapTiles`
  then bailed to the placeholder and freed the slots anyway. The centre is not
  persisted across reboots either (only the zoom is), so every boot starts here.
- **The "Loading map…" hint could not paint.** It was set *after* the probe, so
  the whole scan happened with the outgoing tab still frozen on the panel. Moved
  to the top of the function, and `goToTab` now uses `LV_ANIM_OFF` for the map
  tab so the flush is not racing a 200-400 ms slide. Zero ms saved, but it turns
  a frozen screen into visible progress — which is most of the reported bug.
- **`tileExistsAt` probed `.png`, the generic loader only opens `.jpg`.** The
  zoom guard therefore advertised, and the auto-snap selected, levels that render
  blank. The `.png` leg is gone: the guard now mirrors the loader, and every
  probe costs half as much.

**Map, data loss (ship this one regardless).** `tileCacheRemove(path_png)` in the
fetch worker — "drop any stale .png from an older build" — was written for the
firmware's private LittleFS partition. `UITask::begin` re-points `s_tile_fs` at
the SD **root** on this board with `s_tile_root = ""`, so it resolved to
`SD.remove("/tiles/<z>/<x>/<y>.png")`: the user's own offline map pack. The
skip-check above it only ever tests the `.jpg`, so every queued tile whose `.jpg`
was absent erased the co-located `.png`. Now gated on `s_tile_fs == &s_tiles_fs`,
the same discriminator `tileCacheMarkSdIo`/`tilesFsLowSpace` use. Wi-Fi-only
trigger, permanent loss.

**Back.** The ladder was a faithful copy of `pagerNavGoBack` **minus one rung** —
`else if (getActiveTab() != HOME_TAB_INDEX) navGoToMainTab(HOME_TAB_INDEX);`.
Without it Back fell through to `navPushTap(LV_KEY_ESC)`, and nothing in this
build consumes `LV_KEY_ESC`: the sole consumer is `lv_dropdown`, peeled several
rungs above, and `navMaybeRebuild` deliberately keeps the tab bar out of the
focus group. The press was consumed by the `return true` and nothing happened, on
every bare tab.

Restoring that rung alone gives *Home*, not *back*, so this pass also adds the
history that never existed anywhere in this UI (`s_lv_tab_prev` is a single
transition latch; `s_apppage_close` is depth 1; the popup registry walks in
static table order):

- **`s_m9_nav[]` — 12 entries, 24 B.** Records main-tab switches only. Pushed
  from `goToTab` (the single choke point, and the only place that fires
  `LV_EVENT_VALUE_CHANGED` explicitly), popped by a new rung placed **below**
  every peel rung, so modals still close innermost-first and a null-close blocker
  row still stops Back at the registry rung.
- **Deliberately not stacked:** the app drawer (a registry row rung 6 already
  closes, whose mode survives a tab switch and is restored on return to Home) and
  app pages / chats / settings sheets (all peeled above). Duplicating those here
  would double-close.
- **Never store an `lv_obj_t*`** — settings sheets, app pages and the drawer are
  destroyed, often via `del_async`. Entries are tab indices; one that no longer
  applies is skipped, never a dead press.
- **`s_m9_nav_replaying`** suppresses the re-push from a pop's own `goToTab`.
  The Home-fallback rung needs it too: without it that jump recorded the tab it
  was leaving and the next Back popped straight back — Back ping-ponging forever.
- **HOME clears the trail** (it means "go to root"); MSG/MAP are lateral jumps
  and keep pushing.

Also closed in this pass, all confirmed and all reachable on this board:

- **`openMeshContactDm` bypassed the choke point** — a bare `lv_tabview_set_act`
  with no event send, so `tabChangedCb` never ran, `s_lv_tab_prev` went stale,
  and the *next* real tab change computed `leaving_inbox` false and skipped the
  only code that hides the DM overlay. The chat stayed painted over whatever tab
  you moved to. The switch now fires the event, and moved above the overlay
  setup so `tabChangedCb`'s own `hideKb()` cannot undo it.
- **Setup wizard Back was a dead key** for the whole wizard, even on the three
  steps that draw a working on-screen Back wired to `setupBackCb`. The comment
  claimed "nothing to back out of"; the wizard is a four-step stack.
- **Contacts select mode outlived its tab.** Its registry row has `flags=0`, so
  `anyPopupOpen()` never reported it and nothing cleared it on tab change —
  leaving Back to close it *invisibly* from another tab. `ctExitSelectMode()` now
  runs on leaving Contacts, the same discipline `s_m9_map_pan` already gets.
- **`navGoToMainTab` ignored key-blocker rows** — the unconditional 8-iteration
  drain spun closing nothing and switched tabs out from under a running SD format
  or bulk delete. Now returns bool and stops. (The board's SUB_MESSAGE/SUB_MAP
  cases already did this correctly; HOME and `navGoToMainTab` did not.)
- **Two focus-group blackouts** that made Back's *result* undrivable: the
  notification chip was a non-`NAV_SKIP` `lv_layer_top` child with nothing
  focusable inside, emptying the group for its ~1.1 s life on every incoming
  message; and the Spectrum page's `NAV_SKIP_FLAG` root hid it from
  `navTopHasVisibleChild`, re-rooting the group onto the app drawer still open
  beneath it (d-pad moved an unseen ring over hidden tiles, OK launched them).

  **The Spectrum fix needs BOTH halves — `NAV_PASSTHRU_FLAG` alone is a trap.**
  Passthru excludes only the container; `navCollect` still recurses into the
  subtree. `lv_chart` does **not** clear `LV_OBJ_FLAG_CLICKABLE` (LVGL 8.4 sets
  it in the base `lv_obj` constructor; only `lv_label` and `lv_img`/`lv_canvas`
  clear it), and `lv_obj_remove_style_all()` on the five colour-ramp swatches
  removes styles, not flags. So passthru-on-root alone handed the page six focus
  stops where it previously had zero, and `navFocusCb`'s reverse-video fill
  painted a solid white block straight over the live trace — caught on hardware
  within a minute of flashing. The chart and each swatch now carry
  `NAV_SKIP_FLAG` so the group ends up genuinely empty, which is what the
  original comment always claimed. Anything added to this page later must do the
  same, or it becomes a focus stop by default.

  Known consequence: with the page as the focus container, `useTop` is true and
  `navAddStatusBarActions()` is skipped, so the "‹ Spectrum" chevron is no
  longer a d-pad target. Back (hardware key / registry row) still closes the
  page on every board, which is the documented exit.

- **`ctExitSelectMode` must not go through `ctSetSelectMode(false)`.** That
  setter ends in `s_ct_list_force = true; refreshContactsList();` — the full
  teardown+rebuild measured at ~1.3 s with a large list (#82). Called from
  `tabChangedCb` as the user LEAVES Contacts, inside the tabview's
  `VALUE_CHANGED` chain, that freezes the tab they are moving *to*, on every
  board. It now restores the toolbars and only ARMS `s_ct_list_force`; the
  rebuild lands on the way back in, where `tabChangedCb` already calls
  `refreshContactsList()` and the user can see it.

- **Blocker check must precede the app-page close.** With `navGoToMainTab` now
  refusing to switch while a null-close progress row owns the screen,
  `M9_KEY_LEFT_MESSAGE` and `M9_KEY_MAP` — which called `s_apppage_close()`
  *first* — would have destroyed the page and then not jumped, leaving the user
  with neither. Both now drain-and-bail before closing anything, the order
  `SUB_MESSAGE` / `SUB_MAP` already used.

**Measured on hardware after the above** (`[MAPPROF]`, a `HAS_M9_KEYBOARD`-only
`Serial.printf` in `onMapTabActivated` with `micros()` accumulators around
`loadTileJpeg` and the decode — **diagnostic scaffolding, strip or gate it before
release**):

| | cold open (first after boot) | re-open |
|---|---|---|
| before this pass | 2598 ms | 245 ms |
| after dead-work removal | 2189 ms | 141-165 ms |
| **after repaint throttle** | **1507 ms** | **~150-190 ms** |
| split, before throttle | read 497 / decode 539 / **other 1042** / markers 10 | render 5-6, tiles=0 |
| split, after throttle | read 499 / decode 539 / other **358** / markers 10 | render 7, tiles=0 |

The throttle moved `other` and nothing else (read 497->499, decode 539->539 are
run-to-run noise), which is the confirmation that it cut compositing rather than
starving the load path. Cold open is down **42%** from the 2598 ms baseline; the
`[STALL] ui:lvgl` bucket for the same action fell 2497 ms -> 1822 ms.

With compositing no longer dominant, the two levers named in the superseded note
above finally ARE the top of the list, and they are now roughly equal in size:
decode 539 ms (core-0 decoding) and read 499 ms (the 4 MHz SD clock — one line,
`SD_SPI_FAST_HZ`, which only the V4-R8 env sets today). Treat the SD clock with
care: this board shares one SPI peripheral across radio + display + card, and
deferred item 4 records that the M9's mount-ladder timing margins were never
characterised.

Three things this settled that static reading could not:

1. **`probe=0ms` confirms the `bestAvailableZoom` guard fires.** The 2598→2189
   delta (~410 ms) is the scan that used to run and be discarded — the top of the
   100-500 ms band the audit estimated.
2. **`other` dominates.** See the correction under "Map re-open cost" above. The
   progressive per-tile repaint, not decode and not SD, was the biggest single
   cost. Now throttled via `k_map_progressive_ms`.
3. **The retained-slot path works exactly as designed** — a re-open reports
   `tiles=0`, `render=5-6ms`: a true pure blit. Note that ~135 ms of the ~150 ms
   re-open is now the "Loading map…" hint's `lv_refr_now`, which fires even when
   the render will not block. Suppressing it when a pure blit is predictable
   (same zoom + centre as the last render, and `s_map_last_missing == 0`) is the
   obvious next trim, and is NOT done.

Stall attribution had to be fixed first, or none of these numbers meant what they
said: `uiCp("ui:input")` lives inside `#if CAP_TRACKBALL`, which is **0** on the
M9, while the keyboard drain sits between the `ui:threads` and `ui:diag`
checkpoints. So on the one board whose *only* input is the d-pad, every
keypress-driven action — map opens included — was billed to `ui:threads`. A cold
map open was being reported as a slow chat-threads refresh. The M9 now has its own
`uiCp("ui:input")`. (Work that reaches LVGL via `navPushTap` still lands in
`ui:lvgl`, since the handler runs inside `lv_timer_handler`; that is correct.)

Still open on the map, in win/risk order: a priority lane for visible tiles
(`xQueueSendToFront` — today the nine tiles you are looking at queue *behind* the
29-tile background prefetch pyramid on every pan), a retry pump so a dropped
fetch heals without a manual pan, and the SD clock below.

## Battery pass (2026-09-02) — saver unreachable, GPS-off base current

Battery-focused sweep of the board code plus every shared power path the M9
compiles. Verified sound and left alone: `getBattMilliVolts()`'s ADC2
per-sample filter + 2:1 math; backlight-off (`ledcWrite(ch, 255)` is
special-cased by the framework to duty 256 = constant HIGH — checked in
`esp32-hal-ledc.c`, so "off" is truly off, no 1/256 PWM sliver); compass/IMU
suspend-on-idle actually ticked from the UI loop; the 15 ms keyboard poll; the
20 s default screen timeout with the 240→80 MHz drop; buzzer noTone + high-Z;
the (latent) deep-sleep hold sequence; battery-log flash wear (~century-scale
on SPIFFS at the 5-min cadence). Three findings, two fixed now:

1. **Battery saver was unreachable — FIXED.** The touchSleep idle throttle was
   fully wired on the M9 (hooks installed + pref restored under
   `HAS_TOUCH_UI`, every gate predicate functional) but the only enable UI —
   the Settings→Battery switch — was compiled for T-Deck + V4-R8 only, and the
   pref defaults OFF with no console path either. So an idle, screen-off,
   standalone M9 free-spun its loop forever; the whole subsystem was dead
   code. Both `#if`s (settings row + `sleepIdleToggleCb`) now include
   `HAS_THINKNODE_M9`. The M9 is arguably the *safest* board for it: the
   throttle is a plain vTaskDelay in the loop task, wake is the keyboard poll
   which runs through it (≤50 ms latency on a dark screen), and the M9 sits
   in `batteryIsCharging()`'s `#else` branch (compile-time false), so the
   "USB powered" gate can never block — even stronger than the R8, whose
   runtime voltage heuristic still can (the R8's widening rationale was that
   its heuristic doesn't block in practice, not compile-time falsity).
   Matching the R8 precedent, no status-bar indicator (the amber battery
   tint stays T-Deck-only); the settings row shows the live block reason.
   VERIFY on hardware — and note the obvious probe is a decoy: the Battery
   chart's CPU-MHz series sits at 80 on ANY screen-off M9 (that drop belongs
   to the pre-existing screen timeout, not the throttle), and keypress wake
   also works saver-off. The discriminating readout is the Battery chart
   window's "Idle power-save" stats card (wakeCount / % asleep — unguarded,
   present on the M9). The FULL gate must pass or the counters sit at 0 and
   the feature looks broken: saver on, screen off, no companion client, AND
   the BLE + Wi-Fi radios actually off (BLE is on by default in this
   companion build — "standalone" is not enough; the settings row's block
   reason names the offender), on battery, mesh idle. Under that regime the
   counters must MOVE (on a boot where the saver was never engaged they
   never leave 0). Then confirm a keypress still wakes the dark screen
   (≤50 ms extra latency is expected).

2. **GPS-off burned Q16 base current — FIXED, magnitude needs the bench.**
   `WadaNmeaLocationProvider`'s constructor and `stop()` followed the core's
   convention of parking GPS_RESET *asserted*. Zero-cost on a direct-wired
   active-LOW reset — but the M9's reset is inverted (GPS_RST1 → R46 → NPN
   Q16, GPIO HIGH = asserted), so "asserted" held GPIO5 HIGH sourcing
   (3.3 V − Vbe)/R46 continuously whenever the GPS was OFF (its default
   state; `initBasicGPS()` stops it at every boot) — for a module whose rail
   EN had already cut. Both sites now park the line LOW, the zero-current level
   for *both* circuits (LOW = asserted on direct-wired boards, so nothing
   changes there; the provider is M9-only today anyway). The clean-reset
   guarantee is intact: every start path calls `begin()` then `reset()`,
   which pulses the line explicitly. VERIFY on hardware: R46's value (read
   it off the board or the schematic) decides whether this saved ~0.3 mA
   (10k) or ~2.7 mA (1k); and confirm GPS still acquires after an
   off→on→off→on toggle cycle.

3. **Screen-off leaves the ST7789 panel driving — still open (already on the
   deferred list).** The M9 branch of `touchScreenBacklight()` kills only the
   backlight; SLPIN/DISPOFF panel sleep remains "designed, not coded blind"
   (shared-SPI variant of the T-Deck path). The battery angle strengthens
   the case: SLPIN stops the oscillator/booster/LC drive (~1–2 mA class) for
   every screen-off hour, on top of the anti-burn-in argument.

4. **Saver × GPS: mid-session enable ran NMEA into the stock 256 B ring —
   FIXED (found by this pass's adversarial review of finding 1).** The 4 KB
   Serial1 RX ring from the TTFF fix is installed at boot ONLY when
   gps_enabled was already persisted, and `setRxBufferSize` is a no-op on a
   running UART — so the session where GPS is first enabled ran on 256 B
   (~22 ms of slack at the M9's 115200 baud, the only saver board at that
   rate BY DEFAULT — the baud is NVS-overridable on every touch board, so a
   T-Deck/R8 dialed up to 115200 hits the same wall; at their defaults,
   38400 / 9600 can't overflow in one park). With the saver newly reachable,
   every 50 ms park spans ~576 line-rate bytes: a 1 Hz NMEA burst landing
   inside a park lost its tail. The damage is host-side — MicroNMEA discards
   checksum-failed sentences, so the host's parsed fix goes stale (worst
   case, with every burst clipped, it never sees an intact sentence until
   reboot); the receiver itself keeps its own acquisition regardless. Root
   cause fixed rather than gating the saver (a screen-off standalone tracker
   is exactly the saver's target regime): `gpsEnsureBigRxRing()` in main.cpp
   cycles the UART (end → 4 KB ring → begin at the initBasicGPS pins/baud)
   and is called from BOTH mid-session "gps on" funnels —
   `UITask::toggleGPS()` (keyboard GPS_LONG chord, CC chip, settings switch)
   and the companion `CMD_SET_CUSTOM_VAR("gps")` handler in MyMesh.cpp —
   right AFTER a successful start (so a refused enable on a GPS-less V4/R8
   never spends the 4 KB; the just-started cycle is benign — everything
   Serial1 runs on loopTask, worst case one torn checksum-rejected
   sentence on the EN-less boards whose modules stream even while
   "stopped"). Boot-persisted GPS-on takes the unchanged boot path; GPS-off
   boots still pay 0 extra DRAM until GPS actually starts. VERIFY on
   hardware: enable GPS mid-session (no reboot) with the saver on, screen
   off → a fix should still arrive and hold.

Also corrected: BOTH copies of the "keyboard S2 sits on the switched
peripheral rail" claim — `M9Keyboard.cpp` begin() and the keyboard-backlight
cache comment in `UITask.cpp`'s M9 branch — now match the schematic-verified
note in platformio.ini (always-on 3V3 via R3). It matters: a deep-sleeping
M9 still feeds the S2; only the slider cuts it.

## Deferred — hardware-verify list

These are left intentionally unset/unwired rather than guessed:

1. **RF-switch DIO table — confirmed working.** Pin assignment (DIO5/DIO6)
   was schematic-confirmed; the per-mode HIGH/LOW truth table was carried
   from convention rather than a switch-IC datasheet (part number not legible
   on the schematic) — but bidirectional radio communication is now confirmed
   working on real hardware, so the table is correct as-is. No longer open.
2. **GPS pins + baud — confirmed and fixed.** RX/TX were swapped
   (`PIN_GPS_RX=3`, `PIN_GPS_TX=2`, not 2/3) and the baud rate needed to be
   `GPS_BAUD_RATE=115200`, not the library's 9600 default. Both confirmed via
   hardware testing with `GPS_NMEA_DEBUG=1` (raw-sentence passthrough) —
   remember to pull that debug flag back out for release builds. EN/RESET
   polarity (both active states inverted from the library defaults) was
   fixed earlier and is also confirmed.
3. **Display rotation.** VERIFIED: `DISPLAY_ROTATION=1` (3 was 180 deg off on
   hardware, bring-up #6).
4. **microSD mount-ladder timing.** Mounting/browsing/reading confirmed
   working on hardware; the specific timing margins on M9 (vs. the T-Deck
   electrical characteristics the ladder was originally tuned against)
   haven't been separately characterized — if mounting ever becomes flaky,
   start here.
5. **Wallpaper picker — done.** Guard split completed (settings row, forward
   declarations, implementation block all extended to M9, separated cleanly
   from the genuinely-T-Deck-only I2S notification-sound chooser). Confirmed
   working on hardware, including the dedicated Settings > Lock browsing UI.
6. **Deep-sleep wake source has NO real GPIO on M9 — MITIGATED in the 2026-08-19
   audit pass, GPIO12 characterization still open.** M9 has no BOOT/user button
   at all (only a physical power-cut slider and a reset button, neither a GPIO
   — schematic-confirmed). The Power menu's "Power off" row is now HIDDEN on M9
   (it deep-slept with ext0 armed on the nonexistent button — device
   unrecoverable until a slider cycle; the slider IS the power-off), and
   `PIN_USER_BTN` was removed from the env. `M9Board::enterDeepSleep()` was
   cleaned to timer-only wake — its old "LR1110 DIO1 (GPIO42) wake" was
   electrically impossible (S3 RTC pads are GPIO0-21; every rtc_gpio_*/ext1
   call failed unchecked). **Still open, hardware-required:** characterize
   `ESP_WAKEUP` (GPIO12, from the keyboard MCU — RTC-capable): edge vs. level,
   polarity, pulse width. Once known, a real graceful Power-off can return via
   ext0/ext1 on GPIO12.
7. **KEY_LED (GPIO46) unwired; MIC deliberately unbound; CTRL now bound.**
   CTRL (0x90) opens the Control Center (2026-08-19 pass; the controller
   latches a single key so CTRL can never chord). GPS_LONG (0x87) toggles GPS.
   MIC (0x88) and 0x89 are deliberately unbound — see M9Keyboard.h. KEY_LED
   still has no driver.
8. **Battery reading — largely addressed (2026-08-19 pass), one hardware
   question left.** Three separate things were tangled here: (a) the
   battLogOnSd `/meshcomod` check landed back in 4846d4f (2026-07-08) — the
   "identified but not yet applied" note that used to sit here was stale; the
   empty-history symptom was that pre-fix bug. The log selection is now keyed
   off the RESOLVED ui-data backend (`uiDataFsIsSdCard()`), so a card that
   merely has a /meshcomod dir (telemetry/discover logs mkdir it on any card)
   can no longer hijack the log. (b) GPIO13 is **ADC2** (not ADC1 as the pin
   table used to claim) and ADC2 is Wi-Fi-arbitrated: blocked samples return
   ~offset-mV garbage — `getBattMilliVolts()` now filters per-sample and holds
   the last good reading. (c) Still open, hardware: whether "stuck at charging
   voltage after unplug" beyond that is pack/charger chemistry at the divider
   node — needs the raw-mV serial print on a real unit. Related:
   `batteryIsCharging()` is compile-time FALSE on M9 (no charge detection at
   all); widening the T-Deck's voltage-threshold detection to M9 needs
   hardware confirmation that the charger raises the divider node the same way.
9. **Message data loss on power cycle — root cause found and fixed
   (2026-08-19 pass), bench verification wanted.** `saveThreadsToStorage()`
   rewrote the threads index in place (mode "w"): a hard power-cut mid-write
   left a short file the next boot quarantine-DELETED — chat list, unread
   counts and DM entries gone (threads rebuild by name for channels, so the
   worst visible loss was exactly "channel messages disappeared" plus list
   state). Now tmp+rename (`uiDataReplaceFile`), orphan tmp swept at load.
   Residual, by design: messages inside the ~5 s SPIFFS coalesce window before
   a cut are lost. Verify with a bench power-cut loop while a channel floods.
10. **Commander (Home tab) landscape layout** — fixed (chart width, 5-button
    column height math). **Control-center overflow (6+ toggles not fitting)**
    — also fixed (row/chip sizing extended to M9, matching T-Deck's existing
    2-row wrap grid). Both confirmed working.
11. **Keypad-nav quirks — all but map panning addressed in the 2026-08-19
    audit pass (fixes compile-verified; retest each on hardware):**
    - Dropdown-list navigation: FIXED. The real root cause was simpler than
      the old note here claimed — NO `navOpenDropdown()` dispatch existed in
      any committed M9 key path (`git log -S navOpenDropdown` shows only the
      pager and Tanmatsu commits; the "Serial-verified dispatch" described
      below was never committed). `m9HandleArrowKey` now captures an open
      dropdown and FIFOs LV_KEY_UP/DOWN into it (Tanmatsu's navPump pattern),
      and the restored Back sends LV_KEY_ESC to the list before the popup
      ladder.
    - Textarea 3-4-press focus escape: FIXED (probable cause). Edit-mode
      LEFT/RIGHT consumed arrows as silent caret moves that no-op forever at
      the text boundary; they now fall through to `navMoveDir` when the caret
      doesn't move. If a pure-UP/DOWN reproduction survives on hardware, the
      cause is elsewhere (controller-side latching?) — retest.
    - Modal "breaks out": FIXED (two-part). Keys were dispatched against a
      one-tick-stale focus mirror (Enter's tree mutations land inside
      `lv_timer_handler` at the END of a tick) — the M9 drain now runs
      `navMaybeRebuild()` first; and `closeSettingsModal()` gained
      `navDetachBeforeTreeMutation()` + `navMarkDirty()` (the diagnosis below
      was necessary but not sufficient — the stale-mirror window was the part
      that survived the automatic sig-rebuild).
    - Home key not closing some overlays / Back inconsistent per-modal:
      FIXED. Fully explained by (a) the 0ea242c HW_BACK regression (nav-mode
      Back did NOTHING at all in the shipped tree) and (b) popup-registry rows
      compiled out on M9 (Files overlays, Terminal picker, fullscreen view,
      wallpaper picker) plus app pages (Lua/Snake/Web) not being registry rows
      — HOME and Back now close app pages via `s_apppage_close`.
    - Snake d-pad: FIXED. On M9 Snake is the store Lua app (native SnakeGame
      is compiled out under CAP_LUA_APPS) and it only listens for swipe/tap
      events no M9 path generated; the d-pad now feeds `luaAppSteer` (swipes)
      and the d-pad centre sends a synthetic tap + `ev.key=="enter"`, so
      start/steer/retry all work — for every store app, not just Snake.
    - Map panning: FIXED (second on-device round). Pressing the dedicated Map
      key while ALREADY on the Map tab toggles pan mode — arrows pan via
      mapNudge(), Map or Back exits (with toasts). A fresh entry to the tab
      always starts in nav mode. (Chosen over the old ENTER_LONG idea: the
      Map key re-press is discoverable and was a no-op before.)

    **Resolved this pass:**
    - Lock-screen unlock: fixed via `M9_KEY_ENTER_LONG` (the keyboard
      controller's own hardware-level long-press detection, a distinct byte
      from a normal Enter tap) standing in for "hold the trackball to
      unlock." No progressive countdown UI is possible this way (only a
      single discrete long-press event, not continuous press-state to poll),
      but it's a confirmed-working equivalent. Also fixed: the lock screen
      briefly showing the *previous* app screen before painting over it on
      wake (`lockscreenReveal()` was turning the backlight on before the
      lock overlay had actually been built/flushed — reordered + added
      `lv_refr_now()`), and `lockscreenReveal()` itself being a complete
      no-op for M9 (`#if defined(HAS_TDECK_GT911)` wrapped the whole
      function body — widened to `#if CAP_LOCK_SCREEN`).
    - Chat-message long-press context menu and the SD row's "hold: format":
      both fixed by the same generic mechanism — `M9_KEY_ENTER_LONG` fires
      `LV_EVENT_LONG_PRESSED` on whatever's currently group-focused, covering
      any widget with a long-press handler anywhere in the app, not just
      these two specific cases.
    - Home-button self-conflict: `M9_KEY_HOME`'s "close everything on top"
      dismiss loop was closing the app drawer itself (once registered as a
      popup), then immediately reading the now-mutated `s_home_drawer_mode`
      flag and reopening it in the same keypress. Fixed by snapshotting the
      flag before the dismiss loop runs.
12. **Spectrum app LR1110 pass (2026-08-20) — fixes compile-verified, three
    on-device checks wanted:**
    - Between-bin standby now sends the raw LR1110 `SetStandby(0x01)`
      (STDBY_XOSC) so the DIO3-powered TCXO stays up across bins — the
      vendored RadioLib's `RADIOLIB_LR11X0_STANDBY_XOSC` define equals
      `STANDBY_RC` (both 0x00, upstream define bug), so the named constant
      would silently re-select RC and re-pay the ~5 ms TCXO startup per bin.
      VERIFY: a `micros()` log around `spectrumSweepChunk()` should show
      ~5.5 ms/bin (~1 s full sweep), roughly half the pre-fix time.
    - Open now runs one span-wide `calibrateImageRejection(start, stop)`
      (begin() only calibrated mesh ±4 MHz); restore re-runs the mesh's own
      ±4 MHz cal from a true STDBY_RC. VERIFY: mesh RX sensitivity unchanged
      after a Spectrum session (image cal is back to the begin()-time band).
    - Opening Spectrum during an in-flight mesh TX now waits (bounded by the
      dispatcher's own 1.5x-airtime budget, capped 6 s) instead of truncating
      the packet mid-air; the consumed TX-done means the dispatcher logs that
      packet as a timed-out send — stats/log blemish only. VERIFY: open the
      app while a long send is on air; the send should complete (watch for
      the dispatcher's timeout warning, and no partial burst on a monitor).
    Also fixed in the same pass: sweep clamps now 150-960 MHz (LR1110 range;
    setFrequency failures skip the bin instead of mis-attributing RSSI), a
    0-dBm failed-read sentinel guard in the peak-hold (shared with SX126x),
    config commands now issued from standby on open, and the stale SX126x-era
    comments/readout. Bin pitch (150 kHz) vs RBW (62.5 kHz) = ~42% span
    coverage is documented at the constants, deliberately unchanged.
13. **App sync after a power cycle — fixed (2026-08-20), bench verification
    wanted.** User report: the V4-R8 "remembers the chats and syncs them when
    connecting to an app (MeshCore / MeshCore One)" but the M9 does not. Two
    separate stores are involved. (a) The on-device chat store (UITask
    threads/segments) — its power-cut loss was Deferred #9 above, fixed on
    this branch but NOT in any released build up to beta_66. (b) The
    companion sync ring (`MyMesh::history_ring` + per-client cursors, what
    `CMD_SYNC_NEXT_MESSAGE` / `SyncSince` replay to an app that connects
    later) was RAM-only on EVERY board, so any power cycle emptied it. On the
    V4 that mostly goes unnoticed (USB-powered / left on; its menu power-off
    deep-sleeps and also loses RAM), but the M9's only "off" is the slider —
    a hard VBAT cut — so every M9 session started with an empty ring and the
    app got `NO_MORE_MESSAGES` for everything received while it was away.
    Now persisted on all boards (shared code, `MyMesh.cpp`
    "Companion sync-history persistence"): `/synchist` append-only log of the
    message frames (~180 B per message, coalesced 3 s / 10 s max, compacted
    tmp+swap once it holds > 2x the ring) and `/synccur` per-client
    `last_delivered_seq` + `next_seq` (tiny tmp+swap, coalesced 5 s / 30 s
    max, flushed immediately on app disconnect). Both live on
    `DataStore::getHotDataFS()` — the SD card under /meshcomod when the store
    adopted it, else SPIFFS — and are restored in `MyMesh::begin()`. All
    reboot / power-off / download-mode / SD-copy paths flush them next to
    `persistHistoryNow()`. VERIFY on hardware: receive a few DMs + channel
    messages with the phone app disconnected, slide the M9 off, slide it on,
    connect the app — the messages should arrive. Residuals by design:
    messages inside the 3 s coalesce before a hard cut are not replayed
    (the on-device store has its own 2-5 s window), and a cut inside the
    cursor window can make the app re-receive a few already-seen messages.


## Keyboard: register-addressed I2C slave (protocol build #8, USB-pad release build #9)

"No keys do anything" root cause: the keyboard is a separate ESP32-S2 running
Elecrow's matrix-scanner firmware (ThinkNode-M9-KB-platformio, provided by Kaj
2026-07-06) as an I2C slave @0x6C with addressed registers: 0x00 HW ver (0x03),
0x01 KEY VALUE (0x00 = none, single latched slot), 0x02 backlight duty, 0xFE FW
ver (0x10). A key read must WRITE the register address (0x01) first, then read
one byte. The contributed patch's "one raw byte per read" protocol read the
last-addressed register instead — register 0x00 after the controller's reset,
i.e. a constant 0x03, never a key. Driver fixed in M9Keyboard.cpp
(write-then-read + version-register boot probe with serial log + Wire fallback
probe + 1 s re-probe + backlight setter). The controller resolves shift/sym/alt
layers itself and sends final ASCII; long-press codes 0x87 (from 0x84) and 0xA3
(from Enter) exist but are not yet bound in the UI.

Build #8 was still dead. Schematic verification (V1.0 sheet, high-res crops):
keyboard bus host GPIO20 = ESP32-2_SDA / GPIO21 = ESP32-2_SCL (through R2/R1
series, pullups R73/R72, S2 on always-on 3V3) — wiring correct. The real
blocker: GPIO19/20 are the S3's native USB D-/D+ pads, owned from reset by the
ROM's USB-Serial-JTAG peripheral (D+ pullup on GPIO20 = SDA). The M9's console
is an external UART bridge, so nothing ever released them. M9Board::begin() now
clears USB_SERIAL_JTAG_CONF0.USB_PAD_ENABLE before any bus init (build #9).
Bonus schematic finds: SD_CS = GPIO48 (the patch's "36" misread package pin 36 =
SPICLK_N = GPIO48; SD IS on the shared SPI bus), keyboard wake pulse
ESP32_WAKEUP = host GPIO12, KEY_LED = host GPIO46, LCD_TE = GPIO19. If #9 is
still dead, suspect a BLANK keyboard S2 on preprod units - flash it with the
ThinkNode-M9-KB-platformio project via the J6 header (carries ESP32-2
UART/EN/BOOT).

## Radio init -706: old LR1110 transceiver firmware (SOLVED, build #5 — CONFIRMED on hardware)

Tester log 2026-07-06: `Base FW version: 3.3` (0x0303, the original release),
`DriveDiosInSleepMode unsupported (old LR11x0 FW), skipping`,
`LR1110: hw=0x22 device=0x01 fw=3.3 wifi=2.1 gnss=0.0 errors=0x0000`,
`[BOOT] radio ok`, UI up. Build #6 fixed the panel orientation: DISPLAY_ROTATION
3 -> 1 (was 180 deg off). Build #7 fixed the "content a bit left and down":
UITask's per-board s_ui_rotation overrides (T-Deck, Tanmatsu) had no M9 entry,
so LVGL rendered the PORTRAIT default 240x320 into the landscape 320x240 panel
window. Fix = `s_ui_rotation = LV_DISP_ROT_90` under HAS_THINKNODE_M9 (ROT_90 ->
panel rotation 1). Rule of thumb: a new landscape board needs BOTH the
DISPLAY_ROTATION build flag (splash) AND the UITask s_ui_rotation override (LVGL
UI).

The -707 -> -706 progression decoded: -707 (CMD_FAIL) with tcxo=0 was the
calibration failing on a dead 32 MHz clock; with TCXO 3.3 V the calibration
passes and init reaches `driveDiosInSleepMode` (opcode 0x012A, added in Semtech
transceiver FW 0x0308) which RadioLib 7.x sends unconditionally in
`LR11x0::config()`. Preprod M9 chips run older FW and answer CMD_PERR ->
RADIOLIB_ERR_SPI_CMD_INVALID (-706), aborting an otherwise healthy init.
Meshtastic works on the same unit because it pins an older RadioLib that never
sends the command. Fix: `scripts/build/patch_radiolib_lr11x0.py` (extra_script
on the M9 env) makes config() skip that optional command on old FW;
`radio_init()` now prints `LR1110: hw=.. device=.. fw=X.Y ... errors=0x....` in
both outcomes, and the env carries RADIOLIB_DEBUG_BASIC=1 during bring-up. NOT a
shared-SPI problem: PERR is a well-formed chip reply, so the bus is clean.
