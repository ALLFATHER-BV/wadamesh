// WebSettingsEngine.h -- the web console's Settings tab (#854).
//
// Included once, at the end of UITask.cpp (after the popup registry), and nowhere else.
//
// The browser does not get a second, hand-written copy of every setting. It drives the
// device's own Settings pages instead: for each request the page is built out of sight
// (a hidden root, exactly as openSettingsCategory builds it), its widget tree is walked
// and described as JSON (the kit rows note themselves while s_wc.on, see wcRec; anything
// else is read from its LVGL class), and a change is made by setting the widget and
// sending the same LVGL event a tap would. So a setting added to a page shows up on the
// web with nothing more to do, and saves through the code the device itself uses.
//
// Everything for one request runs inside one UI-loop call (build, replay the path into
// sub-pages, apply, describe, tear down), so nothing is drawn in between. Replies go
// out over the /term data channel as {"t":"cfg","q":<request id>,"k":<kind>,...}.
//
// Protocol (one command at a time; the browser waits for each reply):
//   @cfg cats <q>                       -> k:"cats"   the categories this board has
//   @cfg clr <q>                        -> k:"ok"     drop staged values
//   @cfg set <q> <idx> <value...>       -> k:"ok"     stage a value for item idx
//   @cfg view <q> <cat> <path>          -> k:"view", k:"items"..., k:"end"
//   @cfg run <q> <cat> <path> <idx> <n> <confirm>
//        applies staged values, then clicks item idx (-1 = Save: presses the page's
//        own Save/Apply). <n> = the item count the browser saw (page changed if not).
//        -> k:"confirm" (m), a new view (as above), or k:"done" (al, pt, cl, op)
//   <path> is "-" or dot-separated item indices clicked from the page to reach a view.
#if !defined(HAS_TANMATSU)

enum : uint8_t { WCI_TILE = 13, WCI_DOT = 14 };   // after the WCK_* row kinds
struct WcItem { lv_obj_t* obj; const char* t; const char* h; int16_t row; uint8_t kind; uint8_t grp; };
struct WcView { lv_obj_t* root; int reg; void (*sub_close)(); };
struct WcStage { int16_t idx; uint16_t at; };

static const int WC_MAX_ITEMS = 400;
static const int WC_MAX_GRP   = 64;
static const int WC_MAX_VIEWS = 5;
static const int WC_STAGE_N   = 64;
static const int WC_STAGE_ARENA = 4096;
static const int WC_MSG       = 10240;
static const int WC_CHUNK     = 6144;
static const int WC_QUEUE     = 96 * 1024;
static const int WC_ALERTS    = 512;

static WcItem*   s_wci = nullptr;
static int       s_wci_n = 0;
static bool      s_wci_more = false;
static lv_obj_t* s_wc_grp[WC_MAX_GRP];
static int       s_wc_grp_n = 0;
static WcView    s_wcv[WC_MAX_VIEWS];
static int       s_wcv_n = 0;
static WcStage   s_wcs[WC_STAGE_N];
static int       s_wcs_n = 0;
static char*     s_wcs_arena = nullptr;
static int       s_wcs_an = 0;
static char*     s_wc_msg = nullptr;
static char*     s_wcq = nullptr;
static int       s_wcq_cap = 0, s_wcq_rd = 0, s_wcq_wr = 0;
static char*     s_wc_alerts = nullptr;
static int       s_wc_al_n = 0;
static bool      s_wc_alert_on = false;
static unsigned  s_wc_q = 0;
static char      s_wc_confirm_msg[256];

