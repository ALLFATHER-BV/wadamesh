// SPDX-License-Identifier: GPL-3.0-or-later
// Lua app host — see LuaAppHost.h + LUA_APPS.md. Structure:
//   1. PSRAM-capped allocator + instruction-budget hook (containment)
//   2. the wada.* bindings (ui/input/timer/sys/store) — API v1
//   3. lifecycle: launch -> run chunk -> callbacks via guarded pcall -> dismiss
#include "LuaAppHost.h"

#if CAP_LUA_APPS
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include "AppPage.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// UITask-owned services the host borrows (all pre-existing, fwd-declared here
// to keep this TU decoupled from the 47k-line UITask.cpp).
extern const lv_font_t* luaHostFontForSize(int size_class);       // 12/14/16 -> g_font_*
extern void             luaHostToast(const char* msg, int ms);    // showAlert passthrough
extern fs::FS*          luaHostAppFs();                           // /apps storage root FS (may be null)
extern void             luaHostAppPath(char* out, size_t cap, const char* rel);   // prefixes the store root
extern int  luaHostContactAt(int idx, char* name, size_t name_cap, int* type, uint32_t* secs_ago);
extern int  luaHostRxLogAt(int idx, uint32_t* ms_ago, int* ptype, int* rssi, float* snr, int* hops);
extern void luaHostRadioStats(float* rssi, float* noise, uint32_t* rx_air_s, uint32_t* tx_air_s,
                              uint32_t* rx_pkts, uint32_t* rx_err, int* budget_ms);
extern void luaHostRadioStats2(uint32_t* rx_evt, uint32_t* rx_drop, uint32_t* tx_pkts,
                               float* freq, float* bw, int* sf, int* duty_pct);
extern void luaHostSelfInfo(char* name, size_t name_cap, double* lat, double* lon);
class WiFiClient; class HTTPClient;
extern int  luaStoreHttpGet(WiFiClient& client, HTTPClient& http, const char* url, char* buf, size_t cap);

// ---------------------------------------------------------------------------
// 1) allocator + budget
// ---------------------------------------------------------------------------
namespace {

struct LuaHeap { size_t used = 0, peak = 0, cap = 0; };

void* psAllocCb(void* ud, void* ptr, size_t osize, size_t nsize) {
  LuaHeap* h = (LuaHeap*)ud;
  if (!ptr) osize = 0;
  if (nsize == 0) {
    if (ptr) { h->used -= osize; heap_caps_free(ptr); }
    return nullptr;
  }
  if (h->used - osize + nsize > h->cap) return nullptr;   // hard per-app cap -> Lua OOM error
  void* np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!np) return nullptr;
  h->used += nsize - osize;
  if (h->used > h->peak) h->peak = h->used;
  return np;
}

// 100k VM instructions per callback (~<2 ms of straight-line Lua on the S3 —
// the Phase 0 spike measured containment of a hostile loop in 30 ms at this
// cadence with negligible overhead on real code).
constexpr int kInstrBudget = 100000;

void budgetHook(lua_State* L, lua_Debug*) {
  luaL_error(L, "instruction budget exceeded (app tick too long)");
}

// ---------------------------------------------------------------------------
// host state
// ---------------------------------------------------------------------------
constexpr size_t kHeapCap     = 256 * 1024;   // per-app PSRAM cap
constexpr size_t kMaxSrc      = 64 * 1024;    // app source size limit
constexpr size_t kStoreMax    = 2048;         // per-app persisted KV budget (bytes, serialized)
constexpr int    kMinTickMs   = 33;           // fastest on_tick cadence (~30 fps)

struct Host {
  lua_State*  L = nullptr;
  LuaHeap     heap;
  lv_obj_t*   root = nullptr;        // full-screen overlay on lv_layer_top
  lv_obj_t*   body = nullptr;        // app content area (below the tall bar)
  lv_timer_t* timer = nullptr;
  int         body_w = 0, body_h = 0;   // set at creation — lv_obj_get_width() reads 0 pre-layout
  uint32_t    last_tick = 0;
  int         ref_app   = LUA_NOREF; // the table the chunk returned
  int         ref_btncb = LUA_NOREF; // { [btn lightuserdata] = fn } button callbacks
  bool        store_dirty = false;
  bool        in_lua = false;        // re-entrancy guard (dismiss from inside a callback)
  bool        want_close = false;
  char        id[24]    = "";
  char        title[32] = "";
};
Host* s_h = nullptr;

char s_bar_title[40];   // appPageBegin keeps the pointer — must outlive the page

// ---------------------------------------------------------------------------
// guarded callback invocation
// ---------------------------------------------------------------------------
int tracebackMsgh(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
  return 1;
}

// Push app.<name>; returns false if the app has no such callback.
bool pushCallback(Host* h, const char* name) {
  lua_rawgeti(h->L, LUA_REGISTRYINDEX, h->ref_app);
  lua_getfield(h->L, -1, name);
  lua_remove(h->L, -2);
  if (!lua_isfunction(h->L, -1)) { lua_pop(h->L, 1); return false; }
  return true;
}

