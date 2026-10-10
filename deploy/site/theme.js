/* WADAMESH site: the Night / Day switch and the device's aurora behind every page.
   The sky is auroraCompute() from src/ui-touch/UITask.cpp, line for line: a curtain of light
   hanging from the top (brightest along its wavy lower edge, rays across), a softer band low
   on the screen and a glow breathing in the lower right corner, in the accent's hue and its
   two neighbours, on one seamless 48 s loop. It is worked out on a coarse grid (a point every
   few pixels, like the device's 8 px grid) and the browser stretches it smoothly. */
(function () {
  'use strict';
  var root = document.documentElement;
  var KEY = 'wm-theme';
  var TAU = Math.PI * 2, LOOP_MS = 48000, STILL_P = 0.12, FRAME_MS = 50;

  // ---- palettes: the device's Night and Day (bg, the lit accent COLOR_GLOW) ----
  var PAL = {
    dark:  { bg: [0, 4, 0],       glow: 0x19D6C2, day: false },
    light: { bg: [241, 244, 246], glow: 0x0A7F74, day: true }
  };
  function theme() { return root.getAttribute('data-theme') === 'light' ? 'light' : 'dark'; }

  // hsvRgb() and rgbHue() as on the device (integer steps included)
  function hsvRgb(h, s, v) {
    h = ((h % 360) + 360) % 360;
    var c = Math.trunc(v * s / 100), x = Math.trunc(c * (60 - Math.abs(h % 120 - 60)) / 60), m = v - c;
    var r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    var u8 = function (p) { return Math.trunc((p * 255 + 50) / 100); };
    return [u8(r + m), u8(g + m), u8(b + m)];
  }
  function rgbHue(rgb) {
    var r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    var mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, h;
    if (!d) return 0;
    if (mx === r) h = Math.trunc(60 * (g - b) / d); else if (mx === g) h = Math.trunc(60 * (b - r) / d) + 120;
    else h = Math.trunc(60 * (r - g) / d) + 240;
    return (h + 360) % 360;
  }

  // ---- the sky ----
  var cv = null, ctx = null, img = null, gw = 0, gh = 0, step = 8, vw = 1, vh = 1;
  function size() {
    vw = Math.max(1, window.innerWidth); vh = Math.max(1, window.innerHeight);
    step = Math.max(6, Math.ceil(Math.max(vw, vh) / 200));      // ~200 points across the long side
    var w = Math.ceil(vw / step) + 1, h = Math.ceil(vh / step) + 1;
    if (w !== gw || h !== gh) { gw = w; gh = h; cv.width = gw; cv.height = gh; img = ctx.createImageData(gw, gh); }
  }
  function draw(p) {
    if (!img) return;
    var pal = PAL[theme()], acc = pal.glow, bg = pal.bg, day = pal.day;
    var ar = (acc >> 16) & 255, ag = (acc >> 8) & 255, ab = acc & 255;
    var amx = Math.max(ar, ag, ab), amn = Math.min(ar, ag, ab);
    var h0 = rgbHue(acc);
    var sat = Math.min(80, (amx ? Math.trunc((amx - amn) * 100 / amx) : 0) + 10);   // the Regular look's cap
    var aspect = vw / vh;
    var glow3 = 0.5 + 0.12 * Math.sin(TAU * p);
    var c1 = hsvRgb(h0 + 50, Math.trunc(sat * 9 / 10), 100), c2 = hsvRgb(h0 - 45, Math.trunc(sat * 9 / 10), 100);
    var d = img.data;
    for (var gx = 0; gx < gw; ++gx) {
      var u = gx * step / vw;
      var yc1 = 0.17 + 0.06 * Math.sin(TAU * (0.85 * u + 0.15 + p)) + 0.03 * Math.sin(TAU * (2.2 * u + 0.6 - 2 * p));
      var ray = 0.55 + 0.225 * (1 + Math.sin(TAU * (4.5 * u + 0.35 * Math.sin(TAU * (1.3 * u + p)) - p)));
      var yc2 = 0.74 + 0.05 * Math.sin(TAU * (0.7 * u + 0.55 - p));
      var c0 = hsvRgb(h0 + Math.trunc(16 * Math.sin(TAU * (0.6 * u + 0.1 + p))), sat, 100);
      var du = (u - 0.95) * aspect;
      for (var gy = 0; gy < gh; ++gy) {
        var v = gy * step / vh;
        var d1 = v - yc1, d2 = (v - yc2) * 8.333, dv = v - 1.02;
        // lit all the way up to the top, brightest along the curtain's lower edge, fading below
        var i1 = ray * (d1 < 0 ? 0.7 + 0.3 * Math.exp(-d1 * d1 * 100) : Math.exp(-d1 * 4.545));
        var i2 = 0.55 * Math.exp(-d2 * d2);
        var i3 = glow3 * Math.exp(-(du * du + dv * dv) * 11.11);
        var ws = i1 + i2 + i3, mix = Math.min(0.62, 0.58 * ws);
        var o = (gy * gw + gx) * 4;
        for (var k = 0; k < 3; ++k) {
          var lit = i1 * c0[k] + i2 * c1[k] + i3 * c2[k], x;
          if (!day) x = bg[k] + 0.52 * lit;                                                   // light added to a dark sky
          else x = bg[k] + ((ws > 1e-4 ? lit / ws : bg[k]) * 0.68 + 81.6 - bg[k]) * mix;      // a day sky tinted pastel
          d[o + k] = x < 0 ? 0 : x > 252 ? 252 : x;
        }
        d[o + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0);
  }

  var still = window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : { matches: false };
  var t0 = 0, last = -1e9, raf = 0, phase = STILL_P;
  function frame(now) {
    raf = requestAnimationFrame(frame);
    if (now - last < FRAME_MS) return;
    last = now;
    if (!t0) t0 = now;
    phase = (STILL_P + (now - t0) / LOOP_MS) % 1;
    draw(phase);
  }
  function start() {
    cancelAnimationFrame(raf);
    if (still.matches) { draw(STILL_P); return; }    // reduced motion: the device's still sky
    raf = requestAnimationFrame(frame);
  }
  function repaint() { draw(still.matches ? STILL_P : phase); }

  function initSky() {
    cv = document.getElementById('wm-sky');
    if (!cv) { cv = document.createElement('canvas'); cv.id = 'wm-sky'; cv.setAttribute('aria-hidden', 'true'); document.body.insertBefore(cv, document.body.firstChild); }
    ctx = cv.getContext('2d');
    if (!ctx) return;
    size(); repaint(); start();
    var rt = 0;
    window.addEventListener('resize', function () { clearTimeout(rt); rt = setTimeout(function () { size(); repaint(); }, 120); });
    if (still.addEventListener) still.addEventListener('change', start);
  }

  // ---- Night / Day ----
  function label(t) {
    document.querySelectorAll('[data-theme-toggle]').forEach(function (b) {
      var to = t === 'light' ? 'dark' : 'light';
      b.setAttribute('aria-label', 'Switch to ' + to + ' mode');
      b.setAttribute('title', 'Switch to ' + to + ' mode');
      var lb = b.querySelector('.lb'); if (lb) lb.textContent = to === 'light' ? 'Light mode' : 'Dark mode';
    });
    var m = document.querySelector('meta[name="theme-color"]');
    if (m) m.setAttribute('content', t === 'light' ? '#F1F4F6' : '#000400');
  }
  function setTheme(t) {
    if (t === 'light') root.setAttribute('data-theme', 'light'); else root.removeAttribute('data-theme');
    try { localStorage.setItem(KEY, t); } catch (e) { /* private window: the choice lasts this page */ }
    label(t);
    if (ctx) repaint();
  }
  document.addEventListener('click', function (e) {
    var b = e.target && e.target.closest ? e.target.closest('[data-theme-toggle]') : null;
    if (b) { e.preventDefault(); setTheme(theme() === 'light' ? 'dark' : 'light'); }
  });

  function boot() { label(theme()); initSky(); }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot); else boot();
})();
