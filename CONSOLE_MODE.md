# Console mode

A boot mode where the device runs with **no LVGL**: a text console driven by
typed commands, with core mesh functionality and Lua apps. For boards that
struggle under the UI, and for people who would rather type than tap.

Regular mode is unaffected and you can go back at any time.

---

## Short answer

**Possible: yes, and considerably more of it already exists than you would
expect.** Four of the five pieces are built and shipping today.

**A lot of work: no, not for a usable console.** The work is not the console
itself, it is everything that currently assumes the UI is there. Realistic
shape: a useful console in a few days of work, a polished one that runs Lua
apps in a couple of weeks.

**Saves resources: yes, but not the resource you might think.** See
[What it actually saves](#what-it-actually-saves) below, because this changes
how the feature should be sold and to whom.

---

## What already exists

This is the reason the estimate is not larger.

| Piece | Where | State |
|---|---|---|
| **Command surface** | `CommonCLI::handleCommand` (MeshCore core) | Done. The full command set the node already answers over serial and over the mesh. |
| **Run a command locally, capture its output** | `MyMesh::runLocalCli()` + `MyMesh::setTerminalSink()` | Done. Feed it a line, get reply lines back through a callback. |
| **Text rendering with no LVGL** | `DisplayDriver` (core): `startFrame`, `setCursor`, `print`, `printWordWrap`, `getTextWidth`, `fillRect`, `drawTextEllipsized`, `endFrame`, UTF-8 to blocks | Done, and linked into the touch build already. |
| **A mode where LVGL does not drive the panel** | Remote mode (`s_remote_mode` in `UITask.cpp`) | Done and shipping. It paints the physical screen with `display.drawTextCentered(...)` directly while LVGL renders elsewhere. This is the exact precedent. |
| **An on-device terminal** | `homeTerminalCb` / `s_term_log_box` in `UITask.cpp` | Done, but built out of LVGL widgets. The *behaviour* is the model; the widgets are what console mode replaces. |

So the console does not need a new command language, a new renderer, or a new
way of talking to the mesh. It needs a different **front end** over machinery
that is already there and already tested.

---

## What it actually saves

Worth being precise, because the obvious assumption is wrong in one direction
and right in another.

### PSRAM: a real saving, and it is the object tree, not the draw buffer

Everything LVGL allocates is PSRAM: `include/lv_conf.h` sets `LV_MEM_CUSTOM 1`
with `LV_MEM_CUSTOM_ALLOC lvglPsramAlloc`, so every object, style and font cache
entry is a PSRAM allocation, and the draw buffer is `MALLOC_CAP_SPIRAM` too.

**The draw buffer is not the prize.** `LV_DRAW_BUF_LINES` is 24, so it is
37.5 KB on the 800 px panels and **11.2 KB on the V4**. Worth having back, not
worth building a mode for.

The saving is the **object tree**: every screen, tab, list row, label, style and
cached glyph, all of it live for the life of the session. That is the number
Phase 0 exists to measure, because it is the one nobody here can quote from
memory.

For scale on the V4, its PSRAM is also carrying up to four 128 KB map tiles, so
whatever LVGL is holding sits alongside that. Console mode frees the LVGL half
outright.

**This matters most exactly where you said it hurts.** The Heltec V4 has 2 MB of
PSRAM, and PSRAM pressure there is a recurring source of real bugs: map tiles
fragmenting the heap, the tile pool needing a cull and a cap, the extended Lua
SDK gated off the board entirely for want of memory. Console mode gives that
board its PSRAM back.

### Internal DRAM: a smaller saving than it looks

The scarce resource on these boards is internal DRAM, and LVGL is largely not
in it. What *is* in it is UITask's static data: the contact cache, message
rings, layout tables. Most of that is data the console still needs to do the
same job, so it does not simply disappear.

Two caveats worth keeping honest:

- There is a fallback at boot: if the PSRAM draw-buffer allocation fails,
  `malloc()` puts it in internal DRAM instead. On a board already under
  pressure that is exactly when it happens, so for the worst-off devices the
  DRAM saving is real after all.
- A meaningful DRAM number needs measuring, not guessing. See Phase 0.

### CPU: a real saving, and probably the one users feel

`lv_timer_handler()` runs on every pass of the main loop, alongside input-device
polling and any animation or scroll in flight, and a redraw blits through SPI.
A console redraws only when a line is added.

This is also the mechanism behind several bugs we have already fixed: the loop
being stalled by the UI is what starved the GPS UART ring, and what made SPIFFS
garbage collection visible as a freeze. Less time in the UI is less exposure to
that whole class of problem.

**How to describe the feature honestly:** it buys back PSRAM and CPU. On the
V4 that is the difference between comfortable and not. It is not a fix for
internal-DRAM exhaustion on its own.

---

## The hard parts

Not the console. These three.

### 1. Everything that assumes the UI exists

`src/main.cpp` calls `ui_task.showAlert(...)` in roughly twenty places to report
Wi-Fi and Bluetooth outcomes, and `ui_task.loop()` unconditionally. UITask is
also the owner of things the console still wants: the storage root, the Lua app
host, the update check.

The wrong fix is `#if` sprinkled through main. The right one is that UITask
keeps its role as the front end and gains a **headless personality**: the same
entry points, LVGL never initialised, `showAlert` becoming a console line.

### 2. Lua apps: the `wada.ui` translator

Decided: `wada.ui` gets a **console backend** behind the same names, so an app
keeps working without knowing which mode it is in.

This turns out to be far more achievable than "no UI" suggests, because console
mode is **not a text-only terminal**. It still owns a real pixel panel. The
thing being removed is LVGL's object tree, not the ability to draw.

The enabler is a raw framebuffer blit that already exists outside LVGL:

```
ST7789LCDDisplay::writePixelsRGB565(x, y, w, h, const uint16_t* pixels)
```

Present today on `ST7789LCDDisplay` (V4 TFT, T-Deck), `TanmatsuDisplay`,
`RM69A10Display` and `HI8561Display`. Missing only on `LGFXDisplay` (V4-R8) and
`ST7796LCDDisplay` (the pagers), and LovyanGFX has `pushImage` underneath, so
those are a wrapper method rather than new work.

That changes the answer on the two APIs that looked impossible:

| API | Console backend | Why it works |
|---|---|---|
| `label` | Text at a cursor. | `print` / `printWordWrap` / `drawTextEllipsized`. |
| `button` | A highlighted line with a selection index; activated by key or tap. | Text plus `fillRect` for the highlight. |
| `list` | Numbered lines, one selected. | Same. |
| `input` | A prompt, using the console's own keypad. | Phase 1 already builds it. |
| `text_w` / `text_lines` | `getTextWidth`, honestly measured. | Already the right shape. |
| `chart` | Drawn directly. | It is bars and lines; `fillRect` does it. |
| **`canvas`** | **Works.** The app already gets an RGB565 buffer and draws into it; console mode blits that buffer instead of handing it to LVGL. | `writePixelsRGB565`. |
| **`map`** | **Works.** Tiles are already decoded to RGB565 buffers (`MapTile.rgb565`); the projection and cache are ours and have no LVGL in them. | Same blit, plus markers as `fillRect`. |
| Everything non-UI | Unchanged. | `mesh`, `fs`, `net`, `crypto`, `geo`, `sys`, `timer`, `store` never touched LVGL. |

**What is genuinely lost is interaction, not drawing.** LVGL supplies scrolling
containers, a focus group, flex layout and event bubbling. The console backend
has to provide its own much simpler version: a line cursor, a selection index,
a scroll offset. That is the real work in this phase, and it is bounded.

So the honest expectation is that **most apps work**, including the graphical
ones, and a few that lean on LVGL behaviour we do not reimplement will not.
Apps already feature-detect through `wada.sys.caps()`, so anything that cannot
be supported is reported rather than broken, and `caps().console` lets an app
adapt its layout deliberately.

### 3. Getting back out

A boot flag that lands you in a console with no way back is a device that feels
bricked, and it will happen to somebody. This needs to be designed, not bolted
on. See [Safety](#safety).

---

## Plan

Each phase is shippable and useful on its own. Stop after any of them and
nothing is half-built.

### Phase 0: measure first

Before building anything, get the numbers the feature is being justified by.

- Free internal DRAM and free PSRAM at the home screen, on a V4 and a T-Deck.
- The same with `lv_timer_handler()` stubbed out and nothing built.
- Loop iterations per second, both ways.

Cheap, and it either confirms the case or changes what to build. If the DRAM
saving turns out to be negligible on the V4 and the PSRAM saving large, that is
worth knowing before the docs promise anything.

**Output:** real numbers in this file.

### Phase 1: the console front end

A `ConsoleUI` that owns the panel through `DisplayDriver`, with no LVGL.

- Scrollback ring in PSRAM, N lines, prune oldest.
- Prompt line, cursor, word wrap via `printWordWrap`.
- Input from the hardware keyboard where there is one.
- **An on-screen keypad on touch boards.** Decided: the V4 is the board that
  most needs its resources back, so console mode has to be usable there without
  a keyboard. The existing on-screen keyboard is LVGL and cannot be reused, so
  the console draws its own: `fillRect` + `drawRect` + `print` for the keys, and
  touch read straight from the board driver (`heltecV4CapTouchCheck()` and its
  equivalents), which is already independent of LVGL. `lvglTouchRead` is only an
  adapter feeding LVGL, so nothing needs to be untangled first.
- Commands go straight to `the_mesh.runLocalCli()`; output arrives through
  `setTerminalSink`. This part is close to free.
- One touch affordance, as you said: a back/exit target.

At the end of this phase the device is usable over the mesh from a console.

### Phase 2: boot into it, and back out

- A pref, `touchPrefsGetBootMode()`, read early in `setup()`.
- In console mode: never call `lv_init()`, never allocate the draw buffer, never
  build the UI. This is where the saving comes from, so it has to be a genuine
  skip and not a hidden LVGL instance.
- `ui_task.showAlert()` and friends route to the console.
- Commands to switch: `ui` reboots into the graphical mode, `console` reboots
  into this one. Also a Settings toggle in normal mode.

### Phase 3: the things the CLI does not cover

`CommonCLI` is a node CLI, not a messenger. Console mode needs conversational
commands, some of which the existing on-device terminal already has (`to <name>`
sets a recipient):

- `msg`, `to`, `reply`, unread counts, reading a thread's scrollback
- `contacts`, `chans`, `discover` with the results table
- `apps`, `run <id>`

Most of this is a thin layer over calls that already exist for the UI.

### Phase 4: Lua apps in the console

- A `WADA_UI_CONSOLE` backend behind the same `wada.ui` names, per the table
  above.
- `caps()` gains `console`, `canvas`, `map` so apps can adapt, and the docs say
  which apps work.
- Store apps audited; those that cannot work say so on launch rather than
  failing oddly.

### Phase 5: polish

- Command history and tab completion.
- Colour, where the panel has it.
- A `help` that is actually good, since there is no discoverable UI.
- Docs page, and a section in the user guide.

---

## Safety

The failure that matters is a device that boots to a console the owner cannot
leave. Three independent ways out, because one is not enough:

1. **A command.** `ui` then reboot.
2. **A key held at boot.** Any boot with the key down forces graphical mode
   regardless of the pref, the same shape as the existing recovery paths.
3. **The web flasher and the serial CLI** can always clear the pref.

And the pref should be stored so that a corrupt or unreadable value means
*graphical*, never console. Fail toward the mode everyone can use.

---

## Risks

| Risk | Containment |
|---|---|
| Console mode rots because nobody builds it | It compiles into every build from Phase 1, and SDK Test runs in it. A broken console fails the build, not a user. |
| Two front ends drift apart | The console consumes the same `runLocalCli` and the same MyMesh accessors the UI does. Do not let it grow a private copy of anything. |
| Touch-only boards get a console they cannot type into | Decide in Phase 1. Requiring a keyboard is a legitimate answer. |
| It quietly degrades normal mode | Everything new is behind the boot pref, and the graphical path keeps its current code path unchanged. Phase 0's numbers are the regression check. |
| Lua apps half-work and look broken | `caps()` first, refuse to launch second, document third. Less likely now that canvas and map can actually be drawn. |
| Two display drivers lack a raw blit | `LGFXDisplay` and `ST7796LCDDisplay` need a `writePixelsRGB565`; LovyanGFX's `pushImage` does the work. Until then `caps().canvas` is false on those boards and apps adapt. |

---

## Effort

Honest, assuming no surprises:

| Phase | Effort |
|---|---|
| 0 measure | Half a day |
| 1 console front end (incl. the drawn keypad) | 3 to 4 days |
| 2 boot mode and back out | 1 to 2 days |
| 3 messaging commands | 2 to 3 days |
| 4 `wada.ui` console backend | 4 to 6 days |
| 5 polish and docs | 2 days |

**A console you can actually use on the mesh: Phases 0 to 3, about a week.**
Phase 4 is the one that can grow, because it is the one with a design question
in it rather than just code.

---

## Open decisions

Worth settling before Phase 1 rather than during it.

1. ~~Keyboard required?~~ **Decided: no.** Touch boards get a drawn on-screen
   keypad, because the V4 is exactly the board this feature is for.
2. **Do Lua apps matter in v1?** Phases 0 to 3 are worth shipping without them.
   The translator is now a known quantity rather than a question mark, so it can
   follow safely.
3. **Does this replace the Terminal app** in graphical mode eventually, or do
   both stay?

---

## Audit: every UI surface, and its console equivalent

Done by enumerating the real thing rather than from memory: the tab list, the
app-drawer action enum (`APPACT_*`), the 19 settings categories, and the node
CLI's own command table in `CommonCLI.cpp`.

**The single most useful finding: the node CLI already covers far more than
expected.** `advert`, `board`, `clock`, `clock sync`, `erase`, `get`/`set`,
`gps` (on/off/sync/setloc/advert), `log`, `neighbors`, `ota url`, `password`,
`poweroff`, `powersaving`, `reboot`, `region`, `sensor list/get/set`,
`shutdown`, `start ota`, `tempradio`, `time`, `ver`. All of that arrives free,
which is why the console needed far fewer new commands than the UI has screens.

### Tabs and apps

| UI surface | Console | State |
|---|---|---|
| Chats: read a thread | `chat <name>` | **done** |
| Chats: send | `to <name>` + `msg <text>` | **done** |
| Chats: unread badges | `unread`, `read <name>` | **done**, and the monitor never clears them |
| Live incoming messages | the monitor, `monitor on\|off` | **done** |
| Contacts list | `contacts` | **done** |
| Contacts: favourite / block / delete | none | gap |
| Channels list | `chans` | **done** |
| Discover | `discover`, `discovered` | **done**, with both link directions |
| Advertise | CLI `advert` | free |
| Signal / traffic | `stat` | **done** |
| Map | none | needs the `wada.map` blit path; not console-shaped yet |
| Terminal | this *is* the console | n/a |
| Files | none | gap |
| Spectrum | none | gap, and heavy |
| Store / Lua apps | none | Phase 4 |
| Web reader | none | gap |
| VNC / Remote | none | deliberate: another front end, not a console feature |
| Power menu | CLI `poweroff` / `reboot` | free |
| Mentions | `unread` shows counts | partial |

### Settings, all 19 categories

| Category | Console | State |
|---|---|---|
| Radio & Mesh | CLI `get`/`set`, `region`, `tempradio` | free |
| GPS | CLI `gps *` | free |
| Clock & time | CLI `clock`, `time` | free |
| Sensors | CLI `sensor *` | free |
| About | CLI `ver`, `board`; `mem` | free |
| Battery | `batt` | **done** |
| Wi-Fi | `wifi` (state only) | partial |
| Bluetooth | none | gap |
| Auto-add | none | gap |
| Display / Keyboard / Sound / Lock screen | none | not meaningful without the UI |
| Quick replies | none | gap |
| Backups | none | gap |
| Language | none | gap |
| MQTT bridge | none | gap |
| App permissions | none | with Phase 4 |
| General | `console`/`ui` toggle | partial |

### What the gaps have in common

Almost everything still missing is either **Phase 4** (anything Lua), **not
console-shaped** (Display, Keyboard, Lock screen: settings for a UI that is not
running), or **a list-and-act flow** (files, backups, quick replies, contact
actions) that all wants the same small pattern: number the list, act on the
number. That pattern is worth building once rather than six times.

The two genuinely interesting gaps are **Files** and **contact actions**
(favourite, block, delete), because both are things people do reach for.
