# T-LoRa Pager port — working tracker

Goal: run the **full** wadamesh UI + functionality on the **LilyGo T-LoRa Pager**
(ESP32-S3, **LR1121** radio variant first), kept as **one codebase** with the
existing boards so a UI change ships everywhere at once. This file is the running
plan/status — update it as we go.

Status: **M7's UI portion DONE — boot logo, full LVGL UI, QWERTY
keyboard nav and rotary encoder all VERIFIED WORKING on real hardware
(2026-07-07)**, after root-causing the intermittent black screen to the ST7796
panel's hardware-reset line floating: it's wired to **XL9555 channel 6**, which
both the LilyGoLib doc's channel table and the canonical arduino-esp32 pins
header omit (full war story: `TLORA_PAGER_M7_HW_DEBUG_LOG.md`, session 2 —
including why session 1's radio/display "SPI race" theory was disproven from
TFT_eSPI source). Remaining hardware-comms gates (radio vs a live mesh node —
adverts/ACKs, upstream LR1121 ACK issue #1376; USB companion device-profile
frame; SD/SPIFFS storage) split out to **Milestone 7b** (deferred, needs a
second mesh node + human) so they don't block UI work. **Milestone 8
(on-device UI pass) is now active**: fixing weird UI/keyboard behaviors found
by manual testing on the device, human supplying photos/repro steps. All
three envs build green — Milestone ⑥ (UITask wiring)
landed; the pager is a first-class, non-touch UI target. Board JSON verified
(`boards/lilygo-t-lora-pager.json`). Milestones ①–⑥ (variant skeleton, LR1121
radio glue, ST7796 display + AW9364 backlight, platformio.ini env, TCA8418
keyboard + rotary encoder drivers, UITask wiring) landed.
`[env:tlora_pager_lr1121_companion_radio_touch]` compiles clean: **RAM 23.8%
(78088/327680 B), Flash 66.4% (2696465/4063232 B)** — both still comfortably
lower than the two shipping boards (Heltec V4/T-Deck: RAM 25.3%, Flash 73.5%,
unchanged — confirmed byte-identical to pre-M6, zero regression). M1–M5's
build-blocking fixes (variant/pins split, radio-wrapper include paths, the
minimal display-typedef addition) are recorded in Decisions ②/⑧ and worklist
③/⑤ below; M6's own findings follow.

**M6 findings** — the UI-inventory table + worklist ⑥ below have full detail;
the highlights:
- **Two capability flags I initially set were premature and had to be walked
  back**: `CAP_SD`/`CAP_FILESYSTEM` looked right from the hardware table (the
  pager does have a microSD slot), but the actual mount code
  (`fmSdTryMount()`, `#include <SD.h>`) is still hardcoded to
  `HAS_TDECK_GT911` specifically — `device_caps.h`'s own `CAP_SD` flag was
  never actually wired to it. Turning it on for the pager just produced
  `SD`/`CARD_NONE`/`fmSdTryMount` "not declared" errors, not real SD support.
  Set back to 0 — a real mount needs its own pager-specific wiring (CS 21, a
  non-T-Deck shared-SPI helper), unscheduled follow-up, not this milestone.
- **`navMaybeRebuild()` — the function that actually populates the LVGL focus
  group every screen — was unreachable for the pager** in my first pass: it's
  only called under `#if defined(HAS_TANMATSU) ... #elif CAP_TRACKBALL ...`,
  neither of which the pager matches. Without a fix here the KEYPAD indev
  would have an eternally-empty focus group — nothing focusable, navigation
  completely dead, despite the indev registration itself being correct. Added
  a `#elif defined(TLORA_PAGER)` arm.
- **A self-inflicted `#elif` scoping mistake**: `handleHwKey()` (and several
  helpers it calls) live inside a large, multiply "paused and reopened"
  `#if defined(HAS_TDECK_KEYBOARD)` region. Widening the wrong reopen point
  first orphaned ~50 lines of genuinely T-Deck-specific code (keyboard
  backlight-mode timer, notify-flash, spacebar-lock countdown) into what was
  meant to be the pager's own simpler branch. Fixed by tracing each reopen's
  actual `#endif` (nesting depth, not just grep hits) before touching it, and
  keeping the T-Deck-only pieces under their own unwidened, more specific gate.
- **`isDismissKey()`'s T-Deck logic doesn't transfer**: it treats the letters
  p/q/a as "dismiss popup" because the T-Deck's sparse keyboard has no
  dedicated Esc key. The pager's full QWERTY types those letters constantly —
  reusing that mapping would eat normal typing. Returns `false` for the pager
  instead; the rotary encoder's long-press already covers ESC.
**Execution playbook: `TLORA_PAGER_PORT_MILESTONES.md`** — the worklist below,
broken into agent-executable milestones with gates and per-file instructions.

---

## Why this port is cheaper than it looks

The scary part — "the device has no touchscreen" — is **already solved in this
codebase**. UITask carries a complete non-touch navigation layer built for the
Tanmatsu (keypad-only) and the T-Deck trackball D-pad mode:

- **Focus-group nav**: `s_nav_group` (`lv_group_t`), amber focus ring + scroll-into-view
  (`navFocusCb`), per-screen group rebuild (`navMaybeRebuild`), 2-D directional focus
  (`navMoveDir`), tab-bar handling (`navOnTabBar`/`navSwitchTab`) — `UITask.cpp` ~2138–3128.
- **A KEYPAD indev + key FIFO**: `navFifoPush/Pop` feeds `LV_KEY_UP/DOWN/LEFT/RIGHT/
  NEXT/PREV/ENTER/ESC` into `tanmatsuKeypadRead` (`UITask.cpp` ~3000). The Tanmatsu
  registers ONLY this indev (no pointer) — `UITask.cpp` ~35706–35717. **That branch is
  the pager's template.**
- **Physical-keyboard routing**: `handleHwKey()` (`UITask.cpp` ~27990) routes keys
  into the focused textarea (edit mode) or into nav (navigate mode, `s_nav_ta_editing`
  flag), with the on-screen LVGL keyboard suppressed — exactly how the T-Deck works
  today. The pager's QWERTY plugs into this unchanged.
- **Rotary encoder**: no `LV_INDEV_TYPE_ENCODER` needed — encoder ticks map to
  `navFifoPush(LV_KEY_NEXT/PREV)` (focus walk), press → `ENTER`, long-press → `ESC`.
  Reuses everything above.

So the genuinely NEW work is: the **480×222 ST7796 display driver + layout pass**,
the **TCA8418 keyboard driver** (raw matrix → chars, unlike the T-Deck's C3 which
resolves ASCII for us), the **rotary driver**, the **LR1121 radio glue**, and a
**board class** whose battery/power goes through I²C chips (fuel gauge + IO
expander) instead of an ADC pin.

