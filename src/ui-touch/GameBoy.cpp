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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
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
static constexpr size_t     AUDIO_SB_BYTES = 16384;  // ~370 ms @ 22050 Hz mono S16

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

enum ScaleMode { SCALE_1X, SCALE_FIT, SCALE_INT2X };

// ---------------------------------------------------------------------------
// State (single instance — one game at a time, like the app's other overlays)
// ---------------------------------------------------------------------------
static lv_obj_t*   s_root      = nullptr;   // full-screen overlay
static lv_obj_t*   s_picker    = nullptr;   // ROM list (phase 1)
static lv_obj_t*   s_play      = nullptr;   // play-UI container (canvas + controls)
static lv_obj_t*   s_canvas    = nullptr;   // scaled GB output
static lv_obj_t*   s_menu_btn  = nullptr;   // top-right menu button (repainted over the direct blit)
static lv_obj_t*   s_menu      = nullptr;   // in-game pause menu (phase 2)
static lv_color_t* s_fb        = nullptr;   // canvas buffer (scaled output)
static lv_color_t* s_gbfb      = nullptr;   // gnuboy's fixed 160x144 framebuffer
static lv_timer_t* s_timer     = nullptr;
static uint8_t*    s_rom        = nullptr;  // full ROM in PSRAM (banks point into it)
static bool        s_playing    = false;
static bool        s_rom_loaded = false;    // gnuboy_load_rom succeeded (own the banks)
static bool        s_paused     = false;    // menu open -> emulation halted
static ScaleMode   s_scale_mode = SCALE_1X;

static int         s_out_w = GB_WIDTH, s_out_h = GB_HEIGHT;  // canvas size
static uint8_t     s_sx_map[320];           // out col -> source col (0..159)
static uint8_t     s_sy_map[240];           // out row -> source row (0..143)

static int         s_pad_touch  = 0;        // held bits from on-screen buttons
static bool        s_click_a     = false;   // trackball click -> A (held)
static int         s_pulse_mask = 0;        // trackball/keyboard tap pulse
static uint32_t    s_pulse_until = 0;

static uint32_t    s_next_us     = 0;       // deadline of the next frame
static uint32_t    s_frame_count = 0;

// Audio: producer (audio_cb, UI thread) -> stream buffer -> consumer task (core 1)
// -> blocking i2s_write. Decouples the amp from frame timing so a slow/bursty
// frame can't underrun the DMA (the crackle in the per-frame-push version).
static int16_t             s_abuf[ABUF_LEN];       // gnuboy sound scratch (mono S16)
static bool                s_audio_ok    = false;  // I2S + task up (i.e. unmuted)
static int                 s_audio_warmup = 0;
static StreamBufferHandle_t s_audio_sb   = nullptr;
static StaticStreamBuffer_t s_audio_sb_ctl;
static uint8_t*            s_audio_store  = nullptr; // PSRAM backing for the stream
static TaskHandle_t        s_audio_task   = nullptr;
static volatile bool       s_audio_run    = false;

// ROM catalogue (phase 1 -> phase 2 hand-off)
static char        s_rom_paths[MAX_ROMS][128]; // SD path, e.g. /meshcomod/gb/x.gb
static int         s_rom_count = 0;
static char        s_sav_path[160];            // fopen path, /sd/... + .sav
static char        s_sta_path[160];            // fopen path, /sd/... + .sta (save state)

// Forward decls (cyclic references below).
static void buildPlayUI(void);
static void teardownPlayUI(void);
static void audioStart(void);
static void audioStop(void);
static void closeMenu(void);