static void* wcAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
static bool wcEnsure() {
  if (!s_wc.rows)    s_wc.rows   = (WcRow*)wcAlloc(sizeof(WcRow) * WC_MAX_ROWS);
  if (!s_wc.arena)   s_wc.arena  = (char*)wcAlloc(WC_ARENA);
  if (!s_wci)        s_wci       = (WcItem*)wcAlloc(sizeof(WcItem) * WC_MAX_ITEMS);
  if (!s_wcs_arena)  s_wcs_arena = (char*)wcAlloc(WC_STAGE_ARENA);
  if (!s_wc_msg)     s_wc_msg    = (char*)wcAlloc(WC_MSG);
  if (!s_wc_alerts)  s_wc_alerts = (char*)wcAlloc(WC_ALERTS);
  if (!s_wcq) {
    s_wcq = (char*)heap_caps_malloc(WC_QUEUE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wcq_cap = s_wcq ? WC_QUEUE : 0;
    if (!s_wcq) { s_wcq = (char*)malloc(24 * 1024); s_wcq_cap = s_wcq ? 24 * 1024 : 0; }
  }
  return s_wc.rows && s_wc.arena && s_wci && s_wcs_arena && s_wc_msg && s_wc_alerts && s_wcq;
}

// ---- output -------------------------------------------------------------------------
static void webCfgPump() {
  if (!s_wcq) return;
  while (s_wcq_rd < s_wcq_wr) {
    if (!g_web_mirror.pushTermData(s_wcq + s_wcq_rd)) return;   // ring full: next loop
    s_wcq_rd += (int)strlen(s_wcq + s_wcq_rd) + 1;
  }
  s_wcq_rd = s_wcq_wr = 0;
}
static void wcEnqueue(const char* m) {
  if (!s_wcq) return;
  const int len = (int)strlen(m) + 1;
  if (s_wcq_wr + len > s_wcq_cap && s_wcq_rd > 0) {
    memmove(s_wcq, s_wcq + s_wcq_rd, (size_t)(s_wcq_wr - s_wcq_rd));
    s_wcq_wr -= s_wcq_rd; s_wcq_rd = 0;
  }
  if (s_wcq_wr + len > s_wcq_cap) return;
  memcpy(s_wcq + s_wcq_wr, m, (size_t)len);
  s_wcq_wr += len;
}

// A message under construction in s_wc_msg.
struct WcOut {
  char* p; char* e;
  void begin(const char* kind) {
    p = s_wc_msg; e = s_wc_msg + WC_MSG - 16;
    p += snprintf(p, (size_t)(e - p), "{\"t\":\"cfg\",\"q\":%u,\"k\":\"%s\"", s_wc_q, kind);
  }
  void raw(const char* s) { while (*s && p < e) *p++ = *s++; }
  void num(const char* key, long v) { if (p < e - 32) p += snprintf(p, (size_t)(e - p), ",\"%s\":%ld", key, v); }
  void str(const char* key, const char* v) {
    if (p >= e - 32) return;
    p += snprintf(p, (size_t)(e - p), ",\"%s\":\"", key);
    jsonEsc(p, e, v ? v : "");
    if (p < e) *p++ = '"';
  }
  void end() { if (p < e + 14) { *p++ = '}'; *p = 0; } wcEnqueue(s_wc_msg); }
};
static void wcReplyErr(const char* m) { WcOut o; o.begin("err"); o.str("m", m); o.end(); }
static void wcReplyOk()               { WcOut o; o.begin("ok"); o.end(); }

// What the device said (showAlert) while a web change was being made, for the browser.
static void webCfgNoteAlert(const char* text) {
  if (!s_wc_alert_on || !s_wc_alerts || !text || !text[0]) return;
  const int len = (int)strlen(text);
  if (s_wc_al_n + len + 2 > WC_ALERTS) return;
  if (s_wc_al_n) s_wc_alerts[s_wc_al_n++] = '\n';
  memcpy(s_wc_alerts + s_wc_al_n, text, (size_t)len);
  s_wc_al_n += len;
  s_wc_alerts[s_wc_al_n] = 0;
}

// ---- reading widgets ----------------------------------------------------------------
static inline bool wcIs(lv_obj_t* o, const lv_obj_class_t* c) { return lv_obj_check_type(o, c); }
static inline const char* wcRowStr(uint16_t at) { return at == 0xFFFF ? nullptr : s_wc.arena + at; }
static inline bool wcHidden(lv_obj_t* o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }
static inline const char* wcLabel(lv_obj_t* l) {
  return l && wcIs(l, &lv_label_class) ? lv_label_get_text(l) : nullptr;
}
static inline uint32_t wcHex(lv_color_t c) { return (uint32_t)lv_color_to32(c) & 0xFFFFFF; }

static int wcRowOf(lv_obj_t* o) {
  for (int i = 0; i < s_wc.n; ++i) {
    const WcRow& r = s_wc.rows[i];
    if (!r.obj) continue;
    if (r.kind == WCK_STEP ? lv_obj_get_parent(r.obj) == o : r.obj == o) return i;
  }
  return -1;
}
static bool wcIsInput(lv_obj_t* o) {
  return wcIs(o, &lv_switch_class) || wcIs(o, &lv_checkbox_class) || wcIs(o, &lv_slider_class) ||
         wcIs(o, &lv_dropdown_class) || wcIs(o, &lv_textarea_class);
}
// Whether a clickable thing holds controls of its own (a switch row: descend into it,
// do not take the whole row for one tile).
static bool wcHasInner(lv_obj_t* o) {
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    if (wcIsInput(c) || wcRowOf(c) >= 0) return true;
    if (!wcIs(c, &lv_label_class) && lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) return true;
    if (wcHasInner(c)) return true;
  }
  return false;
}
// The visible text inside something, its lines joined with \n, private-use icon glyphs
// (U+E000..U+F8FF, LV_SYMBOL_* and the Lucide set) left out.
static void wcTextOf(lv_obj_t* o, char*& p, char* e, const char* b) {
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n && p < e - 4; ++i) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    if (wcHidden(c)) continue;
    if (const char* t = wcLabel(c)) {
      char* start = p;
      if (p > b && p[-1] != '\n') *p++ = '\n';
      char* body = p;
      for (const unsigned char* s = (const unsigned char*)t; *s && p < e - 4; ) {
        if ((s[0] == 0xEE || (s[0] == 0xEF && s[1] <= 0xA3)) && s[1] && s[2]) { s += 3; continue; }
        *p++ = (char)*s++;
      }
      while (p > body && p[-1] == ' ') --p;
      char* q = body;
      while (q < p && *q == ' ') ++q;
      if (q > body) { memmove(body, q, (size_t)(p - q)); p -= (q - body); }
      if (p == body) p = start;
      *p = 0;
    } else {
      wcTextOf(c, p, e, b);
    }
  }
}
// The name (and help) beside an unrecorded control: the labels around it in its row,
// or the label just before its row.
static void wcGuessTitle(lv_obj_t* o, const char** t, const char** h) {
  *t = *h = nullptr;
  lv_obj_t* par = lv_obj_get_parent(o);
  if (!par) return;
  const uint32_t n = lv_obj_get_child_cnt(par);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(par, i);
    if (c == o || wcHidden(c)) continue;
    const char* s = wcLabel(c);
    if (!s || !s[0]) continue;
    if (!*t) *t = s; else if (!*h) { *h = s; break; }
  }
  if (*t) return;
  lv_obj_t* gp = lv_obj_get_parent(par);
  if (!gp) return;
  const int32_t at = lv_obj_get_index(par);
  for (int32_t i = at - 1; i >= 0; --i) {
    lv_obj_t* c = lv_obj_get_child(gp, i);
    if (wcHidden(c)) continue;
    const char* s = wcLabel(c);
    if (s && s[0]) *t = s;
    break;
  }
}

