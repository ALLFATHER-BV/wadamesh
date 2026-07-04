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
#include <driver/i2s.h>       // MAX98357A speaker amp — streaming GB audio
#include "../helpers/esp32/TouchPrefsStore.h"  // touchPrefsGetSoundVolume()

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
static constexpr lv_coord_t kTopBar    = 22;      // leave the status bar visible
static constexpr uint32_t   FRAME_US   = 16743;   // 70224 clocks @ 4.194 MHz = 59.73 Hz
static constexpr int        MAX_CATCHUP = 3;      // max frames run per timer tick
static constexpr uint32_t   AUTOSAVE_FRAMES = 600;
static constexpr int        AUDIO_RATE = 22050;
static constexpr size_t     ABUF_LEN   = 1024;    // int16 entries
static constexpr int        MAX_ROMS   = 96;
static constexpr size_t     MAX_ROM_BYTES = 4u * 1024 * 1024;  // sanity cap
static constexpr uint32_t   PULSE_MS   = 130;     // trackball/keyboard pad-tap hold

// I2S audio — same MAX98357A wiring as UITask's notification tones (mono amp).
// GB owns I2S_NUM_0 only while unmuted; the notify path is gated off meanwhile.
#ifndef PIN_I2S_BCK
  #define PIN_I2S_BCK  7
#endif
#ifndef PIN_I2S_WS
  #define PIN_I2S_WS   5
#endif
#ifndef PIN_I2S_DOUT
  #define PIN_I2S_DOUT 6
#endif
static constexpr i2s_port_t kGbI2sPort          = I2S_NUM_0;
static constexpr int        AUDIO_WARMUP_FRAMES = 45;   // swallow startup garble

// ---------------------------------------------------------------------------
// State (single instance — one game at a time, like the app's other overlays)
// ---------------------------------------------------------------------------
static lv_obj_t*   s_root      = nullptr;   // full-screen overlay
static lv_obj_t*   s_picker    = nullptr;   // ROM list (phase 1)
static lv_obj_t*   s_play      = nullptr;   // play-UI container (canvas + controls)
static lv_obj_t*   s_canvas    = nullptr;   // scaled GB output
static lv_obj_t*   s_mute_lbl  = nullptr;   // speaker icon (updated on toggle)
static lv_color_t* s_fb        = nullptr;   // canvas buffer (scaled output)
static lv_color_t* s_gbfb      = nullptr;   // gnuboy's fixed 160x144 framebuffer
static lv_timer_t* s_timer     = nullptr;
static uint8_t*    s_rom        = nullptr;  // full ROM in PSRAM (banks point into it)
static bool        s_playing    = false;
static bool        s_immersive  = false;    // hide controls, scale to full height

static int         s_out_w = GB_WIDTH, s_out_h = GB_HEIGHT;  // canvas size
static uint8_t     s_sx_map[320];           // out col -> source col (0..159)
static uint8_t     s_sy_map[240];           // out row -> source row (0..143)

static int         s_pad_touch  = 0;        // held bits from on-screen buttons
static bool        s_click_a     = false;   // trackball click -> A (held)
static int         s_pulse_mask = 0;        // trackball/keyboard tap pulse
static uint32_t    s_pulse_until = 0;

static uint32_t    s_next_us     = 0;       // deadline of the next frame
static uint32_t    s_frame_count = 0;

static int16_t     s_abuf[ABUF_LEN];        // gnuboy sound scratch (mono S16)
static bool        s_audio_ok    = false;   // I2S installed (i.e. unmuted)
static int         s_audio_warmup = 0;      // frames of audio to swallow at start

// ROM catalogue (phase 1 -> phase 2 hand-off)
static char        s_rom_paths[MAX_ROMS][128]; // SD path, e.g. /meshcomod/gb/x.gb
static int         s_rom_count = 0;
static char        s_sav_path[160];            // fopen path, /sd/... + .sav

// Forward decls (a couple of cyclic references below).
static void buildPlayUI(void);
static void teardownPlayUI(void);
static void audioStart(void);
static void audioStop(void);

