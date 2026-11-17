// SPDX-License-Identifier: GPL-3.0-or-later
// wadamesh-desktop for the Cardputer Zero: SDL2 is loaded at run time from the Pi's own
// libSDL2-2.0.so.0 (Raspberry Pi OS desktop ships it), so the binary cross-compiles without
// an arm64 SDL2 to link against. Only the calls platform/host_main.cpp makes are forwarded.
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void* lib(void) {
  static void* h;
  if (!h) {
    h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
      fprintf(stderr, "wadamesh-desktop needs SDL2: sudo apt install libsdl2-2.0-0 (%s)\n", dlerror());
      exit(1);
    }
  }
  return h;
}
#define FWD(ret, name, params, args) \
  ret name params { \
    static ret (*fn) params; \
    if (!fn) fn = (ret (*) params)dlsym(lib(), #name); \
    return fn args; \
  }
#define FWDV(name, params, args) \
  void name params { \
    static void (*fn) params; \
    if (!fn) fn = (void (*) params)dlsym(lib(), #name); \
    fn args; \
  }

FWD(int, SDL_Init, (Uint32 f), (f))
FWDV(SDL_Quit, (void), ())
FWD(const char*, SDL_GetError, (void), ())
FWDV(SDL_Delay, (Uint32 ms), (ms))
FWD(SDL_Window*, SDL_CreateWindow, (const char* t, int x, int y, int w, int h, Uint32 f), (t, x, y, w, h, f))
FWDV(SDL_DestroyWindow, (SDL_Window* w), (w))
FWD(int, SDL_SetWindowFullscreen, (SDL_Window* w, Uint32 f), (w, f))
FWD(SDL_Renderer*, SDL_CreateRenderer, (SDL_Window* w, int i, Uint32 f), (w, i, f))
FWDV(SDL_DestroyRenderer, (SDL_Renderer* r), (r))
FWD(SDL_Texture*, SDL_CreateTexture, (SDL_Renderer* r, Uint32 fmt, int a, int w, int h), (r, fmt, a, w, h))
FWDV(SDL_DestroyTexture, (SDL_Texture* t), (t))
FWD(int, SDL_UpdateTexture, (SDL_Texture* t, const SDL_Rect* r, const void* p, int pitch), (t, r, p, pitch))
FWD(int, SDL_RenderClear, (SDL_Renderer* r), (r))
FWD(int, SDL_RenderCopy, (SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* s, const SDL_Rect* d), (r, t, s, d))
FWDV(SDL_RenderPresent, (SDL_Renderer* r), (r))
FWD(int, SDL_RenderSetLogicalSize, (SDL_Renderer* r, int w, int h), (r, w, h))
FWD(int, SDL_PollEvent, (SDL_Event* e), (e))
FWDV(SDL_StartTextInput, (void), ())
FWD(int, SDL_ShowCursor, (int t), (t))