static void wcItemGoneCb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  for (int i = 0; i < s_wci_n; ++i) if (s_wci[i].obj == o) s_wci[i].obj = nullptr;
}
static void wcAddItem(lv_obj_t* o, uint8_t kind, int row, const char* t = nullptr, const char* h = nullptr) {
  if (s_wci_n >= WC_MAX_ITEMS) { s_wci_more = true; return; }
  uint8_t g = 0;
  if (kind == WCI_TILE || kind == WCI_DOT) {
    lv_obj_t* par = lv_obj_get_parent(o);
    int k = 0;
    while (k < s_wc_grp_n && s_wc_grp[k] != par) ++k;
    if (k == s_wc_grp_n && s_wc_grp_n < WC_MAX_GRP) s_wc_grp[s_wc_grp_n++] = par;
    g = (uint8_t)k;
  }
  s_wci[s_wci_n++] = { o, t, h, (int16_t)row, kind, g };
  lv_obj_add_event_cb(o, wcItemGoneCb, LV_EVENT_DELETE, nullptr);
}

// Depth first, in the order the page shows things; hidden parts are left out.
static void wcWalk(lv_obj_t* o, bool is_root) {
  if (!is_root && wcHidden(o)) return;
  if (!is_root) {
    const int r = wcRowOf(o);
    if (r >= 0) {
      const uint8_t k = s_wc.rows[r].kind;
      wcAddItem(o, k, r);
      if (k != WCK_CARD) return;
    } else if (wcIs(o, &lv_switch_class) || wcIs(o, &lv_checkbox_class)) {
      const char* t; const char* h; wcGuessTitle(o, &t, &h);
      if (wcIs(o, &lv_checkbox_class)) { t = lv_checkbox_get_text(o); h = nullptr; }
      wcAddItem(o, WCK_SWITCH, -1, t, h); return;
    } else if (wcIs(o, &lv_slider_class)) {
      const char* t; const char* h; wcGuessTitle(o, &t, &h); wcAddItem(o, WCK_SLIDER, -1, t, h); return;
    } else if (wcIs(o, &lv_dropdown_class)) {
      const char* t; const char* h; wcGuessTitle(o, &t, &h); wcAddItem(o, WCK_DROP, -1, t, h); return;
    } else if (wcIs(o, &lv_textarea_class)) {
      const char* t; const char* h; wcGuessTitle(o, &t, &h); wcAddItem(o, WCK_FIELD, -1, t, h); return;
    } else if (wcIs(o, &lv_label_class)) {
      lv_obj_t* par = lv_obj_get_parent(o);
      const int pr = par ? wcRowOf(par) : -1;
      const char* s = lv_label_get_text(o);
      if (pr >= 0 && s_wc.rows[pr].kind == WCK_CARD && s && s[0]) wcAddItem(o, WCK_NOTE, -1);
      return;
    } else if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) &&
               (wcIs(o, &lv_btn_class) || lv_obj_get_class(o) == &lv_obj_class) && !wcHasInner(o)) {
      char buf[160]; char* p = buf; buf[0] = 0;
      wcTextOf(o, p, buf + sizeof buf, buf);
      const char* txt = buf[0] == '\n' ? buf + 1 : buf;
      if (txt[0]) {
        if (strcmp(txt, TR("Close")) != 0) wcAddItem(o, WCI_TILE, -1);
      } else if (!wcIs(o, &lv_btn_class)) {
        wcAddItem(o, WCI_DOT, -1);
      }
      return;
    }
  }
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i) wcWalk(lv_obj_get_child(o, i), false);
}
static void wcWalkView(lv_obj_t* root) {
  s_wci_n = 0; s_wci_more = false; s_wc_grp_n = 0;
  if (root) wcWalk(root, true);
}

static bool wcIsSave(const WcItem& it) {
  if (it.row >= 0) return (s_wc.rows[it.row].sub & WCF_SAVE) != 0;
  if (it.kind != WCI_TILE || !it.obj) return false;
  char buf[160]; char* p = buf; buf[0] = 0;
  wcTextOf(it.obj, p, buf + sizeof buf, buf);
  const char* t = buf[0] == '\n' ? buf + 1 : buf;
  return strcmp(t, TR("Save")) == 0 || strcmp(t, TR("Apply")) == 0 || strcmp(t, TR("Apply and restart")) == 0;
}

