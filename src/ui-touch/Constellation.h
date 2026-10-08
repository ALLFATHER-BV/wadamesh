// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Constellation screensaver: your node at the centre, the nodes you have
// heard placed around it, and a ring wherever a packet actually came from.
//
// Every movement on it is something that happened on the radio. A ring means a
// packet arrived from that node; a dot travelling inward is that packet coming
// to you, through the repeater it took when we know it; a ring on your own node
// is you transmitting; light running inward along a route is a node heard in the
// last ten minutes. Nodes you have not heard from in a while fade, so a quiet
// night shows as a quiet screen. Beyond that only burn-in care moves: the whole
// picture turns slowly about you (a turn an hour, so north goes round and the N
// with it) and drifts a few pixels, your own node pulses, and the status changes
// corners every two minutes.
//
// Pure LVGL, and it knows nothing about MeshCore: UITask.cpp turns contacts into
// CstNode records and calls in, the same arrangement LuaAppHost uses, so the
// drawing and the mesh can change independently. Everything is drawn from lines,
// circles and labels: no images, which matters on boards near their flash limit.

#include <lvgl.h>
#include <stdint.h>

enum CstKind : uint8_t { CST_COMPANION = 0, CST_REPEATER = 1, CST_ROOM = 2 };

struct CstNode {
  uint32_t key;        // first 4 bytes of the public key: stable across rebuilds
  int16_t  bearing;    // degrees, 0 = north, clockwise (true bearing when placed by GPS)
  uint16_t dist;       // 0..1000, centre to outer ring
  uint8_t  kind;       // CstKind
  uint8_t  level;      // recency 0..3: 3 heard under 10 min ago ... 0 over 6 h
  int16_t  via;        // index of the repeater it is reached through;
                       // -1 = straight from us, -2 = path unknown (no line drawn)
  char     label[14];  // drawn when non-empty; the caller labels only a few
};

struct CstStatus {
  char clock[12];        // "21:47": digits and a colon only, the face has nothing else
  bool clock_ok;         // false: no trustworthy time; the clock is hidden, date says why
  char date[40];         // "TUE 6 OCT", or "CLOCK NOT SET · UP 2H14M" (the caller sets the case)
  int  unread;           // unread messages; 0 hides the count
  char unread_word[16];  // the word after the count: "new"
  int  heard;            // nodes heard this hour, drawn strong; -1 for no count
  char info[56];         // after the count: "nodes · heard 12 s ago"
  int  batt;             // battery percent, -1 when there is no reading
  bool charging;
  bool north_up;         // positions are true bearings: draw the N marker
};

struct CstTheme {
  uint32_t bg;
  uint32_t accent;           // range rings and links
  uint32_t glow;             // nodes, you, and the light on a route
  uint32_t text;
  uint32_t sub;
  uint32_t ter;              // the N marker
  const lv_font_t* small;    // labels and the status lines
  const lv_font_t* strong;   // the counts in them
  // Taste the rainbow: every node (its mark, its route, its rings and the packets
  // it sends you) in its own hue at this saturation and value, 0..100. Zero keeps
  // them all in the glow.
  uint8_t node_sat = 0;
  uint8_t node_val = 0;
};

void cstShow(lv_obj_t* parent, const CstTheme& theme);  // build; no-op when shown
void cstHide();                       // tear down; async delete, safe from anywhere
bool cstShown();
// Where the status text is now (it changes corners): the lowest edge of what sits in
// the top half, the highest edge of what sits in the bottom half, and which of the
// two holds the clock. False when the screensaver is not up.
bool cstHudBands(lv_coord_t* top_end, lv_coord_t* bottom_start, bool* clock_top);
void cstSetNodes(const CstNode* nodes, int count);  // relayout only when they changed
void cstSetStatus(const CstStatus& s);              // touches only what changed
void cstPing(uint32_t key);           // a packet arrived from this node
void cstPingSelf();                   // we transmitted
void cstPingFar();                    // heard something we cannot place
void cstTick(uint32_t now_ms);        // the turn, the drift, the status corners; call every loop
