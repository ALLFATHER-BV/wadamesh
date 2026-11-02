// SPDX-License-Identifier: GPL-3.0-or-later
// wadamesh-host: the real touch UI in an SDL2 window (phase 1 of WEB_BUILD.md).
//
// The native front end of the web board: board.cpp owns the board globals and the
// boot/loop order (shared with web_main.cpp, the Emscripten front end); this file is
// the SDL window, the event pump and the scripted tour.
//
//   wadamesh-host [--headless] [--script FILE] [--shots DIR] [--scale N] [--size WxH]
//                 [--serial PATH | --tcp HOST:PORT] [--read-only]
//     --headless   no window (CI / release gate); the framebuffer is still rendered
//     --script     scripted input: wait/tap/drag/key/type/shot/quit, one per line
//     --shots DIR  where `shot` and F12 write PNGs (default: current directory)
//     --scale N    initial window scale (default 1; the window is resizable)
//     --serial PATH     drive a real MeshCore companion node over USB instead of the demo
//     --tcp HOST[:PORT] the same over the node's TCP companion server (default port 5000)
//     --read-only       never change the node's settings, contacts, channels or clock
//   $WADA_DATA_DIR  the virtual flash (SPIFFS, LittleFS, NVS), default ./wadamesh-data
#include <Arduino.h>
#include "WebDisplay.h"
#include "WebInput.h"
#include "WebPlatform.h"
#include "BackendSelect.h"
#include "wada_host.h"
#if defined(WADA_SIM_PAGER)
void simPagerHoldKey(uint8_t c, uint32_t ms);   // web/sim_pager/SimPager.cpp
#endif
#include "miniz.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>
#include <sstream>

extern WebDisplay display;   // board.cpp

// ---- presentation ------------------------------------------------------------------
namespace {
SDL_Window* s_win = nullptr;
SDL_Renderer* s_ren = nullptr;
SDL_Texture* s_tex = nullptr;
bool s_quit = false;
std::string s_shot_dir = ".";

bool savePng(const std::string& path) {
  const uint16_t* fb = display.framebuffer();
  std::vector<uint8_t> rgb((size_t)WADA_WEB_W * WADA_WEB_H * 3);
  for (size_t i = 0; i < (size_t)WADA_WEB_W * WADA_WEB_H; ++i) {
    const uint16_t p = fb[i];
    const uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
  }
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), WADA_WEB_W, WADA_WEB_H, 3, &len);
  if (!png) return false;
  FILE* f = fopen(path.c_str(), "wb");
  bool ok = f && fwrite(png, 1, len, f) == len;
  if (f) fclose(f);
  mz_free(png);
  fprintf(stderr, "[web] screenshot %s %s\n", path.c_str(), ok ? "written" : "FAILED");
  return ok;
}

void present() {
  if (!s_ren || !display.takeDirty()) return;
  SDL_UpdateTexture(s_tex, nullptr, display.framebuffer(), WADA_WEB_W * 2);
  SDL_RenderClear(s_ren);
  SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr);
  SDL_RenderPresent(s_ren);
}

// SDL key -> CardKB code (the external-keyboard path the UI already has).
void keyDown(const SDL_Keysym& k) {
  switch (k.sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: webKeyPush(0x0D); break;
    case SDLK_BACKSPACE: webKeyPush(0x08); break;
    case SDLK_ESCAPE: webKeyPush(0x1B); break;
    case SDLK_TAB: webKeyPush(0x09); break;
    case SDLK_LEFT: webKeyPush(0xB4); break;
    case SDLK_UP: webKeyPush(0xB5); break;
    case SDLK_DOWN: webKeyPush(0xB6); break;
    case SDLK_RIGHT: webKeyPush(0xB7); break;
    case SDLK_F12: {
      char name[64]; snprintf(name, sizeof name, "/wadamesh-%lu.png", (unsigned long)millis());
      savePng(s_shot_dir + name);
      break;
    }
    default: break;
  }
}

