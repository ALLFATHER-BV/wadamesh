#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""i18n audit: every TR("...") key in the touch UI must be covered by every
deploy/apps/lang/*.lang file (the canonical translation source — translators
edit those files and PR them; the firmware ships no baked-in table).

Run from the repo root:  python3 scripts/build/i18n-audit.py
Exit code 1 if any language is missing keys (prints them).
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def unescape_c(s):
    """Decode a C string literal body to the exact runtime text: escapes become
    BYTES (like the compiler emits), normal chars their UTF-8 bytes, then the
    whole buffer decodes as UTF-8 — so "\xE2\x80\xA6" and a literal … agree."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n': out.append(0x0A); i += 2
            elif n == 't': out.append(0x09); i += 2
            elif n == 'x': out.append(int(s[i + 2:i + 4], 16)); i += 4
            else: out += n.encode('utf-8'); i += 2
        else:
            out += s[i].encode('utf-8'); i += 1
    return out.decode('utf-8', 'replace')

def source_keys():
    keys = set()
    for f in glob.glob(os.path.join(ROOT, 'src/ui-touch/*.cpp')) + \
             glob.glob(os.path.join(ROOT, 'src/ui-touch/*.h')):
        src = open(f, encoding='utf-8').read()
        # An optional LV_SYMBOL_* macro may precede the literal: TR(LV_SYMBOL_OK "  Text").
        # Requiring the group to START with a quote silently skipped every such
        # call - the audit's worst blind spot (the sheet rows never got checked).
        for m in re.finditer(r'TR\(\s*((?:LV_SYMBOL_[A-Z0-9_]+\s+)?(?:"(?:[^"\\]|\\.)*"\s*)+)\)', src):
            parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
            k = unescape_c(''.join(parts))
            # strip icon glyphs + their spacer, exactly as TR() does at runtime
            while k and 0xE000 <= ord(k[0]) <= 0xF8FF:
                k = k[1:]
            k = k.lstrip(' ')
            if k.strip():
                keys.add(k)
    keys |= {"Apps", "Built-in", "Languages"}   # TR(kStoreTabs[t])
    return keys

# Known untranslatable / identical-everywhere keys.
SKIP = {'...', '😊', 'Snake', '© OpenStreetMap', '© OpenTopoMap'}

def lang_keys(path):
    have = set()
    for line in open(path, encoding='utf-8'):
        # Header lines have no tab; '#' can legitimately START a key (LVGL
        # recolor markup), so never treat a leading '#' as a comment marker.
        if '\t' not in line:
            continue
        k = line.split('\t', 1)[0]
        have.add(k.replace('\\n', '\n').replace('\\t', '\t').replace('\\\\', '\\'))
    return have

def main():
    keys = source_keys() - SKIP
    bad = 0
    for path in sorted(glob.glob(os.path.join(ROOT, 'deploy/apps/lang/*.lang'))):
        missing = sorted(k for k in keys if k not in lang_keys(path))
        tag = os.path.basename(path)
        if missing:
            bad += 1
            print(f"{tag}: MISSING {len(missing)}")
            for k in missing:
                print(f"  {k!r}")
        else:
            print(f"{tag}: ok ({len(keys)} keys covered)")
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
