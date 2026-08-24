#!/usr/bin/env python3
"""Audit a .lang file against the TR() keys the firmware actually looks up.

Nothing validated this before: the .lang files are hand-maintained and
i18n_builtin.h is generated FROM them, so a row whose English text no longer
appears in any TR() call is translated for nothing and silently stays that way
(and a TR() key with no row silently renders English). This diffs both ways.

Matching mirrors TR() (src/ui-touch/i18n.cpp) exactly, which is the whole
trick — a naive grep gets this wrong three ways:
  * adjacent C literals are concatenated by the compiler ("a" "b" -> "ab"),
  * LV_SYMBOL_* icon prefixes are part of the source literal but TR() strips
    the glyph and following spaces before looking up,
  * non-ASCII is often written as \\x hex escapes, which must be decoded to the
    same UTF-8 bytes the .lang file stores literally.

Usage:  scripts/build/audit-lang.py [deploy/apps/lang/hu.lang ...]
        (no args = every .lang file)
"""
import re, sys, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def c_unescape(s: str) -> str:
    """Decode a C string literal body to the bytes TR() receives, as UTF-8 text."""
    out = bytearray(); i = 0
    while i < len(s):
        if s[i] != '\\':
            out += s[i].encode('utf-8'); i += 1; continue
        i += 1
        if i >= len(s): break
        c = s[i]
        if c == 'x':                       # \xNN
            j = i + 1
            while j < len(s) and j <= i + 2 and s[j] in '0123456789abcdefABCDEF': j += 1
            out.append(int(s[i+1:j], 16)); i = j
        else:
            out += {'n': b'\n', 't': b'\t', 'r': b'\r', '\\': b'\\',
                    '"': b'"', "'": b"'", '0': b'\0'}.get(c, ('\\' + c).encode())
            i += 1
    return out.decode('utf-8', 'replace')

def strip_icon(s: str) -> str:
    """TR()'s prefix strip: drop leading 3-byte 0xEE/0xEF glyphs, then spaces."""
    b = s.encode('utf-8'); i = 0
    while i + 2 < len(b) and b[i] in (0xEE, 0xEF): i += 3
    if i:
        while i < len(b) and b[i:i+1] == b' ': i += 1
        return b[i:].decode('utf-8', 'replace') or s
    return s

# --- every TR( ... ) argument in the UI sources, concatenation-aware ----------
LIT = r'"((?:[^"\\]|\\.)*)"'
def tr_keys():
    keys = set()
    for f in glob.glob(f'{ROOT}/src/**/*.cpp', recursive=True) + \
             glob.glob(f'{ROOT}/src/**/*.h',   recursive=True):
        if 'i18n' in os.path.basename(f):      # the generated table + engine itself
            continue
        src = open(f, encoding='utf-8', errors='ignore').read()
        for m in re.finditer(r'TR\(\s*((?:(?:LV_SYMBOL_\w+|' + LIT + r')\s*)+)', src):
            parts = re.findall(LIT, m.group(1))
            if parts:
                keys.add(strip_icon(c_unescape(''.join(parts))).strip())
        # Labels reached through a variable — TR(kSettingsCats[c].label), TR(d->name).
        # The literals live in the table's initialiser, so resolve the base
        # identifier and harvest every string in its definition block. Without
        # this the whole tab/category/menu vocabulary looks untranslated.
        for ident in set(re.findall(r'TR\(\s*([A-Za-z_]\w*)\s*[\[.\-]', src)):
            for tm in re.finditer(re.escape(ident) + r'\s*(?:\[[^\]]*\])?\s*=\s*\{', src):
                depth, i = 0, tm.end() - 1
                while i < len(src):                       # walk to the matching brace
                    if src[i] == '{': depth += 1
                    elif src[i] == '}':
                        depth -= 1
                        if depth == 0: break
                    i += 1
                for lit in re.findall(LIT, src[tm.end():i]):
                    keys.add(strip_icon(c_unescape(lit)).strip())
    return keys

# Ordered printf conversions in a string, as the (length-modifier, conversion)
# pairs that decide each vararg's TYPE. Mirrors i18nNextSpec() in i18n.cpp — keep
# the two in step. "%%" is a literal percent, '*' takes its own int argument.
_SPEC = re.compile(r"%[-+ #0]*(\*|[0-9]*)(?:\.(\*|[0-9]*))?(hh|h|ll|l|j|z|t|L)?([diouxXeEfgGaAcsp%])")
def fmt_specs(s):
    out = []
    for w, p, ln, cv in _SPEC.findall(s):
        if cv == '%': continue          # literal '%%', consumes no argument
        if w == '*': out.append('*')
        if p == '*': out.append('*')
        out.append((ln or '') + cv)
    return out

def audit(path, keys):
    rows = []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        # A line is a header/comment only when it has NO tab — same rule as the
        # loader. Testing for a leading '#' would drop every key that starts with
        # LVGL recolor markup ("#7A7F87 Wardrive: …#"), which are real keys.
        if '\t' not in line: continue
        en, tr = line.split('\t', 1)
        rows.append((c_unescape(en), tr))
    dead    = [en for en, tr in rows if en not in keys]
    # Keys with no letters (a lone emoji, a glyph-only button) translate to
    # themselves — not worth a row, and not a gap.
    missing = sorted(k for k in keys
                     if k and k not in {en for en, _ in rows} and re.search(r'[A-Za-z]', k))
    # UNSAFE rows: TR()'s result is handed to snprintf as the FORMAT string, so a
    # translation whose conversions differ from the key's makes the caller misread
    # its varargs. A Hungarian row that reordered "%s …%lu" to "%lu …%s" rebooted
    # the device on every repeater login (#258) — snprintf took an integer as a
    # char* and dereferenced it. The runtime now falls back to English on a
    # mismatch, so this is no longer fatal, but a flagged row means that string
    # silently shows English: still a bug, just a survivable one.
    unsafe = [(en, tr) for en, tr in rows
              if tr.strip() and fmt_specs(en) != fmt_specs(tr)]
    name = os.path.basename(path)
    print(f'\n=== {name} — {len(rows)} rows vs {len(keys)} TR() keys ===')
    print(f'  translated but NEVER looked up : {len(dead)}')
    print(f'  looked up but NOT translated   : {len(missing)}')
    print(f'  UNSAFE format mismatch         : {len(unsafe)}')
    for e in dead:    print('    dead     ', repr(e)[:110])
    for e in missing: print('    missing  ', repr(e)[:110])
    for en, tr in unsafe:
        print(f'    UNSAFE    {en!r}\n              key{fmt_specs(en)} vs translation{fmt_specs(tr)}: {tr!r}')
    return len(dead), len(missing), len(unsafe)

if __name__ == '__main__':
    keys = tr_keys()
    files = sys.argv[1:] or sorted(glob.glob(f'{ROOT}/deploy/apps/lang/*.lang'))
    worst = 0
    for f in files:
        d, m, u = audit(f, keys)
        worst = max(worst, u)
    print(f'\n{len(keys)} distinct TR() keys in the UI.')
    if worst:
        print(f'\nFAIL: {worst} unsafe format row(s) — these crash pre-beta_63 firmware.')
        sys.exit(1)
