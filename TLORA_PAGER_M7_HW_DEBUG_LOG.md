# T-LoRa Pager — M7 hardware bring-up debug log

## Session 2 (2026-07-07) — root cause found: floating panel-reset line (XL9555 ch6)

Picked up from session 1's black-screen regression (§11 below). Two desk-side
findings changed the picture entirely:

**Session 1's leading theory (§11, radio/display SPI race) is DISPROVEN, from
source:** TFT_eSPI's `Processors/TFT_eSPI_ESP32_S3.h:41-43` force-defines
`SUPPORT_TRANSACTIONS` ("mandatory for ESP32 so the hal mutex is toggled") —
its raw-register fast path is still bracketed by `spi.beginTransaction()` on
the same HAL mutex RadioLib uses on the shared `SPIClass`. On top of that,
`the_mesh.loop()` and `ui_task.loop()` run on the same task and RadioLib does
no SPI from ISRs, so there is no radio/display SPI concurrency at all. Don't
re-chase this.

**BUG #6 (fixed — believed to be §11's real root cause): the ST7796's hardware
reset line IS wired — to XL9555 channel 6 — and we left it floating.**
Session 1 concluded "no display-specific expander channel exists" from the
LilyGoLib doc's channel table and the canonical arduino-esp32 master
`pins_arduino.h` — both list channels 0-5 and 7-12 and silently omit ch6.
LilyGoLib's own board code (`src/LilyGo_LoRa_Pager.cpp`, `begin()`) defines
`EXPANDS_DISP_RST` (=6, confirmed via forks carrying the newer pins header),
drives it HIGH with the other rails, then pulses it LOW→50ms→HIGH as a real
panel hardware reset before display init. Our port never touched ch6, so the
panel's reset floated at the expander's power-on high-Z default — a floating
active-low reset is exactly the observed failure shape: intermittent black
screen with a clean boot log (a controller held in hardware reset ignores ALL
SPI, including TFT_eSPI's software-reset fallback), no code-change
boot-to-boot variance, and worst behavior on a genuine cold power cycle.
(trail-mate gets away without it because its older vendored pins header lacks
the macro, so its `#ifdef EXPANDS_DISP_RST` blocks compile out — hardware
revisions likely differ in how hard the line floats.)

**Fixes applied this session** (all in the working tree):
- `variants/lilygo_tlora_pager/TLoraPagerBoard.{h,cpp}`: full vendor rail set
  (added DISP_RST ch6, NFC_EN ch5, DRV_EN ch0, AMP_EN ch1, GPS_RST ch7,
  GPIO_EN ch9 to the existing five) — vendor drives everything HIGH, and
  unpowered-but-bus-connected chips (DRV2605 + ES8311 on I2C, ST25R3916 on
  the shared SPI) can clamp a shared bus through their ESD diodes, so
  "off because unused" wasn't safe. Then the vendor's DISP_RST pulse
  (LOW→50ms→HIGH). Also parks the shared-SPI selects/resets OUTPUT-HIGH
  before any bus traffic (LORA_NSS 36, LORA_RST 47, SD_CS 21, NFC_CS 39 —
  LilyGoLib's `initShareSPIPins()` equivalent, which we'd skipped entirely).
- `src/helpers/ui/ST7796LCDDisplay.cpp`: `turnOff()` no longer latches the
  cached brightness at 0 (a `turnOn()` would have restored 0% backlight).
  Latent — UITask's pager screen-off path doesn't use turnOff() — but fixed
  so it can't muddy future testing.

**Flashing note (adds to session 1's recipe):** app-only flashing works and
is what you want for iteration (`write_flash 0x10000 firmware.bin` — leaves
bootloader/partitions/NVS untouched), but the ROM loader needs the explicit
`--flash_mode qio --flash_freq 80m --flash_size 16MB` args even for app-only
writes — without them `--no-stub` fails at erase with
`Failed to enter Flash download mode (result was 01060000)`.

**Open observation (not yet diagnosed):** after long uptime the board emitted
`[STALL] ui:gps 1644ms` continuously (every loop pass), vs. the normal single
~835ms line right after boot. The `ui:gps` checkpoint actually covers
everything from `updateGpsLocation()` to `uiCp("ui:verchk")` — including the
TCA8418 keyboard I2C poll — so repeated 1.6s stalls smell like I2C
transactions timing out (possibly the unpowered-DRV2605/ES8311 clamping
theory above; the full vendor rail set may have fixed this too). Watch for it
after long uptime on the new build; if it recurs, add finer uiCp checkpoints
inside that span.

**Verified this session:** build green, flashed, boot log clean
(`[BOOT] ui ready`, single 836ms gps stall). Awaiting visual confirmation on
the glass + repeated cold-power-cycle testing (the one test that
discriminates: session 1's black screens were cold-boot-worst).

**Session 2 outcome — WORKING, user-confirmed on the glass.** Sequence of
events after the first fix flash, worth keeping straight because it created a
misleading data point:
- User reported "still black" after the first DISP_RST-fix flash, then
  flashed the official MeshCore pager image and confirmed the screen works —
  proving the hardware (glass/backlight/FPC) was fine all along.
- When the board came back to this machine it was **boot-looping every ~2.6s
  with `invalid header: 0xffffffff`** — the flash had NO valid image (the
  MeshCore web-flash was evidently interrupted/incomplete). So the state the
  user was judging was unbootable-erased-flash territory, and the USB port
  was re-enumerating constantly (every esptool/pyserial attempt died with
  `Errno 71` / `could not configure port` until this was understood).
- Reflashed OUR build as the full **merged image at 0x0** (clean slate:
  bootloader + partitions + app, NVS/prefs wiped — appropriate here since
  the MeshCore flash had already destroyed our NVS/partition state). With a
  boot-looping ROM, the flash must catch an alive-window: a simple retry
  loop around esptool connected first try.
- New diagnostics both healthy on the next boot: `[BOOT] xl9555 ok` (the
  expander — and therefore the DISP_RST pulse — really executes on this
  unit), and `[DISP] RDDPM=0xBC` (booster on, sleep-out, display-on read
  back from the ST7796 itself). **User then confirmed the UI is visible:
  "worked!"**
- Caveat for the next session: the working flash differs from the
  still-black flash in TWO ways (DISP_RST fix was in both, but this one is
  also a clean-slate NVS/prefs wipe — and the "still black" observation may
  even have been made against the already-erased flash). If black ever
  returns, the `[DISP]` readback line now discriminates instantly:
  RDDPM=0xBC + black glass = backlight path; RDDPM=0x00/0xFF = panel
  reset/SPI path.
- The `[DISP]` readback + `[BOOT] xl9555` prints are DELIBERATELY left in
  until cold-power-cycle testing passes; remove the `[DISP]` one (marked
  TEMPORARY in ST7796LCDDisplay.cpp) once M7's gates are done.
- Still open: repeated cold-power-cycle test (the historical worst case),
  and the long-uptime repeating `[STALL] ui:gps ~1.6s` observation.

**Final session-2 validation (user, on device):** boot logo → regular
wadamesh UI every time, drove the menus by encoder + keyboard, "nothing to
complain", no perceived lag, encoder detent feel correct (closes tracker
risk 2a — the assumed 4-transitions-per-detent is right for this part).

One unexplained observation to keep an eye on: at one point (after a
user-side reset, before a monitored re-reset) the loop was emitting
`[STALL] ui:lvgl ~265ms` continuously — i.e. near-full-screen LVGL redraws
~4×/s in steady state. Gone after the next reset; user felt no lag while
driving the UI; which screen was up during the stream is unknown. If UI
sluggishness is ever reported, correlate the live `[STALL]` stream with the
on-screen state first — something was invalidating aggressively. (Related
open item from earlier the same day: a long-uptime state where
`[STALL] ui:gps ~1.6s` repeated every pass — that tag's span covers
everything from `updateGpsLocation()` to `uiCp("ui:verchk")` including the
TCA8418 I2C poll, so it may have been I2C timeouts from the then-unpowered
DRV2605/ES8311 clamping the bus; the full vendor rail set landed since. If
it recurs, add finer uiCp checkpoints inside that span.)

---

# Session 1 (original log)

Working notes from the first real-hardware session on the T-LoRa Pager LR1121
board, picking up right after Milestone 6 (UITask wiring) landed on branch
`tlora-pager-port-lr1121`. Not a tracker doc — this is a blow-by-blow record
of what broke, what was tried, what actually got fixed, and what's still
unresolved, so the next session doesn't have to re-derive any of it.

**Status at end of session: UNRESOLVED.** Screen currently shows black after
a full power cycle + reflash of the unchanged binary. Three real bugs got
found and fixed along the way (all believed solid, kept); a fourth, deeper
SPI-sharing issue is the current leading suspect and is NOT yet fixed.

---

## How to pick this back up

1. Re-read this whole file before touching anything.
2. Current hardware state: board is flashed with the build described in
   "Current file state" below. Screen is black; serial boot log is clean
   (reaches `[BOOT] ui ready` + one `[STALL] ui:gps ~700-900ms` line, no
   crash) every time, including immediately after a genuine full power cycle.
3. First thing to try: the "Next diagnostic step" section at the bottom —
   temporarily stub out `radio_init()` (just `return true;` before the real
   body) and reflash, to isolate whether the radio's own SPI activity is what's
   corrupting the display, independent of any boot-order/timing luck.
4. Serial monitoring recipe that actually works in this sandboxed environment
   (PlatformIO's own `pio device monitor` fails here — `termios.error:
   Inappropriate ioctl for device`, no real tty): use a small inline Python
   `pyserial` script to open `/dev/ttyACM0` at 115200 and read for N seconds.
   See any of this session's `Bash` calls for the exact snippet.
5. Flashing recipe: **the canonical, hardware-verified commands now live in
   `CLAUDE.md`'s "T-LoRa Pager" section** (app-only at 0x10000 for iteration —
   preserves NVS/prefs — vs merged at 0x0 for clean-slate/recovery; both
   need `--no-stub --baud 115200` + explicit `--flash_freq 80m
   --flash_size 16MB`, because `pio run -t upload`'s stub handshake fails on
   this board's native-USB-CDC port). Session-1 history: only the merged-image
   form had been discovered at this point.

---

## Chronological account

### 0. Starting point
Milestone 6 was already committed (`641700f`). User wanted to generate a
flashable image for **M5Launcher** (a phone/on-device flasher app) to do the
very first flash of wadamesh onto this physical board, then connect and start
Milestone 7 (headless hardware bring-up).

### 1. Merged binary for M5Launcher
`merge-bin.py`'s `mergebin` PlatformIO custom target already existed in this
repo (bootloader + partition table + boot_app0 + app flattened into one image
at 0x0). Built it, copied to `out/tlora_pager_lr1121_companion_radio_touch-merged.bin`.
Confirmed with the user that the "never flash the merged image" caution in
`CLAUDE.md` is specifically about wiping NVS on a re-flash of an
already-provisioned board — irrelevant here since this was a first-ever flash.

### 2. M5Launcher "install complete" but device didn't reboot
Expected: phone/BLE-based flashers can't toggle EN/GPIO0 like a wired
USB-serial connection can (no DTR/RTS lines to a bootloader stub), so the
chip stays in the ROM download stub until manually reset. User power-cycled
manually. This part was never actually a bug.

### 3. BUG #1 (fixed) — stale NVS BLE-bond data crashing NimBLE init
First real serial capture showed a clean `[BOOT] board ok` immediately
followed by a boot-loop: `Guru Meditation Error: Core 1 panic'ed
(StoreProhibited)`, repeating every ~2s. `addr2line` against
`firmware.elf` resolved the fault to `TFT_eSPI::begin_tft_write()` — but that
turned out to be a RED HERRING for this specific crash; a **second**,
different crash surfaced later at the same investigation stage:
`Stack smashing protect failure!`, backtrace through
`NimBLE-Arduino/.../ble_store_nvs.c:445` (`populate_db_from_nvs`) →
`ble_store_config_init` → `NimBLEDevice::init()` →
`SerialBLEInterface::begin()` → `MultiTransportCompanionInterface::beginBle()`.

Root cause: this board had prior firmware on it (M5Launcher, possibly earlier
test builds) that left BLE bonding records in the **NVS flash partition**
(a region the merged-image flash never touches — it only writes
bootloader/partition-table/app, 0x0–~0x2A0000). Our NimBLE build's compiled-in
bond-array size didn't match what was already stored, overflowing a
fixed-size RAM array while restoring it at boot.

**Fix applied:** full chip erase was blocked by the auto-mode safety
classifier (irreversible-deletion guard, correctly — it would also have wiped
any M5Launcher partition still on the board). Used the **normal (non-merged)
4-component upload** instead (`pio run -t upload`), which — combined with
however PlatformIO/esptool's upload sequence handles the partition regions —
resulted in a much healthier NVS (`nvs_free_entries` went from 182/183 to
549–620 and stayed there). This specific upload attempt itself then failed
over the wire (`esptool` loader-stub handshake issue, see below), which is
what led to discovering the `--no-stub` flashing recipe. Confirmed fixed:
boot log has been clean (no stack-smash, no reboot loop) for the rest of the
session, through many subsequent reflashes.

### 4. Flashing mechanics discovered along the way
- `pio device monitor` doesn't work in this sandboxed/non-tty environment —
  use a `pyserial` script instead (see "How to pick this back up" above).
- `pio run -t upload` (normal path, uses esptool's loader **stub**) fails on
  this board's native-USB-CDC port with `A fatal error occurred: No serial
  data received` right after "Changing baud rate to 921600" — the stub
  handshake doesn't survive the baud change over this specific CDC
  connection.
- Fix: call `esptool.py` directly with `--no-stub --baud 115200` (talks to
  the ROM bootloader the whole time, never hands off to the faster stub).
  Slower (~30s/flash) but reliable every time this session.

### 5. BUG #2 (fixed) — pager had no way to wake the screen once idle-dimmed
After the NVS fix, boot completed cleanly and reached the main loop
(confirmed via `[STALL] ui:gps`/`ui:lvgl` entries firing repeatedly), but the
screen would go dark after the normal idle-timeout and never come back — the
user could see it dim, but no key press or encoder turn revived it.

Root cause: `handleHwKey()` (shared code, used by the T-Deck's keyboard too)
early-returns if the screen is off, and relies on some OTHER input path
(touch, or the T-Deck's trackball) to call `wakeScreen()`. The pager has
neither touch nor a trackball — keyboard + rotary encoder are its ONLY
inputs — so nothing in the T-Deck/Tanmatsu code paths ever called
`wakeScreen()` for it.

**Fix applied** (`src/ui-touch/UITask.cpp`):
- `updatePagerEncoder()`: if the screen is off, any encoder movement or click
  now calls `wakeScreen()` and returns immediately (swallowing that event
  rather than also acting as navigation) instead of falling through to
  `navPushTap()`.
- The `HAS_PAGER_KEYBOARD` drain in `UITask::loop()`: if the screen is off,
  drains the whole keyboard FIFO batch (so nothing queued leaks through as
  real input right after waking) and calls `wakeScreen()` once if anything
  was in it, instead of calling `handleHwKey()` per key.

Confirmed fixed on hardware: encoder/keyboard now reliably wake the display.

### 6. BUG #3 (fixed) — TFT_eSPI ESP32-S3 SPI-port crash on first display write
With the wake fix in, the very first display write (the pre-LVGL boot
wordmark) crashed: `Guru Meditation Error ... StoreProhibited`, backtrace
through `TFT_eSPI::begin_tft_write()` → `writecommand()` → `TFT_eSPI::init()`
→ `ST7796LCDDisplay::begin()` → `main.cpp:setup()`. `EXCVADDR: 0x00000010`.

Root cause, confirmed by reading `TFT_eSPI_ESP32_S3.h`/`.c` and the pulled
ESP-IDF `soc.h`/`spi_reg.h` headers directly: TFT_eSPI's raw register macros
(`_spi_user`, used by `SET_BUS_WRITE_MODE` etc.) compute
`SPI_USER_REG(SPI_PORT) = REG_SPI_BASE(SPI_PORT) + 0x10`, and
`REG_SPI_BASE(i) = (i>=2) ? (DR_REG_SPI2_BASE + (i-2)*0x1000) : 0` — i.e. it
needs the **real IDF host index** (2 or 3), not the Arduino-core `FSPI`/`HSPI`
enum values (0/1 on ESP32-S3). Leaving `USE_HSPI_PORT`/`USE_FSPI_PORT`
undefined makes `TFT_eSPI_ESP32_S3.h` default `SPI_PORT` to the *Arduino*
`FSPI` macro (0) — feeding that into the raw macros above yields address
`0x10`, exactly matching the crash.

**Fix applied** (`platformio.ini`, `tlora_pager_lr1121_companion_radio_touch`
env): added `-D USE_FSPI_PORT=1`. This forces `SPI_PORT=2` (real SPI2)
consistently in both the raw macros and the `SPIClass` object TFT_eSPI
constructs internally. `USE_HSPI_PORT` (real SPI3) was deliberately NOT used
instead, because the radio's own SPI (at the time) defaulted to real SPI3 too
— see bug #4, this turned out to matter a lot more than expected.

Confirmed fixed: boot no longer crashes at the first display write.

### 7. BUG #4 (fixed) — this exact ST7796 panel batch needs INVON
Next: boot proceeded, but the screen showed a **white background with a
black logo** — inverted from the intended dark theme (confirmed the
centering/timing/colors of everything else were otherwise correct).

Root cause: TFT_eSPI's generic `ST7796_Init.h` command table never sends an
inversion command (`0x20`/`0x21`) at all — it leaves the panel at its own
power-on default. Confirmed against **trail-mate's own bespoke (non-TFT_eSPI)
ST7796 driver** for this exact board — their init table explicitly sends
`0x21` (`INVON`), which is why their build never showed this.

**Fix applied** (`src/helpers/ui/ST7796LCDDisplay.cpp`,
`ST7796LCDDisplay::begin()`): added `display.invertDisplay(true);` right
after `display.init()`. TFT_eSPI's `invertDisplay()` sends `TFT_INVON` twice
(per its own code comment, "otherwise it does not always work").

Confirmed fixed: colors correct on the next boot.

### 8. Symptom: correct colors, but frozen on the plain boot mark forever
Boot logs looked perfect (`[BOOT] ui ready`, `[UI] splash dismissed` — both
temporary diagnostic prints added during this investigation, since removed —
fired exactly on schedule), but the user always saw ONLY the plain
pre-LVGL boot mark (no teal dots, no "WADA MESH"/"MESHCOMOD"/"TOUCH BETA"
text, no status bar) — a photo confirmed this directly. Pressing
keys/turning the encoder woke the *backlight* (bug #2's fix working
correctly) but never changed the image.

Added temporary instrumentation to `lvglFlush()` (logged every flush's area
+ `isOn()`) and a temporary per-second direct `fillRect(RED)/fillRect(GREEN)`
canary in the main loop that completely bypassed LVGL. Findings:
- `lvglFlush()` WAS being called dozens of times with varied, correct-looking
  coordinates covering the whole 480×222 screen (status bar, splash text
  region, etc.), always reporting `isOn()==true`.
- The direct canary (bypassing LVGL entirely) ALSO never visibly changed the
  screen, despite firing every second for the whole session.
- Conclusion: software believed every write succeeded; the physical glass
  was not receiving ANY of them, except the very first (pre-`radio_init()`)
  boot-mark paint.

### 9. BUG #5 (fixed, but see §11 — likely incomplete) — shared SPI pins, two separate hosts
This board's radio (`P_LORA_SCLK/MISO/MOSI` = 35/33/34) and display
(`TFT_SCLK/MISO/MOSI` = 35/33/34) use the **identical physical pins** — by
design, only chip-select differs (`LORA_NSS=36` vs `TFT_CS=38`). But
`variants/lilygo_tlora_pager/target.cpp` constructed the radio's OWN, separate
`SPIClass spi;` (Arduino default ctor → real SPI3), while the display (via
`-D USE_FSPI_PORT=1`, bug #3) used TFT_eSPI's own separately-constructed
`SPIClass(FSPI)` (→ real SPI2). Two independent host peripherals wired to the
same GPIO pins is not valid bus sharing on ESP32 — the GPIO matrix's OUTPUT
routing for a pin can only reflect ONE peripheral's signal at a time, and
`radio_init()`'s own `spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI)` call
(which runs AFTER `display.begin()` in `main.cpp`) silently re-routed those
pins' matrix assignment to SPI3 — stealing them from the display's SPI2,
exactly matching every symptom in §8 (software succeeds, glass frozen after
that point).

Compared against the T-Deck's own working pattern
(`variants/lilygo_tdeck/target.cpp`'s `tdeckSharedSPI()`, which returns
`&spi` — the SAME object instance — to whatever else needs the bus). T-Deck
never needed this for its OWN display, though, because its radio and display
are on entirely different pins there; it only shares between radio and SD.

**Fix applied** (`variants/lilygo_tlora_pager/target.cpp`):
- `RADIO_CLASS radio = new Module(..., spi)` → `..., TFT_eSPI::getSPIinstance())`.
  `TFT_eSPI::getSPIinstance()` is a `static` public accessor returning a
  reference to the exact same file-static `SPIClass` object TFT_eSPI
  constructed for the display (safe to capture at global-construction time
  even though the object's own initialization order across translation units
  is technically unspecified, because we only ever *use* it later, inside
  `radio_init()`, well after all global constructors have run).
- Removed target.cpp's own `static SPIClass spi;` and the `spi.begin(...)`
  call inside `radio_init()` — the shared object was already attached to
  those pins by `display.begin()` (which runs earlier in `main.cpp`).
- First attempt at this fix (before finding `getSPIinstance()`) tried adding a
  `ST7796LCDDisplay::reclaimSpiBus()` that called `display.init()` again after
  `radio_init()` — this did **nothing**, because `TFT_eSPI::init()`'s
  `spi.begin()` call is itself guarded by an internal `_booted` flag that gets
  cleared after the first run; calling `init()` a second time silently skips
  the exact re-attachment step needed. That dead-end method was added, then
  removed once the real fix (above) was found.

**Confirmed working, twice, on real hardware:**
1. Right after this fix (with the debug canary still in place): screen
   visibly flashed red/green, alternating, exactly as the canary code
   commanded.
2. After removing ALL temporary debug instrumentation (see §10) and
   reflashing the clean build: user confirmed the actual Wadamesh UI painted
   correctly ("yeah its showing wadamesh! :)").

### 10. Cleanup pass
Removed all temporary debugging code added during §8–9:
- `lvglFlush()`'s per-call `[FLUSH] #N area=... isOn=...` logging.
- The per-second direct-`fillRect` red/green canary in `UITask::loop()`.
- `splashRemove()`'s `[UI] splash dismissed` print.
- The dead-end `ST7796LCDDisplay::reclaimSpiBus()` method (declaration +
  definition) and its call site in `main.cpp`.

**Kept** (judged generally useful, matches the existing `[BOOT] ...`
milestone-line convention, cheap): `Serial.println("[BOOT] ui ready");`
right after `ui_task.begin()` in `main.cpp` — the only diagnostic addition
still present in the current diff.

### 11. Regression: black screen again, survives a full power cycle
After the confirmed-working cleanup build, continued testing/further resets
eventually led to a black screen again. Troubleshooting so far:
- A **soft** reset (via `esptool`'s RTS-pin toggle) reliably reproduces a
  clean, crash-free boot log every time (`mesh ok → wifiConfig ok →
  serial_interface ok → ui ready → STALL ui:gps ~700-900ms`) — indistinguishable
  from the boot that worked.
- A **genuine full power cycle** (USB cable fully unplugged ~10s, replugged)
  was tried on the theory that some external rail (I2C-expander-controlled
  `LORA_EN`/`GPS_EN`/`KB_EN`/`SD_EN` — see `TLoraPagerBoard.cpp`) might stay
  "warm" across soft CPU-only resets but needs settling time on a true cold
  boot. This made things WORSE, not better — black from the very first boot,
  not even the pre-LVGL boot mark. (Note: confirmed there is no
  display/backlight-specific `_EN` channel on the IO expander at all — only
  `KB_RST/LORA_EN/GPS_EN/KB_EN/SD_DET/SD_PULLEN/SD_EN` — so the rail-settle
  theory doesn't actually apply to the display specifically; this avenue is
  likely a dead end, noted here so it isn't re-tried.)
- Reflashing the exact same, unchanged binary again did NOT fix it either —
  rules out "one-off flash corruption."
- Also noted: the physical RESET button does not appear to actually reset the
  board (no boot log appears when the user presses it), while
  software-triggered resets (via `esptool`) do work reliably. Unconfirmed
  whether this is a board wiring quirk or user expectation mismatch — worth
  clarifying next session (which physical button is actually being pressed).

**Current leading theory (unconfirmed):** TFT_eSPI's actual pixel-pushing
implementation does NOT go through the shared `SPIClass` object's own
mutex-protected `transfer()`/`beginTransaction()` path — it pokes the SPI
peripheral's hardware registers directly for speed (the same `_spi_cmd`/
`_spi_user`/`_spi_mosi_dlen` raw-register macros from bug #3). RadioLib, using
the same shared object, DOES go through its normal mutex-protected calls. So
even with one shared C++ object (bug #5's fix), TFT_eSPI's fast path bypasses
the very locking meant to keep two devices off the bus simultaneously — if
the radio's own SPI activity (e.g. polling IRQ/status registers) lands at the
wrong instant relative to a display write, they can corrupt each other at the
hardware level. This would explain intermittent, timing-dependent
success/failure with NO code differences between runs — consistent with
everything observed in §11. **Not yet confirmed or fixed.**

---

## Current file state (uncommitted, on branch `tlora-pager-port-lr1121`)

```
 M TLORA_PAGER_PORT.md                    (tracker doc -- pre-existing M6 update, keep out of commits)
 M TLORA_PAGER_PORT_MILESTONES.md         (tracker doc -- pre-existing M6 update, keep out of commits)
 M platformio.ini                         (+19: -D USE_FSPI_PORT=1 + comment, bug #3)
 M src/helpers/ui/ST7796LCDDisplay.cpp    (+11: display.invertDisplay(true), bug #4)
 M src/main.cpp                           (+1: "[BOOT] ui ready" diagnostic print, kept from investigation)
 M src/ui-touch/UITask.cpp                (+39/-8: pager screen-wake fix, bug #2 -- clean, confirmed working)
 M variants/lilygo_tlora_pager/target.cpp (+24/-3: shared SPIClass fix, bug #5 -- confirmed working twice, but §11 regression suggests incomplete)
```

All of the above (except the two tracker `.md` files, per standing
instruction) are believed-good fixes worth keeping and eventually committing
— they are NOT the cause of the current black-screen regression (the
cleaned-up build with all of them applied was directly confirmed working on
hardware before the regression in §11 appeared). Milestone 6's own commit
(`641700f`) is untouched/already landed separately.

## Next diagnostic step (where to start next session)

Temporarily stub `radio_init()` in `variants/lilygo_tlora_pager/target.cpp` to
just `return true;` before its real body (radio won't actually work, but
nothing will touch the shared SPI bus except the display), reflash, and see
if the display then works **reliably, every single boot, including after a
full power cycle** with zero radio activity:
- If yes, every time → confirms the radio/display SPI race in §11's theory;
  next step is figuring out how to serialize them properly (e.g. wrapping
  RadioLib's SPI calls or TFT_eSPI's raw pokes with an explicit shared mutex,
  or checking whether RadioLib's IRQ-driven reads can be deferred out of any
  window where a display flush might be in flight).
- If it's STILL intermittent with the radio silent → the theory is wrong,
  and the real cause is elsewhere (worth re-examining the power-rail-timing
  angle more carefully despite §11's evidence against it, or looking at
  something else entirely, e.g. PSRAM/heap pressure, watchdog interaction,
  or the AW9364 backlight driver's own state machine).