// ---- describing a view --------------------------------------------------------------
static void wcItemJson(WcOut& o, int i) {
  const WcItem& it = s_wci[i];
  lv_obj_t* ob = it.obj;
  if (!ob) return;
  const WcRow* r = it.row >= 0 ? &s_wc.rows[it.row] : nullptr;
  const char* t = r ? wcRowStr(r->title) : it.t;
  const char* h = r ? wcRowStr(r->help) : it.h;
  static const char* const kKind[] = { "", "card", "switch", "choice", "drop", "slider", "field", "step",
                                       "swatch", "action", "button", "info", "note", "tile", "dot" };
  if (o.p > s_wc_msg && o.p[-1] != '[') o.raw(",");
  o.p += snprintf(o.p, (size_t)(o.e - o.p), "{\"i\":%d,\"k\":\"%s\"", i, kKind[it.kind]);
  if (t && it.kind != WCK_SLIDER) o.str("t", t);
  if (h && it.kind != WCK_SLIDER) o.str("h", h);
  const bool dis = lv_obj_has_state(ob, LV_STATE_DISABLED);
  switch (it.kind) {
    case WCK_CARD: break;
    case WCK_SWITCH:
      o.num("v", lv_obj_has_state(ob, LV_STATE_CHECKED) ? 1 : 0);
      if (dis) o.num("d", 1);
      break;
    case WCK_CHOICE: {
      const uint32_t n = lv_obj_get_child_cnt(ob);
      o.raw(",\"o\":[");
      for (uint32_t k = 0; k < n; ++k) {
        lv_obj_t* tile = lv_obj_get_child(ob, k);
        if (k) o.raw(",");
        o.raw("\""); jsonEsc(o.p, o.e, wcLabel(lv_obj_get_child(tile, 0))); o.raw("\"");
      }
      o.raw("],\"bw\":[");
      for (uint32_t k = 0; k < n; ++k)
        o.p += snprintf(o.p, (size_t)(o.e - o.p), "%s%d", k ? "," : "",
                        (int)lv_obj_get_style_border_width(lv_obj_get_child(ob, k), LV_PART_MAIN));
      o.raw("],\"en\":[");
      for (uint32_t k = 0; k < n; ++k) {
        lv_obj_t* tile = lv_obj_get_child(ob, k);
        const bool en = !lv_obj_has_state(tile, LV_STATE_DISABLED) && lv_obj_get_style_opa(tile, LV_PART_MAIN) >= 128;
        o.p += snprintf(o.p, (size_t)(o.e - o.p), "%s%d", k ? "," : "", en ? 1 : 0);
      }
      o.raw("]");
      break;
    }
    case WCK_DROP:
      o.str("o", lv_dropdown_get_options(ob));
      o.num("v", lv_dropdown_get_selected(ob));
      if (dis) o.num("d", 1);
      break;
    case WCK_SLIDER: {
      if (t) o.str("t", t);
      o.num("mn", lv_slider_get_min_value(ob));
      o.num("mx", lv_slider_get_max_value(ob));
      o.num("v", lv_slider_get_value(ob));
      if (r && r->aux) o.str("s", wcLabel(r->aux));
      if (const char* ends = h) {
        char lo[48] = {0}; const char* tab = strchr(ends, '\t');
        if (tab) {
          snprintf(lo, sizeof lo, "%.*s", (int)LV_MIN((long)(tab - ends), 47L), ends);
          o.str("lo", lo); o.str("hi", tab + 1);
        }
      }
      if (dis) o.num("d", 1);
      break;
    }
    case WCK_FIELD: {
      const bool pw = lv_textarea_get_password_mode(ob);
      const char* v = lv_textarea_get_text(ob);
      if (pw) { o.num("pw", 1); if (v && v[0]) o.num("set", 1); }
      else    o.str("v", v);
      o.num("ml", (long)lv_textarea_get_max_length(ob));
      const char* ph = lv_textarea_get_placeholder_text(ob);
      if (ph && ph[0]) o.str("ph", ph);
      if (r && (r->sub & WCF_NUM)) o.num("num", 1);
      if (dis) o.num("d", 1);
      break;
    }
    case WCK_STEP: {
      o.str("s", wcLabel(r ? r->obj : nullptr));
      char buf[48]; char* p;
      lv_obj_t* minus = lv_obj_get_child(ob, 0);
      lv_obj_t* plus  = lv_obj_get_child(ob, 2);
      p = buf; buf[0] = 0; if (minus) wcTextOf(minus, p, buf + sizeof buf, buf); o.str("m", buf[0] == '\n' ? buf + 1 : buf);
      p = buf; buf[0] = 0; if (plus)  wcTextOf(plus,  p, buf + sizeof buf, buf); o.str("p", buf[0] == '\n' ? buf + 1 : buf);
      break;
    }
    case WCK_SWATCH: {
      const uint32_t n = lv_obj_get_child_cnt(ob);
      o.raw(",\"c\":[");
      for (uint32_t k = 0; k < n; ++k)
        o.p += snprintf(o.p, (size_t)(o.e - o.p), "%s\"%06lx\"", k ? "," : "",
                        (unsigned long)wcHex(lv_obj_get_style_bg_color(lv_obj_get_child(ob, k), LV_PART_MAIN)));
      o.raw("],\"bw\":[");
      for (uint32_t k = 0; k < n; ++k)
        o.p += snprintf(o.p, (size_t)(o.e - o.p), "%s%d", k ? "," : "",
                        (int)lv_obj_get_style_outline_width(lv_obj_get_child(ob, k), LV_PART_MAIN));
      o.raw("]");
      break;
    }
    case WCK_ACTION:
      if (r && r->aux && !wcHidden(r->aux)) o.str("s", wcLabel(r->aux));
      if (r) { o.num("kd", r->sub & 0x0F); if (r->sub & WCF_CHEVRON) o.num("ch", 1); }
      if (dis) o.num("d", 1);
      break;
    case WCK_BUTTON:
      if (r) { o.num("kd", r->sub & 0x0F); if (r->sub & WCF_SAVE) o.num("sv", 1); }
      if (dis) o.num("d", 1);
      break;
    case WCK_INFO:
      o.str("s", wcLabel(ob));
      break;
    case WCK_NOTE:
      o.str("s", wcLabel(ob));
      if (r) o.num("tn", r->sub);
      break;
    case WCI_TILE: {
      char buf[160]; char* p = buf; buf[0] = 0;
      wcTextOf(ob, p, buf + sizeof buf, buf);
      o.str("s", buf[0] == '\n' ? buf + 1 : buf);
      o.num("g", it.grp);
      o.num("bw", lv_obj_get_style_border_width(ob, LV_PART_MAIN) + lv_obj_get_style_outline_width(ob, LV_PART_MAIN));
      if (dis || lv_obj_get_style_opa(ob, LV_PART_MAIN) < 128) o.num("d", 1);
      if (lv_obj_has_state(ob, LV_STATE_CHECKED)) o.num("on", 1);
      if (wcIsSave(it)) o.num("sv", 1);
      break;
    }
    case WCI_DOT: {
      char hex[8]; snprintf(hex, sizeof hex, "%06lx", (unsigned long)wcHex(lv_obj_get_style_bg_color(ob, LV_PART_MAIN)));
      o.str("c", hex);
      o.num("g", it.grp);
      o.num("bw", lv_obj_get_style_outline_width(ob, LV_PART_MAIN));
      break;
    }
  }
  o.raw("}");
}

