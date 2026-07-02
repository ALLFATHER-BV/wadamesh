// SPDX-License-Identifier: GPL-3.0-or-later
#include "Mp3Player.h"
#include <driver/i2s.h>          // I2S amp — install with the firmware-matched config
#include "../helpers/esp32/TouchPrefsStore.h"   // remember last track (blob prefs)

#if defined(HAS_TDECK_GT911)   // player needs the I2S amp + File Manager (T-Deck)

#include <Arduino.h>
#include <SD.h>
#include <SPIFFS.h>
#include <string.h>

#include "AudioGeneratorMP3.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceSPIFFS.h"
#include "AudioOutput.h"
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// ---------------------------------------------------------------------------
// Streaming source with a BACKGROUND FILL TASK.
// Every SD read happens on a dedicated producer task that pours bytes into a
// large PSRAM ring (FreeRTOS StreamBuffer). The decode task only ever pulls
// from the ring, so an SPI-busy window (incoming LoRa packet, display flush)
// stalls just the producer -- the decoder keeps draining the ring and never
// underruns. Works for files of ANY size with a fixed ring, and removes the
// long whole-file load (playback starts as soon as the first chunk lands).
// ---------------------------------------------------------------------------
class AudioFileSourcePSRAMRing : public AudioFileSource {
 public:
  AudioFileSourcePSRAMRing(AudioFileSource* under, uint8_t* storage, uint32_t storageBytes)
      : under_(under) {
    size_ = under_ ? under_->getSize() : 0;
    if (storage && storageBytes > 64)
      ring_ = xStreamBufferCreateStatic(storageBytes - 1, 1, storage, &ringCtl_);
    if (ring_ && under_)
      xTaskCreatePinnedToCore(&AudioFileSourcePSRAMRing::fillThunk, "mp3fill", 8192, this, 1, &task_, 0);
  }
  ~AudioFileSourcePSRAMRing() override {
    stop_ = true;
    uint32_t t0 = millis();
    while (task_ && !exited_ && (millis() - t0) < 1500) vTaskDelay(pdMS_TO_TICKS(5));   // join the producer
    if (under_) { under_->close(); delete under_; under_ = nullptr; }
  }
  uint32_t read(void* data, uint32_t len) override {            // consumer = decode task; never touches SD
    if (!ring_) return 0;
    uint8_t* p = (uint8_t*)data; uint32_t got = 0;
    while (got < len) {
      size_t r = xStreamBufferReceive(ring_, p + got, len - got, pdMS_TO_TICKS(200));
      got += (uint32_t)r;
      if (r == 0) {                                             // nothing new for 200 ms
        if (stop_) break;
        if (eof_ && xStreamBufferBytesAvailable(ring_) == 0) break;   // genuine end of file
      }
    }
    consumed_ += got;
    return got;
  }
  uint32_t readNonBlock(void* data, uint32_t len) override {
    if (!ring_) return 0;
    uint32_t r = (uint32_t)xStreamBufferReceive(ring_, data, len, 0);
    consumed_ += r; return r;
  }
  bool seek(int32_t, int) override { return false; }            // playback is forward-only
  bool close() override { return true; }
  bool isOpen() override { return ring_ != nullptr; }
  uint32_t getSize() override { return size_; }
  uint32_t getPos() override { return consumed_; }
  bool ok() const { return ring_ != nullptr && task_ != nullptr; }

 private:
  static void fillThunk(void* a) { static_cast<AudioFileSourcePSRAMRing*>(a)->fill(); }
  void fill() {
    uint8_t tmp[2048];
    while (!stop_) {
      int n = under_ ? (int)under_->read(tmp, sizeof tmp) : 0;   // BLOCKS on SD/SPI -- only this task waits
      if (n <= 0) { eof_ = true; break; }
      uint32_t sent = 0;
      while (sent < (uint32_t)n && !stop_)
        sent += (uint32_t)xStreamBufferSend(ring_, tmp + sent, (uint32_t)n - sent, pdMS_TO_TICKS(100));
    }
    exited_ = true;
    vTaskDelete(nullptr);
  }
  AudioFileSource*     under_    = nullptr;
  StreamBufferHandle_t ring_     = nullptr;
  StaticStreamBuffer_t ringCtl_;
  TaskHandle_t         task_     = nullptr;
  volatile bool        stop_     = false;
  volatile bool        eof_      = false;
  volatile bool        exited_   = false;
  uint32_t             size_     = 0;
  uint32_t             consumed_ = 0;
};

