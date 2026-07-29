// SPDX-License-Identifier: GPL-3.0-or-later
#include "ChannelUtil.h"
#include "i18n.h"

#include <Arduino.h>   // millis()
#include <stdio.h>     // snprintf
#include "MyMesh.h"    // extern MyMesh& the_mesh;  (+ pulls target.h -> extern radio_driver)

// The globals the rest of the UI already talks to. `the_mesh` derives from
// mesh::Dispatcher (airtime + packet-count accumulators) and `radio_driver` is
// the RadioLib wrapper (live RSSI, noise floor, RX event/error/drop counts).
extern MyMesh& the_mesh;
// radio_driver is declared `extern WRAPPER_CLASS radio_driver;` in the board's
// target.h, reached transitively through MyMesh.h above.

// Mirrors the app's status-bar height; the overlay starts below it so the bar
// stays visible (and can dismiss us on the big-screen boards). Local so this
// module needs nothing from UITask.
static constexpr lv_coord_t kTopBar = 22;
static constexpr uint32_t   kTickMs  = 1000;   // 1 Hz sample
static constexpr int        kBusyPct = 40;     // congestion threshold line / verdict pivot

// Palette (hex literals so the module is self-contained).
static constexpr uint32_t kColBg     = 0x0E1216;
static constexpr uint32_t kColText   = 0xE6EAEE;
static constexpr uint32_t kColDim    = 0x8A929A;
static constexpr uint32_t kColGood   = 0x6FCF6F;   // green   (idle / not congested)
static constexpr uint32_t kColWarn   = 0xE0B84C;   // amber   (moderate)
static constexpr uint32_t kColBusy   = 0xE0584C;   // red     (busy / congested)
static constexpr uint32_t kColUtil   = 0x35C9C9;   // chart util series (cyan)
static constexpr uint32_t kColThresh = 0x5A6166;   // chart threshold line (grey)

ChannelUtil* ChannelUtil::s_active = nullptr;

bool ChannelUtil::isOpen() { return s_active != nullptr && s_active->root_ != nullptr; }

void ChannelUtil::launch() {
  if (s_active) return;
  ChannelUtil* c = new ChannelUtil();
  s_active = c;
  if (!c->open()) { s_active = nullptr; delete c; }
}

void ChannelUtil::dismiss() {
  // Guaranteed exit from outside (a status-bar tap): the in-page X can sit under
  // a taller status bar on big-screen boards. Mirrors closeCb's teardown; safe
  // to delete synchronously here (the triggering target is the status bar).
  if (!s_active) return;
  ChannelUtil* c = s_active;
  s_active = nullptr;
  c->close();
  delete c;
}

static uint32_t utilColor(int pct) {
  if (pct >= kBusyPct)     return kColBusy;
  if (pct >= kBusyPct / 2) return kColWarn;
  return kColGood;
}

void ChannelUtil::snapshotBaseline() {
  base_ms_      = millis();
  base_rx_air_  = the_mesh.getReceiveAirTime();
  base_tx_air_  = the_mesh.getTotalAirTime();
  base_rx_pkts_ = radio_driver.getPacketsRecv();
  base_rx_err_  = radio_driver.getPacketsRecvErrors();
  base_rx_evt_  = radio_driver.getRxEvents();
  base_rx_drop_ = radio_driver.getRxQueueDrops();
  have_prev_    = false;   // windowed numbers restart too
}

