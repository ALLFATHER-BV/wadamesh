# T-LoRa Pager simulator

The host build (`web/`) laid out as the LilyGo T-LoRa Pager: 480x222, no touch,
the QWERTY keyboard and the rotary encoder as the only input, and the UI's own
`TLORA_PAGER` branches compiled in. It is for reviewing and scripting the Pager's
screens without flashing a device after every layout change.

```bash
web/fetch-deps.sh
cmake -S web -B web/build-pager -G Ninja -DCMAKE_BUILD_TYPE=Release -DWADA_SIM_PAGER=ON
cmake --build web/build-pager --target wadamesh-host

# interactive (SDL window, 2x)
WADA_DATA_DIR=/tmp/pager-data web/build-pager/wadamesh-host --size 480x222 --scale 2

# headless screenshots
WADA_DATA_DIR=/tmp/pager-data web/build-pager/wadamesh-host --headless --size 480x222 \
  --script web/sim_pager/tour.wada --shots /tmp/pager-shots
```

Screenshots are written at the canvas size (800x480); the Pager's screen is the
top-left 480x222.

## Input

| Host key | Pager |
|---|---|
| Up / Down | turn the knob one detent |
| Right | click the knob |
| Left / Esc | hold the knob ~1.2 s (Back) |
| Enter, Backspace, letters | the keyboard's own keys |

Script-only: `holdkey C MS` types C and keeps its key down for MS (the accent
picker on a held letter); `holdkey space MS` holds the space bar (Space to lock),
`holdkey shift MS` / `holdkey fn MS` hold Shift / Fn alone (a `key down` meanwhile
is Shift + turn), and a `key bs` tap on the dark locked screen shows the lock
screen. Alt/Fn chords are not simulated.

`WADA_SIM_SOC=N` sets the gauge's percentage (`WADA_SIM_SOC_DROP=1` lets it fall
1 % per 5 s), `WADA_SIM_CARE_HOLD=1` fakes battery care holding at 80 % on USB,
`WADA_SIM_FACEDOWN=1` lays the Pager face down, `WADA_SIM_PICKUP=MS` lifts it MS after
start, `WADA_SIM_BAG=1` carries it in a swinging bag, `WADA_SIM_USE_MA=N` seeds the learned
average draw for "time left at your usage".

`WADA_SIM_CHARGING=1` in the environment fakes USB charging at about 480 mA
(charge icon, gauge lines, the screensaver while idle).

## What is stubbed

`SimPager.cpp` and the headers here stand in for the Pager's hardware: the
ES8311 codec and I2S (silent), the DRV2605 motor (prints the pattern), the
BHI260AP (lying flat), the charger and gauge (76 %, on battery), no microSD.
The mesh is the web build's demo backend.
