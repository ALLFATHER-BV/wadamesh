# minimp3

This directory vendors `minimp3.h` from
[lieff/minimp3](https://github.com/lieff/minimp3) at commit
`ea99364f61c14656440e8d77e9c233ccf3124633` under CC0-1.0.

Pinned upstream and local file hashes:

- upstream `minimp3.h`: `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`
- local `minimp3.h` with the hook below: `3f59a7f29de636ca0db0aab8a5b86739c43ac8bf9ac902010e7220f783c0ae0a`
- `LICENSE`: `6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01`

Wadamesh adds one opt-in `MINIMP3_SCRATCH` hook around the decoder's local
scratch object. The Lua audio worker uses it to place the roughly 16 KB decoder
workspace in PSRAM instead of its FreeRTOS task stack. Builds that do not define
the hook retain upstream behavior.