// ---------------------------------------------------------------------------
// Rendering: gnuboy renders into s_gbfb (160x144). Scale it into the canvas
// buffer with nearest-neighbour (Fit/2x); 1x is a straight copy.
// ---------------------------------------------------------------------------
static void scaleGbfbInto(lv_color_t* dst) {
    int prev_sy = -1;
    const lv_color_t* prev = nullptr;
    for (int oy = 0; oy < s_out_h; oy++) {
        lv_color_t* row = &dst[oy * s_out_w];
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

#if defined(GB_DIRECT_PANEL)
// Experimental fast path: push the finished frame straight to the ST7789,
// bypassing the LVGL canvas-flush pipeline (dirty tracking + blend + copy).
// video_cb runs on the UI thread inside frameCb, so this is serialized with
// LVGL's own flush and the mesh loop — no SPI-bus contention. The canvas is
// never invalidated, so LVGL leaves the game region to us.
extern "C" void touchGbBlit(int x, int y, int w, int h, const uint16_t* px);

static void video_cb(void* /*buffer*/) {
    if (!s_canvas || !s_gbfb) return;
    const uint16_t* src;
    if (s_out_w == GB_WIDTH && s_out_h == GB_HEIGHT) {
        src = (const uint16_t*)s_gbfb;          // 1x: push the emulator fb directly (no copy)
    } else {
        if (!s_fb) return;
        scaleGbfbInto(s_fb);
        src = (const uint16_t*)s_fb;
    }
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);            // LVGL-space coords (what writePixelsRGB565 wants)
    touchGbBlit(a.x1, a.y1, s_out_w, s_out_h, src);
    // The blit rect can cover the top-right menu button (e.g. full-width 2x).
    // Repaint it so LVGL composites it back on top of the frame.
    if (s_menu_btn) lv_obj_invalidate(s_menu_btn);
}
#else
static void video_cb(void* /*buffer*/) {
    if (!s_canvas || !s_fb || !s_gbfb) return;
    if (s_out_w == GB_WIDTH && s_out_h == GB_HEIGHT)
        memcpy(s_fb, s_gbfb, (size_t)GB_WIDTH * GB_HEIGHT * sizeof(lv_color_t));
    else
        scaleGbfbInto(s_fb);
    lv_obj_invalidate(s_canvas);
}
#endif

static void audio_cb(void* buffer, size_t length) {
    // Scale by the volume pref and hand off to the audio task via the stream
    // buffer. Non-blocking (drop on full) so the frame is never stalled here.
    if (!s_audio_ok || s_audio_warmup > 0 || length == 0 || !s_audio_sb) return;
    const int vol = (int)touchPrefsGetSoundVolume();   // 0..100
    if (vol <= 0) return;
    int16_t* s = (int16_t*)buffer;
    if (vol < 100)
        for (size_t i = 0; i < length; i++) s[i] = (int16_t)((int)s[i] * vol / 100);
    xStreamBufferSend(s_audio_sb, s, length * sizeof(int16_t), 0);
}

// ---------------------------------------------------------------------------
// Audio consumer task — blocks on i2s_write so the DMA stays fed. Pinned to
// core 1 so the core-0 LVGL flush can't starve it. Exits when s_audio_run clears.
// ---------------------------------------------------------------------------
static void audioTaskFn(void*) {
    uint8_t buf[512];   // up to 256 mono S16 samples
    while (s_audio_run) {
        size_t got = xStreamBufferReceive(s_audio_sb, buf, sizeof(buf), pdMS_TO_TICKS(20));
        // On empty (timeout) write nothing: tx_desc_auto_clear zeroes the DMA, so
        // an underrun is clean silence. Feeding our own silence chunks mid-stream
        // was itself a click source. Write ALL received bytes (portMAX) so a
        // partial i2s_write can't drop the tail — the other click source.
        size_t off = 0;
        while (off < got && s_audio_run) {
            size_t w = 0;
            if (i2s_write(kGbI2sPort, buf + off, got - off, &w, portMAX_DELAY) != ESP_OK) break;
            off += w;
        }
    }
    s_audio_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// I2S + audio task lifecycle — up only while unmuted (muted by default).
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
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = 256;
    cfg.tx_desc_auto_clear = true;
    cfg.use_apll = true;                        // accurate 22050 Hz -> less drift/crackle
    if (i2s_driver_install(kGbI2sPort, &cfg, 0, nullptr) != ESP_OK) {
        cfg.use_apll = false;                  // APLL unavailable on this clock -> fall back
        if (i2s_driver_install(kGbI2sPort, &cfg, 0, nullptr) != ESP_OK) return;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_BCK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(kGbI2sPort, &pins) != ESP_OK) { i2s_driver_uninstall(kGbI2sPort); return; }

    s_audio_store = (uint8_t*)ps_malloc(AUDIO_SB_BYTES + 1);
    if (!s_audio_store) { i2s_driver_uninstall(kGbI2sPort); return; }
    s_audio_sb = xStreamBufferCreateStatic(AUDIO_SB_BYTES, 1, s_audio_store, &s_audio_sb_ctl);
    if (!s_audio_sb) { free(s_audio_store); s_audio_store = nullptr; i2s_driver_uninstall(kGbI2sPort); return; }

    s_audio_run = true;
    if (xTaskCreatePinnedToCore(audioTaskFn, "gbaudio", 4096, nullptr, 3, &s_audio_task, 1) != pdPASS) {
        s_audio_run = false;
        vStreamBufferDelete(s_audio_sb); s_audio_sb = nullptr;
        free(s_audio_store); s_audio_store = nullptr;
        i2s_driver_uninstall(kGbI2sPort);
        return;
    }
    s_audio_ok = true;
    gnuboy_set_mute(false);   // resume APU synthesis now that we're playing it out
}

static void audioStop(void) {
    if (!s_audio_ok) { gnuboy_set_mute(true); return; }
    s_audio_ok = false;
    gnuboy_set_mute(true);                         // stop synthesizing while silent
    s_audio_run = false;                          // ask the task to exit
    for (int i = 0; i < 60 && s_audio_task; i++)  // wait for it (bounded ~300 ms)
        vTaskDelay(pdMS_TO_TICKS(5));
    i2s_zero_dma_buffer(kGbI2sPort);
    i2s_driver_uninstall(kGbI2sPort);
    if (s_audio_sb)    { vStreamBufferDelete(s_audio_sb); s_audio_sb = nullptr; }
    if (s_audio_store) { free(s_audio_store); s_audio_store = nullptr; }
}

// ---------------------------------------------------------------------------
// Frame pacing (runs inside lv_timer_handler; outer loop() services the mesh)
// ---------------------------------------------------------------------------
static void frameCb(lv_timer_t* /*t*/) {
    if (!s_playing) return;
    if (s_paused) { s_next_us = micros(); return; }   // menu open — hold, no drift

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
// On-screen pad
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

static void buildControls(void) {
    // Game canvas is on the left (160x144). All controls sit to its right:
    // D-pad cross top, A/B below it (raised), Start bottom-left, Select bottom-right.
    const uint32_t kDpad = 0x2B2F36, kAB = 0x8A3A46, kSS = 0x30343B;
    makePadBtn(s_play, 204, 4,   46, 38, LV_SYMBOL_UP,    GB_PAD_UP,    kDpad);
    makePadBtn(s_play, 168, 44,  42, 42, LV_SYMBOL_LEFT,  GB_PAD_LEFT,  kDpad);
    makePadBtn(s_play, 250, 44,  42, 42, LV_SYMBOL_RIGHT, GB_PAD_RIGHT, kDpad);
    makePadBtn(s_play, 204, 88,  46, 38, LV_SYMBOL_DOWN,  GB_PAD_DOWN,  kDpad);
    makePadBtn(s_play, 266, 122, 50, 48, "A", GB_PAD_A, kAB);   // raised from y148
    makePadBtn(s_play, 206, 140, 50, 48, "B", GB_PAD_B, kAB);   // raised from y170
    makePadBtn(s_play, 8,   186, 70, 30, "START",  GB_PAD_START,  kSS);  // bottom-left
    makePadBtn(s_play, 238, 186, 78, 30, "SELECT", GB_PAD_SELECT, kSS);  // moved to the right
}

// ---------------------------------------------------------------------------
// In-game menu (pause). Actions that delete/rebuild the UI defer via lv_async
// so we never free the widget tree from inside its own event callback.
// ---------------------------------------------------------------------------
enum PendAct { PEND_NONE, PEND_RESUME, PEND_DISPLAY, PEND_EXIT };
static volatile int s_pend = PEND_NONE;

static void pendDispatch(void*) {
    const int a = s_pend; s_pend = PEND_NONE;
    switch (a) {
        case PEND_RESUME:  closeMenu(); break;
        case PEND_DISPLAY: closeMenu(); teardownPlayUI(); buildPlayUI(); break;
        case PEND_EXIT:    GameBoy::close(); break;
        default: break;
    }
}
static void pend(int a) { s_pend = a; lv_async_call(pendDispatch, nullptr); }

static void miResumeCb(lv_event_t*) { pend(PEND_RESUME); }
static void miSaveCb  (lv_event_t*) { gnuboy_save_state(s_sta_path); pend(PEND_RESUME); }
static void miLoadCb  (lv_event_t*) { gnuboy_load_state(s_sta_path); pend(PEND_RESUME); }
static void miResetCb (lv_event_t*) { gnuboy_reset(true); pend(PEND_RESUME); }
static void miDispCb  (lv_event_t*) {
    s_scale_mode = (ScaleMode)((s_scale_mode + 1) % 3);   // 1x -> Fit -> Int2x
    pend(PEND_DISPLAY);
}
static void miSoundCb (lv_event_t*) { if (s_audio_ok) audioStop(); else audioStart(); pend(PEND_RESUME); }
static void miExitCb  (lv_event_t*) { pend(PEND_EXIT); }

static void menuRow(lv_obj_t* list, const char* icon, const char* text, lv_event_cb_t cb) {
    lv_obj_t* b = lv_list_add_btn(list, icon, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
}

static void buildMenu(void) {
    if (s_menu) return;
    s_menu = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, 210, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_menu, lv_disp_get_ver_res(nullptr) - 20, LV_PART_MAIN);
    lv_obj_center(s_menu);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x14181C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_menu, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_menu, 6, LV_PART_MAIN);

    lv_obj_t* list = lv_list_create(s_menu);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, lv_disp_get_ver_res(nullptr) - 32, LV_PART_MAIN);

    const char* disp = (s_scale_mode == SCALE_1X) ? "Display: 1x"
                     : (s_scale_mode == SCALE_FIT) ? "Display: Fit"
                                                   : "Display: 2x";
    menuRow(list, LV_SYMBOL_PLAY,    "Resume",     miResumeCb);
    menuRow(list, LV_SYMBOL_SAVE,    "Save state", miSaveCb);
    menuRow(list, LV_SYMBOL_UPLOAD,  "Load state", miLoadCb);
    menuRow(list, LV_SYMBOL_REFRESH, "Reset",      miResetCb);
    menuRow(list, LV_SYMBOL_IMAGE,   disp,         miDispCb);
    menuRow(list, s_audio_ok ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE,
                  s_audio_ok ? "Sound: On" : "Sound: Off", miSoundCb);
    menuRow(list, LV_SYMBOL_CLOSE,   "Exit",       miExitCb);
}