// Run the function at the top of the stack with nargs args under budget+pcall.
// On error: toast + schedule close. Returns true on clean completion.
bool guardedCall(Host* h, int nargs) {
  lua_State* L = h->L;
  int base = lua_gettop(L) - nargs;            // function slot
  lua_pushcfunction(L, tracebackMsgh);
  lua_insert(L, base);
  lua_sethook(L, budgetHook, LUA_MASKCOUNT, kInstrBudget);   // (re)arms the counter
  h->in_lua = true;
  int rc = lua_pcall(L, nargs, 0, base);
  h->in_lua = false;
  lua_sethook(L, nullptr, 0, 0);
  lua_remove(L, base);                          // the traceback handler
  if (rc != LUA_OK) {
    const char* err = lua_tostring(L, -1);
    Serial.printf("[LUAAPP] %s error: %s\n", h->id, err ? err : "?");
    char msg[96];
    snprintf(msg, sizeof msg, "App error: %.60s", err ? err : "unknown");
    luaHostToast(msg, 2600);
    lua_pop(L, 1);
    h->want_close = true;                       // unwind AFTER we leave Lua
    return false;
  }
  return true;
}

void serviceDeferredClose() {
  if (s_h && s_h->want_close && !s_h->in_lua) luaAppDismiss();
}

// ---------------------------------------------------------------------------
// 2) wada.* bindings
// ---------------------------------------------------------------------------
uint32_t argColor(lua_State* L, int idx, uint32_t def = 0xFFFFFF) {
  return (uint32_t)luaL_optinteger(L, idx, (lua_Integer)def) & 0xFFFFFF;
}

// ---- canvas userdata ----
struct CanvasUd { lv_obj_t* obj; lv_color_t* buf; int w, h; };

CanvasUd* checkCanvas(lua_State* L) {
  return (CanvasUd*)luaL_checkudata(L, 1, "wada.canvas");
}