// Opens the File Manager so the user can pick an .mp3 (defined in UITask.cpp).
extern void mp3OpenFilePicker();

// Local palette (decoupled from UITask's theme constants, like SnakeGame).
static constexpr uint32_t kColBg     = 0x000000;
static constexpr uint32_t kColText   = 0xE0E3E6;
static constexpr uint32_t kColSub    = 0x828891;
static constexpr uint32_t kColAccent = 0x15B6A6;
static constexpr uint32_t kColBtn    = 0x222A30;

static constexpr lv_coord_t kStatusH = 24;   // keep the system status bar visible

// I2S amp pins (T-Deck): BCLK / LRCLK(WS) / DOUT on port 0 — same as the system
// chime path in UITask. The player is T-Deck-only (file guarded above).
static constexpr int kI2sBck = 7, kI2sWs = 5, kI2sDout = 6, kI2sPortNum = 0;
static bool s_i2s_installed = false;   // our own install state, so doInstall skips a no-op uninstall

// Custom ESP8266Audio sink. We install the legacy I2S driver with the SAME
// i2s_config the firmware's chime path uses (intr_alloc_flags=0, 4x256 DMA, no
// event queue). ESP8266Audio's own AudioOutputI2S uses a different config that
// — on this T-Deck build — left the touch input wedged after the very first
// install (clicks stopped registering everywhere but Map/Control-Center). The
// firmware config is proven not to do that. Mono mix to the MAX98357A.
class TdeckMp3Out : public AudioOutput {
 public:
  bool doInstall(int rate) {              // (re)install the driver fresh at `rate`
    if (s_i2s_installed) i2s_driver_uninstall((i2s_port_t)kI2sPortNum);   // guarded: nothing to remove on a fresh port
    installed_ = false; ofill_ = 0; played_ = 0;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = rate > 0 ? rate : 44100;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;     // MAX98357A is mono
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 8;     // deeper DMA cushion vs message-handling CPU spikes
    cfg.dma_buf_len = 512;     // 8*512 = ~93 ms @44.1k (was 4*256 = ~23 ms)
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    if (i2s_driver_install((i2s_port_t)kI2sPortNum, &cfg, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE; pins.bck_io_num = kI2sBck;
    pins.ws_io_num = kI2sWs; pins.data_out_num = kI2sDout; pins.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin((i2s_port_t)kI2sPortNum, &pins) != ESP_OK) {
      i2s_driver_uninstall((i2s_port_t)kI2sPortNum); s_i2s_installed = false; return false;
    }
    i2s_zero_dma_buffer((i2s_port_t)kI2sPortNum);
    inst_rate_ = cfg.sample_rate;
    installed_ = true; s_i2s_installed = true;
    return true;
  }
  bool begin() override { return installed_ ? true : doInstall(hertz > 0 ? hertz : 44100); }
  bool SetRate(int hz) override {
    hertz = hz;
    // Do NOT use i2s_set_sample_rates — on this legacy/mono config it leaves the
    // output silent. Reinstall the driver fresh only when the rate actually
    // changes; same-rate (the common 44.1 kHz case) needs no reconfigure at all.
    if (hz > 0 && (!installed_ || hz != inst_rate_)) doInstall(hz);
    return true;
  }
  bool SetBitsPerSample(int b) override { bps = b; return true; }
  bool SetChannels(int c) override { channels = c; return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    if (ofill_ >= kOBuf && !flushBuf()) return false;   // buffer full + DMA busy -> retry same sample
    int32_t mono = ((int32_t)sample[0] + (int32_t)sample[1]) >> 1;
    mono = (mono * (int32_t)gainF2P6) >> 6;   // apply gain (2.6 fixed-point)
    if (mono >  32767) mono =  32767;
    if (mono < -32768) mono = -32768;
    obuf_[ofill_++] = (int16_t)mono; played_++;
    return true;
  }
  bool stop() override {            // generator calls this on EOF / song switch
    if (installed_) { flushBuf(); i2s_zero_dma_buffer((i2s_port_t)kI2sPortNum); }
    ofill_ = 0;
    return true;                    // KEEP the driver installed (install-once model)
  }
  void shutdown() {                 // explicit teardown — only when the player closes
    if (installed_) {
      i2s_zero_dma_buffer((i2s_port_t)kI2sPortNum);
      i2s_driver_uninstall((i2s_port_t)kI2sPortNum);
      installed_ = false; s_i2s_installed = false;
    }
    ofill_ = 0;
  }
  uint32_t played() const { return played_; }
  int      rate()   const { return inst_rate_; }
  void     resetPlayed() { played_ = 0; }
 private:
  bool flushBuf() {
    if (!ofill_) return true;
    size_t bw = 0;
    esp_err_t e = i2s_write((i2s_port_t)kI2sPortNum, obuf_, ofill_ * sizeof(int16_t), &bw, 0);   // non-blocking: lets gen->loop() return so the task re-reads stop/pause/volume
    size_t wrote = bw / sizeof(int16_t);
    if (e != ESP_OK || wrote == 0) return false;
    if (wrote < ofill_) { memmove(obuf_, obuf_ + wrote, (ofill_ - wrote) * sizeof(int16_t)); ofill_ -= (int)wrote; return false; }
    ofill_ = 0;
    return true;
  }
  static constexpr int kOBuf = 256;
  int16_t obuf_[kOBuf];
  int     ofill_ = 0;
  int     inst_rate_ = 0;
  volatile uint32_t played_ = 0;
  bool    installed_ = false;
};


// Audio engine state. One persistent decode task owns the decoder/source/output
// for the whole player session; the UI thread talks to it only through these
// flags. audioStart spawns it once (and feeds new tracks via s_a_newcmd);
// audioStop tears it down. End-of-track is surfaced via s_a_ended, polled by an
// lv_timer on the UI thread (LVGL is single-threaded).
static TaskHandle_t       s_audio_task = nullptr;
static volatile bool      s_a_stop    = false;   // tear the persistent task down
static volatile bool      s_a_paused  = false;
static volatile bool      s_a_ended   = false;   // set by the task at natural EOF
static volatile bool      s_a_newcmd  = false;   // UI set a new track in s_a_path
static volatile float     s_a_gain    = 0.7f;
static char               s_a_path[200] = {0};   // track to open (UI sets, task copies)
static volatile int       s_pos_sec   = 0;     // elapsed seconds (task -> UI)
static bool               s_bg_pref   = false; // "play in background" (persisted in prefs)

static void mp3AudioTask(void*) {
  // Persistent decode task for the whole player session: install I2S ONCE, then
  // load tracks on demand (s_a_newcmd + s_a_path). No per-song task spawn and no
  // I2S re-install — that churn caused first-track silence and a use-after-free
  // crash when switching tracks quickly (two tasks racing the SD/I2S libs).
  TdeckMp3Out out;
  out.SetGain(s_a_gain);
  if (!out.begin()) { s_audio_task = nullptr; vTaskDelete(nullptr); return; }
  AudioGeneratorMP3* gen = nullptr;
  AudioFileSource*   src = nullptr;
  // Ring storage: allocate ONCE for the whole program (function-local static survives task
  // restarts) so it never re-fragments, and stay well under the ~4MB PSRAM bank ceiling that
  // made the old 4MB alloc fail by ~1KB. 1MB ring ~= 60s of audio cushion -> ample.
  static uint8_t* s_ring_buf = nullptr;
  static uint32_t s_ring_sz  = 0;
  if (!s_ring_buf) {
    const uint32_t cand[] = { 1024u * 1024, 512u * 1024, 256u * 1024 };
    for (uint32_t want : cand) {
      s_ring_buf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
      if (s_ring_buf) { s_ring_sz = want; break; }
    }
  }
  uint8_t*       psbuf    = s_ring_buf;
  const uint32_t PSBUF_SZ = s_ring_sz;
  char cur[200] = {0};
  while (!s_a_stop) {
    if (s_a_newcmd) {                            // UI requested a new track
      s_a_newcmd = false;
      s_pos_sec = 0; out.resetPlayed();   // zero elapsed for the new track (install-once: doInstall may not re-run)
      if (gen) { if (gen->isRunning()) gen->stop(); delete gen; gen = nullptr; }
      if (src) { delete src; src = nullptr; }   // psbuf is reused across tracks; only freed on task exit
      strncpy(cur, (const char*)s_a_path, sizeof cur - 1); cur[sizeof cur - 1] = '\0';
      if (cur[0]) {
        const bool sd = !strncmp(cur, "sd:", 3);
        const char* fp = sd ? cur + 3 : cur;
        AudioFileSource* under = sd ? (AudioFileSource*)new AudioFileSourceSD(fp)
                                    : (AudioFileSource*)new AudioFileSourceSPIFFS(fp);
        bool ring = false;
        if (psbuf) {                                 // stream through the PSRAM ring with a background fill task
          AudioFileSourcePSRAMRing* r = new AudioFileSourcePSRAMRing(under, psbuf, PSBUF_SZ);
          if (r->ok()) { src = r; ring = true; }
          else { delete r; under = nullptr; }        // r's dtor already closed+freed `under`
        }
        if (!ring) {                                 // fallback: decode straight off SD (no ring)
          if (!under) under = sd ? (AudioFileSource*)new AudioFileSourceSD(fp)
                                 : (AudioFileSource*)new AudioFileSourceSPIFFS(fp);
          src = under;
        }
        gen = new AudioGeneratorMP3();
        out.SetGain(s_a_gain);
        if (!gen->begin(src, &out)) {            // bad/unsupported file
          delete gen; gen = nullptr; delete src; src = nullptr;
          s_a_ended = true;
        }
      }
      continue;
    }
    if (gen && gen->isRunning() && !s_a_paused) {
      out.SetGain(s_a_gain);                     // live volume
      { int r = out.rate(); uint32_t pl = out.played();
        int ps = r > 0 ? (int)(pl / (uint32_t)r) : 0;     // elapsed seconds for this track
        if (ps != s_pos_sec) s_pos_sec = ps;              // advance once per second
      }
      if (!gen->loop()) {                        // natural end of track
        gen->stop(); delete gen; gen = nullptr;
        delete src; src = nullptr;
        s_a_ended = true;
      }
      vTaskDelay(1);                             // yield (touch poll + idle/WDT)
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));             // idle or paused
    }
  }
  if (gen) { if (gen->isRunning()) gen->stop(); delete gen; gen = nullptr; }
  if (src) { delete src; src = nullptr; }
  // s_ring_buf is intentionally kept allocated for the program lifetime (reused on every relaunch)
  out.shutdown();                                // uninstall I2S once, on the way out
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

