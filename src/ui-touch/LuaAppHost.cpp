// SPDX-License-Identifier: GPL-3.0-or-later
// Lua app host — see LuaAppHost.h + LUA_APPS.md. Structure:
//   1. PSRAM-capped allocator + instruction-budget hook (containment)
//   2. the wada.* bindings (ui/input/timer/sys/store) — API v1
//   3. lifecycle: launch -> run chunk -> callbacks via guarded pcall -> dismiss
#include "LuaAppHost.h"
#if defined(ESP32)
#include "mbedtls/md.h"      // wada.crypto: HMAC/SHA in C, not interpreted Lua
#endif

#if CAP_LUA_APPS
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include <time.h>
#include <math.h>   // isnan for optional sensor fields   // wada.sys.epoch/datetime (#245)
#include "AppPage.h"
#include "device_caps.h"
#include "i18n.h"   // wada.sys.tr: apps can use the same translation table the UI does
#include "Utf8Text.h"
#include "helpers/esp32/WdtHeavyGuard.h"   // wada.fs writes can trigger SPIFFS GC

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// UITask-owned services the host borrows (all pre-existing, fwd-declared here
// to keep this TU decoupled from the 47k-line UITask.cpp).
extern const lv_font_t* luaHostFontForSize(int size_class);       // 12/14/16 -> g_font_*
extern void             luaHostToast(const char* msg, int ms);    // showAlert passthrough
extern bool             luaHostBeep();
extern bool             luaHostScreenOn();   // false = display asleep; app ticks pause
extern void             luaHostKeepAwake(bool on);   // hold the screen + ticks for a measuring app                            // notification chime; false = no sounder / muted
extern fs::FS*          luaHostAppFs();                           // /apps storage root FS (may be null)
extern void             luaHostAppPath(char* out, size_t cap, const char* rel);   // prefixes the store root
#if CAP_LUA_AUDIO
extern bool             luaHostAudioPlay(fs::FS* fs, const char* path, const char* shown,
                                         const char* source, uint32_t owner,
                                         char* error, size_t error_cap);
extern bool             luaHostAudioPause(uint32_t owner, bool pause);
extern bool             luaHostAudioStop(uint32_t owner, bool release);
extern void             luaHostAudioStatus(uint32_t owner, char* state, size_t state_cap,
                                           char* path, size_t path_cap,
                                           char* source, size_t source_cap,
                                           char* format, size_t format_cap,
                                           char* error, size_t error_cap);
#endif
#if CAP_LUA_SD_LIST
extern fs::FS*          luaHostSdFs(bool* busy);                  // mounted physical SD, or null
extern bool             luaHostSdReadFailed();                    // failed open was a dead card
#endif
extern int  luaHostContactAt(int idx, char* name, size_t name_cap, int* type, uint32_t* secs_ago,
                             double* lat, double* lon, char* pk_hex, size_t pk_cap,
                             int32_t* lat_e6, int32_t* lon_e6);
extern int  luaHostRxLogAt(int idx, uint32_t* ms_ago, int* ptype, int* rssi, float* snr, int* hops,
                           int* route, int* len, int* org_kind, char* org_hex, size_t org_cap,
                           uint32_t* at_ms = nullptr);
extern int  luaHostDiscoverCount();
extern void luaHostDiscoverClear();
extern int  luaHostDiscoverAt(int idx, char* pk_hex, size_t pk_cap, char* name, size_t name_cap,
                              int* type, int* rssi, float* snr, float* their_snr, int* hops,
                              uint32_t* first_ms_ago, uint32_t* last_ms_ago, int* heard);
extern void luaHostTextPrompt(const char* title, const char* initial, void (*cb)(const char*));
extern void luaHostTextPromptDismiss();
extern void luaHostRadioStats(float* rssi, float* noise, uint32_t* rx_air_s, uint32_t* tx_air_s,
                              uint32_t* rx_pkts, uint32_t* rx_err, int* budget_ms);
extern void luaHostRadioStats2(uint32_t* rx_evt, uint32_t* rx_drop, uint32_t* tx_pkts,
                               float* freq, float* bw, int* sf, int* duty_pct);
extern void luaHostSelfInfo(char* name, size_t name_cap, double* lat, double* lon,
                            char* pk_hex, size_t pk_cap, int32_t* lat_e6, int32_t* lon_e6);
#if CAP_LUA_SDK_EXT
extern void luaHostBattery(uint16_t* mv, int* pct, bool* charging);
// No HDOP: MeshCore's LocationProvider interface does not expose one, and a
// fabricated accuracy figure is worse than none for anything that would use it.
// speed_kmh / course_deg come back NAN where the board's provider does not
// report them (only boards built on WadaNmeaLocationProvider do). They are
// appended AFTER the fix_time / micro-degree outputs so ONE signature serves
// both callers -- the merge of two independent widenings of this bridge.
extern bool luaHostGps(double* lat, double* lon, int* sats, int* alt_m, uint32_t* fix_time,
                       int32_t* lat_e6, int32_t* lon_e6,
                       float* speed_kmh, float* course_deg);
#endif
#if CAP_SENSORS
extern void luaHostEnv(bool* ok, bool* have_t, float* temp_c, bool* have_h, float* hum_pct,
                       bool* have_p, float* press_hpa, bool* have_alt, int* alt_m);
#endif
#if CAP_LUA_SDK_EXT
extern int  luaHostSendPerm(const char* app_id);                              // 1 grant, -1 refused, 0 unasked
extern bool luaHostReadPerm(const char* app_id);                             // may be shown incoming messages
extern void luaHostRequestSendPerm(const char* app_id, const char* app_name); // raises the consent prompt
extern bool luaHostMeshSendChannel(const char* chan_name, const char* text);
extern bool luaHostMeshSendDM(const char* to_name, const char* text, bool* was_room);
extern int  luaHostMeshChannelNames(char out[][32], int max_n);
extern int  luaHostDmSendPerm(const char* app_id);
extern bool luaHostDmReadPerm(const char* app_id);
extern void luaHostRequestDmSendPerm(const char* app_id, const char* app_name);
extern void* luaHostMapCreate(int x, int y, int w, int h);
extern void  luaHostMapDestroy(void* v);
extern void  luaHostMapSet(void* v, double lat, double lon, int zoom);
extern void  luaHostMapSetZoom(void* v, int zoom);
extern void  luaHostMapRender(void* v);
extern int   luaHostMapZoom(void* v);
extern int   luaHostMapPlaced(void* v);
extern void  luaHostMapToScreen(void* v, double lat, double lon, int* px, int* py);
extern void  luaHostMapToLatLon(void* v, int px, int py, double* lat, double* lon);
extern void  luaHostMapClearOverlay(void* v);
extern int   luaHostMapMarker(void* v, double lat, double lon, uint32_t color, int size);
extern int   luaHostMapLine(void* v, double la1, double lo1, double la2, double lo2,
                            uint32_t color, int width);
extern int      luaHostProbePerm(const char* app_id);
extern void     luaHostRequestProbePerm(const char* app_id, const char* app_name);
extern uint32_t luaHostMeshDiscover(int type_filter);
#endif
#if CAP_COMPASS
extern bool luaHostCompass(float* x_gauss, float* y_gauss, float* z_gauss, bool* overflow);   // sensor frame, uncalibrated
#endif
#if CAP_IMU
extern bool luaHostAccel(float* x_g, float* y_g, float* z_g);   // sensor frame, g
#endif
// DO NOT name the network client type here. What "WiFiClient" means depends on the
// board: a real class on the S3 envs, a `using WiFiClient = NetworkClient` alias
// under arduino-esp32 3.x (Tanmatsu), and a #define to C6Client on the T-Display P4
// (its C6 runs ESP-AT, so sockets go over AT-over-SDIO). Writing `class WiFiClient;`
// declared a BRAND NEW incomplete class on the two boards where the name is an
// alias, so this file emitted luaNetWorkerService under a signature the caller in
// UITask.cpp never referenced — both P4 targets failed to LINK while all 8 S3 envs
// built clean, which is why it stayed hidden.
//
// UITask owns the networking, so it passes the objects down as opaque handles and
// casts them back on its side. This TU never dereferences them.
extern int luaStoreHttpGetOpaque(void* client, void* http, const char* url, char* buf, size_t cap);
extern int luaStoreHttpPostOpaque(void* client, void* http, const char* url, char* buf, size_t cap,
                                  const void* body, size_t body_len, const char* ctype);

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
// App source size limit. Was 64 KB, which a real app can reach: reported from a
// 2000-contact device whose author hit it writing an ordinary Lua program, not
// anything pathological. The source lives in PSRAM only until luaL_loadbuffer
// has compiled it, so this is a transient allocation rather than a per-app cost,
// and the compiled chunk still has to fit the 256 KB per-app heap below.
constexpr size_t kMaxSrc      = 192 * 1024;   // app source size limit
constexpr size_t kStoreMax    = 2048;         // per-app persisted KV budget (bytes, serialized)
constexpr int    kMinTickMs   = 33;           // fastest on_tick cadence (~30 fps)

// Bumped by wada.ui.clear(). Every widget handle records the generation it was
// created in; a handle from an older one has already been destroyed, so the
// accessors below null its pointer and the `if (u->obj)` guard that every
// method already has does the rest.
//
// Without this, clear() would be a use-after-free generator: WidgetUd holds a
// raw lv_obj_t*, so a Lua variable still referring to a label from the previous
// screen would sail past the null check straight into freed memory.
static uint32_t s_ui_gen = 1;

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
  uint32_t    fs_last_write_ms = 0;   // wada.fs write rate limit (#222 lesson)
  uint32_t    mesh_last_send_ms = 0;  // wada.mesh.send airtime rate limit
  uint32_t    mesh_last_probe_ms = 0; // wada.mesh.discover airtime rate limit (separate: a
                                      // probe also spends every neighbour's airtime)
  int         prompt_cb = LUA_NOREF;  // wada.ui.input callback in flight
  // Named timers (wada.timer.after / every-with-a-function). Separate from
  // `timer` above, which is the single on_tick heartbeat and stays as it was.
  struct AppTimer { lv_timer_t* t = nullptr; int cb = LUA_NOREF; bool once = false; };
  static const int kMaxTimers = 8;
  AppTimer    timers[kMaxTimers];
  bool        in_lua = false;        // re-entrancy guard (dismiss from inside a callback)
  bool        want_close = false;
  uint32_t    generation = 0;      // resource ownership across close/reopen
  char        id[24]    = "";
  char        title[32] = "";
};
Host* s_h = nullptr;
uint32_t s_host_generation = 0;

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

// LVGL assumes structurally valid UTF-8 and can stop advancing when an app
// hands it a partial codepoint. Keep one reusable repair buffer: valid strings
// stay zero-copy, while malformed runs become one ASCII '?'. Every call below
// is synchronous and LVGL copies label text before this buffer can be reused.
const char* safeUiText(const char* text, size_t length, size_t* safe_length = nullptr,
                       unsigned scratch_slot = 0) {
  static std::string scratch_slots[2];
  std::string& scratch = scratch_slots[scratch_slot & 1U];
  const char* safe = Utf8Text::sanitize(text, length, scratch);
  if (safe_length) *safe_length = safe == text ? length : scratch.size();
  return safe;
}

