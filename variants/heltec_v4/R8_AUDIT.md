# Heltec V4-R8 (Expansion Kit V2) — audit pass, 2026-08-19

Multi-agent audit (4 dimensions, each adversarially verified) of the R8 build,
which compiles the whole plain-V4 TFT codebase with `HELTEC_LORA_V4_R8` gating
only the deltas. 26 findings: 23 confirmed, 1 refuted, 2 hardware-uncertain.
Everything below is compile-verified; items marked **[HW]** still want a check
on the physical device.

## Fixed in this pass (all R8-gated unless noted)

### Storage
- **Chat history / battery log / Lua app files now follow the SD adoption.**
  `uiDataFsReady()` had no R8 arm, so even when the boot store adopted the card
  (identity/prefs/contacts on SD), history stayed on SPIFFS with the small
  500-message ring — a split store with none of the SD hardening. New R8 arm is
  keyed on `g_full_data_on_sd` (the card is REMOVABLE — pager-shaped decision,
  unlike the M9's always-adopt), plus `uiDataFsIsSdCard()` and the write-failure
  wedge stamps / remount flush blocks. **[HW]** Upgrade caveat: an R8 that
  adopted a card on an older build carries a day-one `/msgs` snapshot on it —
  run Settings → Storage → "Copy internal data to SD" once to refresh.
- **The reinsert watch no longer unmounts the live data store.** UITask never
  adopted the boot SD mount on the R8 (`s_sd_mounted` stayed false), so
  `sdHealthTick` SD.end()'d the volume DataStore was using ~30 s after boot —
  spurious "SD card remounted" toast on warm cards; slow cards (the ones the
  boot ladder's 400 kHz–1 MHz rungs exist for) could be stranded unmounted all
  session with contacts silently unsaveable. `sdAdoptLiveMount()` now runs at
  UITask::begin like the pager.
- **Map tile cache prefers the SD card** (boot-mount-follow only — no mount
  ladder on card-less boots), same rationale as the M9: the 4.75 MB tiles
  partition fills and has no eviction, after which every tile download fails
  forever. Partition remains the fallback when the card is out.
- **SD chip-select (GPIO3, strapping pin) parked HIGH in board init** before
  LGFX floods the shared FSPI bus with the boot logo.

### Display / touch
- **Rotation trap defused.** `CAP_ROTATABLE` → 0: the Orientation control was a
  reboot trap (the boot guard force-reverts landscape because the R8 landscape
  touch maps are TESTER-VERIFY), and worse, the boot wordmark had already
  rotated the panel before the guard ran — the pref got reverted but the panel
  stayed landscape → one fully garbled session per attempt. The guard now also
  heals the panel (`::display.setDisplayRotation(0)`). Re-enable rotation +
  remove the guard together once a tester confirms landscape touch. **[HW]**
- **Display SPI raised 20 → 40 MHz** (full-screen flush was ~61 ms of pure bus
  time, ~4× the plain V4's 80 MHz driver). **[HW]** Watch for tearing/garbled
  bands; fall back to 26.6 MHz if seen.
- **Panel sleep got its datasheet delays** (SLPIN +5 ms, SLPOUT +6 ms — the
  LGFX call writes the bare command; the wake path fires backlight + flushes
  immediately after). **[HW]**
- **Backlight single-owner.** GPIO44 had three owners (LGFX Light_PWM ch7
  44 kHz/9-bit, target.cpp digitalWrite, UITask LEDC ch6 20 kHz/8-bit — ch6/ch7
  share LEDC timer 3). LGFX no longer configures a Light; begin/turnOn/turnOff
  drive the pad directly and UITask's LEDC owns brightness after boot.
- **Touch poll throttled while the screen is off** (8 → 50 ms): the CHSC6x poll
  is a blocking ~2 ms I2C read on the SHARED sensor/RTC bus, formerly 24/7.
  Wake-on-touch worst case +42 ms. (INT-pin gating would be better still but
  needs the INT level/pulse behavior characterized on hardware first.)
- **Swipe map corrected for the transient keyboard landscape** (LVGL
  sw-rotate, panel NOT rotated): the R8's panel-baseline-swapped map applied
  there too, inverting left/right swipes — a swipe could read backwards and
  e.g. close a chat via the swipe-back gesture. The hardware-verified V4
  formulas now apply whenever the panel isn't hardware-rotated. **[HW]**
- **Touch probe address honored** (shared fix): a CHSC6x variant ACKing at
  0x15/0x14 used to pass the probe and then read hardcoded 0x2E forever
  ("touch init ok", dead touch). The driver now reads the address that ACKed.

### Power
- **"Power off" actually powers things off**: it used to leave the SX1262 in
  RX with the FEM enabled (pure drain — every wake source except the button is
  cleared) and the non-RTC rail pins (VEXT=40, GPS_EN=42, backlight=44)
  floating. Now: radio off, FEM sleep, rails parked + `gpio_hold` through deep
  sleep (released on wake in HeltecV4Board::begin). Toast says "press BOOT to
  wake". **[HW]** measure off-current before/after.
- **Battery saver unlocked** (Settings → Battery): the toggle was T-Deck-only
  on a stale "gate never passes on the V4" rationale that doesn't hold for the
  R8; a parked R8 busy-spun the main loop all night. **[HW]**
- **Charging detection enabled** (T-Deck voltage heuristic): the R8's divider
  is permanently connected (PIN_ADC_CTRL=-1), so the EMA + above-full check
  applies; previously compile-time false — no bolt, no charge-flip publish.
  **[HW]** confirm the charger lifts the divider node above full+50 mV, and
  sanity-check ADC_MULTIPLIER=5.07 absolute accuracy.

### UX
- **"Save update bin to SD"** now offered on the R8 (About page) — it
  qualified on every axis and its OTA bin name (`wadamesh-heltec-v4-r8-tft`)
  already existed.
- **"Set as wallpaper" hidden where no lock screen exists** (`CAP_LOCK_SCREEN`
  gate — the R8 is the only filesystem board without one; the button toasted
  "Lock wallpaper set" into the void).
- `CAP_LOCK_SCREEN=0` documented: enabling it is a feature (needs a
  reveal + hold-to-unlock input path), not a cap flip.

## Resolved on hardware (2026-08-19 evening, edge-probe + NMEA passthrough)

- **GPS: fully working — CLOSED.** Probed on the device: EN (GPIO42) is
  active-LOW as inherited (module streams only with EN low); the module (CASIC
  AT6558R GNSS) transmits on GPIO39 at 9600 baud; 67 clean NMEA sentences
  captured. A "no GPS signal" report is the module honestly having no
  satellite FIX (GGA quality 0 indoors) — needs sky view. Two traps recorded
  for posterity: (1) `Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX)` passes the TX
  macro as OUR RX pin (Arduino's first arg is RX) — the macros look swapped
  but are correct; documented at the env pins. (2) The R8's `Serial` goes to
  UART0, NOT the USB port — for a USB console build diagnostics with
  `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`. The USB port is the
  S3's USB-Serial-JTAG; pyserial DTR/RTS games can strap it into ROM download
  mode (device "dead" until esptool's USBJTAGSerialReset sequence or the
  reset button) — that bit us repeatedly.
- **Battery reading + charging bolt: fixed and hardware-confirmed.** The
  generic V4 read (uncalibrated `raw*(3.3/1024)` at the default 11 dB
  attenuation) under-read the R8's always-connected HIGH-RESISTANCE divider:
  the value pinned at 3839 mV (one 10-bit count ≈ 16 mV of display) through
  charger plug/unplug, and a genuinely full 4.2 V pack displayed ~3.9 V — so
  the charging-bolt threshold (batteryFullMv()+50 ≈ 4250 mV) was unreachable
  by construction. Fix (R8 arm in `HeltecV4Board::getBattMilliVolts`): match
  Meshtastic's `heltec_v4_r8` variant — `ADC_2_5db` attenuation ("lower dB
  for high resistance voltage divider"; the node sits at VBAT/5.07 ≈
  0.65-0.85 V) + IDF-calibrated `analogReadMilliVolts` × ADC_MULTIPLIER.
  User-confirmed working on the device (value live, charging visible).
  Diag hook `-DR8_DIAG_BATT` (Serial print + on-screen toast) kept flag-gated
  in main.cpp for future battery debugging.

## Open / hardware-uncertain (documented, not coded)
- **FEM TX-enable vs the TX LED**: the R8 env moved the LED to GPIO46, which is
  also the inherited GC1109 FEM TX-enable. If the real FEM enable moved
  elsewhere, TX runs through the FEM bypass (several dB down, presents as
  "poor range"). The new boot line `[R8] FEM type=…` plus a TX-current/RSSI
  comparison against a plain V4 settles it. **[HW]** — NB related observation
  below: this unit's 2.4 GHz Wi-Fi is measurably weak, so judge LoRa range
  with possible RF-path/assembly variance in mind.
- **Wi-Fi "auth failed" root-caused on hardware: weak signal.** The UI's Wi-Fi
  status now appends the esp_wifi disconnect reason ("auth failed (rNN)" —
  permanent diagnostic, all Wi-Fi boards; recorder in main.cpp, string in
  wifiStaStatusBrief). This unit failed with r2 (AUTH_EXPIRE) at a distance
  where the M9 connects fine, and connected normally next to the router —
  i.e. the AP timed out the handshake on a marginal link; not WPA3, not the
  password. Practical: keep the R8 nearer the AP for tile/OTA downloads, and
  treat the weak 2.4 GHz path as a data point for the FEM/LoRa-range check
  above. Reason-code decode for future reports: r15 = wrong password (WPA2
  4-way timeout), r2 = auth expired (weak signal, or a wrong password on a
  WPA3-SAE association), r201 = AP not found (band steering / 5 GHz-only),
  r23 = 802.1X (enterprise, unsupported).
- Refuted during verification: the About "Model:" mislabel (code is inside
  `#if 0`); slot-pool/cull, drain-order, tab-bar-geometry concerns (M9-pass
  parity checks) — all verified non-issues on the R8.

---

# Perf pass (2026-08-20)

Question asked: "are we achieving full performance out of the R8 build?" Audit of
CPU / memory / flash / display / SD / Wi-Fi / RF paths. Compute side was already at
spec (240 MHz runtime clock, octal PSRAM @ 80 MHz, QIO flash @ 80 MHz, LVGL heap in
PSRAM, draw buffer in internal DMA RAM, 16 ms refresh). Six gaps found and fixed,
all flashed to the user's unit the same day:

1. **FEM LNA defaulted OFF.** The R8 is a V4.3.1-generation board (KCT8103L FEM with
   the software-switchable ~17 dB RX LNA); `fem_lna` defaulted to 0 = bypassed.
   Prefs schema v49: default ON on `HELTEC_LORA_V4_R8`, one-time flip of existing
   installs (no new field). Toggle stays in Radio & Mesh. **[HW]** confirm the boot
   line `[R8] FEM type=1` / About "LNA on"; expect better RX in quiet sites, possibly
   worse in RF-noisy ones (then turn it off).
2. **Display bus 40 → 80 MHz** (`LGFX_SPI_WRITE_HZ`, LGFXDisplay.h): parity with
   the plain V4's TFT_eSPI driver. Full-frame bus time ~31 → ~15 ms. **[HW]** watch
   for tearing / garbled bands; `-D LGFX_SPI_WRITE_HZ=40000000` steps back.
3. **Async DMA band flush** (`LGFXDisplay::flushBandRGB565`, used by `lvglFlush` on
   the R8 only). Before: LVGL's LE pixels went through LovyanGFX's convert path
   (per-pixel swap into 32..256 px chunks, each its own DMA kick) and the flush
   returned only when the band was on the wire — render and transfer never
   overlapped. Now: one 16-bit rotate per pixel into an internal DMA buffer (12 KB),
   ONE no-convert DMA per band, `lv_disp_flush_ready` immediately; the last band
   (`lv_disp_flush_is_last`) waits and closes the frame transaction so the shared
   micro-SD gets the bus between frames exactly as before. Sync fallback if the
   buffer can't be allocated. `finishFrame()` guards panel sleep / rotation / clear.
4. **Wi-Fi modem power save is now a pref** (`wifi_ps` in the NVS Wi-Fi store;
   toggle in Wi-Fi settings, applies live once associated). Default ON everywhere
   (unchanged behaviour), OFF on the R8 (USB-powered kit; lower-latency TCP/app
   link and steadier association on this unit's weak 2.4 GHz path).
5. **micro-SD operating clock 4 → 20 MHz** (`SD_SPI_FAST_HZ`, include/SdFastClock.h):
   after the proven 4 MHz mount, re-begin at 20 MHz and READ-VERIFY (a probe file's
   first 512 B captured at 4 MHz and byte-compared after the raise; SPI-mode SD has
   no data CRC so a bare mount success would not catch a marginal clock). Falls
   back to 4 MHz. Wired at all three mount sites (boot adoption in main.cpp, the
   UITask mount ladder, the reinsert remount). No-op on boards without the flag.
6. **Boot at 240 MHz**: the R8 env now sets `ESP32_CPU_FREQ=240`; previously all of
   setup() (radio init, SD ladder, store load, mesh begin) ran at the base env's
   80 MHz until UITask bumped it. Screen-off DFS to 80 MHz unchanged.

Verification aid: Settings → About on the R8 gained a `Perf:` row —
`CPU 240 · TFT 80 MHz DMA · SD 20 MHz · LNA on` is the all-green reading.
("sync" = DMA buffer alloc failed; "SD 4 MHz" = the 20 MHz verify failed and it
fell back; both are safe degradations, not faults.)

Deliberately NOT done: `-O2` (image is 3.70 MB in a 3.875 MB OTA slot), bigger data
cache (baked into Arduino's prebuilt IDF libs), radio BW/SF/CR (network parameters).
