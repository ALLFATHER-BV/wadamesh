#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PlatformIO pre-build hook: keep the baked-in tables in step with deploy/apps/.

gen-lang-builtin.py's whole point is that the baked-in table and the .lang files
the store serves can never drift. That only holds if something runs it. Nothing
did: its docstring promised a pre-build step that was never wired up, so editing
a .lang and building produced an image with the OLD translations and no warning
(caught 2026-08-22 adding the Hungarian map credits, #257).

The same hazard applies to lua_builtin.h, which seeds the Lua apps into boards
that cannot reach the Store: it read a mirror directory nothing writes, so two
apps added in beta_68 were never baked in at all.

Regenerates only when a source file is newer than its generated output, so a
normal rebuild pays a handful of stat() calls.
"""
import glob, os, subprocess, sys

# PlatformIO runs pre: scripts through SConscript, where __file__ is undefined;
# there the project dir is the cwd. Standalone (the IDF build.sh) has __file__.
try:
    ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
except NameError:
    ROOT = os.getcwd()
HERE = os.path.join(ROOT, "scripts", "build")
HDR  = os.path.join(ROOT, "src", "ui-touch", "i18n_builtin.h")
GEN  = os.path.join(HERE, "gen-lang-builtin.py")
LUAH = os.path.join(ROOT, "src", "ui-touch", "lua_builtin.h")
LUAG = os.path.join(HERE, "gen-lua-builtin.py")

def _stale(out, sources):
    if not sources:
        return False
    return not os.path.exists(out) or os.path.getmtime(out) < max(os.path.getmtime(f) for f in sources)


if _stale(HDR, glob.glob(os.path.join(ROOT, "deploy", "apps", "lang", "*.lang"))):
    print("i18n: a .lang changed, regenerating i18n_builtin.h")
    subprocess.run([sys.executable, GEN], cwd=ROOT, check=True)

lua_src = glob.glob(os.path.join(ROOT, "deploy", "apps", "*", "*", "*.lua"))
lua_src += [os.path.join(ROOT, "deploy", "apps", "apps.json")]
if _stale(LUAH, [f for f in lua_src if os.path.exists(f)]):
    print("lua: an app changed, regenerating lua_builtin.h")
    subprocess.run([sys.executable, LUAG], cwd=ROOT, check=True)