// ---- canvas userdata ----
struct CanvasUd { lv_obj_t* obj; lv_color_t* buf; int w, h; uint32_t gen; };

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
  const lv_coord_t x = (lv_coord_t)luaL_checkinteger(L, 2);
  const lv_coord_t y = (lv_coord_t)luaL_checkinteger(L, 3);
  size_t text_length = 0;
  const char* text = luaL_checklstring(L, 4, &text_length);
  lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
  d.color = lv_color_hex(argColor(L, 5));
  d.font = luaHostFontForSize((int)luaL_optinteger(L, 6, 14));
  lv_canvas_draw_text(c->obj, x, y, c->w, &d, safeUiText(text, text_length));
  return 0;
}
int cvPos(lua_State* L) {
  CanvasUd* c = checkCanvas(L);
  if (c->obj) lv_obj_set_pos(c->obj, (lv_coord_t)luaL_checkinteger(L, 2), (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}
int cvGc(lua_State* L) {
  // Frees the pixel buffer. This runs only when the Lua state is closed (see
  // uiCanvas: every canvas is pinned in the registry for the app's lifetime),
  // so the LVGL object is never left pointing at freed pixels.
  //
  // The comment here used to claim the free was conditional on the app
  // closing; it never was, and nothing stopped an ordinary collection cycle
  // from reclaiming a canvas whose Lua handle had gone out of scope while the
  // widget was still on screen and being drawn from that buffer.
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
  ud->gen = s_ui_gen;
  ud->obj = cv; ud->buf = buf; ud->w = w; ud->h = h;
  luaL_setmetatable(L, "wada.canvas");
  // Pin the handle in the registry for the app's lifetime. LVGL draws straight
  // out of this buffer, so the pixels must outlive the widget — and an app that
  // creates a canvas without keeping a reference (or drops one) would otherwise
  // have it collected mid-run, leaving the widget reading freed memory. The
  // registry dies with the state at lua_close, which is where __gc frees the
  // buffer; the ref id is deliberately not tracked, since there is no API to
  // destroy a canvas early.
  lua_pushvalue(L, -1);
  luaL_ref(L, LUA_REGISTRYINDEX);
  return 1;
}

// ---- label userdata ----
struct WidgetUd { lv_obj_t* obj; uint32_t gen; };
WidgetUd* checkLabel(lua_State* L) {
  WidgetUd* u = (WidgetUd*)luaL_checkudata(L, 1, "wada.label");
  if (u->gen != s_ui_gen) u->obj = nullptr;   // cleared out from under this handle
  return u;
}

int lbSet(lua_State* L) {
  WidgetUd* u = checkLabel(L);
  size_t length = 0;
  const char* text = luaL_checklstring(L, 2, &length);
  if (u->obj) lv_label_set_text(u->obj, safeUiText(text, length));
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

int lbWidth(lua_State* L) {   // label:width(px [, "left"|"center"|"right"]) — wraps instead of overflowing
  WidgetUd* u = checkLabel(L);
  if (u->obj) {
    lv_obj_set_width(u->obj, (lv_coord_t)luaL_checkinteger(L, 2));
    lv_label_set_long_mode(u->obj, LV_LABEL_LONG_WRAP);
    // Optional alignment inside that width: the only way an app can centre a
    // line exactly, since it cannot measure glyphs itself.
    const char* al = luaL_optstring(L, 3, nullptr);
    if (al) {
      lv_text_align_t a = strcmp(al, "center") == 0 ? LV_TEXT_ALIGN_CENTER
                        : strcmp(al, "right")  == 0 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
      lv_obj_set_style_text_align(u->obj, a, LV_PART_MAIN);
    }
  }
  return 0;
}

int uiLabel(lua_State* L) {
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  size_t length = 0;
  const char* text = luaL_checklstring(L, 1, &length);
  const lv_coord_t x = (lv_coord_t)luaL_optinteger(L, 2, 0);
  const lv_coord_t y = (lv_coord_t)luaL_optinteger(L, 3, 0);
  const lv_font_t* font = luaHostFontForSize((int)luaL_optinteger(L, 4, 14));
  const uint32_t color = argColor(L, 5, 0xE6E9ED);
  lv_obj_t* l = lv_label_create(s_h->body);
  lv_label_set_text(l, safeUiText(text, length));
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  WidgetUd* ud = (WidgetUd*)lua_newuserdatauv(L, sizeof(WidgetUd), 0);
  ud->gen = s_ui_gen;
  ud->obj = l;
  luaL_setmetatable(L, "wada.label");
  return 1;
}

// ---- chart userdata (the primitive the native Monitor/Airtime pages use) ----
struct ChartUd { lv_obj_t* obj; lv_chart_series_t* ser[2]; int n_ser; uint32_t gen; };
ChartUd* checkChart(lua_State* L) {
  ChartUd* u = (ChartUd*)luaL_checkudata(L, 1, "wada.chart");
  if (u->gen != s_ui_gen) { u->obj = nullptr; u->n_ser = 0; }
  return u;
}

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
// chart:axis(major_ticks, gutter_px) — Y-axis value labels. LVGL draws them to
// the LEFT of the chart's outer edge, so position the chart at least gutter_px
// in from the screen edge or they clip (the built-in page uses x=40).
int chAxis(lua_State* L) {
  ChartUd* c = checkChart(L);
  if (!c->obj) return 0;
  int major = (int)luaL_optinteger(L, 2, 4);
  int gutter = (int)luaL_optinteger(L, 3, 40);
  lv_obj_set_style_text_font(c->obj, luaHostFontForSize(12), LV_PART_TICKS);
  lv_obj_set_style_text_color(c->obj, lv_color_hex(0x7A7F87), LV_PART_TICKS);
  lv_obj_set_style_pad_left(c->obj, 4, LV_PART_TICKS);
  lv_chart_set_axis_tick(c->obj, LV_CHART_AXIS_PRIMARY_Y, 4, 0, major, 1, true, gutter);
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
  lv_obj_clear_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(ch, lua_toboolean(L, 6) ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ch, pts);
  lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(ch, 3, 0);
  // Match the built-in Monitor/Airtime chart styling exactly: panel fill, thin
  // dark border + rounded corners, dim grid lines, no point dots, 2 px series.
  lv_obj_set_style_size(ch, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(ch, lv_color_hex(0x15181B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(ch, lv_color_hex(0x18191A), LV_PART_MAIN);
  lv_obj_set_style_border_width(ch, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(ch, 6, LV_PART_MAIN);
  lv_obj_set_style_line_color(ch, lv_color_hex(0x1A1D1F), LV_PART_MAIN);   // grid
  lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
  ChartUd* ud = (ChartUd*)lua_newuserdatauv(L, sizeof(ChartUd), 0);
  ud->gen = s_ui_gen;
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
  size_t text_length = 0;
  const char* text = luaL_checklstring(L, 1, &text_length);
  const lv_coord_t x = (lv_coord_t)luaL_checkinteger(L, 2);
  const lv_coord_t y = (lv_coord_t)luaL_checkinteger(L, 3);
  const lv_coord_t w = (lv_coord_t)luaL_optinteger(L, 4, 90);
  const lv_coord_t h = (lv_coord_t)luaL_optinteger(L, 5, 34);
  lv_obj_t* b = lv_btn_create(s_h->body);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_t* bl = lv_label_create(b);
  lv_label_set_text(bl, safeUiText(text, text_length));
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
  ud->gen = s_ui_gen;
  ud->obj = b;
  luaL_setmetatable(L, "wada.label");   // shares set/pos/color methods
  return 1;
}

// ---- wada.ui.list: a scrollable, selectable row list -------------------------
//
// The gap this fills: an app had labels, buttons, a canvas and a chart, and no
// "pick one of N". Every non-trivial app therefore hand-rolled rows of labels
// and tracked scrolling itself, which is both tedious and the reason app UIs
// looked worse than the firmware's own.
//
// Rows are real LVGL buttons in a scrollable flex column, so the existing
// keyboard/trackball focus navigation walks them on touchless boards for free,
// and a tap works on the ones with a screen. Per-row callbacks ride the same
// button-callback table uiButton uses.
struct ListUd { lv_obj_t* obj; int sel; uint32_t gen; };
ListUd* checkList(lua_State* L) {
  ListUd* u = (ListUd*)luaL_checkudata(L, 1, "wada.list");
  if (u->gen != s_ui_gen) u->obj = nullptr;
  return u;
}

lv_obj_t* listRowAt(ListUd* u, int i) {          // i is 1-based, as everywhere in Lua
  if (!u->obj || i < 1) return nullptr;
  const uint32_t n = lv_obj_get_child_cnt(u->obj);
  if ((uint32_t)i > n) return nullptr;
  return lv_obj_get_child(u->obj, i - 1);
}
void listPaintRow(lv_obj_t* row, bool selected) {
  lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x15B6A6 : 0x1A1F25), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
}

// wada.ui.text_w(text [, size]) -> rendered width in pixels.
//
// The gap this closes: an app can ask how TALL a line is (text_h) but had no way
// to ask how WIDE it is, so any app laying out its own rows had to guess whether
// a string would wrap. Guessing wrong puts the next row on top of it, which is
// exactly what happened to the SDK Test app's channel list. With this an app can
// compute its own line count instead of estimating from character counts.
int uiTextW(lua_State* L) {
  size_t len = 0;
  const char* txt = luaL_checklstring(L, 1, &len);
  const lv_font_t* f = luaHostFontForSize((int)luaL_optinteger(L, 2, 14));
  txt = safeUiText(txt, len, &len);
  lua_pushinteger(L, (lua_Integer)lv_txt_get_width(txt, (uint32_t)len, f, 0, LV_TEXT_FLAG_NONE));
  return 1;
}

// wada.ui.text_lines(text, width [, size]) -> how many lines it wraps to.
// The question apps actually have. Counts explicit newlines too, and measures
// rather than guessing, so a layout built on it cannot overlap.
int uiTextLines(lua_State* L) {
  size_t len = 0;
  const char* txt = luaL_checklstring(L, 1, &len);
  const int w = (int)luaL_checkinteger(L, 2);
  const lv_font_t* f = luaHostFontForSize((int)luaL_optinteger(L, 3, 14));
  txt = safeUiText(txt, len, &len);
  if (w <= 0) { lua_pushinteger(L, 1); return 1; }
  int lines = 0;
  const char* p = txt;
  while (*p) {
    uint32_t consumed = _lv_txt_get_next_line(p, f, 0, (lv_coord_t)w, nullptr, LV_TEXT_FLAG_NONE);
    if (!consumed) break;
    p += consumed;
    lines++;
    if (lines > 512) break;               // pathological input: stop counting
  }
  lua_pushinteger(L, lines < 1 ? 1 : lines);
  return 1;
}

// wada.ui.clear() -- remove every widget from the app's page.
//
// The SDK documented building widgets in on_open() and gave no way to take them
// down again, so an app with more than one screen drew the second on top of the
// first. Reported by pisti87, who had been working around it with buttons.
//
// Bumping the generation is what makes this safe. A Lua variable still holding a
// label from the screen just cleared keeps a raw pointer to freed memory; the
// accessors compare generations and null it, so the existing `if (u->obj)` guard
// in every method turns a stale call into a no-op instead of a crash.
static int uiClear(lua_State* L) {
  (void)L;
  if (!s_h || !s_h->body) return 0;
  lv_obj_clean(s_h->body);   // deletes the children, keeps the page itself
  ++s_ui_gen;
  return 0;
}

int uiList(lua_State* L) {
  if (!s_h || !s_h->body) return luaL_error(L, "no app body");
  const int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  const int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
  luaL_argcheck(L, w > 0 && h > 0, 3, "width and height must be positive");
  lv_obj_t* c = lv_obj_create(s_h->body);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, (lv_coord_t)x, (lv_coord_t)y);
  lv_obj_set_size(c, (lv_coord_t)w, (lv_coord_t)h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x0E1216), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_row(c, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  ListUd* ud = (ListUd*)lua_newuserdatauv(L, sizeof(ListUd), 0);
  ud->gen = s_ui_gen;
  ud->obj = c; ud->sel = 0;
  luaL_setmetatable(L, "wada.list");
  return 1;
}

// list:add(text [, fn]) -> index of the new row
int lsAdd(lua_State* L) {
  ListUd* u = checkList(L);
  size_t text_length = 0;
  const char* text = luaL_checklstring(L, 2, &text_length);
  if (!u->obj || !s_h) return 0;
  lv_obj_t* row = lv_btn_create(u->obj);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
  listPaintRow(row, false);
  lv_obj_t* lb = lv_label_create(row);
  lv_label_set_text(lb, safeUiText(text, text_length));
  lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);   // a long name truncates instead of reflowing the row
  lv_obj_set_width(lb, LV_PCT(100));
  lv_obj_set_style_text_font(lb, luaHostFontForSize(12), LV_PART_MAIN);
  lv_obj_set_style_text_color(lb, lv_color_hex(0xE6E9ED), LV_PART_MAIN);
  if (lua_isfunction(L, 3)) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_h->ref_btncb);
    lua_pushlightuserdata(L, row);
    lua_pushvalue(L, 3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lv_obj_add_event_cb(row, btnEventCb, LV_EVENT_CLICKED, nullptr);
  }
  lua_pushinteger(L, (lua_Integer)lv_obj_get_child_cnt(u->obj));
  return 1;
}
int lsSet(lua_State* L) {
  ListUd* u = checkList(L);
  lv_obj_t* row = listRowAt(u, (int)luaL_checkinteger(L, 2));
  size_t text_length = 0;
  const char* text = luaL_checklstring(L, 3, &text_length);
  if (!row) return 0;
  lv_obj_t* lb = lv_obj_get_child(row, 0);
  if (lb) lv_label_set_text(lb, safeUiText(text, text_length));
  return 0;
}
int lsColor(lua_State* L) {
  ListUd* u = checkList(L);
  lv_obj_t* row = listRowAt(u, (int)luaL_checkinteger(L, 2));
  if (!row) return 0;
  lv_obj_t* lb = lv_obj_get_child(row, 0);
  if (lb) lv_obj_set_style_text_color(lb, lv_color_hex((uint32_t)luaL_checkinteger(L, 3)), LV_PART_MAIN);
  return 0;
}
// Clearing drops the rows' entries from the callback table too. Without that,
// a list rebuilt every tick leaks one table entry per row for the app's life.
int lsClear(lua_State* L) {
  ListUd* u = checkList(L);
  if (!u->obj || !s_h) return 0;
  lua_rawgeti(L, LUA_REGISTRYINDEX, s_h->ref_btncb);
  const uint32_t n = lv_obj_get_child_cnt(u->obj);
  for (uint32_t i = 0; i < n; i++) {
    lua_pushlightuserdata(L, lv_obj_get_child(u->obj, i));
    lua_pushnil(L);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
  lv_obj_clean(u->obj);
  u->sel = 0;
  return 0;
}
int lsCount(lua_State* L) {
  ListUd* u = checkList(L);
  lua_pushinteger(L, u->obj ? (lua_Integer)lv_obj_get_child_cnt(u->obj) : 0);
  return 1;
}
// list:select(i) highlights AND scrolls the row into view -- on a board with no
// touch, moving the selection without scrolling to it just loses the cursor.
int lsSelect(lua_State* L) {
  ListUd* u = checkList(L);
  const int i = (int)luaL_checkinteger(L, 2);
  if (!u->obj) return 0;
  const uint32_t n = lv_obj_get_child_cnt(u->obj);
  for (uint32_t k = 0; k < n; k++) listPaintRow(lv_obj_get_child(u->obj, k), false);
  lv_obj_t* row = listRowAt(u, i);
  if (!row) { u->sel = 0; return 0; }
  listPaintRow(row, true);
  lv_obj_scroll_to_view(row, LV_ANIM_OFF);
  u->sel = i;
  return 0;
}
int lsSelected(lua_State* L) {
  ListUd* u = checkList(L);
  if (u->sel < 1) { lua_pushnil(L); return 1; }
  lua_pushinteger(L, u->sel);
  return 1;
}
int lsPos(lua_State* L) {
  ListUd* u = checkList(L);
  if (u->obj) lv_obj_set_pos(u->obj, (lv_coord_t)luaL_checkinteger(L, 2),
                                     (lv_coord_t)luaL_checkinteger(L, 3));
  return 0;
}

// ---- sys / timer / store ----
// wada.ui.scroll(true) — let the app body scroll vertically. Apps that lay out
// more content than the screen (a log feed, a settings list) need this; games
// leave it off so a swipe steers instead of dragging the page.
int uiScroll(lua_State* L) {
  if (!s_h || !s_h->body) return 0;
  const bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
  if (on) {
    lv_obj_add_flag(s_h->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_h->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_h->body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_h->body, lv_color_hex(0x15B6A6), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_h->body, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_h->body, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(s_h->body, 10, LV_PART_MAIN);
  } else {
    lv_obj_clear_flag(s_h->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_h->body, LV_SCROLLBAR_MODE_OFF);
  }
  return 0;
}

// wada.ui.text_h(size) — real line height of a size class, so an app can stack
// rows without guessing (guessing is what made labels overlap).
int uiTextH(lua_State* L) {
  lua_pushinteger(L, lv_font_get_line_height(luaHostFontForSize((int)luaL_optinteger(L, 1, 12))));
  return 1;
}

int sysMillis(lua_State* L) { lua_pushinteger(L, (lua_Integer)millis()); return 1; }
// wada.sys.epoch() -> Unix seconds, or nil when the clock has never been set.
// nil rather than 0 on purpose: an app that stamps a log wants to be able to
// tell "no clock yet" from "1970", and the device genuinely boots without one
// until GPS, NTP or a mesh peer supplies the time (#245).
int sysEpoch(lua_State* L) {
  const time_t now = time(nullptr);
  if (now < 1700000000) { lua_pushnil(L); return 1; }   // same sane-clock floor the mesh uses
  lua_pushinteger(L, (lua_Integer)now);
  return 1;
}
// wada.sys.datetime() -> table in LOCAL time (the timezone the user picked),
// because every use for it is display. Fields match pisti87's request in #245.
// nil under the same unset-clock rule as epoch().
int sysDatetime(lua_State* L) {
  const time_t now = time(nullptr);
  struct tm tmv;
  if (now < 1700000000 || !localtime_r(&now, &tmv)) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 7);
  lua_pushinteger(L, tmv.tm_year + 1900); lua_setfield(L, -2, "year");
  lua_pushinteger(L, tmv.tm_mon + 1);     lua_setfield(L, -2, "month");
  lua_pushinteger(L, tmv.tm_mday);        lua_setfield(L, -2, "day");
  lua_pushinteger(L, tmv.tm_hour);        lua_setfield(L, -2, "hour");
  lua_pushinteger(L, tmv.tm_min);         lua_setfield(L, -2, "min");
  lua_pushinteger(L, tmv.tm_sec);         lua_setfield(L, -2, "sec");
  lua_pushinteger(L, tmv.tm_wday);        lua_setfield(L, -2, "wday");   // 0 = Sunday
  return 1;
}
// wada.sys.beep() -> true if a sound was actually produced. #245 asked for a
// melody player; what the firmware has is one notification chime, and only some
// boards have a sounder at all. Exposing the truthful primitive beats inventing
// a melody engine an app could not rely on: the return value lets an app fall
// back to a visual cue instead of silently doing nothing.
int sysBeep(lua_State* L) { lua_pushboolean(L, luaHostBeep()); return 1; }

// wada.sys.caps() -> what THIS board actually offers. Apps must branch on this
// rather than assume: the extended SDK is absent on low-resource boards (see
// CAP_LUA_SDK_EXT in device_caps.h), and a store app runs on all of them.
int sysCaps(lua_State* L) {
  lua_createtable(L, 0, 20);
  lua_pushboolean(L, CAP_LUA_SDK_EXT);  lua_setfield(L, -2, "sdk_ext");
  lua_pushboolean(L, CAP_KEYBOARD);     lua_setfield(L, -2, "keyboard");
  lua_pushboolean(L, CAP_TOUCH);        lua_setfield(L, -2, "touch");
  lua_pushboolean(L, CAP_SD);           lua_setfield(L, -2, "sd");
  lua_pushboolean(L, CAP_LUA_SD_LIST);  lua_setfield(L, -2, "sd_list");
  lua_pushboolean(L, CAP_LUA_AUDIO);    lua_setfield(L, -2, "audio");
  lua_pushboolean(L, CAP_LUA_AUDIO);    lua_setfield(L, -2, "audio_wav");
  lua_pushboolean(L, CAP_LUA_AUDIO);    lua_setfield(L, -2, "audio_mp3");
  lua_pushboolean(L, CAP_LUA_AUDIO && CAP_LUA_SD_LIST);
  lua_setfield(L, -2, "audio_sd");
  // Feature flags for the calls added after the first extended SDK shipped, so
  // an app can degrade instead of erroring on firmware that predates them.
  lua_pushboolean(L, CAP_LUA_SDK_EXT);  lua_setfield(L, -2, "discover");   // wada.mesh.discover
  lua_pushboolean(L, 1);                lua_setfield(L, -2, "input");      // wada.ui.input
  lua_pushboolean(L, 1);                lua_setfield(L, -2, "rx_identity");// rx_log pubkey/src/dst
  lua_pushboolean(L, 1);                lua_setfield(L, -2, "list");       // wada.ui.list
  lua_pushboolean(L, 1);                lua_setfield(L, -2, "packets");    // app.on_packet
  lua_pushboolean(L, 1);                lua_setfield(L, -2, "measure");    // ui.text_w / text_lines
  lua_pushboolean(L, CAP_SENSORS);      lua_setfield(L, -2, "sensors");    // wada.sys.env
  lua_pushboolean(L, CAP_LUA_SDK_EXT);  lua_setfield(L, -2, "map");        // wada.map.view
  lua_pushboolean(L, CAP_COMPASS);      lua_setfield(L, -2, "compass");    // wada.sys.compass()
  lua_pushboolean(L, CAP_IMU);          lua_setfield(L, -2, "accel");      // wada.sys.accel()
  return 1;
}

#if CAP_LUA_SDK_EXT
// wada.sys.battery() -> { mv, pct, charging }
int sysBattery(lua_State* L) {
  uint16_t mv = 0; int pct = -1; bool chg = false;
  luaHostBattery(&mv, &pct, &chg);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, mv);      lua_setfield(L, -2, "mv");
  lua_pushinteger(L, pct);     lua_setfield(L, -2, "pct");
  lua_pushboolean(L, chg);     lua_setfield(L, -2, "charging");
  return 1;
}
// wada.sys.gps() -> { lat, lon, lat_e6, lon_e6, sats, alt_m, [time],
// [speed_kmh], [course] } or nil with NO fix. nil rather than stale
// coordinates: wada.mesh.self() already hands out the last known position, and
// an app plotting a track needs to know the difference. alt_m is metres; time
// is satellite time, absent until the receiver has decoded the date. speed_kmh and course (degrees clockwise from north) are
// present only on boards whose GPS provider reports them, and course only
// while actually moving — a stationary receiver's course is meaningless, so it
// is absent rather than 0.
int sysGps(lua_State* L) {
  double lat = 0, lon = 0; int sats = 0, alt = 0; uint32_t ftime = 0; int32_t lat6 = 0, lon6 = 0;
  float spd = NAN, crs = NAN;
  if (!luaHostGps(&lat, &lon, &sats, &alt, &ftime, &lat6, &lon6, &spd, &crs)) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 9);
  lua_pushnumber(L, lat);   lua_setfield(L, -2, "lat");
  lua_pushnumber(L, lon);   lua_setfield(L, -2, "lon");
  // Exact micro-degrees: Lua floats here are single precision (LUA_32BITS), so
  // lat/lon above are good for display and lossy for a log. Write these.
  lua_pushinteger(L, lat6); lua_setfield(L, -2, "lat_e6");
  lua_pushinteger(L, lon6); lua_setfield(L, -2, "lon_e6");
  lua_pushinteger(L, sats); lua_setfield(L, -2, "sats");
  lua_pushinteger(L, alt);  lua_setfield(L, -2, "alt_m");
  if (ftime) { lua_pushinteger(L, (lua_Integer)ftime); lua_setfield(L, -2, "time"); }
  if (!isnan(spd)) { lua_pushnumber(L, spd); lua_setfield(L, -2, "speed_kmh"); }
  if (!isnan(crs)) { lua_pushnumber(L, crs); lua_setfield(L, -2, "course"); }
  return 1;
}
#endif  // CAP_LUA_SDK_EXT

#if CAP_COMPASS
// wada.sys.compass() -> { x, y, z, ovfl } in Gauss, SENSOR frame, uncalibrated
// -- or nil when the chip is absent, not answering, or has nothing fresh. On
// the hardware gate (CAP_COMPASS), not the memory gate.
// `ovfl` is true when the chip flagged the sample as saturated (magnet nearby,
// or a hard-iron bias beyond the range): the numbers are delivered so the app
// can say so, but they are not a heading.
// Deliberately no `heading`: a heading needs hard-iron offsets (the M9 carries
// a large on-board bias that the user has to calibrate away by rotating the
// device) and the sensor-to-screen axis mapping, both of which belong in the
// app where they can be adjusted and persisted per user. Once the offsets are
// subtracted and the axes mapped so fx points along the screen's top edge and
// fy along its right edge, heading = math.atan(-fy, fx) (clockwise from
// magnetic north) -- deploy/apps/gpscompass is the worked example.
int sysCompass(lua_State* L) {
  float x = 0, y = 0, z = 0; bool ovfl = false;
  if (!luaHostCompass(&x, &y, &z, &ovfl)) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, x);     lua_setfield(L, -2, "x");
  lua_pushnumber(L, y);     lua_setfield(L, -2, "y");
  lua_pushnumber(L, z);     lua_setfield(L, -2, "z");
  lua_pushboolean(L, ovfl); lua_setfield(L, -2, "ovfl");
  return 1;
}
#endif  // CAP_COMPASS

#if CAP_IMU
// wada.sys.accel() -> { x, y, z } in g, SENSOR frame — or nil when the chip is
// absent or has nothing fresh. Held still, the magnitude is 1 and the axis
// pointing at the sky carries it, which is how an app identifies the axes.
// Its purpose here is tilt: a magnetic heading taken from two axes is wrong by
// roughly 1.5 degrees per degree of tilt at mid latitudes, because the field
// dips ~60 degrees and tipping the device swaps some of that vertical field
// into the horizontal pair.
int sysAccel(lua_State* L) {
  float x = 0, y = 0, z = 0;
  if (!luaHostAccel(&x, &y, &z)) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 3);
  lua_pushnumber(L, x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, y); lua_setfield(L, -2, "y");
  lua_pushnumber(L, z); lua_setfield(L, -2, "z");
  return 1;
}
#endif  // CAP_IMU

#if CAP_SENSORS
// wada.sys.env() -> { temp_c, humidity, pressure_hpa, alt_m }, or nil.
//
// Gated on CAP_SENSORS -- does this board HAVE the sensor rail -- and not on
// CAP_LUA_SDK_EXT, which is a memory gate. Those are different questions, and
// conflating them was wrong in both directions: the plain Heltec V4 has the
// Expansion Kit but not the extended SDK, and most boards with the extended SDK
// have no sensors at all. A field is present only when the hardware actually
// reported it, so an app can tell "no humidity sensor" from "0% humidity".
int sysEnv(lua_State* L) {
  bool ok = false;
  float t = 0, h = 0, p = 0; int alt = 0;
  bool ht = false, hh = false, hp = false, ha = false;
  luaHostEnv(&ok, &ht, &t, &hh, &h, &hp, &p, &ha, &alt);
  if (!ok) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 4);
  if (ht) { lua_pushnumber(L, t);   lua_setfield(L, -2, "temp_c"); }
  if (hh) { lua_pushnumber(L, h);   lua_setfield(L, -2, "humidity"); }
  if (hp) { lua_pushnumber(L, p);   lua_setfield(L, -2, "pressure_hpa"); }
  if (ha) { lua_pushinteger(L, alt); lua_setfield(L, -2, "alt_m"); }
  return 1;
}
#endif  // CAP_SENSORS