int cvFill(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (c->obj) lv_canvas_fill_bg(c->obj, lv_color_hex(argColor(L, 2, 0x000000)), LV_OPA_COVER);
  return 0;
}
int cvRect(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (!c->obj) return 0;
  lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
  uint32_t col = argColor(L, 6);
  bool filled = lua_isnoneornil(L, 7) ? true : lua_toboolean(L, 7);
  d.bg_color = lv_color_hex(col); d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
  d.border_color = lv_color_hex(col); d.border_width = filled ? 0 : 1; d.border_opa = LV_OPA_COVER;
  d.radius = (lv_coord_t)luaL_optinteger(L, 8, 0);
  lv_canvas_draw_rect(c->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3),
                      (lv_coord_t)luaL_checkinteger(L, 4), (lv_coord_t)luaL_checkinteger(L, 5), &d);
  return 0;
}
int cvLine(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (!c->obj) return 0;
  lv_point_t pts[2] = {
    { (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3) },
    { (lv_coord_t)luaL_checkinteger(L, 4), (lv_coord_t)luaL_checkinteger(L, 5) } };
  lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(argColor(L, 6));
  d.width = (lv_coord_t)luaL_optinteger(L, 7, 1);
  lv_canvas_draw_line(c->obj, pts, 2, &d);
  return 0;
}
int cvCircle(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (!c->obj) return 0;
  lv_coord_t x = (lv_coord_t)luaL_checkinteger(L, 2), y = (lv_coord_t)luaL_checkinteger(L, 3);
  lv_coord_t r = (lv_coord_t)luaL_checkinteger(L, 4);
  lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
  d.color = lv_color_hex(argColor(L, 5));
  bool filled = lua_isnoneornil(L, 6) ? true : lua_toboolean(L, 6);
  d.width = filled ? r : (lv_coord_t)luaL_optinteger(L, 7, 1);
  lv_canvas_draw_arc(c->obj, x, y, r, 0, 360, &d);
  return 0;
}
int cvText(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (!c->obj) return 0;
  lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
  d.color = lv_color_hex(argColor(L, 5));
  d.font = luaHostFontForSize((int)luaL_optinteger(L, 6, 14));
  lv_canvas_draw_text(c->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3),
                      c->w, &d, luaL_checkstring(L, 4));
  return 0;
}
int cvPos(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (c->obj) lv_obj_set_pos(c->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int cvGc(lua_State* L) {
  // The buffer is heap_caps memory owned by the userdata; the lv object dies
  // with the app root. Free the pixels only when Lua collects the handle AND
  // the app is closing (root gone) — during app lifetime the canvas holds it.
  CanvasUd* c = (CanvasUd*)luaL_checkudata(L, 1, "wada.canvas");
  if (c->buf) { heap_caps_free(c->buf); c->buf = nullptr; }
  c->obj = nullptr;
  return 0;
}

int uiCanvas(lua_State* L) {
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  int w = (int)luaL_checkinteger(L, 1), h = (int)luaL_checkinteger(L, 2);
  luaL_argcheck(L, w > 0 && w <= 480, 1, "width 1..480");
  luaL_argcheck(L, h > 0 && h <= 480, 2, "height 1..480");
  size_t bytes = (size_t)w * h * sizeof(lv_color_t);
  lv_color_t* buf = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) return luaL_error(L, "canvas alloc failed");
  lv_obj_t* cv = lv_canvas_create(s_h->body);
  lv_canvas_set_buffer(cv, buf, w, h, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(cv, lv_color_hex(0x000000), LV_OPA_COVER);
  CanvasUd* ud = (CanvasUd*)lua_newuserdatauv(L, sizeof(CanvasUd), 0);
  ud->obj = cv; ud->buf = buf; ud->w = w; ud->h = h;
  luaL_setmetatable(L, "wada.canvas");
  return 1;
}

// ---- label userdata ----
struct WidgetUd { lv_obj_t* obj; };
WidgetUd* checkLabel(lua_State* L) { return (WidgetUd*)luaL_checkudata(L, 1, "wada.label"); }

int lbSet(lua_State* L) {
  WidgetUd* u = checkLabel(L);
  if (u->obj) lv_label_set_text(u->obj, luaL_checkstring(L, 2));
  return 0;
}
int lbPos(lua_State* L) {
  WidgetUd* u = checkLabel(L);
  if (u->obj) lv_obj_set_pos(u->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int lbColor(lua_State* L) {
  WidgetUd* u = checkLabel(L);
  if (u->obj) lv_obj_set_style_text_color(u->obj, lv_color_hex(argColor(L, 2)), LV_PART_MAIN);
  return 0;
}

int uiLabel(lua_State* L) {
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  lv_obj_t* l = lv_label_create(s_h->body);
  lv_label_set_text(l, luaL_checkstring(L, 1));
  lv_obj_set_pos(l, (lv_coord_t)luaL_optinteger(L, 2, 0), (lv_coord_t)luaL_optinteger(L, 3, 0));
  lv_obj_set_style_text_font(l, luaHostFontForSize((int)luaL_optinteger(L, 4, 14)), LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(argColor(L, 5, 0xE6E9ED)), LV_PART_MAIN);
  WidgetUd* ud = (WidgetUd*)lua_newuserdatauv(L, sizeof(WidgetUd), 0);
  ud->obj = l;
  luaL_setmetatable(L, "wada.label");
  return 1;
}

// ---- chart userdata (the primitive the native Monitor/Airtime pages use) ----
struct ChartUd { lv_obj_t* obj; lv_chart_series_t* ser[2]; int n_ser; };
ChartUd* checkChart(lua_State* L) { return (ChartUd*)luaL_checkudata(L, 1, "wada.chart"); }

int chPush(lua_State* L) {   // chart:push(series_idx, value)
  ChartUd* c = checkChart(L);
  int si = (int)luaL_checkinteger(L, 2) - 1;
  if (!c->obj || si < 0 || si >= c->n_ser) return 0;
  lv_chart_set_next_value(c->obj, c->ser[si], (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int chSetAll(lua_State* L) {  // chart:fill(series_idx, value) — reset a series
  ChartUd* c = checkChart(L);
  int si = (int)luaL_checkinteger(L, 2) - 1;
  if (!c->obj || si < 0 || si >= c->n_ser) return 0;
  lv_chart_set_all_value(c->obj, c->ser[si], (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int chRange(lua_State* L) {   // chart:range(min, max)
  ChartUd* c = checkChart(L);
  if (c->obj) lv_chart_set_range(c->obj, LV_CHART_AXIS_PRIMARY_Y,
                                 (lv_coord_t)luaL_checkinteger(L, 2),
                                 (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int chPos(lua_State* L) {
  ChartUd* c = checkChart(L);
  if (c->obj) lv_obj_set_pos(c->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}

int uiChart(lua_State* L) {   // wada.ui.chart(w, h, points, color1 [, color2] [, bars])
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  int w = (int)luaL_checkinteger(L, 1), h = (int)luaL_checkinteger(L, 2);
  int pts = (int)luaL_optinteger(L, 3, 60);
  if (pts < 2) pts = 2;
  if (pts > 240) pts = 240;
  lv_obj_t* ch = lv_chart_create(s_h->body);
  lv_obj_set_size(ch, w, h);
  lv_chart_set_type(ch, lua_toboolean(L, 6) ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ch, pts);
  lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(ch, 3, 0);
  lv_obj_set_style_size(ch, 0, LV_PART_INDICATOR);          // no point dots
  lv_obj_set_style_bg_color(ch, lv_color_hex(0x101418), LV_PART_MAIN);
  lv_obj_set_style_border_width(ch, 0, LV_PART_MAIN);
  lv_obj_set_style_line_color(ch, lv_color_hex(0x2A3038), LV_PART_ITEMS);
  ChartUd* ud = (ChartUd*)lua_newuserdatauv(L, sizeof(ChartUd), 0);
  ud->obj = ch;
  ud->n_ser = 0;
  ud->ser[0] = lv_chart_add_series(ch, lv_color_hex(argColor(L, 4, 0x15B6A6)), LV_CHART_AXIS_PRIMARY_Y);
  ud->n_ser = 1;
  if (!lua_isnoneornil(L, 5)) {
    ud->ser[1] = lv_chart_add_series(ch, lv_color_hex(argColor(L, 5, 0x7A7F87)), LV_CHART_AXIS_PRIMARY_Y);
    ud->n_ser = 2;
  }
  luaL_setmetatable(L, "wada.chart");
  return 1;
}

// ---- buttons: Lua callback dispatched through the guarded path ----
void btnEventCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_h || !s_h->L) return;
  lua_State* L = s_h->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, s_h->ref_btncb);
  lua_pushlightuserdata(L, lv_event_get_target(e));
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (lua_isfunction(L, -1)) guardedCall(s_h, 0);
  else lua_pop(L, 1);
  serviceDeferredClose();
}

int uiButton(lua_State* L) {
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  const char* txt = luaL_checkstring(L, 1);
  lv_obj_t* b = lv_btn_create(s_h->body);
  lv_obj_set_pos(b, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3));
  lv_obj_set_size(b, (lv_coord_t)luaL_optinteger(L, 4, 90), (lv_coord_t)luaL_optinteger(L, 5, 34));
  lv_obj_t* bl = lv_label_create(b);
  lv_label_set_text(bl, txt);
  lv_obj_center(bl);
  if (lua_isfunction(L, 6)) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_h->ref_btncb);
    lua_pushlightuserdata(L, b);
    lua_pushvalue(L, 6);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lv_obj_add_event_cb(b, btnEventCb, LV_EVENT_CLICKED, nullptr);
  }
  WidgetUd* ud = (WidgetUd*)lua_newuserdatauv(L, sizeof(WidgetUd), 0);
  ud->obj = b;
  luaL_setmetatable(L, "wada.label");   // shares set/pos/color methods
  return 1;
}

// ---- sys / timer / store ----
int sysMillis(lua_State* L) { lua_pushinteger(L, (lua_Integer)millis()); return 1; }
int sysToast(lua_State* L)  { luaHostToast(luaL_checkstring(L, 1), (int)luaL_optinteger(L, 2, 1500)); return 0; }
int sysRandom(lua_State* L) {
  // esp_random-backed: apps should not have to seed math.random for games
  lua_Integer lo = luaL_optinteger(L, 1, 0), hi = luaL_optinteger(L, 2, 0);
  if (hi <= lo) { lua_pushinteger(L, (lua_Integer)esp_random()); return 1; }
  lua_pushinteger(L, lo + (lua_Integer)(esp_random() % (uint32_t)(hi - lo + 1)));
  return 1;
}
int sysBoard(lua_State* L) {
  lua_createtable(L, 0, 6);
  lua_pushinteger(L, s_h ? s_h->body_w : lv_disp_get_hor_res(nullptr));
  lua_setfield(L, -2, "w");
  lua_pushinteger(L, s_h ? s_h->body_h : lv_disp_get_ver_res(nullptr));
  lua_setfield(L, -2, "h");
#if CAP_TOUCH
  lua_pushboolean(L, 1);
#else
  lua_pushboolean(L, 0);
#endif
  lua_setfield(L, -2, "touch");
  return 1;
}

void tickTimerCb(lv_timer_t* t) {
  (void)t;
  if (!s_h || !s_h->L) return;
  uint32_t now = millis();
  uint32_t dt = s_h->last_tick ? now - s_h->last_tick : 0;
  s_h->last_tick = now;
  if (pushCallback(s_h, "on_tick")) {
    lua_pushinteger(s_h->L, (lua_Integer)dt);
    guardedCall(s_h, 1);
  }
  serviceDeferredClose();
}

int timerEvery(lua_State* L) {
  if (!s_h) return 0;
  int ms = (int)luaL_checkinteger(L, 1);
  if (ms < kMinTickMs) ms = kMinTickMs;
  if (s_h->timer) lv_timer_set_period(s_h->timer, ms);
  else            s_h->timer = lv_timer_create(tickTimerCb, ms, nullptr);
  return 0;
}
int timerStop(lua_State* L) {
  (void)L;
  if (s_h && s_h->timer) { lv_timer_del(s_h->timer); s_h->timer = nullptr; s_h->last_tick = 0; }
  return 0;
}

// Store: a Lua table mirrored to /apps/<id>.sav as "k=v" lines (strings and
// numbers only). Loaded at launch, flushed on dismiss when dirty — tiny, rare,
// and off the hot path (touch_perf: never spam flash from the UI thread).
int storeGet(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "wada.storetab");
  lua_pushvalue(L, 1);
  lua_rawget(L, -2);
  if (lua_isnil(L, -1) && !lua_isnoneornil(L, 2)) { lua_pop(L, 1); lua_pushvalue(L, 2); }
  return 1;
}
int storeSet(lua_State* L) {
  luaL_checkstring(L, 1);
  if (!lua_isnumber(L, 2) && !lua_isstring(L, 2) && !lua_isnil(L, 2))
    return luaL_error(L, "store values must be strings or numbers");
  lua_getfield(L, LUA_REGISTRYINDEX, "wada.storetab");
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_rawset(L, -3);
  if (s_h) s_h->store_dirty = true;
  return 0;
}

void storePath(char* out, size_t cap) {
  char rel[40];
  snprintf(rel, sizeof rel, "/apps/%s.sav", s_h->id);
  luaHostAppPath(out, cap, rel);
}

void storeLoad() {
  lua_State* L = s_h->L;
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "wada.storetab");
  fs::FS* fs = luaHostAppFs();
  if (!fs) return;
  char path[64]; storePath(path, sizeof path);
  File f = fs->open(path, "r");
  if (!f) return;
  lua_getfield(L, LUA_REGISTRYINDEX, "wada.storetab");
  char line[192];
  size_t n;
  while ((n = f.readBytesUntil('\n', line, sizeof line - 1)) > 0) {
    line[n] = 0;
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    lua_pushstring(L, line);
    lua_Number num;
    char* end = nullptr;
    num = strtod(eq + 1, &end);
    if (end && *end == 0 && end != eq + 1) lua_pushnumber(L, num);
    else                                   lua_pushstring(L, eq + 1);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
  f.close();
}

void storeFlush() {
  if (!s_h || !s_h->store_dirty) return;
  fs::FS* fs = luaHostAppFs();
  if (!fs) return;
  lua_State* L = s_h->L;
  char path[64]; storePath(path, sizeof path);
  char appsdir[48]; luaHostAppPath(appsdir, sizeof appsdir, "/apps");
  fs->mkdir(appsdir);
  File f = fs->open(path, "w");
  if (!f) return;
  size_t written = 0;
  lua_getfield(L, LUA_REGISTRYINDEX, "wada.storetab");
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (lua_type(L, -2) == LUA_TSTRING && (lua_isstring(L, -1) || lua_isnumber(L, -1))) {
      char lbuf[192];
      int m = snprintf(lbuf, sizeof lbuf, "%s=%s\n", lua_tostring(L, -2), luaL_tolstring(L, -1, nullptr));
      lua_pop(L, 1);   // luaL_tolstring's copy
      if (m > 0 && written + (size_t)m <= kStoreMax) { f.write((const uint8_t*)lbuf, m); written += m; }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  f.close();
  s_h->store_dirty = false;
}

// ---- input plumbing ----
void sendInput(const char* type, const char* dir, int x, int y) {
  if (!s_h || !s_h->L) return;
  if (!pushCallback(s_h, "on_input")) return;
  lua_State* L = s_h->L;
  lua_createtable(L, 0, 4);
  lua_pushstring(L, type); lua_setfield(L, -2, "type");
  if (dir) { lua_pushstring(L, dir); lua_setfield(L, -2, "dir"); }
  lua_pushinteger(L, x); lua_setfield(L, -2, "x");
  lua_pushinteger(L, y); lua_setfield(L, -2, "y");
  guardedCall(s_h, 1);
  serviceDeferredClose();
}

void gestureCb(lv_event_t* e) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  const char* dir = (d == LV_DIR_TOP) ? "up" : (d == LV_DIR_BOTTOM) ? "down"
                  : (d == LV_DIR_LEFT) ? "left" : (d == LV_DIR_RIGHT) ? "right" : nullptr;
  if (dir) sendInput("swipe", dir, 0, 0);
  (void)e;
}

void pressCb(lv_event_t* e) {
  lv_indev_t* indev = lv_indev_get_act();
  lv_point_t p{0, 0};
  if (indev) lv_indev_get_point(indev, &p);
  lv_area_t a;
  lv_obj_get_coords(s_h->body, &a);
  sendInput(lv_event_get_code(e) == LV_EVENT_PRESSED ? "down" : "up", nullptr,
            p.x - a.x1, p.y - a.y1);
}

// ---- wada.mesh (read-only) ----
int meshContacts(lua_State* L) {
  lua_newtable(L);
  char name[36]; int type; uint32_t ago;
  for (int i = 0, out = 0; i < 200 && out < 100; i++) {
    if (!luaHostContactAt(i, name, sizeof name, &type, &ago)) break;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, name);          lua_setfield(L, -2, "name");
    lua_pushinteger(L, type);         lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)ago); lua_setfield(L, -2, "ago_s");
    lua_rawseti(L, -2, ++out);
  }
  return 1;
}
int meshRxLog(lua_State* L) {
  lua_newtable(L);
  uint32_t ms_ago; int ptype, rssi, hops; float snr;
  for (int i = 0, out = 0; i < 64; i++) {
    if (!luaHostRxLogAt(i, &ms_ago, &ptype, &rssi, &snr, &hops)) break;
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)ms_ago); lua_setfield(L, -2, "ago_ms");
    lua_pushinteger(L, ptype);               lua_setfield(L, -2, "type");
    lua_pushinteger(L, rssi);                lua_setfield(L, -2, "rssi");
    lua_pushnumber(L, snr);                  lua_setfield(L, -2, "snr");
    lua_pushinteger(L, hops);                lua_setfield(L, -2, "hops");
    lua_rawseti(L, -2, ++out);
  }
  return 1;
}
int meshStats(lua_State* L) {
  float rssi, noise; uint32_t rx_air, tx_air, rx_pkts, rx_err; int budget;
  luaHostRadioStats(&rssi, &noise, &rx_air, &tx_air, &rx_pkts, &rx_err, &budget);
  lua_createtable(L, 0, 7);
  lua_pushnumber(L, rssi);                  lua_setfield(L, -2, "rssi");
  lua_pushnumber(L, noise);                 lua_setfield(L, -2, "noise");
  lua_pushinteger(L, (lua_Integer)rx_air);  lua_setfield(L, -2, "rx_air_s");
  lua_pushinteger(L, (lua_Integer)tx_air);  lua_setfield(L, -2, "tx_air_s");
  lua_pushinteger(L, (lua_Integer)rx_pkts); lua_setfield(L, -2, "rx_pkts");
  lua_pushinteger(L, (lua_Integer)rx_err);  lua_setfield(L, -2, "rx_err");
  lua_pushinteger(L, budget);               lua_setfield(L, -2, "tx_budget_ms");
  uint32_t rx_evt, rx_drop, tx_pkts; float freq, bw; int sf, duty;
  luaHostRadioStats2(&rx_evt, &rx_drop, &tx_pkts, &freq, &bw, &sf, &duty);
  lua_pushinteger(L, (lua_Integer)rx_evt);  lua_setfield(L, -2, "rx_events");
  lua_pushinteger(L, (lua_Integer)rx_drop); lua_setfield(L, -2, "rx_dropped");
  lua_pushinteger(L, (lua_Integer)tx_pkts); lua_setfield(L, -2, "tx_pkts");
  lua_pushnumber(L, freq);                  lua_setfield(L, -2, "freq");
  lua_pushnumber(L, bw);                    lua_setfield(L, -2, "bw");
  lua_pushinteger(L, sf);                   lua_setfield(L, -2, "sf");
  lua_pushinteger(L, duty);                 lua_setfield(L, -2, "duty_pct");
  return 1;
}
int meshSelf(lua_State* L) {
  char name[36]; double lat, lon;
  luaHostSelfInfo(name, sizeof name, &lat, &lon);
  lua_createtable(L, 0, 3);
  lua_pushstring(L, name);  lua_setfield(L, -2, "name");
  lua_pushnumber(L, lat);   lua_setfield(L, -2, "lat");
  lua_pushnumber(L, lon);   lua_setfield(L, -2, "lon");
  return 1;
}

// ---- wada.net: one in-flight async http_get per app ----
constexpr size_t kNetCapMax = 32 * 1024;
char     s_net_url[160];
char*    s_net_buf = nullptr;
size_t   s_net_cap = 0;
int      s_net_result = -1;
volatile bool s_net_pending = false;   // worker should run it
volatile bool s_net_done = false;      // result ready for the UI thread
int      s_net_cb = LUA_NOREF;
lv_timer_t* s_net_poll = nullptr;

void netDeliver(lv_timer_t* t) {
  (void)t;
  if (!s_net_done) return;
  s_net_done = false;
  if (s_net_poll) { lv_timer_del(s_net_poll); s_net_poll = nullptr; }
  if (!s_h || !s_h->L || s_net_cb == LUA_NOREF) { s_net_cb = LUA_NOREF; return; }
  lua_State* L = s_h->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, s_net_cb);
  luaL_unref(L, LUA_REGISTRYINDEX, s_net_cb);
  s_net_cb = LUA_NOREF;
  lua_pushinteger(L, s_net_result >= 0 ? 200 : -1);
  if (s_net_result >= 0 && s_net_buf) lua_pushlstring(L, s_net_buf, (size_t)s_net_result);
  else                                lua_pushnil(L);
  guardedCall(s_h, 2);
  serviceDeferredClose();
}

int netHttpGet(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  size_t maxb = (size_t)luaL_optinteger(L, 3, 16 * 1024);
  if (maxb > kNetCapMax) maxb = kNetCapMax;
  if (strncmp(url, "http://", 7) != 0) return luaL_error(L, "http:// urls only");
  if (strlen(url) >= sizeof(s_net_url)) return luaL_error(L, "url too long");
  if (s_net_pending || s_net_cb != LUA_NOREF) return luaL_error(L, "a fetch is already running");
  if (!s_net_buf || s_net_cap < maxb + 1) {
    if (s_net_buf) heap_caps_free(s_net_buf);
    s_net_buf = (char*)heap_caps_malloc(maxb + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_net_cap = s_net_buf ? maxb + 1 : 0;
    if (!s_net_buf) return luaL_error(L, "no memory for the fetch buffer");
  }
  snprintf(s_net_url, sizeof s_net_url, "%s", url);
  lua_pushvalue(L, 2);
  s_net_cb = luaL_ref(L, LUA_REGISTRYINDEX);
  s_net_done = false;
  s_net_pending = true;
  if (!s_net_poll) s_net_poll = lv_timer_create(netDeliver, 120, nullptr);
  return 0;
}

// ---- module registration ----
void openWada(lua_State* L) {
  lua_newtable(L);                                       // wada

  lua_newtable(L);                                       // wada.ui
  lua_pushcfunction(L, uiCanvas); lua_setfield(L, -2, "canvas");
  lua_pushcfunction(L, uiLabel);  lua_setfield(L, -2, "label");
  lua_pushcfunction(L, uiButton); lua_setfield(L, -2, "button");
  lua_pushcfunction(L, uiChart);  lua_setfield(L, -2, "chart");
  lua_createtable(L, 0, 6);                              // wada.ui.colors (theme)
  lua_pushinteger(L, 0x15B6A6); lua_setfield(L, -2, "accent");
  lua_pushinteger(L, 0xE6E9ED); lua_setfield(L, -2, "text");
  lua_pushinteger(L, 0x7A7F87); lua_setfield(L, -2, "sub");
  lua_pushinteger(L, 0x0E1216); lua_setfield(L, -2, "bg");
  lua_pushinteger(L, 0xD7574E); lua_setfield(L, -2, "bad");
  lua_pushinteger(L, 0x53C06B); lua_setfield(L, -2, "good");
  lua_setfield(L, -2, "colors");
  lua_setfield(L, -2, "ui");

  lua_newtable(L);                                       // wada.sys
  lua_pushcfunction(L, sysMillis); lua_setfield(L, -2, "millis");
  lua_pushcfunction(L, sysToast);  lua_setfield(L, -2, "toast");
  lua_pushcfunction(L, sysBoard);  lua_setfield(L, -2, "board");
  lua_pushcfunction(L, sysRandom); lua_setfield(L, -2, "random");
  lua_setfield(L, -2, "sys");

  lua_newtable(L);                                       // wada.timer
  lua_pushcfunction(L, timerEvery); lua_setfield(L, -2, "every");
  lua_pushcfunction(L, timerStop);  lua_setfield(L, -2, "stop");
  lua_setfield(L, -2, "timer");

  lua_newtable(L);                                       // wada.store
  lua_pushcfunction(L, storeGet); lua_setfield(L, -2, "get");
  lua_pushcfunction(L, storeSet); lua_setfield(L, -2, "set");
  lua_setfield(L, -2, "store");

  lua_newtable(L);                                       // wada.mesh (read-only)
  lua_pushcfunction(L, meshContacts); lua_setfield(L, -2, "contacts");
  lua_pushcfunction(L, meshRxLog);    lua_setfield(L, -2, "rx_log");
  lua_pushcfunction(L, meshStats);    lua_setfield(L, -2, "stats");
  lua_pushcfunction(L, meshSelf);     lua_setfield(L, -2, "self");
  lua_setfield(L, -2, "mesh");

  lua_newtable(L);                                       // wada.net
  lua_pushcfunction(L, netHttpGet); lua_setfield(L, -2, "http_get");
  lua_setfield(L, -2, "net");

  lua_setglobal(L, "wada");

  // metatables
  luaL_newmetatable(L, "wada.canvas");
  lua_newtable(L);
  lua_pushcfunction(L, cvFill);   lua_setfield(L, -2, "fill");
  lua_pushcfunction(L, cvRect);   lua_setfield(L, -2, "rect");
  lua_pushcfunction(L, cvLine);   lua_setfield(L, -2, "line");
  lua_pushcfunction(L, cvCircle); lua_setfield(L, -2, "circle");
  lua_pushcfunction(L, cvText);   lua_setfield(L, -2, "text");
  lua_pushcfunction(L, cvPos);    lua_setfield(L, -2, "pos");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, cvGc);     lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, "wada.chart");
  lua_newtable(L);
  lua_pushcfunction(L, chPush);   lua_setfield(L, -2, "push");
  lua_pushcfunction(L, chSetAll); lua_setfield(L, -2, "fill");
  lua_pushcfunction(L, chRange);  lua_setfield(L, -2, "range");
  lua_pushcfunction(L, chPos);    lua_setfield(L, -2, "pos");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, "wada.label");
  lua_newtable(L);
  lua_pushcfunction(L, lbSet);   lua_setfield(L, -2, "set");
  lua_pushcfunction(L, lbPos);   lua_setfield(L, -2, "pos");
  lua_pushcfunction(L, lbColor); lua_setfield(L, -2, "color");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