void ChannelUtil::tick() {
  if (!root_ || !chart_) return;

  const uint32_t      now    = millis();
  const unsigned long rx_air = the_mesh.getReceiveAirTime();
  const unsigned long tx_air = the_mesh.getTotalAirTime();
  const uint32_t      rx_pk  = radio_driver.getPacketsRecv();
  const uint32_t      rx_err = radio_driver.getPacketsRecvErrors();
  const uint32_t      rx_evt = radio_driver.getRxEvents();
  const uint32_t      rx_drp = radio_driver.getRxQueueDrops();

  // --- Windowed (instantaneous, last tick) ---------------------------------
  int inst_util = 0, inst_rx = 0, inst_tx = 0, rate = 0;
  if (have_prev_ && now > prev_ms_) {
    const uint32_t dt   = now - prev_ms_;                 // ms of wall clock
    const uint32_t d_rx = (uint32_t)(rx_air - prev_rx_air_);
    const uint32_t d_tx = (uint32_t)(tx_air - prev_tx_air_);
    inst_rx = (int)((uint64_t)d_rx * 100 / dt);
    inst_tx = (int)((uint64_t)d_tx * 100 / dt);
    inst_util = inst_rx + inst_tx;
    if (inst_util > 100) inst_util = 100;
    if (inst_rx   > 100) inst_rx   = 100;
    if (inst_tx   > 100) inst_tx   = 100;
    rate = (int)((uint64_t)(rx_pk - prev_rx_pkts_) * 60000 / dt);
  }
  prev_ms_ = now; prev_rx_air_ = rx_air; prev_tx_air_ = tx_air;
  prev_rx_pkts_ = rx_pk; have_prev_ = true;

  // --- Session (since open / last Reset) -----------------------------------
  const uint32_t sess_ms = now - base_ms_;
  int avg_util = 0;
  if (sess_ms > 0) {
    const uint64_t busy = (uint64_t)(rx_air - base_rx_air_) + (uint64_t)(tx_air - base_tx_air_);
    avg_util = (int)(busy * 100 / sess_ms);
    if (avg_util > 100) avg_util = 100;
  }
  const uint32_t sess_rx   = rx_pk  - base_rx_pkts_;
  const uint32_t sess_err  = rx_err - base_rx_err_;
  const uint32_t sess_drop = rx_drp - base_rx_drop_;
  // Radio heard it (RX-done ISR) but we never delivered it: events - recv - err.
  // Can dip transiently negative by the 1-2 packets currently queued/unread.
  long late = (long)(rx_evt - base_rx_evt_) - (long)sess_rx - (long)sess_err;
  if (late < 0) late = 0;
  const uint32_t sess_lost = (uint32_t)late + sess_drop;

  // --- Chart ---------------------------------------------------------------
  lv_chart_set_next_value(chart_, ser_util_, (lv_coord_t)inst_util);

  // --- Headline ------------------------------------------------------------
  lv_obj_set_style_text_color(headline_, lv_color_hex(utilColor(inst_util)), LV_PART_MAIN);
  lv_label_set_text_fmt(headline_, TR("CHANNEL %d%% BUSY"), inst_util);

  // --- Signal + preset -----------------------------------------------------
  const int now_rssi = (int)radio_driver.getCurrentRSSI();
  const int nf       = radio_driver.getNoiseFloor();
  NodePrefs* np      = the_mesh.getNodePrefs();
  const int sf       = np ? (int)np->sf : 0;
  const int bw_khz   = np ? (int)np->bw : 0;
  const int cr       = np ? (int)np->cr : 0;

  const uint32_t h = sess_ms / 3600000UL;
  const uint32_t m = (sess_ms / 60000UL) % 60;
  const uint32_t s = (sess_ms / 1000UL) % 60;

  // Metrics block (recolour markup: #RRGGBB text#).
  const uint32_t lossCol = sess_lost ? kColBusy : kColGood;
  char b[420];
  snprintf(b, sizeof b,
    "Now #%06X %d%%# (RX %d / TX %d)  avg #%06X %d%%#\n"
    "RX #%06X %d/min#  %lu pkt   sig %d / nf %d\n"
    "Lost #%06X %lu# (crc %lu late %ld qf %lu)\n"
    "SF%d BW%d CR%d   run %02lu:%02lu:%02lu",
    (unsigned)utilColor(inst_util), inst_util, inst_rx, inst_tx,
    (unsigned)utilColor(avg_util), avg_util,
    (unsigned)kColUtil, rate, (unsigned long)sess_rx, now_rssi, nf,
    (unsigned)lossCol, (unsigned long)sess_lost,
    (unsigned long)sess_err, late, (unsigned long)sess_drop,
    sf, bw_khz, cr,
    (unsigned long)h, (unsigned long)m, (unsigned long)s);
  lv_label_set_text(metrics_, b);

  // --- Verdict -------------------------------------------------------------
  // Two independent axes: is the air busy, and are we losing packets. Report the
  // honest combination so a tester can copy a real conclusion, not a guess.
  const bool busy = avg_util >= kBusyPct;
  const bool losing = sess_lost > 0;
  char v[200]; uint32_t vc;
  if (!busy && !losing) {
    vc = kColGood;
    snprintf(v, sizeof v, TR("Air only %d%% busy, 0 lost - congestion unlikely."), avg_util);
  } else if (busy && losing) {
    vc = kColBusy;
    snprintf(v, sizeof v, TR("Air %d%% busy and %lu lost - congestion is plausible."), avg_util, (unsigned long)sess_lost);
  } else if (!busy && losing) {
    vc = kColWarn;
    snprintf(v, sizeof v, TR("Air only %d%% but %lu lost - not congestion (weak signal or stall?)."), avg_util, (unsigned long)sess_lost);
  } else {
    vc = kColWarn;
    snprintf(v, sizeof v, TR("Air %d%% busy but 0 lost - handling the load fine."), avg_util);
  }
  lv_obj_set_style_text_color(verdict_, lv_color_hex(vc), LV_PART_MAIN);
  lv_label_set_text(verdict_, v);

  // Re-anchor the verdict under the now-4-line metrics block (the placeholder it
  // was aligned to at open() was a single line). Layout must be current first.
  lv_obj_update_layout(metrics_);
  lv_obj_align_to(verdict_, metrics_, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
}

bool ChannelUtil::open() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
  const bool narrow = sw < 280;

  root_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root_);
  lv_obj_set_size(root_, sw, sh - kTopBar);
  lv_obj_set_pos(root_, 0, kTopBar);
  lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(root_, 6, LV_PART_MAIN);

  // Title.
  lv_obj_t* title = lv_label_create(root_);
  lv_label_set_text(title, TR("Channel Utilization"));
  lv_obj_set_style_text_color(title, lv_color_hex(kColDim), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);

  // Close (X) + Reset buttons, top-right.
  lv_obj_t* x = lv_btn_create(root_);
  lv_obj_set_size(x, 30, 24);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, this);
  lv_obj_t* xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE); lv_obj_center(xl);

  lv_obj_t* rst = lv_btn_create(root_);
  lv_obj_set_size(rst, 30, 24);
  lv_obj_align(rst, LV_ALIGN_TOP_RIGHT, -36, -2);
  lv_obj_add_event_cb(rst, resetCb, LV_EVENT_CLICKED, this);
  lv_obj_t* rl = lv_label_create(rst); lv_label_set_text(rl, LV_SYMBOL_REFRESH); lv_obj_center(rl);

  // Headline utilization %.
  headline_ = lv_label_create(root_);
  lv_obj_set_style_text_color(headline_, lv_color_hex(kColText), LV_PART_MAIN);
  lv_label_set_text(headline_, TR("CHANNEL --% BUSY"));
  lv_obj_align(headline_, LV_ALIGN_TOP_MID, 0, narrow ? 30 : 26);

  // Rolling utilization chart.
  chart_ = lv_chart_create(root_);
  const int chart_w = sw - 12;
  const int chart_h = narrow ? 46 : 50;
  const int chart_y = narrow ? 50 : 46;
  lv_obj_set_size(chart_, chart_w, chart_h);
  lv_obj_align(chart_, LV_ALIGN_TOP_MID, 0, chart_y);
  lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart_, kPoints);
  lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_size(chart_, 0, LV_PART_INDICATOR);          // no point dots
  lv_obj_set_style_bg_color(chart_, lv_color_hex(0x11161B), LV_PART_MAIN);
  lv_obj_set_style_border_width(chart_, 0, LV_PART_MAIN);
  ser_thresh_ = lv_chart_add_series(chart_, lv_color_hex(kColThresh), LV_CHART_AXIS_PRIMARY_Y);
  ser_util_   = lv_chart_add_series(chart_, lv_color_hex(kColUtil),   LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(chart_, ser_thresh_, kBusyPct);        // flat congestion reference
  lv_chart_set_all_value(chart_, ser_util_,   0);

  // Metrics block (recolour markup).
  metrics_ = lv_label_create(root_);
  lv_label_set_recolor(metrics_, true);
  lv_obj_set_style_text_color(metrics_, lv_color_hex(kColText), LV_PART_MAIN);
  lv_obj_set_width(metrics_, sw - 12);
  lv_label_set_long_mode(metrics_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(metrics_, TR("Sampling\xE2\x80\xA6"));
  lv_obj_align(metrics_, LV_ALIGN_TOP_LEFT, 2, chart_y + chart_h + 6);

  // Verdict line, anchored directly under the (fixed 4-line) metrics block so it
  // can never overlap it, whatever the board height.
  verdict_ = lv_label_create(root_);
  lv_label_set_recolor(verdict_, false);
  lv_obj_set_style_text_color(verdict_, lv_color_hex(kColDim), LV_PART_MAIN);
  lv_obj_set_width(verdict_, sw - 12);
  lv_label_set_long_mode(verdict_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(verdict_, TR("Measuring\xE2\x80\xA6 give it a few seconds of traffic."));
  lv_obj_align_to(verdict_, metrics_, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

  snapshotBaseline();
  timer_ = lv_timer_create(timerCb, kTickMs, this);
  lv_timer_ready(timer_);   // first sample right away (seeds prev_, shows preset)
  return true;
}

void ChannelUtil::close() {
  if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
  if (root_)  { lv_obj_del(root_);    root_  = nullptr; }   // deletes all children
  headline_ = nullptr; chart_ = nullptr; ser_util_ = nullptr; ser_thresh_ = nullptr;
  metrics_ = nullptr; verdict_ = nullptr;
}

// ---- LVGL C-callback trampolines (user_data = the instance) ----
void ChannelUtil::timerCb(lv_timer_t* t) {
  auto* self = static_cast<ChannelUtil*>(t->user_data);
  if (self) self->tick();
}
void ChannelUtil::resetCb(lv_event_t* e) {
  auto* self = static_cast<ChannelUtil*>(lv_event_get_user_data(e));
  if (!self) return;
  self->snapshotBaseline();
  if (self->chart_ && self->ser_util_) lv_chart_set_all_value(self->chart_, self->ser_util_, 0);
}
void ChannelUtil::closeCb(lv_event_t* e) {
  auto* self = static_cast<ChannelUtil*>(lv_event_get_user_data(e));
  if (!self) return;
  self->close();
  if (s_active == self) s_active = nullptr;
  delete self;
}
