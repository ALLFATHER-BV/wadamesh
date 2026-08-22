# World Magnetic Model → Lua

Generates the magnetic-declination block that lives inside
`deploy/apps/gpscompass/1.0/gpscompass.lua`.

## Why an app carries a geophysical model

A magnetometer measures **magnetic** north. Every bearing computed from two
sets of coordinates is relative to **true** north. Draw one against the other
and every waypoint on the compass is wrong by the local declination — which is
how this arrived: on a ThinkNode M9 in Atlantic Canada, every contact sat about
22° west of where it belonged, including nodes whose positions were known to be
correct.

Declination is not a constant. It runs from about +25° to −25° across the
populated world, reverses sign along the agonic line, and drifts on the order
of a tenth of a degree a year. A single hard-coded offset is wrong for almost
everyone, and a user-entered one is a support burden that most people will get
wrong or never set.

So the app carries the real model: **WMM2025 to degree 12**, coefficients and
evaluation in ~4.7 KB of Lua, including the secular-variation terms so it stays
correct across the model's whole 2025.0–2030.0 window rather than drifting from
a frozen snapshot. A lookup grid accurate enough to stay inside a degree would
have cost several times the space and still been worse.

Past expiry it degrades gracefully — roughly 0.14°/yr — so it stays inside a
degree until about 2033 even if nobody reissues it.

## Files

| | |
|---|---|
| `WMM.COF` | NOAA/NCEI coefficients, epoch 2025.0. Upstream data, unmodified. |
| `WMM2025_TestValues.txt` | NOAA's 100 official test values. Upstream data, unmodified. |
| `wmm.py` | float64 reference implementation, used by both scripts below. |
| `gen_lua.py` | emits the Lua block; `--check` / `--update` keep the app in sync. |
| `verify.py` | checks `wmm.py` against NOAA's test values. |
| `wmm_block.lua` | the evaluation code the generator wraps around the coefficients. |

Source for both data files:
<https://www.ncei.noaa.gov/sites/default/files/2024-12/WMM2025COF.zip>

## Regenerating

```sh
scripts/wmm/gen_lua.py --update deploy/apps/gpscompass/1.0/gpscompass.lua
```

The block is delimited in the app by `-- WMM-GEN BEGIN` / `end -- WMM-GEN END`.
Everything between those markers is generated; the prose above them is not.

```sh
scripts/wmm/gen_lua.py --check deploy/apps/gpscompass/1.0/gpscompass.lua
```

`--check` exits non-zero with a diff if the app has drifted from `WMM.COF` —
either because the coefficients were updated without regenerating, or because
someone edited the block by hand. Worth running before a release, and the
obvious thing to wire into CI if this repo ever grows a PR workflow.

The generator owns the `local declination` / `do ... end` wrapper, and that is
deliberate. The coefficient tables are named `G`, `H`, `GD`, `HD`; gpscompass
uses a global `H` for the screen height, and an unscoped `local H` silently ate
it. Keeping the wrapper generated means it cannot come unwrapped again.

## Verifying

Two independent checks, because "4.7 KB of numbers that produce a plausible
angle" is not evidence of anything.

**The maths, against NOAA:**

```sh
scripts/wmm/verify.py
```

Runs all 100 official test values (declination, inclination, H, X, Y, Z, F over
the full 2025–2030 range and both hemispheres) and prints the worst absolute
error per component.

**The generated Lua, in the interpreter the device actually runs** — single
precision, `LUA_32BITS`, which is where an implementation that is fine in
float64 can quietly fall apart:

```sh
scripts/lua-harness/run.sh          # the `declination` scenario
```

It pulls the block straight out of the app file rather than testing a copy, so
a bad regeneration fails there instead of on the device. Current worst error
against NOAA reference values: **0.0002°**.

## Updating to a future model

When NOAA issues WMM2030 (or an out-of-cycle revision):

1. Drop in the new `WMM.COF` and `WMM2025_TestValues.txt` equivalents.
2. `verify.py` — should still pass; if it does not, the file format changed.
3. `gen_lua.py --update <app>`.
4. `scripts/lua-harness/run.sh` — update the reference values in the
   `declination` scenario, which are epoch-dependent.

The evaluation code in `wmm_block.lua` is degree-agnostic and needs no change
for a same-degree model.
