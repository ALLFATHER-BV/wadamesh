// wadamesh in the browser: the page side of web/platform/web_main.cpp.
//
//  - checks the page is cross-origin isolated (the build uses wasm threads, which need
//    SharedArrayBuffer, which needs COOP: same-origin + COEP: require-corp)
//  - mounts IndexedDB (IDBFS) at /data and loads it BEFORE the UI boots
//  - persists /data after writes settle, and on tab hide / page close
//  - scales the 800x480 canvas to the window (aspect kept; whole device pixels = crisp
//    nearest when that costs little, smooth otherwise)
//  - pointer = finger, keyboard = the UI's external keyboard (CardKB codes)
(function () {
  'use strict';
  var W = 800, H = 480;
  var BUILD = '__WADA_BUILD__';
  var DB_MOUNT = '/data';   // also the IndexedDB database name IDBFS uses

  var $ = function (id) { return document.getElementById(id); };
  var canvas = $('screen'), stage = $('stage'), overlay = $('overlay');
  var ctx = canvas.getContext('2d', { alpha: false });
  var M = null;            // the Emscripten module once created
  var running = false;     // the UI has presented its first frame
  var resetting = false;   // a "Reset demo data" reload is under way: write nothing more

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
  function toast(text) {
    var t = $('toast');
    t.textContent = text; t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, 4200);
  }

  // ---- scaling -------------------------------------------------------------------------
  function layout() {
    var full = document.fullscreenElement || document.webkitFullscreenElement;
    var r = stage.getBoundingClientRect();
    var cs = getComputedStyle(stage);
    var aw = r.width - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    var ah = r.height - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    if (!full) {
      ah = Math.max(ah, 120);
      // a phone in portrait: scale to the width, the page scrolls if it must
      if (window.innerWidth < 600) ah = Math.max(ah, aw * H / W);
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
      overlay.hidden = true;
      canvas.focus({ preventScroll: true });
    }
  }
  (function countFrames() { stats.frames++; requestAnimationFrame(countFrames); })();

  // ---- input -----------------------------------------------------------------------------
  var activePointer = null;
  function toCanvas(e) {
    var r = canvas.getBoundingClientRect();
    return [Math.floor((e.clientX - r.left) * W / r.width), Math.floor((e.clientY - r.top) * H / r.height)];
  }
  function sendPointer(down, e) {
    if (!M || !running) return;
    var p = toCanvas(e);
    M._wadaWebPointer(down ? 1 : 0, p[0], p[1]);
  }
  canvas.addEventListener('pointerdown', function (e) {
    if (activePointer !== null) return;              // one finger: the panel is single-touch
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    activePointer = e.pointerId;
    try { canvas.setPointerCapture(e.pointerId); } catch (_) {}
    canvas.focus({ preventScroll: true });
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
  }
  canvas.addEventListener('pointerup', pointerEnd);
  canvas.addEventListener('pointercancel', pointerEnd);
  canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });

  // The computer keyboard is the UI's external keyboard (HAS_CARDKB): ASCII, Enter,
  // Backspace, Esc (Back), Tab, arrows. Same codes as host_main.cpp's keyDown().
  var KEYS = { Enter: 0x0D, Backspace: 0x08, Escape: 0x1B, Tab: 0x09,
               ArrowLeft: 0xB4, ArrowUp: 0xB5, ArrowDown: 0xB6, ArrowRight: 0xB7 };
  document.addEventListener('keydown', function (e) {
    if (!M || !running) return;
    if (e.ctrlKey || e.metaKey || e.altKey) return;        // browser shortcuts stay the browser's
    var t = e.target;
    if (t && t !== canvas && t !== document.body && /^(BUTTON|A|INPUT|TEXTAREA|SELECT)$/.test(t.tagName)) {
      if (e.key === 'Enter' || e.key === ' ' || e.key === 'Tab') return;   // the bar's own buttons
    }
    var code = KEYS[e.key];
    if (code === undefined && e.key.length === 1) {
      var c = e.key.charCodeAt(0);
      if (c >= 0x20 && c < 0x7F) code = c;
    }
    if (code === undefined) return;
    e.preventDefault();
    M._wadaWebKey(code);
  });

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
  $('btn-connect').addEventListener('click', function () {
    toast('Coming soon: connect this UI to your own MeshCore node over USB, Bluetooth or Wi-Fi. For now it runs on a demo mesh.');
  });
  $('btn-reset').addEventListener('click', function () {
    if (!confirm('Reset the demo? Your messages, contacts and settings in this browser are deleted and the demo starts fresh.')) return;
    resetting = true;
    var u = new URL(location.href);
    u.searchParams.set('reset', '1');
    location.replace(u.toString());
  });
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
  if (!window.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
    fatal('This page is not cross-origin isolated',
      'wadamesh runs its background tasks on WebAssembly threads, which browsers only allow on a page served with ' +
      '<code>Cross-Origin-Opener-Policy: same-origin</code> and <code>Cross-Origin-Embedder-Policy: require-corp</code>. ' +
      'Run it with <code>web/build.sh serve</code>, or add the headers in <code>web/deploy/nginx-snippet.conf</code> to your server.');
    return;
  }

  var params = new URLSearchParams(location.search);
  var wantReset = params.get('reset') === '1';
  if (wantReset) {
    params.delete('reset');
    var q = params.toString();
    history.replaceState(null, '', location.pathname + (q ? '?' + q : '') + location.hash);
  }

  function deleteDb(cb) {
    var done = false, fin = function () { if (!done) { done = true; cb(); } };
    try {
      var rq = indexedDB.deleteDatabase(DB_MOUNT);
      rq.onsuccess = rq.onerror = fin;
      rq.onblocked = function () { setTimeout(fin, 1500); };
    } catch (e) { fin(); }
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
        Mod.removeRunDependency('wada-idbfs');
      });
    };
    if (wantReset) deleteDb(load); else load();
  }

  setStatus('Loading wadamesh', '', true);
  var script = document.createElement('script');
  script.src = 'wadamesh.js?v=' + BUILD;
  script.onerror = function () { fatal('Could not load wadamesh', 'The file <code>wadamesh.js</code> did not load.', true); };
  script.onload = function () {
    var t0 = performance.now();
    var Mod = {
      locateFile: function (path) { return path + '?v=' + BUILD; },
      print: function (t) { console.log(t); },
      printErr: function (t) { console.warn(t); },
      preRun: [function () { setStatus('Starting', '', true); mountStorage(Mod); }],
      onRuntimeInitialized: function () { stats.bootMs = Math.round(performance.now() - t0); },
      onAbort: function (what) {
        fatal('wadamesh stopped', 'Something went wrong: <code>' + String(what).replace(/</g, '&lt;') + '</code>', true);
      },
      wadaPresent: present,
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
    stats: stats,
    storage: persistState,
    persist: function () { return new Promise(function (res) { persist(res); }); },
    flush: function () { return new Promise(function (res) { flushNow(); persist(res); }); },
    running: function () { return running; },
    _m: function () { return M; },   // the module, for tests
    // Is `needle` in any saved file under /data? (the persistence test)
    findInStorage: function (needle) {
      if (!M || !M.FS) return false;
      var FS = M.FS, hits = [];
      (function walk(dir) {
        FS.readdir(dir).forEach(function (n) {
          if (n === '.' || n === '..') return;
          var p = dir + '/' + n, st = FS.stat(p);
          if (FS.isDir(st.mode)) walk(p);
          else if (new TextDecoder('latin1').decode(FS.readFile(p)).indexOf(needle) >= 0) hits.push(p);
        });
      })(DB_MOUNT);
      return hits;
    },
  };
})();