int s_mouse_x = 0, s_mouse_y = 0;
void pumpEvents() {
  if (!s_win) return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: s_quit = true; break;
      case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) webPointer(e.type == SDL_MOUSEBUTTONDOWN, e.button.x, e.button.y);
        break;
      case SDL_MOUSEMOTION:
        s_mouse_x = e.motion.x; s_mouse_y = e.motion.y;   // where a wheel turn applies
        if (e.motion.state & SDL_BUTTON_LMASK) webPointer(true, e.motion.x, e.motion.y);
        break;
      case SDL_MOUSEWHEEL: {
        // precise (trackpad) deltas when SDL has them; a wheel notch = 3 lines of 16 px
        const float dy = e.wheel.preciseY != 0 ? e.wheel.preciseY : (float)e.wheel.y;
        const int sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        webWheel(s_mouse_x, s_mouse_y, (int)(-dy * sign * 48.0f));
        break;
      }
      case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP:
        webPointer(e.type != SDL_FINGERUP, (int)(e.tfinger.x * WADA_WEB_W), (int)(e.tfinger.y * WADA_WEB_H));
        break;
      case SDL_KEYDOWN: keyDown(e.key.keysym); break;
      case SDL_TEXTINPUT:
        webTextPush(e.text.text);   // any script: ASCII as CardKB codes, the rest as text
        break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
          SDL_UpdateTexture(s_tex, nullptr, display.framebuffer(), WADA_WEB_W * 2);
          SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr); SDL_RenderPresent(s_ren);
        }
        break;
      default: break;
    }
  }
}

// ---- scripted input (screenshots, smoke tests, the phase-4 release gate) ------------
struct Script {
  std::vector<std::string> lines;
  size_t pc = 0;
  uint32_t wait_until = 0;
  // an in-flight press: release at release_at
  bool pressing = false; uint32_t release_at = 0;
  int drag_x0 = 0, drag_y0 = 0, drag_x1 = 0, drag_y1 = 0; uint32_t drag_t0 = 0, drag_ms = 0; bool dragging = false;
} s_script;

void scriptStep() {
  const uint32_t now = millis();
  if (s_script.dragging) {
    const uint32_t t = now - s_script.drag_t0;
    if (t >= s_script.drag_ms) { webPointer(true, s_script.drag_x1, s_script.drag_y1); s_script.dragging = false; s_script.pressing = true; s_script.release_at = now + 40; }
    else {
      const float f = (float)t / (float)s_script.drag_ms;
      webPointer(true, s_script.drag_x0 + (int)((s_script.drag_x1 - s_script.drag_x0) * f),
                 s_script.drag_y0 + (int)((s_script.drag_y1 - s_script.drag_y0) * f));
    }
    return;
  }
  if (s_script.pressing) {
    if ((int32_t)(now - s_script.release_at) < 0) return;
    webPointer(false, 0, 0);   // (coordinates of a release are not used)
    s_script.pressing = false;
    s_script.wait_until = now + 120;   // let the UI see the release before the next step
    return;
  }
  if ((int32_t)(now - s_script.wait_until) < 0) return;
  while (s_script.pc < s_script.lines.size()) {
    std::istringstream in(s_script.lines[s_script.pc++]);
    std::string cmd; in >> cmd;
    if (cmd.empty() || cmd[0] == '#') continue;
    fprintf(stderr, "[script] %s\n", s_script.lines[s_script.pc - 1].c_str());
    if (cmd == "wait") { uint32_t ms = 0; in >> ms; s_script.wait_until = now + ms; return; }
    if (cmd == "tap" || cmd == "hold") {
      int x = 0, y = 0; uint32_t ms = (cmd == "tap") ? 60 : 1300; in >> x >> y; if (cmd == "hold") in >> ms;
      webPointer(true, x, y); s_script.pressing = true; s_script.release_at = now + ms; return;
    }
    if (cmd == "drag") {
      in >> s_script.drag_x0 >> s_script.drag_y0 >> s_script.drag_x1 >> s_script.drag_y1 >> s_script.drag_ms;
      if (!s_script.drag_ms) s_script.drag_ms = 300;
      webPointer(true, s_script.drag_x0, s_script.drag_y0);
      s_script.drag_t0 = now; s_script.dragging = true; return;
    }
    if (cmd == "type") {
      std::string rest; std::getline(in, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      for (char c : rest) webKeyPush((uint8_t)c);
      s_script.wait_until = now + 200; return;
    }
    if (cmd == "key") {
      std::string k; in >> k;
      const uint8_t code = k == "enter" ? 0x0D : k == "esc" ? 0x1B : k == "bs" ? 0x08 : k == "tab" ? 0x09
                         : k == "left" ? 0xB4 : k == "up" ? 0xB5 : k == "down" ? 0xB6 : k == "right" ? 0xB7
                         : k.size() > 2 && k[0] == '0' && (k[1] == 'x' || k[1] == 'X')
                             ? (uint8_t)strtoul(k.c_str() + 2, nullptr, 16)   // a raw key byte (M9 function keys)
                         : (uint8_t)(k.empty() ? 0 : k[0]);
      // An optional count pushes the key that many times at once: the device drains
      // several queued keys in one loop pass, before the UI renders (a fast d-pad).
      int count = 1; in >> count; if (count < 1) count = 1;
      for (int i = 0; code && i < count; ++i) webKeyPush(code);
      s_script.wait_until = now + 150; return;
    }
#if defined(WADA_SIM_PAGER)
    if (cmd == "holdkey") {   // holdkey C MS: type C and keep its key down for MS (Pager simulator; C = space for the space bar)
      std::string k; int ms = 1000; in >> k >> ms;
      if (k == "space") k = " ";   // the stream drops a literal blank
      if (k == "shift") k = "\x01";   // a modifier alone, held (Shift, Fn)
      if (k == "fn")    k = "\x02";
      if (!k.empty()) simPagerHoldKey((uint8_t)k[0], (uint32_t)ms);
      s_script.wait_until = now + 50; return;
    }
#endif
    if (cmd == "wheel") {   // wheel X Y DY: a wheel turn over (X, Y), DY canvas pixels (> 0 = down)
      int x = 0, y = 0, dy = 0; in >> x >> y >> dy;
      webWheel(x, y, dy);
      s_script.wait_until = now + 50; return;
    }
    if (cmd == "shot") { std::string n; in >> n; savePng(s_shot_dir + "/" + n + ".png"); continue; }
    if (cmd == "quit") { s_quit = true; return; }
    fprintf(stderr, "[script] unknown command '%s'\n", cmd.c_str());
  }
}
}  // namespace