// ---------------------------------------------------------------------------
// Rendering: gnuboy renders into s_gbfb (160x144); we copy/scale into the canvas.
// ---------------------------------------------------------------------------
static void buildScaleMaps(int ow, int oh) {
    for (int ox = 0; ox < ow; ox++) s_sx_map[ox] = (uint8_t)(ox * GB_WIDTH / ow);
    for (int oy = 0; oy < oh; oy++) s_sy_map[oy] = (uint8_t)(oy * GB_HEIGHT / oh);
}

static void video_cb(void* /*buffer*/) {
    if (!s_canvas || !s_fb || !s_gbfb) return;
    if (s_out_w == GB_WIDTH && s_out_h == GB_HEIGHT) {
        memcpy(s_fb, s_gbfb, (size_t)GB_WIDTH * GB_HEIGHT * sizeof(lv_color_t));
    } else {
        int prev_sy = -1;
        const lv_color_t* prev = nullptr;
        for (int oy = 0; oy < s_out_h; oy++) {
            lv_color_t* row = &s_fb[oy * s_out_w];
            const int sy = s_sy_map[oy];
            if (sy == prev_sy && prev) {
                memcpy(row, prev, (size_t)s_out_w * sizeof(lv_color_t));
            } else {
                const lv_color_t* srow = &s_gbfb[sy * GB_WIDTH];
                for (int ox = 0; ox < s_out_w; ox++) row[ox] = srow[s_sx_map[ox]];
                prev_sy = sy;
            }
            prev = row;
        }
    }
    lv_obj_invalidate(s_canvas);
}

static void audio_cb(void* buffer, size_t length) {
    // gnuboy hands us `length` mono S16 samples. Scale by the user volume pref
    // and stream to the amp. Short write timeout: the DMA cushion smooths jitter,
    // and we must never stall the frame for long.
    if (!s_audio_ok || s_audio_warmup > 0 || length == 0) return;
    const int vol = (int)touchPrefsGetSoundVolume();   // 0..100
    if (vol <= 0) return;
    int16_t* s = (int16_t*)buffer;
    if (vol < 100)
        for (size_t i = 0; i < length; i++) s[i] = (int16_t)((int)s[i] * vol / 100);
    size_t written = 0;
    i2s_write(kGbI2sPort, s, length * sizeof(int16_t), &written, pdMS_TO_TICKS(4));
}

// ---------------------------------------------------------------------------
// I2S lifecycle — installed only while unmuted (muted by default).
// ---------------------------------------------------------------------------
static void audioStart(void) {
    s_audio_ok = false;
    s_audio_warmup = AUDIO_WARMUP_FRAMES;
    // Skip audio if internal DMA RAM is too tight — mirrors UITask's
    // i2s_driver_install pre-flight so we don't trip its NO_MEM abort.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16 * 1024) return;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = AUDIO_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // MAX98357A is mono
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 6;      // ~70 ms cushion at 22050 Hz
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    if (i2s_driver_install(kGbI2sPort, &cfg, 0, nullptr) != ESP_OK) return;
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_BCK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(kGbI2sPort, &pins) != ESP_OK) { i2s_driver_uninstall(kGbI2sPort); return; }
    s_audio_ok = true;
}
static void audioStop(void) {
    if (!s_audio_ok) return;
    i2s_zero_dma_buffer(kGbI2sPort);
    i2s_driver_uninstall(kGbI2sPort);
    s_audio_ok = false;
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

    int pad = s_pad_touch;
    if (s_click_a) pad |= GB_PAD_A;
    if ((int32_t)(millis() - s_pulse_until) < 0) pad |= s_pulse_mask;
    else                                         s_pulse_mask = 0;
    gnuboy_set_pad(pad);

    for (int i = 0; i < frames; i++) {
        gnuboy_run(i == frames - 1);   // draw only the final frame of the burst
        s_next_us += FRAME_US;
    }

    if (s_audio_warmup > 0) s_audio_warmup--;

    if (++s_frame_count % AUTOSAVE_FRAMES == 0 && gnuboy_sram_dirty())
        gnuboy_save_sram(s_sav_path, false);
}

