// SPDX-License-Identifier: GPL-3.0-or-later
// wadamesh web build: the browser side of the companion transports.
//
// Linked into the Emscripten build with --pre-js (web/CMakeLists.txt), so it shares the
// module's scope (HEAPU8, the wada_* C entry points). It only moves bytes; the framing
// is in C++ (web/mesh/BrowserTransport.cpp). One link at a time.
//
//   C++ calls  WadaCompanion.open(kind, arg)   at boot (kind: "web-serial" | "ble" | "ws")
//              WadaCompanion.send(Uint8Array)   one framed write
//              WadaCompanion.close()
//   The page   WadaCompanion.connect(kind?, arg?)  from a click: Web Serial and Web
//              Bluetooth need a user gesture to pick a device the first time.
//              Module.wadaCompanionState(up, detail)   optional: link up / down, why
//              Module.wadaCompanionNeedsGesture(kind)  optional: no device this page
//                  may open without a click (show a button that calls connect())
//
// arg: "ws" the URL; "web-serial" "VID:PID" of the port to prefer (from the page's
// requestPort); "ble" the Web Bluetooth device id to prefer. A lost link is retried
// every few seconds (a node that resets comes back on its own) until close().
//
// Web Serial: the port is opened with DTR and RTS released (an ESP32-S3 can reset or
// enter its bootloader on a DTR/RTS sequence), 115200 baud (ignored by USB CDC).
// Web Bluetooth: MeshCore's Nordic UART service; the node may ask for its PIN (the OS
// pairing dialog). WebSocket: the node's WS companion server, ws://NODE-IP:8765/ (Wi-Fi
// on, plain ws:// only, so the page itself must be served over http:// or from the node).
var WadaCompanion = (function () {
  var NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
  var NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // we write
  var NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // the node notifies

  var kind = null, arg = "";
  var link = null;         // {write(bytes) -> Promise, close()}
  var writeChain = Promise.resolve();
  var wanted = false;      // keep the link up (retry when it drops) until close()
  var retryTimer = 0, starting = false;
  var api = { onstate: null };
  var RETRY_MS = 3000;

  function toC(bytes) {
    var p = _wada_companion_js_alloc(bytes.length);
    HEAPU8.set(bytes, p);
    return p;
  }
  function rx(bytes) {
    if (bytes && bytes.length) _wada_companion_js_rx(toC(bytes), bytes.length);
  }
  function state(up, detail) {
    var d = new TextEncoder().encode((detail || "") + "\0");
    _wada_companion_js_state(up ? 1 : 0, toC(d));
    if (typeof api.onstate === "function") { try { api.onstate(up, detail || ""); } catch (e) {} }
    if (typeof Module !== "undefined" && typeof Module["wadaCompanionState"] === "function") {
      try { Module["wadaCompanionState"](up, detail || ""); } catch (e) {}
    }
  }
  function lost(detail) {
    if (!link) return;
    link = null;
    state(false, detail);
    retry();
  }
  function retry() {
    if (!wanted || retryTimer) return;
    retryTimer = setTimeout(function () {
      retryTimer = 0;
      if (wanted && !link) api.open(kind, arg);
    }, RETRY_MS);
  }
  function needGesture() {
    if (typeof Module !== "undefined" && typeof Module["wadaCompanionNeedsGesture"] === "function") {
      try { Module["wadaCompanionNeedsGesture"](kind); } catch (e) {}
    }
  }

  // ---- Web Serial ----
  async function openSerial(port) {
    await port.open({ baudRate: 115200 });
    try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch (e) {}
    var writer = port.writable.getWriter();
    var reader = port.readable.getReader();
    var closed = false;
    link = {
      write: function (b) { return writer.write(b); },
      close: async function () {
        closed = true;
        try { await reader.cancel(); } catch (e) {}
        try { reader.releaseLock(); writer.releaseLock(); await port.close(); } catch (e) {}
      },
    };
    state(true, "USB");
    (async function () {
      try {
        for (;;) {
          var r = await reader.read();
          if (r.done) break;
          rx(r.value);
        }
      } catch (e) {}
      if (!closed) lost("the USB device went away");
    })();
  }
  async function serialAuto() {
    if (!navigator.serial) return false;
    var ports = await navigator.serial.getPorts();
    if (!ports.length) return false;
    var want = (arg || "").split(":"), pick = ports[0];
    ports.forEach(function (p) {
      var i = p.getInfo ? p.getInfo() : {};
      if (want.length === 2 && String(i.usbVendorId) === want[0] && String(i.usbProductId) === want[1]) pick = p;
    });
    await openSerial(pick);
    return true;
  }
  async function serialPick() {
    if (!navigator.serial) throw new Error("this browser has no Web Serial (use Chrome or Edge)");
    await openSerial(await navigator.serial.requestPort());
  }

  // ---- Web Bluetooth ----
  async function openBle(device) {
    var server = await device.gatt.connect();
    var svc = await server.getPrimaryService(NUS_SERVICE);
    var rxc = await svc.getCharacteristic(NUS_RX);
    var txc = await svc.getCharacteristic(NUS_TX);
    txc.addEventListener("characteristicvaluechanged", function (ev) {
      var v = ev.target.value;
      rx(new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)));
    });
    await txc.startNotifications();
    device.addEventListener("gattserverdisconnected", function () { lost("Bluetooth disconnected"); });
    link = {
      write: function (b) {
        return rxc.writeValueWithResponse ? rxc.writeValueWithResponse(b) : rxc.writeValue(b);
      },
      close: async function () { try { device.gatt.disconnect(); } catch (e) {} },
    };
    state(true, device.name || "Bluetooth");
  }
  async function bleAuto() {
    if (!navigator.bluetooth || !navigator.bluetooth.getDevices) return false;
    var devs = await navigator.bluetooth.getDevices();
    if (arg) devs = devs.filter(function (d) { return d.id === arg; });
    if (!devs.length) return false;
    await openBle(devs[0]);
    return true;
  }
  async function blePick() {
    if (!navigator.bluetooth) throw new Error("this browser has no Web Bluetooth (use Chrome or Edge)");
    var dev = await navigator.bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }, { namePrefix: "MeshCore" }],
      optionalServices: [NUS_SERVICE],
    });
    await openBle(dev);
  }

  // ---- WebSocket ----
  function openWs(url) {
    return new Promise(function (resolve, reject) {
      var ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";
      var opened = false;
      ws.onopen = function () {
        opened = true;
        link = {
          write: function (b) { ws.send(b); return Promise.resolve(); },
          close: async function () { try { ws.close(); } catch (e) {} },
        };
        state(true, url);
        resolve();
      };
      ws.onmessage = function (ev) {
        if (ev.data instanceof ArrayBuffer) rx(new Uint8Array(ev.data));
      };
      ws.onerror = function () { if (!opened) reject(new Error("could not reach " + url)); };
      ws.onclose = function () { if (opened) lost("the WebSocket closed"); };
    });
  }

  async function start(k, a, gesture) {
    if (link) { var l = link; link = null; await l.close(); }
    kind = k; arg = a || "";
    wanted = true;
    if (kind === "ws") return openWs(arg);
    var auto = kind === "web-serial" ? serialAuto : kind === "ble" ? bleAuto : null;
    if (!auto) throw new Error("unknown companion transport " + kind);
    if (await auto()) return;
    if (!gesture) { needGesture(); return; }
    return kind === "web-serial" ? serialPick() : blePick();
  }

  api.open = function (k, a) {
    if (starting) return;
    starting = true;
    start(k, a, false).then(function () { starting = false; }, function (e) {
      starting = false;
      state(false, String(e && e.message || e));
      retry();
    });
  };
  // From a click: may show the browser's device picker.
  api.connect = function (k, a) {
    return start(k || kind, a !== undefined ? a : arg, true).catch(function (e) {
      state(false, String(e && e.message || e));
      throw e;
    });
  };
  api.send = function (bytes) {
    if (!link) return false;
    var l = link;
    var copy = new Uint8Array(bytes);   // the caller's view may be reused
    writeChain = writeChain.then(function () { return l.write(copy); }).catch(function (e) {
      lost("write failed: " + (e && e.message || e));
    });
    return true;
  };
  api.close = async function () {
    wanted = false;
    clearTimeout(retryTimer); retryTimer = 0;
    var l = link;
    link = null;
    if (l) await l.close();
  };
  api.isConnected = function () { return !!link; };
  // A granted USB device plugged (back) in: reconnect without a click.
  if (typeof navigator !== "undefined" && navigator.serial && navigator.serial.addEventListener) {
    navigator.serial.addEventListener("connect", function () {
      if (wanted && !link && kind === "web-serial") api.open(kind, arg);
    });
  }
  return api;
})();
if (typeof Module !== "undefined") Module.WadaCompanion = WadaCompanion;
