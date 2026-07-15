# LilyGo T-Display P4 (AMOLED) — wadamesh port tracker

A phone-class ESP32-P4 + ESP32-C6 handheld with a real SX1262. Essentially a **touchscreen
Tanmatsu**, so we reuse the Tanmatsu P4/IDF/esp-hosted foundation (approach A: standalone IDF app
in `tdisplay_p4/`, sharing the Tanmatsu's ESP-IDF 5.5.1 via symlink). Full pinout + rationale:
memory `tdisplay-p4-port`. Device confirmed: ESP32-P4, 16 MB flash, MAC 30:ed:a0:e1:c2:a7,
flashes over the right-side USB (USB-Serial-JTAG). LoRa region = EU 868.

## Pinout (from LilyGo `components/private_library/t_display_p4_config.h`)
- **SX1262** SPI SCLK=2/MOSI=3/MISO=4, CS=24, BUSY=6 (raw GPIO); **RST=XL9535-IO16, DIO1=XL9535-IO17**
- **XL9535 I2C expander** on I2C_1 (SDA=7/SCL=8, INT=GPIO5): power rails, C6-EN(IO14), SD-EN(IO15),
  SCREEN_RST(IO2), TOUCH_RST(IO3)/INT(IO4), SX1262 RST(IO16)/DIO1(IO17), RF-switch VCTL(IO1), GPS-WAKE
- **Display** RM69A10 AMOLED MIPI-DSI 568×1232 (portrait); reset via XL9535
- **Touch** HI8561 (or GT9895) on I2C_1 (7/8); RST/INT via XL9535
- **C6** esp-hosted over SDIO_2 (CLK18/CMD19/D0-3=14-17); **SD** SD_MMC on SDIO_1 slot 0 (43/44/39-42)
- **GPS** L76K UART: ESP RX=22 / TX=23. **RTC** PCF8563, **gauge** BQ27220 on I2C_1

## Scaffold state (DONE)
- `tdisplay_p4/` created; `esp-idf`,`esp-idf-tools` + components `meshcore/ardlibs/lvgl/esp_hosted`
  + `sdkconfigs/{general,wadamesh}` symlinked to `../tanmatsu/`
- `build.sh` (DEVICE=tdisplay_p4, target esp32p4, standalone), `CMakeLists.txt`
- `main/CMakeLists.txt` — the full board WADA_DEFS (raw SX1262 pins, RM69A10, HI8561, XL9535 IOs)
- `components/wadamesh_app/CMakeLists.txt` — builds `src/` + `variants/tdisplay_p4/`, no badge-bsp
- `sdkconfigs/tdisplay_p4` (P4/PSRAM/DSI/esp-hosted-SDIO_2), `partition_tables/tdisplay_p4_16M.csv`

## Remaining Phase 1 — bring-up (empirical, needs the device; do in this order)
1. ✅ DONE `variants/tdisplay_p4/Xl9535.{h,cpp}` — I2C GPIO-expander driver + `powerOnSequence()` (rails →
   C6/SD enable → release screen/touch resets). Uses Arduino Wire on 7/8. Power-seq delays TUNE on-device.
2. `RM69A10Display.{h,cpp}` — MIPI-DSI panel. Recipe (from LilyGo t_display_p4_driver.cpp + config):
   - `esp_ldo_acquire_channel(chan_id=3, voltage_mv=2500)` powers the P4 DSI PHY (do BEFORE the DSI bus).
   - `esp_lcd_new_dsi_bus` (bus_id 0, **lanes=2**, lane_bit_rate ~**1000 Mbps** [confirm]) → `esp_lcd_new_panel_io_dbi`
     (8/8 cmd/param) → `esp_lcd_dpi_panel_config_t`: **dpi_clk=60MHz, RGB565(16bpp), 568×1232**,
     **HSYNC=50 HBP=150 HFP=50 / VSYNC=40 VBP=120 VFP=80**, num_fbs=1, use_dma2d=true.
   - RM69A10 vendor init: port LilyGo `rm69a10_driver.cpp` as an `esp_lcd_new_panel_rm69a10(io,&dev,&panel)`
     vendor driver (reset_gpio=-1 — reset is via XL9535 IO2, done in powerOnSequence). Then reset/init/disp_on.
   - Subclass `DisplayDriver(568,1232)`; `writePixelsRGB565(x,y,w,h,px)` → `esp_lcd_panel_draw_bitmap(panel,
     x,y,x+w,y+h,px)` (EXCLUSIVE end coords — see TanmatsuDisplay). This is UITask's lvglFlush target.
2b. ✅ DONE `esp_lcd_rm69a10.{cpp,h}` (LilyGo's vendor panel driver, ported verbatim) +
   `RM69A10Display.{h,cpp}` (the DSI bring-up recipe above → DisplayDriver + writePixelsRGB565).
3. ✅ DONE `target.{h,cpp}` — `TDisplayP4Board : ESP32Board` (RF switch via XL9535 in onBefore/AfterTransmit),
   raw SX1262 Module (RST/DIO1=RADIOLIB_NC), `radio_init()` = `xl9535.sx1262Reset()` + `radio.std_init(&spi)`,
   RM69A10 `display`, PCF8563 RTC on Wire. `tdisplay_p4_compat.h` (adcAttachPin shim) copied.
4. ✅ DONE `src/ui-touch/device_caps.h` — `HAS_TDISPLAY_P4` block (CAP_TOUCH/SD/GPS/FILESYSTEM/LARGE=1;
   web browser via the 32MB/#else path).

### Build integration DONE (bring-up commit — decisions worth remembering)
- **RadioLib is a P4-local IDF component** (`tdisplay_p4/components/RadioLib/`, v7.7.1 copied from the
  S3 libdep). Its CMake keeps the Arduino HAL and forces `RADIOLIB_BUILD_ARDUINO` + `RADIOLIB_GODMODE=1`
  + `RADIOLIB_STATIC_ONLY=1` **PUBLIC** (matches the S3 platformio flags; GODMODE is what lets the core's
  CustomSX1262Wrapper/SX126xReset reach SX126x private members). NOT added to the shared `ardlibs` — the
  Tanmatsu stays RadioLib-free.
- **P4 has its OWN meshcore component** (no longer a symlink to the Tanmatsu's): `core/` is symlinked to
  share the vendored source, but the P4 CMakeLists does NOT exclude `helpers/radiolib/` (compiles
  `RadioLibWrappers.cpp` — the buffered-RX `radioAcquire/rxQueueEnable/rxQueueSuspend` that MyMesh calls
  unconditionally) and `REQUIRES RadioLib`. The Tanmatsu's vendored core was pre-beta_34 and missing those;
  synced `RadioLibWrappers.{h,cpp}` from the S3 core (only 2 files differed; safe — Tanmatsu uses the bridge).
- **`CAP_SD=0`** for the P4 (device_caps.h). CAP_SD means "Arduino `SD` on shared SPI" (T-Deck/M9/R8); the
  P4's SD_MMC is the DataStore backend + exposed via CAP_FILESYSTEM like the Tanmatsu's FFat. This compiles
  out the whole Arduino-`SD`/`CARD_NONE`/`fmSdTryMount` UI cluster. SD_MMC file-browser = later, via the FS path.
- **P4 folded into the Tanmatsu's P4/IDF special-cases**: MQTT no-op bridge (no PubSubClient over esp-hosted),
  `soc/rtc_cntl_reg.h` + RTC force-download guarded off (S3-only reg), `feedLoopWDT` no-op shim. Msg-LED
  settings row re-gated to HAS_TANMATSU (it lived under CAP_LARGE_SCREEN; P4 is the 1st other large board).
- **HI8561 touch (`Hi8561Touch.cpp`) = display-first**: full gesture state machine (shared w/ CHSC6x/FT5x06)
  but `hi8561ReadPoint()` returns no-touch until the report format is verified on-device (documented skeleton
  inside). Boots + renders; taps come after pixels are confirmed (avoids phantom-press UI lockups).

### 🎉 FIRST BOOT — 2026-07-14 (image built + flashed; app runs)
Build links (app 2.9 MB, 27% free in the 4 MB slot) and flashes over the right-side USB
(`./build.sh flash -p <port>`; MAC 30:ed:a0:e1:c2:a7 confirms the board vs the Tanmatsu). `app_main`
runs: 32 MB PSRAM found, ESP-IDF 5.5.1. Two HW-integration issues seen on the serial + their fixes:
- **MIPI-DSI underrun** (`lcd.dsi.dpi: can't fetch data from external memory fast enough`): PSRAM was
  running at **20 MHz** — HEX-PSRAM @ 200 MHz on the P4 needs `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`
  alongside `SPIRAM_SPEED_200M` (without it Kconfig silently drops to 20 MHz, starving the DPI). FIXED
  in the sdkconfig fragment (matches the Tanmatsu). ⟶ re-flash to confirm pixels.
- **esp-hosted C6 reboot loop** (`transport: Init event not received within timeout, Resetting myself`
  → full reboot ~10 s): the C6 isn't answering over SDIO yet. Added
  `CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE=n` so the host stays up (UI + LoRa work;
  Wi-Fi/BLE stay down) instead of rebooting. The C6 SDIO pins (CLK18/CMD19/D0-3=14-17) + XL9535 power/
  enable/reset timing still need on-device verification — that's the next hardware task after pixels.

### 🖥️ DISPLAY WORKING — 2026-07-14 (the RM69A10 AMOLED emits + the wadamesh UI renders)
The panel came up only after matching the **pelgraine/Meck-P4** `screen_lvgl` reference EXACTLY (my
config matched LilyGo's config.h — pin map, 568×1232, DSI timing, init array — but the *bring-up
sequence* was wrong). The non-obvious, must-keep recipe (all in Xl9535.cpp + RM69A10Display.cpp):
- **DSI-PHY LDO = channel 3 @ 1830 mV** (NOT the "standard" 2500 mV — 2500 left it dark).
- **Power rails are power-CYCLED, not just set high**: VCCA→LOW; 5V HIGH/LOW/HIGH; 3V3 LOW/HIGH/LOW;
  200 ms between each; final states VCCA=LOW, 5V=HIGH, 3V3=LOW.
- **SCREEN_RST = HIGH→LOW→HIGH, 200 ms each, AFTER the LDO but BEFORE the DSI bus** (reset_gpio_num=-1;
  driven via the XL9535 inside RM69A10Display::begin, not powerOnSequence).
- **Brightness is a runtime write**: the init array's `0x51` is a placeholder (=0); send DCS `0x51=0xFF`
  via `esp_lcd_panel_io_tx_param` after `disp_on` (Meck ramps set_rm69a10_brightness()).
- LVGL flush → writePixelsRGB565 → esp_lcd_panel_draw_bitmap works (UI text renders clean).
Reference (working): github.com/pelgraine/Meck-P4 `main/examples/screen_lvgl/main.cpp`. LilyGo source:
github.com/Xinyuan-LilyGO/T-Display-P4 `components/private_library/{t_display_p4_config.h,
rm69a10_driver.cpp,t_display_p4_driver.cpp}`.
- ⚠️ OPEN: the tall 568×1232 portrait UI needs layout work — the wizard (designed for other aspect
  ratios) fills only the top; the rest showed the stale framebuffer. Also re-verify LoRa still works
  after the rail-state change (3V3 ends LOW).

### STILL TODO (needs the device — write→build→flash→iterate)
5. `Hi8561Touch.{h,cpp}` — I2C touch → the touch-UI indev (mirror RakTapV2Touch); RST/INT via XL9535.
   Can stub first (display-only boot), add after pixels work. Pull LilyGo `hi8561_touch.cpp` for the read proto.
6. `tdisplay_p4/main/main.cpp` — standalone `app_main` (model on tanmatsu/main/main.cpp, strip AppFS/badge-bsp):
   Serial.begin → `xl9535.begin()` + `powerOnSequence()` → `display.begin()` → esp-hosted C6 connect →
   `radio_init()` → placement-new the_mesh in PSRAM → storage (SD_MMC/FFat) → `ui_task` + companion → LVGL loop.
7. UITask.cpp threading: the display extern (line ~139: add `#elif defined(HAS_TDISPLAY_P4) extern RM69A10Display display;`)
   + the SD/`fmSdTryMount`/touch guards get `|| defined(HAS_TDISPLAY_P4)` (mirror the R8 pass); touch indev wiring.
8. `./build.sh build` → fix (P4/IDF-specific) → flash → **first-boot-on-screen**. Then P2 radio, P3 Wi-Fi/SD/GPS,
   P4 the 568×1232 portrait UI layout.

## Build
`cd tdisplay_p4 && ./build.sh build`  ·  flash: `./build.sh flash -p /dev/cu.usbmodem<P4>`