// ---------------------------------------------------------------------------
// UI: on-screen pad + top-right cluster (close / mode / mute)
// ---------------------------------------------------------------------------
static void padCb(lv_event_t* e) {
    const int bit  = (int)(intptr_t)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)                       s_pad_touch |= bit;
    else if (code == LV_EVENT_RELEASED ||
             code == LV_EVENT_PRESS_LOST)               s_pad_touch &= ~bit;
}

static void makePadBtn(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
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
}

static void closeCb(lv_event_t*) { GameBoy::close(); }

// Mode toggle deletes the play-UI (an ancestor of the button that fired), so do
// it after the event settles via lv_async_call to avoid a use-after-free.
static void doRebuild(void*) {
    if (!s_playing) return;
    teardownPlayUI();
    buildPlayUI();
}
static void modeCb(lv_event_t*) { s_immersive = !s_immersive; lv_async_call(doRebuild, nullptr); }

static void muteCb(lv_event_t*) {
    if (s_audio_ok) audioStop(); else audioStart();
    if (s_mute_lbl) lv_label_set_text(s_mute_lbl, s_audio_ok ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
}

static lv_obj_t* clusterBtn(const char* icon, lv_coord_t yoff, lv_event_cb_t cb) {
    lv_obj_t* b = lv_btn_create(s_play);
    lv_obj_set_size(b, 30, 24);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -2, yoff);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, icon);
    lv_obj_center(l);
    return l;
}

static void buildControls(void) {
    // D-pad cross (upper right), A/B (lower right), Start/Select (under canvas).
    const uint32_t kDpad = 0x2B2F36, kAB = 0x8A3A46, kSS = 0x30343B;
    makePadBtn(s_play, 210, 40,  46, 40, LV_SYMBOL_UP,    GB_PAD_UP,    kDpad);
    makePadBtn(s_play, 210, 126, 46, 40, LV_SYMBOL_DOWN,  GB_PAD_DOWN,  kDpad);
    makePadBtn(s_play, 176, 82,  42, 44, LV_SYMBOL_LEFT,  GB_PAD_LEFT,  kDpad);
    makePadBtn(s_play, 258, 82,  42, 44, LV_SYMBOL_RIGHT, GB_PAD_RIGHT, kDpad);
    makePadBtn(s_play, 272, 148, 46, 46, "A", GB_PAD_A, kAB);
    makePadBtn(s_play, 216, 170, 46, 46, "B", GB_PAD_B, kAB);
    makePadBtn(s_play, 8,  180, 68, 32, "START",  GB_PAD_START,  kSS);
    makePadBtn(s_play, 86, 180, 68, 32, "SELECT", GB_PAD_SELECT, kSS);
}

static void teardownPlayUI(void) {
    if (s_play) { lv_obj_del(s_play); s_play = nullptr; }
    s_canvas = nullptr; s_mute_lbl = nullptr;
    if (s_fb) { lvglPsramFree(s_fb); s_fb = nullptr; }
}

static void buildPlayUI(void) {
    const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
    const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
    const int root_h = (int)sh - kTopBar;

    s_play = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_play);
    lv_obj_set_size(s_play, sw, root_h);
    lv_obj_set_pos(s_play, 0, 0);
    lv_obj_clear_flag(s_play, LV_OBJ_FLAG_SCROLLABLE);

    if (s_immersive) {                          // aspect-scale to full height, centered
        s_out_h = root_h;
        s_out_w = GB_WIDTH * root_h / GB_HEIGHT;
        if (s_out_w > (int)sw) { s_out_w = (int)sw; s_out_h = GB_HEIGHT * (int)sw / GB_WIDTH; }
    } else {                                    // 1x, room for the on-screen pad
        s_out_w = GB_WIDTH;
        s_out_h = GB_HEIGHT;
    }

    s_fb = (lv_color_t*)lvglPsramAlloc((size_t)s_out_w * s_out_h * sizeof(lv_color_t));
    if (!s_fb) { GameBoy::close(); return; }
    buildScaleMaps(s_out_w, s_out_h);

    s_canvas = lv_canvas_create(s_play);
    lv_canvas_set_buffer(s_canvas, s_fb, s_out_w, s_out_h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    if (s_immersive) lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    else             lv_obj_set_pos(s_canvas, 2, 2);

    if (!s_immersive) buildControls();

    clusterBtn(LV_SYMBOL_CLOSE, 0,  closeCb);
    clusterBtn(LV_SYMBOL_IMAGE, 28, modeCb);
    s_mute_lbl = clusterBtn(s_audio_ok ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE, 56, muteCb);
}

