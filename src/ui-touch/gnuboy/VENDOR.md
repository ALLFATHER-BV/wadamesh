# Vendored gnuboy core

Source: https://github.com/ducalex/retro-go — `retro-core/components/gnuboy/`
Commit: `4ced120669750ca7228fd0414211430c1d923166` (master, fetched 2026-07-02)
Re-vendored into wadamesh from the same upstream via the meshpunk snapshot.

License: **GPL-2.0-or-later** (see COPYING). CREDITS retained. Compatible with
wadamesh's GPL-3.0-or-later — folds forward into GPL-3.

Files: cpu.c hw.c lcd.c sound.c gnuboy.c + cpu.h hw.h lcd.h sound.h gnuboy.h tables.h

Built WITHOUT `-DRETRO_GO` so logging stays on plain `printf`. `IRAM_ATTR`
resolves to empty (defined in gnuboy.h under the non-RETRO_GO branch); the core
`.c` files pull no Arduino/ESP headers, so there is no clash with esp_attr.h.

Platform glue lives OUTSIDE this dir in `../GameBoy.{h,cpp}` (the wadamesh-native
host, replacing meshpunk's ELF-loader `main_tdeck.c`).

Local modifications:
- `sound.c` / `gnuboy.h`: added `gnuboy_set_mute(bool)` — when muted, `gb_sound_emulate`
  drains accumulated cycles and emits no samples instead of synthesizing all four
  channels (perf; audio is muted by default in the player). CPU/PPU timing unchanged.

Build note: the gnuboy `.c` files are compiled at `-O2` with jump tables restored via
`scripts/gnuboy_opt.py` (the Arduino-S3 default is `-Os -fno-jump-tables`, poor for the
opcode-dispatch switch). No source change for that — it's a build-flag middleware.