void openSandbox(lua_State* L) {
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);        lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);  lua_pop(L, 1);
  static const char* const kill[] = { "dofile", "loadfile", "load", "require", "collectgarbage" };
  for (const char* k : kill) { lua_pushnil(L); lua_setglobal(L, k); }
}

void hostTeardown();   // fwd

}  // namespace

// ---------------------------------------------------------------------------
// 3) lifecycle
// ---------------------------------------------------------------------------
bool luaAppIsOpen() { return s_h != nullptr; }

void luaAppSteer(int dx, int dy) {
  if (!s_h) return;
  const char* dir = (dy < 0) ? "up" : (dy > 0) ? "down" : (dx < 0) ? "left" : (dx > 0) ? "right" : nullptr;
  if (dir) sendInput("swipe", dir, 0, 0);
}

static void luaAppRootDeletedCb(lv_event_t* e) {
  (void)e;
  // Root died (async delete completed) — the lv objects are gone; the Lua
  // state was already closed in hostTeardown().
}

namespace {
void hostTeardown() {
  Host* h = s_h;
  if (!h) return;
  s_h = nullptr;                     // bindings see "closed" from here on
  if (h->timer) { lv_timer_del(h->timer); h->timer = nullptr; }
  if (s_net_poll) { lv_timer_del(s_net_poll); s_net_poll = nullptr; }
  if (h->L && s_net_cb != LUA_NOREF) { luaL_unref(h->L, LUA_REGISTRYINDEX, s_net_cb); }
  s_net_cb = LUA_NOREF;              // a worker fetch may still land; netDeliver sees no app and drops it
  if (h->L) {
    // best-effort on_close + store flush before the state dies
    lua_State* L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->ref_app);
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "on_close");
      if (lua_isfunction(L, -1)) { lua_sethook(L, budgetHook, LUA_MASKCOUNT, kInstrBudget); lua_pcall(L, 0, 0, 0); lua_sethook(L, nullptr, 0, 0); }
      else lua_pop(L, 1);
    }
    lua_pop(L, 1);
    s_h = h;               // storeFlush needs the id + dirty flag
    storeFlush();
    s_h = nullptr;
    lua_close(L);          // runs canvas __gc -> frees pixel buffers
    Serial.printf("[LUAAPP] %s closed, leaked=%u peak=%u\n", h->id,
                  (unsigned)h->heap.used, (unsigned)h->heap.peak);
  }
  if (h->root) appPageDeleteRootAsync(h->root);
  appPageEnd(&luaAppDismiss);
  delete h;
}
}  // namespace