// ---- wada.geo ---------------------------------------------------------------
// Great-circle maths in C. Not a convenience wrapper: haversine and the bearing
// formula are a dozen trig calls each, and an app doing them per contact per
// tick spends a real slice of its 100k instruction budget on arithmetic the
// chip does in microseconds.
//
// NOTE ON "COMPASS": bearing() is a TRUE bearing from one coordinate to
// another. It is NOT a magnetic heading and this is not a compass. To know
// which way the user is FACING you need either their course over ground
// (successive GPS fixes, or wada.sys.gps().course where the board reports it)
// or a magnetometer -- wada.sys.compass(), which only the CAP_COMPASS boards
// have. This gives you the direction to steer, not the direction they face.
static double geoRad(double d) { return d * M_PI / 180.0; }

int geoDistance(lua_State* L) {
  const double la1 = geoRad(luaL_checknumber(L, 1)), lo1 = geoRad(luaL_checknumber(L, 2));
  const double la2 = geoRad(luaL_checknumber(L, 3)), lo2 = geoRad(luaL_checknumber(L, 4));
  const double dla = la2 - la1, dlo = lo2 - lo1;
  const double a = sin(dla / 2) * sin(dla / 2) + cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
  lua_pushnumber(L, 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));   // metres
  return 1;
}
int geoBearing(lua_State* L) {
  const double la1 = geoRad(luaL_checknumber(L, 1)), lo1 = geoRad(luaL_checknumber(L, 2));
  const double la2 = geoRad(luaL_checknumber(L, 3)), lo2 = geoRad(luaL_checknumber(L, 4));
  const double dlo = lo2 - lo1;
  const double y = sin(dlo) * cos(la2);
  const double x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo);
  double deg = atan2(y, x) * 180.0 / M_PI;
  if (deg < 0) deg += 360.0;
  lua_pushnumber(L, deg);
  return 1;
}
// 0..360 -> "N", "NE", ... Sixteen points would be false precision on a bearing
// derived from consumer GPS, so this stops at eight.
int geoCardinal(lua_State* L) {
  static const char* kPts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  double deg = luaL_checknumber(L, 1);
  while (deg < 0) deg += 360.0;
  int idx = (int)((fmod(deg, 360.0) + 22.5) / 45.0) & 7;
  lua_pushstring(L, kPts[idx]);
  return 1;
}
// wada.sys.keep_awake(on) — for an app that is measuring rather than showing:
// holds the screen on and keeps on_tick running. Cleared automatically when the
// app closes. Use it around a calibration or a capture, not for the whole app.
int sysKeepAwake(lua_State* L) {
  const bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
  luaHostKeepAwake(on);
  return 0;
}
int sysToast(lua_State* L)  { luaHostToast(luaL_checkstring(L, 1), (int)luaL_optinteger(L, 2, 1500)); return 0; }
int sysRandom(lua_State* L) {
  // esp_random-backed: apps should not have to seed math.random for games
  lua_Integer lo = luaL_optinteger(L, 1, 0), hi = luaL_optinteger(L, 2, 0);
  if (hi <= lo) { lua_pushinteger(L, (lua_Integer)esp_random()); return 1; }
  lua_pushinteger(L, lo + (lua_Integer)(esp_random() % (uint32_t)(hi - lo + 1)));
  return 1;
}
// wada.sys.tr(s) -- translate through the same table the UI uses.
//
// Lua apps had no way to reach it, so every built-in app (airtime's "channel
// busy", the monitor, the games) was hard-English no matter what language the
// device was set to, and a translator could file the string but nothing could
// consume it (#257). Keys go in deploy/apps/lang/*.lang exactly like the
// firmware's own; an untranslated key returns unchanged, so an app that calls
// this is never worse off than one that does not.
static int sysTr(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  lua_pushstring(L, TR(s));
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
  // Screen asleep: nothing the app draws is visible, and an app polling a
  // sensor would keep the hardware awake for a dark screen. Skip the tick and
  // let it resume on wake — dt is derived from millis(), so the app sees the
  // pause as one long frame rather than a broken clock.
  if (!luaHostScreenOn()) { s_h->last_tick = 0; return; }
  uint32_t now = millis();
  uint32_t dt = s_h->last_tick ? now - s_h->last_tick : 0;
  s_h->last_tick = now;
  if (pushCallback(s_h, "on_tick")) {
    lua_pushinteger(s_h->L, (lua_Integer)dt);
    guardedCall(s_h, 1);
  }
  serviceDeferredClose();
}

