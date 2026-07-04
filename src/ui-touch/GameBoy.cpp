// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameBoy.h"

// gnuboy is C — pull its public API into this C++ TU behind extern "C". This
// also validates, at compile time on BOTH build envs, that the vendored core's
// headers are C++-includable and its declared signatures resolve.
//
// Unlike the core's own .c files (which pull no Arduino/ESP headers), this TU
// already saw esp_attr.h via lvgl.h, so drop its IRAM_ATTR before gnuboy.h
// re-defines the macro empty — avoids a -Wmacro-redefined warning. GameBoy.cpp
// itself never uses IRAM_ATTR.
#ifdef IRAM_ATTR
#undef IRAM_ATTR
#endif
extern "C" {
#include "gnuboy/gnuboy.h"
}

bool GameBoy::s_open = false;

// ---------------------------------------------------------------------------
// SCAFFOLD API probe.
//
// Step 1's job is to prove the gnuboy core compiles under wadamesh's Xtensa
// toolchain + flags and that every public entry point we will drive in step 2
// is declared with the expected signature. Taking their addresses makes the
// compiler check each declaration against gnuboy.h; a missing/renamed symbol
// fails to compile here rather than surfacing later at wiring time. (This is a
// COMPILE gate — launch() is not yet reachable from a root, so --gc-sections
// strips the core back out of the linked image; genuine link + runtime
// validation follows in step 2 when launch() is wired into the Apps drawer.)
// Deleted once launch() actually drives the core.
// ---------------------------------------------------------------------------
static const void* const s_gb_api_probe[] __attribute__((used)) = {
    (const void*)&gnuboy_init,
    (const void*)&gnuboy_load_rom_file,
    (const void*)&gnuboy_free_rom,
    (const void*)&gnuboy_reset,
    (const void*)&gnuboy_run,
    (const void*)&gnuboy_set_pad,
    (const void*)&gnuboy_set_framebuffer,
    (const void*)&gnuboy_set_soundbuffer,
    (const void*)&gnuboy_sram_dirty,
    (const void*)&gnuboy_load_bank,
};

void GameBoy::launch() {
    // SCAFFOLD: reference the probe so the API surface is retained, then return.
    // The real player (LVGL canvas overlay + gnuboy_run() game loop + input +
    // SD ROM/save wiring) lands in step 2.
    (void)s_gb_api_probe;
    s_open = false;
}

bool GameBoy::isOpen()        { return s_open; }
void GameBoy::steer(int, int) { /* SCAFFOLD: no-op until step 2 */ }
void GameBoy::close()         { s_open = false; }