void luaAppDismiss() {
  if (!s_h) return;
  if (s_h->in_lua) { s_h->want_close = true; return; }   // unwind first
  hostTeardown();
}

bool luaAppLaunch(const char* id, const char* title, const char* src, size_t len) {
  if (s_h || !id || !src || len == 0 || len > kMaxSrc) return false;

  Host* h = new Host();
  h->heap.cap = kHeapCap;
  snprintf(h->id, sizeof h->id, "%s", id);
  snprintf(h->title, sizeof h->title, "%s", title ? title : id);

  h->L = lua_newstate(psAllocCb, &h->heap);
  if (!h->L) { delete h; luaHostToast("App: out of memory", 1800); return false; }
  openSandbox(h->L);
  openWada(h->L);

  // UI scaffold: full-screen overlay + tall "< title" bar via AppPage, exactly
  // like SnakeGame. Body = the content area apps build into.
  h->root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(h->root);
  lv_obj_set_size(h->root, lv_disp_get_hor_res(nullptr), lv_disp_get_ver_res(nullptr));
  lv_obj_set_style_bg_color(h->root, lv_color_hex(0x0E1216), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(h->root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(h->root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(h->root, luaAppRootDeletedCb, LV_EVENT_DELETE, nullptr);

  h->body = lv_obj_create(h->root);
  lv_obj_remove_style_all(h->body);
  const int bar_h = 44;   // matches the AppPage tall bar band
  h->body_w = lv_disp_get_hor_res(nullptr);
  h->body_h = lv_disp_get_ver_res(nullptr) - bar_h;
  lv_obj_set_pos(h->body, 0, bar_h);
  lv_obj_set_size(h->body, h->body_w, h->body_h);
  lv_obj_clear_flag(h->body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(h->body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(h->body, gestureCb, LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(h->body, pressCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(h->body, pressCb, LV_EVENT_RELEASED, nullptr);

  snprintf(s_bar_title, sizeof s_bar_title, "%s", h->title);
  appPageBegin(s_bar_title, &luaAppDismiss);

  s_h = h;
  storeLoad();

  // button-callback table
  lua_newtable(h->L);
  h->ref_btncb = luaL_ref(h->L, LUA_REGISTRYINDEX);

  // compile + run the chunk (must return the app callback table)
  lua_pushcfunction(h->L, tracebackMsgh);
  int rc = luaL_loadbuffer(h->L, src, len, id);
  if (rc == LUA_OK) {
    lua_sethook(h->L, budgetHook, LUA_MASKCOUNT, kInstrBudget * 5);   // init gets a bigger budget
    h->in_lua = true;
    rc = lua_pcall(h->L, 0, 1, -2);
    h->in_lua = false;
    lua_sethook(h->L, nullptr, 0, 0);
  }
  if (rc != LUA_OK || !lua_istable(h->L, -1)) {
    const char* err = rc != LUA_OK ? lua_tostring(h->L, -1) : "app did not return a table";
    Serial.printf("[LUAAPP] %s load failed: %s\n", id, err ? err : "?");
    char msg[96];
    snprintf(msg, sizeof msg, "App failed: %.60s", err ? err : "load error");
    luaHostToast(msg, 2600);
    hostTeardown();
    return false;
  }
  h->ref_app = luaL_ref(h->L, LUA_REGISTRYINDEX);
  lua_pop(h->L, 1);   // traceback handler

  if (pushCallback(h, "on_open")) {
    lua_pushinteger(h->L, h->body_w);
    lua_pushinteger(h->L, h->body_h);
    guardedCall(h, 2);
  }
  serviceDeferredClose();
  if (s_h) Serial.printf("[LUAAPP] %s open, heap=%u\n", id, (unsigned)h->heap.used);
  return s_h != nullptr;
}

bool luaAppLaunchFile(const char* id, const char* title, const char* embedded, size_t embedded_len) {
  fs::FS* fs = luaHostAppFs();
  if (fs) {
    char rel[40], path[64];
    snprintf(rel, sizeof rel, "/apps/%s.lua", id);
    luaHostAppPath(path, sizeof path, rel);
    File f = fs->open(path, "r");
    if (f) {
      size_t sz = f.size();
      if (sz > 0 && sz <= kMaxSrc) {
        char* buf = (char*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf) {
          size_t rd = f.read((uint8_t*)buf, sz);
          f.close();
          bool ok = rd == sz && luaAppLaunch(id, title, buf, sz);
          heap_caps_free(buf);
          if (ok) return true;
          // fall through to the embedded copy on any failure
        } else f.close();
      } else f.close();
    }
  }
  if (embedded && embedded_len) return luaAppLaunch(id, title, embedded, embedded_len);
  return false;
}

// ---- net worker entry points (UITask's tile/net worker calls these) --------
bool luaNetWorkerPending() { return s_net_pending; }
void luaNetWorkerService(WiFiClient& client, HTTPClient& http) {
  s_net_pending = false;
  s_net_result = s_net_buf ? luaStoreHttpGet(client, http, s_net_url, s_net_buf, s_net_cap) : -1;
  s_net_done = true;
}

#endif  // CAP_LUA_APPS
