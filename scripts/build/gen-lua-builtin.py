#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Bake the catalog's Lua apps into the firmware as seeded built-ins.

The Lua host already supports this: luaAppLaunchFile(id, title, embedded, len)
tries <data>/apps/<id>.lua first and falls back to an embedded source when the
file is not there. That file-first order is what makes a seeded app updatable —
download a newer version from the Store and the file simply wins.

This generates src/ui-touch/lua_builtin.h from deploy/apps/, i.e. exactly the
sources the Store serves, so a board that cannot reach the Store still ships
with the apps rather than an empty drawer.

It used to read out/firmware/apps/ instead. Nothing writes that directory --
deploy-apps.sh rsyncs deploy/apps/ straight to the VPS -- so it was a stale
mirror, and the "same sources the Store serves" guarantee was false: a bumped
app shipped to the Store while the baked-in copy stayed on whatever version was
mirrored last (#257).

Run from the repo root:  python3 scripts/build/gen-lua-builtin.py
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, 'deploy/apps')      # canonical; what deploy-apps.sh publishes
OUT = os.path.join(ROOT, 'src/ui-touch/lua_builtin.h')
DELIM = 'WADALUA'          # raw-string delimiter: keeps Lua source verbatim

_LONG_OPEN = re.compile(r'\[(=*)\[')


def strip_lua(code):
    """Drop comments and indentation from a Lua chunk before it is baked in.

    The baked copy is flash on the boards with the least of it, and the Store's
    copy keeps every comment, so only the image loses them. Every newline stays,
    so a line number in an app's error message still points at the Store source.
    Strings and long strings are copied as they are; a comment turns into the
    whitespace the Lua lexer would have seen. The result compiles to the same
    bytecode, line info included (checked when this was written).
    """
    out = []
    i, n = 0, len(code)
    line_start = True
    while i < n:
        c = code[i]
        if c == '\n':
            while out and out[-1] in (' ', '\t'):
                out.pop()                           # trailing blanks
            out.append('\n')
            i += 1
            line_start = True
            continue
        if line_start and c in ' \t':
            i += 1                                  # indentation
            continue
        line_start = False
        if code.startswith('--', i):
            m = _LONG_OPEN.match(code, i + 2)
            if m:                                   # --[[ long comment ]]
                close = ']' + m.group(1) + ']'
                j = code.find(close, m.end())
                if j < 0:
                    raise ValueError('unterminated long comment')
                nl = code.count('\n', i, j)
                if nl:
                    while out and out[-1] in (' ', '\t'):
                        out.pop()
                out.append('\n' * nl if nl else ' ')
                line_start = nl > 0
                i = j + len(close)
                continue
            j = code.find('\n', i)                  # -- line comment
            i = n if j < 0 else j
            continue
        if c in '"\'':                              # short string, escapes included
            j = i + 1
            while j < n and code[j] != c:
                j += 2 if code[j] == '\\' else 1
            out.append(code[i:j + 1])
            i = j + 1
            continue
        if c == '[':
            m = _LONG_OPEN.match(code, i)
            if m:                                   # [[ long string ]]
                close = ']' + m.group(1) + ']'
                j = code.find(close, m.end())
                if j < 0:
                    raise ValueError('unterminated long string')
                out.append(code[i:j + len(close)])
                i = j + len(close)
                continue
        out.append(c)
        i += 1
    return ''.join(out)


def main():
    cat_path = os.path.join(APPS, 'apps.json')
    if not os.path.exists(cat_path):
        sys.exit('no catalog at ' + cat_path)
    cat = json.load(open(cat_path, encoding='utf-8'))['apps']

    entries = []
    for app in cat:
        aid, ver, name = app['id'], app['ver'], app['name']
        # "seed": false keeps an app out of the image while still publishing it to
        # the Store. Seeding is not free: it is flash on the boards that have the
        # least of it, which are exactly the CAP_BUILTIN_LUA_APPS boards. An app
        # that is large, or that needs hardware those boards do not have, earns its
        # place rather than getting it by being in the catalog.
        if app.get('seed') is False:
            print('  skip %s: seed=false in apps.json' % aid)
            continue
        src = os.path.join(APPS, aid, ver, aid + '.lua')
        if not os.path.exists(src):
            print('  skip %s: no %s' % (aid, os.path.relpath(src, ROOT)))
            continue
        code = strip_lua(open(src, encoding='utf-8').read())
        if (')' + DELIM + '"') in code:
            sys.exit('%s contains the raw-string delimiter' % aid)
        # An app that needs the extended SDK cannot run on a board without it, so it
        # is baked in only where it can (CAP_LUA_SDK_EXT, every board but the 2 MB
        # Heltec V4, the one with the least flash to give).
        entries.append((aid, name, ver, code, app.get('requires') == 'sdk_ext'))

    if not entries:
        sys.exit('no app sources found under ' + APPS)

    w = ['// Generated by scripts/build/gen-lua-builtin.py — DO NOT EDIT.',
         '// Source: deploy/apps/ (the same .lua the Lua Store serves).',
         '// Seeded built-ins: a downloaded <data>/apps/<id>.lua always wins.',
         '#pragma once', '',
         'struct LuaBuiltinApp { const char* id; const char* name; const char* ver; const char* src; };', '']
    for aid, name, ver, code, ext in entries:
        if ext: w.append('#if CAP_LUA_SDK_EXT')
        w.append('static const char kLuaSrc_%s[] = R"%s(%s)%s";' % (aid, DELIM, code, DELIM))
        if ext: w.append('#endif')
    w.append('')
    w.append('static const LuaBuiltinApp kLuaBuiltin[] = {')
    for aid, name, ver, _, ext in entries:
        if ext: w.append('#if CAP_LUA_SDK_EXT')
        w.append('  { "%s", "%s", "%s", kLuaSrc_%s },' % (aid, name, ver, aid))
        if ext: w.append('#endif')
    w.append('};')
    w.append('static const int kLuaBuiltinCount = (int)(sizeof(kLuaBuiltin)/sizeof(kLuaBuiltin[0]));')
    w.append('')

    open(OUT, 'w', encoding='utf-8').write('\n'.join(w))
    total = sum(len(e[3]) for e in entries)
    print('%s: %d apps, %d bytes of Lua' %
          (os.path.relpath(OUT, ROOT), len(entries), total))
    for aid, _, ver, code, ext in entries:
        print('   %-10s v%-4s %6d B%s' % (aid, ver, len(code), '  (extended SDK only)' if ext else ''))


if __name__ == '__main__':
    main()
