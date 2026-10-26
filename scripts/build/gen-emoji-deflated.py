#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deflate the baked colour emoji (src/ui-touch/emoji_data.c) into one blob.

emoji_data.c stays the source of truth (gen-emoji-font.py and add-emoji.py write
it). Its 273 RGB565+alpha images are ~209 KB of flash; deflated as ONE stream
they are ~105 KB. The Tanmatsu's AppFS app may not exceed 50 x 64 KB pages, so
its IDF build runs this at build time and compiles variants/tanmatsu/
emoji_deflated.cpp instead of emoji_data.c: the same images, inflated once into
PSRAM on the first lookup. Nothing generated here is committed.

    gen-emoji-deflated.py <emoji_data.c> <out.h>

The output is verified before it is written: the blob is inflated again and
every image must come back byte for byte, every glyph must point at the image
the source maps it to, and the glyph table must be sorted (the lookup is a
binary search).
"""
import re, sys, zlib


def parse(src):
    arrays = {}
    for name, body in re.findall(r'static const uint8_t (\w+)\[\] = \{([^}]*)\};', src):
        arrays[name] = bytes(int(x) for x in body.replace(' ', '').replace('\n', '').split(',') if x)
    dscs = {}
    pat = (r'static const lv_img_dsc_t (\w+) = \{ \{ (LV_IMG_CF_\w+), 0, 0, (\d+), (\d+) \}, '
           r'sizeof\((\w+)\), (\w+) \};')
    for d, cf, w, h, szof, data in re.findall(pat, src):
        if szof != data or data not in arrays:
            sys.exit('gen-emoji-deflated: %s does not describe its own array' % d)
        dscs[d] = (cf, int(w), int(h), data)
    table = re.search(r'static const EmojiGlyph kGlyphs\[\] = \{(.*?)\};', src, re.S)
    if not table:
        sys.exit('gen-emoji-deflated: no kGlyphs table in the source')
    glyphs = [(int(cp, 16), d) for cp, d in re.findall(r'\{ 0x([0-9A-Fa-f]+)u, &(\w+) \}', table.group(1))]
    count = re.search(r'const uint16_t kEmojiGlyphCount = (\d+);', src)
    if not glyphs or not count:
        sys.exit('gen-emoji-deflated: emoji_data.c changed shape; update this parser')
    for _, d in glyphs:
        if d not in dscs:
            sys.exit('gen-emoji-deflated: glyph table names unknown image %s' % d)
    cps = [cp for cp, _ in glyphs]
    if cps != sorted(cps) or len(set(cps)) != len(cps):
        sys.exit('gen-emoji-deflated: glyph table is not strictly sorted')
    return arrays, dscs, glyphs, int(count.group(1))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding='utf-8').read()
    arrays, dscs, glyphs, count = parse(src)

    # Images in source order (similar emoji sit together there, which deflates
    # ~1.5 KB better than glyph-table order), each stored once. An image no glyph
    # points at is dropped, as the linker would drop it from emoji_data.c.
    used = set(d for _, d in glyphs)
    order = [d for d in re.findall(r'static const lv_img_dsc_t (\w+) =', src) if d in used]
    index = {d: i for i, d in enumerate(order)}
    raw, imgs = bytearray(), []
    for d in order:
        cf, w, h, data = dscs[d]
        imgs.append((len(raw), len(arrays[data]), w, h, cf))
        raw += arrays[data]
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 8)
    z = c.compress(bytes(raw)) + c.flush()

    # Self-check: what the device will inflate must be exactly the source images.
    back = zlib.decompress(z, -15)
    if back != bytes(raw):
        sys.exit('gen-emoji-deflated: round trip failed')
    for cp, d in glyphs:
        off, ln, w, h, cf = imgs[index[d]]
        if back[off:off + ln] != arrays[dscs[d][3]] or (cf, w, h) != dscs[d][:3]:
            sys.exit('gen-emoji-deflated: glyph U+%X does not match its source image' % cp)

    if len(raw) >= 1 << 32 or any(ln >= 1 << 16 or w >= 1 << 16 or h >= 1 << 16 for _, ln, w, h, _ in imgs):
        sys.exit('gen-emoji-deflated: an image no longer fits the table fields')
    w_ = ['// Generated at build time by scripts/build/gen-emoji-deflated.py from',
          '// src/ui-touch/emoji_data.c. Do not edit, do not commit.',
          '#pragma once', '#include <stdint.h>', '#include "lvgl.h"', '',
          '#define EMOJI_Z_RAW_LEN %d' % len(raw),
          '#define EMOJI_Z_IMG_COUNT %d' % len(imgs),
          '#define EMOJI_Z_GLYPH_COUNT %d' % count, '',
          'struct EmojiZImg { uint32_t off; uint16_t len; uint16_t w; uint16_t h; uint8_t cf; };',
          'struct EmojiZGlyph { uint32_t cp; uint16_t img; };', '',
          'static const uint8_t kEmojiZ[%d] = {  // %d B of images' % (len(z), len(raw))]
    for i in range(0, len(z), 24):
        w_.append('  ' + ','.join('0x%02x' % x for x in z[i:i + 24]) + ',')
    w_.append('};')
    w_.append('static const EmojiZImg kEmojiZImgs[EMOJI_Z_IMG_COUNT] = {')
    for off, ln, w, h, cf in imgs:
        w_.append('  { %d, %d, %d, %d, %s },' % (off, ln, w, h, cf))
    w_.append('};')
    w_.append('static const EmojiZGlyph kEmojiZGlyphs[] = {')
    for cp, d in glyphs:
        w_.append('  { 0x%X, %d },' % (cp, index[d]))
    w_.append('};')
    w_.append('')
    # Always written: the build reruns this only when emoji_data.c (or this script)
    # is newer than the header, and a fresh mtime is what tells it the header is current.
    open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(w_))
    print('emoji: %d glyphs, %d images, %d B deflated from %d B' % (len(glyphs), len(imgs), len(z), len(raw)))


if __name__ == '__main__':
    main()