Mp3Player* Mp3Player::s_active = nullptr;

bool Mp3Player::isOpen() { return s_active != nullptr && s_active->root_ != nullptr; }

void Mp3Player::launch() {
  if (s_active) {                 // an instance already exists
    if (s_active->root_) return;  //   foreground -> nothing to do
    s_active->reopen();           //   background -> rebuild UI on the live object (no restart)
    return;
  }
  Mp3Player* p = new Mp3Player();
  if (!p->open()) { delete p; return; }
  s_active = p;
}

// ---- helpers ---------------------------------------------------------------

static bool mp3IsMp3Name(const char* name) {
  if (!name) return false;
  const char* dot = strrchr(name, '.');
  return dot && !strcasecmp(dot, ".mp3");
}
static const char* mp3Base(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}
static void mp3StyleBtn(lv_obj_t* b, uint32_t bg) {
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
}

// ---- UI --------------------------------------------------------------------

bool Mp3Player::open() {
  { uint8_t b = 0; if (touchPrefsGetBlob("mp3bg", &b, 1) > 0) s_bg_pref = (b != 0); }   // load "play in background"
  vol_ = (int)touchPrefsGetSoundVolume();   // mirror the system volume for the "Vol NN" label
  buildUI();
  // Restore the last played track: select + show it (does not auto-play).
  { char last[kPathMax];
    size_t n = touchPrefsGetBlob("mp3last", (uint8_t*)last, sizeof last - 1);
    if (n > 0) {
      if (n >= sizeof last) n = sizeof last - 1;
      last[n] = '\0';
      if (last[0]) {
        scanFolder(last);                      // `last` is the remembered folder
        cur_ = (count_ > 0) ? 0 : -1;           // first track in catalog order
      }
    } }
  refresh();
  return root_ != nullptr;
}

