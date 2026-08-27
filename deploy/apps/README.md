# Submitting an app to the WADAMESH store

Apps are small Lua programs that appear in the device's **Apps** drawer and are
downloaded over Wi-Fi from `firmware.wadamesh.com/apps/`. Snake, RF Monitor and
Airtime all ship this way — they are ordinary submissions, not special cases, so
reading their source is the fastest way to see what an app looks like.

**Every submission is reviewed and safety-checked before it is added.** What that
means in practice is spelled out under [Review](#review) below. It is not a
formality and it is not a judgement of you — an app runs on other people's radios,
so somebody has to read it first.

## What an app is

One Lua file plus a small manifest. Apps talk to the firmware through the `wada.*`
API — `wada.ui` (widgets, colours, a text prompt), `wada.sys`, `wada.store`
(persistence), `wada.timer`, and on the larger boards `wada.fs`, `wada.net`,
`wada.crypto` and the writable half of `wada.mesh`. Supported boards also expose
asynchronous WAV/MP3 playback through `wada.audio`; plain filenames come from the
app sandbox on internal flash, SD, or SD_MMC. Read-only physical-SD directory
metadata is available through `wada.sd`, and direct card playback uses an explicit
`sd:/...` path. There is no general-purpose filesystem or network access; the API
is the whole surface, which is what makes reviewing tractable. It is documented at
[wadamesh.com/sdk.html](https://wadamesh.com/sdk.html).

Two numbers worth knowing before you start: every callback runs under a
**100,000 Lua instruction** budget, and Lua here is built with **32-bit numbers**,
so its floats are single precision. If you are logging coordinates, use the
`lat_e6` / `lon_e6` integers rather than `lat` / `lon`.

Apps run on **every** board that has the Apps drawer, from the 2 MB-PSRAM Heltec
V4 to the 800×480 Tanmatsu. Do not hard-code pixel sizes — read the screen from
the API and lay out relative to it, the way `snake.lua` computes its grid.

## Layout

```
deploy/apps/<id>/<ver>/<id>.json     manifest
deploy/apps/<id>/<ver>/<id>.lua      the app
```

`<id>` is lowercase, no spaces, and is the filename stem everywhere. `<ver>` is
`major.minor`.

The manifest is one line:

```json
{"id":"snake","name":"Snake","ver":"1.0","desc":"The classic, in Lua. Swipe to steer.","icon":"game"}
```

`desc` is what people read in the store listing before installing, so make it say
what the app *does*. One sentence.

`icon` is optional and picks the drawer tile's glyph by NAME — an app cannot
ship its own artwork, so anything unrecognised falls back to the generic app
symbol. Choose from: `gps` / `compass` / `map`, `radio`, `signal`, `chart`,
`list`, `message`, `person`, `group`, `bell`, `star`, `search`, `settings`,
`battery`, `game`.

**Version directories are immutable.** Once `1.0/` is published it is never edited
— a change ships as `1.1/`. Devices cache by version, so editing in place means
some users silently run different code from others under the same version number.

## Submitting

1. Fork `ALLFATHER-BV/wadamesh` and branch off `main`.
2. Add your `deploy/apps/<id>/<ver>/` directory with the two files.
3. Add or update your app's row in `deploy/apps/apps.json` (the store catalog).
   Add `"seed": false` to that row if your app should be downloadable but not
   compiled into the firmware. Apps in the catalog are baked into the image as
   built-ins for boards that cannot reach the Store, and that is flash spent on
   the boards with the least of it. A large app, or one that needs hardware
   those boards do not have, is better left to the Store.
4. Open a pull request. In the description, say what the app does, which boards
   you tested it on, and anything it persists via `wada.store`.

Please open the PR even if you are unsure about the code — a rough app that works
is easier to review than a perfect one that never gets sent. If you would rather
not use git at all, open an issue with the `.lua` file attached and say so; we
would rather have the app than the paperwork.

You keep authorship: merging the PR preserves your commits, and the store listing
is generated from your manifest. The repository is GPL-3.0, so your app is
published under that licence too — do not submit code you are not free to license
that way.

## Review

Two passes. The first is ordinary code review: does it work, does it fit the
screen on a small board, does the description match the behaviour.

The second is the **safety check**, and it exists because an app ships to other
people's devices. We read every submission for:

- **Identity and keys** — an app must not read, log, transmit or persist the node
  identity, private key, channel secrets or contact keys. This is the one that
  gets a submission rejected outright rather than sent back for changes.
- **Flash wear and stalls** — no writes in a tight loop and none on a per-packet
  path. Frequent small writes to internal flash trigger garbage collection, which
  on ESP32 suspends the flash cache and stalls *both* CPU cores; we have shipped
  two firmware bugs of exactly this shape. Persist on a user action or a slow
  timer, not on every frame.
- **Blocking the UI** — no busy-waits and no long synchronous work. The UI and the
  mesh share a loop; an app that blocks it drops packets as well as frames.
- **Memory** — bounded allocations, and nothing that assumes 8 MB of PSRAM. The V4
  has 2 MB and is the floor.
- **Radio behaviour** — an app may read counters and statistics. Transmitting, or
  changing radio parameters, needs a clear reason and an explicit user action.
- **Probing** — `wada.mesh.discover()` costs a transmission from the user *and a
  reply from every node that hears it*, so it spends other people's airtime as
  well as theirs. The firmware enforces 15 seconds between probes, but a survey
  app is still expected to probe on a cadence a person chose and to stop when it
  is not on screen. Probing in a loop for no stated reason gets sent back.
- **Where data goes** — anything leaving the device has to be something the user
  asked for and can see.

If something needs changing we will say what and why on the PR, and we would
rather help you land it than close it. If we reject an app we will tell you the
reason plainly.

## Testing before you send it

Try it on real hardware, not just one screen size. If you only have one board,
say so in the PR — that is useful information, not a disqualification, and it
tells the reviewer what still needs checking.