// The first visible text in a view, for its title when it set none on the bar.
static const char* wcFirstText(lv_obj_t* o) {
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    if (wcHidden(c)) continue;
    const char* s = wcLabel(c);
    if (s && s[0] && !((unsigned char)s[0] == 0xEE || (unsigned char)s[0] == 0xEF)) return s;
    if (!s) if (const char* d = wcFirstText(c)) return d;
  }
  return nullptr;
}

static void wcSendView(const char* title, const char* path) {
  WcOut o;
  o.begin("view");
  o.str("ti", title ? title : "");
  o.str("p", path);
  o.num("n", s_wci_n);
  if (s_wci_more) o.num("more", 1);
  o.end();
  o.begin("items");
  o.raw(",\"a\":[");
  for (int i = 0; i < s_wci_n; ++i) {
    wcItemJson(o, i);
    if (o.p - s_wc_msg > WC_CHUNK && i + 1 < s_wci_n) {
      o.raw("]"); o.end();
      o.begin("items"); o.raw(",\"a\":[");
    }
  }
  o.raw("]");
  o.end();
  o.begin("end");
  o.end();
}

// ---- working widgets ----------------------------------------------------------------
enum { WCR_NONE = 0, WCR_VIEW, WCR_CONFIRM, WCR_OPENED };
static const size_t WC_NREG = sizeof(k_popup_registry) / sizeof(k_popup_registry[0]);
static const int WC_SNAP = 96;
struct WcSnap {
  bool reg[WC_NREG];
  lv_obj_t* top[WC_SNAP]; int ntop;
  lv_obj_t* scr[WC_SNAP]; int nscr;
  uint8_t subs;
  void take() {
    for (size_t i = 0; i < WC_NREG; ++i) reg[i] = k_popup_registry[i].is_open();
    ntop = nscr = 0;
    lv_obj_t* lt = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(lt) && ntop < WC_SNAP; ++i) top[ntop++] = lv_obj_get_child(lt, i);
    lv_obj_t* sa = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(sa) && nscr < WC_SNAP; ++i) scr[nscr++] = lv_obj_get_child(sa, i);
    subs = s_subpage_n;
  }
  static lv_obj_t* fresh(lv_obj_t* layer, lv_obj_t* const* had, int nhad, lv_obj_t* skip = nullptr) {
    for (int32_t i = (int32_t)lv_obj_get_child_cnt(layer) - 1; i >= 0; --i) {
      lv_obj_t* c = lv_obj_get_child(layer, i);
      if (c == g_statusbar.root || c == skip) continue;
      bool old = false;
      for (int k = 0; k < nhad && !old; ++k) old = had[k] == c;
      if (!old) return c;
    }
    return nullptr;
  }
};

static bool s_wc_confirm_ok = false;   // this request may answer the device's confirms "yes"

