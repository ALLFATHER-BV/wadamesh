// SPDX-License-Identifier: GPL-3.0-or-later
// Renders the Constellation screensaver (src/ui-touch/Constellation.cpp) on the
// host at one logical screen size and writes it as a 24-bit BMP, so its layout
// can be checked for every board without the board. run.sh calls this once per
// size. The scene is a fixed, plausible mesh: repeaters with nodes behind them,
// direct neighbours, a room, stale ghosts, names with emoji in them.
//
//   render <width> <height> <out.bmp> [ping]
#include "lvgl.h"
#include "../../src/ui-touch/Constellation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" const lv_font_t ui_semibold_12;   // the counts in the status lines

static int s_w = 0, s_h = 0;
static uint8_t* s_rgb = nullptr;

static void flush(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  for (int y = a->y1; y <= a->y2; ++y) {
    for (int x = a->x1; x <= a->x2; ++x) {
      const uint32_t c = lv_color_to32(*px++);
      uint8_t* o = s_rgb + ((size_t)y * s_w + x) * 3;
      o[0] = (c >> 16) & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = c & 0xFF;
    }
  }
  lv_disp_flush_ready(drv);
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) { lv_tick_inc(10); lv_timer_handler(); }
}

static bool writeBmp(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const uint32_t row = (uint32_t)((s_w * 3 + 3) & ~3), img = row * (uint32_t)s_h;
  uint8_t h[54] = {'B', 'M'};
  auto p32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  p32(2, 54 + img); p32(10, 54); p32(14, 40); p32(18, (uint32_t)s_w); p32(22, (uint32_t)s_h);
  h[26] = 1; h[28] = 24; p32(34, img);
  fwrite(h, 1, sizeof h, f);
  uint8_t pad[3] = {0, 0, 0};
  for (int y = s_h - 1; y >= 0; --y) {
    for (int x = 0; x < s_w; ++x) {
      const uint8_t* c = s_rgb + ((size_t)y * s_w + x) * 3;
      const uint8_t bgr[3] = {c[2], c[1], c[0]};
      fwrite(bgr, 1, 3, f);
    }
    fwrite(pad, 1, row - (uint32_t)s_w * 3, f);
  }
  fclose(f);
  return true;
}

static CstNode node(uint32_t key, int bearing, int dist, CstKind kind, int level, int via, const char* label) {
  CstNode n = {};
  n.key = key; n.bearing = (int16_t)bearing; n.dist = (uint16_t)dist;
  n.kind = (uint8_t)kind; n.level = (uint8_t)level; n.via = (int16_t)via;
  if (label) snprintf(n.label, sizeof n.label, "%s", label);
  return n;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: render <w> <h> <out.bmp> [ping]\n"); return 2; }
  s_w = atoi(argv[1]); s_h = atoi(argv[2]);
  const bool ping = argc > 4 && strcmp(argv[4], "ping") == 0;
  s_rgb = (uint8_t*)calloc((size_t)s_w * s_h, 3);

  lv_init();
  static lv_disp_draw_buf_t db;
  lv_color_t* buf = (lv_color_t*)malloc(sizeof(lv_color_t) * (size_t)s_w * s_h);
  lv_disp_draw_buf_init(&db, buf, nullptr, (uint32_t)(s_w * s_h));
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = (lv_coord_t)s_w; dd.ver_res = (lv_coord_t)s_h;
  dd.flush_cb = flush; dd.draw_buf = &db; dd.full_refresh = 1;
  lv_disp_drv_register(&dd);
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x202428), 0);   // what it covers

  CstTheme th = {0x000000, 0x15B6A6, 0x19D6C2, 0xE6EBED, 0x8A969C, 0x5C686E,
                 &lv_font_montserrat_12, &ui_semibold_12};
  cstShow(lv_layer_top(), th);

  // Index:            0 repeater (direct)        1 repeater (via 0)
  CstNode n[24];
  int c = 0;
  n[c++] = node(0xA1000001, 40, 330, CST_REPEATER, 3, -1, "BE-3001 RPT01");
  n[c++] = node(0xA1000002, 75, 620, CST_REPEATER, 3, 0, "GNK \xF0\x9F\x93\xA1 Hill");
  n[c++] = node(0xA1000003, 210, 420, CST_REPEATER, 2, -1, nullptr);
  n[c++] = node(0xA1000004, 300, 700, CST_REPEATER, 1, 2, nullptr);
  n[c++] = node(0xB2000001, 95, 820, CST_COMPANION, 3, 1, "Jade \xE2\x9C\xA8");
  n[c++] = node(0xB2000002, 20, 560, CST_COMPANION, 3, 0, "pisti87");
  n[c++] = node(0xB2000003, 160, 260, CST_COMPANION, 2, -1, "Kaj T-Deck");
  n[c++] = node(0xB2000004, 230, 760, CST_COMPANION, 2, 2, nullptr);
  n[c++] = node(0xB2000005, 330, 900, CST_COMPANION, 1, 3, nullptr);
  n[c++] = node(0xC3000001, 120, 680, CST_ROOM, 2, 1, "Room \xC3\xA9t\xC3\xA9");
  for (int g = 0; c < 24; ++g) {
    n[c++] = node(0xD4000000u + (uint32_t)g, (g * 47 + 13) % 360, 500 + (g * 97) % 480,
                  g % 3 == 0 ? CST_REPEATER : CST_COMPANION, g % 2, -2, nullptr);
  }
  cstSetNodes(n, c);

  CstStatus st = {};
  snprintf(st.clock, sizeof st.clock, "21:47");
  st.clock_ok = true;
  snprintf(st.date, sizeof st.date, "TUE 6 OCT");
  st.unread = 3;
  snprintf(st.unread_word, sizeof st.unread_word, "new");
  st.heard = 14;
  snprintf(st.info, sizeof st.info, "nodes \xC2\xB7 heard 12 s ago");
  st.batt = 86;
  st.charging = false;
  st.north_up = true;
  cstSetStatus(st);
  run(1200);
  if (ping) {
    cstPing(0xB2000001);   // two hops in, through both repeaters
    cstPingSelf();
    run(420);
  }
  lv_refr_now(nullptr);
  if (!writeBmp(argv[3])) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  printf("ok: %dx%d -> %s\n", s_w, s_h, argv[3]);
  return 0;
}