// ---- named timers ----------------------------------------------------------
// One app used to get exactly one clock: on_tick. Anything with a second
// cadence -- a slow poll beside a fast animation, a delayed retry, a timeout on
// a network call -- had to be a counter inside on_tick, which is both tedious
// and wrong under a variable tick period. These are real timers, capped at
// kMaxTimers so an app cannot fill the LVGL timer list.
//
// A handle is returned so the app can stop one specifically; stopping is also
// automatic for a one-shot after it fires, and for everything at app close.
struct TimerUd { int slot; uint32_t gen; };
uint32_t s_timer_gen[Host::kMaxTimers] = {0};   // bumped on reuse, so a stale handle is inert

void timerRelease(int slot) {
  if (!s_h || slot < 0 || slot >= Host::kMaxTimers) return;
  Host::AppTimer& s = s_h->timers[slot];
  if (s.t) { lv_timer_del(s.t); s.t = nullptr; }
  if (s.cb != LUA_NOREF && s_h->L) luaL_unref(s_h->L, LUA_REGISTRYINDEX, s.cb);
  s.cb = LUA_NOREF;
  s.once = false;
  s_timer_gen[slot]++;
}

void namedTimerCb(lv_timer_t* t) {
  if (!s_h || !s_h->L || !t) return;
  const int slot = (int)(intptr_t)t->user_data;
  if (slot < 0 || slot >= Host::kMaxTimers) return;
  Host::AppTimer& s = s_h->timers[slot];
  if (s.cb == LUA_NOREF) return;
  // Same rule as on_tick: a REPEATING timer does not run while the display is
  // asleep, since its usual job is polling something nobody can see. A one-shot
  // still fires — it is a scheduled piece of app logic, and skipping it would
  // drop that work permanently rather than defer it (the slot is released
  // around the call), which is not the same trade at all.
  if (!s.once && !luaHostScreenOn()) return;
  lua_State* L = s_h->L;
  // A one-shot is released BEFORE the call, so the callback may safely start
  // another timer in the same slot without it being torn down underneath.
  if (s.once) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.cb);
    timerRelease(slot);
    guardedCall(s_h, 0);
  } else {
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.cb);
    guardedCall(s_h, 0);
  }
  serviceDeferredClose();
}

int timerStart(lua_State* L, int ms, int fnidx, bool once) {
  if (!s_h) return luaL_error(L, "no app");
  if (ms < kMinTickMs) ms = kMinTickMs;
  int slot = -1;
  for (int i = 0; i < Host::kMaxTimers; i++) if (!s_h->timers[i].t) { slot = i; break; }
  if (slot < 0) return luaL_error(L, "too many timers (max %d)", Host::kMaxTimers);
  lua_pushvalue(L, fnidx);
  s_h->timers[slot].cb   = luaL_ref(L, LUA_REGISTRYINDEX);
  s_h->timers[slot].once = once;
  s_h->timers[slot].t    = lv_timer_create(namedTimerCb, ms, (void*)(intptr_t)slot);
  if (!s_h->timers[slot].t) { timerRelease(slot); return luaL_error(L, "timer alloc failed"); }
  TimerUd* ud = (TimerUd*)lua_newuserdatauv(L, sizeof(TimerUd), 0);
  ud->slot = slot;
  ud->gen  = s_timer_gen[slot];
  luaL_setmetatable(L, "wada.timerh");
  return 1;
}

// wada.timer.every(ms)      -> set the on_tick period (unchanged)
// wada.timer.every(ms, fn)  -> a repeating timer calling fn; returns a handle
int timerEvery(lua_State* L) {
  if (!s_h) return 0;
  int ms = (int)luaL_checkinteger(L, 1);
  if (lua_isfunction(L, 2)) return timerStart(L, ms, 2, false);
  if (ms < kMinTickMs) ms = kMinTickMs;
  if (s_h->timer) lv_timer_set_period(s_h->timer, ms);
  else            s_h->timer = lv_timer_create(tickTimerCb, ms, nullptr);
  return 0;
}
// wada.timer.after(ms, fn) -> fires once; returns a handle you can cancel.
int timerAfter(lua_State* L) {
  int ms = (int)luaL_checkinteger(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  return timerStart(L, ms, 2, true);
}
// wada.timer.stop()  -> stop the on_tick heartbeat (unchanged)
// wada.timer.stop(h) -> stop that one timer
int timerStop(lua_State* L) {
  if (!s_h) return 0;
  if (luaL_testudata(L, 1, "wada.timerh")) {
    TimerUd* u = (TimerUd*)lua_touserdata(L, 1);
    if (u->gen == s_timer_gen[u->slot]) timerRelease(u->slot);
    return 0;
  }
  if (s_h->timer) { lv_timer_del(s_h->timer); s_h->timer = nullptr; s_h->last_tick = 0; }
  return 0;
}
int timerHandleStop(lua_State* L) {
  TimerUd* u = (TimerUd*)luaL_checkudata(L, 1, "wada.timerh");
  if (s_h && u->gen == s_timer_gen[u->slot]) timerRelease(u->slot);
  return 0;
}

// Store: a Lua table mirrored to /apps/<id>.sav as "k=v" lines (strings and
// numbers only). Loaded at launch, flushed on dismiss when dirty — tiny, rare,
// and off the hot path (touch_perf: never spam flash from the UI thread).
#if CAP_LUA_SDK_EXT
// wada.mesh.send(channel, text) -> true | false, reason
//
// The ONLY write path an app has into the mesh, and the one API where a bad app
// costs other people rather than just its own device: LoRa is a shared channel,
// and anything sent goes out under the user's node name.
//
// Three gates, in order:
//   1. per-app consent, granted by the user once and bound to THIS app id
//   2. an airtime rate limit, far stricter than the filesystem one
//   3. a length cap, so one call cannot occupy the channel
//
// The app never sees a prompt API and cannot pre-approve itself: the first call
// without a grant fails AND raises the prompt, so consent is always something
// the user did, not something the app asked for at a convenient moment.
static const uint32_t kMeshSendMinGapMs = 5000;   // per app
static const size_t   kMeshSendMaxLen   = 180;    // one LoRa text payload

int meshSend(lua_State* L) {
  const char* chan = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* text = luaL_checklstring(L, 2, &len);
  if (!s_h) { lua_pushboolean(L, 0); lua_pushstring(L, "no app"); return 2; }
  if (len == 0 || len > kMeshSendMaxLen) {
    lua_pushboolean(L, 0); lua_pushstring(L, "bad length"); return 2;
  }
  const int perm = luaHostSendPerm(s_h->id);
  if (perm != 1) {
    if (perm == 0) luaHostRequestSendPerm(s_h->id, s_h->id);   // ask once, never in a loop
    lua_pushboolean(L, 0);
    lua_pushstring(L, perm == 0 ? "permission requested" : "permission denied");
    return 2;
  }
  const uint32_t now = millis();
  if (s_h->mesh_last_send_ms && (uint32_t)(now - s_h->mesh_last_send_ms) < kMeshSendMinGapMs) {
    lua_pushboolean(L, 0); lua_pushstring(L, "too fast"); return 2;
  }
  const bool ok = luaHostMeshSendChannel(chan, text);
  if (ok) s_h->mesh_last_send_ms = now;   // only a real transmission starts the clock
  lua_pushboolean(L, ok);
  if (!ok) { lua_pushstring(L, "no such channel"); return 2; }
  return 1;
}

// wada.mesh.send_dm(to, text) -> ok, err
// `to` is a CONTACT NAME as it appears in wada.mesh.contacts(). A room server is a
// contact too, so the same call posts to a room; the second return value says which
// it was ("sent" or "room") so an app does not have to keep its own table of types.
// Shares the send rate limiter with channel sends: one app, one radio.
int meshSendDm(lua_State* L) {
  const char* to = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* text = luaL_checklstring(L, 2, &len);
  if (!s_h) { lua_pushboolean(L, 0); lua_pushstring(L, "no app"); return 2; }
  if (len == 0 || len > kMeshSendMaxLen) {
    lua_pushboolean(L, 0); lua_pushstring(L, "bad length"); return 2;
  }
  const int perm = luaHostDmSendPerm(s_h->id);
  if (perm != 1) {
    if (perm == 0) luaHostRequestDmSendPerm(s_h->id, s_h->id);   // ask once, never in a loop
    lua_pushboolean(L, 0);
    lua_pushstring(L, perm == 0 ? "permission requested" : "permission denied");
    return 2;
  }
  const uint32_t now = millis();
  if (s_h->mesh_last_send_ms && (uint32_t)(now - s_h->mesh_last_send_ms) < kMeshSendMinGapMs) {
    lua_pushboolean(L, 0); lua_pushstring(L, "too fast"); return 2;
  }
  bool was_room = false;
  const bool ok = luaHostMeshSendDM(to, text, &was_room);
  if (ok) s_h->mesh_last_send_ms = now;
  lua_pushboolean(L, ok);
  lua_pushstring(L, ok ? (was_room ? "room" : "sent") : "no such contact");
  return 2;
}

// wada.mesh.discover([types]) -> tag | false, reason
//
// Broadcasts a zero-hop discovery request and returns immediately; replies land
// in wada.mesh.discovered() over the next few seconds. `types` is an optional
// bitmask over wada.mesh.NODE_* (omit for every type).
//
// Rate-limited harder than a message send, and deliberately so. A message costs
// one transmission; a probe costs one transmission plus a reply from every node
// that hears it, so an app looping at 1 Hz would saturate a neighbourhood on its
// own. 15 seconds is faster than anybody can drive out of a cell and slow enough
// that the channel survives it.
static const uint32_t kMeshProbeMinGapMs = 15000;   // per app

int meshDiscover(lua_State* L) {
  const int types = (int)luaL_optinteger(L, 1, 0);
  if (!s_h) { lua_pushboolean(L, 0); lua_pushstring(L, "no app"); return 2; }
  const int perm = luaHostProbePerm(s_h->id);
  if (perm != 1) {
    if (perm == 0) luaHostRequestProbePerm(s_h->id, s_h->id);   // ask once, never in a loop
    lua_pushboolean(L, 0);
    lua_pushstring(L, perm == 0 ? "permission requested" : "permission denied");
    return 2;
  }
  const uint32_t now = millis();
  if (s_h->mesh_last_probe_ms && (uint32_t)(now - s_h->mesh_last_probe_ms) < kMeshProbeMinGapMs) {
    lua_pushboolean(L, 0); lua_pushstring(L, "too fast"); return 2;
  }
  const uint32_t tag = luaHostMeshDiscover(types);
  if (!tag) { lua_pushboolean(L, 0); lua_pushstring(L, "radio busy"); return 2; }
  s_h->mesh_last_probe_ms = now;
  lua_pushinteger(L, (lua_Integer)tag);
  return 1;
}

// ---- wada.map: the firmware's own slippy map, inside an app --------------
//
// Before this an app could draw geography only on a bare canvas: no basemap, no
// projection, no tile cache. Now it gets the real thing -- the same OSM tiles,
// the same Web Mercator projection and the same on-disk cache the Map tab uses.
//
// ONE view per app. Each one owns a pool of up to four decoded 256x256 tiles,
// which is 512 KB of PSRAM; letting an app open them in a loop would be a
// straightforward way to exhaust the board. :close() disposes of one early
// rather than waiting for the collector.
struct MapUd { void* v; };
int s_map_views = 0;

MapUd* checkMap(lua_State* L) { return (MapUd*)luaL_checkudata(L, 1, "wada.map"); }

int mapView(lua_State* L) {
  if (!s_h) return luaL_error(L, "no app");
  if (s_map_views > 0) return luaL_error(L, "only one map view at a time (call :close() first)");
  const int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  const int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
  luaL_argcheck(L, w > 0 && w <= 800, 3, "width 1..800");
  luaL_argcheck(L, h > 0 && h <= 800, 4, "height 1..800");
  void* v = luaHostMapCreate(x, y, w, h);
  if (!v) return luaL_error(L, "map view alloc failed");
  MapUd* ud = (MapUd*)lua_newuserdatauv(L, sizeof(MapUd), 0);
  ud->v = v;
  s_map_views++;
  luaL_setmetatable(L, "wada.map");
  return 1;
}

// map:center(lat, lon [, zoom]) -- moves and redraws.
int mpCenter(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) return 0;
  const double lat = luaL_checknumber(L, 2), lon = luaL_checknumber(L, 3);
  luaHostMapSet(u->v, lat, lon, (int)luaL_optinteger(L, 4, -1));
  luaHostMapRender(u->v);
  return 0;
}
// map:zoom() -> z   |   map:zoom(z) -- sets and redraws.
int mpZoom(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) { lua_pushinteger(L, 0); return 1; }
  if (lua_isnumber(L, 2)) {
    luaHostMapSetZoom(u->v, (int)lua_tointeger(L, 2));
    luaHostMapRender(u->v);
    return 0;
  }
  lua_pushinteger(L, luaHostMapZoom(u->v));
  return 1;
}
int mpMarker(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) return 0;
  lua_pushboolean(L, luaHostMapMarker(u->v, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                      (uint32_t)luaL_optinteger(L, 4, 0x15B6A6),
                                      (int)luaL_optinteger(L, 5, 8)));
  return 1;   // false = the point is outside the view, so nothing was drawn
}
int mpLine(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) return 0;
  lua_pushboolean(L, luaHostMapLine(u->v, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                    luaL_checknumber(L, 4), luaL_checknumber(L, 5),
                                    (uint32_t)luaL_optinteger(L, 6, 0x15B6A6),
                                    (int)luaL_optinteger(L, 7, 2)));
  return 1;
}
int mpClear(lua_State* L) { MapUd* u = checkMap(L); if (u->v) luaHostMapClearOverlay(u->v); return 0; }
int mpRedraw(lua_State* L) { MapUd* u = checkMap(L); if (u->v) luaHostMapRender(u->v); return 0; }
// map:tiles() -> how many tiles the last redraw actually placed. 0 means this
// area is not cached at this zoom: an app should say so rather than present an
// empty rectangle as if it were open water.
int mpTiles(lua_State* L) {
  MapUd* u = checkMap(L);
  lua_pushinteger(L, u->v ? luaHostMapPlaced(u->v) : 0);
  return 1;
}
int mpToScreen(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) return 0;
  int px = 0, py = 0;
  luaHostMapToScreen(u->v, luaL_checknumber(L, 2), luaL_checknumber(L, 3), &px, &py);
  lua_pushinteger(L, px); lua_pushinteger(L, py);
  return 2;
}
int mpToLatLon(lua_State* L) {
  MapUd* u = checkMap(L);
  if (!u->v) return 0;
  double lat = 0, lon = 0;
  luaHostMapToLatLon(u->v, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), &lat, &lon);
  lua_pushnumber(L, lat); lua_pushnumber(L, lon);
  return 2;
}
int mpClose(lua_State* L) {
  MapUd* u = checkMap(L);
  if (u->v) { luaHostMapDestroy(u->v); u->v = nullptr; if (s_map_views > 0) s_map_views--; }
  return 0;
}
int mpGc(lua_State* L) { return mpClose(L); }