// ---------------------------------------------------------------------------
// Phase 2: start emulating the chosen ROM
// ---------------------------------------------------------------------------
static void teardownEmu(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    audioStop();                            // frameCb is gone -> no more audio_cb
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

    // gnuboy's fixed 160x144 framebuffer (RGB565 LE == lv_color_t); the canvas
    // buffer is separate so it can be scaled without touching the emulator.
    s_gbfb = (lv_color_t*)lvglPsramAlloc((size_t)GB_WIDTH * GB_HEIGHT * sizeof(lv_color_t));
    if (!s_gbfb) { GameBoy::close(); return; }

    size_t rom_sz = 0;
    if (!loadRomToPsram(sd_path, &s_rom, &rom_sz)) { GameBoy::close(); return; }
    makeSavPath(sd_path);

    if (gnuboy_init(AUDIO_RATE, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE,
                    video_cb, audio_cb) < 0) { GameBoy::close(); return; }
    gnuboy_set_framebuffer(s_gbfb);
    gnuboy_set_soundbuffer(s_abuf, ABUF_LEN);
    if (gnuboy_load_rom(s_rom, rom_sz) < 0) { gnuboy_free_rom(); GameBoy::close(); return; }
    gnuboy_reset(true);
    gnuboy_load_sram(s_sav_path);

    buildPlayUI();                 // muted by default -> no audioStart here
    if (!s_play) return;           // buildPlayUI failed + already closed

    s_pad_touch = 0; s_click_a = false; s_pulse_mask = 0; s_pulse_until = 0;
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
    if (adx >= ady) s_pulse_mask |= (dx > 0) ? GB_PAD_RIGHT : GB_PAD_LEFT;
    else            s_pulse_mask |= (dy > 0) ? GB_PAD_DOWN  : GB_PAD_UP;
    s_pulse_until = millis() + PULSE_MS;
}

void GameBoy::setA(bool down) { s_click_a = down; }

void GameBoy::keyChar(char c) {
    if (!s_playing) return;
    int bit = 0;
    switch (c) {
        case 'w': case 'W': bit = GB_PAD_UP;    break;
        case 's': case 'S': bit = GB_PAD_DOWN;  break;
        case 'a': case 'A': bit = GB_PAD_LEFT;  break;
        case 'd': case 'D': bit = GB_PAD_RIGHT; break;
        case 'k': case 'K': case 'm': case 'M': bit = GB_PAD_A;      break;
        case 'j': case 'J': case 'n': case 'N': bit = GB_PAD_B;      break;
        case '\r': case '\n':                   bit = GB_PAD_START;  break;
        case ' ':                               bit = GB_PAD_SELECT; break;
        default: return;
    }
    s_pulse_mask |= bit;
    s_pulse_until = millis() + PULSE_MS;
}

void GameBoy::close() {
    teardownEmu();
    if (s_root) { lv_obj_del(s_root); s_root = nullptr; }   // deletes play-UI + children
    s_picker = nullptr; s_play = nullptr; s_canvas = nullptr; s_mute_lbl = nullptr;
    if (s_fb)   { lvglPsramFree(s_fb);   s_fb = nullptr; }   // after the canvas is gone
    if (s_gbfb) { lvglPsramFree(s_gbfb); s_gbfb = nullptr; }
    if (s_rom)  { free(s_rom); s_rom = nullptr; }            // banks pointed into it; free_rom left it
    s_pad_touch = 0; s_click_a = false; s_pulse_mask = 0;
}

#else  // !HAS_TDECK_GT911 — no SD / input on the V4; stub the player.

void GameBoy::launch()          {}
bool GameBoy::isOpen()          { return false; }
void GameBoy::steer(int, int)   {}
void GameBoy::setA(bool)        {}
void GameBoy::keyChar(char)     {}
void GameBoy::close()           {}

#endif