void Mp3Player::buildUI() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);

  root_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root_);
  lv_obj_set_size(root_, sw, sh - kStatusH);
  lv_obj_set_pos(root_, 0, kStatusH);
  lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(root_, LV_DIR_VER);   // safety net on short screens

  // title
  lv_obj_t* title = lv_label_create(root_);
  lv_label_set_text(title, LV_SYMBOL_AUDIO "  Music");
  lv_obj_set_style_text_color(title, lv_color_hex(kColAccent), LV_PART_MAIN);
  lv_obj_set_pos(title, 10, 6);

  // close (X)
  lv_obj_t* x = lv_btn_create(root_);
  lv_obj_set_size(x, 30, 26);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -6, 2);
  mp3StyleBtn(x, kColBtn);
  lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, this);
  lv_obj_t* xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE); lv_obj_center(xl);

  // settings (gear) — sits between the volume indicator and the X
  lv_obj_t* g = lv_btn_create(root_);
  lv_obj_set_size(g, 30, 26);
  lv_obj_align(g, LV_ALIGN_TOP_RIGHT, -40, 2);
  mp3StyleBtn(g, kColBtn);
  lv_obj_add_event_cb(g, settingsCb, LV_EVENT_CLICKED, this);
  lv_obj_t* gl = lv_label_create(g); lv_label_set_text(gl, LV_SYMBOL_SETTINGS); lv_obj_center(gl);

  // volume indicator (top bar, left of the gear)
  vol_lbl_ = lv_label_create(root_);
  lv_obj_align(vol_lbl_, LV_ALIGN_TOP_RIGHT, -78, 8);
  lv_obj_set_style_text_color(vol_lbl_, lv_color_hex(kColSub), LV_PART_MAIN);

  // now-playing line
  nowlbl_ = lv_label_create(root_);
  lv_label_set_long_mode(nowlbl_, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nowlbl_, sw - 20);
  lv_obj_set_pos(nowlbl_, 10, 34);
  lv_obj_set_style_text_color(nowlbl_, lv_color_hex(kColText), LV_PART_MAIN);

  // transport row: prev / play-pause / stop / next  (compact to fit 240px)
  const lv_coord_t bw = (sw - 20 - 3 * 8) / 4;
  const lv_coord_t by = 62, bh = 46;
  struct { const char* sym; lv_event_cb_t cb; } tr[4] = {
    { LV_SYMBOL_PREV, prevCb }, { LV_SYMBOL_PLAY, playPauseCb },
    { LV_SYMBOL_STOP, stopCb }, { LV_SYMBOL_NEXT, nextCb },
  };
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* b = lv_btn_create(root_);
    lv_obj_set_size(b, bw, bh);
    lv_obj_set_pos(b, 10 + i * (bw + 8), by);
    mp3StyleBtn(b, i == 1 ? kColAccent : kColBtn);
    lv_obj_add_event_cb(b, tr[i].cb, LV_EVENT_CLICKED, this);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, tr[i].sym);
    lv_obj_center(l);
    if (i == 1) pp_lbl_ = l;
  }

  // choose-track (smaller) + volume -/+ on one row
  const lv_coord_t cy = by + bh + 10;
  const lv_coord_t volw = 44;
  const lv_coord_t chw  = sw - 20 - 2 * (volw + 8);
  lv_obj_t* ch = lv_btn_create(root_);
  lv_obj_set_size(ch, chw, 36);
  lv_obj_set_pos(ch, 10, cy);
  mp3StyleBtn(ch, kColBtn);
  lv_obj_add_event_cb(ch, chooseCb, LV_EVENT_CLICKED, this);
  lv_obj_t* chl = lv_label_create(ch);
  lv_label_set_text(chl, LV_SYMBOL_DIRECTORY "  Choose");
  lv_obj_center(chl);

  lv_obj_t* vd = lv_btn_create(root_);
  lv_obj_set_size(vd, volw, 36);
  lv_obj_set_pos(vd, 10 + chw + 8, cy);
  mp3StyleBtn(vd, kColBtn);
  lv_obj_add_event_cb(vd, volDownCb, LV_EVENT_CLICKED, this);
  lv_obj_t* vdl = lv_label_create(vd); lv_label_set_text(vdl, LV_SYMBOL_MINUS); lv_obj_center(vdl);

  lv_obj_t* vu = lv_btn_create(root_);
  lv_obj_set_size(vu, volw, 36);
  lv_obj_set_pos(vu, 10 + chw + 8 + volw + 8, cy);
  mp3StyleBtn(vu, kColBtn);
  lv_obj_add_event_cb(vu, volUpCb, LV_EVENT_CLICKED, this);
  lv_obj_t* vul = lv_label_create(vu); lv_label_set_text(vul, LV_SYMBOL_PLUS); lv_obj_center(vul);

  // shuffle + mode toggles (two halves)
  const lv_coord_t ty = cy + 36 + 10;
  const lv_coord_t tw = (sw - 20 - 8) / 2;
  shuf_ = lv_btn_create(root_);
  lv_obj_set_size(shuf_, tw, 36);
  lv_obj_set_pos(shuf_, 10, ty);
  mp3StyleBtn(shuf_, kColBtn);
  lv_obj_add_event_cb(shuf_, shuffleCb, LV_EVENT_CLICKED, this);
  lv_obj_t* sl = lv_label_create(shuf_);
  lv_label_set_text(sl, LV_SYMBOL_SHUFFLE "  Shuffle");
  lv_obj_center(sl);

  lv_obj_t* md = lv_btn_create(root_);
  lv_obj_set_size(md, tw, 36);
  lv_obj_set_pos(md, 10 + tw + 8, ty);
  mp3StyleBtn(md, kColBtn);
  lv_obj_add_event_cb(md, modeCb, LV_EVENT_CLICKED, this);
  mode_lbl_ = lv_label_create(md);
  lv_obj_center(mode_lbl_);

  if (!tick_) tick_ = lv_timer_create(tickCb, 250, this);   // poll end-of-track; one timer, survives background
}

