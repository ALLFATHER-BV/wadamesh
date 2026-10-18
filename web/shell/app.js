// wadamesh in the browser: the page side of web/platform/web_main.cpp.
//
//  - checks the page is cross-origin isolated (the build uses wasm threads, which need
//    SharedArrayBuffer, which needs COOP: same-origin + COEP: require-corp)
//  - mounts IndexedDB (IDBFS) at /data and loads it BEFORE the UI boots
//  - persists /data after writes settle, and on tab hide / page close
//  - scales the 800x480 canvas to the window (aspect kept; whole device pixels = crisp
//    nearest when that costs little, smooth otherwise)
//  - pointer = finger, keyboard = the UI's external keyboard (CardKB codes)
//  - Connect: the demo, or the visitor's own MeshCore node over USB (Web Serial),
//    Bluetooth (Web Bluetooth) or Wi-Fi (WebSocket, the node's port 8765). The choice
//    is kept in localStorage; each backend has its own data dir under /data
//    (/data/demo, /data/node-<key>), and switching reboots the UI into it.
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
      // a phone in portrait: scale to the width, the page scrolls if it must. Only in
      // portrait: a small LANDSCAPE window (a docked or split browser) must fit its height,
      // or the screen grows past the stage and over the header.
      if (window.innerWidth < 600 && window.innerHeight > window.innerWidth) ah = Math.max(ah, aw * H / W);
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
  canvas.addEventListener('lostpointercapture', pointerEnd);
  // Window loses focus mid-press (alt-tab, a dialog): release, or the UI keeps a
  // phantom finger down.
  window.addEventListener('blur', function () {
    if (activePointer === null) return;
    activePointer = null;
    if (M && running) M._wadaWebPointer(0, lastPt[0], lastPt[1]);
  });
  canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });

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
  function setMode(cls, text, title) {
    var b = $('mode');
    b.className = 'badge' + (cls ? ' ' + cls : '');
    b.textContent = text;
    b.title = title || text;
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
    var notes = [];
    if (!hasSerial && !hasBle) notes.push('USB and Bluetooth need Chrome or Edge on a computer; this browser can connect over Wi-Fi.');
    else if (!hasBle) notes.push('Bluetooth needs Chrome or Edge.');
    else if (!hasSerial) notes.push('USB needs Chrome or Edge on a computer.');
    if (location.protocol === 'https:' && !/^(localhost|127\.0\.0\.1|\[::1\])$/.test(location.hostname))
      notes.push('Over Wi-Fi the browser has to reach ws://, which it blocks on a secure (https) page; use the page from your own computer or network for Wi-Fi.');
    $('connect-note').hidden = !notes.length;
    $('connect-note').textContent = notes.join(' ');
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
      var url = /^wss?:\/\//i.test(h) ? h : 'ws://' + (/:\d+$/.test(h) ? h : h + ':8765') + '/';
      connectTo('ws', url);
    };
    dlg.querySelector('[data-kind="ws"]').addEventListener('click', goWs);
    host.addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); goWs(); } });
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
    if (everUp || !choice) return;
    if (++linkFails < 2) return;            // one miss can be a slow start
    setMode('bad', "Can't reach your node", detail || undefined);
    if (!box) {
      box = document.createElement('div');
      box.id = 'link-help';
      box.className = 'link-help';
      var bar = document.querySelector('.bar');
      bar.parentNode.insertBefore(box, bar.nextSibling);
    }
    var how = choice.kind === 'ws'
      ? 'Check that the node has Wi-Fi on and is on the same network as this computer (a guest or IoT network often cannot reach the rest), and that you typed its address right: ' + choice.arg.replace(/^wss?:\/\//, '').replace(/\/$/, '') + '.'
      : choice.kind === 'web-serial'
        ? 'Check the cable (a charge-only cable carries no data) and that no other app has the port open.'
        : 'Check that the node is on, in range, and has Bluetooth on.';
    box.innerHTML = '';
    var p = document.createElement('p');
    p.textContent = 'No answer from your node yet. ' + how + ' Still trying every few seconds.';
    var back = document.createElement('button');
    back.type = 'button'; back.className = 'btn'; back.textContent = 'Back to the demo';
    back.addEventListener('click', function () { switchTo(null); });
    box.appendChild(p); box.appendChild(back);
    box.hidden = false;
    layout();   // the box takes height from the stage: refit the screen below it
  }
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
    stats: stats,
    storage: persistState,
    backend: function () { return choice; },
    dataDir: dataDir,
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
