// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameBoy.h"

#if defined(HAS_TDECK_GT911)

#include <Arduino.h>          // millis/micros, ps_malloc
#include <SD.h>               // ROM listing + read (T-Deck microSD)
#include <LvglPsramAlloc.h>   // PSRAM canvas buffer
#include <string.h>
#include <strings.h>          // strcasecmp
#include <stdio.h>
#include <stdint.h>

// gnuboy is C. Its header re-defines IRAM_ATTR empty (non-RETRO_GO build); this
// TU already saw esp_attr.h via Arduino/lvgl, so drop the macro first to avoid a
// -Wmacro-redefined warning. GameBoy.cpp never uses IRAM_ATTR itself.
#ifdef IRAM_ATTR
#undef IRAM_ATTR
#endif
extern "C" {
#include "gnuboy/gnuboy.h"
}

// Bridge to UITask's shared-SPI microSD mounter (UITask internals are static).
extern "C" bool touchGbSdEnsureMounted();

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static constexpr lv_coord_t kTopBar   = 22;      // leave the status bar visible
static constexpr uint32_t   FRAME_US  = 16743;   // 70224 clocks @ 4.194 MHz = 59.73 Hz
static constexpr int        MAX_CATCHUP = 3;      // max frames run per timer tick
static constexpr uint32_t   AUTOSAVE_FRAMES = 600;
static constexpr int        AUDIO_RATE = 22050;
static constexpr size_t     ABUF_LEN   = 1024;   // int16 entries
static constexpr int        MAX_ROMS   = 96;
static constexpr size_t     MAX_ROM_BYTES = 4u * 1024 * 1024;  // sanity cap

// GB pad bit for a control button, carried as the button's user_data.
// (Mirror of gb_padbtn_t; kept local so the .h needn't include gnuboy.)

// ---------------------------------------------------------------------------
// State (single instance — one game at a time, like the app's other overlays)
// ---------------------------------------------------------------------------
static lv_obj_t*   s_root      = nullptr;   // full-screen overlay
static lv_obj_t*   s_picker    = nullptr;   // ROM list (phase 1)
static lv_obj_t*   s_canvas    = nullptr;   // 160x144 playfield (phase 2)
static lv_color_t* s_fb        = nullptr;   // canvas buffer == gnuboy framebuffer
static lv_timer_t* s_timer     = nullptr;
static uint8_t*    s_rom        = nullptr;  // full ROM in PSRAM (banks point into it)
static bool        s_playing    = false;

static int         s_pad_touch  = 0;        // held bits from on-screen buttons
static int         s_pulse_mask = 0;        // trackball D-pad pulse
static uint32_t    s_pulse_until = 0;

static uint32_t    s_next_us     = 0;       // deadline of the next frame
static uint32_t    s_frame_count = 0;

static int16_t     s_abuf[ABUF_LEN];        // gnuboy sound scratch (muted v1)

// ROM catalogue (phase 1 -> phase 2 hand-off)
static char        s_rom_paths[MAX_ROMS][128]; // SD path, e.g. /meshcomod/gb/x.gb
static int         s_rom_count = 0;
static char        s_sav_path[160];            // fopen path, /sd/... + .sav

// ---------------------------------------------------------------------------
// gnuboy callbacks
// ---------------------------------------------------------------------------
static void video_cb(void* /*buffer*/) {
    // gnuboy rendered straight into s_fb (== the canvas buffer); just flush.
    if (s_canvas) lv_obj_invalidate(s_canvas);
}
static void audio_cb(void* /*buffer*/, size_t /*len*/) {
    // Muted in this first cut. APU still runs (cheap); no output wired yet.
}

// ---------------------------------------------------------------------------
// Input: one callback for every pad button; user_data holds the GB bit.
// ---------------------------------------------------------------------------
static void padCb(lv_event_t* e) {
    const int bit  = (int)(intptr_t)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)                       s_pad_touch |= bit;
    else if (code == LV_EVENT_RELEASED ||
             code == LV_EVENT_PRESS_LOST)               s_pad_touch &= ~bit;
}