static void closeMenu(void) {
    if (s_menu) { lv_obj_del(s_menu); s_menu = nullptr; }
    s_paused = false;
    s_next_us = micros();
}

static void openMenuCb(lv_event_t*) {
    if (s_menu) return;
    s_paused = true;
    buildMenu();
}

// ---------------------------------------------------------------------------
// Play-UI build / teardown (rebuilt on display-mode change)
// ---------------------------------------------------------------------------
static void teardownPlayUI(void) {
    // Callers always closeMenu() first; s_menu (a child of s_root, not s_play)
    // is managed by closeMenu()/close(), so we only drop the play container here.
    if (s_play) { lv_obj_del(s_play); s_play = nullptr; }
    s_canvas = nullptr; s_menu_btn = nullptr;
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

    if (s_scale_mode == SCALE_1X) {
        s_out_w = GB_WIDTH;  s_out_h = GB_HEIGHT;         // native, room for the pad
    } else if (s_scale_mode == SCALE_FIT) {
        s_out_h = root_h;                                 // aspect-scale to full height
        s_out_w = GB_WIDTH * root_h / GB_HEIGHT;
        if (s_out_w > (int)sw) { s_out_w = (int)sw; s_out_h = GB_HEIGHT * (int)sw / GB_WIDTH; }
        for (int ox = 0; ox < s_out_w; ox++) s_sx_map[ox] = (uint8_t)(ox * GB_WIDTH / s_out_w);
        for (int oy = 0; oy < s_out_h; oy++) s_sy_map[oy] = (uint8_t)(oy * GB_HEIGHT / s_out_h);
    } else {                                              // SCALE_INT2X: crisp 2x, center-cropped
        s_out_w = GB_WIDTH * 2;                           // 320 — fits width exactly
        if (s_out_w > (int)sw) s_out_w = (int)sw;
        s_out_h = (GB_HEIGHT * 2 <= root_h) ? GB_HEIGHT * 2 : root_h;
        const int voff = (GB_HEIGHT * 2 - s_out_h) / 2;   // rows cropped top/bottom
        for (int ox = 0; ox < s_out_w; ox++) { int s = ox / 2; s_sx_map[ox] = (uint8_t)(s < GB_WIDTH ? s : GB_WIDTH - 1); }
        for (int oy = 0; oy < s_out_h; oy++) { int s = (oy + voff) / 2; s_sy_map[oy] = (uint8_t)(s < GB_HEIGHT ? s : GB_HEIGHT - 1); }
    }

    s_fb = (lv_color_t*)lvglPsramAlloc((size_t)s_out_w * s_out_h * sizeof(lv_color_t));
    if (!s_fb) { GameBoy::close(); return; }

    s_canvas = lv_canvas_create(s_play);
    lv_canvas_set_buffer(s_canvas, s_fb, s_out_w, s_out_h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    if (s_scale_mode == SCALE_1X) lv_obj_set_pos(s_canvas, 2, 2);
    else                          lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    if (s_scale_mode == SCALE_1X) buildControls();

    // Single menu button (top-right) — everything else lives in the pause menu.
    s_menu_btn = lv_btn_create(s_play);
    lv_obj_set_size(s_menu_btn, 34, 24);
    lv_obj_align(s_menu_btn, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_add_event_cb(s_menu_btn, openMenuCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* ml = lv_label_create(s_menu_btn); lv_label_set_text(ml, LV_SYMBOL_LIST); lv_obj_center(ml);
}

// ---------------------------------------------------------------------------
// Phase 2: start emulating the chosen ROM
// ---------------------------------------------------------------------------
static void teardownEmu(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    audioStop();                            // frameCb is gone -> no more audio_cb
    if (s_playing) {
        gnuboy_save_state(s_sta_path);          // auto-suspend: resume where we left off
        gnuboy_save_sram(s_sav_path, false);    // battery save (no-op unless cart has one)
        s_playing = false;
    }
    // Free the emulator's banks even if we never reached play (e.g. buildPlayUI
    // OOM'd before s_playing was set) — otherwise gnuboy's rambanks/rombanks leak.
    if (s_rom_loaded) { gnuboy_free_rom(); s_rom_loaded = false; }
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

static void makeSidePath(const char* sd_path, const char* ext, char* out, size_t cap) {
    // /meshcomod/gb/x.gb -> /sd/meshcomod/gb/x<ext>  (gnuboy's fopen VFS path)
    const char* dot = strrchr(sd_path, '.');
    size_t base = dot ? (size_t)(dot - sd_path) : strlen(sd_path);
    if (base > cap - 8) base = cap - 8;
    strcpy(out, "/sd");
    memcpy(out + 3, sd_path, base);
    strcpy(out + 3 + base, ext);
}

static void startRom(const char* sd_path) {
    if (s_picker) { lv_obj_del(s_picker); s_picker = nullptr; }

    // gnuboy's fixed 160x144 framebuffer (RGB565 LE == lv_color_t); the canvas
    // buffer is separate so it can be scaled without touching the emulator.
    s_gbfb = (lv_color_t*)lvglPsramAlloc((size_t)GB_WIDTH * GB_HEIGHT * sizeof(lv_color_t));
    if (!s_gbfb) { GameBoy::close(); return; }

    size_t rom_sz = 0;
    if (!loadRomToPsram(sd_path, &s_rom, &rom_sz)) { GameBoy::close(); return; }
    makeSidePath(sd_path, ".sav", s_sav_path, sizeof(s_sav_path));
    makeSidePath(sd_path, ".sta", s_sta_path, sizeof(s_sta_path));

    if (gnuboy_init(AUDIO_RATE, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE,
                    video_cb, audio_cb) < 0) { GameBoy::close(); return; }
    gnuboy_set_framebuffer(s_gbfb);
    gnuboy_set_soundbuffer(s_abuf, ABUF_LEN);
    if (gnuboy_load_rom(s_rom, rom_sz) < 0) { gnuboy_free_rom(); GameBoy::close(); return; }
    s_rom_loaded = true;
    gnuboy_reset(true);
    // Auto-resume the prior session; on a missing/corrupt/mismatched .sta, fall
    // back to a clean reset + battery save. gnuboy_load_state mutates live RAM as
    // it reads, so a failed/partial load must be discarded by re-resetting before
    // we trust the state.
    if (gnuboy_load_state(s_sta_path) < 0) {
        gnuboy_reset(true);
        gnuboy_load_sram(s_sav_path);
    }
    gnuboy_set_mute(true);         // muted by default; the Sound menu item unmutes

    buildPlayUI();                 // muted by default -> no audioStart here
    if (!s_play) return;           // buildPlayUI failed + already closed

    s_pad_touch = 0; s_click_a = false; s_pulse_mask = 0; s_pulse_until = 0;
    s_paused = false; s_frame_count = 0;
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

static void closeCb(lv_event_t*) { GameBoy::close(); }

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
    if (!s_playing || s_paused) return;
    if (dx == 0 && dy == 0) return;
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= ady) s_pulse_mask |= (dx > 0) ? GB_PAD_RIGHT : GB_PAD_LEFT;
    else            s_pulse_mask |= (dy > 0) ? GB_PAD_DOWN  : GB_PAD_UP;
    s_pulse_until = millis() + PULSE_MS;
}

void GameBoy::setA(bool down) { s_click_a = (down && !s_paused); }

void GameBoy::keyChar(char c) {
    if (!s_playing || s_paused) return;
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
    if (s_menu) { lv_obj_del(s_menu); s_menu = nullptr; }
    if (s_root) { lv_obj_del(s_root); s_root = nullptr; }   // deletes play-UI + children
    s_picker = nullptr; s_play = nullptr; s_canvas = nullptr; s_menu_btn = nullptr;
    if (s_fb)   { lvglPsramFree(s_fb);   s_fb = nullptr; }   // after the canvas is gone
    if (s_gbfb) { lvglPsramFree(s_gbfb); s_gbfb = nullptr; }
    if (s_rom)  { free(s_rom); s_rom = nullptr; }            // banks pointed into it; free_rom left it
    s_pad_touch = 0; s_click_a = false; s_pulse_mask = 0; s_paused = false;
}

#else  // !HAS_TDECK_GT911 — no SD / input on the V4; stub the player.

void GameBoy::launch()          {}
bool GameBoy::isOpen()          { return false; }
void GameBoy::steer(int, int)   {}
void GameBoy::setA(bool)        {}
void GameBoy::keyChar(char)     {}
void GameBoy::close()           {}

#endif
