# Host harness for Wadamesh Lua apps

Runs a Store app on your desktop, against a mock of the `wada.*` host, before
it ever touches a device. Not part of the firmware build.

```sh
scripts/lua-harness/run.sh                                   # gpscompass, all scenarios
scripts/lua-harness/run.sh deploy/apps/wardrive/1.1/wardrive.lua
SCENARIO=declination scripts/lua-harness/run.sh              # one scenario
```

`run.sh` compiles `luah` on first use (and whenever `main.c` changes) from the
Lua sources already vendored in `lib/lua/src` — no dependency beyond a C
compiler.

## What makes it worth running

It is built from the **same vendored Lua 5.4.7 as the firmware, with the same
`LUA_32BITS` numeric model** — 32-bit integers, single-precision floats. That
is the point. Maths that behaves in a desktop float64 interpreter can lose
catastrophic precision on the device, and this is where that shows up.

The mock mirrors `LuaAppHost.cpp` rather than being convenient: the same
argument type-checks, the same 100k-instruction budget per callback, the same
event shapes, and the same store restrictions. That has caught real defects —
the device store accepts strings and numbers only, and a boolean flag thrown
inside a guarded callback looks exactly like a keypress doing nothing at all.

Device attitude is simulated physically, not faked: a magnetic field of the
right magnitude and dip, rotated into the body frame by heading/roll/pitch,
plus the hard-iron and zero-g biases measured on a real M9. So calibration,
tilt compensation and heading all get exercised for real.

## Scenarios

`harness.lua` holds them all. Board shapes — M9 320x196 landscape, V4/R8
portrait, base-SDK V4 (no extended SDK), Pager, Pager portrait at Jumbo font
sizes, Tanmatsu — plus an instruction-cost probe that fails the run if any tick
exceeds a quarter of the budget.

Three are about the compass being correct rather than merely working:

- **`declination`** — pulls the generated WMM block straight out of the app
  file and checks it against NOAA reference values at six sites, including a
  weak-field one near the pole. Testing the shipped bytes, not a copy, so a bad
  regeneration fails here rather than on hardware. See `scripts/wmm/`.
- **`bearings_absolute`** — contacts placed due N/E/S/W and NE of the fix, to
  check bearings against **absolute** compass directions. The older marker test
  compares each drawn dot against the app's *own* bearing, which stays
  self-consistent even if east and west are swapped; this one would not. The
  north-east case is what separates a mirror from a lat/lon transpose (38.3°
  vs 51.7°).
- **`align_nofix`** — "set north" pressed with no position at all. The offset
  it stores silently swallows the whole declination, so the app has to give it
  back when the first fix lands, or the heading is wrong by twice it.

## Reading a failure

Scenarios print what they saw before asserting, so a failure usually names the
cause. Two conventions worth knowing:

- The dial centre is read out of the canvas **draw order**: digits, degree
  sign, T/M reference, cardinal. Add anything to that sequence and the index
  offsets in the assertions move with it.
- `APP ERROR:` means the app raised inside a guarded callback — the harness
  keeps going, and the scenario fails later on a symptom. The first `APP ERROR`
  line is the real one.
