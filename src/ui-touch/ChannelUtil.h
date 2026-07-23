// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Self-contained "Channel Utilization" tool launched from the Apps drawer.
//
// Purpose: give field testers a single screen that proves or DISPROVES the
// "the mesh is congested" theory for lost messages. It separates two distinct
// questions that get conflated:
//
//   1. Is the SPECTRUM actually busy?  -> channel utilization % =
//      (RX airtime + TX airtime) / wall-clock time. This is the real
//      "is the air full" number, sampled live and averaged over the session.
//
//   2. Are packets actually being LOST at the radio?  -> CRC errors
//      (collisions), packets the radio heard but the loop read too late
//      (getRxEvents - packetsRecv - recvErrors), and RX-queue drops.
//
// A mesh can be busy without losing anything, and can lose packets while the
// air is nearly idle (weak signal, SW stalls). Showing both side by side lets a
// tester report a concrete verdict instead of a hunch.
//
// Owns a full-screen overlay on lv_layer_top (mirrors SnakeGame): a title row
// with a Reset + close button, a big headline utilization %, a rolling chart of
// utilization over time, a metrics block, and a plain-language verdict line. A
// 1 Hz lv_timer samples the counters. One instance at a time. Decoupled from
// UITask internals — reaches the radio via the file-scope `the_mesh` /
// `radio_driver` globals the rest of the UI already uses.
class ChannelUtil {
public:
  static void launch();     // open (no-op if already open)
  static bool isOpen();     // UITask: gate the tab bar + raise the status bar
  static void dismiss();    // close from outside (status-bar tap) — guaranteed exit

private:
  static constexpr int kPoints = 60;   // chart history (~60 s at 1 Hz)

  lv_obj_t*          root_        = nullptr;   // full-screen overlay (owns all below)
  lv_obj_t*          headline_    = nullptr;   // big "12%" + BUSY/idle word
  lv_obj_t*          chart_       = nullptr;   // utilization % over time
  lv_chart_series_t* ser_util_    = nullptr;   // total channel util (RX+TX)
  lv_chart_series_t* ser_thresh_  = nullptr;   // flat congestion-threshold line
  lv_obj_t*          metrics_     = nullptr;   // recolour label: rate / loss / signal / preset
  lv_obj_t*          verdict_     = nullptr;   // plain-language conclusion
  lv_timer_t*        timer_       = nullptr;

  // Baselines snapshotted at open() (and re-snapshotted by Reset) so the
  // session average and loss totals are measured from a known start, without
  // touching the core's lifetime counters (other UI reads them too).
  uint32_t      base_ms_       = 0;
  unsigned long base_rx_air_   = 0;   // ms
  unsigned long base_tx_air_   = 0;   // ms
  uint32_t      base_rx_pkts_  = 0;
  uint32_t      base_rx_err_   = 0;
  uint32_t      base_rx_evt_   = 0;
  uint32_t      base_rx_drop_  = 0;

  // Previous-tick snapshot for the windowed (instantaneous) numbers.
  uint32_t      prev_ms_       = 0;
  unsigned long prev_rx_air_   = 0;
  unsigned long prev_tx_air_   = 0;
  uint32_t      prev_rx_pkts_  = 0;
  bool          have_prev_     = false;

  bool open();          // build UI; false on failure
  void close();
  void snapshotBaseline();
  void tick();          // 1 Hz sample -> chart + labels

  static ChannelUtil* s_active;   // the one live instance (or nullptr)
  static void timerCb(lv_timer_t* t);
  static void closeCb(lv_event_t* e);
  static void resetCb(lv_event_t* e);
};