void Mp3Player::refresh() {
  if (nowlbl_) {
    if (cur_ < 0 || count_ == 0) {
      lv_label_set_text(nowlbl_, "No track - tap Choose track");
      lv_obj_set_style_text_color(nowlbl_, lv_color_hex(kColSub), LV_PART_MAIN);
    } else {
      const char* state = !playing_ ? "Stopped" : (paused_ ? "Paused" : "Playing");
      lv_label_set_text_fmt(nowlbl_, "%s (%d/%d): %s", state, cur_ + 1, count_, mp3Base(tracks_[cur_]));
      lv_obj_set_style_text_color(nowlbl_, lv_color_hex(kColText), LV_PART_MAIN);
    }
  }
  if (pp_lbl_)
    lv_label_set_text(pp_lbl_, (playing_ && !paused_) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  if (shuf_)
    lv_obj_set_style_bg_color(shuf_, lv_color_hex(shuffle_ ? kColAccent : kColBtn), LV_PART_MAIN);
  if (mode_lbl_)
    lv_label_set_text(mode_lbl_, single_ ? LV_SYMBOL_LOOP "  Single" : LV_SYMBOL_LOOP "  All");
  if (vol_lbl_)
    lv_label_set_text_fmt(vol_lbl_, "Vol %d", vol_);
}

// ---- File-Manager pick -> playlist (folder of the chosen file) -------------

void Mp3Player::scanFolder(const char* anyPref) {
  count_ = 0;
  if (!anyPref || !anyPref[0]) { cur_ = -1; return; }
  const bool sd = !strncmp(anyPref, "sd:", 3);
  const char* p = sd ? anyPref + 3 : anyPref;             // absolute path (folder or file)
  fs::FS& fsx = sd ? (fs::FS&)SD : (fs::FS&)SPIFFS;

  char dir[kPathMax];
  strncpy(dir, p, sizeof dir - 1); dir[sizeof dir - 1] = '\0';
  { File probe = fsx.open(dir);                            // given a folder? use it directly
    const bool isdir = probe && probe.isDirectory();
    if (probe) probe.close();
    if (!isdir) {                                          // otherwise treat as a file -> take its parent
      char* slash = strrchr(dir, '/');
      if (slash) { if (slash == dir) dir[1] = '\0'; else *slash = '\0'; }
      else       { strcpy(dir, "/"); }
    }
  }
  File d = fsx.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); count_ = 0; cur_ = -1; return; }   // no card / folder gone -> empty, never crash
  File e = d.openNextFile();
  while (e && count_ < kMaxTracks) {
    if (!e.isDirectory()) {
      const char* base = mp3Base(e.name());
      if (mp3IsMp3Name(base)) {
        const bool root = !strcmp(dir, "/");
        if (sd) snprintf(tracks_[count_], kPathMax, root ? "sd:/%s" : "sd:%s/%s",
                         root ? base : dir, base);
        else    snprintf(tracks_[count_], kPathMax, root ? "/%s" : "%s/%s",
                         root ? base : dir, base);
        count_++;
      }
    }
    e.close();
    e = d.openNextFile();
  }
  if (e) e.close();
  d.close();
}