// Tap one thing, then see what it did: a confirm (answered when allowed, else reported),
// a new page or sheet (a view the browser can go into), or something the device opened
// that cannot be described here (left open, reported).
static int wcClick(lv_obj_t* ob, WcView* out) {
  WcSnap s; s.take();
  lv_obj_t* conf0 = s_confirm_modal;
  lv_obj_t* gone = nullptr;   // the confirm we answered: deleted async, still on layer_top
  lv_event_send(ob, LV_EVENT_CLICKED, nullptr);
  if (s_confirm_modal && s_confirm_modal != conf0) {
    gone = s_confirm_modal;
    if (!s_wc_confirm_ok) {
      const char* m = s_confirm_msg_lbl ? wcLabel(s_confirm_msg_lbl) : nullptr;
      strlcpy(s_wc_confirm_msg, m ? m : TR("Are you sure?"), sizeof s_wc_confirm_msg);
      confirmDismiss();
      return WCR_CONFIRM;
    }
    SimpleCb cb = s_confirm_cb;
    confirmDismiss();
    if (cb) cb();
  }
  for (size_t i = 0; i < WC_NREG; ++i) {
    if (s.reg[i] || !k_popup_registry[i].is_open()) continue;
    lv_obj_t* root = WcSnap::fresh(lv_layer_top(), s.top, s.ntop, gone);
    if (!root) root = WcSnap::fresh(lv_scr_act(), s.scr, s.nscr);
    if (!root || !k_popup_registry[i].close) return WCR_OPENED;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    *out = { root, (int)i, nullptr };
    return WCR_VIEW;
  }
  if (s_subpage_n > s.subs) {
    lv_obj_t* root = WcSnap::fresh(lv_layer_top(), s.top, s.ntop, gone);
    if (root) {
      lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
      *out = { root, -1, s_subpages[s_subpage_n - 1].close };
      return WCR_VIEW;
    }
    return WCR_OPENED;
  }
  if (WcSnap::fresh(lv_layer_top(), s.top, s.ntop, gone)) return WCR_OPENED;
  return WCR_NONE;
}

static bool wcViewOpen(const WcView& v) {
  if (v.reg >= 0) return k_popup_registry[v.reg].is_open();
  if (v.sub_close) {
    for (int k = 0; k < (int)s_subpage_n; ++k) if (s_subpages[k].close == v.sub_close) return true;
    return false;
  }
  return s_settings_sheet != nullptr && s_settings_page != nullptr;
}

// ---- one request --------------------------------------------------------------------
struct WcSession {
  bool open = false, opened_other = false;
  bool tall; const char* ap_title; void (*ap_close)(); bool ap_slim;
  lv_obj_t* root = nullptr;
};

static const char* wcBusy() {
  if (s_settings_sheet || s_settings_open_cat >= 0 || settingsModalIsOpen() || s_confirm_modal || s_subpage_n)
    return "Settings are open on the device. Close them there and try again.";
  if (s_kb_bind_ta) return "Someone is typing on the device. Try again in a moment.";
  return nullptr;
}

static lv_obj_t* wcBuild(int cat, WcSession& ss) {
  ss.tall = s_statusbar_tall; ss.ap_title = s_apppage_title; ss.ap_close = s_apppage_close; ss.ap_slim = s_apppage_slim;
  s_wc.n = 0; s_wc.an = 0; s_wc.on = true;
  s_wc_al_n = 0; s_wc_alerts[0] = 0; s_wc_alert_on = true;
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr) - STATUSBAR_H;
  lv_obj_t* root = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(root);
  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(root, sw, sh);
  lv_obj_set_pos(root, 0, STATUSBAR_H);
  lv_obj_t* page = lv_obj_create(root);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_size(page, sw, sh);
  prepSettingsPage(page);
  s_settings_open_cat = cat;
  s_settings_sheet = root;
  resetSettingsModalState();
  s_settings_page = page;
  s_settings_inline_parent = page;
  settingsCatBuild(cat);
  s_settings_inline_parent = nullptr;
  ss.open = true; ss.root = root;
  s_wcv_n = 0;
  s_wcv[s_wcv_n++] = { page, -1, nullptr };
  return page;
}

static void wcTeardown(WcSession& ss) {
  if (!ss.open) return;
  // Our views are hidden: never leave one behind, even under something the click opened.
  for (int v = s_wcv_n - 1; v >= 1; --v) {
    const WcView& w = s_wcv[v];
    if (!wcViewOpen(w)) continue;
    if (w.reg >= 0 && k_popup_registry[w.reg].close) k_popup_registry[w.reg].close();
    else if (w.sub_close) w.sub_close();
  }
  if (!ss.opened_other && s_confirm_modal) confirmDismiss();
  s_wcv_n = 0;
  if (s_settings_sheet == ss.root) settingsCategoryDrop();
  s_wc.on = false; s_wc.n = 0;
  s_wc_alert_on = false;
  if (!ss.opened_other) {
    s_apppage_title = ss.ap_title; s_apppage_close = ss.ap_close; s_apppage_slim = ss.ap_slim;
    statusBarSetTall(ss.tall);
  }
  updateGlobalStatusBar();
  settingsLandingRefresh();
  ss.open = false;
}

// Go from the page down the clicked path; true with s_wcv holding every view on the way.
static bool wcReplay(const char* path) {
  if (!path || !path[0] || path[0] == '-') return true;
  const char* s = path;
  while (*s) {
    const int idx = atoi(s);
    wcWalkView(s_wcv[s_wcv_n - 1].root);
    if (idx < 0 || idx >= s_wci_n || !s_wci[idx].obj || s_wcv_n >= WC_MAX_VIEWS) return false;
    WcView v;
    const bool was = s_wc_confirm_ok;
    s_wc_confirm_ok = false;
    const int r = wcClick(s_wci[idx].obj, &v);
    s_wc_confirm_ok = was;
    if (r != WCR_VIEW) return false;
    s_wcv[s_wcv_n++] = v;
    while (*s && *s != '.') ++s;
    if (*s == '.') ++s;
  }
  return true;
}

static const char* wcViewTitle(const WcSession& ss) {
  const WcView& top = s_wcv[s_wcv_n - 1];
  if (s_wcv_n == 1) return TR(kSettingsCats[s_settings_open_cat].label);
  if (s_apppage_title && s_apppage_title != ss.ap_title) return s_apppage_title;
  const char* t = wcFirstText(top.root);
  return t ? t : "";
}

