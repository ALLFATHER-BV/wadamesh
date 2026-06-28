// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Self-contained MP3 player launched from the Apps drawer (T-Deck I2S amp).
//
// Full-screen overlay on lv_layer_top: now-playing line, transport row
// (prev / play-pause / stop / next), a "Choose track" button that opens the
// File Manager (pick any .mp3 anywhere; the playlist becomes every .mp3 in that
// file's folder), and shuffle + single/all toggles. One instance at a time.
//
// Stage 1 (this file): UI + playlist navigation, audio backend stubbed.
// Stage 2 wires the decoder into audioStart/Stop/Pause and calls onTrackEnded().
class Mp3Player {
public:
  static void launch();                          // open (no-op if already open)
  static bool isOpen();
  static void onExternalPick(const char* prefPath);   // File Manager -> load .mp3
  static void closeActive();                          // dismiss overlay on tab-nav (and, later, background)
  static void stopForLock();                          // device lock / screen-off -> full stop (no decode at 80 MHz)

private:
  static constexpr int kMaxTracks = 96;
  static constexpr int kPathMax   = 160;

  lv_obj_t* root_     = nullptr;   // full-screen overlay (owns everything below)
  lv_obj_t* nowlbl_   = nullptr;   // now-playing / status text
  lv_obj_t* pp_lbl_   = nullptr;   // play/pause glyph
  lv_obj_t* shuf_     = nullptr;   // shuffle toggle button (state-styled)
  lv_obj_t* mode_lbl_ = nullptr;   // "All" / "Single" label
  lv_obj_t* vol_lbl_  = nullptr;   // "Vol NN" indicator
  lv_timer_t* tick_   = nullptr;   // polls end-of-track from the decode task
  lv_obj_t* settings_ = nullptr;   // gear panel overlay (nullptr = closed)

  char tracks_[kMaxTracks][kPathMax];   // pref paths ("sd:/.../x.mp3" | "/x.mp3")
  int  count_   = 0;
  int  cur_     = -1;
  bool playing_ = false;
  bool paused_  = false;
  bool shuffle_ = false;
  bool single_  = false;           // false = play all; true = stop after one
  int  vol_     = 70;              // 0..100

  bool open();
  void close();
  void reopen();   // rebuild the UI on a backgrounded instance (no audio restart)
  void buildUI();
  void refresh();
  void scanFolder(const char* anyTrackPref);   // flat scan of that file's folder
  void loadFromExternal(const char* prefPath);
  void play();
  void pause();
  void stop();
  void next();
  void prev();
  void volUp();
  void volDown();
  int  pickNextIndex() const;
  void onTrackEnded();             // single -> stop; all -> next (stage 2 hook)

  // ---- audio backend (stage 2: real decoder); stubs for now ----
  void audioStart(const char* path);
  void audioStop();
  void audioPause(bool p);
  void audioSetVolume(int v);

  static Mp3Player* s_active;
  static void closeCb(lv_event_t* e);
  static void playPauseCb(lv_event_t* e);
  static void stopCb(lv_event_t* e);
  static void nextCb(lv_event_t* e);
  static void prevCb(lv_event_t* e);
  static void chooseCb(lv_event_t* e);
  static void shuffleCb(lv_event_t* e);
  static void modeCb(lv_event_t* e);
  static void volUpCb(lv_event_t* e);
  static void volDownCb(lv_event_t* e);
  static void tickCb(lv_timer_t* t);
  static void settingsCb(lv_event_t* e);
  static void bgToggleCb(lv_event_t* e);   // "play in background" switch
};

// Cross-module helpers (defined in Mp3Player.cpp, T-Deck only). The notification
// chime calls mp3OwnsI2S() to stay silent while the player holds I2S0 (music
// priority); the status bar reads mp3StatusTime() to show elapsed/total time and
// offer tap-to-open.
bool mp3OwnsI2S();
bool mp3StatusTime(int& pos_sec);
bool uiSoundMuted();              // defined in UITask.cpp: master "Sound" off (buzzer_quiet) -> player stays silent