## Hardware / platform facts

Confirmed from the LilyGo product page, CNX-Software (2025-08-12), Meshtastic docs,
and cross-checked against three working/authoritative sources: upstream
`meshcore-dev/MeshCore`'s pager target (see caveat under Decision ②), the official
[LilyGoLib hardware page](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/hardware/lilygo-t-lora-pager.md),
and `~/dev/trail-mate` (local project with a running LR1121 pager build — pin map
below is from its `variants/lilygo_tlora_pager/pins_arduino.h`, which is
byte-identical on every pin/channel to the canonical
[`espressif/arduino-esp32` pins_arduino.h](https://github.com/espressif/arduino-esp32/blob/master/variants/lilygo_tlora_pager/pins_arduino.h)
for this board). **Caveat**: the LilyGoLib page's own "Pins Map" table has
internal inconsistencies for the XL9555 channel assignments (e.g. it lists
keyboard-enable at ch10 and SD-detect/enable at ch12/ch14, which contradicts
its own "PowerManage Channel" table on the same page *and* the canonical
arduino-esp32 header). We treat the arduino-esp32 header + trail-mate (which
agree with each other and with the "PowerManage Channel" table) as ground
truth — that's what our own `TLoraPagerBoard.cpp` already uses.

| | |
|---|---|
| SoC | ESP32-**S3** @ 240 MHz, 16 MB flash (QIO), **8 MB QSPI PSRAM** (`memory_type: qio_qspi`) |
| Display | 2.33" IPS **ST7796U**, 480×222 (221 PPI, 262K colors, 450 cd/m²), **480×222 landscape**, SPI, **no touch**. CS 38, DC 37, backlight 42 (AW9364 16-level stepped driver). **Hardware reset is XL9555 ch6, NOT unwired** — `TFT_RST=-1` only because it isn't an ESP GPIO; `TLoraPagerBoard::begin()` owns the reset pulse (LOW→50ms→HIGH, vendor sequence). Leaving ch6 floating = intermittent black screen, clean boot log (M7 root cause) |
| Shared SPI bus | **SCK 35, MOSI 34, MISO 33** — display + LoRa + SD + ST25R3916 NFC all on it (like the T-Deck's 40/41/38 — solved pattern, CS discipline + SPI transactions) |
| Radio | **LR1121** (sub-GHz 830–945 MHz + 2.4 GHz; we use sub-GHz only). CS 36, RST 47, BUSY 48, IRQ/DIO1 14. **Also sold with SX1262** (the LilyGoLib page documents the SX1262 retail SKU as primary) — same board/pins, different defines (cheap 2nd env later) |
| Keyboard | Physical QWERTY via **TCA8418** I²C matrix controller (addr `0x34`), INT 6, backlight 46. Raw matrix events — keymap/shift/sym handled on our side |
| Encoder | Rotary A 40, B 41, **press 7** |
| Buttons | **BOOT = GPIO0** (usable as user button + sleep wake — matches both existing boards' `PIN_USER_BTN=0`). Physical power key is PMU QON, not a GPIO — can only wake the device (1s hold), never programmable |
| Power | **BQ25896** charger PMU (addr `0x6B`) + **BQ27220 fuel gauge** (addr `0x55`, battery % / mV over I²C — NOT an ADC divider). Battery: 3.7 V / 1500 mAh (5.55 Wh). DeepSleep ≈530 µA, LightSleep ≈2.26 mA, Power-off ≈26 µA |
| IO expander | **XL9555** (addr `0x20`): DRV_EN ch0, AMP_EN ch1, KB_RST ch2, LORA_EN ch3, GPS_EN ch4, NFC_EN ch5, **DISP_RST ch6 — the ST7796 panel's hardware reset** (missing from BOTH the LilyGoLib doc's channel table and arduino-esp32 master's pins header; confirmed from LilyGoLib's `LilyGo_LoRa_Pager.cpp` begin(), which pulses it LOW→50ms→HIGH before display init — leaving it floating was M7's intermittent-black-screen root cause, see `TLORA_PAGER_M7_HW_DEBUG_LOG.md` session 2), GPS_RST ch7, KB_EN ch8, GPIO_EN ch9, SD_DET ch10, SD_PULLEN ch11, SD_EN ch12 |
| GPS | u-blox **MIA-M10Q**: TX 12, RX 4, PPS 13 |
| I²C bus | SDA 3, SCL 2 — shared by TCA8418 (`0x34`), XL9555 (`0x20`), BQ25896 (`0x6B`), BQ27220 (`0x55`), PCF85063 RTC (`0x51`), BHI260AP IMU (`0x28`), DRV2605 haptics (`0x5A`), ES8311 codec (`0x18`) |
| SD | microSD on the shared SPI bus, CS 21, card-detect via expander (ch10), max 32 GB, **FAT32 only** |
| Audio | ES8311 codec (I2S 10/11/17/18/45) driving an **NS4150B** 3 W Class-D amp (enabled via expander AMP_EN ch1) |
| Misc | PCF85063A RTC (INT 1), BHI260AP IMU (INT 8), ST25R3916 NFC (unused — CS 39, INT 5, powered via expander NFC_EN ch5), USB VID/PID `0x303A:0x82D4` |

## Board JSON — VERIFIED ✅ (landed as `boards/lilygo-t-lora-pager.json`)

The JSON (Meshtastic-lineage; byte-identical to trail-mate's copy) is **correct for
the LR1121 unit but radio-agnostic**: it describes only the S3 module (flash/PSRAM/
USB id/CDC-on-boot), which every radio variant of the pager shares. The radio is
selected by our build flags (`RADIO_CLASS`/`WRAPPER_CLASS` + pins), same as the
existing boards. Notes:

- `partitions: app3M_fat9M_16MB.csv` is just the default — we override with our own
  OTA+tiles+spiffs csv via `board_build.partitions` (see worklist ①).
- `variant: lilygo_tlora_pager` + `variants_dir: variants` → needs
  `variants/lilygo_tlora_pager/pins_arduino.h` (write our own; don't copy
  trail-mate's — it brands `USB_PRODUCT "TRAIL MATE"`).
- `-DARDUINO_USB_MODE=1` (HW-CDC) is in the JSON's extra_flags. The T-Deck ships
  MODE=1 fine; the Heltec V4 regressed on it (large companion frames dropped —
  see the note in `platformio.ini`). **Verify the device-profile frame over USB
  companion early** (worklist ⑦); if it drops bytes, switch to TinyUSB CDC like
  the V4.

## Decisions (architecture)

1. **Normal PlatformIO env, T-Deck model** — NOT the Tanmatsu IDF-subproject route.
   The pager is a plain ESP32-S3 Arduino target; it slots into `platformio.ini`
   next to the existing two envs and into `release.sh`'s env list later.
2. **LR1121 wrappers vendored in the variant dir — zero core-fork churn for
   bring-up.** The core fork (`core-v1.16.5`) only has `CustomLR1110*`.
   **Re-verified 2026-07-06: upstream `meshcore-dev/MeshCore`'s `main` branch does
   NOT currently have `CustomLR1121*` or a `variants/lilygo_tlora_pager/` dir**
   (checked `src/helpers/radiolib/` — only `CustomLR1110{,Wrapper}.h` exists there
   too; issue [meshcore-dev/MeshCore#861](https://github.com/meshcore-dev/MeshCore/issues/861)
   "Support for LR1121" is still open). Earlier research that assumed an
   upstream crib source was wrong or looked at a branch/fork that no longer
   exists — **re-check upstream at Milestone ② time**, but plan for having to
   author `CustomLR1121{,Wrapper}.h` ourselves by adapting the core fork's own
   `CustomLR1110{,Wrapper}.h` pair (same RadioLib `LR11x0` family — swap the
   base type from `LR1110` to `LR1121`), cross-checked against trail-mate's
   `initLoRa()` (which drives RadioLib's stock `LR1121` class directly, no
   custom wrapper) for the RF-switch table / `setTCXO` sequence. Since
   `CustomLR1121{,Wrapper}.h` only subclass RadioLib's `LR1121` and the core's
   `RadioLibWrapper` (both on the include path), they can live in
   `variants/lilygo_tlora_pager/` for now and move into the fork at the next
   `core-*` tag. Precedent: the Tanmatsu keeps its whole radio bridge in its
   variant dir.
   **Found at Milestone ④'s first real compile**: `CustomLR1121Wrapper.h`'s
   bare quoted `#include "RadioLibWrappers.h"`/`"LR11x0Reset.h"` (copied
   verbatim from `CustomLR1110Wrapper.h`'s shape) don't resolve — those work
   in the core fork only because the file sits in the *same*
   `src/helpers/radiolib/` directory as its targets (quote-include searches
   the including file's own directory first); ours lives in the variant dir
   instead. Fixed to angle-bracket `<helpers/radiolib/...>` includes, matching
   `target.h`'s already-correct pattern for the same headers.
3. **LR1121 init is explicit** (no `std_init`): RF-switch table on **DIO5/DIO6**
   (`STBY {L,L} / RX {L,H} / TX {H,L} / TX_HP {H,L}`) + **`setTCXO(3.0f)`** —
   confirmed in both trail-mate (`boards/tlora_pager/src/tlora_pager_board.cpp`,
   `initLoRa()`) and upstream. Sync word / preamble / CR come from the same
   NodePrefs plumbing as the other boards so it interoperates with the mesh.
4. **Display = new `ST7796LCDDisplay` app-side** (in `src/helpers/ui/`), implementing
   the core's `DisplayDriver` interface (`begin/width/height/startFrame/endFrame/
   setDisplayRotation/writePixelsRGB565`) — that's all the LVGL flush path uses.
   TFT_eSPI has `ST7796_DRIVER` (`USER_SETUP_LOADED` + `-D` pin set).
   **Correction, landed with Milestone ③**: "the Heltec V4 already builds on
   TFT_eSPI, so mirror that wiring" was wrong — `ST7789LCDDisplay` (Heltec
   V4's *and* T-Deck's display class, same file) is actually Adafruit_GFX/
   Adafruit_ST7789-based; Heltec V4's TFT_eSPI lib_dep/`-D` flags are
   vestigial (nothing else in the repo `#include`s `TFT_eSPI.h`). This makes
   `ST7796LCDDisplay` the first real TFT_eSPI consumer in this codebase —
   every method call was verified directly against the pinned
   `bodmer/TFT_eSPI @ ^2.5.43` source, not cribbed from the sibling class.
   **Also found and mitigated**: this panel's 222px glass is narrower than
   the ST7796 controller's 320px GRAM, requiring a 49px column/row offset
   that TFT_eSPI only applies automatically when `-D CGRAM_OFFSET=1` is set —
   missing from this decision's original flag list; a `#error` guard in
   `ST7796LCDDisplay.cpp` now catches the omission at M4 compile time (see
   worklist ③ for the full writeup). Backlight is the AW9364 (stepped pulse
   dimming), not a plain GPIO PWM — wrapped in `ST7796LCDDisplay` itself using
   `lewisxhe/SensorLib`'s `AW9364LedDriver` directly (a maintained library,
   already an M1-established dependency) rather than a hand-rolled driver.
5. **Input = the Tanmatsu registration branch** (KEYPAD indev + `s_nav_group` only,
   no pointer indev), gated by a new device cap. Rotary → nav FIFO; TCA8418 →
   `handleHwKey()`. New pollable drivers in `src/helpers/input/` following the
   existing style (begin/poll/read API, no LVGL inside the driver).
6. **Battery/power via libraries, not hand-rolled** (CONTRIBUTING rule): lewisxhe
   **SensorLib** covers BQ27220 (`GaugeBQ27220`), XL9555 (`ExtensionIOXL9555`),
   PCF85063, DRV2605; **XPowersLib** covers the BQ25896. `TLoraPagerBoard :
   public ESP32Board` overrides `getBattMilliVolts()` (gauge query),
   `getManufacturerName()`, power-rail bring-up in `begin()` (expander), and sleep.
7. **One codebase**: all pager-specific UI behavior rides existing/new `CAP_*`
   flags in `src/ui-touch/device_caps.h` — no forked screens.
8. **`variants/lilygo_tlora_pager/` app glue and the board's Arduino "variant"
   pin map had to split into two directories** — found only by actually
   compiling in Milestone ④, not anticipated by any earlier decision.
   PlatformIO's arduino-esp32 build script (`platformio-build.py`) always
   compiles every source file under `board_build.variants_dir/<board.variant>/`
   as a separate `FrameworkArduinoVariant` library, in a build context with
   none of our app's `lib_deps` include paths. T-Deck/Heltec V4 never hit this
   because their board JSONs point `"variant"` at a variant the *framework*
   already bundles (`esp32s3`, `heltec_v4`) — completely separate from our own
   `variants/lilygo_tdeck/`/`variants/heltec_v4/` app-glue directories, zero
   collision. The pager has no framework-bundled variant, so its board JSON
   was forced to set `"variants_dir": "variants"`, pointing PlatformIO's
   variant resolution at the exact same directory we'd used for
   `TLoraPagerBoard.*`/`target.*`/`CustomLR1121*` — so the framework tried to
   compile those too, and failed on missing `Wire.h`/`RadioLib.h`. Fixed by
   moving `pins_arduino.h` alone into a new `variants/lilygo_tlora_pager_pins/`
   folder and pointing the board JSON's `"variant"` there instead; our own
   `TLoraPagerBoard.*`/`target.*`/`CustomLR1121*` stay in
   `variants/lilygo_tlora_pager/`, reached only via our own
   `build_src_filter`, exactly once. Confirmed safe against
   `get_partition_table_csv()`'s similar `variants_dir`-based fallback logic
   (moot here since `board_build.partitions` is set to a full explicit path,
   which that function returns verbatim once its variant-relative lookup
   attempts fail).

## Worklist (ordered, each step ends build-green for ALL envs)

- [x] ⓪ Research + board JSON verification; land `boards/lilygo-t-lora-pager.json` + this tracker.
- [x] ① **Variant skeleton**: `variants/lilygo_tlora_pager/{pins_arduino.h, TLoraPagerBoard.h/.cpp}` + `partitions_tlora_pager_touch.csv` (T-Deck's OTA/tiles/spiffs/coredump layout, byte-identical offsets). Board class (`TLoraPagerBoard : public ESP32Board`): `begin()` re-inits Wire on SDA3/SCL2, probes the XL9555 expander (addr 0x20) and enables LORA_EN/GPS_EN/KB_EN/KB_RST/SD_EN rails, probes the BQ27220 gauge, handles the deep-sleep RX-packet wake reason (mirrors `TDeckBoard.cpp`). `getBattMilliVolts()` reads the gauge (`refresh()` + `getVoltage()`), falling back to 3700 mV if the probe/refresh fails. `getManufacturerName()` → "LilyGo T-LoRa Pager". `enterDeepSleep()` mirrors `TDeckBoard.h`. BQ25896 charger deliberately left as a TODO (gauge alone covers the UI). `target.{h,cpp}` deferred to ② — nothing references the new files yet, so both shipping envs build unchanged (verified green).
- [x] ② **Radio**: `variants/lilygo_tlora_pager/{CustomLR1121.h, CustomLR1121Wrapper.h, target.h, target.cpp}`.
  `CustomLR1121{,Wrapper}.h` authored by adapting the core fork's own
  `CustomLR1110{,Wrapper}.h` (LR1110/LR1121 share RadioLib's `LR11x0` base —
  confirmed by reading RadioLib 7.6.0 source directly: same protected
  `freqMHz`/`spreadingFactor` members, same `getIrqStatus()`/`getRssiInst()`
  inherited from `LRxxxx`/`LR11x0`), since upstream has nothing to crib (see
  Decision ②). `radio_init()` in `target.cpp`: `spi.begin(...)` →
  `radio.begin(LORA_FREQ, LORA_BW, LORA_SF, cr, ..._SYNC_WORD_PRIVATE,
  LORA_TX_POWER, 8, 3.0f)` → `setRfSwitchTable(DIO5/DIO6)` → `setCRC(1)`.
  **Three deliberate deviations from trail-mate, found by reading RadioLib's
  actual source rather than copying its call sequence — see Decision ② for
  the full reasoning:**
  1. No explicit `radio.reset()` before `begin()` — RadioLib's
     `LR11x0::modSetup()`→`findChip()` already resets the chip internally
     (with retries); trail-mate's explicit reset is redundant, not wrong.
  2. No second `radio.setTCXO(3.0f)` call after `begin()` — trail-mate only
     needs that because its `initLoRa()` calls the *zero-arg* `begin()`
     (default `tcxoVoltage=1.6V`) and fixes it up after. Our `begin()` passes
     `3.0f` as the 8th arg directly, which `LR11x0::modSetup()` already
     applies internally — a second call would be a no-op.
  3. Added `radio.setCRC(1)` after `begin()` (LR11x0's `begin()` defaults to a
     2-byte CRC) to match the 1-byte CRC every other MeshCore radio wrapper
     uses for wire-protocol interop (`CustomSX1262::std_init()` makes the
     identical override) — trail-mate never needed this since its app isn't
     interoperating with MeshCore's own framing.
  Also **not** calling `rtc_clock.begin(Wire)` in `radio_init()` — see the new
  Risk item below (RTC address collision). `RfSwitchMode_t`/`OpMode_t`/DIO5-6
  constants verified to exist with the expected shape directly in the pinned
  `jgromes/RadioLib @ ^7.6.0` source (not just trusted from trail-mate).
  Env defines for M4 to use: `RADIO_CLASS=CustomLR1121`,
  `WRAPPER_CLASS=CustomLR1121Wrapper`, `P_LORA_NSS=36 / _RESET=47 / _BUSY=48 /
  _DIO_1=14`, SPI 35/34/33, **`PIN_GPS_RX=12` / `PIN_GPS_TX=4`** (see the GPS
  risk item below — these are swapped relative to trail-mate's raw
  `GPS_RX`/`GPS_TX` macro values, on purpose), `GPS_BAUD_RATE=38400`. Keep the
  standard `RADIOLIB_EXCLUDE_*` set (LR11X0 stays IN; can also exclude SX126X
  here).
- [x] ③ **Display**: `src/helpers/ui/ST7796LCDDisplay.{h,cpp}` on TFT_eSPI
  (`ST7796_DRIVER`, `TFT_WIDTH=222`, `TFT_HEIGHT=480`, MADCTL rotation for
  landscape). This is the **first real TFT_eSPI consumer in this codebase** —
  `ST7789LCDDisplay` (the sibling this was "modeled on") turned out to be
  Adafruit_GFX/Adafruit_ST7789-based for both Heltec V4 and T-Deck; the
  Heltec V4 env's TFT_eSPI lib_dep/`-D` flags are vestigial (nothing
  `#include`s `TFT_eSPI.h` anywhere else in the repo). Only the
  `DisplayDriver`-satisfying shape was mirrored, not any API calls — every
  TFT_eSPI method used (`init`, `setRotation`, `setAttribute`/`CP437_SWITCH`,
  `textWidth`, `pushColors`, `writecommand`/`TFT_DISPOFF`, `fillScreen`,
  `fillRect`/`drawRect`/`drawPixel`, `setAddrWindow`/`startWrite`/`endWrite`)
  was verified directly against the pinned `bodmer/TFT_eSPI @ ^2.5.43` source
  in `.pio/libdeps/*/TFT_eSPI/TFT_eSPI.h`, not assumed.
  **Found and fixed a real correctness bug in the same pass**: this panel's
  222px glass is narrower than the ST7796 controller's 320px GRAM (trail-mate
  applies explicit 49px column/row offsets — confirmed the source of the
  magic number: `320 - 222 = 98`, halved/centered = 49px each side). TFT_eSPI's
  own `ST7796_Rotation.h` already has this exact fix (`colstart=49`/
  `rowstart=49` depending on rotation, applied automatically inside every
  `setAddrWindow()` call) — but only when `CGRAM_OFFSET` is `#define`d.
  Unlike `ST7789_Defines.h` (which self-defines it), `ST7796_Defines.h` does
  not, and it was **missing from this milestone's own drafted M4 flag list**.
  Without it the build compiles clean but every frame renders shifted/cropped
  by 49px with no error — added a `#error` guard in the new `.cpp` that fires
  the moment M4 compiles this file without the flag, with the fix spelled out
  in the message. **M4 must add `-D CGRAM_OFFSET=1`.**
  Backlight: AW9364 stepped pulse-dimmer, wrapped directly in
  `ST7796LCDDisplay` (no separate `Aw9364Backlight.{h,cpp}` — `ST7789LCDDisplay`
  turned out to have no brightness hook to mirror at all; brightness on the
  other two boards is a `UITask.cpp`-owned free function doing raw LEDC PWM
  on `PIN_TFT_LEDA_CTL`, which the AW9364's pulse protocol can't use).
  Consumes `lewisxhe/SensorLib`'s `AW9364LedDriver` directly (header-only,
  already vendored via the M1-established SensorLib dependency) rather than
  hand-rolling the pulse timing the milestone doc originally suggested.
  Exposes `setBrightness(uint8_t pct)`/`getBrightness()` (0-100, matching the
  Settings UI's existing convention) so Milestone 6 can wire it in with one
  line. **M6 needs a new branch ahead of `UITask.cpp`'s existing
  `PIN_TFT_LEDA_CTL` PWM branch** — once M4 defines that macro for the pager,
  the existing LEDC-PWM code would also compile and fight the AW9364's pulse
  protocol (a duty cycle is not a valid input to this chip). `NOTICE` updated:
  added a `SensorLib` entry (missing since M1) and corrected the `TFT_eSPI`/
  `Adafruit GFX` lines' backend descriptions (both were wrong about which
  boards use which library). `DISPLAY_CLASS=ST7796LCDDisplay`. Gate: both
  shipping envs build unchanged (neither's `build_src_filter` references
  `helpers/ui/*.cpp` yet, so the new TU isn't even parsed by either compiler
  today) — full compiler verification of the new code waits for M4.
- [x] ④ **Env**: `[env:tlora_pager_lr1121_companion_radio_touch]` in
  `platformio.ini`, cloned from the T-Deck env with board/radio/display/GPS/
  input deltas (see Decisions ①-④, ⑥-⑧ for exact values and reasoning). Added
  lib_deps: `bodmer/TFT_eSPI @ ^2.5.43`, `adafruit/Adafruit TCA8418 @ ^1.0.2`,
  `lewisxhe/SensorLib @ 0.3.3` (exact version trail-mate proves works, not
  guessed) — **dropped** `adafruit/Adafruit ST7735 and ST7789 Library` from
  the cloned list (T-Deck/Heltec's Adafruit display backend; unused here,
  TFT_eSPI replaces it). No `XPowersLib` (M1 left the BQ25896 charger out of
  scope). `default_envs` left unchanged (still just the two shipping boards).
  Also fixed two structural bugs found only by actually compiling (see
  Decision ⑧) and added one forced, minimal `UITask.cpp` `#elif
  defined(TLORA_PAGER)` arm (display-class typedef only — see the Status
  line). **Compile gate: all three envs build green** — verified.
- [x] ⑤ **Input drivers**: `src/helpers/input/{PagerKeyboard,PagerEncoder}.{h,cpp}`.
  Gated `HAS_PAGER_KEYBOARD`/`HAS_PAGER_ENCODER`, already reached by every
  env's `+<helpers/input/*.cpp>` filter — no `platformio.ini` change needed
  for compilation, only the pin flags (`KB_INT=6`, `KB_BACKLIGHT=46`,
  `ROTARY_A=40`, `ROTARY_B=41`, `ROTARY_C=7`, matching the repo's
  explicit-`-D`-alongside-`.cpp`-fallback convention).
  **`PagerKeyboard`**: T-Deck's own keyboard driver turned out to be the
  wrong shape to mirror directly — its C3 co-processor resolves ASCII itself
  over I2C, so `TDeckKeyboard.cpp` never sees a raw matrix event. The
  TCA8418 (Adafruit_TCA8418 lib, already an M4 dependency) reports raw
  row/col events instead, so the keymap + shift/sym/alt state machine lives
  in `PagerKeyboard.cpp` itself. Reused trail-mate's `LilyGoKeyboard` keymap
  tables verbatim (same physical PCB) — `keymap[4][10]`/`symbol_map[4][10]`,
  Alt as a hold-to-symbol-layer modifier (no separate physical Symbol key on
  this hardware), Caps as a press-to-toggle case lock, Backspace
  special-cased to `'\b'` — all UX choices trail-mate already field-validated,
  reused rather than re-derived. Ring-buffer/SPSC/threading-contract shape
  mirrors `TDeckKeyboard.cpp`'s (single poll context, UI-thread-safe
  `readKey()`) even though the underlying hardware access differs completely.
  Implemented as straightforward polling (`available()`/`getEvent()` drained
  every `pagerKeyboardPoll()` call), not INT-pin-gated, despite the milestone
  doc's "prefer INT-driven drain" suggestion — trail-mate's own INT handling
  does extra `INT_STAT`/`GPIO_INT_STAT` register bookkeeping whose exact
  clear-vs-latch semantics weren't independently verified, and an ISR-gated
  poll that's wrong would present as "keyboard stops after first keypress" —
  a regression only caught on hardware (M7). Polling is explicitly sanctioned
  as a fallback by the milestone doc and carries no such risk; INT-driven
  draining is a valid future optimization once verified on real hardware.
  **`PagerEncoder`**: the milestone's "ISR edge-counting exactly like
  `TDeckTrackball.cpp:27-47`" doesn't transfer literally — the T-Deck
  trackball is 4 independent direction-pulse GPIOs (no direction logic
  needed, each pin already means one direction), not a true A/B quadrature
  pair, so it can't answer "which way did it turn." Implemented a standard
  Gray-code quadrature transition table instead (both edges of both A and B
  feed one ISR, table lookup yields +1/-1/0 per transition) — same
  ISR-does-cheap-arithmetic-only shape and `noInterrupts()`-snapshot read
  pattern as `TDeckTrackball.cpp`, just the right decode logic for a genuine
  quadrature signal. Divides by `PAGER_ENCODER_STEPS_PER_DETENT` (default 4,
  the common EC11-style ratio) to convert raw transitions to detents,
  carrying the remainder forward across reads — **this divisor is an
  unverified assumption, confirm on hardware in M7**.
  **Gate:** all three envs build — verified.
- [x] ⑥ **UITask wiring**: `device_caps.h` got its `TLORA_PAGER` cap block (no
  touch, no rotate, no large-screen, GPS 1, OTA 1, lock-screen 1; `CAP_SD`/
  `CAP_FILESYSTEM` corrected to 0 — see below) plus widened `CAP_KEYBOARD`/
  `CAP_KEYPAD_NAV` derivations to recognize `HAS_PAGER_KEYBOARD`/
  `TLORA_PAGER`. `UITask.cpp` changes, all pager-gated: KEYPAD indev
  registered by widening the Tanmatsu branch's `#if` to
  `defined(HAS_TANMATSU) || defined(TLORA_PAGER)` (~35706) — pager reuses
  `tanmatsuKeypadRead`'s plumbing but not `bsp_input_get_queue`, which stays
  Tanmatsu-only; forced `s_ui_rotation = LV_DISP_ROT_270` and a new
  `hor_res=480/ver_res=222` branch (~35626); a new `updatePagerEncoder()`
  (delta → `navPushTap(NEXT/PREV)`, click → ENTER, long-press ≥1000ms →
  ESC); main-loop drain wired for both `pagerKeyboardPoll()`/
  `pagerKeyboardReadKey()` → `handleHwKey()` and the encoder function above;
  `applyBrightness()`/`touchScreenBacklight()` got pager-first branches
  calling `display.setBrightness()` instead of falling into the
  `PIN_TFT_LEDA_CTL` PWM branch (closes risk 1f — PWM would never have
  worked on the AW9364's discrete-pulse interface). Draw buffer already
  sized off `hor_res` so needed no separate edit. 222-px vertical audit
  deferred to on-device measurement (M8) rather than guessed constants — see
  risk 8 below. Final sizes: pager RAM 23.8% (78088/327680 B), Flash 66.4%
  (2696465/4063232 B); Heltec V4/T-Deck unchanged at 25.3%/73.5% (byte-
  identical to pre-M6, confirming the gate on every edit). Three corrections
  vs. the milestone doc's assumptions, all found by compiling rather than
  assumed — see risks 1h/1i/1j:
  - `CAP_SD`/`CAP_FILESYSTEM` looked right from the hardware (real microSD
    slot) but the actual mount code is hardcoded to `HAS_TDECK_GT911`, not
    truly `CAP_SD`-generic — set back to 0, real support is unscheduled
    follow-up.
  - `navMaybeRebuild()` (populates the LVGL focus group every screen) wasn't
    reachable on the pager's own cap combination — added a
    `#elif defined(TLORA_PAGER)` arm; without it the KEYPAD indev would have
    had a permanently empty focus group.
  - Several T-Deck-keyboard-gated helpers (`handleHwKey`, `isDismissKey`,
    `tabForKey`, `navMenubarKeysSync`) live inside multiply "paused and
    reopened" `#if defined(HAS_TDECK_KEYBOARD)` regions; widened each
    reopen's gate individually (tracing real nesting depth, not just grep
    hits) rather than touching the whole file, and kept genuinely
    T-Deck-only sub-logic (the p/q/a dismiss-key mapping, which would eat
    normal QWERTY typing on the pager) under its own narrower, unwidened gate.
- [x] ⑦ **Headless bring-up gate, UI portion** (needs hardware) — DONE
  (2026-07-06/07 sessions, see `TLORA_PAGER_M7_HW_DEBUG_LOG.md`): flash +
  serial recipes established; boot clean; SPIFFS mounts; display + boot logo +
  full UI + keyboard nav + encoder verified on the glass (after 6 real bugs —
  stale-NVS BLE bonds, screen-wake path, TFT_eSPI S3 SPI-port index, panel
  INVON, two-SPI-hosts pin theft, and the floating XL9555-ch6 panel reset).
- [ ] ⑦b **Radio/USB/SD bring-up gates** (deferred, needs hardware + second mesh
  node) — radio joins the live mesh (adverts both ways, ACKs — watch upstream
  LR1121 ACK issue meshcore-dev/MeshCore#1376); USB companion link passes the
  large device-profile frame (see HW-CDC note above); SD storage. Split out of
  ⑦ so it doesn't block ⑧'s UI work. Remove the temporary `[DISP]` register
  readback in `ST7796LCDDisplay::begin()` once these pass.
- [~] ⑧ **On-device UI pass** — ACTIVE: nav-coverage audit screen by screen (every interactive control reachable via focus group — the Tanmatsu work paved this), chat layout at 222 px, map pan via encoder/keys, fonts legibility at 480-wide, plus fixing any weird UI/keyboard behaviors surfaced by manual testing (human provides photos/repro steps).
- [ ] ⑨ **Release pipeline**: add the env:binname pair to `release.sh` `ENVS`, flasher manifest (`deploy/flasher/manifest-tlora-pager.json`), OTA env name via `FIRMWARE_OTA_ENV`. Separate PR.
- [ ] ⑩ (Optional, cheap) `tlora_pager_sx1262_...` env for SX1262-variant owners — same board JSON + variant, swap the four radio defines back to the T-Deck's SX1262 set.

## UI-changes inventory (what actually changes in `src/ui-touch/`)

| Area | Change | Size | Status |
|---|---|---|---|
| Indev registration (~35706) | Widened the Tanmatsu keypad-only branch's gate to `\|\| defined(TLORA_PAGER)`; no pointer indev registered | small | done |
| Input drain (main loop ~37350) | Pager branch: `pagerKeyboardPoll()`+`pagerKeyboardReadKey()`→`handleHwKey()`; `updatePagerEncoder()`→`navPushTap(NEXT/PREV/ENTER/ESC)` | small | done |
| `handleHwKey()` (~27990) and its multiply-reopened `HAS_TDECK_KEYBOARD` regions | Widened each reopen's gate individually to admit `HAS_PAGER_KEYBOARD`; kept the T-Deck-only p/q/a dismiss mapping under its own narrower gate | small | done |
| Resolution block (~35604–35650) | New branch: forced `LV_DISP_ROT_270`, `hor_res=480/ver_res=222` | small | done |
| Draw buffer (~1399) | Sized off `hor_res`, no separate edit needed | trivial | done (no-op) |
| Brightness (`applyBrightness`~28906, `touchScreenBacklight`~36511) | New pager-first branches calling `display.setBrightness()` instead of the PWM `PIN_TFT_LEDA_CTL` path | small | done |
| `navMaybeRebuild()` reachability | Added `#elif defined(TLORA_PAGER)` arm — was unreachable, would've left focus group permanently empty | small | done (bug fix) |
| Vertical budget | Audit `STATUSBAR_H`, `TABBAR_H`, `CHAT_KB_H`, modal/chat height helpers for 222 px | **the real work** | deferred to M8 on-device (risk 8) |
| Focus-nav coverage | Screen-by-screen pass that every control is in `s_nav_group` | medium, on-device | deferred to M8 |
| device_caps.h | New `CAP_*` block: no touch, hw keyboard, encoder, 480×222, GPS, OTA, lock-screen; SD/filesystem left 0 (see risk 1h) | trivial | done |
| Keyboard backlight (`pagerKeyboardSetBacklight()`) | Driver function exists (M5) but not wired to any brightness-mode timer or Settings UI | small | deferred |

Everything else (map, chat, contacts, channels, settings, companion protocol,
MQTT, OTA) is resolution-agnostic or already keyed off caps.

## Risks / open questions

1. **LR1121 ACK/TX reliability** — upstream issue meshcore-dev/MeshCore#1376 reports
   ACK problems on the pager's LR1121 (confirmed still open). Track it; our wrapper
   crib should include any upstream fix. Gate ⑦ tests this explicitly.
1b. **No upstream `CustomLR1121` crib source exists yet** (re-verified
   2026-07-06 — see Decision ②'s caveat). Milestone ② needs to author the
   wrapper by adapting the core fork's `CustomLR1110{,Wrapper}.h`, not by
   copying an upstream file. Re-check upstream first in case it lands before
   we get there — would save the work.
1c. **GPS `PIN_GPS_RX`/`PIN_GPS_TX` are named from the GPS module's
   perspective in wadamesh's own core (`EnvironmentSensorManager.cpp` calls
   `Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX)`, and `HardwareSerial::setPins()`
   takes `(rxPin, txPin)` — so `PIN_GPS_TX` supplies the ESP's own **RX** pin,
   `PIN_GPS_RX` supplies the ESP's own **TX** pin). trail-mate's/the canonical
   arduino-esp32 `GPS_RX=4`/`GPS_TX=12` macros are named the OPPOSITE way — its
   own `Serial1.begin(baud, cfg, GPS_RX, GPS_TX)` call uses
   `HardwareSerial::begin()`'s `(rxPin, txPin)` order directly, so there
   `GPS_RX` IS the ESP's own RX pin. **Net result: wadamesh's `PIN_GPS_RX` must
   be set to `12` and `PIN_GPS_TX` to `4`** for M4 — the raw trail-mate values
   swapped, not copied verbatim. Verified by reading both projects' actual
   `Serial1.setPins()`/`begin()` call sites and the ESP32 core's
   `HardwareSerial::setPins()`/`begin()` signatures directly, not by trusting
   either project's macro names at face value. Baud confirmed at 38400 (same
   MIA-M10Q as T-Deck Plus).
1d. **RTC auto-discovery would misread this board's real RTC.**
   `AutoDiscoverRTCClock` (core, shared by all boards) only recognizes DS3231
   (`0x68`), RV3028 (`0x52`), and PCF8563 (`0x51`) — its probe is a bare I2C ACK
   check. This board's PCF85063A sits at that same `0x51` address but has a
   different register layout (RTClib's `RTC_PCF8563` driver would misread its
   registers), so calling `rtc_clock.begin(Wire)` would silently produce
   garbage timestamps instead of a clean fallback. `target.cpp`'s `radio_init()`
   deliberately skips that call — same time behavior as T-Deck/Heltec V4 (ESP32
   software clock) rather than a false "RTC found" that's actually wrong. Real
   PCF85063A support (SensorLib's `SensorPCF85063`) is unscheduled follow-up
   work, not part of any milestone ①–⑩ yet.
1e. **`-D CGRAM_OFFSET=1` must land in Milestone ④'s pager env flags.**
   This panel's 222px glass is narrower than the ST7796 controller's 320px
   GRAM; TFT_eSPI's `ST7796_Rotation.h` only applies the required 49px
   column/row offset when this flag is set (unlike `ST7789_Defines.h`, which
   self-defines it). Without it, the pager build compiles clean but every
   frame renders shifted/cropped by 49px with no error. `ST7796LCDDisplay.cpp`
   has a `#error` guard that will catch the omission the moment M4 compiles
   this file — but the actual fix belongs in `platformio.ini`, and it's easy
   to miss since the build "succeeds."
1f. **M6 must add a pager branch to `UITask.cpp`'s brightness code ahead of
   its existing `PIN_TFT_LEDA_CTL` PWM branch.** Milestone ③ reused that
   macro name for the pager's AW9364 enable pin (naming consistency across
   boards), but `UITask.cpp:~28868`'s `#if defined(PIN_TFT_LEDA_CTL) &&
   (PIN_TFT_LEDA_CTL >= 0)` branch drives that pin with 20kHz LEDC PWM — which
   the AW9364 does not accept (it needs discrete edge pulses per step, not a
   duty cycle). Once M4 defines `PIN_TFT_LEDA_CTL=42` for the pager, that
   existing branch will compile and run for it too unless M6 adds a
   `#if defined(TLORA_PAGER)` branch first that calls
   `display.setBrightness(pct)` instead (the hook `ST7796LCDDisplay` already
   exposes for exactly this).
1g. **RX-boosted-gain silently doesn't apply for the pager — deferred, not
   fixed, in Milestone ④** (matches M3's shared-file-edit precedent). Now that
   `-D USE_LR1121=1` is defined (not `USE_SX1262`/`USE_SX1268`), three call
   sites default/apply this radio setting only for the SX126x macros even
   though `CustomLR1121Wrapper` supports the identical feature:
   `src/MyMesh.cpp` ~2540 (default pref value on first boot), ~2556 and ~3355
   (actually calling `radio_driver.setRxBoostedGainMode(...)`), and
   `src/DataStore.cpp` ~264 (default pref value). The radio still works fine
   at default gain — this is a missed RX-sensitivity improvement, not a
   broken build or crash. Fix (whenever picked up): add
   `|| defined(USE_LR1121)` to those three conditions — safe/additive, zero
   behavior change for T-Deck/Heltec V4. (A fourth, core-lib-only gate exists
   in `CommonCLI.cpp`'s `radio.rxgain` companion CLI command, checking
   `USE_SX1262`/`USE_SX1268`/`USE_LR1110` — deliberately left alone too, since
   fixing just the CLI command without the three call sites above that
   actually apply the setting would expose a command that appears to succeed
   but doesn't do anything.)
2. **TCA8418 keymap** — raw matrix + our own shift/sym/alt state machine; the T-Deck
   never needed this (its C3 resolves ASCII). Bounded: trail-mate's layout tables are
   a working reference. Landed in M5 as `src/helpers/input/PagerKeyboard.cpp`,
   polling-based (not INT-pin-gated — see worklist ⑤ for why).
2a. **Encoder detent scaling — RESOLVED on hardware (2026-07-07).** The
   assumed 4-transitions-per-detent (`PAGER_ENCODER_STEPS_PER_DETENT`)
   matches this part: user drove the menus by encoder and reported it feels
   right ("encoder is looking good"), no fractional/multiple stepping.
1h. **`CAP_SD`/`CAP_FILESYSTEM` are 0 for the pager despite real microSD
   hardware — a latent gap in `device_caps.h`'s own abstraction, not a pager
   bug.** The mount code these caps are supposed to gate (`fmSdTryMount()`,
   the `#include <SD.h>` block, the file manager's SD-vs-FFat backend
   selection) was never actually migrated off `HAS_TDECK_GT911`/
   `HAS_TANMATSU` device-name checks onto the `CAP_SD` flag it claims to key
   off. Setting `CAP_SD=1` for the pager in the first M6 pass just produced
   `SD`/`CARD_NONE`/`fmSdTryMount` "not declared" errors, not real SD
   support — reverted to 0. Real support needs pager-specific wiring (CS 21,
   its own shared-SPI helper) *and* migrating that mount code to genuinely
   branch on `CAP_SD` — unscheduled, do both together whenever picked up.
1i. **`navMaybeRebuild()` was unreachable for the pager in the first M6
   pass.** It's only called under `#if defined(HAS_TANMATSU) ... #elif
   CAP_TRACKBALL ...`, neither of which the pager's cap combination matches
   (`CAP_TRACKBALL=0`, not `HAS_TANMATSU`). Left alone, the KEYPAD indev
   would register successfully but its focus group would stay permanently
   empty — total, silent navigation dead-end, easy to miss since the build
   still succeeds. Fixed with a `#elif defined(TLORA_PAGER)` arm alongside
   the existing branches.
1j. **`isDismissKey()`'s T-Deck p/q/a mapping doesn't transfer to the
   pager's full QWERTY** — that mapping exists only because the T-Deck's
   sparse keyboard lacks a dedicated Esc key; reusing it verbatim for the
   pager would make it impossible to type those three letters normally.
   Returns `false` for the pager instead — its Esc equivalent is the rotary
   encoder's long-press, handled separately in `updatePagerEncoder()`.
3. **222-px chat screen** — tightest layout wadamesh has shipped (current min is
   240). Mitigations: no on-screen keyboard (physical QWERTY), slimmer status/tab
   bars, landscape chat already exists (320×240 path).
4. **HW-CDC companion frames** (`ARDUINO_USB_MODE=1`) — known-regressed on the V4,
   fine on the T-Deck. Test the big device-profile frame first thing on hardware.
5. **Encoder-only ergonomics** on long lists (contacts @ 2000 max) — NEXT/PREV focus
   walk may need page-jump keys from the QWERTY (cheap: map to `navMoveDir`).
6. **Shared SPI contention** (display flush vs radio IRQ vs SD) — same topology the
   T-Deck ships, so expected fine via SPI transactions + CS discipline; keep an eye
   on SD-write + RX overlap during history flush. **Partially de-risked in M7:**
   TFT_eSPI force-defines `SUPPORT_TRANSACTIONS` on ESP32 (its raw-register fast
   path still takes the SPI HAL mutex), RadioLib does no SPI from ISRs, and mesh +
   UI loops share one task — so there is NO radio/display race (disproven theory,
   don't re-chase; see the debug log). CS discipline is now enforced at boot:
   `TLoraPagerBoard::begin()` parks LORA_NSS/LORA_RST/SD_CS/NFC_CS OUTPUT-HIGH
   before any bus traffic (LilyGoLib's `initShareSPIPins()` equivalent).
7. **NVS-preserving flash chain** applies here too — 4-component flash, never the
   merged image (wipes saved Wi-Fi creds).
8. **222-px vertical layout not yet audited against real constants** —
   `STATUSBAR_H`/`TABBAR_H`/`CHAT_KB_H` and the modal/chat height helpers were
   deliberately left untouched in M6 rather than guessing slimmer values
   desk-side; the 480×222 branch compiles and boots (verified by the wordmark
   centering math already being generic), but whether every screen actually
   fits without clipping/overlap at 222 px tall can only be judged on real
   hardware. Do this first in M8, before the broader nav-coverage pass.
9. **Keyboard backlight not wired to any timer/UI.**
   `pagerKeyboardSetBacklight()` (M5) works standalone but nothing calls it —
   no brightness-mode auto-dim, no Settings toggle. Low priority, cosmetic;
   pick up whenever the Settings screen work for this board happens.

## References

- Upstream MeshCore (MIT): `meshcore-dev/MeshCore` — does **NOT** currently have
  `variants/lilygo_tlora_pager/` or `CustomLR1121*` (re-verified 2026-07-06, see
  Decision ②); only `CustomLR1110{,Wrapper}.h` exists in `src/helpers/radiolib/`,
  same as our own core fork. Flasher precedent (unrelated to firmware source):
  `flasher.meshcore.io/lilygo-t-lora-pager/`
- Local working port (pin map + LR1121 init + keymap + AW9364/BQ27220/XL9555 usage):
  `~/dev/trail-mate/boards/tlora_pager/` (esp. `src/tlora_pager_board.cpp initLoRa()`,
  `include/boards/tlora_pager/tlora_pager_board.h`) and
  `~/dev/trail-mate/variants/lilygo_tlora_pager/pins_arduino.h`
- Canonical pin map (ground truth, matches trail-mate exactly): [`espressif/arduino-esp32`
  `variants/lilygo_tlora_pager/pins_arduino.h`](https://github.com/espressif/arduino-esp32/blob/master/variants/lilygo_tlora_pager/pins_arduino.h)
- Official hardware doc (chip list, I²C addresses, power-rail table, electrical
  specs): [LilyGoLib `docs/hardware/lilygo-t-lora-pager.md`](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/hardware/lilygo-t-lora-pager.md) —
  see the caveat under "Hardware / platform facts" above about its Pins Map
  table's internal inconsistencies.
- Hardware docs: LilyGo product page (T-LoRa Pager), CNX-Software 2025-08-12 writeup,
  Meshtastic device page (`meshtastic.org/docs/hardware/devices/lilygo/tpager/`)
- In-repo templates: `variants/lilygo_tdeck/` (S3 + SPI radio + shared bus),
  Tanmatsu keypad-nav path in `src/ui-touch/UITask.cpp`, `TANMATSU_PORT.md` (tracker
  precedent)
- TFT_eSPI's ST7796 driver support (ground truth for the CGRAM_OFFSET/
  colstart/rowstart mechanism, Decision ④): `.pio/libdeps/*/TFT_eSPI/TFT_Drivers/{ST7796_Rotation.h,ST7796_Defines.h}`,
  cross-checked against `ST7789_Defines.h` and `setAddrWindow()` in
  `TFT_eSPI.cpp` — pulled via the pinned `bodmer/TFT_eSPI @ ^2.5.43` lib_dep.
- AW9364 backlight driver actually consumed (not hand-rolled): `lewisxhe/SensorLib`'s
  `AW9364LedDriver.hpp` (MIT, header-only) — reference copy at
  `~/dev/trail-mate/.pio/libdeps/*/SensorLib/src/AW9364LedDriver.hpp`.