// wada.mesh.channels() -> { "Public", "MyPrivate", ... }
// Names only. The channel secret is never handed to Lua, so an app can post to a
// channel the user already has but can neither derive one nor pass it on.
int meshChannels(lua_State* L) {
  static char names[MAX_GROUP_CHANNELS][32];
  const int n = luaHostMeshChannelNames(names, MAX_GROUP_CHANNELS);
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_pushstring(L, names[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
#endif

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

#if CAP_LUA_SDK_EXT
// ---- wada.fs: scoped file access -------------------------------------------
// Every app gets ONE directory, /apps/<id>.d/, and cannot address anything
// outside it. Store apps are user-submitted, so the name check is a security
// boundary rather than tidiness: no '/', no '..', no leading dot, and a strict
// [A-Za-z0-9._-] whitelist. Rejecting outright beats sanitising -- a sanitiser
// that "fixes" ../../identity into something valid is how these go wrong.
static const size_t kFsMaxFile  = 32u * 1024u;   // per file
static const uint32_t kFsMinGapMs = 1000;        // between writes, per app

static bool fsSafeName(const char* n) {
  if (!n || !*n || n[0] == '.') return false;
  size_t len = 0;
  for (const char* p = n; *p; ++p, ++len) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return len <= 32;
}
static bool fsPath(char* out, size_t cap, const char* name) {
  if (!s_h || !fsSafeName(name)) return false;
  char rel[96];
  snprintf(rel, sizeof rel, "/apps/%s.d/%s", s_h->id, name);
  luaHostAppPath(out, cap, rel);
  return true;
}
static void fsEnsureDir() {
  fs::FS* fs = luaHostAppFs();
  if (!fs || !s_h) return;
  char d[80], rel[64];
  luaHostAppPath(d, sizeof d, "/apps");            fs->mkdir(d);
  snprintf(rel, sizeof rel, "/apps/%s.d", s_h->id);
  luaHostAppPath(d, sizeof d, rel);                fs->mkdir(d);
}
// Shared by write and append. Rate-limited on purpose: on internal flash a burst
// of small writes triggers garbage collection, and a GC pass suspends the flash
// cache and stalls BOTH cores -- the fault behind #222 and the beta_25 bootloop.
// An app that ignores the advice in deploy/apps/README.md and writes every frame
// gets `false, "too fast"` instead of wedging the device. The guard covers the
// case where a write DOES trigger GC, so it stalls briefly instead of rebooting.
static int fsWriteCommon(lua_State* L, const char* mode) {
  const char* name = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  char path[128];
  if (!fsPath(path, sizeof path, name)) { lua_pushboolean(L, 0); lua_pushstring(L, "bad name"); return 2; }
  if (len > kFsMaxFile)                 { lua_pushboolean(L, 0); lua_pushstring(L, "too big");  return 2; }
  const uint32_t now = millis();
  if (s_h->fs_last_write_ms && (uint32_t)(now - s_h->fs_last_write_ms) < kFsMinGapMs) {
    lua_pushboolean(L, 0); lua_pushstring(L, "too fast"); return 2;
  }
  fs::FS* fs = luaHostAppFs();
  if (!fs) { lua_pushboolean(L, 0); lua_pushstring(L, "no storage"); return 2; }
  fsEnsureDir();
  bool ok = false;
  {
    WdtHeavyGuard guard;            // a GC pass here must stall, not reboot
    File f = fs->open(path, mode);
    if (f) { ok = (f.write((const uint8_t*)data, len) == len); f.close(); }
  }
  s_h->fs_last_write_ms = now;
  lua_pushboolean(L, ok);
  if (!ok) { lua_pushstring(L, "write failed"); return 2; }
  return 1;
}
int fsWrite(lua_State* L)  { return fsWriteCommon(L, "w"); }
int fsAppend(lua_State* L) { return fsWriteCommon(L, "a"); }

// wada.fs.read(name [, offset [, len]]) -> data, total_size
//
// A single read still hands back at most kFsMaxFile, because a Lua string that
// large already costs more than the app heap wants to spend. The optional
// offset is what makes that a window rather than a ceiling: an append-only log
// may grow past it, and a reader walks it in chunks using the returned total.
int fsRead(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_Integer off = luaL_optinteger(L, 2, 0);
  lua_Integer want = luaL_optinteger(L, 3, (lua_Integer)kFsMaxFile);
  char path[128];
  if (off < 0 || want < 0) { lua_pushnil(L); return 1; }
  if (!fsPath(path, sizeof path, name)) { lua_pushnil(L); return 1; }
  fs::FS* fs = luaHostAppFs();
  if (!fs) { lua_pushnil(L); return 1; }
  File f = fs->open(path, "r");
  if (!f) { lua_pushnil(L); return 1; }
  const size_t total = f.size();
  if ((size_t)off >= total) { f.close(); lua_pushstring(L, ""); lua_pushinteger(L, (lua_Integer)total); return 2; }
  if (off && !f.seek((uint32_t)off)) { f.close(); lua_pushnil(L); return 1; }
  size_t n = total - (size_t)off;
  if (n > (size_t)want)   n = (size_t)want;
  if (n > kFsMaxFile)     n = kFsMaxFile;         // never hand Lua an unbounded buffer
  luaL_Buffer b;
  char* dst = luaL_buffinitsize(L, &b, n);
  const size_t got = f.read((uint8_t*)dst, n);
  f.close();
  luaL_pushresultsize(&b, got);
  lua_pushinteger(L, (lua_Integer)total);
  return 2;
}
int fsRemove(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  char path[128];
  fs::FS* fs = luaHostAppFs();
  if (!fs || !fsPath(path, sizeof path, name)) { lua_pushboolean(L, 0); return 1; }
  lua_pushboolean(L, fs->remove(path));
  return 1;
}
int fsList(lua_State* L) {
  lua_newtable(L);
  fs::FS* fs = luaHostAppFs();
  if (!fs || !s_h) return 1;
  char d[80], rel[64];
  snprintf(rel, sizeof rel, "/apps/%s.d", s_h->id);
  luaHostAppPath(d, sizeof d, rel);
  File dir = fs->open(d);
  if (!dir || !dir.isDirectory()) return 1;
  int i = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (e.isDirectory()) continue;
    lua_createtable(L, 0, 2);
    const char* nm = strrchr(e.name(), '/');
    lua_pushstring(L, nm ? nm + 1 : e.name()); lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)e.size()); lua_setfield(L, -2, "size");
    lua_rawseti(L, -2, ++i);
  }
  dir.close();
  return 1;
}

#if CAP_LUA_SD_LIST
// ---- wada.sd: read-only physical SD directory listing ----------------------
// Unlike wada.fs, this intentionally accepts paths, but only absolute paths
// rooted on the card. Reject traversal rather than normalising it into a
// different valid path. Directory work is bounded so one huge card folder
// cannot consume an app's entire Lua heap.
static const size_t kSdMaxPath = 192;
static const int kSdMaxEntries = 192;

static bool sdSafePath(const char* in, size_t len, char* out, size_t cap) {
  if (!in || len == 0 || len >= cap || in[0] != '/') return false;
  if (len > 1 && in[len - 1] == '/') return false;
  if (len == 1) { out[0] = '/'; out[1] = '\0'; return true; }

  size_t segment = 1;
  for (size_t i = 1; i <= len; ++i) {
    if (i < len) {
      const unsigned char c = (unsigned char)in[i];
      if (c == 0 || c < 0x20 || c == 0x7F || c == '\\') return false;
      if (c != '/') continue;
    }
    const size_t n = i - segment;
    if (n == 0 || (n == 1 && in[segment] == '.') ||
        (n == 2 && in[segment] == '.' && in[segment + 1] == '.')) return false;
    segment = i + 1;
  }
  memcpy(out, in, len);
  out[len] = '\0';
  return true;
}

static int sdListError(lua_State* L, const char* error) {
  lua_pushnil(L);
  lua_pushstring(L, error);
  return 2;
}

// wada.sd.list(path) -> entries | nil,error
// entries is an array of { name, type = "file"|"dir", size, mtime } and gains
// entries.truncated = true only when the directory exceeds kSdMaxEntries.
int sdList(lua_State* L) {
  const char* requested = "/";
  size_t requested_len = 1;
  if (!lua_isnoneornil(L, 1)) requested = luaL_checklstring(L, 1, &requested_len);
  char path[kSdMaxPath];
  if (!sdSafePath(requested, requested_len, path, sizeof path))
    return sdListError(L, "bad path");

  bool busy = false;
  fs::FS* fs = luaHostSdFs(&busy);
  if (!fs) return sdListError(L, busy ? "busy" : "no sd");
  File dir = fs->open(path, "r");
  if (!dir) return sdListError(L, luaHostSdReadFailed() ? "no sd" : "not found");
  if (!dir.isDirectory()) { dir.close(); return sdListError(L, "not a directory"); }

  lua_newtable(L);
  int count = 0;
  bool truncated = false;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const char* full = entry.name();
    const char* base = full ? strrchr(full, '/') : nullptr;
    base = base ? base + 1 : full;
    if (!base || !base[0]) { entry.close(); continue; }
    if (count >= kSdMaxEntries) { truncated = true; entry.close(); break; }

    const bool is_dir = entry.isDirectory();
    lua_createtable(L, 0, 4);
    lua_pushstring(L, base);                         lua_setfield(L, -2, "name");
    lua_pushstring(L, is_dir ? "dir" : "file");  lua_setfield(L, -2, "type");
    lua_pushinteger(L, is_dir ? 0 : (lua_Integer)entry.size());
                                                     lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)entry.getLastWrite());
                                                     lua_setfield(L, -2, "mtime");
    lua_rawseti(L, -2, ++count);
    entry.close();
  }
  dir.close();
  if (truncated) { lua_pushboolean(L, 1); lua_setfield(L, -2, "truncated"); }
  return 1;
}
#endif
#endif  // CAP_LUA_SDK_EXT

