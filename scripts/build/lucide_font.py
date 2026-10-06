#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lucide line icons for the touch UI fonts (used by gen-touch-fonts.sh).

  lucide_font.py ranges <codepoints.json> <set>
      Print lv_font_conv range arguments ("-r", "0xE0F5=>0xF015", ...) that pull
      the icons of <set> out of lucide.ttf and place them on their target
      codepoints.

  lucide_font.py header <codepoints.json>
      Print ui_icons.h: a UI_ICON_* string literal per app icon.

  lucide_font.py metrics <text.c>
      Print "<line_height> <base_line> <cap_height>" of a Montserrat cut: its
      line metrics, and the height of its capitals (the box of H, or of A when
      the cut has only that letter).

  lucide_font.py patch <font.c> <icons> <line_height> <base_line> <cap_height>
      Fix up a font lv_font_conv generated with <icons> Lucide glyphs (always
      the highest codepoints, so the last glyph ids; all of them in an icon-only
      face): centre every icon's drawing on the middle of the capitals, so icons
      share one centre line with each other and with the text beside them, and
      set the text's line metrics, which the taller icon boxes would otherwise
      grow by a pixel and shift every layout.

Sets:
  lv_symbols  LVGL's LV_SYMBOL_* codepoints, which every widget and most of the
              firmware use for its icons. Drawing Lucide there re-skins all of
              them without touching a call site.
  app         The app and launcher icons, on Lucide's own codepoints (U+E000
              and up, clear of the FontAwesome range above).
  person, zoom, sleep, cc
              The codepoints of the small icon fonts the UI chains in as
              fallbacks (TOUCH_SYM_* in UITask.cpp).
