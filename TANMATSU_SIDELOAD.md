# Sideloading wadamesh on the Tanmatsu

For developers building wadamesh from source and putting it on a Tanmatsu.
If you just want to run it, install **WadaMesh** from the launcher's app store
instead; this document is about running your own build.

## Understand the model first, or you will brick the launcher

wadamesh is **not** standalone firmware on this board. The Tanmatsu boots
badge.team's launcher, and wadamesh runs as an **AppFS app** underneath it,
exactly like the launcher's own apps.

So you never flash the whole device. You write **one app image plus one metadata
sector** into the AppFS partition and leave the bootloader, the partition table
and the OTA slots alone.

> **Never run `idf.py flash` on a Tanmatsu.** It writes the bootloader, partition
> table and OTA data, which replaces the launcher OS itself. Recovering means
> reflashing the launcher from badge.team. Every command in this guide touches
> only the AppFS partition.

The board has three processors and only one of them is yours: the **ESP32-P4**
runs the launcher and your app, an **ESP32-C6** handles Wi-Fi, Bluetooth and LoRa
over esp-hosted, and a **CH32** runs the keyboard and power. You flash the P4.

## One-time setup

You need ESP-IDF 5.5.1. The build script uses a project-local copy in
`tanmatsu/esp-idf` (gitignored, around 3 GB); `make prepare` from `tanmatsu/`
fetches it.

Then two one-off steps:

```bash
tanmatsu/tools/fetch-appfs.sh
```

This clones badge.team's `esp32-component-appfs`, whose `appfs.py` reads and
rewrites the AppFS image. It is not vendored into this repo on purpose: it is
their code, and the copy in circulation has no licence header.

```bash
tanmatsu/tools/dump-pristine.sh /dev/cu.usbmodemXXXX
```

This dumps **your** device's AppFS partition as the baseline that later deploys
diff against. Do it while the launcher's own apps are installed and wadamesh is
not, so the baseline is the device as it ships.

Use your own dump, not someone else's. The deploy writes only the sectors that
differ from this file, so if it describes a different device you can overwrite
apps you actually have.

## Build

```bash
cd tanmatsu && ./build.sh build
```

Use `build.sh`, not `idf.py` directly: it applies two build-time patches to
`managed_components` (esp-hosted Wi-Fi-init tolerance, and an Arduino BLE stub)
that the build needs.

**The build ends with an error and that is expected:**

```
Generated .../application.bin
Error: All app partitions are too small
```

The ~2.9 MB app is being size-checked against the 2 MB `ota_0` slot, but it does
not live there: it lives in the 8 MB `appfs` partition. Judge success by
`Generated ... application.bin` appearing with no `error:` or `undefined
reference` above it.

## Install

```bash
tanmatsu/tools/tan_flash.sh
```

With no port argument it probes each USB serial device and picks the one that
answers as an ESP32-P4. The Tanmatsu exposes **two** ports, the P4 and the C6,
and which name each gets changes between replugs, so detecting by chip beats
guessing. Pass a port explicitly if you prefer.

What it does, and why in this order:

1. Builds the AppFS image from your pristine baseline plus the new
   `application.bin`, and works out the minimum set of 64 KB sectors that changed.
2. Writes the **app data first**, with no reset afterwards.
3. Writes the **metadata sector last**, then resets. That sector is what makes the
   app visible to the launcher, so writing it last means an interrupted flash
   leaves the previous app intact instead of a half-written one marked complete.
4. Reads the metadata back and compares it. The commit write is the one that can
   silently drop, and without this check the launcher would quietly keep showing
   the old build.

Expect `metadata verified — installed`. Anything else means re-run it.

The version number increments automatically on each deploy. It has to: the
launcher keys updates on name plus version, so republishing the same number
leaves you on the old binary with no error anywhere.

## Verify

Launch **WadaMesh** from the launcher menu. An empty serial log immediately after
flashing is normal, because the launcher is a silent GUI OS on USB-CDC; the boot
log appears once the app itself starts.

```bash
python3 tanmatsu/tools/sermon.py /dev/cu.usbmodemXXXX /tmp/wada.log
```

`sermon.py` is a non-resetting monitor that survives the USB re-enumeration when
an app launches. Prefer it to a monitor that asserts DTR/RTS: **resetting the P4
also desynchronises the C6**, which has no reset line of its own, and the launcher
then hangs on "Initializing radio" until you fully power-cycle the board.

## When it goes wrong

**No serial ports at all.** The C6 has wedged. Fully power-cycle the board, off
then on. A reset is not enough.

**"No ESP32-P4 found"** with ports present. Something else is holding the port,
often a still-running monitor. Close it and retry.

**"metadata DIFFERS".** The commit write did not land. Re-run; nothing is broken,
the device still has the previous app.

**A crash to decode.**

```bash
riscv32-esp-elf-addr2line -e tanmatsu/build/tanmatsu/application.elf -fpC <MEPC> <RA> <stack RAs>
```

## Before you ship anything

The Tanmatsu shares one `src/` with the ESP32-S3 boards, so a change here can
break them without you noticing. Build at least:

```bash
pio run -e LilyGo_TDeck_companion_radio_touch
pio run -e heltec_v4_tft_companion_radio_usb_tcp_touch
```

## Related

- `TANMATSU_PORT.md` — the port's status and outstanding work
- [wadamesh.com/sdk.html](https://wadamesh.com/sdk.html) — writing Lua apps, which
  needs no firmware build at all