#if CAP_LUA_AUDIO
static bool audioSafeName(const char* name, size_t len) {
  if (!name || len == 0 || len > 32 || name[0] == '.') return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = name[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static int audioError(lua_State* L, const char* error) {
  lua_pushnil(L);
  lua_pushstring(L, error);
  return 2;
}

// wada.audio.play(name) resolves inside /apps/<id>.d on the board's active
// storage backend. An explicit sd:/... path is available only with sd_list.
int audioPlay(lua_State* L) {
  if (!s_h) return audioError(L, "closed");
  size_t requested_len = 0;
  const char* requested = luaL_checklstring(L, 1, &requested_len);
  fs::FS* fs = nullptr;
  char path[224] = "";
  const char* source = "app";

  if (requested_len >= 3 && !memcmp(requested, "sd:", 3)) {
#if CAP_LUA_SD_LIST
    const char* card_path = requested + 3;
    const size_t card_len = requested_len - 3;
    if (!sdSafePath(card_path, card_len, path, sizeof path)) return audioError(L, "bad path");
    bool busy = false;
    fs = luaHostSdFs(&busy);
    if (!fs) return audioError(L, busy ? "busy" : "no sd");
    source = "sd";
#else
    return audioError(L, "no sd");
#endif
  } else {
    if (!audioSafeName(requested, requested_len)) return audioError(L, "bad path");
    fs = luaHostAppFs();
    if (!fs) return audioError(L, "no storage");
    char rel[96];
    snprintf(rel, sizeof rel, "/apps/%s.d/%s", s_h->id, requested);
    luaHostAppPath(path, sizeof path, rel);
  }

  char error[40] = "";
  if (!luaHostAudioPlay(fs, path, requested, source, s_h->generation, error, sizeof error))
    return audioError(L, error[0] ? error : "playback failed");
  lua_pushboolean(L, 1);
  return 1;
}

int audioPause(lua_State* L) {
  lua_pushboolean(L, s_h && luaHostAudioPause(s_h->generation, true));
  return 1;
}

int audioResume(lua_State* L) {
  lua_pushboolean(L, s_h && luaHostAudioPause(s_h->generation, false));
  return 1;
}

int audioStop(lua_State* L) {
  lua_pushboolean(L, s_h && luaHostAudioStop(s_h->generation, false));
  return 1;
}

int audioStatus(lua_State* L) {
  char state[12], path[192], source[8], format[8], error[40];
  luaHostAudioStatus(s_h ? s_h->generation : 0,
                     state, sizeof state, path, sizeof path,
                     source, sizeof source, format, sizeof format,
                     error, sizeof error);
  lua_createtable(L, 0, 5);
  lua_pushstring(L, state);  lua_setfield(L, -2, "state");
  lua_pushstring(L, path);   lua_setfield(L, -2, "path");
  lua_pushstring(L, source); lua_setfield(L, -2, "source");
  lua_pushstring(L, format); lua_setfield(L, -2, "format");
  if (error[0]) { lua_pushstring(L, error); lua_setfield(L, -2, "error"); }
  return 1;
}
#endif

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

// Key delivery reuses on_input rather than adding an on_key callback: an app
// already branches on ev.type, and one entry point keeps a Lua app's input model
// to a single place. type="key", with `key` as a one-character string for
// printable input and a name ("up", "enter", ...) for the rest, so an app can
// write ev.key == "w" without knowing scancodes.
void sendKey(int key) {
  if (!s_h || !s_h->L) return;
  if (!pushCallback(s_h, "on_input")) return;
  lua_State* L = s_h->L;
  lua_createtable(L, 0, 2);
  lua_pushstring(L, "key"); lua_setfield(L, -2, "type");
  char buf[8];
  const char* named = nullptr;
  switch (key) {
    case LV_KEY_UP:    named = "up";    break;
    case LV_KEY_DOWN:  named = "down";  break;
    case LV_KEY_LEFT:  named = "left";  break;
    case LV_KEY_RIGHT: named = "right"; break;
    case LV_KEY_ENTER: case '\r': named = "enter"; break;   // '\r': physical keyboards send CR, not LV_KEY_ENTER
    case LV_KEY_ESC:   named = "esc";   break;
    case '\b': case 127: named = "backspace"; break;
    default: break;
  }
  if (named) { lua_pushstring(L, named); }
  else if (key >= 32 && key < 127) { buf[0] = (char)key; buf[1] = '\0'; lua_pushstring(L, buf); }
  else { lua_pushnil(L); }
  lua_setfield(L, -2, "key");
  lua_pushinteger(L, key); lua_setfield(L, -2, "code");   // raw, for anything unmapped
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
  // Content coordinates, not viewport ones: once an app has turned on
  // ui.scroll() and the body is scrolled, a hit-test against where the app
  // PLACED its widgets only works if the scroll offset is folded in.
  sendInput(lv_event_get_code(e) == LV_EVENT_PRESSED ? "down" : "up", nullptr,
            p.x - a.x1 + lv_obj_get_scroll_x(s_h->body),
            p.y - a.y1 + lv_obj_get_scroll_y(s_h->body));
}

// ---- wada.mesh (read-only) ----
// wada.mesh.contacts([offset], [limit])
//
// Used to scan the first 200 contacts and return at most 100, with no way to see
// past that and nothing to say it had truncated. On a device with 2000 contacts
// an app simply could not reach most of them (reported by Jade).
//
// It takes an offset and a limit now. The default limit stays 100 because every
// entry is an 8-field Lua table and building thousands of them in one call would
// blow the per-app heap -- but the window can be moved, so all contacts are
// reachable by asking twice. wada.mesh.contact_count() says how many there are.
// How many contacts the device holds, so an app can page through them without
// calling contacts() repeatedly just to discover where the list ends.
static int meshContactCount(lua_State* L) {
  char name[36], pk[12]; int type; uint32_t ago; double lat, lon; int32_t lat6, lon6;
  int n = 0;
  while (n < 4096 && luaHostContactAt(n, name, sizeof name, &type, &ago, &lat, &lon,
                                      pk, sizeof pk, &lat6, &lon6)) ++n;
  lua_pushinteger(L, n);
  return 1;
}

int meshContacts(lua_State* L) {
  const int offset = (int)luaL_optinteger(L, 1, 0);
  int limit = (int)luaL_optinteger(L, 2, 100);
  if (limit < 1) limit = 1;
  if (limit > 250) limit = 250;      // one call still has to fit the app's heap
  lua_newtable(L);
  char name[36], pk[12]; int type; uint32_t ago; double lat, lon; int32_t lat6, lon6;
  for (int i = (offset > 0 ? offset : 0), out = 0; out < limit; i++) {
    if (!luaHostContactAt(i, name, sizeof name, &type, &ago, &lat, &lon, pk, sizeof pk,
                          &lat6, &lon6)) break;
    lua_createtable(L, 0, 8);
    lua_pushstring(L, name);          lua_setfield(L, -2, "name");
    lua_pushstring(L, pk);            lua_setfield(L, -2, "pubkey");
    lua_pushinteger(L, type);         lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)ago); lua_setfield(L, -2, "ago_s");
    lua_pushnumber(L, lat);           lua_setfield(L, -2, "lat");
    lua_pushnumber(L, lon);           lua_setfield(L, -2, "lon");
    lua_pushinteger(L, lat6);         lua_setfield(L, -2, "lat_e6");
    lua_pushinteger(L, lon6);         lua_setfield(L, -2, "lon_e6");
    lua_rawseti(L, -2, ++out);
  }
  return 1;
}
// wada.mesh.rx_log() -> newest-first list of frames the radio actually received.
//
// `pubkey` is set ONLY on an advert (type 4), where the frame really does carry
// the sender's public key -- that is the entry a coverage survey wants, since an
// advert both identifies a node and proves it was audible from here. Addressed
// frames instead expose `src`/`dst`, the one-byte hashes they carry; two hex
// characters collide easily, so treat them as a hint, not an identity. Anything
// else is anonymous on the wire and gets neither field.
int meshRxLog(lua_State* L) {
  lua_newtable(L);
  uint32_t ms_ago; int ptype, rssi, hops, route, flen, org_kind; float snr; char org[12];
  for (int i = 0, out = 0; i < 64; i++) {
    if (!luaHostRxLogAt(i, &ms_ago, &ptype, &rssi, &snr, &hops,
                        &route, &flen, &org_kind, org, sizeof org)) break;
    lua_createtable(L, 0, 9);
    lua_pushinteger(L, (lua_Integer)ms_ago); lua_setfield(L, -2, "ago_ms");
    lua_pushinteger(L, ptype);               lua_setfield(L, -2, "type");
    lua_pushinteger(L, rssi);                lua_setfield(L, -2, "rssi");
    lua_pushnumber(L, snr);                  lua_setfield(L, -2, "snr");
    lua_pushinteger(L, hops);                lua_setfield(L, -2, "hops");
    lua_pushinteger(L, route);               lua_setfield(L, -2, "route");
    lua_pushinteger(L, flen);                lua_setfield(L, -2, "len");
    if (org_kind == 1) {
      lua_pushstring(L, org); lua_setfield(L, -2, "pubkey");
    } else if (org_kind == 2 && strlen(org) >= 4) {
      lua_pushlstring(L, org, 2);     lua_setfield(L, -2, "dst");
      lua_pushlstring(L, org + 2, 2); lua_setfield(L, -2, "src");
    }
    lua_rawseti(L, -2, ++out);
  }
  return 1;
}
// ---- app.on_packet: push instead of poll -----------------------------------
// Polling rx_log() on a 1 Hz tick samples a 16-deep ring, so in any real traffic
// an app sees a subset and cannot count anything. This delivers each frame once,
// in arrival order, from a timer fast enough that the ring cannot wrap between
// passes -- the shortest LoRa frame is tens of milliseconds of airtime, so 16
// frames cannot arrive inside 250 ms on any supported setting.
//
// No new hook in the packet path: Dispatcher::checkRecv (and so logRxRaw) runs
// inside the_mesh.loop(), the same Arduino task as LVGL, but running Lua there
// would put an app's code inside packet reception. Draining on a timer keeps
// app code where all the other callbacks are.
//
// Ungated, exactly like rx_log(): this is radio metadata about frames the
// device already received, not message content. Anything carrying content still
// goes through on_message and its permissions.
lv_timer_t* s_pkt_poll = nullptr;
uint32_t    s_pkt_last_ms = 0;      // newest record timestamp already delivered
bool        s_pkt_primed = false;

void pushPacketTable(lua_State* L, uint32_t ago_ms, int ptype, int rssi, float snr, int hops,
                     int route, int flen, int org_kind, const char* org) {
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, (lua_Integer)ago_ms); lua_setfield(L, -2, "ago_ms");
  lua_pushinteger(L, ptype);               lua_setfield(L, -2, "type");
  lua_pushinteger(L, rssi);                lua_setfield(L, -2, "rssi");
  lua_pushnumber(L, snr);                  lua_setfield(L, -2, "snr");
  lua_pushinteger(L, hops);                lua_setfield(L, -2, "hops");
  lua_pushinteger(L, route);               lua_setfield(L, -2, "route");
  lua_pushinteger(L, flen);                lua_setfield(L, -2, "len");
  if (org_kind == 1) {
    lua_pushstring(L, org); lua_setfield(L, -2, "pubkey");
  } else if (org_kind == 2 && strlen(org) >= 4) {
    lua_pushlstring(L, org, 2);     lua_setfield(L, -2, "dst");
    lua_pushlstring(L, org + 2, 2); lua_setfield(L, -2, "src");
  }
}

void packetPollCb(lv_timer_t* t) {
  (void)t;
  if (!s_h || !s_h->L) return;
  // Snapshot the ring newest-first, then deliver oldest-first so an app sees
  // arrival order. 16 is the ring depth; reading past it just stops.
  struct Rec { uint32_t at, ago; int ptype, rssi, hops, route, len, kind; float snr; char org[12]; };
  Rec recs[16];
  int n = 0;
  uint32_t newest = s_pkt_last_ms;
  for (int i = 0; i < 16; i++) {
    Rec r;
    if (!luaHostRxLogAt(i, &r.ago, &r.ptype, &r.rssi, &r.snr, &r.hops,
                        &r.route, &r.len, &r.kind, r.org, sizeof r.org, &r.at)) break;
    // millis() wraps at 49 days; compare as a signed delta so the wrap does not
    // silently stop delivery for the rest of the session.
    if ((int32_t)(r.at - s_pkt_last_ms) <= 0) break;   // already delivered, and so is everything older
    if ((int32_t)(r.at - newest) > 0) newest = r.at;
    if (n < 16) recs[n++] = r;
  }
  s_pkt_last_ms = newest;
  if (!s_pkt_primed) { s_pkt_primed = true; return; }   // first pass only establishes the mark
  for (int i = n - 1; i >= 0; i--) {                    // oldest first
    if (!s_h || !s_h->L) return;
    if (!pushCallback(s_h, "on_packet")) return;
    pushPacketTable(s_h->L, recs[i].ago, recs[i].ptype, recs[i].rssi, recs[i].snr, recs[i].hops,
                    recs[i].route, recs[i].len, recs[i].kind, recs[i].org);
    guardedCall(s_h, 1);
    serviceDeferredClose();
  }
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
  char name[36], pk[12]; double lat, lon; int32_t lat6, lon6;
  luaHostSelfInfo(name, sizeof name, &lat, &lon, pk, sizeof pk, &lat6, &lon6);
  lua_createtable(L, 0, 6);
  lua_pushstring(L, name);  lua_setfield(L, -2, "name");
  lua_pushstring(L, pk);    lua_setfield(L, -2, "pubkey");
  lua_pushnumber(L, lat);   lua_setfield(L, -2, "lat");
  lua_pushnumber(L, lon);   lua_setfield(L, -2, "lon");
  lua_pushinteger(L, lat6); lua_setfield(L, -2, "lat_e6");
  lua_pushinteger(L, lon6); lua_setfield(L, -2, "lon_e6");
  return 1;
}

// wada.mesh.discovered() -> everything that has answered a probe this session.
//
// Each entry carries BOTH link directions: `snr` is how well we heard them,
// `their_snr` how well they heard us. Asymmetry is the normal case (their
// antenna and power are not ours) and it is the single most useful number a
// coverage survey can record -- "I can hear the repeater but it cannot hear me"
// is a different fact from "no coverage", and only a probe reply reveals it.
int meshDiscovered(lua_State* L) {
  lua_newtable(L);
  char pk[12], name[36];
  int type, rssi, hops, heard; float snr, their_snr; uint32_t first_ago, last_ago;
  const int n = luaHostDiscoverCount();
  for (int i = 0, out = 0; i < n; i++) {
    if (!luaHostDiscoverAt(i, pk, sizeof pk, name, sizeof name, &type, &rssi, &snr,
                           &their_snr, &hops, &first_ago, &last_ago, &heard)) break;
    lua_createtable(L, 0, 10);
    lua_pushstring(L, pk);                       lua_setfield(L, -2, "pubkey");
    if (name[0]) { lua_pushstring(L, name);      lua_setfield(L, -2, "name"); }
    lua_pushinteger(L, type);                    lua_setfield(L, -2, "type");
    lua_pushinteger(L, rssi);                    lua_setfield(L, -2, "rssi");
    lua_pushnumber(L, snr);                      lua_setfield(L, -2, "snr");
    lua_pushnumber(L, their_snr);                lua_setfield(L, -2, "their_snr");
    lua_pushinteger(L, hops);                    lua_setfield(L, -2, "hops");
    lua_pushboolean(L, hops == 0);               lua_setfield(L, -2, "direct");
    lua_pushinteger(L, (lua_Integer)first_ago);  lua_setfield(L, -2, "first_ms_ago");
    lua_pushinteger(L, (lua_Integer)last_ago);   lua_setfield(L, -2, "last_ms_ago");
    lua_pushinteger(L, heard);                   lua_setfield(L, -2, "heard");
    lua_rawseti(L, -2, ++out);
  }
  return 1;
}

// wada.mesh.discover_clear() -> forget every hit so far.
// A survey calls this between locations: without it, a node heard two streets
// back is still in the table and the next sample claims coverage it does not
// have. Ungated -- it only discards our own local observations.
int meshDiscoverClear(lua_State* L) { (void)L; luaHostDiscoverClear(); return 0; }

// ---- wada.net: one in-flight async http_get per app ----
constexpr size_t kNetCapMax = 32 * 1024;
char     s_net_url[160];
char*    s_net_buf = nullptr;
size_t   s_net_cap = 0;
char*    s_net_body = nullptr;      // POST payload (null = GET), PSRAM
size_t   s_net_body_len = 0;
char     s_net_ctype[64] = "";
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
  if (s_net_body) { heap_caps_free(s_net_body); s_net_body = nullptr; s_net_body_len = 0; }
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

// ---- wada.ui.input(title, initial, cb) --------------------------------------
// One modal text field, using the same dialog and on-screen keyboard the rest of
// the firmware uses -- so it behaves identically on a touchscreen, on the
// Tanmatsu's physical keyboard and on a trackball board, which an app drawing
// its own field could not manage.
//
// cb(text) on OK, cb(nil) on cancel or on the dialog being closed any other way.
// Exactly one call, always: an app that disabled itself while waiting for input
// will always get the chance to re-enable.
void promptDeliver(const char* text) {
  if (!s_h || !s_h->L || s_h->prompt_cb == LUA_NOREF) return;
  lua_State* L = s_h->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, s_h->prompt_cb);
  luaL_unref(L, LUA_REGISTRYINDEX, s_h->prompt_cb);
  s_h->prompt_cb = LUA_NOREF;
  if (text) lua_pushstring(L, text); else lua_pushnil(L);
  guardedCall(s_h, 1);
  serviceDeferredClose();
}

