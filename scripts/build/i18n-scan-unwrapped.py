#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Find EVERY user-visible string literal in the touch UI that is not wrapped in TR().

The coverage audit (i18n-audit.py) only checks strings that already go through
TR(). This one finds the ones that never do — a bare literal handed straight to
an LVGL text setter or an alert/confirm helper renders in English forever, and
no amount of translating can reach it.

It parses each known text-taking call, splits its argument list at top level and
inspects the argument positions that carry human text. An argument counts as
covered when it starts with TR(; a bare "..." literal (optionally preceded by an
LV_SYMBOL_* macro) is reported.

Run from the repo root:  python3 scripts/build/i18n-scan-unwrapped.py
Exit code 1 if anything is unwrapped.
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# call name -> indices (0-based) of arguments that hold user-visible text
TEXT_ARGS = {
    'lv_label_set_text':              [1],
    'lv_label_set_text_static':       [1],
    'lv_label_set_text_fmt':          [1],
    'lv_checkbox_set_text':           [1],
    'lv_dropdown_set_options':        [1],
    'lv_dropdown_set_options_static': [1],
    'lv_roller_set_options':          [1],
    'lv_textarea_set_placeholder_text': [1],
    'lv_tabview_add_tab':             [1],
    'lv_table_set_cell_value':        [3],
    'lv_msgbox_create':               [1, 2],
    'showAlert':                      [0],
    'showConfirm':                    [0, 1],
    'appPageBegin':                   [0],
    'appPageBeginSlim':               [0],
    'setPageTitle':                   [0],
    'createSettingsModal':            [0],
    'sectionHeader':                  [1],
    'addSettingRow':                  [1],
}

def split_args(s):
    """Split an argument list at top-level commas (parens/brackets/strings aware)."""
    out, depth, cur, i, in_str = [], 0, '', 0, False
    while i < len(s):
        c = s[i]
        if in_str:
            cur += c
            if c == '\\' and i + 1 < len(s):
                cur += s[i + 1]; i += 2; continue
            if c == '"':
                in_str = False
            i += 1; continue
        if c == '"':
            in_str = True; cur += c; i += 1; continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur.strip()); cur = ''; i += 1; continue
        cur += c; i += 1
    if cur.strip():
        out.append(cur.strip())
    return out

def call_args(src, start):
    """Given index of the '(' after a call name, return (args, end_index)."""
    depth, i, in_str = 0, start, False
    while i < len(src):
        c = src[i]
        if in_str:
            if c == '\\': i += 2; continue
            if c == '"': in_str = False
            i += 1; continue
        if c == '"': in_str = True; i += 1; continue
        if c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return split_args(src[start + 1:i]), i
        i += 1
    return None, start

# a bare literal, optionally LV_SYMBOL_X-prefixed, possibly concatenated
LITERAL = re.compile(r'^(?:LV_SYMBOL_[A-Z0-9_]+\s*)?(?:"(?:[^"\\]|\\.)*"\s*)+$')
HAS_LETTERS = re.compile(r'[A-Za-z]{2}')

def meaningful(arg):
    """Is this literal actual UI prose (not a format spec / symbol / path)?"""
    if not LITERAL.match(arg):
        return False
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', arg)
    text = ''.join(parts)
    stripped = re.sub(r'%[-+ #0-9.]*[a-zA-Z]', '', text)      # drop format specs
    stripped = re.sub(r'\\x[0-9A-Fa-f]{2}|\\.', '', stripped)  # drop escapes
    if not HAS_LETTERS.search(stripped):
        return False
    if stripped.strip().startswith('/'):     # filesystem paths
        return False
    if re.match(r'^[a-z0-9_.:\-/]+$', stripped.strip()):  # ids, filenames, keys ("ui:hist")
        return False
    return True

# Local row/button builders. Their signatures differ, so --fix wraps EVERY
# argument that reads as prose rather than a fixed index — a helper nobody
# remembered to list is exactly how "Remove channel" stayed English.
HELPER_CALLS = {
    'mk', 'mk_full', 'mk_btn', 'mk_label', 'mk_ta', 'mk_switch', 'mk_row',
    'item', 'setupHeader', 'setupBtn', 'fmActionBtn', 'ccToggle',
    'make_launcher', 'setAddChannelError', 'addAppTile', 'navBtn',
}

# Brand and pure-technical tokens that are identical in every language.
SKIP_TEXT = {'WADAMESH', 'CPU', 'PSK', 'TX 0  /  RX 0', 'Sig --', 'OK'}

# ---- wide mode -------------------------------------------------------------
# Naming the functions that take UI text has the same blind spot as grepping
# for the strings: a local row helper nobody listed (mk_full) hides its
# labels forever. Wide mode inverts it — EVERY prose literal is suspect unless
# it is consumed by a known non-UI sink.
NON_UI_CALLS = {
    'snprintf_path', 'strcmp', 'strncmp', 'strcasecmp', 'strncasecmp', 'strstr',
    'strchr', 'strrchr', 'strcpy', 'strncpy', 'strcat', 'strlen', 'atoi', 'atof',
    'printf', 'sprintf', 'fprintf', 'sscanf', 'log_e', 'log_w', 'log_i', 'log_d',
    'ESP_LOGE', 'ESP_LOGW', 'ESP_LOGI', 'ESP_LOGD', 'begin', 'open', 'mkdir',
    'rmdir', 'remove', 'rename', 'exists', 'setUserAgent', 'addHeader',
    'luaHostAppPath', 'prefsGetStr', 'putString', 'getString', 'isKey',
    'lv_obj_set_style_text_font', 'captureScreenToSerial', 'docSettle',
    'lua_setfield', 'lua_getfield', 'luaL_error', 'luaL_newmetatable',
    'luaL_checkudata', 'lua_pushstring', 'lua_setglobal', 'luaL_requiref',
    'MDNS', 'WiFi', 'client', 'http', 'Serial',
}
# Calls whose FORMAT argument is UI text (index 2 for snprintf-style).
FMT_CALLS = {'snprintf': 2}