void Mp3Player::loadFromExternal(const char* prefPath) {
  if (!prefPath || !prefPath[0]) return;
  scanFolder(prefPath);
  cur_ = (count_ > 0) ? 0 : -1;
  for (int i = 0; i < count_; ++i)
    if (!strcmp(tracks_[i], prefPath)) { cur_ = i; break; }
  if (root_) lv_obj_move_foreground(root_);
  if (cur_ >= 0) play();
  else           refresh();
}

void Mp3Player::onExternalPick(const char* prefPath) {
  if (!isOpen()) launch();
  if (isOpen()) s_active->loadFromExternal(prefPath);
}

// ---- transport / playlist --------------------------------------------------

void Mp3Player::play() {
  if (count_ == 0 || cur_ < 0) return;
  if (uiSoundMuted()) return;            // master "Sound" is off -> do not start (toggling it off also stops playback)
  audioStart(tracks_[cur_]);
  playing_ = true; paused_ = false;
  refresh();
}
void Mp3Player::pause() {
  if (!playing_) return;
  paused_ = !paused_; audioPause(paused_); refresh();
}
void Mp3Player::stopForLock() {                  // called from the screen-off path; full stop so the CPU can idle at 80 MHz
  if (s_active) s_active->stop();
}
void Mp3Player::stop() {
  audioStop(); playing_ = false; paused_ = false; refresh();
}
int Mp3Player::pickNextIndex() const {
  if (count_ <= 1) return 0;
  if (shuffle_) { int n = (int)random(count_); if (n == cur_) n = (n + 1) % count_; return n; }
  return (cur_ + 1) % count_;
}
void Mp3Player::next() { if (count_ == 0) return; cur_ = pickNextIndex(); play(); }
void Mp3Player::prev() { if (count_ == 0) return; cur_ = (cur_ - 1 + count_) % count_; play(); }
void Mp3Player::onTrackEnded() { if (single_) stop(); else next(); }
void Mp3Player::volUp()   { vol_ = (int)touchPrefsGetSoundVolume() + 10; if (vol_ > 100) vol_ = 100; touchPrefsSetSoundVolume((uint8_t)vol_); audioSetVolume(vol_); refresh(); }
void Mp3Player::volDown() { vol_ = (int)touchPrefsGetSoundVolume() - 10; if (vol_ < 0)   vol_ = 0;   touchPrefsSetSoundVolume((uint8_t)vol_); audioSetVolume(vol_); refresh(); }

