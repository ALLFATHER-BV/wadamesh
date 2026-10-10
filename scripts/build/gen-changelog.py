#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write deploy/site/changelog.html: every build's notes from release-notes/, newest first.

One page that both a browser and the firmware's text reader show well (the reader opens
it when there is no home page on the SD card, #622): plain HTML, no scripts, the notes as
lists. The notes files are the same ones release.sh turns into version.json, so the page
says what each build said. Lines starting with '#' are section comments there and are
left out here too. scripts/deploy-site.sh runs this before every upload.

Usage: python3 scripts/build/gen-changelog.py [out.html]
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NOTES = os.path.join(ROOT, "release-notes")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "deploy", "site", "changelog.html")


def builds():
    found = []
    for name in os.listdir(NOTES):
        m = re.fullmatch(r"beta_(\d+)\.txt", name)
        if m:
            found.append((int(m.group(1)), name))
    for num, name in sorted(found, reverse=True):
        with open(os.path.join(NOTES, name), encoding="utf-8") as f:
            lines = [l.strip() for l in f]
        title = ""
        if lines and lines[0].startswith("#"):
            # "# beta_90 - colour comes back: ... TEST BUILD." -> the part after the dash
            head = lines[0].lstrip("# ").strip()
            title = re.sub(r"^beta_\d+\s*[-:]\s*", "", head)
            title = re.sub(r"\s*(TEST BUILD|STABLE)\.?\s*$", "", title).strip()
            title = title.replace(" \u2014 ", ", ").replace("\u2014", ", ")
        # The early notes were written with em dashes; the site uses none, so the
        # page shows those clauses with a comma (the notes files keep their text).
        notes = [l.replace(" \u2014 ", ", ").replace("\u2014", ", ")
                 for l in lines if l and not l.startswith("#")]
        if notes:
            yield "beta_%d" % num, title, notes


def main():
    parts = []
    for tag, title, notes in builds():
        parts.append('<section id="%s">' % tag)
        parts.append("<h2>%s</h2>" % html.escape(tag))
        if title:
            parts.append('<p class="t">%s</p>' % html.escape(title))
        parts.append("<ul>")
        parts.extend("<li>%s</li>" % html.escape(n) for n in notes)
        parts.append("</ul></section>")
    page = """<!doctype html>
<html lang="en" data-sky="soft">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<script>try{if(localStorage.getItem('wm-theme')==='light')document.documentElement.setAttribute('data-theme','light')}catch(e){}</script>
<meta name="theme-color" content="#000400">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Montserrat:wght@500;600;700&family=JetBrains+Mono:wght@600;700&display=swap" rel="stylesheet">
<title>WADAMESH Changelog</title>
<meta name="description" content="What changed in every WADAMESH build, newest first.">
<link rel="canonical" href="https://wadamesh.com/changelog.html">
<meta property="og:type" content="website">
<meta property="og:site_name" content="WADAMESH">
<meta property="og:url" content="https://wadamesh.com/changelog.html">
<meta property="og:title" content="WADAMESH changelog">
<meta property="og:description" content="What changed in every WADAMESH build, newest first.">
<meta property="og:image" content="https://wadamesh.com/og-image.png?v=2">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta property="og:image:alt" content="The WADAMESH home screen on a LilyGo T-Deck">
<meta name="twitter:card" content="summary_large_image">
<link rel="icon" href="favicon.ico" sizes="32x32">
<link rel="icon" href="wadamesh-badge.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="apple-touch-icon.png">
<style>
  :root{--bg:#ffffff;--bg2:#f4f6f2;--ink:#15181e;--mut:#5d6770;--line:#e5e8e2;--teal:#15b6a6;--teal-ink:#0b7a6f;color-scheme:light}
  body{margin:0;font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,sans-serif;background:var(--bg);color:var(--ink)}
  main{max-width:46rem;margin:0 auto;padding:28px 18px 60px}
  a{color:var(--teal-ink)}
  h1{font-size:1.5rem;letter-spacing:.06em;margin:0 0 4px}
  h1 .t{color:var(--teal)}
  .lede{color:var(--mut);margin:0 0 26px}
  section{border-top:1px solid var(--line);padding:18px 0 6px}
  h2{font:700 1rem/1.3 "JetBrains Mono",ui-monospace,monospace;margin:0;color:var(--teal-ink)}
  p.t{margin:4px 0 0;font-weight:600}
  ul{margin:10px 0 0;padding-left:1.2rem}
  li{margin:0 0 8px}
  .wm-theme{position:fixed; top:14px; right:14px; z-index:5}
</style>
<link rel="stylesheet" href="theme.css?v=2">
<script src="theme.js?v=2" defer></script>
</head>
<body>
<main>
<button type="button" class="wm-theme" data-theme-toggle aria-label="Switch to light mode" title="Switch to light mode"><svg class="i-moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/></svg><svg class="i-sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41"/></svg></button>
<h1>WADA<span class="t">MESH</span> changelog</h1>
<p class="lede">What changed in every build, newest first. Install any of them at <a href="https://wadamesh.com/">wadamesh.com</a>.</p>
%s
</main>
</body>
</html>
""" % "\n".join(parts)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(page)
    print("changelog: %d builds -> %s" % (sum(1 for p in parts if p.startswith("<h2>")), OUT))


if __name__ == "__main__":
    main()