def enclosing_call(src, pos):
    """Name of the function whose argument list encloses `pos` (or '')."""
    depth, i = 0, pos
    while i > 0:
        c = src[i]
        if c == ')':
            depth += 1
        elif c == '(':
            if depth == 0:
                j = i - 1
                while j >= 0 and src[j] in ' \t\n':
                    j -= 1
                k = j
                while k >= 0 and (src[k].isalnum() or src[k] in '_:.>-'):
                    k -= 1
                name = src[k + 1:j + 1]
                return name.split('>')[-1].split('.')[-1].split(':')[-1]
            depth -= 1
        i -= 1
    return ''

def wide_scan():
    """Report every prose literal not inside TR() and not bound for a non-UI sink."""
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'src/ui-touch/*.cpp'))):
        src = open(path, encoding='utf-8').read()
        rel = os.path.relpath(path, ROOT)
        # index every TR( ... ) span so we can tell what is already covered
        tr_spans = []
        for m in re.finditer(r'\bTR\s*\(', src):
            _, close = call_args(src, m.end() - 1)
            tr_spans.append((m.start(), close))
        i = 0
        while i < len(src):
            c = src[i]
            if c == '/' and i + 1 < len(src) and src[i + 1] == '/':      # line comment
                i = src.find('\n', i)
                if i < 0: break
                continue
            if c == '/' and i + 1 < len(src) and src[i + 1] == '*':      # block comment
                i = src.find('*/', i)
                if i < 0: break
                i += 2
                continue
            if c != '"':
                i += 1
                continue
            start = i
            i += 1
            while i < len(src):
                if src[i] == '\\': i += 2; continue
                if src[i] == '"': break
                i += 1
            end = i
            i += 1
            lit = src[start:end + 1]
            if not meaningful(lit):
                continue
            if any(a < start < b for a, b in tr_spans):
                continue
            name = enclosing_call(src, start)
            if name in NON_UI_CALLS:
                continue
            if name in FMT_CALLS:
                args, _ = call_args(src, src.rfind('(', 0, start))
                # only the format argument of an snprintf carries UI text
                if args is None or len(args) <= FMT_CALLS[name] or \
                   not args[FMT_CALLS[name]].lstrip().startswith('"'):
                    continue
            txt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', lit))
            if txt.strip() in SKIP_TEXT:
                continue
            line = src.count('\n', 0, start) + 1
            out.append((rel, line, name or '?', txt))
    return out

def main():
    fix = '--fix' in sys.argv
    if '--wide' in sys.argv:
        found = wide_scan()
        for rel, line, name, txt in found:
            print(f"{rel}:{line}  {name}()  {txt!r}")
        print(f"\n{len(found)} prose literal(s) outside TR() [wide scan]")
        sys.exit(1 if found else 0)
    findings = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'src/ui-touch/*.cpp'))):
        src = open(path, encoding='utf-8').read()
        rel = os.path.relpath(path, ROOT)
        edits = []          # (start, end, replacement) for --fix, applied back-to-front
        for hname in sorted(HELPER_CALLS):
            for m in re.finditer(r'(?<![A-Za-z0-9_])' + hname + r'\s*\(', src):
                open_paren = m.end() - 1
                args, close = call_args(src, open_paren)
                if not args:
                    continue
                new_args, touched = list(args), False
                for i, a in enumerate(args):
                    if a.startswith('TR(') or a.startswith('TR ('):
                        continue
                    if not meaningful(a):
                        continue
                    txt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', a))
                    if txt.strip() in SKIP_TEXT:
                        continue
                    findings.append((rel, src.count('\n', 0, m.start()) + 1, hname, txt))
                    new_args[i] = 'TR(' + a + ')'
                    touched = True
                if touched and fix:
                    edits.append((open_paren + 1, close, ', '.join(new_args)))
        for name, idxs in TEXT_ARGS.items():
            for m in re.finditer(r'\b' + name + r'\s*\(', src):
                open_paren = m.end() - 1
                args, close = call_args(src, open_paren)
                if not args:
                    continue
                for idx in idxs:
                    if idx >= len(args):
                        continue
                    a = args[idx]
                    if a.startswith('TR(') or a.startswith('TR ('):
                        continue
                    if not meaningful(a):
                        continue
                    txt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', a))
                    if txt.strip() in SKIP_TEXT:
                        continue
                    line = src.count('\n', 0, m.start()) + 1
                    findings.append((rel, line, name, txt))
                    if fix:
                        new_args = list(args)
                        new_args[idx] = 'TR(' + a + ')'
                        edits.append((open_paren + 1, close, ', '.join(new_args)))
        if fix and edits:
            for start, end, rep in sorted(edits, key=lambda e: -e[0]):
                src = src[:start] + rep + src[end:]
            open(path, 'w', encoding='utf-8').write(src)
            print(f"[fix] {rel}: {len(edits)} call(s) wrapped")
    if not fix:
        for rel, line, name, txt in findings:
            print(f"{rel}:{line}  {name}  {txt!r}")
    print(f"\n{len(findings)} unwrapped user-visible literal(s)")
    sys.exit(1 if (findings and not fix) else 0)

if __name__ == '__main__':
    main()