// ---- audio backend stubs (stage 2 fills these) -----------------------------

void Mp3Player::audioStart(const char* prefpath) {
  if (!prefpath || !prefpath[0]) return;
  strncpy(s_a_path, prefpath, sizeof s_a_path - 1);
  s_a_path[sizeof s_a_path - 1] = '\0';
  s_a_gain   = (float)touchPrefsGetSoundVolume() / 100.0f;   // single system volume (shared with notifications)
  s_a_paused = false;
  s_a_ended  = false;
  s_a_newcmd = true;                             // running task picks up the new track
  { char fold[200]; strncpy(fold, prefpath, sizeof fold - 1); fold[sizeof fold - 1] = '\0';   // remember the FOLDER (reopen starts at its first track)
    char* sl = strrchr(fold, '/');
    if (sl) *sl = '\0';
    if (!fold[0])                  strcpy(fold, "/");        // SPIFFS root
    else if (!strcmp(fold, "sd:")) strcpy(fold, "sd:/");     // SD root
    touchPrefsSetBlob("mp3last", (const uint8_t*)fold, strlen(fold) + 1); }
  if (!s_audio_task) {                           // first play / after stop: spawn the persistent task
    s_a_stop = false;
    // Core 0 (UI/LVGL on core 1), priority 1 — one below the touch poll task.
    xTaskCreatePinnedToCore(mp3AudioTask, "mp3dec", 40960, nullptr, 1, &s_audio_task, 0);
  }
}void Mp3Player::audioStop() {
  if (!s_audio_task) return;
  s_a_stop = true;
  for (int i = 0; i < 400 && *(TaskHandle_t volatile*)&s_audio_task; ++i) vTaskDelay(pdMS_TO_TICKS(5));
  s_a_stop = false;
  s_pos_sec = 0;
}
void Mp3Player::audioPause(bool p)    { s_a_paused = p; }
void Mp3Player::audioSetVolume(int v) { s_a_gain = (float)v / 100.0f; }

// --- cross-module helpers (declared in Mp3Player.h) ---
bool mp3OwnsI2S() { return s_audio_task != nullptr; }   // decode task alive => we hold I2S0
bool mp3StatusTime(int& pos_sec) {
  if (!s_audio_task) return false;
  pos_sec = s_pos_sec;
  return true;
}