// Staged values onto the current view's widgets. Fields first (all their texts, then the
// blur each saves on), then the rest in page order. Returns false when something it was
// to set went away (the page rebuilt) or a confirm stopped it.
static int wcApply(bool* partial, WcSession& ss) {
  *partial = false;
  for (int pass = 0; pass < 3; ++pass) {
    for (int s = 0; s < s_wcs_n; ++s) {
      const int i = s_wcs[s].idx;
      const char* val = s_wcs_arena + s_wcs[s].at;
      if (i < 0 || i >= s_wci_n) continue;
      WcItem& it = s_wci[i];
      const bool field = it.kind == WCK_FIELD;
      if ((pass < 2) != field) continue;
      if (!it.obj) { *partial = true; continue; }
      lv_obj_t* o = it.obj;
      if (pass == 0) { lv_textarea_set_text(o, val); continue; }
      if (pass == 1) { lv_event_send(o, LV_EVENT_DEFOCUSED, nullptr); continue; }
      if (lv_obj_has_state(o, LV_STATE_DISABLED)) continue;
      WcView v; int r = WCR_NONE;
      switch (it.kind) {
        case WCK_SWITCH: {
          const bool want = atoi(val) != 0;
          if (want == lv_obj_has_state(o, LV_STATE_CHECKED)) break;
          if (want) lv_obj_add_state(o, LV_STATE_CHECKED); else lv_obj_clear_state(o, LV_STATE_CHECKED);
          lv_event_send(o, LV_EVENT_VALUE_CHANGED, nullptr);
          break;
        }
        case WCK_DROP:
          lv_dropdown_set_selected(o, (uint16_t)atoi(val));
          lv_event_send(o, LV_EVENT_VALUE_CHANGED, nullptr);
          break;
        case WCK_SLIDER:
          lv_slider_set_value(o, atoi(val), LV_ANIM_OFF);
          lv_event_send(o, LV_EVENT_VALUE_CHANGED, nullptr);
          if (it.obj) lv_event_send(o, LV_EVENT_RELEASED, nullptr);
          break;
        case WCK_CHOICE: case WCK_SWATCH: {
          lv_obj_t* c = lv_obj_get_child(o, atoi(val));
          if (c) r = wcClick(c, &v);
          break;
        }
        case WCK_STEP: {
          const int d = atoi(val);
          lv_obj_t* b = lv_obj_get_child(o, d < 0 ? 0 : 2);
          for (int k = 0; b && k < (d < 0 ? -d : d) && k < 200 && it.obj; ++k) r = wcClick(b, &v);
          break;
        }
        case WCI_TILE: case WCI_DOT:
          r = wcClick(o, &v);
          break;
        default: break;
      }
      if (r == WCR_CONFIRM) return WCR_CONFIRM;
      if (r == WCR_OPENED) { ss.opened_other = true; return WCR_OPENED; }
      if (r == WCR_VIEW) {   // a pick that opened a sheet: not what a value means, close it
        if (v.reg >= 0 && k_popup_registry[v.reg].close) k_popup_registry[v.reg].close();
        else if (v.sub_close) v.sub_close();
      }
    }
  }
  return WCR_NONE;
}

static void wcStageClear() { s_wcs_n = 0; s_wcs_an = 0; }

static void wcCats() {
  WcOut o;
  o.begin("cats");
  char name[64] = {0};
  if (NodePrefs* np = the_mesh.getNodePrefs()) strlcpy(name, np->node_name, sizeof name);
  o.str("dev", name);
  o.raw(",\"g\":[");
  bool first_g = true;
  for (const SettingsGroupDef& g : kSettingsGroups) {
    if (!first_g) o.raw(",");
    first_g = false;
    o.raw("{\"t\":\""); jsonEsc(o.p, o.e, g.title[0] ? TR(g.title) : ""); o.raw("\",\"c\":[");
    bool first = true;
    for (int k = 0; k < g.n; ++k) {
      const int c = g.cats[k];
      if (!settingsCatOnBoard(c)) continue;
#if CAP_LUA_APPS
      if (c == CAT_MQTT && (touchPrefsGetAppHide() & APPHIDE_MQTT)) continue;
#endif
      char val[96];
      settingsCatValue(c, val, sizeof val);
      if (!first) o.raw(",");
      first = false;
      o.p += snprintf(o.p, (size_t)(o.e - o.p), "{\"id\":%d", c);
      o.str("l", TR(kSettingsCats[c].label));
      o.str("e", kSettingsCats[c].label);
      if (val[0]) o.str("v", val);
      o.raw("}");
    }
    o.raw("]}");
  }
  o.raw("]");
  o.end();
}