int uiInput(lua_State* L) {
  size_t title_length = 0, initial_length = 0;
  const char* title_raw = luaL_optlstring(L, 1, "", &title_length);
  const char* initial_raw = luaL_optlstring(L, 2, "", &initial_length);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (!s_h) return luaL_error(L, "no app");
  if (s_h->prompt_cb != LUA_NOREF) return luaL_error(L, "an input prompt is already open");
  const char* title = safeUiText(title_raw, title_length, nullptr, 0);
  const char* initial = safeUiText(initial_raw, initial_length, nullptr, 1);
  lua_pushvalue(L, 3);
  s_h->prompt_cb = luaL_ref(L, LUA_REGISTRYINDEX);
  luaHostTextPrompt(title, initial, promptDeliver);
  return 0;
}

// wada.net.http_post(url, body [, content_type], cb [, max_reply])
//
// The counterpart to http_get, and the call that lets an app get data OFF the
// device: a survey log, a sensor series, a webhook. Same single-in-flight rule
// and same http:// restriction as http_get -- on-device TLS is not workable at
// the heap these boards have left once Wi-Fi has associated.
int netHttpPost(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  size_t blen = 0;
  const char* body = luaL_checklstring(L, 2, &blen);
  int argi = 3;
  const char* ctype = "application/octet-stream";
  if (lua_isstring(L, argi) && !lua_isfunction(L, argi)) ctype = lua_tostring(L, argi++);
  luaL_checktype(L, argi, LUA_TFUNCTION);
  size_t maxb = (size_t)luaL_optinteger(L, argi + 1, 4 * 1024);
  if (maxb > kNetCapMax) maxb = kNetCapMax;
  if (strncmp(url, "http://", 7) != 0) return luaL_error(L, "http:// urls only");
  if (strlen(url) >= sizeof(s_net_url)) return luaL_error(L, "url too long");
  if (blen == 0 || blen > kNetCapMax)   return luaL_error(L, "bad body length");
  if (s_net_pending || s_net_cb != LUA_NOREF) return luaL_error(L, "a fetch is already running");
  if (!s_net_buf || s_net_cap < maxb + 1) {
    if (s_net_buf) heap_caps_free(s_net_buf);
    s_net_buf = (char*)heap_caps_malloc(maxb + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_net_cap = s_net_buf ? maxb + 1 : 0;
    if (!s_net_buf) return luaL_error(L, "no memory for the reply buffer");
  }
  // The worker runs on another task, so the payload cannot stay a Lua string:
  // a collection between here and the send would pull it out from under it.
  if (s_net_body) heap_caps_free(s_net_body);
  s_net_body = (char*)heap_caps_malloc(blen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_net_body) { s_net_body_len = 0; return luaL_error(L, "no memory for the body"); }
  memcpy(s_net_body, body, blen);
  s_net_body_len = blen;
  snprintf(s_net_ctype, sizeof s_net_ctype, "%s", ctype);
  snprintf(s_net_url, sizeof s_net_url, "%s", url);
  lua_pushvalue(L, argi);
  s_net_cb = luaL_ref(L, LUA_REGISTRYINDEX);
  s_net_done = false;
  s_net_pending = true;
  if (!s_net_poll) s_net_poll = lv_timer_create(netDeliver, 120, nullptr);
  return 0;
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
  if (s_net_body) { heap_caps_free(s_net_body); s_net_body = nullptr; s_net_body_len = 0; }
  snprintf(s_net_url, sizeof s_net_url, "%s", url);
  lua_pushvalue(L, 2);
  s_net_cb = luaL_ref(L, LUA_REGISTRYINDEX);
  s_net_done = false;
  s_net_pending = true;
  if (!s_net_poll) s_net_poll = lv_timer_create(netDeliver, 120, nullptr);
  return 0;
}

// ---- wada.crypto ------------------------------------------------------------
// Hashing belongs in C. An OTP app doing HMAC-SHA1 in pure Lua blows the 100k
// per-callback instruction budget legitimately — SHA-1 is 80 bit-twiddling rounds
// per 64-byte block, and the interpreter overhead around each one multiplies it
// (reported from a real app: "otp_messenger:941: instruction budget exceeded").
// The chip already links mbedTLS, so the same work costs microseconds and a
// handful of VM instructions here.
//
// NOT gated on CAP_LUA_SDK_EXT: this is pure computation over data the app already
// holds. It reads nothing of the user's, transmits nothing, and the mbedTLS context
// is ~100 bytes — so there is no reason to deny it to the small boards, and an app
// that needs a hash should not have to check a capability first.
//
// Strings in and out are BINARY (Lua strings are 8-bit clean), so an app gets the
// raw digest and can do RFC 4226 dynamic truncation on it directly.
#if defined(ESP32)
static int cryptoDigest(lua_State* L, mbedtls_md_type_t type, bool hmac) {
  size_t klen = 0, mlen = 0;
  const char* key = hmac ? luaL_checklstring(L, 1, &klen) : nullptr;
  const char* msg = luaL_checklstring(L, hmac ? 2 : 1, &mlen);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
  if (!info) { lua_pushnil(L); lua_pushstring(L, "digest unavailable"); return 2; }
  unsigned char out[32];
  const unsigned char olen = mbedtls_md_get_size(info);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  int rc = mbedtls_md_setup(&ctx, info, hmac ? 1 : 0);
  if (rc == 0) {
    if (hmac) {
      rc = mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, klen);
      if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg, mlen);
      if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);
    } else {
      rc = mbedtls_md_starts(&ctx);
      if (rc == 0) rc = mbedtls_md_update(&ctx, (const unsigned char*)msg, mlen);
      if (rc == 0) rc = mbedtls_md_finish(&ctx, out);
    }
  }
  mbedtls_md_free(&ctx);
  if (rc != 0) { lua_pushnil(L); lua_pushstring(L, "digest failed"); return 2; }
  lua_pushlstring(L, (const char*)out, olen);
  return 1;
}
int cryptoSha256(lua_State* L)     { return cryptoDigest(L, MBEDTLS_MD_SHA256, false); }
int cryptoSha1(lua_State* L)       { return cryptoDigest(L, MBEDTLS_MD_SHA1,   false); }
int cryptoHmacSha256(lua_State* L) { return cryptoDigest(L, MBEDTLS_MD_SHA256, true);  }
int cryptoHmacSha1(lua_State* L)   { return cryptoDigest(L, MBEDTLS_MD_SHA1,   true);  }

// Lowercase hex of a binary string — the one conversion every app writes badly.
int cryptoHex(lua_State* L) {
  size_t n = 0;
  const unsigned char* p = (const unsigned char*)luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  static const char* const kHex = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    luaL_addchar(&b, kHex[p[i] >> 4]);
    luaL_addchar(&b, kHex[p[i] & 0x0F]);
  }
  luaL_pushresult(&b);
  return 1;
}
#endif

// ---- module registration ----
void openWada(lua_State* L) {
  lua_newtable(L);                                       // wada

  lua_newtable(L);                                       // wada.ui
  lua_pushcfunction(L, uiCanvas); lua_setfield(L, -2, "canvas");
  lua_pushcfunction(L, uiLabel);  lua_setfield(L, -2, "label");
  lua_pushcfunction(L, uiButton); lua_setfield(L, -2, "button");
  lua_pushcfunction(L, uiChart);  lua_setfield(L, -2, "chart");
  lua_pushcfunction(L, uiScroll); lua_setfield(L, -2, "scroll");
  lua_pushcfunction(L, uiTextH);  lua_setfield(L, -2, "text_h");
  lua_pushcfunction(L, uiTextW);     lua_setfield(L, -2, "text_w");
  lua_pushcfunction(L, uiTextLines); lua_setfield(L, -2, "text_lines");
  lua_createtable(L, 0, 7);                              // wada.ui.colors (theme)
  lua_pushinteger(L, 0x15B6A6); lua_setfield(L, -2, "accent");
  lua_pushinteger(L, 0xE6E9ED); lua_setfield(L, -2, "text");
  lua_pushinteger(L, 0x7A7F87); lua_setfield(L, -2, "sub");
  lua_pushinteger(L, 0x000000); lua_setfield(L, -2, "bg");      // the page ground (matches the firmware)
  lua_pushinteger(L, 0x15181B); lua_setfield(L, -2, "panel");   // a raised surface on top of it
  lua_pushinteger(L, 0xD7574E); lua_setfield(L, -2, "bad");
  lua_pushinteger(L, 0x53C06B); lua_setfield(L, -2, "good");
  lua_setfield(L, -2, "colors");
  lua_pushcfunction(L, uiInput); lua_setfield(L, -2, "input");   // modal text entry
  lua_pushcfunction(L, uiList);  lua_setfield(L, -2, "list");    // scrollable selectable rows
  lua_pushcfunction(L, uiClear); lua_setfield(L, -2, "clear");   // wipe the page (#318 / Discord)
  lua_setfield(L, -2, "ui");

  lua_newtable(L);                                       // wada.sys
  lua_pushcfunction(L, sysMillis); lua_setfield(L, -2, "millis");
  lua_pushcfunction(L, sysToast);  lua_setfield(L, -2, "toast");
  lua_pushcfunction(L, sysKeepAwake); lua_setfield(L, -2, "keep_awake");
  lua_pushcfunction(L, sysBoard);  lua_setfield(L, -2, "board");
  lua_pushcfunction(L, sysTr);     lua_setfield(L, -2, "tr");      // #257
  lua_pushcfunction(L, sysRandom); lua_setfield(L, -2, "random");
  lua_pushcfunction(L, sysEpoch);    lua_setfield(L, -2, "epoch");     // #245
  lua_pushcfunction(L, sysDatetime); lua_setfield(L, -2, "datetime");  // #245
  lua_pushcfunction(L, sysBeep);     lua_setfield(L, -2, "beep");      // #245
  lua_pushcfunction(L, sysCaps);     lua_setfield(L, -2, "caps");      // feature detection
#if CAP_LUA_SDK_EXT
  lua_pushcfunction(L, sysBattery);  lua_setfield(L, -2, "battery");
  lua_pushcfunction(L, sysGps);      lua_setfield(L, -2, "gps");
#endif
#if CAP_COMPASS
  lua_pushcfunction(L, sysCompass);  lua_setfield(L, -2, "compass");   // hardware-gated, see caps().compass
#endif
#if CAP_IMU
  lua_pushcfunction(L, sysAccel);    lua_setfield(L, -2, "accel");     // tilt, see caps().accel
#endif
#if CAP_SENSORS
  lua_pushcfunction(L, sysEnv);      lua_setfield(L, -2, "env");   // HARDWARE gate, not the memory one
#endif
  lua_setfield(L, -2, "sys");

  lua_newtable(L);                                       // wada.timer
  lua_pushcfunction(L, timerEvery); lua_setfield(L, -2, "every");
  lua_pushcfunction(L, timerAfter); lua_setfield(L, -2, "after");
  lua_pushcfunction(L, timerStop);  lua_setfield(L, -2, "stop");
  lua_setfield(L, -2, "timer");

#if CAP_LUA_SDK_EXT
  lua_newtable(L);                                       // wada.map
  lua_pushcfunction(L, mapView); lua_setfield(L, -2, "view");
  lua_setfield(L, -2, "map");
#endif

  lua_newtable(L);                                       // wada.geo (all boards)
  lua_pushcfunction(L, geoDistance); lua_setfield(L, -2, "distance");
  lua_pushcfunction(L, geoBearing);  lua_setfield(L, -2, "bearing");
  lua_pushcfunction(L, geoCardinal); lua_setfield(L, -2, "cardinal");
  lua_setfield(L, -2, "geo");

  lua_newtable(L);                                       // wada.store
  lua_pushcfunction(L, storeGet); lua_setfield(L, -2, "get");
  lua_pushcfunction(L, storeSet); lua_setfield(L, -2, "set");
  lua_setfield(L, -2, "store");

#if CAP_LUA_SDK_EXT
  lua_newtable(L);                                       // wada.fs (scoped to /apps/<id>.d)
  lua_pushcfunction(L, fsRead);   lua_setfield(L, -2, "read");
  lua_pushcfunction(L, fsWrite);  lua_setfield(L, -2, "write");
  lua_pushcfunction(L, fsAppend); lua_setfield(L, -2, "append");
  lua_pushcfunction(L, fsList);   lua_setfield(L, -2, "list");
  lua_pushcfunction(L, fsRemove); lua_setfield(L, -2, "remove");
  lua_setfield(L, -2, "fs");
#endif

#if CAP_LUA_SD_LIST
  lua_newtable(L);                                       // wada.sd (read-only physical card)
  lua_pushcfunction(L, sdList); lua_setfield(L, -2, "list");
  lua_setfield(L, -2, "sd");
#endif

#if CAP_LUA_AUDIO
  lua_newtable(L);                                       // wada.audio (app storage + optional SD)
  lua_pushcfunction(L, audioPlay);   lua_setfield(L, -2, "play");
  lua_pushcfunction(L, audioPause);  lua_setfield(L, -2, "pause");
  lua_pushcfunction(L, audioResume); lua_setfield(L, -2, "resume");
  lua_pushcfunction(L, audioStop);   lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, audioStatus); lua_setfield(L, -2, "status");
  lua_setfield(L, -2, "audio");
#endif

  lua_newtable(L);                                       // wada.mesh (read-only)
  lua_pushcfunction(L, meshContacts); lua_setfield(L, -2, "contacts");
  lua_pushcfunction(L, meshContactCount); lua_setfield(L, -2, "contact_count");
  lua_pushcfunction(L, meshRxLog);    lua_setfield(L, -2, "rx_log");
  lua_pushcfunction(L, meshStats);    lua_setfield(L, -2, "stats");
  lua_pushcfunction(L, meshSelf);     lua_setfield(L, -2, "self");
  lua_pushcfunction(L, meshDiscovered);    lua_setfield(L, -2, "discovered");
  lua_pushcfunction(L, meshDiscoverClear); lua_setfield(L, -2, "discover_clear");
  // Node-type bits, for the discover() filter and the `type` field everything
  // else returns. Named so an app never has to hardcode the wire numbers.
  lua_pushinteger(L, 1); lua_setfield(L, -2, "NODE_CHAT");
  lua_pushinteger(L, 2); lua_setfield(L, -2, "NODE_REPEATER");
  lua_pushinteger(L, 3); lua_setfield(L, -2, "NODE_ROOM");
  lua_pushinteger(L, 4); lua_setfield(L, -2, "NODE_SENSOR");