// ---- lifecycle / callbacks -------------------------------------------------

void Mp3Player::close() {
  if (s_bg_pref && s_audio_task) {                 // background: keep the decode task + tick timer, drop only the UI
    if (root_) { lv_obj_del(root_); root_ = nullptr; }
    nowlbl_ = pp_lbl_ = shuf_ = mode_lbl_ = vol_lbl_ = settings_ = nullptr;   // children gone with root_; null so refresh() no-ops
    return;                                        // object + tick_ stay alive; status-bar time is the way back
  }
  audioStop();
  if (tick_) { lv_timer_del(tick_); tick_ = nullptr; }
  if (root_) { lv_obj_del(root_); root_ = nullptr; }
  if (s_active == this) s_active = nullptr;
  delete this;
}

void Mp3Player::reopen() {
  buildUI();    // recreate UI (tick_ already alive, so not duplicated)
  refresh();    // repaint from the live state (track, play/pause, volume, mode...)
}

void Mp3Player::closeActive() { if (s_active) s_active->close(); }
void Mp3Player::closeCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->close();
}

void Mp3Player::settingsCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto* self = static_cast<Mp3Player*>(lv_event_get_user_data(e));
  if (!self || !self->root_) return;
  if (self->settings_) { lv_obj_del(self->settings_); self->settings_ = nullptr; return; }   // toggle closed
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  lv_obj_t* p = lv_obj_create(self->root_);
  self->settings_ = p;
  lv_obj_remove_style_all(p);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(p, sw - 40, 116);
  lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_style_bg_color(p, lv_color_hex(kColBtn), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(p, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(p, 10, LV_PART_MAIN);
  lv_obj_t* t = lv_label_create(p);
  lv_label_set_text(t, LV_SYMBOL_SETTINGS "  Settings");
  lv_obj_set_style_text_color(t, lv_color_hex(kColAccent), LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t* c = lv_btn_create(p);
  lv_obj_set_size(c, 30, 26);
  lv_obj_align(c, LV_ALIGN_TOP_RIGHT, 0, -4);
  mp3StyleBtn(c, kColBg);
  lv_obj_add_event_cb(c, settingsCb, LV_EVENT_CLICKED, self);
  lv_obj_t* cl = lv_label_create(c); lv_label_set_text(cl, LV_SYMBOL_CLOSE); lv_obj_center(cl);
  lv_obj_t* bgsw = lv_switch_create(p);
  lv_obj_align(bgsw, LV_ALIGN_TOP_LEFT, 0, 36);
  if (s_bg_pref) lv_obj_add_state(bgsw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(bgsw, bgToggleCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* bgl = lv_label_create(p);
  lv_label_set_text(bgl, "Background play");
  lv_obj_set_style_text_color(bgl, lv_color_hex(kColText), LV_PART_MAIN);
  lv_obj_align_to(bgl, bgsw, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
}

void Mp3Player::bgToggleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  s_bg_pref = on;
  uint8_t b = on ? 1 : 0;
  touchPrefsSetBlob("mp3bg", &b, 1);
}
void Mp3Player::playPauseCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto* self = static_cast<Mp3Player*>(lv_event_get_user_data(e));
  if (!self->playing_) self->play(); else self->pause();
}
void Mp3Player::stopCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->stop();
}
void Mp3Player::nextCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->next();
}
void Mp3Player::prevCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->prev();
}
void Mp3Player::chooseCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  mp3OpenFilePicker();
}
void Mp3Player::shuffleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto* self = static_cast<Mp3Player*>(lv_event_get_user_data(e));
  self->shuffle_ = !self->shuffle_; self->refresh();
}
void Mp3Player::modeCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto* self = static_cast<Mp3Player*>(lv_event_get_user_data(e));
  self->single_ = !self->single_; self->refresh();
}
void Mp3Player::volUpCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->volUp();
}
void Mp3Player::volDownCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  static_cast<Mp3Player*>(lv_event_get_user_data(e))->volDown();
}

void Mp3Player::tickCb(lv_timer_t* t) {
  auto* self = static_cast<Mp3Player*>(t->user_data);
  if (s_a_ended) {                          // a track finished on its own
    s_a_ended = false;
    if (self->playing_ && !self->paused_) self->onTrackEnded();   // single->stop, all->next
  }
}

#endif  // HAS_TDECK_GT911
