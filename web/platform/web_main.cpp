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
#include "wada_host.h"
#include <stdint.h>
#include <stdlib.h>

extern WebDisplay display;            // board.cpp
extern void (*g_wada_restart_hook)();  // compat/src/arduino_host.cpp

// The view is made here, inside the module, where Emscripten keeps HEAPU8 current
// across memory growth (pthreads + ALLOW_MEMORY_GROWTH).
EM_JS(void, wada_js_present, (const uint8_t* rgba, int x, int y, int w, int h), {
  if (Module["wadaPresent"]) Module["wadaPresent"](HEAPU8.subarray(rgba, rgba + w * h * 4), x, y, w, h);
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

void drainPointer() {
  while (s_qt != s_qh) {
    const PtrEv e = s_q[s_qt];
    if (e.down != s_down) {          // an edge: deliver it and stop until the next step
      s_down = e.down;
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
  drainPointer();
  wadaBoardStep();
  present();
}

// esp_restart() (Reboot, factory reset, a language change, ...): the page persists the
// IDBFS mount and reloads. The caller already flushed what the device flushes.
void webRestart() {
  if (emscripten_is_main_browser_thread()) {
    emscripten_cancel_main_loop();
    wada_js_reboot();
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
EMSCRIPTEN_KEEPALIVE void wadaWebKey(int cardkb_code) { webKeyPush((uint8_t)cardkb_code); }
EMSCRIPTEN_KEEPALIVE unsigned wadaWebFsGeneration() { return wadaHostFsGeneration(); }
// pagehide / visibilitychange: write the lazily-flushed chat history now, then the
// page persists the mount.
EMSCRIPTEN_KEEPALIVE void wadaWebFlush() { wadaBoardFlush(); }
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  s_rgba = (uint8_t*)malloc((size_t)WADA_WEB_W * WADA_WEB_H * 4);
  g_wada_restart_hook = webRestart;
  wadaBoardBegin();
  emscripten_set_main_loop(step, 0, false);   // requestAnimationFrame pace
  return 0;
}