// ---------------------------------------------------------------------------
// Frame pacing (runs inside lv_timer_handler; outer loop() services the mesh)
// ---------------------------------------------------------------------------
static void frameCb(lv_timer_t* /*t*/) {
    if (!s_playing) return;

    const uint32_t now = micros();
    int32_t behind = (int32_t)(now - s_next_us);
    if (behind > (int32_t)(8 * FRAME_US)) {   // hopelessly behind -> resync
        s_next_us = now;
        behind = 0;
    }
    int frames = 1 + (behind > 0 ? behind / (int32_t)FRAME_US : 0);
    if (frames > MAX_CATCHUP) frames = MAX_CATCHUP;

    const int pad = s_pad_touch |
                    ((int32_t)(millis() - s_pulse_until) < 0 ? s_pulse_mask : 0);
    gnuboy_set_pad(pad);

    for (int i = 0; i < frames; i++) {
        gnuboy_run(i == frames - 1);   // draw only the final frame of the burst
        s_next_us += FRAME_US;
    }

    if (++s_frame_count % AUTOSAVE_FRAMES == 0 && gnuboy_sram_dirty())
        gnuboy_save_sram(s_sav_path, false);
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static lv_obj_t* makePadBtn(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h, const char* label,
                            int gb_bit, uint32_t col) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, padCb, LV_EVENT_PRESSED,    (void*)(intptr_t)gb_bit);
    lv_obj_add_event_cb(b, padCb, LV_EVENT_RELEASED,   (void*)(intptr_t)gb_bit);
    lv_obj_add_event_cb(b, padCb, LV_EVENT_PRESS_LOST, (void*)(intptr_t)gb_bit);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}

static void closeCb(lv_event_t*) { GameBoy::close(); }

// ---------------------------------------------------------------------------
// Phase 2: start emulating the chosen ROM
// ---------------------------------------------------------------------------
static void teardownEmu(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_playing) {
        gnuboy_save_sram(s_sav_path, false);   // no-op unless the cart has a battery
        gnuboy_free_rom();
        s_playing = false;
    }
}

static bool loadRomToPsram(const char* sd_path, uint8_t** out, size_t* out_size) {
    File f = SD.open(sd_path, FILE_READ);
    if (!f) return false;
    const size_t sz = f.size();
    if (sz == 0 || sz > MAX_ROM_BYTES) { f.close(); return false; }
    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (!buf) { f.close(); return false; }
    size_t got = 0;
    while (got < sz) {
        int n = f.read(buf + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    f.close();
    if (got != sz) { free(buf); return false; }
    *out = buf; *out_size = sz;
    return true;
}

static void makeSavPath(const char* sd_path) {
    // /meshcomod/gb/x.gb -> /sd/meshcomod/gb/x.sav  (gnuboy's fopen VFS path)
    const char* dot = strrchr(sd_path, '.');
    size_t base = dot ? (size_t)(dot - sd_path) : strlen(sd_path);
    if (base > sizeof(s_sav_path) - 8) base = sizeof(s_sav_path) - 8;
    strcpy(s_sav_path, "/sd");
    memcpy(s_sav_path + 3, sd_path, base);
    strcpy(s_sav_path + 3 + base, ".sav");
}

static void startRom(const char* sd_path) {
    if (s_picker) { lv_obj_del(s_picker); s_picker = nullptr; }

    const lv_coord_t sw = lv_disp_get_hor_res(nullptr);   // 320
    const lv_coord_t sh = lv_disp_get_ver_res(nullptr);   // 240

    // Canvas buffer doubles as gnuboy's RGB565 framebuffer (LV_COLOR_DEPTH 16,
    // no byte swap -> lv_color_t layout == GB_PIXEL_565_LE).
    s_fb = (lv_color_t*)lvglPsramAlloc((size_t)GB_WIDTH * GB_HEIGHT * sizeof(lv_color_t));
    if (!s_fb) { GameBoy::close(); return; }

    s_canvas = lv_canvas_create(s_root);
    lv_canvas_set_buffer(s_canvas, s_fb, GB_WIDTH, GB_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_obj_set_pos(s_canvas, 2, kTopBar + 2);

    // Read the whole ROM into PSRAM (banks point into it — no SD access mid-game).
    size_t rom_sz = 0;
    if (!loadRomToPsram(sd_path, &s_rom, &rom_sz)) { GameBoy::close(); return; }
    makeSavPath(sd_path);

    if (gnuboy_init(AUDIO_RATE, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE,
                    video_cb, audio_cb) < 0) { GameBoy::close(); return; }
    gnuboy_set_framebuffer(s_fb);
    gnuboy_set_soundbuffer(s_abuf, ABUF_LEN);
    if (gnuboy_load_rom(s_rom, rom_sz) < 0) { gnuboy_free_rom(); GameBoy::close(); return; }
    gnuboy_reset(true);
    gnuboy_load_sram(s_sav_path);

    // --- On-screen controls (T-Deck keyboard/trackball can't hold a key) ---
    // D-pad cross (upper right), A/B (lower right), Start/Select (under canvas).
    const uint32_t kDpad = 0x2B2F36, kAB = 0x8A3A46, kSS = 0x30343B;
    makePadBtn(s_root, 210, 40,  46, 40, LV_SYMBOL_UP,    GB_PAD_UP,    kDpad);
    makePadBtn(s_root, 210, 126, 46, 40, LV_SYMBOL_DOWN,  GB_PAD_DOWN,  kDpad);
    makePadBtn(s_root, 176, 82,  42, 44, LV_SYMBOL_LEFT,  GB_PAD_LEFT,  kDpad);
    makePadBtn(s_root, 258, 82,  42, 44, LV_SYMBOL_RIGHT, GB_PAD_RIGHT, kDpad);
    makePadBtn(s_root, 272, 150, 46, 46, "A", GB_PAD_A, kAB);
    makePadBtn(s_root, 216, 172, 46, 46, "B", GB_PAD_B, kAB);
    makePadBtn(s_root, 8,  190, 68, 34, "START",  GB_PAD_START,  kSS);
    makePadBtn(s_root, 86, 190, 68, 34, "SELECT", GB_PAD_SELECT, kSS);

    lv_obj_t* x = lv_btn_create(s_root);
    lv_obj_set_size(x, 30, 22);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE); lv_obj_center(xl);
    (void)sw; (void)sh;

    // --- Start the frame pump ---
    s_pad_touch = 0; s_pulse_mask = 0; s_pulse_until = 0;
    s_frame_count = 0;
    s_next_us = micros();
    s_playing = true;
    s_timer = lv_timer_create(frameCb, 15, nullptr);
}

// ---------------------------------------------------------------------------
// Phase 1: ROM picker
// ---------------------------------------------------------------------------
static bool hasRomExt(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0;
}

static void scanDir(const char* dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f && s_rom_count < MAX_ROMS; f = d.openNextFile()) {
        if (!f.isDirectory()) {
            const char* n = f.name();
            const char* base = strrchr(n, '/'); base = base ? base + 1 : n;
            if (hasRomExt(base))
                snprintf(s_rom_paths[s_rom_count++], sizeof(s_rom_paths[0]), "%s/%s", dir, base);
        }
        f.close();
    }
    d.close();
}

static void romPickCb(lv_event_t* e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_rom_count) startRom(s_rom_paths[idx]);
}