#if CAP_LUA_SDK_EXT
  lua_pushcfunction(L, meshSend);     lua_setfield(L, -2, "send");      // consent-gated (channels)
  lua_pushcfunction(L, meshSendDm);   lua_setfield(L, -2, "send_dm");   // consent-gated (DMs + rooms)
  lua_pushcfunction(L, meshChannels); lua_setfield(L, -2, "channels");  // names only, no secrets
  lua_pushcfunction(L, meshDiscover); lua_setfield(L, -2, "discover");  // consent-gated (transmits)
#endif
  lua_setfield(L, -2, "mesh");

#if defined(ESP32)
  lua_newtable(L);                                       // wada.crypto (all boards)
  lua_pushcfunction(L, cryptoSha256);     lua_setfield(L, -2, "sha256");
  lua_pushcfunction(L, cryptoSha1);       lua_setfield(L, -2, "sha1");
  lua_pushcfunction(L, cryptoHmacSha256); lua_setfield(L, -2, "hmac_sha256");
  lua_pushcfunction(L, cryptoHmacSha1);   lua_setfield(L, -2, "hmac_sha1");
  lua_pushcfunction(L, cryptoHex);        lua_setfield(L, -2, "hex");
  lua_setfield(L, -2, "crypto");
#endif

  lua_newtable(L);                                       // wada.net
  lua_pushcfunction(L, netHttpGet);  lua_setfield(L, -2, "http_get");
  lua_pushcfunction(L, netHttpPost); lua_setfield(L, -2, "http_post");
  lua_setfield(L, -2, "net");

  lua_setglobal(L, "wada");

  // metatables
#if CAP_LUA_SDK_EXT
  luaL_newmetatable(L, "wada.map");
  lua_newtable(L);
  lua_pushcfunction(L, mpCenter);   lua_setfield(L, -2, "center");
  lua_pushcfunction(L, mpZoom);     lua_setfield(L, -2, "zoom");
  lua_pushcfunction(L, mpMarker);   lua_setfield(L, -2, "marker");
  lua_pushcfunction(L, mpLine);     lua_setfield(L, -2, "line");
  lua_pushcfunction(L, mpClear);    lua_setfield(L, -2, "clear");
  lua_pushcfunction(L, mpRedraw);   lua_setfield(L, -2, "redraw");
  lua_pushcfunction(L, mpTiles);    lua_setfield(L, -2, "tiles");
  lua_pushcfunction(L, mpToScreen); lua_setfield(L, -2, "to_screen");
  lua_pushcfunction(L, mpToLatLon); lua_setfield(L, -2, "to_latlon");
  lua_pushcfunction(L, mpClose);    lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, mpGc);       lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
#endif

  luaL_newmetatable(L, "wada.list");
  lua_newtable(L);
  lua_pushcfunction(L, lsAdd);      lua_setfield(L, -2, "add");
  lua_pushcfunction(L, lsSet);      lua_setfield(L, -2, "set");
  lua_pushcfunction(L, lsColor);    lua_setfield(L, -2, "color");
  lua_pushcfunction(L, lsClear);    lua_setfield(L, -2, "clear");
  lua_pushcfunction(L, lsCount);    lua_setfield(L, -2, "count");
  lua_pushcfunction(L, lsSelect);   lua_setfield(L, -2, "select");
  lua_pushcfunction(L, lsSelected); lua_setfield(L, -2, "selected");
  lua_pushcfunction(L, lsPos);      lua_setfield(L, -2, "pos");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, "wada.timerh");
  lua_newtable(L);
  lua_pushcfunction(L, timerHandleStop); lua_setfield(L, -2, "stop");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

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
  lua_pushcfunction(L, chAxis);   lua_setfield(L, -2, "axis");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, "wada.label");
  lua_newtable(L);
  lua_pushcfunction(L, lbSet);   lua_setfield(L, -2, "set");
  lua_pushcfunction(L, lbPos);   lua_setfield(L, -2, "pos");
  lua_pushcfunction(L, lbColor); lua_setfield(L, -2, "color");
  lua_pushcfunction(L, lbWidth); lua_setfield(L, -2, "width");
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

// The app's content area, so UITask can parent a map view into it. External
// linkage on purpose: UITask owns the map machinery and this file owns the app.
lv_obj_t* luaHostAppBody() { return s_h ? s_h->body : nullptr; }

// Returns true when the running app took the key. False lets the firmware keep
// its own handling, so an app that does not implement on_input changes nothing.
#if CAP_LUA_SDK_EXT
// Incoming channel message -> app.on_message{channel, sender, text}. Permission
// checked HERE rather than at subscribe time, so revoking it in Settings stops
// delivery immediately instead of at the next launch.
// Each kind is behind its OWN grant, rechecked per message so revoking in Settings
// stops delivery at once rather than at the app's next launch. Channel traffic is
// semi-public and rides LUA_PERM_READ; direct messages and room posts are private
// and need LUA_PERM_DM_READ, which is never auto-prompted (it would fire on someone
// else's traffic arriving) and is granted only from Settings > App permissions.
static void deliverMessage(const char* kind, const char* channel, const char* sender, const char* text) {
  if (!s_h || !s_h->L) return;
  const bool is_channel = (kind && strcmp(kind, "channel") == 0);
  if (is_channel) { if (!luaHostReadPerm(s_h->id))   return; }
  else            { if (!luaHostDmReadPerm(s_h->id)) return; }
  if (!pushCallback(s_h, "on_message")) return;
  lua_State* L = s_h->L;
  lua_createtable(L, 0, 4);
  lua_pushstring(L, kind    ? kind    : ""); lua_setfield(L, -2, "kind");
  lua_pushstring(L, channel ? channel : ""); lua_setfield(L, -2, "channel");
  lua_pushstring(L, sender  ? sender  : ""); lua_setfield(L, -2, "sender");
  lua_pushstring(L, text    ? text    : ""); lua_setfield(L, -2, "text");
  guardedCall(s_h, 1);
  serviceDeferredClose();
}
#endif

bool luaAppKey(int key) {
  if (!s_h || !s_h->L || key <= 0) return false;
  sendKey(key);
  return true;
}

#if CAP_LUA_SDK_EXT
void luaAppMessage(const char* kind, const char* channel, const char* sender, const char* text) {
  deliverMessage(kind, channel, sender, text);
}
#endif

void luaAppSteer(int dx, int dy) {
  if (!s_h) return;
  const char* dir = (dy < 0) ? "up" : (dy > 0) ? "down" : (dx < 0) ? "left" : (dx > 0) ? "right" : nullptr;
  if (dir) sendInput("swipe", dir, 0, 0);
}

// Synthetic centre tap (down then up) for boards with no touch: the select key
// stands in for "tap to start / retry" flows that listen for type=="down".
void luaAppPress() {
  if (!s_h || !s_h->body) return;
  const int x = lv_obj_get_width(s_h->body) / 2;
  const int y = lv_obj_get_height(s_h->body) / 2;
  sendInput("down", nullptr, x, y);
  sendInput("up",   nullptr, x, y);
}

// True when the running app declares on_input. Touchless boards use this to
// decide whether the d-pad belongs to the app (steer/press events) or should
// keep its native meaning so it can drive the app page's own LVGL widgets —
// display-only apps (Airtime, RF Monitor) otherwise ate every key into a
// callback that doesn't exist.
bool luaAppHasOnInput() {
  if (!s_h || !s_h->L) return false;
  if (!pushCallback(s_h, "on_input")) return false;
  lua_pop(s_h->L, 1);
  return true;
}

// Page-scroll the app body (display-only apps: RF Monitor's feed runs past the
// screen and there is no touch to drag it). Returns true while an app is open
// — even at the scroll edge the key belongs to the page, not to whatever is
// underneath it.
bool luaAppScroll(bool up) {
  if (!s_h || !s_h->body) return false;
  const lv_coord_t room = up ? lv_obj_get_scroll_top(s_h->body)
                             : lv_obj_get_scroll_bottom(s_h->body);
  if (room <= 0) return true;   // at the edge / page not scrollable
  lv_coord_t step = (lv_coord_t)((s_h->body_h * 3) / 5);
  if (step > room) step = room;
  const lv_coord_t cur = lv_obj_get_scroll_y(s_h->body);
  lv_obj_scroll_to_y(s_h->body, up ? cur - step : cur + step, LV_ANIM_ON);
  return true;
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
#if CAP_LUA_AUDIO
  luaHostAudioStop(h->generation, true);
#endif
  // A wada.ui.input dialog lives on lv_layer_top, so it would outlive the app
  // that opened it. Drop it before the state goes, not after.
  luaHostTextPromptDismiss();
  if (h->L && h->prompt_cb != LUA_NOREF) luaL_unref(h->L, LUA_REGISTRYINDEX, h->prompt_cb);
  h->prompt_cb = LUA_NOREF;
  if (s_pkt_poll) { lv_timer_del(s_pkt_poll); s_pkt_poll = nullptr; }
#if CAP_LUA_SDK_EXT
  // A map view is torn down with the app body below, so its userdata __gc must
  // not free it a second time. Zeroing the count here also lets the next app
  // open a view even if the previous one never called :close().
  s_map_views = 0;
#endif
  // Named timers hold both an lv_timer and a registry ref; both go now, while
  // the state is still alive to unref against.
  for (int i = 0; i < Host::kMaxTimers; i++) {
    if (h->timers[i].t) { lv_timer_del(h->timers[i].t); h->timers[i].t = nullptr; }
    if (h->timers[i].cb != LUA_NOREF && h->L) luaL_unref(h->L, LUA_REGISTRYINDEX, h->timers[i].cb);
    h->timers[i].cb = LUA_NOREF;
    s_timer_gen[i]++;              // any handle Lua still holds is now inert
  }
  luaHostKeepAwake(false);          // an app cannot hold the screen after it closes
  if (h->timer) { lv_timer_del(h->timer); h->timer = nullptr; }
  if (s_net_poll) { lv_timer_del(s_net_poll); s_net_poll = nullptr; }
  if (h->L && s_net_cb != LUA_NOREF) { luaL_unref(h->L, LUA_REGISTRYINDEX, s_net_cb); }
  s_net_cb = LUA_NOREF;              // a worker fetch may still land; netDeliver sees no app and drops it
  // The fetch buffer (up to 32 KB of PSRAM) and the POST body belong to a fetch,
  // and with no app there is no fetch -- hold them only while the worker could
  // still be reading them (s_net_pending), otherwise they sit allocated until
  // some later app happens to call http_get/http_post.
  if (!s_net_pending) {
    if (s_net_buf)  { heap_caps_free(s_net_buf);  s_net_buf  = nullptr; s_net_cap = 0; }
    if (s_net_body) { heap_caps_free(s_net_body); s_net_body = nullptr; s_net_body_len = 0; }
  }
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
    Serial.printf("[LUAAPP] %s closed, leaked=%u peak=%u psram_free=%u\n", h->id,
                  (unsigned)h->heap.used, (unsigned)h->heap.peak,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
  h->generation = ++s_host_generation;
  if (!h->generation) h->generation = ++s_host_generation;
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
  // Pure black, the same ground the rest of the firmware paints (COLOR_BG in
  // UITask.cpp) — an app page must not read as a lighter panel floating over
  // the UI. Apps that want a raised surface draw one (wada.ui.colors.panel).
  lv_obj_set_style_bg_color(h->root, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(h->root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(h->root, LV_OBJ_FLAG_SCROLLABLE);
  // A plain lv_obj is CLICKABLE by default, and it has to stay so (it is the
  // overlay that keeps touches off the screen underneath) — but it must not be
  // a keyboard-nav focus target either, or the focus highlight paints it
  // solid: with only the body excluded, navCollect simply promoted the root to
  // the leaf target and the app went white all the same (seen on the M9).
  lv_obj_add_flag(h->root, NAV_PASSTHRU_FLAG);
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
  // Clickable for touch, but never a keyboard-nav focus target: on the M9 the
  // nav collector harvested this body as a leaf and the focus highlight's
  // reverse-video fill painted the whole app white under its widgets. The
  // flag leaves the app's own buttons reachable (see AppPage.h).
  lv_obj_add_flag(h->body, NAV_PASSTHRU_FLAG);
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

  // Only run the packet drain for apps that asked for it. Checked once here
  // rather than per pass, so an app without on_packet costs nothing at all.
  if (pushCallback(h, "on_packet")) {
    lua_pop(h->L, 1);
    s_pkt_last_ms = 0;
    s_pkt_primed  = false;    // the first pass marks the ring instead of replaying it
    s_pkt_poll = lv_timer_create(packetPollCb, 250, nullptr);
  }

  if (pushCallback(h, "on_open")) {
    lua_pushinteger(h->L, h->body_w);
    lua_pushinteger(h->L, h->body_h);
    guardedCall(h, 2);
  }
  serviceDeferredClose();
  // PSRAM is reported alongside the Lua heap because the app heap does NOT
  // account for canvas pixel buffers (allocated directly), so it cannot show
  // whether an app really gave everything back. Compare this against the same
  // figure on the matching "closed" line.
  if (s_h) Serial.printf("[LUAAPP] %s open, heap=%u psram_free=%u\n", id, (unsigned)h->heap.used,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
void luaNetWorkerService(void* client, void* http) {
  // s_net_pending stays TRUE for the whole request and is cleared at the end,
  // not here. It is the only thing telling the UI thread that the worker may
  // still be reading s_net_buf / s_net_body: clearing it first left a window
  // the length of an HTTP round trip in which an app closing (hostTeardown)
  // would free both buffers out from under this function. The caller runs one
  // service() per loop pass and re-checks the flag afterwards, so holding it
  // across the call cannot re-enter, and http_get/http_post keep rejecting a
  // second fetch while it is set -- which is what we want anyway.
  if (!s_net_buf)            s_net_result = -1;
  else if (s_net_body)       s_net_result = luaStoreHttpPostOpaque(client, http, s_net_url, s_net_buf,
                                                                   s_net_cap, s_net_body, s_net_body_len,
                                                                   s_net_ctype);
  else                       s_net_result = luaStoreHttpGetOpaque(client, http, s_net_url, s_net_buf, s_net_cap);
  // Order matters: drop the in-flight flag BEFORE announcing the result, so a
  // teardown that observes done=true can never also observe pending=true and
  // decide to hold the buffers forever. Between the two writes the request is
  // finished and s_net_result is set, and netDeliver drops the payload without
  // touching s_net_buf when the app is already gone.
  s_net_pending = false;
  s_net_done = true;
}

#endif  // CAP_LUA_APPS
