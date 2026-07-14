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
| Peripheral I2C SDA/SCL       | 7 / 6           | RTC 0x51, IMU 0x6b, compass 0x7c                                                                                                                                                                                                                              |
| Keyboard I2C SDA/SCL         | 20 / 21         | controller @ 0x6c, **own bus** (Wire1)                                                                                                                                                                                                                        |
| Battery ADC                  | 13              | ADC1_CH2, **2:1 divider** (manufacturer-confirmed)                                                                                                                                                                                                            |
| GPS RX/TX                    | 2 / 3           | CC1167Q, UART1                                                                                                                                                                                                                                                |
| GPS EN / ON_OFF / RST / 1PPS | 11 / 10 / 5 / 4 | EN+RST wired, **both active-LOW** (confirmed on schematic: same P-MOS circuit as the peripheral power rail for EN; GPS_RST1 -> R46 -> NPN Q16 base, GPIO HIGH turns Q16 on and pulls the module's reset line to GND, so HIGH asserts reset / LOW releases it — inverted vs. the library defaults, both now overridden in platformio.ini). ON_OFF + 1PPS unused |
| Buzzer                       | 9               | wired — simple GPIO piezo (`THINKNODE_M9_BUZZER_PIN`), Arduino `tone()`/`noTone()`, shares the Heltec V4 buzzer code path |
| ESP_WAKEUP (from KB MCU)     | 12              | not wired; behaviour undocumented                                                                                                                                                                                                                             |
| KEY_LED                      | 46              | not wired                                                                                                                                                                                                                                                     |
| User button (BOOT)           | 0               | only direct button besides the keyboard                                                                                                                                                                                                                       |

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

One raw byte per read, controller resolves shift/symbol layers itself:

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
  rails (periph rail + backlight) via `RefCountedDigitalPin`, 1:1 battery
  divider, deep-sleep/wake on LR1110 IRQ.
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
6. **Deep-sleep wake source has NO real GPIO on M9 — confirmed broken, not
   just unverified.** The "Power off" menu's `esp_sleep_enable_ext0_wakeup()`
   call is written against `PIN_USER_BTN`, which does not correspond to any
   real button on this board — M9 has no BOOT/user button at all (only a
   physical power-cut slider and a reset button, neither of which are GPIOs
   the firmware can wake from). Confirmed on schematic: M9 has no such button.
   Practical effect: using "Power off" currently leaves the device requiring
   a full manual power cycle (slider) to wake — not a graceful wake at all.
   **Fix path, not yet implemented:** `ESP_WAKEUP` (GPIO12, from the keyboard
   MCU) is a strong candidate — it's specifically documented as an unwired
   wake-pulse line separate from the normal I2C keyboard bus, exactly the
   kind of signal meant to wake the host chip while the main bus is powered
   down. Its actual trigger behavior (edge vs. level, polarity, pulse width)
   is undocumented and needs characterizing on real hardware before wiring
   `esp_sleep_enable_ext0_wakeup()`/`ext1_wakeup()` to it. Separately,
   `M9Board::enterDeepSleep()` (a different, scheduled/automatic sleep path
   used by the mesh stack, not the user-facing Power-off menu) also has no
   button/GPIO wake source — only LR1110 `DIO1` (radio activity) and a timer
   — same open question applies there once GPIO12 is characterized.
7. **KEY_LED (GPIO46), MIC/CTRL keys.** Pins/keycodes exist; no driver/action
   references them yet.
8. **Battery reading appears stuck at "charging" voltage after charger
   disconnect, and battery-history logging shows no entries over hours of
   runtime.** Traced the entire software chain (`getBattMilliVolts()` ->
   `batteryMvSampled()` -> `batteryMvSmoothed()` -> status bar) — every layer
   either re-samples fresh from the ADC or holds a value for at most 20
   seconds; nothing in the code caches indefinitely. No software bug found in
   this specific path via static reading. Two real possibilities, not yet
   distinguished: (a) genuine hardware/battery-chemistry behavior (Li-ion
   charge-termination voltage recovery can legitimately take longer than
   expected to settle), or (b) a real bug not yet found. Needs a raw
   millivolt diagnostic print directly in `getBattMilliVolts()` to tell which.
   Separately, but likely related: `battLogAppend()`'s SD-vs-SPIFFS selection
   (`battLogOnSd()`) should check `SD.exists("/meshcomod")` and return false
   if it doesn't exist (respecting DataStore's opt-in "store on SD" setting
   rather than assuming SD whenever a card happens to be mounted) — identified
   but not yet applied.
9. **Message data loss on power cycle (not on a clean reboot).** Confirmed
   with channel messages specifically — points at something not being
   flushed to persistent storage before a hard power-off that a clean reboot
   flushes correctly. Not yet investigated.
10. **Commander (Home tab) landscape layout** — fixed (chart width, 5-button
    column height math). **Control-center overflow (6+ toggles not fitting)**
    — also fixed (row/chip sizing extended to M9, matching T-Deck's existing
    2-row wrap grid). Both confirmed working.
11. **Keypad-nav quirks still open, all UI/UX not hardware:**
    - Dropdown-list navigation doesn't work (dropdown closes and focus moves
      to the next page element instead of moving the highlighted option).
      Traced the entire mechanism — `navOpenDropdown()` detection, FIFO
      push/pop, indev group assignment, LVGL's own dropdown `LV_EVENT_KEY`
      handler, main-loop ordering relative to `lv_timer_handler()` — and
      confirmed the M9 dispatch code is correct up to and including the
      `navPushTap(LV_KEY_DOWN)` call itself (verified via an in-code Serial
      print that the correct branch is taken). Root cause not found; likely
      downstream in LVGL's own delivery/processing at runtime.
    - Textarea fields specifically need 3-4 presses to move focus off them
      (confirmed: buttons/toggles/icons/apps all traverse in one press, only
      textareas affected). Ruled out the keyboard drain-loop/`lv_timer_handler()`
      ordering as the cause (reducing the drain to one key per `loop()`
      iteration didn't help). Root cause not found.
    - Modal navigation occasionally "breaks out" to the screen behind and back
      while navigating up through a modal's items. Likely candidate:
      `createSettingsModal()`'s standalone-modal path (used for Device/About/
      etc.) has no `navMarkDirty()` call after `closeSettingsModal()`, same
      class of stale-focus-group bug the wizard and home-drawer fixes above
      addressed — diagnosed, not yet applied/confirmed.
    - Some apps/overlays don't close via the dedicated Home key — only some do
      (inconsistent per-screen, not a HOME-key dispatch bug given other
      overlays close correctly).
    - Some modals close via Back, some don't — also inconsistent per-modal.
    - The Snake game does not respond to d-pad input at all (confirmed the
      hardware keys themselves work correctly elsewhere) — likely reads input
      through its own loop rather than the shared `handleHwKey()`/nav-group
      system; not yet traced.
    - Map panning: currently no way to pan the map at all with the d-pad
      (arrows only drive UI nav). Idea, not started: use `M9_KEY_ENTER_LONG`
      (now wired and proven via the lock-screen/context-menu work below) to
      toggle between UI-nav mode and map-pan mode.

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