static void webCfgCmd(const char* a) {
  while (*a == ' ') ++a;
  char verb[8] = {0};
  int k = 0;
  while (*a && *a != ' ' && k < 7) verb[k++] = *a++;
  while (*a == ' ') ++a;
  s_wc_q = (unsigned)strtoul(a, (char**)&a, 10);
  while (*a == ' ') ++a;
  if (!wcEnsure()) { wcReplyErr("Out of memory"); webCfgPump(); return; }

  if (!strcmp(verb, "cats")) { wcCats(); webCfgPump(); return; }
  if (!strcmp(verb, "clr"))  { wcStageClear(); wcReplyOk(); webCfgPump(); return; }
  if (!strcmp(verb, "set")) {
    char* e = nullptr;
    const long idx = strtol(a, &e, 10);
    const char* v = e && *e == ' ' ? e + 1 : "";
    const int len = (int)strlen(v);
    int s = 0;
    while (s < s_wcs_n && s_wcs[s].idx != idx) ++s;
    if (s == s_wcs_n && s_wcs_n >= WC_STAGE_N) { wcReplyErr("Too many changes at once"); webCfgPump(); return; }
    if (s_wcs_an + len + 1 > WC_STAGE_ARENA)   { wcReplyErr("Too many changes at once"); webCfgPump(); return; }
    memcpy(s_wcs_arena + s_wcs_an, v, (size_t)len + 1);
    s_wcs[s] = { (int16_t)idx, (uint16_t)s_wcs_an };
    s_wcs_an += len + 1;
    if (s == s_wcs_n) ++s_wcs_n;
    wcReplyOk(); webCfgPump();
    return;
  }
  const bool is_view = !strcmp(verb, "view"), is_run = !strcmp(verb, "run");
  if (!is_view && !is_run) { wcReplyErr("Unknown command"); webCfgPump(); return; }

  char* e = nullptr;
  const int cat = (int)strtol(a, &e, 10);
  a = e; while (*a == ' ') ++a;
  char path[48] = {0};
  k = 0;
  while (*a && *a != ' ' && k < (int)sizeof path - 1) path[k++] = *a++;
  int idx = -1, want_n = -1, confirm = 0;
  if (is_run) sscanf(a, "%d %d %d", &idx, &want_n, &confirm);

  if (cat < 0 || cat >= CAT_COUNT || !settingsCatOnBoard(cat)) { wcReplyErr("No such settings page"); webCfgPump(); return; }
  if (const char* why = wcBusy()) { wcReplyErr(TR(why)); webCfgPump(); return; }

  WcSession ss;
  wcBuild(cat, ss);
  s_wc_confirm_ok = false;
  if (!wcReplay(path)) {
    wcTeardown(ss);
    wcReplyErr(TR("This page changed on the device. Reloading."));
    webCfgPump();
    return;
  }
  wcWalkView(s_wcv[s_wcv_n - 1].root);

  if (is_view) {
    wcSendView(wcViewTitle(ss), path);
    wcTeardown(ss);
    webCfgPump();
    return;
  }

  if (want_n != s_wci_n) {
    wcTeardown(ss);
    wcReplyErr(TR("This page changed on the device. Reloading."));
    webCfgPump();
    return;
  }
  s_wc_confirm_ok = confirm != 0;
  const int staged = s_wcs_n;
  bool partial = false;
  int r = wcApply(&partial, ss);
  WcView nv = {};
  if (r == WCR_NONE) {
    if (idx >= 0 && idx < s_wci_n) {
      if (!s_wci[idx].obj) partial = true;
      else r = wcClick(s_wci[idx].obj, &nv);
    } else if (idx < 0 && staged) {
      // The page's own Save / Apply, as it stands now: some only show once something
      // is picked. One press: a page may carry the same Apply twice (the Theme bar).
      wcWalkView(s_wcv[s_wcv_n - 1].root);
      for (int i = 0; i < s_wci_n; ++i)
        if (s_wci[i].obj && wcIsSave(s_wci[i]) && !lv_obj_has_state(s_wci[i].obj, LV_STATE_DISABLED)) {
          idx = i;
          r = wcClick(s_wci[i].obj, &nv);
          break;
        }
    }
  }
  if (r == WCR_CONFIRM) {
    wcTeardown(ss);
    // ap: the staged values went in before the click asked; some (steppers) save as
    // they are pressed, so the browser must not send those again with its "yes".
    WcOut o; o.begin("confirm"); o.str("m", s_wc_confirm_msg); if (staged) o.num("ap", 1); o.end();
    webCfgPump();
    return;
  }
  wcStageClear();
  if (r == WCR_VIEW && s_wcv_n < WC_MAX_VIEWS) {
    s_wcv[s_wcv_n++] = nv;
    wcWalkView(nv.root);
    char np[56];
    if (path[0] == '-' || !path[0]) snprintf(np, sizeof np, "%d", idx);
    else                            snprintf(np, sizeof np, "%s.%d", path, idx);
    wcSendView(wcViewTitle(ss), np);
    wcTeardown(ss);
    webCfgPump();
    return;
  }
  if (r == WCR_VIEW) {   // one level too deep to describe: close it again
    if (nv.reg >= 0 && k_popup_registry[nv.reg].close) k_popup_registry[nv.reg].close();
    else if (nv.sub_close) nv.sub_close();
    r = WCR_NONE; partial = true;
  }
  if (r == WCR_OPENED) ss.opened_other = true;
  const bool closed = !wcViewOpen(s_wcv[s_wcv_n - 1]) || s_settings_sheet != ss.root;
  if (s_settings_sheet != ss.root && s_settings_sheet) ss.opened_other = true;
  wcTeardown(ss);
  WcOut o;
  o.begin("done");
  if (s_wc_al_n) o.str("al", s_wc_alerts);
  if (partial) o.num("pt", 1);
  if (closed)  o.num("cl", 1);
  if (ss.opened_other) o.num("op", 1);
  o.end();
  webCfgPump();
}

#endif  // !HAS_TANMATSU
