// wadamesh in the browser: the page side of web/platform/web_main.cpp.
//
//  - checks the page is cross-origin isolated (the build uses wasm threads, which need
//    SharedArrayBuffer, which needs COOP: same-origin + COEP: require-corp)
//  - mounts IndexedDB (IDBFS) at /data and loads it BEFORE the UI boots
//  - persists /data after writes settle, and on tab hide / page close
//  - picks the canvas size (800x480 on a computer, the screen's own size on a phone or
//    tablet) and scales it to the window (whole device pixels = crisp nearest when that
//    costs little, smooth otherwise); a turn of the phone restarts the UI in the other layout
//  - on a phone, the phone's own keyboard types into the UI (a hidden textarea)
//  - pointer = finger, keyboard = the UI's external keyboard (CardKB codes)
//  - Connect: the demo, or the visitor's own MeshCore node over USB (Web Serial),
//    Bluetooth (Web Bluetooth) or Wi-Fi (WebSocket, the node's port 8765). The choice
//    is kept in localStorage; each backend has its own data dir under /data
//    (/data/demo, /data/node-<key>), and switching reboots the UI into it.
(function () {
  'use strict';
  // The canvas size = the board's resolution, picked once per boot from the window
  // (web_main.cpp reads Module.wadaSize; WEB_BUILD.md "Phones and tablets").
  //  - A computer (fine pointer) in a landscape window: 800x480, the framed device, scaled
  //    to the window (the acceptance tours and the docs shots are at this size).
  //  - A phone or tablet (coarse pointer), or a computer window clearly taller than wide:
  //    the canvas IS the screen. It is sized from the space below the bar (safe areas
  //    out) so it fills that space edge to edge, with about one CSS pixel per canvas pixel
  //    (text as big as the page's own), preferring a width that is a whole number of
  //    device pixels per canvas pixel (crisp): iPhone 14 390x616 at 3:1, Pixel 7 360x692
  //    at 3:1, iPad 540x686 at 3:1. Portrait or landscape follows the window.
  // Turning the phone restarts the UI in the other layout (see "orientation"); data stays.
  var touchDevice = window.matchMedia && window.matchMedia('(pointer: coarse)').matches;
  function wantPortrait() {   // upright; a desktop window must be clearly taller than wide
    return window.innerHeight > window.innerWidth * (touchDevice ? 1.0 : 1.15);
  }
  var portrait = wantPortrait();
  var fill = touchDevice || portrait;   // the canvas fills the stage (no device frame)
  var bootWidth = window.innerWidth;
  var W = 800, H = 480;
  // The best logical length for `css` CSS pixels (dev = the same in device pixels):
  // near `target`, within [lo, hi], a whole number of device pixels per canvas pixel
  // when one is within 16 % of the target (a bit bigger is fine; smaller is not).
  function pickLen(css, dev, target, lo, hi) {
    var best = 0, bestErr = 1e9;
    for (var k = 1; k <= 6; k++) {
      var w = Math.round(dev / k);
      if (w < lo || w > hi || Math.abs(dev / k - w) > 0.75) continue;
      var err = (target - w) / target;   // > 0: the UI is drawn bigger than 1:1
      if (err < -0.02 || err > 0.16) continue;
      if (Math.abs(err) < bestErr) { best = w; bestErr = Math.abs(err); }
    }
    return best || Math.max(lo, Math.min(hi, Math.round(target)));
  }
  // Measured now for the first frame, and again just before the UI boots: a home-screen
  // app on iOS starts with a viewport that is not settled yet (taller than what it ends up
  // as), which cut the UI's bottom bar off until a turn of the phone restarted it.
  function pickSize() {
    if (!fill) return;
    document.body.classList.add('fill');
    if (portrait) document.body.classList.add('portrait');
    var st0 = document.getElementById('stage'), r0 = st0.getBoundingClientRect(), c0 = getComputedStyle(st0);
    var aw0 = Math.max(240, r0.width - parseFloat(c0.paddingLeft) - parseFloat(c0.paddingRight));
    var ah0 = Math.max(200, r0.height - parseFloat(c0.paddingTop) - parseFloat(c0.paddingBottom));
    var dpr0 = window.devicePixelRatio || 1;
    if (portrait) {
      // width: the phone's CSS width (360-430 on phones); a tablet stays near 600 wide
      W = pickLen(aw0, aw0 * dpr0, Math.min(aw0, 600), 300, 640);
      H = Math.max(480, Math.min(1400, Math.floor(W * ah0 / aw0)));   // never taller than the space
    } else {
      // height: 1:1 up to 600 (a phone on its side: ~330; a tablet: ~500-600)
      H = pickLen(ah0, ah0 * dpr0, Math.min(ah0, 600), 280, 640);
      W = Math.max(480, Math.min(1280, Math.round(H * aw0 / ah0)));
      if (W * ah0 / aw0 < H) H = Math.floor(W * ah0 / aw0);   // the width rules (layout): never taller
    }
    W -= W & 1; H -= H & 1;
  }
  pickSize();
  var BUILD = '__WADA_BUILD__';
  var DB_MOUNT = '/data';   // also the IndexedDB database name IDBFS uses

  var $ = function (id) { return document.getElementById(id); };
  var canvas = $('screen'), stage = $('stage'), overlay = $('overlay');
  canvas.width = W; canvas.height = H;
  var ctx = canvas.getContext('2d', { alpha: false });
  var M = null;            // the Emscripten module once created
  var running = false;     // the UI has presented its first frame
  var resetting = false;   // a "Reset demo data" reload is under way: write nothing more

  // ---- which mesh: the demo or a node ----------------------------------------------
  var CHOICE_KEY = 'wadamesh.backend';   // {kind, arg, key?, name?}: the node to connect to
  var NODES_KEY = 'wadamesh.nodes';      // kind|arg -> the node key last seen there
  var HOST_KEY = 'wadamesh.wsHost';
  function lsGet(k) { try { return JSON.parse(localStorage.getItem(k) || 'null'); } catch (e) { return null; } }
  function lsSet(k, v) { try { if (v == null) localStorage.removeItem(k); else localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} }
  var choice = lsGet(CHOICE_KEY);
  if (!choice || !/^(web-serial|ble|ws)$/.test(choice.kind)) choice = null;
  function nodeDir(c) { return DB_MOUNT + '/node-' + (c.key || 'new'); }
  var dataDir = choice ? nodeDir(choice) : DB_MOUNT + '/demo';
  var everUp = false;

  // ---- status / overlay ------------------------------------------------------------
  function setStatus(text, detail, busy) {
    overlay.hidden = false;
    overlay.className = busy ? 'busy' : '';
    $('status').textContent = text;
    $('detail').innerHTML = detail || '';
  }
  function fatal(text, detailHtml, withReload) {
    overlay.hidden = false;
    overlay.className = 'fatal';
    $('status').textContent = text;
    $('detail').innerHTML = detailHtml || '';
    if (withReload) {
      var b = document.createElement('button');
      b.className = 'btn'; b.type = 'button'; b.textContent = 'Reload';
      b.onclick = function () { location.reload(); };
      overlay.appendChild(b);
    }
  }
  var toastTimer = 0;
  function toast(text, ms) {
    var t = $('toast');
    t.textContent = text; t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, ms || 4200);
  }

  // ---- scaling -------------------------------------------------------------------------
  function layout() {
    var full = document.fullscreenElement || document.webkitFullscreenElement;
    var r = stage.getBoundingClientRect();
    var cs = getComputedStyle(stage);
    var aw = r.width - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    var ah = r.height - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    if (!full) ah = Math.max(ah, 120);
    if (fill && !full) {   // the canvas was sized for this space: fill it edge to edge
      // (the keyboard or a toolbar can shorten the space later: the width rules, and
      // fitAboveKeyboard slides the screen up instead of shrinking it)
      var f = aw / W, d = f * (window.devicePixelRatio || 1);
      canvas.style.width = aw + 'px';
      canvas.style.height = Math.round(H * f) + 'px';
      canvas.classList.toggle('crisp', Math.abs(d - Math.round(d)) < 0.02 || d >= 3);
      overflowCheck();
      return;
    }
    var dpr = window.devicePixelRatio || 1;
    var fit = Math.max(0.1, Math.min(aw / W, ah / H));
    var dev = fit * dpr;                 // canvas pixel -> device pixels
    var k = Math.floor(dev + 1e-6);
    var css, crisp;
    if (k >= 1 && k / dev >= 0.8) { css = k / dpr; crisp = true; }   // whole device pixels: nearest is exact
    else { css = fit; crisp = dev >= 3; }                             // fractional: smooth, unless big enough to hide it
    canvas.style.width = Math.round(W * css) + 'px';
    canvas.style.height = Math.round(H * css) + 'px';
    canvas.classList.toggle('crisp', crisp);
  }
  window.addEventListener('resize', layout);
  // The space got shorter than the UI after it booted, with no keyboard up (a viewport
  // that settled late): the bottom of the UI is cut off, so boot it again at the right
  // size. At most once every 30 s, so a browser that keeps changing its mind cannot loop.
  var overflowTimer = 0;
  // How far the UI's bottom edge sits below the visible window (the stage itself can grow
  // with its content, so the window is the measure).
  function overflowPx() {
    var vh = window.innerHeight;
    var r = canvas.getBoundingClientRect();
    return r.bottom - vh;
  }
  function overflowCheck() {
    clearTimeout(overflowTimer);
    if (!running || overflowPx() <= 8) return;
    overflowTimer = setTimeout(function () {
      if (!running || resetting || imeOn || (document.activeElement && document.activeElement.id === 'ime')) return;
      if (overflowPx() <= 8) return;
      var last = 0;
      try { last = +sessionStorage.getItem('wada.refit') || 0; } catch (e) {}
      if (Date.now() - last < 30000) return;
      try { sessionStorage.setItem('wada.refit', String(Date.now())); } catch (e) {}
      restartUi('Fitting the screen');
    }, 900);
  }
  // ---- orientation: the other layout needs a restart of the UI (data kept) -----------
  // Debounced, only when the window's aspect really flipped AND its width changed (a phone
  // keyboard or a collapsing toolbar only changes the height, which is no rotation), and
  // never while the visitor is typing.
  var flipTimer = 0;
  function maybeFlip() {
    clearTimeout(flipTimer);
    flipTimer = setTimeout(function () {
      if (!running || resetting || wantPortrait() === portrait) return;
      if (Math.abs(window.innerWidth - bootWidth) < 40) return;
      if (imeOn || (document.activeElement && document.activeElement.id === 'ime')) return;
      restartUi(portrait ? 'Turning to landscape' : 'Turning to portrait');
    }, 600);
  }
  // A keyboard that was open during the turn: check again once it has closed.
  window.addEventListener('focusout', function () { setTimeout(maybeFlip, 400); });
  window.addEventListener('resize', maybeFlip);
  if (screen.orientation && screen.orientation.addEventListener) screen.orientation.addEventListener('change', maybeFlip);
  document.addEventListener('fullscreenchange', layout);
  document.addEventListener('webkitfullscreenchange', layout);
  if (window.matchMedia) {
    // a zoom / monitor change alters devicePixelRatio without a resize on some browsers
    var watchDpr = function () {
      var mq = window.matchMedia('(resolution: ' + (window.devicePixelRatio || 1) + 'dppx)');
      var on = function () { layout(); watchDpr(); };
      if (mq.addEventListener) mq.addEventListener('change', on, { once: true });
    };
    watchDpr();
  }
  layout();

  // ---- presentation (called by web_main.cpp with the changed rectangle as RGBA) --------
  var imgCache = {};
  var stats = { frames: 0, presents: 0, presentedPx: 0, t0: performance.now() };
  function present(view, x, y, w, h) {
    var key = w + 'x' + h;
    var img = imgCache[key];
    if (!img) {
      img = ctx.createImageData(w, h);
      if (Object.keys(imgCache).length > 48) imgCache = {};
      imgCache[key] = img;
    }
    img.data.set(view);
    ctx.putImageData(img, x, y);
    stats.presents++; stats.presentedPx += w * h;
    if (!running) {
      running = true;
      if (fill) { overflowCheck(); setTimeout(overflowCheck, 1500); }   // a viewport that settled while it booted
      overlay.hidden = true;
      canvas.focus({ preventScroll: true });
    }
  }
  (function countFrames() { stats.frames++; requestAnimationFrame(countFrames); })();

  // ---- location: the UI's GPS (web/platform/WebLocation.cpp) --------------------------------
  // The UI asks for it only while its GPS is switched on (Module.wadaGeoStart / Stop).
  // The browser's permission prompt is never shown on page load: when the visitor has not
  // decided yet, the request waits for a tap (the GPS switch itself, opening the map, any
  // tap). Refused or unavailable: the UI keeps "searching"; nothing asks again until the
  // GPS is switched off and on. The position stays in this page unless the node's own
  // "share position in adverts" setting is on (WEB_BUILD.md "Location").
  var geo = { want: false, watch: null, waitingTap: false, lastTap: -1e9, toldDenied: false };
  function geoUserActive() {
    if (navigator.userActivation) return navigator.userActivation.isActive;
    return performance.now() - geo.lastTap < 5000;
  }
  function geoPermission() {
    if (!navigator.permissions || !navigator.permissions.query) return Promise.resolve('prompt');
    return navigator.permissions.query({ name: 'geolocation' }).then(function (r) { return r.state; },
                                                                   function () { return 'prompt'; });
  }
  function geoBegin() {
    geo.waitingTap = false;
    if (!geo.want || geo.watch !== null || !M) return;
    if (!navigator.geolocation) { M._wadaWebGeoLost(2); return; }
    geo.watch = navigator.geolocation.watchPosition(function (p) {
      var c = p.coords, n = function (v) { return v == null ? NaN : v; };
      if (M && geo.want) M._wadaWebGeoFix(c.latitude, c.longitude, n(c.accuracy), n(c.altitude),
                                          n(c.speed), n(c.heading), p.timestamp / 1000);
    }, function (err) {
      if (err.code !== 1) return;   // unavailable / timeout: the browser keeps trying by itself
      geoEnd();
      if (M) M._wadaWebGeoLost(1);
      if (!geo.toldDenied) { geo.toldDenied = true; toast('The browser does not let this page use your location, so the GPS has no position. You can allow it in the site settings.'); }
    }, { enableHighAccuracy: true, maximumAge: 30000 });
  }
  function geoEnd() {
    if (geo.watch !== null && navigator.geolocation) navigator.geolocation.clearWatch(geo.watch);
    geo.watch = null;
  }
  function geoStart() {
    geo.want = true;
    if (geo.watch !== null) return;
    geoPermission().then(function (state) {
      if (!geo.want || geo.watch !== null) return;
      if (state === 'denied') { if (M) M._wadaWebGeoLost(1); return; }
      if (state === 'granted' || geoUserActive()) geoBegin();
      else geo.waitingTap = true;   // switched on at boot: ask on the visitor's next tap
    });
  }
  function geoStop() { geo.want = false; geo.waitingTap = false; geoEnd(); }
  function geoTap() {
    geo.lastTap = performance.now();
    if (geo.waitingTap && geo.want) geoBegin();
  }

  // ---- input -----------------------------------------------------------------------------
  var activePointer = null;
  var lastPt = [0, 0];      // where the finger last was, so a forced release lands there
  function toCanvas(e) {
    var r = canvas.getBoundingClientRect();
    return [Math.floor((e.clientX - r.left) * W / r.width), Math.floor((e.clientY - r.top) * H / r.height)];
  }
  function sendPointer(down, e) {
    if (!M || !running) return;
    var p = toCanvas(e);
    lastPt = p;
    M._wadaWebPointer(down ? 1 : 0, p[0], p[1]);
  }
  canvas.addEventListener('pointerdown', function (e) {
    // One finger: the panel is single-touch. A mouse has only one pointer, so a new
    // mouse press while one is "active" means its release was lost (released outside
    // the window, a focus change): drop the stale one instead of ignoring input forever.
    if (activePointer !== null) {
      if (e.pointerType !== 'mouse') return;
      activePointer = null;
      if (M && running) M._wadaWebPointer(0, lastPt[0], lastPt[1]);
    }
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    activePointer = e.pointerId;
    geoTap();
    try { canvas.setPointerCapture(e.pointerId); } catch (_) {}
    // While the phone keyboard is up, a tap on the UI (Send, a bubble) leaves it up.
    if (!(touchDevice && imeOn)) canvas.focus({ preventScroll: true });
    sendPointer(true, e);
    e.preventDefault();
  });
  canvas.addEventListener('pointermove', function (e) {
    if (e.pointerId !== activePointer) return;
    sendPointer(true, e);
  });
  function pointerEnd(e) {
    if (e.pointerId !== activePointer) return;
    activePointer = null;
    sendPointer(false, e);
    // A tap on a text field opens the phone's own keyboard, here, inside the gesture
    // (iOS only shows it for a focus() made in the user's own event).
    if (e.type === 'pointerup' && touchDevice && M && running) {
      var p = toCanvas(e);
      if (M._wadaWebTextareaAt(p[0], p[1])) imeOpen();
      else if (imeOn && document.activeElement !== ime) ime.focus({ preventScroll: true });   // the edit goes on
    }
  }
  canvas.addEventListener('pointerup', pointerEnd);
  canvas.addEventListener('pointercancel', pointerEnd);
  canvas.addEventListener('lostpointercapture', pointerEnd);
  // Window loses focus mid-press (alt-tab, a dialog): release, or the UI keeps a
  // phantom finger down.
  window.addEventListener('blur', function () {
    if (activePointer === null) return;
    activePointer = null;
    if (M && running) M._wadaWebPointer(0, lastPt[0], lastPt[1]);
  });
  canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });
  // The mouse wheel / two-finger trackpad scroll: scrolls the list or page under the
  // pointer (web/platform/WebLvglInput.cpp). Pixels, lines and pages all become canvas pixels.
  var wheelRest = 0;
  canvas.addEventListener('wheel', function (e) {
    if (!M || !running || e.ctrlKey) return;   // ctrl+wheel / pinch: the browser's zoom
    e.preventDefault();
    var r = canvas.getBoundingClientRect();
    // deltaMode 0: CSS pixels (scaled to canvas pixels), 1: lines (16 px), 2: pages
    var k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? H * 0.9 : H / r.height;
    var dy = e.deltaY * k + wheelRest;
    var whole = Math.trunc(dy);
    wheelRest = dy - whole;
    if (!whole) return;
    var p = toCanvas(e);
    M._wadaWebWheel(p[0], p[1], whole);
  }, { passive: false });

  // The computer keyboard is the UI's external keyboard (HAS_CARDKB): ASCII, Enter,
  // Backspace, Esc (Back), Tab, arrows. Same codes as host_main.cpp's keyDown().
  var KEYS = { Enter: 0x0D, Backspace: 0x08, Escape: 0x1B, Tab: 0x09,
               ArrowLeft: 0xB4, ArrowUp: 0xB5, ArrowDown: 0xB6, ArrowRight: 0xB7 };
  document.addEventListener('keydown', function (e) {
    if (!M || !running) return;
    if (e.ctrlKey || e.metaKey || e.altKey) return;        // browser shortcuts stay the browser's
    var t = e.target;
    if ($('connect-dlg').open || (t && /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;   // the page's own fields
    if (t && t !== canvas && t !== document.body && /^(BUTTON|A)$/.test(t.tagName)) {
      if (e.key === 'Enter' || e.key === ' ' || e.key === 'Tab') return;   // the bar's own buttons
    }
    var code = KEYS[e.key];
    if (code === undefined && e.key.length === 1) {
      var c = e.key.charCodeAt(0);
      if (c >= 0x20 && c < 0x7F) code = c;
    }
    if (code === undefined && Array.from(e.key).length === 1 && e.key.codePointAt(0) >= 0xA0) {
      e.preventDefault();   // any other script, typed on the computer: as text (UTF-8)
      geoTap();
      M.ccall('wadaWebText', null, ['string'], [e.key]);
      return;
    }
    if (code === undefined) return;
    e.preventDefault();
    geoTap();
    M._wadaWebKey(code);
  });

  // ---- the phone's own keyboard --------------------------------------------------------
  // On a touch device the UI's on-screen keys stay down (the phone keyboard counts as an
  // external keyboard: wadaWebNativeKeyboard) and a hidden <textarea> takes the typing:
  // autocorrect, emoji, every language, voice. What changes in it goes to the bound LVGL
  // field as text (UTF-8) and Backspaces, as a diff, so a composing / autocorrecting
  // keyboard is followed word by word. A leading space is a sentinel the keyboard can
  // delete, so Backspace works on an empty field too. Enter = the UI's Enter (send).
  var ime = $('ime'), IME_PAD = ' ';
  var imeLast = IME_PAD, imeOn = false, imeComposing = false, imeIdle = 0;
  function imeReset() {
    ime.value = IME_PAD; imeLast = IME_PAD;
    try { ime.setSelectionRange(1, 1); } catch (e) {}
  }
  function imeOpen() {
    if (!M || !running) return;
    if (!imeOn) imeReset();
    imeOn = true; imeIdle = 0;
    ime.focus({ preventScroll: true });
  }
  function imeClose() {
    imeOn = false;
    if (document.activeElement === ime) ime.blur();
    fitAboveKeyboard();
  }
  function imeKey(code) { if (M && running) M._wadaWebKey(code); }
  function imeSync() {
    if (!M || !running) return;
    var a = Array.from(imeLast), b = Array.from(ime.value), p = 0;
    while (p < a.length && p < b.length && a[p] === b[p]) p++;
    for (var i = p; i < a.length; i++) imeKey(0x08);
    var add = b.slice(p).join('');
    if (add) {
      var parts = add.split(/\r?\n/);
      parts.forEach(function (t, j) {
        if (t) M.ccall('wadaWebText', null, ['string'], [t]);
        if (j < parts.length - 1) imeKey(0x0D);
      });
    }
    imeLast = ime.value;
    if (!imeComposing && (ime.value.indexOf(IME_PAD) !== 0 || ime.value.length > 400 || /\n/.test(ime.value))) imeReset();
  }
  ime.addEventListener('input', imeSync);
  ime.addEventListener('compositionstart', function () { imeComposing = true; });
  ime.addEventListener('compositionend', function () { imeComposing = false; imeSync(); });
  ime.addEventListener('beforeinput', function (e) {
    if (e.inputType === 'insertLineBreak' || e.inputType === 'insertParagraph') {
      e.preventDefault(); imeKey(0x0D); imeReset();
    }
  });
  ime.addEventListener('keydown', function (e) {
    if (imeComposing || e.isComposing) return;
    if (e.key === 'Enter') { e.preventDefault(); imeKey(0x0D); imeReset(); }
    else if (e.key === 'Escape') { e.preventDefault(); imeKey(0x1B); }
    else if (e.key === 'Backspace' && ime.value === '') { e.preventDefault(); imeKey(0x08); imeReset(); }
  });
  ime.addEventListener('blur', function () { imeOn = false; setTimeout(fitAboveKeyboard, 50); });
  // iOS opens its keyboard only for a focus() made inside the touch's own handlers, and it
  // is touchend (not every pointerup) it trusts for that: the same check, here too.
  canvas.addEventListener('touchend', function (e) {
    if (!touchDevice || !M || !running || !e.changedTouches.length) return;
    var t = e.changedTouches[0], p = toCanvas(t);
    if (M._wadaWebTextareaAt(p[0], p[1])) imeOpen();
  });
  // A field the UI focused by itself (a screen that opens with its text field active, the
  // composer after a tap on a chat) has no tap of its own to open the phone's keyboard:
  // a Keyboard button is that tap.
  var kbPill = $('kb-pill');
  kbPill.addEventListener('click', function () { kbPill.hidden = true; imeOpen(); });
  setInterval(function () {
    var want = touchDevice && !!M && running && !imeOn && !!M._wadaWebEditing();
    if (kbPill.hidden === want) kbPill.hidden = !want;
  }, 250);
  // The edit ended in the UI (the chat closed, a field was left): the keyboard goes too.
  setInterval(function () {
    if (!imeOn || !M || !running) return;
    imeIdle = M._wadaWebEditing() ? 0 : imeIdle + 1;
    if (imeIdle >= 2) imeClose();
  }, 300);
  // While the keyboard is up, slide the screen up so its bottom (the composer) sits on
  // the keyboard instead of under it.
  function fitAboveKeyboard() {
    var frame = $('frame'), vv = window.visualViewport;
    if (!vv || !imeOn) { frame.style.transform = ''; return; }
    frame.style.transform = '';
    var r = canvas.getBoundingClientRect();
    var visibleBottom = vv.offsetTop + vv.height;
    var lift = Math.min(0, visibleBottom - r.bottom);
    frame.style.transform = lift ? 'translateY(' + Math.round(lift) + 'px)' : '';
  }
  if (window.visualViewport) {
    window.visualViewport.addEventListener('resize', fitAboveKeyboard);
    window.visualViewport.addEventListener('scroll', fitAboveKeyboard);
  }

  // ---- storage: IndexedDB behind /data -----------------------------------------------
  var persistState = { inflight: false, again: false, waiters: [], lastGen: -1, savedGen: -1,
                       changedAt: 0, firstUnsaved: 0, saves: 0 };
  // One IDBFS sync at a time; a request during a sync queues one more after it (and its
  // callback waits for that one, which includes its writes).
  function persist(done) {
    var p = persistState;
    if (done) p.waiters.push(done);
    if (!M || !M.FS || resetting) { var w0 = p.waiters; p.waiters = []; w0.forEach(function (f) { f(); }); return; }
    if (p.inflight) { p.again = true; return; }
    var gen = M._wadaWebFsGeneration();
    var mine = p.waiters; p.waiters = [];
    p.inflight = true;
    M.FS.syncfs(false, function (err) {
      p.inflight = false;
      if (err) console.warn('[web] saving to IndexedDB failed', err);
      else { p.savedGen = gen; p.firstUnsaved = 0; p.saves++; }
      mine.forEach(function (f) { f(); });
      if (p.again) { p.again = false; persist(); }
    });
  }
  // Persist once writes settle (0.8 s quiet), and at least every 5 s while they continue.
  setInterval(function () {
    if (!M || !running) return;
    var p = persistState, now = performance.now();
    var gen = M._wadaWebFsGeneration();
    if (gen !== p.lastGen) { p.lastGen = gen; p.changedAt = now; if (!p.firstUnsaved) p.firstUnsaved = now; }
    if (gen !== p.savedGen && !p.inflight && (now - p.changedAt > 800 || now - p.firstUnsaved > 5000)) persist();
  }, 400);
  function flushNow() {
    if (!M || !running || resetting) return;
    try { M._wadaWebFlush(); } catch (e) { console.warn(e); }
    persist();
  }
  document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'hidden') flushNow(); });
  window.addEventListener('pagehide', flushNow);

  // ---- the bar -------------------------------------------------------------------------
  function setMode(cls, text, title) {
    var b = $('mode');
    b.className = 'badge' + (cls ? ' ' + cls : '');
    b.textContent = text;
    b.title = title || text;
  }
  // Save what the running UI has and boot it again (an orientation change).
  function restartUi(why) {
    running = false;
    setStatus(why, '', true);
    try { if (M && M._wadaWebFlush) M._wadaWebFlush(); } catch (e) {}
    persist(function () { location.reload(); });
  }
  // Save what the running UI has, then boot the UI again into `next` (null = the demo).
  function switchTo(next) {
    lsSet(CHOICE_KEY, next);
    running = false;
    setStatus(next ? 'Connecting to your node' : 'Back to the demo', '', true);
    try { if (M && M._wadaWebFlush) M._wadaWebFlush(); } catch (e) {}
    persist(function () { location.reload(); });
  }
  function nodeKeyFor(kind, arg) { var m = lsGet(NODES_KEY) || {}; return m[kind + '|' + arg] || ''; }
  function rememberNode(c) {
    var m = lsGet(NODES_KEY) || {};
    m[c.kind + '|' + c.arg] = c.key;
    lsSet(NODES_KEY, m);
  }
  function connectTo(kind, arg) {
    $('connect-dlg').close();
    switchTo({ kind: kind, arg: arg, key: nodeKeyFor(kind, arg) });
  }

  // The chooser: only the transports this browser has.
  var NUS = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  var hasSerial = !!navigator.serial, hasBle = !!navigator.bluetooth;
  (function setupChooser() {
    var dlg = $('connect-dlg');
    dlg.querySelector('[data-kind="web-serial"]').hidden = !hasSerial;
    dlg.querySelector('[data-kind="ble"]').hidden = !hasBle;
    // The two copies of the page (WEB_BUILD.md "Two copies"): https has USB, Bluetooth and
    // the location (secure context), http reaches a node's plain ws:// over Wi-Fi.
    var local = /^(localhost|127\.0\.0\.1|\[::1\])$/.test(location.hostname);
    var secure = window.isSecureContext;
    var ios = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    var notes = [], links = [];
    if (!secure) {
      notes.push('This copy of the page connects over Wi-Fi.');
      links.push(['For USB, Bluetooth and your location, use ', secureCopyUrl()]);
    } else if (ios) notes.push('On iPhone and iPad the browser has no Bluetooth or USB: connect over Wi-Fi.');
    else if (!hasSerial && !hasBle) notes.push('USB and Bluetooth need Chrome or Edge; this browser can connect over Wi-Fi.');
    else if (!hasBle) notes.push('Bluetooth needs Chrome or Edge.');
    else if (!hasSerial) notes.push('USB needs Chrome or Edge on a computer.');
    var note = $('connect-note');
    note.textContent = notes.join(' ');
    links.forEach(function (l) {
      var span = document.createElement('span');
      span.className = 'link-line';
      span.appendChild(document.createTextNode(l[0]));
      var a = document.createElement('a');
      a.href = l[1]; a.textContent = l[1].replace(/\/$/, '');
      span.appendChild(a);
      note.appendChild(span);
    });
    note.hidden = !notes.length && !links.length;
    var host = $('ws-host');
    host.value = lsGet(HOST_KEY) || '';
    dlg.querySelector('[data-kind="web-serial"]').addEventListener('click', function () {
      navigator.serial.requestPort().then(function (port) {
        var i = port.getInfo ? port.getInfo() : {};
        connectTo('web-serial', i.usbVendorId != null ? i.usbVendorId + ':' + i.usbProductId : '');
      }, function () { /* the visitor closed the picker */ });
    });
    dlg.querySelector('[data-kind="ble"]').addEventListener('click', function () {
      navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }, { namePrefix: 'MeshCore' }], optionalServices: [NUS] })
        .then(function (dev) { connectTo('ble', dev.id); }, function () {});
    });
    var goWs = function () {
      var h = host.value.trim();
      if (!h) { host.focus(); return; }
      lsSet(HOST_KEY, h);
      var url = wsUrlFor(h);
      // The https copy may not open a node's plain ws://: say so, then carry the address
      // to the Wi-Fi (http) copy, which connects to it on arrival (#wifi=<address>).
      if (location.protocol === 'https:' && !local && /^ws:/i.test(url)) { wifiHandoff(h); return; }
      connectTo('ws', url);
    };
    function wifiHandoff(addr) {
      var wifi = dlg.querySelector('.choice.wifi');
      var old = $('wifi-handoff');
      if (old) old.remove();
      var box = document.createElement('div');
      box.id = 'wifi-handoff';
      var p = document.createElement('p');
      p.textContent = 'Wi-Fi opens the Wi-Fi version of this page (' + wifiCopyUrl('').replace(/^http:\/\/|\/?#wifi=$/g, '') +
        ') and connects to ' + addr + ' there. Browsers only let a plain http page talk to a node on your ' +
        'network; USB, Bluetooth and your location stay on this secure page.';
      var row = document.createElement('div');
      row.className = 'row';
      var go = document.createElement('button');
      go.type = 'button'; go.className = 'btn primary'; go.textContent = 'Continue';
      go.addEventListener('click', function () { location.href = wifiCopyUrl(addr); });
      var no = document.createElement('button');
      no.type = 'button'; no.className = 'btn'; no.textContent = 'Cancel';
      no.addEventListener('click', function () { box.remove(); host.focus(); });
      row.appendChild(go); row.appendChild(no);
      box.appendChild(p); box.appendChild(row);
      wifi.appendChild(box);
      go.focus();
    }
    dlg.querySelector('[data-kind="ws"]').addEventListener('click', goWs);
    host.addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); goWs(); } });
  })();

  // The hosted client has two names: app.wadamesh.com (https, behind Cloudflare) and
  // wifi.app.wadamesh.com, which answers plain http only. Chrome upgrades a typed or linked
  // http:// to https whenever the name answers https, which would bring Wi-Fi visitors
  // straight back to the copy that may not open ws://; a name with no https stays http.
  // (function declarations, not a var: the chooser above calls these before this runs)
  function wifiHosts() { return { 'app.wadamesh.com': 'wifi.app.wadamesh.com' }; }
  function wifiCopyUrl(addr) {
    var h = wifiHosts()[location.hostname] || location.host;
    return 'http://' + h + location.pathname + (addr ? '#wifi=' + encodeURIComponent(addr) : '#wifi=');
  }
  function wsUrlFor(h) {
    return /^wss?:\/\//i.test(h) ? h : 'ws://' + (/:\d+$/.test(h) ? h : h + ':8765') + '/';
  }
  function secureCopyUrl() {
    var m = wifiHosts();
    for (var k in m) if (m[k] === location.hostname) return 'https://' + k + location.pathname;
    return 'https://' + location.host + location.pathname;
  }
  // Arrived from the https copy with an address: the chooser, Wi-Fi, that address.
  (function wifiFromHash() {
    var m = /^#wifi=(.*)$/.exec(location.hash);
    if (!m) return;
    history.replaceState(null, '', location.pathname + location.search);
    if (choice) return;   // already on a node
    var addr = decodeURIComponent(m[1] || '');
    if (!addr) { setTimeout(function () { $('btn-connect').click(); $('ws-host').focus(); }, 0); return; }
    $('ws-host').value = addr; lsSet(HOST_KEY, addr);
    try { sessionStorage.setItem('wada.wifiNotice', '1'); } catch (e) {}
    setTimeout(function () { connectTo('ws', wsUrlFor(addr)); }, 0);
  })();
  // Just handed over from the https copy (the connect reloads the page): say where we are.
  (function wifiNotice() {
    if (!choice || choice.kind !== 'ws') return;   // still the demo: the connect reloads into the node
    var shown = null;
    try { shown = sessionStorage.getItem('wada.wifiNotice'); sessionStorage.removeItem('wada.wifiNotice'); } catch (e) {}
    if (shown)
      toast('This is the Wi-Fi version of the web client: plain http, so it can reach your node. ' +
            'For USB, Bluetooth and your location use ' + secureCopyUrl().replace(/^https:\/\/|\/$/g, '') + '.', 9000);
  })();
  $('btn-connect').addEventListener('click', function () {
    if (choice) { switchTo(null); return; }   // Disconnect: back to the demo
    var dlg = $('connect-dlg');
    if (dlg.showModal) dlg.showModal(); else dlg.setAttribute('open', '');
    if (!hasSerial && !hasBle) $('ws-host').focus();
  });
  // Web Serial / Bluetooth without a device this page may open by itself: one click.
  $('btn-pair').addEventListener('click', function () {
    $('btn-pair').hidden = true;
    var wc = M && M.WadaCompanion;
    if (wc) wc.connect().catch(function (e) { toast(String(e && e.message || e)); });
  });
  if (choice) {
    $('btn-connect').textContent = 'Disconnect';
    $('btn-connect').classList.remove('primary');
    $('btn-reset').hidden = true;
    setMode('wait', 'Connecting');
    var hint = document.querySelector('.hint');
    if (hint) hint.textContent = 'Mouse or finger = touch. Your keyboard types into the focused field, Enter sends, Esc goes back. ' +
      'Messages go out through your node; this browser keeps a copy of the chats.';
  }
  // From web_main.cpp (BackendSelect.h g_wada_link_hook): state 1 = up, 0 = (re)connecting.
  function onLink(state, name, via, key) {
    if (!choice) return;
    if (state === 1) {
      everUp = true;
      $('btn-pair').hidden = true;
      setMode('up', 'Connected: ' + name + ' over ' + via);
      if (key && choice.key !== key) {
        var first = !choice.key;
        choice.key = key; choice.name = name;
        lsSet(CHOICE_KEY, choice);
        rememberNode(choice);
        // A different node on the same cable / address: its own data dir, so the two
        // nodes' contacts and messages never mix. (The first time, the data this
        // session wrote moves there on the next start.)
        if (!first) { toast('This is a different node: opening its own data.'); switchTo(choice); }
      } else if (name && choice.name !== name) {
        choice.name = name; lsSet(CHOICE_KEY, choice);
      }
    } else {
      setMode('wait', everUp ? 'Reconnecting' : 'Connecting', linkDetail || undefined);
    }
  }
  var linkDetail = '';
  // A node that never answered gets a plain explanation instead of an endless
  // "Connecting": the likely causes, and the demo one click away. (Kaj's V4 sat on
  // another VLAN, 192.168.20.x, that this computer could not route to.)
  var linkFails = 0;
  function linkTrouble(up, detail) {
    var box = $('link-help');
    if (up) { linkFails = 0; if (box && !box.hidden) { box.hidden = true; layout(); } return; }
    if (/^paused/.test(detail || '')) return;   // this page was out of sight and let the node go
    // The node dropped us for another client (companion-transports.js): worth saying even
    // after the link was up once, since it looks like a connection that never finishes.
    var taken = !!choice && choice.kind === 'ws' && /another app or browser/.test(detail || '');
    if ((everUp && !taken) || !choice) return;
    var mixed = choice.kind === 'ws' && /mixed content/.test(detail || '');
    if (++linkFails < 2 && !mixed && !taken) return;  // one miss can be a slow start (a blocked ws:// cannot)
    setMode('bad', taken ? 'Node in use elsewhere' : "Can't reach your node", detail || undefined);
    if (!box) {
      box = document.createElement('div');
      box.id = 'link-help';
      box.className = 'link-help';
      var bar = document.querySelector('.bar');
      bar.parentNode.insertBefore(box, bar.nextSibling);
    }
    var how = taken ? 'Your node serves one Wi-Fi app at a time, and something else just took it: another tab or ' +
        'window of this page, the home-screen app, another phone or computer, or the MeshCore app over Wi-Fi. Close that ' +
        'one and this page takes over again; it keeps trying, more slowly.'
      : mixed ? 'This secure (https) copy of the page may not open a plain ws:// connection to your node. Use the http copy for Wi-Fi: ' + wifiCopyUrl('').replace(/#wifi=$/, '')
      : choice.kind === 'ws'
      ? 'Check that the node has Wi-Fi on and is on the same network as this computer (a guest or IoT network often cannot reach the rest), and that you typed its address right: ' + choice.arg.replace(/^wss?:\/\//, '').replace(/\/$/, '') + '.'
      : choice.kind === 'web-serial'
        ? 'Check the cable (a charge-only cable carries no data) and that no other app has the port open.'
        : 'Check that the node is on, in range, and has Bluetooth on.';
    box.innerHTML = '';
    var p = document.createElement('p');
    p.textContent = mixed || taken ? how : 'No answer from your node yet. ' + how + ' Still trying every few seconds.';
    // A ws:// that never connected from the https copy (blocked, or a silent timeout): the
    // http copy is the way, with this address.
    if (choice.kind === 'ws' && location.protocol === 'https:' && /^ws:/i.test(choice.arg)) {
      var addr = choice.arg.replace(/^wss?:\/\//, '').replace(/\/$/, '');
      var copy = document.createElement('button');
      copy.type = 'button'; copy.className = 'btn primary'; copy.textContent = 'Open the Wi-Fi version';
      copy.addEventListener('click', function () { lsSet(CHOICE_KEY, null); location.href = wifiCopyUrl(addr); });
      box.appendChild(p); box.appendChild(copy);
      p = null;
    }
    var back = document.createElement('button');
    back.type = 'button'; back.className = 'btn'; back.textContent = 'Back to the demo';
    back.addEventListener('click', function () { switchTo(null); });
    if (p) box.appendChild(p);
    box.appendChild(back);
    box.hidden = false;
    layout();   // the box takes height from the stage: refit the screen below it
  }
  $('btn-reset2').hidden = !!choice;
  $('btn-reset2').addEventListener('click', function () { menuOpen(false); $('btn-reset').click(); });
  $('btn-reset').addEventListener('click', function () {
    if (!confirm('Reset the demo? Your messages, contacts and settings in this browser are deleted and the demo starts fresh.')) return;
    resetting = true;
    var u = new URL(location.href);
    u.searchParams.set('reset', '1');
    location.replace(u.toString());
  });
  // ---- files: the UI's downloads + file picker (web/platform/WebFiles.cpp) -------------
  function download(bytes, name, type) {
    var url = URL.createObjectURL(new Blob([bytes], { type: type || 'application/octet-stream' }));
    var a = document.createElement('a');
    a.href = url; a.download = name; a.rel = 'noopener';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 30000);
  }
  // The UI asked for a file (Settings > Backups > Import from this computer): the chosen
  // file goes into this backend's SPIFFS root, then the UI's own import takes it.
  var pickInput = $('file-pick');
  var pickFor = null;   // 'ui' (into the data dir) or 'restore' (a browser backup)
  function pickFile(accept, purpose) {
    pickFor = purpose;
    pickInput.value = '';
    pickInput.accept = accept || '';
    pickInput.click();
  }
  pickInput.addEventListener('change', function () {
    var f = pickInput.files && pickInput.files[0];
    if (!f) return;
    f.arrayBuffer().then(function (buf) {
      var bytes = new Uint8Array(buf);
      if (pickFor === 'restore') { restoreBrowser(bytes, f.name); return; }
      if (!M || !M.FS) return;
      var name = f.name.replace(/[^A-Za-z0-9._-]/g, '_').replace(/^\.+/, '').slice(0, 48) || 'import.json';
      if (!/\.json$/i.test(name)) name += '.json';
      M.FS.writeFile(dataDir + '/spiffs/' + name, bytes);
      persist();
      M.ccall('wadaWebFilePicked', null, ['string'], ['/' + name]);
    });
  });

  // ---- "Back up this browser" / "Restore": the active backend's whole data dir --------
  // One tar file (ustar), so it moves to another browser or computer. The map tile cache
  // is left out (it downloads again). A marker file names what it is.
  var TAR_MARK = 'wadamesh-browser-backup.json';
  function tarFiles(entries) {   // [{path, data:Uint8Array}] -> Uint8Array
    var enc = new TextEncoder(), blocks = [], total = 0;
    var oct = function (n, len) { var t = n.toString(8); while (t.length < len - 1) t = '0' + t; return t + '\0'; };
    entries.forEach(function (e) {
      var h = new Uint8Array(512), put = function (str, off) { h.set(enc.encode(str), off); };
      var name = e.path, prefix = '';
      if (name.length > 99) { var cut = name.lastIndexOf('/', 154); prefix = name.slice(0, cut); name = name.slice(cut + 1); }
      put(name, 0); put('0000644\0', 100); put(oct(0, 8), 108); put(oct(0, 8), 116);
      put(oct(e.data.length, 12), 124); put(oct(Math.floor(Date.now() / 1000), 12), 136);
      put('        ', 148); put('0', 156); put('ustar\0', 257); put('00', 263); put(prefix, 345);
      var sum = 0; for (var i = 0; i < 512; i++) sum += h[i];
      put(oct(sum, 7) + ' ', 148);
      blocks.push(h); total += 512;
      var pad = new Uint8Array(Math.ceil(e.data.length / 512) * 512); pad.set(e.data);
      blocks.push(pad); total += pad.length;
    });
    blocks.push(new Uint8Array(1024)); total += 1024;
    var out = new Uint8Array(total), o = 0;
    blocks.forEach(function (b) { out.set(b, o); o += b.length; });
    return out;
  }
  function untar(bytes) {   // -> [{path, data}]
    var dec = new TextDecoder(), out = [], o = 0;
    var str = function (a, b) { var s = dec.decode(bytes.subarray(a, b)); var z = s.indexOf('\0'); return z >= 0 ? s.slice(0, z) : s; };
    while (o + 512 <= bytes.length) {
      if (bytes[o] === 0) break;
      var name = str(o, o + 100), size = parseInt(str(o + 124, o + 136).trim() || '0', 8), type = str(o + 156, o + 157);
      var prefix = str(o + 345, o + 500);
      if (prefix) name = prefix + '/' + name;
      o += 512;
      if (type === '0' || type === '') out.push({ path: name, data: bytes.slice(o, o + size) });
      o += Math.ceil(size / 512) * 512;
    }
    return out;
  }
  function walk(FS, dir, rel, out) {
    FS.readdir(dir).forEach(function (n) {
      if (n === '.' || n === '..') return;
      var p = dir + '/' + n, r = rel ? rel + '/' + n : n, st = FS.stat(p);
      if (FS.isDir(st.mode)) { if (r !== 'littlefs/tiles') walk(FS, p, r, out); }
      else out.push({ path: r, data: FS.readFile(p) });
    });
  }
  function backupBrowser() {
    if (!M || !M.FS || !running) return;
    try { M._wadaWebFlush(); } catch (e) {}
    var files = [];
    walk(M.FS, dataDir, '', files);
    var who = choice ? (choice.name || 'node') : 'demo';
    files.unshift({ path: TAR_MARK, data: new TextEncoder().encode(JSON.stringify({
      kind: 'wadamesh-browser-backup', version: 1, backend: choice ? choice.kind : 'demo', node: choice ? (choice.name || '') : '',
      dataDir: dataDir, created: new Date().toISOString(), build: BUILD, files: files.length }, null, 1)) });
    var day = new Date().toISOString().slice(0, 10);
    download(tarFiles(files), 'wadamesh-browser-' + who.replace(/[^A-Za-z0-9_-]+/g, '-') + '-' + day + '.tar', 'application/x-tar');
    toast('Saved ' + (files.length - 1) + ' files. Restore them from this menu in any browser.');
  }
  function restoreBrowser(bytes, fname) {
    var files = untar(bytes), mark = null;
    files.forEach(function (f) { if (f.path === TAR_MARK) { try { mark = JSON.parse(new TextDecoder().decode(f.data)); } catch (e) {} } });
    if (!mark || mark.kind !== 'wadamesh-browser-backup') { toast(fname + ' is not a wadamesh browser backup.'); return; }
    var into = choice ? 'your node\'s data in this browser' : 'the demo';
    if (!confirm('Restore ' + fname + '? It replaces ' + into + ' (messages, contacts, settings, apps), then restarts.')) return;
    if (!M || !M.FS) return;
    var FS = M.FS;
    resetting = true;   // nothing else writes or saves from here
    rmrf(FS, dataDir);
    FS.mkdir(dataDir);
    files.forEach(function (f) {
      if (f.path === TAR_MARK || /(^|\/)\.\.(\/|$)/.test(f.path) || f.path[0] === '/') return;
      var parts = f.path.split('/'), d = dataDir;
      for (var i = 0; i < parts.length - 1; i++) { d += '/' + parts[i]; if (!exists(FS, d)) FS.mkdir(d); }
      FS.writeFile(dataDir + '/' + f.path, f.data);
    });
    setStatus('Restoring', '', true);
    FS.syncfs(false, function () { location.reload(); });
  }
  var moreBtn = $('btn-more'), moreMenu = $('more-menu');
  function menuOpen(on) { moreMenu.hidden = !on; moreBtn.setAttribute('aria-expanded', on ? 'true' : 'false'); }
  moreBtn.addEventListener('click', function (e) { e.stopPropagation(); menuOpen(moreMenu.hidden); });
  document.addEventListener('click', function (e) { if (!moreMenu.hidden && !moreMenu.contains(e.target)) menuOpen(false); });
  $('btn-backup').addEventListener('click', function () { menuOpen(false); backupBrowser(); });
  $('btn-restore').addEventListener('click', function () { menuOpen(false); pickFile('.tar,application/x-tar', 'restore'); });

  var fsBtn = $('btn-full');
  var canFull = stage.requestFullscreen || stage.webkitRequestFullscreen;
  if (!canFull) fsBtn.hidden = true;
  fsBtn.addEventListener('click', function () {
    var full = document.fullscreenElement || document.webkitFullscreenElement;
    if (full) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    else (stage.requestFullscreen || stage.webkitRequestFullscreen).call(stage);
  });

  // ---- boot --------------------------------------------------------------------------------
  if (typeof WebAssembly !== 'object') {
    fatal('This browser cannot run wadamesh', 'It needs WebAssembly. Any current Chrome, Edge, Firefox or Safari has it.');
    return;
  }
  // Two bundles (WEB_BUILD.md "Two copies"): with cross-origin isolation (https, or a
  // localhost dev server) the threaded one next to the page; without it (the http copy,
  // which reaches a node's ws:// over Wi-Fi) the no-threads one in nt/.
  var isolated = !!window.crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined';
  // ?nt forces the no-threads build (tests: the http copy's engine on any page)
  var BUNDLE = isolated && !/[?&]nt\b/.test(location.search) ? '' : 'nt/';
  var devHost = /^(localhost|127\.0\.0\.1|\[::1\])$/.test(location.hostname);
  function cannotLoad() {
    if (devHost) {
      fatal('Could not load wadamesh', 'The ' + (isolated ? 'threaded' : 'no-threads') + ' bundle (<code>' + BUNDLE +
        'wadamesh.js</code>) did not load. <code>web/build.sh web</code> builds both; <code>web/build.sh serve</code> serves ' +
        'the no-threads one at <code>nt/</code>.', true);
    } else {
      fatal('wadamesh could not start here', 'This browser blocks something the web client needs. Try a current ' +
        'Chrome, Edge, Firefox or Safari.', true);
    }
  }

  var params = new URLSearchParams(location.search);
  var wantReset = params.get('reset') === '1';
  if (wantReset) {
    params.delete('reset');
    var q = params.toString();
    history.replaceState(null, '', location.pathname + (q ? '?' + q : '') + location.hash);
  }

  // Each backend boots on its own directory under /data: /data/demo, /data/node-<key>.
  function exists(FS, p) { try { FS.stat(p); return true; } catch (e) { return false; } }
  function rmrf(FS, p) {
    var st; try { st = FS.stat(p); } catch (e) { return; }
    if (FS.isDir(st.mode)) {
      FS.readdir(p).forEach(function (n) { if (n !== '.' && n !== '..') rmrf(FS, p + '/' + n); });
      FS.rmdir(p);
    } else FS.unlink(p);
  }
  function prepareDataDirs(FS) {
    var demo = DB_MOUNT + '/demo';
    // Before the node backends the demo lived at the top of /data: move it down.
    if (!exists(FS, demo)) {
      var old = FS.readdir(DB_MOUNT).filter(function (n) { return n !== '.' && n !== '..' && !/^(demo|node-.*)$/.test(n); });
      if (old.length) {
        FS.mkdir(demo);
        old.forEach(function (n) { FS.rename(DB_MOUNT + '/' + n, demo + '/' + n); });
      }
    }
    if (wantReset) rmrf(FS, demo);   // "Reset demo data": only the demo's
    // A node connected the first time ran on node-new until its key was known.
    if (choice && choice.key) {
      var fresh = DB_MOUNT + '/node-new', mine = nodeDir(choice);
      if (exists(FS, fresh)) { if (exists(FS, mine)) rmrf(FS, fresh); else FS.rename(fresh, mine); }
    }
    if (!exists(FS, dataDir)) FS.mkdir(dataDir);
  }

  function mountStorage(Mod) {
    var FS = Mod.FS;
    FS.mkdir(DB_MOUNT);
    FS.mount(Mod.IDBFS || FS.filesystems.IDBFS, {}, DB_MOUNT);
    Mod.addRunDependency('wada-idbfs');
    var load = function () {
      FS.syncfs(true, function (err) {
        if (err) {
          console.warn('[web] IndexedDB unavailable, running without saving', err);
          toast('This browser does not allow saving here (private window?). The demo works, but resets on reload.');
        }
        try { prepareDataDirs(FS); } catch (e) { console.warn('[web] data dirs', e); }
        Mod.removeRunDependency('wada-idbfs');
      });
    };
    load();
  }

  setStatus('Loading wadamesh', '', true);
  var script = document.createElement('script');
  script.src = BUNDLE + 'wadamesh.js?v=' + BUILD;
  script.onerror = cannotLoad;
  script.onload = function () {
    var t0 = performance.now();
    if (fill) {   // the viewport has had the time to settle: size the UI from it
      pickSize();
      canvas.width = W; canvas.height = H;
      layout();
    }
    var Mod = {
      locateFile: function (path) { return BUNDLE + path + '?v=' + BUILD; },
      print: function (t) { console.log(t); },
      printErr: function (t) { console.warn(t); },
      preRun: [function () { setStatus('Starting', '', true); mountStorage(Mod); }],
      onRuntimeInitialized: function () {
        stats.bootMs = Math.round(performance.now() - t0);
        // The map's tile downloads need the network (web/compat/wada_http.h wadaNetOnline).
        if (touchDevice) Mod._wadaWebNativeKeyboard(1);   // the phone's keyboard, not the UI's keys
        var online = function () { try { Mod._wadaWebOnline(navigator.onLine === false ? 0 : 1); } catch (e) {} };
        online();
        window.addEventListener('online', online);
        window.addEventListener('offline', online);
      },
      onAbort: function (what) {
        fatal('wadamesh stopped', 'Something went wrong: <code>' + String(what).replace(/</g, '&lt;') + '</code>', true);
      },
      wadaPresent: present,
      wadaSize: [W, H],
      wadaDownload: function (bytes, name) { download(bytes, name, /\.json$/i.test(name) ? 'application/json' : undefined); },
      wadaPickFile: function (accept) { pickFile(accept, 'ui'); },
      wadaGeoStart: geoStart,
      wadaGeoStop: geoStop,
      // which mesh to boot on (web_main.cpp reads it before the UI starts)
      wadaBackend: choice ? { kind: choice.kind, arg: choice.arg, dataDir: dataDir }
                          : { kind: 'demo', dataDir: dataDir },
      wadaLink: function (state, name, via, key) { onLink(state, name, via, key); },
      wadaCompanionState: function (up, detail) { if (!up) linkDetail = detail; linkTrouble(up, detail); },
      wadaCompanionNeedsGesture: function (kind) {
        $('btn-pair').hidden = false;
        $('btn-pair').textContent = kind === 'ble' ? 'Pair again' : 'Reconnect';
        setMode('wait', kind === 'ble' ? 'Bluetooth: tap Pair again' : 'USB: tap Reconnect');
      },
      wadaReboot: function () {
        // esp_restart(): the UI flushed what a device flushes; save, then start over.
        running = false;
        setStatus('Restarting', '', true);
        persist(function () { location.reload(); });
      },
    };
    createWadamesh(Mod).then(function (m) { M = m; }, function (e) {
      fatal('wadamesh could not start', String(e).replace(/</g, '&lt;'), true);
    });
    // The module's own functions are reachable once its runtime is up (main runs then).
    M = Mod;
  };
  document.head.appendChild(script);

  // For tests and the curious: window.wadamesh.stats, .persist(), .idle()
  window.wadamesh = {
    build: BUILD,
    threads: function () { return isolated; },
    stats: stats,
    storage: persistState,
    backend: function () { return choice; },
    dataDir: dataDir,
    persist: function () { return new Promise(function (res) { persist(res); }); },
    flush: function () { return new Promise(function (res) { flushNow(); persist(res); }); },
    running: function () { return running; },
    geo: function () { return { want: geo.want, watching: geo.watch !== null, waitingTap: geo.waitingTap }; },
    _m: function () { return M; },   // the module, for tests
    // Is `needle` (text, compared as UTF-8 bytes) in any saved file under /data? (the
    // persistence tests)
    findInStorage: function (needle) {
      if (!M || !M.FS) return false;
      var FS = M.FS, hits = [], nb = new TextEncoder().encode(needle);
      var has = function (b) {
        outer: for (var i = 0; i + nb.length <= b.length; i++) {
          if (b[i] !== nb[0]) continue;
          for (var j = 1; j < nb.length; j++) if (b[i + j] !== nb[j]) continue outer;
          return true;
        }
        return false;
      };
      (function walk(dir) {
        FS.readdir(dir).forEach(function (n) {
          if (n === '.' || n === '..') return;
          var p = dir + '/' + n, st = FS.stat(p);
          if (FS.isDir(st.mode)) walk(p);
          else if (has(FS.readFile(p))) hits.push(p);
        });
      })(DB_MOUNT);
      return hits;
    },
    // The middle of the first visible LVGL label showing `text` (canvas pixels), or null:
    // lets the phone tests tap by name at any canvas size (web/platform/WebLvglInput.cpp).
    findText: function (text) {
      if (!M || !running) return null;
      var v = M.ccall('wadaWebFindText', 'number', ['string'], [text]);
      return v < 0 ? null : [v >> 16, v & 0xffff];
    },
    // The text of the LVGL field being edited (tests: what the phone keyboard typed).
    editText: function () { return M && running ? M.ccall('wadaWebEditText', 'string', [], []) : ''; },
    size: function () { return [W, H]; },
  };
})();