bool wadaHostStep() {
  pumpEvents();
  if (!s_script.lines.empty()) scriptStep();
  wadaBoardStep();
  present();
  return !s_quit;
}

int main(int argc, char** argv) {
  bool headless = false;
  int scale = 1;
  std::string script_path;
  WadaBackendChoice choice;   // the demo unless a node is named
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--headless") headless = true;
    else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
    else if (a == "--shots" && i + 1 < argc) s_shot_dir = argv[++i];
    else if (a == "--scale" && i + 1 < argc) scale = atoi(argv[++i]);
    else if (a == "--size" && i + 1 < argc) {   // e.g. 480x960: a phone's portrait canvas
      int w = 0, h = 0;
      if (sscanf(argv[++i], "%dx%d", &w, &h) == 2) wadaWebSetSize(w, h);
    }
    else if (a == "--serial" && i + 1 < argc) { choice.kind = "serial"; choice.arg = argv[++i]; }
    else if (a == "--tcp" && i + 1 < argc) { choice.kind = "tcp"; choice.arg = argv[++i]; }
    else if (a == "--read-only") choice.read_only = true;
  }
  // A headless run (the smoke tour, CI) stays offline unless asked: no map tiles from
  // the network in its screenshots (compat/wada_http.h).
  if (headless) setenv("WADA_NET", "0", 0);
  if (!script_path.empty()) {
    std::ifstream f(script_path);
    for (std::string l; std::getline(f, l);) s_script.lines.push_back(l);
    if (s_script.lines.empty()) fprintf(stderr, "[web] script %s is empty or missing\n", script_path.c_str());
  }
  wadaHostMkdirs(s_shot_dir);
  setvbuf(stdout, nullptr, _IOLBF, 0);

  if (!headless) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (scale < 1) scale = 1;
    s_win = SDL_CreateWindow("wadamesh", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WADA_WEB_W * scale, WADA_WEB_H * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    SDL_RenderSetLogicalSize(s_ren, WADA_WEB_W, WADA_WEB_H);   // mouse coordinates arrive in canvas pixels
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WADA_WEB_W, WADA_WEB_H);
    SDL_StartTextInput();
  }

  wadaBoardBegin(choice);   // demo, or the companion node --serial / --tcp named

  while (wadaHostStep()) SDL_Delay(4);
  // Closing the window is the web board's power-off: flush what the device flushes
  // on its deliberate power paths (chat history is written lazily).
  wadaBoardFlush();

  if (s_win) { SDL_DestroyTexture(s_tex); SDL_DestroyRenderer(s_ren); SDL_DestroyWindow(s_win); SDL_Quit(); }
  fflush(stdout);
  _exit(0);   // detached host tasks may still be sleeping; do not unwind them
}
