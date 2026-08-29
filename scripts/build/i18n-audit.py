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
        src = strip_comments(open(f, encoding='utf-8').read())
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
    # INDIRECT keys: TR(kSomeTable[i]) / TR(tbl[i].field). The regex above only sees
    # TR("literal"), so every string reached through a table looked unused — including
    # "About", "Backups" and every other settings-category name. Anyone pruning the
    # language files on that output would have deleted live translations, which is
    # exactly what a translator asked about before doing it (#262).
    #
    # So: find the table names that appear inside a TR(...) subscript, then pull every
    # string literal out of each table's initialiser. Deliberately greedy — over-
    # collecting keeps a translation alive, under-collecting deletes one.
    for f in glob.glob(os.path.join(ROOT, 'src/ui-touch/*.cpp')) + \
             glob.glob(os.path.join(ROOT, 'src/ui-touch/*.h')):
        src = strip_comments(open(f, encoding='utf-8').read())
        for name in set(re.findall(r'TR\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[', src)):
            for m in re.finditer(r'\b' + re.escape(name) + r'\s*\[[^\]]*\]\s*=\s*\{', src):
                i, depth = m.end(), 1
                while i < len(src) and depth:
                    if src[i] == '{': depth += 1
                    elif src[i] == '}': depth -= 1
                    i += 1
                for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src[m.end():i]):
                    k = unescape_c(lit)
                    if k.strip():
                        keys.add(k)

    # HELPER-WRAPPED keys: mk_row_btn("Reload tiles in view", cb) and its ~20
    # siblings take a raw literal and call TR() on it *inside* the helper. At
    # runtime they translate correctly -- but the extractor above only ever saw
    # TR("literal"), so the literal at the CALL SITE was never emitted as a key,
    # so it never landed in a .lang file, so no translator could ever supply it.
    # That is why a whole map/options/settings sheet stayed English in every
    # language while looking perfectly wrapped in the source (#257).
    #
    # Two passes: find every function or lambda that calls TR() on one of its own
    # parameters and note which parameter, then pull the literal out of that
    # argument position at every call site.
    for f in _sources():
        src = strip_comments(open(f, encoding='utf-8').read())
        for name, idx in _tr_wrapping_helpers(src):
            for arg in _call_args_at(src, name, idx):
                for lit in literal_groups(arg):
                    k = unescape_c(lit)
                    while k and 0xE000 <= ord(k[0]) <= 0xF8FF:
                        k = k[1:]
                    k = k.lstrip(' ')
                    if k.strip():
                        keys.add(k)

    # RANGE-FOR over a local table: `struct {...} rows[] = {{"Show contacts", ...}};`
    # then `for (auto& r : rows) TR(r.label)`. Same blind spot as the helper case --
    # correct at runtime, invisible to a TR("literal") scan, so untranslatable.
    for f in _sources():
        src = strip_comments(open(f, encoding='utf-8').read())
        for m in re.finditer(r'for\s*\(\s*(?:const\s+)?auto\s*&?\s*(\w+)\s*:\s*(\w+)\s*\)', src):
            var, tbl = m.group(1), m.group(2)
            brace = src.find('{', m.end())
            if brace < 0 or brace - m.end() > 8:   # single-statement body, no block
                brace = -1
            body = src[m.end():_balanced(src, brace)] if brace >= 0 else src[m.end():m.end() + 400]
            if not re.search(r'\bTR\(\s*' + re.escape(var) + r'\s*(?:\.|->)', body):
                continue
            init = None
            for d in re.finditer(r'\b' + re.escape(tbl) + r'\s*\[[^\]]*\]\s*=\s*\{', src[:m.start()]):
                init = d
            if init:
                seg = src[init.end():_balanced(src, init.end() - 1)]
                for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', seg):
                    k = unescape_c(lit)
                    if k.strip():
                        keys.add(k)

    # TR(someFunc(...)): a helper that RETURNS one of several literals, translated
    # by the caller. Every literal it can return is a key.
    for f in _sources():
        src = strip_comments(open(f, encoding='utf-8').read())
        for name in set(re.findall(r'TR\(\s*([A-Za-z_]\w*)\s*\(', src)):
            for d in re.finditer(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', src):
                seg = src[d.end() - 1:_balanced(src, d.end() - 1)]
                for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', seg):
                    k = unescape_c(lit)
                    if k.strip():
                        keys.add(k)
    # LUA APPS: wada.sys.tr("...") (aliased to a local `tr` by convention). The
    # apps ship from the same catalog and their strings belong in the same files.
    for f in sorted(glob.glob(os.path.join(ROOT, 'deploy/apps/*/*/*.lua'))):
        src = strip_comments(open(f, encoding='utf-8').read())
        for lit in re.findall(r'\b(?:sys\.)?tr\(\s*"((?:[^"\\]|\\.)*)"', src):
            k = unescape_c(lit)
            if k.strip():
                keys.add(k)
    return keys


def strip_comments(src):
    """Blank out //... and /*...*/ while preserving string/char literals and line
    count. The extractor scans raw text, so a quoted phrase inside a comment was
    being collected as a translatable key -- that is how the Dutch string
    "Geblokkeerde gebruikers", which only appears in a comment explaining how a
    long translation degrades, ended up as a KEY in all thirteen files."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c; out.append(c); i += 1
            while i < n:
                out.append(src[i])
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i + 1]); i += 2; continue
                if src[i] == q: i += 1; break
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n': i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                if src[i] == '\n': out.append('\n')
                i += 1
            i += 2
            continue
        out.append(c); i += 1
    return ''.join(out)


def literal_groups(arg):
    """Keys from one call argument.

    A C compiler concatenates ADJACENT string literals, so `"a" "b"` is one
    string. `cond ? "a" : "b"` is two, and joining them (which this used to do)
    invented keys that exist nowhere: "Paste (move)Paste (copy)",
    "Unblock  Block", "Unfav  Favorite". Those shipped to every translator in
    v18. Only whitespace and an LV_SYMBOL_* macro may sit between literals of
    one group; anything else starts a new one."""
    groups, cur, i, n = [], [], 0, len(arg)
    while i < n:
        c = arg[i]
        if c == '"':
            j, buf = i + 1, []
            while j < n:
                if arg[j] == '\\' and j + 1 < n: buf.append(arg[j:j + 2]); j += 2; continue
                if arg[j] == '"': break
                buf.append(arg[j]); j += 1
            cur.append(''.join(buf)); i = j + 1; continue
        if c.isspace(): i += 1; continue
        m = re.match(r'LV_SYMBOL_[A-Z0-9_]+', arg[i:])
        if m: i += m.end(); continue
        if cur: groups.append(cur); cur = []
        i += 1
    if cur: groups.append(cur)
    return [''.join(g) for g in groups]


def _sources():
    return sorted(glob.glob(os.path.join(ROOT, 'src/ui-touch/*.cpp')) +
                  glob.glob(os.path.join(ROOT, 'src/ui-touch/*.h')))


def _balanced(src, i):
    """Index just past the '}' closing the block whose '{' is at src[i]."""
    depth = 0
    while i < len(src):
        if src[i] == '{': depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0: return i + 1
        i += 1
    return len(src)


def _split_args(s):
    """Top-level comma split, ignoring commas inside (), [], {} or a string."""
    out, cur, depth, q, i = [], [], 0, None, 0
    while i < len(s):
        c = s[i]
        if q:
            cur.append(c)
            if c == '\\': 
                if i + 1 < len(s): cur.append(s[i+1]); i += 1
            elif c == q: q = None
        elif c in '"\'': q = c; cur.append(c)
        elif c in '([{': depth += 1; cur.append(c)
        elif c in ')]}': depth -= 1; cur.append(c)
        elif c == ',' and depth == 0: out.append(''.join(cur)); cur = []
        else: cur.append(c)
        i += 1
    out.append(''.join(cur))
    return out


# A function/lambda header: a name, a parenthesised parameter list, then '{'.
_HDR = re.compile(r'(?:auto\s+(?P<lam>[A-Za-z_]\w*)\s*=\s*\[[^\]]*\]\s*'
                  r'|\b(?P<fn>[A-Za-z_]\w*)\s*)\((?P<args>[^;{}]*)\)\s*'
                  r'(?:const\s*)?(?:->\s*[\w:*&<> ]+\s*)?\{')


def _tr_wrapping_helpers(src):
    """(helper name, argument index) for every helper that TR()s a parameter."""
    found = set()
    for m in _HDR.finditer(src):
        name = m.group('lam') or m.group('fn')
        if not name or name in ('if', 'for', 'while', 'switch', 'catch', 'TR'):
            continue
        params = []
        for a in _split_args(m.group('args')):
            names = re.findall(r'[A-Za-z_]\w*', a)
            params.append(names[-1] if names else '')
        body = src[m.end() - 1:_balanced(src, m.end() - 1)]
        for i, pn in enumerate(params):
            if pn and re.search(r'\bTR\(\s*' + re.escape(pn) + r'\s*\)', body):
                found.add((name, i))
    return found


def _call_args_at(src, name, idx):
    """Every argument at position idx across all calls to name(...)."""
    out = []
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', src):
        i = m.end()
        depth, q, start = 1, None, i
        while i < len(src) and depth:
            c = src[i]
            if q:
                if c == '\\': i += 1
                elif c == q: q = None
            elif c in '"\'': q = c
            elif c == '(': depth += 1
            elif c == ')': depth -= 1
            i += 1
        args = _split_args(src[start:i - 1])
        if idx < len(args):
            out.append(args[idx])
    return out

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

    # --obsolete: the REVERSE check. Rows in the .lang files that no TR() key matches
    # any more, i.e. translation work that is being carried for strings the firmware
    # no longer shows. Reported, never deleted automatically.
    #
    # Read the caveat before acting on it: this is static analysis. A string reached in
    # a way the extractor cannot see reads as unreferenced, and pruning it silently
    # deletes a working translation. That is not hypothetical -- before the indirect-
    # table support above, "About" and every other settings-category name appeared in
    # this list. Treat the output as candidates to CHECK, not a delete list.
    if "--obsolete" in sys.argv:
        for path in sorted(glob.glob(os.path.join(ROOT, 'deploy/apps/lang/*.lang'))):
            have = lang_keys(path)
            dead = sorted(k for k in have if k not in keys)
            tag = os.path.basename(path)
            print(f"{tag}: {len(have)} rows, {len(dead)} unreferenced")
            for k in dead:
                print(f"  {k!r}")
        print("\nCandidates only. Grep the source for one before removing it: a string "
              "reached indirectly can look unreferenced while being on screen.")
        return 0

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