"""
import json
import re
import sys

LV_SYMBOLS = {
    0xF001: "music", 0xF008: "video", 0xF00B: "list", 0xF00C: "check",
    0xF00D: "x", 0xF011: "power", 0xF013: "settings", 0xF015: "house",
    0xF019: "download", 0xF01C: "hard-drive", 0xF021: "refresh-cw",
    0xF026: "volume-x", 0xF027: "volume-1", 0xF028: "volume-2",
    0xF03E: "image", 0xF043: "droplet", 0xF048: "skip-back", 0xF04B: "play",
    0xF04C: "pause", 0xF04D: "square", 0xF051: "skip-forward", 0xF052: "eject",
    0xF053: "chevron-left", 0xF054: "chevron-right", 0xF067: "plus",
    0xF068: "minus", 0xF06E: "eye", 0xF070: "eye-off", 0xF071: "triangle-alert",
    0xF074: "shuffle", 0xF077: "chevron-up", 0xF078: "chevron-down",
    0xF079: "repeat", 0xF07B: "folder", 0xF093: "upload", 0xF095: "phone",
    0xF0C4: "scissors", 0xF0C5: "copy", 0xF0C7: "save", 0xF0C9: "menu",
    0xF0E0: "mail", 0xF0E7: "zap", 0xF0EA: "clipboard", 0xF0F3: "bell",
    0xF11C: "keyboard", 0xF124: "navigation", 0xF15B: "file", 0xF1EB: "wifi",
    # FontAwesome has five battery levels, Lucide four: 3/4 and 1/2 share one.
    0xF240: "battery-full", 0xF241: "battery-medium", 0xF242: "battery-medium",
    0xF243: "battery-low", 0xF244: "battery", 0xF287: "usb",
    0xF293: "bluetooth", 0xF2ED: "trash-2", 0xF304: "pencil",
    0xF55A: "delete", 0xF7C2: "card-sim", 0xF8A2: "corner-down-left",
}

# Launcher tiles, tab bar, chat chrome. Kept on Lucide's own codepoints; the
# firmware names them in ui_icons.h, which this list must stay in step with.
APP = [
    "message-square", "user", "users", "at-sign", "map-pin", "map", "radio",
    "signal", "activity", "audio-lines", "square-terminal", "monitor", "cast",
    "sliders-horizontal", "settings", "house", "folder", "layout-grid",
    "gamepad-2", "globe", "radar", "shield-check", "usb", "qr-code", "power",
    "compass", "satellite", "wifi", "bluetooth", "lock", "bell", "bell-off",
    "sun", "moon", "search", "arrow-up", "check", "check-check",
    "chevron-left", "chevron-right", "x", "plus", "ellipsis", "hash", "mail",
    "refresh-cw", "trash-2", "pencil", "zap", "battery-charging",
    "thermometer", "gauge", "info", "link", "star", "keyboard", "file-text",
    "store", "waves", "antenna", "route", "radio-tower", "crosshair",
    "languages", "palette", "timer", "history", "copy", "eye", "smartphone",
    "chart-no-axes-column", "message-square-text", "reply", "share-2",
    "smile", "send-horizontal", "arrow-left", "battery-full", "battery-medium",
    "battery-low", "battery", "image", "lock-open", "bell-ring", "circle-alert",
    "arrow-down-wide-narrow", "user-plus", "list-checks", "ban",
]

FA_PERSON = {0xF007: "user", 0xF519: "radio-tower", 0xF0C0: "users"}
FA_ZOOM = {0xF002: "search"}
FA_SLEEP = {0xF185: "sun", 0xF186: "moon"}
FA_CC = {0xF023: "lock", 0xF0F3: "bell", 0xF1F6: "bell-off"}


def icon_map(codepoints, which):
    """target codepoint -> Lucide codepoint, for one set."""
    def cp(name):
        if name not in codepoints:
            sys.exit(f"lucide_font.py: no Lucide icon named {name!r}")
        return codepoints[name]

    if which == "lv_symbols":
        return {t: cp(n) for t, n in LV_SYMBOLS.items()}
    if which == "app":
        return {cp(n): cp(n) for n in APP}
    named = {"person": FA_PERSON, "zoom": FA_ZOOM, "sleep": FA_SLEEP, "cc": FA_CC}
    if which in named:
        return {t: cp(n) for t, n in named[which].items()}
    sys.exit(f"lucide_font.py: unknown icon set {which!r}")


def cmd_ranges(cp_path, which):
    codepoints = json.load(open(cp_path, encoding="utf-8"))
    out = []
    for target, src in sorted(icon_map(codepoints, which).items()):
        out += ["-r", f"0x{src:X}=>0x{target:X}" if src != target else f"0x{src:X}"]
    print("\n".join(out))


def cmd_count(cp_path, which):
    codepoints = json.load(open(cp_path, encoding="utf-8"))
    print(len(icon_map(codepoints, which)))


def cmd_patch(path, icons, line_height, base_line, cap_height):
    icons, line_height, base_line = int(icons), int(line_height), int(base_line)
    cap = int(cap_height)
    src = open(path, encoding="utf-8").read()
    start = src.index("glyph_dsc[] = {")
    end = src.index("};", start)
    body = src[start:end]
    entries = list(re.finditer(r"\{\.bitmap_index = [^}]*\}", body))
    if icons > len(entries) - 1:
        sys.exit(f"lucide_font.py: {path} has fewer glyphs than {icons} icons")
    # Each Lucide drawing fills a different part of its 24-unit square, so one
    # common shift leaves them scattered by a pixel or two (the status bar's
    # battery sat 2 px above the Wi-Fi beside it). lv_font_conv crops every glyph
    # to its ink, so placing the box itself puts the drawing exactly: its middle
    # on the middle of the capitals, rounding half a pixel down.
    pieces, last = [], 0
    for m in entries[len(entries) - icons:]:
        e = m.group(0)
        box_h = int(re.search(r"\.box_h = (\d+)", e).group(1))
        oy = (cap - box_h) // 2
        pieces.append(body[last:m.start()])
        pieces.append(re.sub(r"\.ofs_y = -?\d+", f".ofs_y = {oy}", e))
        last = m.end()
    pieces.append(body[last:])
    src = src[:start] + "".join(pieces) + src[end:]
    src, n1 = re.subn(r"\.line_height = \d+,", f".line_height = {line_height},", src, count=1)
    src, n2 = re.subn(r"\.base_line = -?\d+,", f".base_line = {base_line},", src, count=1)
    if n1 != 1 or n2 != 1:
        sys.exit(f"lucide_font.py: no line metrics in {path}")
    open(path, "w", encoding="utf-8").write(src)


def cmd_header(cp_path):
    codepoints = json.load(open(cp_path, encoding="utf-8"))
    print("// SPDX-License-Identifier: GPL-3.0-or-later")
    print("// Generated by scripts/build/gen-touch-fonts.sh (lucide_font.py header).")
    print("// The app icons in ui_icons_16 / ui_icons_20, as UTF-8 string literals:")
    print("// Lucide 1.52.0, on Lucide's own codepoints.")
    print("#pragma once")
    print()
    for name in APP:
        c = codepoints[name]
        utf8 = "".join(f"\\x{b:02X}" for b in chr(c).encode("utf-8"))
        macro = "UI_ICON_" + re.sub(r"[^A-Z0-9]", "_", name.upper())
        print(f'#define {macro:<32} "{utf8}"   /* U+{c:04X} {name} */')


def glyph_box_h(src, cp):
    """box_h of codepoint cp in a generated font, or None when it has no such glyph."""
    entries = re.findall(r"\{\.bitmap_index = [^}]*\}", src[src.index("glyph_dsc[] = {"):])
    for m in re.finditer(r"\.range_start = (\d+), \.range_length = (\d+), "
                         r"\.glyph_id_start = (\d+),[^}]*?\.type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY", src):
        start, length, gid = (int(v) for v in m.groups())
        if start <= cp < start + length:
            return int(re.search(r"\.box_h = (\d+)", entries[gid + cp - start]).group(1))
    return None


def cmd_metrics(path):
    src = open(path, encoding="utf-8").read()
    lh = re.search(r"\.line_height = (\d+),", src).group(1)
    bl = re.search(r"\.base_line = (-?\d+),", src).group(1)
    cap = glyph_box_h(src, ord("H"))
    if cap is None:
        cap = glyph_box_h(src, ord("A"))
    if cap is None:
        sys.exit(f"lucide_font.py: no H or A in {path} to measure the capitals")
    print(lh, bl, cap)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "ranges" and len(sys.argv) == 4:
        cmd_ranges(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 2 and sys.argv[1] == "count" and len(sys.argv) == 4:
        cmd_count(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 2 and sys.argv[1] == "patch" and len(sys.argv) == 7:
        cmd_patch(*sys.argv[2:])
    elif len(sys.argv) >= 2 and sys.argv[1] == "header" and len(sys.argv) == 3:
        cmd_header(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == "metrics" and len(sys.argv) == 3:
        cmd_metrics(sys.argv[2])
    else:
        sys.exit(__doc__)
