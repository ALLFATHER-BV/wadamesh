// SPDX-License-Identifier: GPL-3.0-or-later
// The browser front end of the web board (Emscripten). board.cpp owns the board
// globals and the boot/loop order; this file is the page side of it:
//
//   - the loop: emscripten_set_main_loop on the browser main thread, one board step
//     per animation frame (LVGL and the UI never leave the main thread; the UI's
//     background tasks are pthreads via compat/freertos)
//   - the panel: the changed rectangle of the RGB565 framebuffer, as RGBA, handed to
//     the page's Module.wadaPresent (web/shell/wadamesh.js puts it on the <canvas>)
//   - input: the page calls wadaWebPointer / wadaWebKey (canvas pixels, CardKB codes)
//   - storage: the page persists the IDBFS mount when wadaWebFsGeneration() moves
//   - restart: esp_restart() asks the page to persist and reload
//
// The page (web/shell/) mounts IDBFS at /data and syncs it from IndexedDB BEFORE
// main() runs, so the UI boots on the visitor's saved prefs, contacts and history.
#include <Arduino.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "WebDisplay.h"
#include "WebInput.h"
#include "WebPlatform.h"
#include "BackendSelect.h"
#include "wada_host.h"
#include "WebMesh.h"
#include <stdint.h>
#include <stdlib.h>
#include <string>

extern WebDisplay display;            // board.cpp
extern void (*g_wada_restart_hook)();  // compat/src/arduino_host.cpp

// The view is made here, inside the module, where Emscripten keeps HEAPU8 current
// across memory growth (pthreads + ALLOW_MEMORY_GROWTH).
EM_JS(void, wada_js_present, (const uint8_t* rgba, int x, int y, int w, int h), {
  if (Module["wadaPresent"]) Module["wadaPresent"](HEAPU8.subarray(rgba, rgba + w * h * 4), x, y, w, h);
});
// The page's backend choice (web/shell/app.js Module.wadaBackend): kind, arg, data dir,
// read-only, one per line, in a malloc'd string main() frees.
EM_JS(char*, wada_js_backend_config, (), {
  var c = Module["wadaBackend"] || {};
  var s = [c.kind || "demo", c.arg || "", c.dataDir || "", c.readOnly ? "1" : "0"].join("\n");
  var b = new TextEncoder().encode(s);
  var p = _malloc(b.length + 1);
  HEAPU8.set(b, p);
  HEAPU8[p + b.length] = 0;
  return p;
});
// The companion link for the page's status bar (BackendSelect.h g_wada_link_hook).
EM_JS(void, wada_js_link, (int state, const char* name, const char* via, const char* key), {
  if (!Module["wadaLink"]) return;
  var str = function (p) { var e = p; while (HEAPU8[e]) e++; return new TextDecoder().decode(HEAPU8.slice(p, e)); };
  Module["wadaLink"](state, str(name), str(via), str(key));
});
EM_JS(void, wada_js_reboot, (), {
  if (Module["wadaReboot"]) Module["wadaReboot"](); else location.reload();
});

