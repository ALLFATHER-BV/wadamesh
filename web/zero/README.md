# web/zero: the M5Stack Cardputer Zero board

WADAMESH on the Cardputer Zero (Raspberry Pi CM0, Linux): the user guide is
**[CARDPUTER_ZERO.md](../../CARDPUTER_ZERO.md)**.

| File | |
|---|---|
| `zero_main.cpp` | the front end: fbdev output (480x222 Pager layout, area-averaged to 320x170), evdev keys, boot log, HUD, key help, shell, the easter egg, respawn |
| `zero_battery.cpp` | the BQ27220 gauge (power_supply, else I²C 0x55) |
| `zero_gps.cpp` | NMEA from the Cap's GPS, GPX and WiGLE loggers |
| `zero_audio.cpp` | the Pager's I2S tone path to PipeWire / ALSA |
| `zero_ctl.cpp` | `wadamesh.sock` (wadamesh-cli) and the on-message hook |
| `zero_sys.cpp`, `zero_term.cpp` | system facts; the canvas and terminal font for the overlays |
| `gen_help.py`, `gen_font.py` | generate `zero_help.h` and `zero_font.h` |
| `build.sh`, `aarch64-toolchain.cmake` | cross build and packaging |
| `dist/` | what goes on the device: installer, service, wadamesh-cli, launcher entries, example hook |
| `docs/img/` | screenshots and the device picture |

Build: `web/zero/build.sh` (see the guide). CMake: `-DWADA_ZERO=ON` for the LCD program,
`-DWADA_DESKTOP_DLSDL=ON -DWADA_SDL2_INCLUDE=...` for the HDMI desktop program.
