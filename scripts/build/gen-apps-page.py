#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write deploy/site/apps.html: the Lua app store as a web page.

Names, versions and what each app needs come from deploy/apps/apps.json, the same
catalog the devices download, so the page can never list a version the Store does not
serve. The longer write-ups, categories, credits and screenshots live in
deploy/site/apps-info.json (site-only: the device catalog stays small). An app in the
catalog without an entry there still gets a card, from its catalog description.
scripts/deploy-site.sh runs this before every upload.

Usage: python3 scripts/build/gen-apps-page.py [out.html]
"""
import html
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOG = os.path.join(ROOT, "deploy", "apps", "apps.json")
INFO = os.path.join(ROOT, "deploy", "site", "apps-info.json")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "deploy", "site", "apps.html")
GH = "https://github.com/ALLFATHER-BV/wadamesh"

# What a catalog "requires" means for the person reading the page.
RUNS_ON = {
    "": ("Every board", "every"),
    "sdk_ext": ("Every board but the 2 MB Heltec V4", "most"),
    "sd": ("Boards with an SD card", "most"),
    "gps": ("Needs a GPS", "most"),
    "audio": ("Boards with a speaker", "some"),
}

# Outline glyphs (24x24, stroke = currentColor), one per manifest icon name.
GLYPH = {
    "signal": '<path d="M2 20h.01M7 20v-4M12 20v-8M17 20V8M22 4v16"/>',
    "chart": '<path d="M3 3v18h18"/><path d="m7 15 4-4 3 3 6-7"/>',
    "map": '<path d="M9 4 3 6v14l6-2 6 2 6-2V4l-6 2-6-2Z"/><path d="M9 4v14M15 6v14"/>',
    "message": '<path d="M21 15a2 2 0 0 1-2 2H8l-5 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2Z"/>',
    "bell": '<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/><path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"/>',
    "person": '<circle cx="12" cy="8" r="4"/><path d="M4 21v-1a6 6 0 0 1 6-6h4a6 6 0 0 1 6 6v1"/>',
    "compass": '<circle cx="12" cy="12" r="9"/><path d="m15.5 8.5-2 5-5 2 2-5Z"/>',
    "gps": '<path d="M12 21s-7-6.3-7-11a7 7 0 0 1 14 0c0 4.7-7 11-7 11Z"/><circle cx="12" cy="10" r="2.5"/>',
    "star": '<path d="m12 3 2.7 5.6 6.1.9-4.4 4.3 1 6.1L12 17l-5.4 2.9 1-6.1-4.4-4.3 6.1-.9Z"/>',
    "music": '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>',
    "game": '<rect x="2" y="7" width="20" height="11" rx="4"/><path d="M7 11v3M5.5 12.5h3"/><circle cx="16" cy="11.5" r="1"/><circle cx="18" cy="14" r="1"/>',
    "snake": '<path d="M4 18h6a3 3 0 0 0 0-6H8a3 3 0 0 1 0-6h10"/><circle cx="19.5" cy="6" r="1.5"/>',
    "blocks": '<path d="M4 14h5v5H4zM9 14h5v5H9zM9 9h5v5H9zM15 4h5v5h-5z"/>',
    "bricks": '<path d="M3 4h8v4H3zM13 4h8v4h-8zM7 10h10v4H7z"/><path d="M8 20h8"/><circle cx="12" cy="17" r="1.2"/>',
    "cards": '<rect x="3" y="6" width="11" height="15" rx="2"/><path d="m10 6 .6-2.1a2 2 0 0 1 2.4-1.4l6.6 1.8a2 2 0 0 1 1.4 2.4l-3 11.3"/><path d="M8.5 11v5M6 13.5h5"/>',
    "grid": '<rect x="3" y="3" width="18" height="18" rx="3"/><path d="M9 3v18M15 3v18M3 9h18M3 15h18"/>',
    "search": '<circle cx="11" cy="11" r="7"/><path d="m21 21-4.3-4.3"/>',
    "settings": '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.6 1.6 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-1.8-.3 1.6 1.6 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.6 1.6 0 0 0-1-1.5 1.6 1.6 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0 .3-1.8 1.6 1.6 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.6 1.6 0 0 0 1.5-1 1.6 1.6 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.6 1.6 0 0 0 1.8.3H9a1.6 1.6 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 1 1.5 1.6 1.6 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.8V9a1.6 1.6 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1Z"/>',
}


def esc(s):
    return html.escape(s or "", quote=True)


def svg(name, cls="g"):
    return '<svg class="%s" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">%s</svg>' % (
        cls, GLYPH.get(name, GLYPH["game"]))


def credit_html(info):
    parts = []
    if info.get("credit"):
        u = info["credit"]
        parts.append('By <a href="https://github.com/%s" target="_blank" rel="noopener">%s</a>' % (esc(u), esc(u)))
    if info.get("credit_note"):
        note = esc(info["credit_note"])
        u = info.get("credit_user")
        if u:
            note = note.replace(esc(u), '<a href="https://github.com/%s" target="_blank" rel="noopener">%s</a>' % (esc(u), esc(u)))
        parts.append(note)
    return '<p class="by">%s</p>' % " &middot; ".join(parts) if parts else ""


def card(app, info, hue):
    req = app.get("requires", "") or ""
    runs, level = RUNS_ON.get(req, ("Check the SDK guide", "some"))
    glyph = info.get("glyph") or app.get("icon") or "game"
    tagline = info.get("tagline") or app.get("desc", "")
    about = info.get("about") or app.get("desc", "")
    out = ['<article class="app" id="%s" data-cat="%s" style="--hue:var(%s)">' % (esc(app["id"]), esc(info.get("category", "tools")), hue)]
    if not info.get("shot"):
        # no screenshot yet: a poster in the category colour keeps the row even
        out.append('<div class="shot poster">%s</div>' % svg(info.get("glyph") or app.get("icon") or "game", "pg"))
    else:
        out.append('<div class="shot%s"><img src="%s" alt="%s on a device" loading="lazy" width="%s" height="%s"></div>' % (
            " tall" if info.get("shot_tall") else "", esc(info["shot"]), esc(app["name"]),
            "284" if info.get("shot_tall") else "320", "616" if info.get("shot_tall") else "240"))
    out.append('<div class="head"><span class="ico">%s</span><div><h3>%s <span class="ver">v%s</span></h3><p class="tl">%s</p></div></div>' % (
        svg(glyph), esc(app["name"]), esc(app["ver"]), esc(tagline)))
    out.append('<p class="runs %s">%s</p>' % (level, esc(runs)))
    out.append(credit_html(info))
    out.append('<details><summary>More about %s</summary><p>%s</p>' % (esc(app["name"]), esc(about)))
    if info.get("highlights"):
        out.append("<ul>%s</ul>" % "".join("<li>%s</li>" % esc(h) for h in info["highlights"]))
    out.append("</details></article>")
    return "\n".join(o for o in out if o)


def main():
    cat = json.load(open(CATALOG, encoding="utf-8"))["apps"]
    info_doc = json.load(open(INFO, encoding="utf-8"))
    infos = info_doc["apps"]
    cats = info_doc["categories"]
    hue = {c["id"]: c["hue"] for c in cats}
    sections, chips = [], ['<button type="button" class="chip on" data-f="all">All <span>%d</span></button>' % len(cat)]
    for c in cats:
        apps = [a for a in cat if infos.get(a["id"], {}).get("category", "tools") == c["id"]]
        if not apps:
            continue
        chips.append('<button type="button" class="chip" data-f="%s">%s <span>%d</span></button>' % (esc(c["id"]), esc(c["name"]), len(apps)))
        cards = "\n".join(card(a, infos.get(a["id"], {}), hue[c["id"]]) for a in apps)
        sections.append('<section class="cat" data-cat="%s"><h2>%s</h2><div class="grid">\n%s\n</div></section>' % (esc(c["id"]), esc(c["name"]), cards))
    page = PAGE.replace("@@COUNT@@", str(len(cat))).replace("@@CHIPS@@", "\n".join(chips)) \
               .replace("@@SECTIONS@@", "\n".join(sections)).replace("@@GH@@", GH)
    open(OUT, "w", encoding="utf-8").write(page)
    print("%s: %d apps in %d categories" % (os.path.relpath(OUT, ROOT), len(cat), len(sections)))


PAGE = r"""<!doctype html>
<html lang="en" data-sky="soft">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<script>try{if(localStorage.getItem('wm-theme')==='light')document.documentElement.setAttribute('data-theme','light')}catch(e){}</script>
<meta name="theme-color" content="#000400">
<title>WADAMESH · App Store</title>
<meta name="description" content="The Lua apps you can install on a WADAMESH device from its built-in Store: radio tools, navigation, emergency messaging, music and games. Free, sandboxed and reviewed. Write your own with the SDK.">
<link rel="canonical" href="https://wadamesh.com/apps.html">
<meta property="og:type" content="website">
<meta property="og:site_name" content="WADAMESH">
<meta property="og:url" content="https://wadamesh.com/apps.html">
<meta property="og:title" content="WADAMESH · App Store">
<meta property="og:description" content="Radio tools, navigation, emergency messaging, music and games for your mesh radio. Free, sandboxed and reviewed.">
<meta property="og:image" content="https://wadamesh.com/og-image.png?v=2">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:card" content="summary_large_image">
<link rel="icon" href="favicon.ico" sizes="32x32">
<link rel="icon" href="wadamesh-badge.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="apple-touch-icon.png">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Montserrat:wght@500;600;700&family=JetBrains+Mono:wght@600;700&display=swap" rel="stylesheet">
<style>
  :root{--bg:#000400; --ink:#E6EBED; --mut:#B0BAC0; --line:#1C2428; --teal:#15B6A6; --teal-ink:#19D6C2; color-scheme:dark}
  *{box-sizing:border-box;margin:0;padding:0}
  body{font:16px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; background:var(--bg); color:var(--ink); -webkit-font-smoothing:antialiased}
  a{color:var(--teal-ink); text-decoration:none}
  a:hover{text-decoration:underline}

  .top{position:sticky; top:0; z-index:30; display:flex; align-items:center; gap:14px; padding:12px 22px;
    background:var(--bar); -webkit-backdrop-filter:saturate(1.2) blur(10px); backdrop-filter:saturate(1.2) blur(10px); border-bottom:1px solid var(--line)}
  .top .word{font-weight:700; letter-spacing:2px; font-size:1.05rem; color:var(--ink)}
  .top .word .t{color:var(--teal)}
  .top .tag{font-family:'JetBrains Mono',monospace; font-size:.66rem; letter-spacing:.18em; text-transform:uppercase; color:var(--mut); border:1px solid var(--line); border-radius:100px; padding:4px 9px}
  .top .sp{flex:1}
  .top .home{display:inline-flex; align-items:center; gap:7px; color:var(--mut); font-size:.86rem; border:1px solid var(--line); border-radius:100px; padding:6px 13px; background:var(--control)}
  .top .home:hover{color:var(--teal-ink); border-color:var(--teal); text-decoration:none}
  .top .home svg{width:16px;height:16px}

  .wrap{max-width:1080px; margin:0 auto; padding:0 22px 80px}
  h1{font-size:2.1rem; line-height:1.15; margin:38px 0 10px; letter-spacing:-.01em; text-wrap:balance; text-shadow:var(--glow-text)}
  .lede{color:var(--sky-sub); max-width:66ch; font-size:1.03rem; text-shadow:var(--glow-text)}
  .cta{display:flex; flex-wrap:wrap; gap:10px; margin-top:20px}
  .btn{display:inline-flex; align-items:center; gap:8px; padding:10px 18px; border-radius:100px; font-size:.92rem; font-weight:600;
    border:1px solid var(--line); background:var(--control); color:var(--ink)}
  .btn:hover{text-decoration:none; border-color:var(--teal)}
  .btn.pri{background:var(--accent-fill); border-color:var(--accent-fill); color:var(--on-accent)}
  .btn svg{width:17px; height:17px}

  .steps{display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:12px; margin:30px 0 8px}
  .step{padding:14px 16px; border-radius:14px; background:var(--surface); border:1px solid var(--line); font-size:.9rem; color:var(--mut)}
  .step b{display:block; color:var(--ink); font-size:.95rem; margin-bottom:2px}
  .step .n{font-family:'JetBrains Mono',monospace; color:var(--teal-ink); font-size:.75rem; letter-spacing:.12em}

  .chips{display:flex; flex-wrap:wrap; gap:8px; margin:34px -10px 4px; position:sticky; top:66px; z-index:20; padding:8px 10px; border-radius:18px;
    background:var(--bar); -webkit-backdrop-filter:saturate(1.2) blur(10px); backdrop-filter:saturate(1.2) blur(10px)}
  .chip{display:inline-flex; align-items:center; gap:7px; padding:7px 13px; border-radius:100px; cursor:pointer; font:inherit; font-size:.85rem;
    border:1px solid var(--line); background:var(--control); color:var(--ink2)}
  .chip span{color:var(--dim); font-size:.78rem; font-variant-numeric:tabular-nums}
  .chip:hover{border-color:var(--teal)}
  .chip.on{border-color:var(--teal); background:var(--accent-soft); color:var(--teal-ink); font-weight:600}

  .cat h2{font-size:1.15rem; margin:30px 0 12px; text-shadow:var(--glow-text)}
  .grid{display:grid; grid-template-columns:repeat(auto-fill,minmax(300px,1fr)); gap:16px; align-items:start}
  .app{border-radius:16px; background:var(--surface); border:1px solid var(--line); padding:16px; box-shadow:var(--shadow); scroll-margin-top:150px}
  .app .shot{margin:-4px -4px 14px; border-radius:11px; background:#05080A; border:1px solid var(--frame-edge); display:grid; place-items:center; padding:10px; height:200px}
  .app .shot.poster{color:var(--hue); border-color:color-mix(in srgb, var(--hue) 22%, transparent);
    background:radial-gradient(120% 90% at 50% 110%, color-mix(in srgb, var(--hue) 26%, transparent), transparent 70%), var(--bg2)}
  .app .shot.poster .pg{width:72px; height:72px; stroke-width:1.3; opacity:.9; filter:drop-shadow(0 0 18px color-mix(in srgb, var(--hue) 55%, transparent))}
  .app .shot img{max-width:100%; max-height:180px; width:auto; height:auto; border-radius:6px; display:block}
  .app .shot.tall img{width:170px; height:180px; max-height:none; object-fit:cover; object-position:top; border-radius:8px}
  .app .head{display:flex; gap:12px; align-items:flex-start}
  .app .ico{flex:none; width:46px; height:46px; border-radius:13px; display:grid; place-items:center;
    color:var(--hue); background:color-mix(in srgb, var(--hue) 15%, transparent); box-shadow:inset 0 0 0 1px color-mix(in srgb, var(--hue) 30%, transparent)}
  .app .ico .g{width:24px; height:24px}
  .app h3{font-size:1.05rem; line-height:1.25}
  .app .ver{font-family:'JetBrains Mono',monospace; font-size:.7rem; color:var(--dim); font-weight:600; margin-left:4px}
  .app .tl{color:var(--mut); font-size:.9rem; margin-top:2px}
  .app .runs{display:inline-block; margin-top:12px; padding:3px 10px; border-radius:100px; font-size:.76rem; font-weight:600}
  .app .runs.every{color:var(--ok); background:var(--ok-bg)}
  .app .runs.most{color:var(--info); background:color-mix(in srgb, var(--info) 14%, transparent)}
  .app .runs.some{color:var(--warn-ink); background:var(--warn-bg)}
  .app .by{font-size:.8rem; color:var(--dim); margin-top:8px}
  .app details{margin-top:10px; border-top:1px solid var(--line); padding-top:8px}
  .app summary{cursor:pointer; font-size:.85rem; color:var(--teal-ink); list-style:none}
  .app summary::-webkit-details-marker{display:none}
  .app summary::before{content:"+ "; font-family:'JetBrains Mono',monospace}
  .app details[open] summary::before{content:"\2212  "}
  .app details p{font-size:.9rem; color:var(--ink2); margin-top:8px}
  .app details ul{margin:8px 0 2px 18px; font-size:.86rem; color:var(--mut)}

  .make{scroll-margin-top:140px; margin-top:56px; padding:26px; border-radius:18px; background:var(--surface); border:1px solid var(--line)}
  .make h2{font-size:1.4rem; margin-bottom:6px}
  .make > p{color:var(--mut); max-width:70ch}
  .mk{display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:14px; margin-top:20px}
  .mk div{padding:16px 18px; border-radius:14px; background:var(--bg2); border:1px solid var(--line)}
  .mk h3{font-size:.98rem; margin-bottom:6px}
  .mk p, .mk li{font-size:.9rem; color:var(--mut)}
  .mk ul{margin:6px 0 0 18px}
  code{font-family:'JetBrains Mono',monospace; font-size:.82em; background:var(--bg2); border:1px solid var(--line); padding:1px 6px; border-radius:6px; color:var(--ink2); overflow-wrap:anywhere}
  .foot{margin-top:40px; padding-top:18px; border-top:1px solid var(--line); color:var(--mut); font-size:.85rem}
  [hidden]{display:none!important}

  @media (max-width:760px){
    .steps,.mk{grid-template-columns:1fr}
    h1{font-size:1.65rem}
    .wrap{padding:0 16px 60px}
    .top{padding:10px 16px}
    .top .tag{display:none}
    .chips{top:60px; margin-inline:-6px; padding-inline:6px; flex-wrap:nowrap; overflow-x:auto; scrollbar-width:none}
    .chips::-webkit-scrollbar{display:none}
    .chip{flex:none}
    .app .shot{height:160px}
    .app .shot img,.app .shot.tall img{max-height:140px}
    .app .shot.tall img{height:140px; width:140px}
    .grid{grid-template-columns:1fr}
    .make{padding:18px}
  }
</style>
<link rel="stylesheet" href="theme.css?v=2">
<script src="theme.js?v=2" defer></script>
<script src="menu.js?v=3" defer></script>
</head>
<body>

<header class="top">
  <span class="word">WADA<span class="t">MESH</span></span>
  <span class="tag">app store</span>
  <span class="sp"></span>
  <button type="button" class="wm-theme" data-theme-toggle aria-label="Switch to light mode" title="Switch to light mode"><svg class="i-moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/></svg><svg class="i-sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41"/></svg></button>
  <a class="home" href="/">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 11l9-8 9 8M5 10v10h14V10"/></svg>
    Home
  </a>
</header>

<main class="wrap">
  <h1>Apps for your mesh radio</h1>
  <p class="lede">@@COUNT@@ small apps you install from the Store on the device itself, over Wi-Fi. Radio tools, navigation, emergency messages, music and games. They are free, run in a sandbox that keeps them away from your keys, and every one is read before it is listed.</p>
  <div class="cta">
    <a class="btn pri" href="#make"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>Make your own app</a>
    <a class="btn" href="sdk.html"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/></svg>Lua SDK guide</a>
  </div>

  <div class="steps">
    <div class="step"><span class="n">01</span><b>Open the Store</b>In the app drawer, tap Store. It needs Wi-Fi.</div>
    <div class="step"><span class="n">02</span><b>Tap Get</b>The app installs in a few seconds and shows up in the drawer.</div>
    <div class="step"><span class="n">03</span><b>Updates find you</b>The Store tile shows a number when an installed app has a newer version.</div>
  </div>

  <nav class="chips" aria-label="Filter by category">
@@CHIPS@@
  </nav>

@@SECTIONS@@

  <section class="make" id="make">
    <h2>Make your own app</h2>
    <p>Apps are one Lua file plus a one-line manifest. They talk to the device through the <code>wada.*</code> API: screens and input, timers, storage, the mesh, GPS, the map and audio. Read a reference app, write yours on the device, and send it in for everyone.</p>
    <div class="mk">
      <div><h3>1. Read the SDK guide</h3><p>The <a href="sdk.html">SDK guide</a> starts with <a href="sdk.html#hello">your first app</a> in ten lines and documents every call. Snake, RF Monitor and Nearby are small enough to read in one sitting.</p></div>
      <div><h3>2. Try it on your own device</h3><p>Copy your <code>.lua</code> file to <code>/meshcomod/apps/</code> on the SD card (or <code>/apps/</code> on boards that store internally) and it appears in the app drawer, no manifest needed. <a href="sdk.html#install">How side-loading works</a>.</p></div>
      <div><h3>3. Send it in</h3><p>Fork <a href="@@GH@@" target="_blank" rel="noopener">ALLFATHER-BV/wadamesh</a>, add <code>deploy/apps/&lt;id&gt;/&lt;ver&gt;/</code> with your <code>.lua</code> and manifest, add your app to <code>deploy/apps/apps.json</code>, and open a pull request saying what it does and which boards you tried. A published version is never edited: a change ships as the next version. No git? <a href="@@GH@@/issues/new" target="_blank" rel="noopener">Open an issue</a> with the file attached. <a href="@@GH@@/blob/main/deploy/apps/README.md" target="_blank" rel="noopener">The full guide</a>.</p></div>
      <div><h3>4. What review looks for</h3><ul>
        <li>Never reads, sends or stores keys, identities or channel secrets</li>
        <li>No flash writes in a loop or on every packet</li>
        <li>Nothing that needs more than the Heltec V4's 2 MB of PSRAM</li>
        <li>Transmits or changes radio settings only when the user asks</li>
        <li>Data leaves the device only when the user asked for it</li>
      </ul><p style="margin-top:6px">If something needs changing we say what and why on the PR, and help you land it. Apps are published under GPL-3.0 like the rest of the firmware.</p></div>
    </div>
  </section>

  <p class="foot">This page is generated from the same catalog the devices read, so the versions here are the versions you get. Questions? Ask on <a href="https://discord.gg/T8X2UaQ7Sm" target="_blank" rel="noopener">Discord</a> or read the <a href="docs.html">user guide</a>.</p>
</main>

<script>
(function(){
  var chips=document.querySelectorAll('.chip'), cats=document.querySelectorAll('.cat');
  function show(f){
    chips.forEach(function(c){c.classList.toggle('on', c.dataset.f===f)});
    cats.forEach(function(s){s.hidden = !(f==='all' || s.dataset.cat===f)});
  }
  chips.forEach(function(c){c.addEventListener('click', function(){show(c.dataset.f)})});
  // a link to #<app id> opens that app's details
  var id=location.hash.slice(1), el=id && document.getElementById(id);
  if(el && el.classList.contains('app')){var d=el.querySelector('details'); if(d) d.open=true}
})();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