static void buildPicker(void) {
    const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
    const lv_coord_t sh = lv_disp_get_ver_res(nullptr);

    lv_obj_t* x = lv_btn_create(s_root);
    lv_obj_set_size(x, 30, 22);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE); lv_obj_center(xl);

    lv_obj_t* title = lv_label_create(s_root);
    lv_label_set_text(title, "Game Boy");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    s_rom_count = 0;
    if (touchGbSdEnsureMounted()) {
        SD.mkdir("/meshcomod");
        SD.mkdir("/meshcomod/gb");
        scanDir("/meshcomod/gb");
        scanDir("/gb");   // legacy location
    }

    if (s_rom_count == 0) {
        lv_obj_t* msg = lv_label_create(s_root);
        lv_label_set_text(msg, "No ROMs.\nPut .gb / .gbc files in\n/meshcomod/gb on the SD card.");
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    s_picker = lv_list_create(s_root);
    lv_obj_set_size(s_picker, sw - 8, sh - kTopBar - 30);
    lv_obj_align(s_picker, LV_ALIGN_BOTTOM_MID, 0, -2);
    for (int i = 0; i < s_rom_count; i++) {
        const char* base = strrchr(s_rom_paths[i], '/');
        base = base ? base + 1 : s_rom_paths[i];
        lv_obj_t* b = lv_list_add_btn(s_picker, LV_SYMBOL_PLAY, base);
        lv_obj_add_event_cb(b, romPickCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void GameBoy::launch() {
    if (s_root) return;
    const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
    const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, sw, sh - kTopBar);
    lv_obj_set_pos(s_root, 0, kTopBar);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0A0B0C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    buildPicker();
}

bool GameBoy::isOpen() { return s_root != nullptr; }

void GameBoy::steer(int dx, int dy) {
    if (!s_playing) return;
    if (dx == 0 && dy == 0) return;
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= ady) s_pulse_mask = (dx > 0) ? GB_PAD_RIGHT : GB_PAD_LEFT;
    else            s_pulse_mask = (dy > 0) ? GB_PAD_DOWN  : GB_PAD_UP;
    s_pulse_until = millis() + 120;   // brief hold; frameCb clears it
}

void GameBoy::close() {
    teardownEmu();
    if (s_root) { lv_obj_del(s_root); s_root = nullptr; }   // deletes canvas + buttons
    s_picker = nullptr; s_canvas = nullptr;
    if (s_fb)  { lvglPsramFree(s_fb); s_fb = nullptr; }     // after the canvas is gone
    if (s_rom) { free(s_rom); s_rom = nullptr; }            // banks pointed into it; free_rom left it
    s_pad_touch = 0; s_pulse_mask = 0;
}

#else  // !HAS_TDECK_GT911 — no SD / touch controls on the V4; stub the player.

void GameBoy::launch()          {}
bool GameBoy::isOpen()          { return false; }
void GameBoy::steer(int, int)   {}
void GameBoy::close()           {}

#endif
