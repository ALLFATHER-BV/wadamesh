#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PlatformIO pre-build hook: keep i18n_builtin.h in step with deploy/apps/lang.

gen-lang-builtin.py's whole point is that the baked-in table and the .lang files
the store serves can never drift. That only holds if something runs it. Nothing
did: its docstring promised a pre-build step that was never wired up, so editing
a .lang and building produced an image with the OLD translations and no warning
(caught 2026-08-22 adding the Hungarian map credits, #257).

Regenerates only when a .lang is newer than the header, so a normal rebuild
pays one stat() per language.
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

langs = glob.glob(os.path.join(ROOT, "deploy", "apps", "lang", "*.lang"))
if langs:
    newest = max(os.path.getmtime(f) for f in langs)
    if not os.path.exists(HDR) or os.path.getmtime(HDR) < newest:
        print("i18n: a .lang changed, regenerating i18n_builtin.h")
        subprocess.run([sys.executable, GEN], cwd=ROOT, check=True)