namespace {
// ---- pointer: queued, one press/release edge per board step -------------------------
// The touch driver (WebTouch.cpp) samples the pointer once per UI loop, as the CHSC6x
// driver samples the panel. A quick click can press and release between two animation
// frames, so edges are queued and handed over one per step; moves coalesce.
struct PtrEv { bool down; int16_t x, y; };
PtrEv s_q[64];
unsigned s_qh = 0, s_qt = 0;
bool s_down = false;
uint32_t s_down_ms = 0;
// A press must outlive a couple of LVGL input reads (LV_INDEV_DEF_READ_PERIOD) or the UI
// never sees it: a trackpad tap-to-click sends press and release in the same frame, and
// one 16 ms step was shorter than the 30 ms read period, so taps landed at random.
constexpr uint32_t kMinPressMs = 70;

void drainPointer() {
  while (s_qt != s_qh) {
    const PtrEv e = s_q[s_qt];
    if (e.down != s_down) {          // an edge: deliver it and stop until the next step
      if (!e.down && (uint32_t)(millis() - s_down_ms) < kMinPressMs) return;   // hold the press
      s_down = e.down;
      if (e.down) s_down_ms = millis();
      webPointer(e.down, e.x, e.y);
      s_qt = (s_qt + 1) & 63u;
      return;
    }
    webPointer(e.down, e.x, e.y);    // a move while pressed (or a stray hover)
    s_qt = (s_qt + 1) & 63u;
  }
}

// ---- present: RGB565 -> RGBA for the changed rectangle ------------------------------
uint8_t* s_rgba = nullptr;

void present() {
  int x0, y0, x1, y1;
  if (!display.takeDirtyRect(x0, y0, x1, y1)) return;
  const int w = x1 - x0, h = y1 - y0;
  const uint16_t* fb = display.framebuffer();
  uint8_t* o = s_rgba;
  for (int r = y0; r < y1; ++r) {
    const uint16_t* p = fb + (size_t)r * WADA_WEB_W + x0;
    for (int c = 0; c < w; ++c) {
      const uint16_t v = p[c];
      const uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
      o[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
      o[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
      o[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
      o[3] = 255;
      o += 4;
    }
  }
  wada_js_present(s_rgba, x0, y0, w, h);
}

void step() {
#if defined(WADA_NO_THREADS)
  wadaCoopSetInLoop(true);
#endif
  drainPointer();
  wadaBoardStep();
#if defined(WADA_NO_THREADS)
  wadaCoopRunTasks();   // the background tasks, on this thread (compat/src/freertos_coop.cpp)
  wadaCoopSetInLoop(false);
#endif
  present();
}

// esp_restart() (Reboot, factory reset, a language change, ...): the page persists the
// IDBFS mount and reloads. The caller already flushed what the device flushes.
// A restart right after changes for a node (a backup import: its channels and contacts)
// lets the queued commands reach the node first: only the mesh runs, up to 20 s.
uint32_t s_drain_until = 0;
void drainThenReboot() {
  the_mesh.loop();
  if (the_mesh.backendBusy() && (int32_t)(millis() - s_drain_until) < 0) return;
  emscripten_cancel_main_loop();
  wada_js_reboot();
}
void webRestart() {
#if defined(WADA_NO_THREADS)
  if (wadaCoopInTask()) {   // a background task asked: the page reloads; park the task
    wada_js_reboot();
    for (;;) delay(1000);
  }
#endif
  if (emscripten_is_main_browser_thread()) {
    emscripten_cancel_main_loop();
    if (the_mesh.backendBusy()) {
      s_drain_until = millis() + 20000;
      emscripten_set_main_loop(drainThenReboot, 0, false);
    } else {
      wada_js_reboot();
    }
    emscripten_unwind_to_js_event_loop();   // no return: the page reloads after the sync
  }
  MAIN_THREAD_ASYNC_EM_ASM({ if (Module["wadaReboot"]) Module["wadaReboot"](); else location.reload(); });
  for (;;) delay(1000);   // a background task asked: park it until the reload
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void wadaWebPointer(int down, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= WADA_WEB_W) x = WADA_WEB_W - 1;
  if (y >= WADA_WEB_H) y = WADA_WEB_H - 1;
  const unsigned next = (s_qh + 1) & 63u;
  if (next == s_qt) {   // full: keep the edges, drop the oldest move
    if (s_q[s_qt].down == s_down) s_qt = (s_qt + 1) & 63u; else return;
  }
  s_q[s_qh] = PtrEv{down != 0, (int16_t)x, (int16_t)y};
  s_qh = next;
}
EMSCRIPTEN_KEEPALIVE void wadaWebKey(int code) { webKeyPush(code); }
// Text from the page (the phone's keyboard, a non-ASCII key): UTF-8.
EMSCRIPTEN_KEEPALIVE void wadaWebText(const char* utf8) { webTextPush(utf8); }
// The phone's own keyboard is the UI's keyboard from now on (the on-screen keys stay down).
EMSCRIPTEN_KEEPALIVE void wadaWebNativeKeyboard(int on) { webKeyboardPresent(on != 0); }
// Is there a text field under (x, y)? Asked in the tap's own event handler, so the page can
// focus its hidden field (and the phone open its keyboard) inside the user's gesture.
EMSCRIPTEN_KEEPALIVE int wadaWebTextareaAt(int x, int y) { return webTextareaAt(x, y) ? 1 : 0; }
// Is a text field being edited (the UI's keyboard is bound to one)?
EMSCRIPTEN_KEEPALIVE int wadaWebEditing() { return webEditing() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int wadaWebOskShown() { return webOskShown() ? 1 : 0; }
// Tests: where a label with this text is (web/shell/app.js wadamesh.findText).
EMSCRIPTEN_KEEPALIVE int wadaWebFindText(const char* text) { return webFindText(text); }
EMSCRIPTEN_KEEPALIVE const char* wadaWebEditText() { return webEditText(); }
// The mouse wheel / a two-finger trackpad scroll over (x, y), in canvas pixels.
EMSCRIPTEN_KEEPALIVE void wadaWebWheel(int x, int y, int dy) { webWheel(x, y, dy); }
EMSCRIPTEN_KEEPALIVE unsigned wadaWebFsGeneration() { return wadaHostFsGeneration(); }
// pagehide / visibilitychange: write the lazily-flushed chat history now, then the
// page persists the mount.
EMSCRIPTEN_KEEPALIVE void wadaWebFlush() { wadaBoardFlush(); }
}

// The canvas size the page picked from the window (web/shell/app.js Module.wadaSize).
EM_JS(int, wada_js_size, (int which), {
  var s = Module["wadaSize"] || [800, 480];
  return s[which] | 0;
});

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  wadaWebSetSize(wada_js_size(0), wada_js_size(1));
  s_rgba = (uint8_t*)malloc((size_t)WADA_WEB_W * WADA_WEB_H * 4);
  g_wada_restart_hook = webRestart;
  // Which mesh: the demo, or the node the page's Connect chooser picked. Each backend
  // has its own data dir under /data, so demo and node data never mix.
  WadaBackendChoice choice;
  {
    char* cfg = wada_js_backend_config();
    std::string f[4];
    int i = 0;
    for (const char* p = cfg; *p && i < 4; ++p) { if (*p == '\n') ++i; else f[i] += *p; }
    free(cfg);
    if (!f[0].empty()) choice.kind = f[0];
    choice.arg = f[1];
    choice.read_only = f[3] == "1";
    if (!f[2].empty()) setenv("WADA_DATA_DIR", f[2].c_str(), 1);
  }
  g_wada_link_hook = [](int state, const char* name, const char* via, const char* key) { wada_js_link(state, name, via, key); };
  wadaBoardBegin(choice);
  emscripten_set_main_loop(step, 0, false);   // requestAnimationFrame pace
  return 0;
}
