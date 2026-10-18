// SPDX-License-Identifier: GPL-3.0-or-later
// Repeater / room server admin app ("Manage" on a repeater or room contact).
//
// NOT a normal header: UITask.cpp includes it once, in the middle of the file,
// after the settings kit, the confirm popup and the old admin console it reuses.
// It lives in its own file to keep the app in one readable place; it shares the
// translation unit (and so every static helper) with UITask.cpp, which is also
// why it needs no entry in any build_src_filter or IDF CMakeLists.
//
// Protocol (see dev-docs/REPEATER_ADMIN.md, RepeaterAdminProto.h):
//   * login     uiSendAdminLogin (blank = guest), result in raOnLoginResult
//   * reads     CLI "XX|get key" (admin), binary GET_STATUS / NEIGHBOURS / OWNER /
//               ACCESS_LIST (guests may read status, neighbours, owner info)
//   * writes    CLI "XX|set key value", the reply shown on the row, then re-read
// One request at a time (a FIFO pumped by an lv_timer); the "XX|" correlation
// prefix ties a CLI reply to its field, so a late reply can never land on the
// wrong row. Every reply hook checks the app is open for THAT contact and every
// widget pointer is cleared before its page is deleted, so a reply that arrives
// after the app closed touches nothing.
#pragma once
#include "RepeaterAdminProto.h"

namespace {
namespace RAP = RepeaterAdminProto;

enum RaView : uint8_t {
  RA_V_LOGIN = 0, RA_V_OVERVIEW, RA_V_NEIGHBOURS, RA_V_CLIENTS, RA_V_IDENTITY, RA_V_RADIO,
  RA_V_FORWARD, RA_V_ADVERTS, RA_V_ACCESS, RA_V_REGIONS, RA_V_ACTIONS, RA_V_COUNT
};
enum RaKind : uint8_t { RA_K_TEXT, RA_K_INT, RA_K_FLOAT, RA_K_BOOL, RA_K_CHOICE, RA_K_INFO };
enum RaWho : uint8_t { RA_BOTH = 0, RA_RPT = 1, RA_ROOM = 2 };

enum RaField : uint8_t {
  RA_F_NAME = 0, RA_F_LAT, RA_F_LON, RA_F_OWNER,
  RA_F_RADIO, RA_F_TX,
  RA_F_REPEAT, RA_F_FLOODMAX, RA_F_FLOODMAXADV, RA_F_LOOP, RA_F_AF, RA_F_TXDELAY, RA_F_DTXDELAY, RA_F_RXDELAY,
  RA_F_ADV, RA_F_FADV,
  RA_F_GUESTPW, RA_F_ALLOWRO,
  RA_F_CLOCK, RA_F_REGIONS, RA_F_VER,
  RA_F_COUNT
};

struct RaFieldDef {
  const char* key;     // CLI key ("get <key>" / "set <key> <v>"); never translated
  uint8_t view, kind, who;
  float lo, hi;        // numeric range (inclusive)
  uint8_t maxlen;
  const char* choices; // RA_K_CHOICE: '\n' separated CLI values
};

const RaFieldDef k_ra_fields[RA_F_COUNT] = {
  { "name",                  RA_V_IDENTITY, RA_K_TEXT,   RA_BOTH, 0, 0, 31, nullptr },
  { "lat",                   RA_V_IDENTITY, RA_K_FLOAT,  RA_BOTH, -90, 90, 12, nullptr },
  { "lon",                   RA_V_IDENTITY, RA_K_FLOAT,  RA_BOTH, -180, 180, 12, nullptr },
  { "owner.info",            RA_V_IDENTITY, RA_K_TEXT,   RA_BOTH, 0, 0, 119, nullptr },
  { "radio",                 RA_V_RADIO,    RA_K_INFO,   RA_BOTH, 0, 0, 0, nullptr },
  { "tx",                    RA_V_RADIO,    RA_K_INT,    RA_BOTH, -9, 30, 4, nullptr },
  { "repeat",                RA_V_FORWARD,  RA_K_BOOL,   RA_RPT, 0, 0, 0, nullptr },
  { "flood.max",             RA_V_FORWARD,  RA_K_INT,    RA_RPT, 0, 64, 2, nullptr },
  { "flood.max.advert",      RA_V_FORWARD,  RA_K_INT,    RA_RPT, 0, 64, 2, nullptr },
  { "loop.detect",           RA_V_FORWARD,  RA_K_CHOICE, RA_RPT, 0, 0, 0, "off\nminimal\nmoderate\nstrict" },
  { "af",                    RA_V_FORWARD,  RA_K_FLOAT,  RA_RPT, 0, 9, 5, nullptr },
  { "txdelay",               RA_V_FORWARD,  RA_K_FLOAT,  RA_RPT, 0, 2, 5, nullptr },
  { "direct.txdelay",        RA_V_FORWARD,  RA_K_FLOAT,  RA_RPT, 0, 2, 5, nullptr },
  { "rxdelay",               RA_V_FORWARD,  RA_K_FLOAT,  RA_RPT, 0, 20, 5, nullptr },
  { "advert.interval",       RA_V_ADVERTS,  RA_K_INT,    RA_BOTH, 0, 240, 3, nullptr },
  { "flood.advert.interval", RA_V_ADVERTS,  RA_K_INT,    RA_BOTH, 0, 168, 3, nullptr },
  { "guest.password",        RA_V_ACCESS,   RA_K_TEXT,   RA_BOTH, 0, 0, 15, nullptr },
  { "allow.read.only",       RA_V_ACCESS,   RA_K_BOOL,   RA_ROOM, 0, 0, 0, nullptr },
  { "clock",                 RA_V_ACTIONS,  RA_K_INFO,   RA_BOTH, 0, 0, 0, nullptr },
  { "region",                RA_V_REGIONS,  RA_K_INFO,   RA_RPT, 0, 0, 0, nullptr },
  { "ver",                   RA_V_OVERVIEW, RA_K_INFO,   RA_BOTH, 0, 0, 0, nullptr },
};
// What the rows say, by field (English literals, TR() at use).
const char* const k_ra_titles[RA_F_COUNT] = {
  "Name", "Latitude", "Longitude", "Owner info", "Radio", "Transmit power",
  "Repeat packets", "Flood hop limit", "Advert hop limit", "Loop detection", "Airtime factor",
  "Flood send delay", "Direct send delay", "Receive delay",
  "Local advert", "Flood advert", "Guest password", "Read-only access",
  "Node clock", "Regions", "Firmware",
};
const char* const k_ra_helps[RA_F_COUNT] = {
  "Up to 31 characters, without [ ] \\ : , ? *",
  "Decimal degrees, for example 51.2194",
  "Decimal degrees, for example 4.4025",
  "Shown to anyone who asks this node. Up to 119 characters.",
  "",
  "dBm, -9 to 30. Applies at once.",
  "Off: this node stops forwarding anything.",
  "Floods with more hops are not repeated. 0 to 64.",
  "The same limit for adverts. 0 to 64.",
  "How hard to drop packets that came back around.",
  "0 to 9. Higher waits longer between sends.",
  "Random delay factor before repeating a flood. 0 to 2.",
  "The same for direct packets. 0 to 2.",
  "Prefers stronger copies of a flood. 0 to 20.",
  "Minutes between zero-hop adverts: 0 (off) or 60 to 240.",
  "Hours between flood adverts: 0 (off) or 3 to 168.",
  "", "Lets a wrong or blank password in to read, not to post.",
  "", "", "",
};
// The loop.detect choices as labels.
const char* raLoopLabel(int i) {
  switch (i) { case 0: return "Off"; case 1: return "Minimal"; case 2: return "Moderate"; default: return "Strict"; }
}
const char* raRoleName(int r) {
  switch (r) { case 3: return "Admin"; case 2: return "Read-write"; case 1: return "Read-only"; default: return "Guest"; }
}

// What a queued request is.
enum RaOpKind : uint8_t {
  RA_OP_GET = 0,    // CLI read of a field
  RA_OP_SET,        // CLI write of a field (then re-read)
  RA_OP_ACT,        // CLI action, its reply shown on the Actions page
  RA_OP_FIRE,       // CLI action that does not answer (reboot)
  RA_OP_STATUS,     // binary GET_STATUS
  RA_OP_OWNER,      // binary GET_OWNER_INFO
  RA_OP_NEIGH,      // binary GET_NEIGHBOURS (page at s->nb_n)
  RA_OP_ACL,        // binary GET_ACCESS_LIST
  RA_OP_PERM,       // CLI setperm (then re-read the access list)
};
enum RaFieldState : uint8_t { RA_FS_UNKNOWN = 0, RA_FS_LOADING, RA_FS_OK, RA_FS_TIMEOUT, RA_FS_SAVING, RA_FS_ERROR };

struct RaOp { uint8_t kind, field, tries; char cmd[150]; };
constexpr int RA_Q = 24;
constexpr int RA_NB_MAX = 64;
constexpr int RA_ACL_MAX = 40;
constexpr int RA_VAL = 168;

struct RaState {
  bool     open;
  uint8_t  pub[32];
  char     name[32];        // shown name (sanitised)
  bool     is_room;
  int8_t   role;            // -1 not logged in; RAP::ROLE_*
  uint32_t login_ms;        // when the login was accepted
  bool     logging_in;
  uint32_t login_deadline;
  char     login_pw[16];    // the attempt, until its result lands
  bool     login_remember;
  uint8_t  view;
  // values
  char     val[RA_F_COUNT][RA_VAL];
  uint8_t  fst[RA_F_COUNT];
  char     last_err[RA_F_COUNT][48];
  RAP::Status status;
  bool     has_status;
  uint32_t status_ms;
  char     fw[32];
  char     owner[128];
  bool     has_owner;
  RAP::Neighbour nb[RA_NB_MAX];
  int      nb_n;
  uint16_t nb_total;
  uint8_t  nb_order;
  bool     nb_loaded;
  RAP::AclEntry acl[RA_ACL_MAX];
  int      acl_n;
  bool     acl_loaded;
  char     act_result[200];
  // queue
  RaOp     q[RA_Q];
  int      q_head, q_len;
  bool     busy;
  RaOp     cur;
  char     cur_id[3];
  uint32_t cur_deadline;
  uint8_t  id_seq;
  // edit sheet target
  uint8_t  edit_field;
  // pending confirmed action
  char     pend_cmd[150];
  uint8_t  pend_kind;
  uint8_t  pend_field;
  // radio editor values (pending confirm)
  RAP::Radio radio_new;
  uint8_t  radio_try_mins;
  int      perm_idx;          // access-list row being edited
  uint8_t  perm_role;
  char     admin_pw_new[16];
};
RaState* s_ra = nullptr;

lv_obj_t* s_ra_root = nullptr;        // the app page (lv_layer_top, under the bar)
lv_obj_t* s_ra_page = nullptr;        // the scrolling content of the current view
lv_obj_t* s_ra_edit_root = nullptr;   // the edit sheet (popup)
lv_obj_t* s_ra_edit_ta = nullptr;
lv_obj_t* s_ra_edit_choice = nullptr;
int       s_ra_edit_pick = -1;
lv_obj_t* s_ra_pw_ta2 = nullptr;      // admin password: the repeat field
lv_timer_t* s_ra_timer = nullptr;
char      s_ra_title[48] = "";

// Per-view widgets (nulled with the page).
lv_obj_t* s_ra_row[RA_F_COUNT];       // the action row of each field shown
lv_obj_t* s_ra_stat_lbl[16];          // overview status values
lv_obj_t* s_ra_updated_lbl = nullptr; // "Updated ... ago"
lv_obj_t* s_ra_login_ta = nullptr;
lv_obj_t* s_ra_login_sw = nullptr;
lv_obj_t* s_ra_login_note = nullptr;
lv_obj_t* s_ra_list = nullptr;        // neighbours / clients list column
lv_obj_t* s_ra_more_btn = nullptr;
lv_obj_t* s_ra_act_lbl = nullptr;     // last action result
lv_obj_t* s_ra_owner_lbl = nullptr;
lv_obj_t* s_ra_radio_ta[4];           // freq, bw(dd), sf(dd), cr(dd)
lv_obj_t* s_ra_regions_lbl = nullptr;

void raClearViewPtrs() {
  for (auto& r : s_ra_row) r = nullptr;
  for (auto& l : s_ra_stat_lbl) l = nullptr;
  for (auto& t : s_ra_radio_ta) t = nullptr;
  s_ra_updated_lbl = s_ra_login_ta = s_ra_login_sw = s_ra_login_note = nullptr;
  s_ra_list = s_ra_more_btn = s_ra_act_lbl = s_ra_owner_lbl = s_ra_regions_lbl = nullptr;
}

bool raFieldShown(int f) {
  if (!s_ra) return false;
  const uint8_t who = k_ra_fields[f].who;
  return who == RA_BOTH || (who == RA_RPT && !s_ra->is_room) || (who == RA_ROOM && s_ra->is_room);
}
bool raIsAdmin() { return s_ra && s_ra->role == RAP::ROLE_ADMIN; }
ContactInfo* raContact() { return s_ra ? the_mesh.lookupContactByPubKey(s_ra->pub, PUB_KEY_SIZE) : nullptr; }
const char* raFieldTitle(int f) {
  if (f == RA_F_GUESTPW && s_ra && s_ra->is_room) return TR("Room password");
  return TR(k_ra_titles[f]);
}

void raBuildView(uint8_t v);   // fwd
void raRefreshView();          // fwd
void raRenderField(int f);     // fwd
void raRenderStatus();         // fwd
void raRenderList();           // fwd
void raBack();                 // fwd

// ---- request queue -----------------------------------------------------------------
void raEnqueue(uint8_t kind, uint8_t field, const char* cmd) {
  if (!s_ra) return;
  // A read already waiting for the same field is enough.
  if (kind == RA_OP_GET) {
    for (int i = 0; i < s_ra->q_len; ++i) {
      const RaOp& o = s_ra->q[(s_ra->q_head + i) % RA_Q];
      if (o.kind == RA_OP_GET && o.field == field) return;
    }
    if (s_ra->busy && s_ra->cur.kind == RA_OP_GET && s_ra->cur.field == field) return;
  }
  if (s_ra->q_len >= RA_Q) return;
  RaOp& o = s_ra->q[(s_ra->q_head + s_ra->q_len) % RA_Q];
  o.kind = kind; o.field = field; o.tries = 0;
  snprintf(o.cmd, sizeof o.cmd, "%s", cmd ? cmd : "");
  ++s_ra->q_len;
  if (kind == RA_OP_GET && field < RA_F_COUNT) { s_ra->fst[field] = RA_FS_LOADING; raRenderField(field); }
}
void raEnqueueGet(int f) {
  if (!raFieldShown(f)) return;
  char cmd[64];
  if (f == RA_F_CLOCK || f == RA_F_REGIONS || f == RA_F_VER) snprintf(cmd, sizeof cmd, "%s", k_ra_fields[f].key);
  else snprintf(cmd, sizeof cmd, "get %s", k_ra_fields[f].key);
  raEnqueue(RA_OP_GET, (uint8_t)f, cmd);
}
void raQueueClear() { if (s_ra) { s_ra->q_len = 0; s_ra->busy = false; } }

uint32_t raReplyWindowMs() {
  ContactInfo* c = raContact();
  return (c && c->out_path_len != OUT_PATH_UNKNOWN) ? 12000u : 22000u;
}

// Send the current op. Returns false when it could not even be sent.
bool raSendCur() {
  ContactInfo* c = raContact();
  if (!c) return false;
  RaOp& o = s_ra->cur;
  int r = MSG_SEND_FAILED;
  switch (o.kind) {
    case RA_OP_GET: case RA_OP_SET: case RA_OP_ACT: case RA_OP_FIRE: case RA_OP_PERM: {
      // The "XX|" prefix comes back at the start of the reply (CommonCLI): the two
      // characters cycle through w0..wz so each reply names the request it answers.
      static const char k36[] = "0123456789abcdefghijklmnopqrstuvwxyz";
      s_ra->cur_id[0] = 'w'; s_ra->cur_id[1] = k36[s_ra->id_seq++ % 36]; s_ra->cur_id[2] = 0;
      char line[160];
      snprintf(line, sizeof line, "%s|%s", s_ra->cur_id, o.cmd);
      r = the_mesh.uiSendAdminCommand(*c, line);
      break;
    }
    case RA_OP_STATUS:
      r = the_mesh.sendStatusPingForUI(*c);
      break;
    case RA_OP_OWNER: {
      const uint8_t req[] = { RAP::REQ_GET_OWNER_INFO, 0, 0, 0, 0, (uint8_t)esp_random(), (uint8_t)esp_random(), (uint8_t)esp_random(), (uint8_t)esp_random() };
      r = the_mesh.uiSendBinaryRequest(*c, req, sizeof req);
      break;
    }
    case RA_OP_NEIGH: {
      uint8_t req[12];
      const uint8_t n = RAP::buildNeighboursReq(req, 11, (uint16_t)s_ra->nb_n, s_ra->nb_order, 6, (uint32_t)esp_random());
      r = the_mesh.uiSendBinaryRequest(*c, req, n);
      break;
    }
    case RA_OP_ACL: {
      const uint8_t req[] = { RAP::REQ_GET_ACCESS_LIST, 0, 0, (uint8_t)esp_random(), (uint8_t)esp_random(), (uint8_t)esp_random(), (uint8_t)esp_random() };
      r = the_mesh.uiSendBinaryRequest(*c, req, sizeof req);
      break;
    }
  }
  if (r != MSG_SEND_SENT_FLOOD && r != MSG_SEND_SENT_DIRECT) return false;
  markMeshRequest();
  s_ra->cur_deadline = millis() + (o.kind == RA_OP_FIRE ? 3000u : raReplyWindowMs());
  if (!s_ra->cur_deadline) s_ra->cur_deadline = 1;
  return true;
}

void raFinishCur() {
  if (!s_ra) return;
  const uint8_t k = s_ra->cur.kind;
  if (k == RA_OP_STATUS || k == RA_OP_OWNER || k == RA_OP_NEIGH || k == RA_OP_ACL) the_mesh.cancelUIPingPending();
  s_ra->busy = false;
}

void raOnTimeout() {
  RaOp& o = s_ra->cur;
  if (o.kind == RA_OP_FIRE) {   // nothing comes back from a reboot: it went out
    snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", TR("Reboot sent. The node is back in about 10 s."));
    raFinishCur();
    raRefreshView();
    return;
  }
  // A read gets one more try (a fresh timestamp, a fresh id); writes do not repeat.
  const bool read = (o.kind == RA_OP_GET || o.kind == RA_OP_STATUS || o.kind == RA_OP_OWNER ||
                     o.kind == RA_OP_NEIGH || o.kind == RA_OP_ACL);
  if (read && o.tries == 0) {
    o.tries = 1;
    if (o.kind != RA_OP_GET) the_mesh.cancelUIPingPending();
    if (raSendCur()) return;
  }
  raFinishCur();
  if (o.field < RA_F_COUNT && (o.kind == RA_OP_GET || o.kind == RA_OP_SET)) {
    s_ra->fst[o.field] = RA_FS_TIMEOUT;
    raRenderField(o.field);
  }
  if (o.kind == RA_OP_SET || o.kind == RA_OP_ACT || o.kind == RA_OP_PERM) {
    snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", TR("No reply. It may or may not have been applied: read it again."));
    if (g_lv.task) g_lv.task->showAlert(TR("No reply from the node"), 1800);
  }
  raRefreshView();
}

void raPump() {
  if (!s_ra || !s_ra->open) return;
  if (s_ra->logging_in) return;
  if (s_ra->busy) {
    if ((int32_t)(millis() - s_ra->cur_deadline) >= 0) raOnTimeout();
    return;
  }
  // The UI's single request slot may be held by someone else (a ping from the
  // contact sheet): binary requests wait for it.
  while (s_ra->q_len > 0) {
    RaOp& nx = s_ra->q[s_ra->q_head];
    const bool bin = nx.kind == RA_OP_STATUS || nx.kind == RA_OP_OWNER || nx.kind == RA_OP_NEIGH || nx.kind == RA_OP_ACL;
    if (bin && the_mesh.hasUIRequestPending()) return;
    s_ra->cur = nx;
    s_ra->q_head = (s_ra->q_head + 1) % RA_Q;
    --s_ra->q_len;
    s_ra->busy = true;
    if (raSendCur()) return;
    s_ra->busy = false;
    if (s_ra->cur.field < RA_F_COUNT && s_ra->cur.kind == RA_OP_GET) { s_ra->fst[s_ra->cur.field] = RA_FS_TIMEOUT; raRenderField(s_ra->cur.field); }
    if (g_lv.task) g_lv.task->showAlert(TR("Send failed"), 1200);
  }
}

// ---- reply hooks (called from the UITask callbacks) --------------------------------
bool raForUs(const ContactInfo& c) { return s_ra && s_ra->open && memcmp(c.id.pub_key, s_ra->pub, 32) == 0; }

void raAfterLogin();   // fwd
void raActConfirmed(); // fwd

bool raOnLoginResult(const ContactInfo& contact, bool success, uint8_t perms) {
  if (!raForUs(contact) || !s_ra->logging_in) return false;
  s_ra->logging_in = false;
  loginWaitCancel();
  if (!success) {
    memset(s_ra->login_pw, 0, sizeof s_ra->login_pw);
    if (s_ra_login_note) lv_label_set_text(s_ra_login_note, TR("Login refused."));
    return true;
  }
  // Role: the ACL byte when the server sent the new LOGIN_OK, else the admin flag.
  const MyMesh::UiLoginInfo& li = the_mesh.uiLastLoginInfo();
  int8_t role = (perms == 1) ? RAP::ROLE_ADMIN : RAP::ROLE_GUEST;
  if (li.valid && memcmp(li.pub4, contact.id.pub_key, 4) == 0) role = (int8_t)RAP::aclRole(li.acl);
  if (perms == 1) role = RAP::ROLE_ADMIN;
  s_ra->role = role;
  s_ra->login_ms = millis();
#if defined(ESP32)
  // Remember (or forget) a typed password once it worked. A blank guest login
  // leaves a remembered admin password alone.
  if (s_ra->login_pw[0]) touchPrefsSetRepeaterPassword(contact.id.pub_key, s_ra->login_remember ? s_ra->login_pw : "");
#endif
  memset(s_ra->login_pw, 0, sizeof s_ra->login_pw);
  raAfterLogin();
  return true;
}

void raFieldReply(int f, const char* body) {
  const char* v = RAP::cliGetValue(body);
  if (v) {
    snprintf(s_ra->val[f], RA_VAL, "%s", v);
    s_ra->fst[f] = RA_FS_OK;
  } else if (f == RA_F_CLOCK || f == RA_F_REGIONS || f == RA_F_VER) {
    snprintf(s_ra->val[f], RA_VAL, "%s", body);   // plain-text commands, no "> "
    s_ra->fst[f] = RA_FS_OK;
  } else {
    snprintf(s_ra->last_err[f], sizeof s_ra->last_err[f], "%s", body);
    s_ra->fst[f] = RA_FS_ERROR;
  }
  if (f == RA_F_VER && s_ra->fst[f] == RA_FS_OK && !s_ra->fw[0]) snprintf(s_ra->fw, sizeof s_ra->fw, "%s", s_ra->val[f]);
  raRenderField(f);
}

bool raOnCliReply(const ContactInfo& contact, const char* text) {
  if (!raForUs(contact) || !text) return false;
  // Ours carry "wX|"; anything else is the console's (typed there, no prefix).
  if (!(text[0] == 'w' && text[1] && text[2] == '|')) return false;
  if (!s_ra->busy || text[0] != s_ra->cur_id[0] || text[1] != s_ra->cur_id[1]) return true;   // late reply to a retried/abandoned request: drop
  const char* body = text + 3;
  const RaOp o = s_ra->cur;
  raFinishCur();
  switch (o.kind) {
    case RA_OP_GET:
      raFieldReply(o.field, body);
      break;
    case RA_OP_SET: {
      const RAP::CliResult res = RAP::cliClassify(body);
      if (res == RAP::CliResult::Error) {
        snprintf(s_ra->last_err[o.field], sizeof s_ra->last_err[o.field], "%s", body);
        s_ra->fst[o.field] = RA_FS_ERROR;
        raRenderField(o.field);
        char msg[96]; snprintf(msg, sizeof msg, "%s: %s", TR("Not saved"), body);
        if (g_lv.task) g_lv.task->showAlert(msg, 2600);
      } else {
        s_ra->last_err[o.field][0] = 0;
        if (o.field == RA_F_RADIO) {
          snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", body);
          // The new radio settings wait for a reboot: offer it right here.
          snprintf(s_ra->pend_cmd, sizeof s_ra->pend_cmd, "%s", "reboot");
          s_ra->pend_kind = RA_OP_FIRE;
          showConfirm(TR("Saved. The node uses the new radio settings after a reboot. Reboot it now?"), TR("Reboot"), raActConfirmed);
        } else if (RAP::cliNeedsReboot(body)) {
          if (g_lv.task) g_lv.task->showAlert(TR("Saved. Applies after a reboot."), 2200);
        } else if (g_lv.task) {
          g_lv.task->showAlert(TR("Saved"), 1000);
        }
        // Never leave the old value up: read back what the node now holds.
        if (o.field < RA_F_COUNT && k_ra_fields[o.field].kind != RA_K_INFO) raEnqueueGet(o.field);
        else if (o.field == RA_F_RADIO) raEnqueueGet(RA_F_RADIO);
        else { s_ra->fst[o.field] = RA_FS_OK; raRenderField(o.field); }
      }
      break;
    }
    case RA_OP_ACT:
      snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", body);
      if (g_lv.task && strncmp(o.cmd, "password ", 9) != 0) g_lv.task->showAlert(body, 2000);   // never toast a password
      if (strncmp(o.cmd, "clock", 5) == 0 || strncmp(o.cmd, "time ", 5) == 0) raEnqueueGet(RA_F_CLOCK);
      if (strncmp(o.cmd, "clear stats", 11) == 0) raEnqueue(RA_OP_STATUS, 0xFF, "");
      if (strncmp(o.cmd, "password ", 9) == 0 && RAP::startsWithNoCase(body, "password now")) {
#if defined(ESP32)
        // The remembered password follows the change, or the next login fails.
        char saved[TOUCH_REPEATER_PW_LEN];
        if (touchPrefsGetRepeaterPassword(s_ra->pub, saved, sizeof saved) > 0)
          touchPrefsSetRepeaterPassword(s_ra->pub, s_ra->admin_pw_new);
#endif
        memset(s_ra->admin_pw_new, 0, sizeof s_ra->admin_pw_new);
        snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", TR("Admin password changed."));
      }
      break;
    case RA_OP_PERM:
      snprintf(s_ra->act_result, sizeof s_ra->act_result, "%s", body);
      if (g_lv.task) g_lv.task->showAlert(RAP::cliClassify(body) == RAP::CliResult::Ok ? TR("Access changed") : body, 2000);
      s_ra->acl_loaded = false;
      raEnqueue(RA_OP_ACL, 0xFF, "");
      break;
    default: break;
  }
  raRefreshView();
  return true;
}

bool raOnStatusReply(const ContactInfo& contact, const uint8_t* data, size_t len) {
  if (!raForUs(contact) || !s_ra->busy || s_ra->cur.kind != RA_OP_STATUS) return false;
  raFinishCur();
  RAP::Status st;
  if (RAP::parseStatus(data, len, s_ra->is_room, st)) {
    s_ra->status = st;
    s_ra->has_status = true;
    s_ra->status_ms = millis();
  }
  raRenderStatus();
  return true;
}

bool raOnBinaryReply(const ContactInfo& contact, const uint8_t* data, size_t len) {
  if (!raForUs(contact) || !s_ra->busy) return false;
  const uint8_t k = s_ra->cur.kind;
  if (k != RA_OP_OWNER && k != RA_OP_NEIGH && k != RA_OP_ACL) return false;
  raFinishCur();
  if (k == RA_OP_OWNER) {
    char txt[180];
    const size_t n = len < sizeof txt - 1 ? len : sizeof txt - 1;
    memcpy(txt, data, n); txt[n] = 0;
    const char *ver, *nm, *own;
    RAP::splitOwnerInfo(txt, &ver, &nm, &own);
    copyUtf8ReplacingMissingGlyphs(&g_font_12, s_ra->fw, sizeof s_ra->fw, ver);
    copyUtf8ReplacingMissingGlyphs(&g_font_12, s_ra->owner, sizeof s_ra->owner, own);
    s_ra->has_owner = true;
    raRenderStatus();
  } else if (k == RA_OP_NEIGH) {
    uint16_t total = 0;
    const int got = RAP::parseNeighbours(data, len, 6, s_ra->nb + s_ra->nb_n, RA_NB_MAX - s_ra->nb_n, &total);
    if (got >= 0) { s_ra->nb_n += got; s_ra->nb_total = total; }
    if (got == 0) s_ra->nb_total = (uint16_t)s_ra->nb_n;   // nothing more to page
    s_ra->nb_loaded = true;
    raRenderList();
  } else {
    const int got = RAP::parseAcl(data, len, s_ra->acl, RA_ACL_MAX);
    s_ra->acl_n = got < 0 ? 0 : got;
    s_ra->acl_loaded = true;
    raRenderList();
  }
  return true;
}

// ---- formatting ---------------------------------------------------------------------
// A field's value as the row shows it.
void raFieldText(int f, char* out, size_t cap) {
  const uint8_t st = s_ra->fst[f];
  if (st == RA_FS_LOADING) { snprintf(out, cap, "%s", TR("Reading...")); return; }
  if (st == RA_FS_SAVING)  { snprintf(out, cap, "%s", TR("Saving...")); return; }
  if (st == RA_FS_TIMEOUT) { snprintf(out, cap, "%s", TR("No reply")); return; }
  if (st == RA_FS_ERROR)   { snprintf(out, cap, "%s", s_ra->last_err[f][0] ? s_ra->last_err[f] : TR("Error")); return; }
  if (st != RA_FS_OK)      { snprintf(out, cap, "%s", "-"); return; }
  const char* v = s_ra->val[f];
  const RaFieldDef& d = k_ra_fields[f];
  if (f == RA_F_GUESTPW) { snprintf(out, cap, "%s", v[0] ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : TR("None")); return; }
  if (d.kind == RA_K_BOOL) { const int b = RAP::cliBool(v); snprintf(out, cap, "%s", b == 1 ? TR("On") : b == 0 ? TR("Off") : v); return; }
  if (f == RA_F_TX) { int t = atoi(v); if (t > 127) t -= 256; snprintf(out, cap, "%d dBm", t); return; }
  if (f == RA_F_ADV)  { const int m = atoi(v); if (m == 0) snprintf(out, cap, "%s", TR("Off")); else snprintf(out, cap, "%d min", m); return; }
  if (f == RA_F_FADV) { const int h = atoi(v); if (h == 0) snprintf(out, cap, "%s", TR("Off")); else snprintf(out, cap, "%d h", h); return; }
  if (f == RA_F_RADIO) {
    RAP::Radio r;
    if (RAP::parseRadio(v, r)) { snprintf(out, cap, "%.3f MHz SF%d BW%g CR%d", (double)r.freq, r.sf, (double)r.bw, r.cr); return; }
  }
  if (f == RA_F_OWNER) {   // '|' is the CLI's line break
    char t[RA_VAL]; snprintf(t, sizeof t, "%s", v);
    for (char* p = t; *p; ++p) if (*p == '|') *p = ' ';
    copyUtf8ReplacingMissingGlyphs(&g_font_12, out, cap, t);
    return;
  }
  copyUtf8ReplacingMissingGlyphs(&g_font_12, out, cap, v);
}

void raRenderField(int f) {
  if (!s_ra || f < 0 || f >= RA_F_COUNT) return;
  lv_obj_t* row = s_ra_row[f];
  if (!row || !lv_obj_is_valid(row)) return;
  char t[RA_VAL + 8];
  raFieldText(f, t, sizeof t);
  if (f == RA_F_REGIONS || f == RA_F_CLOCK || f == RA_F_VER) {   // these rows are plain labels
    setLabelIfChanged(row, t);
    return;
  }
  setActionValue(row, t);
  lv_obj_t* vl = (lv_obj_t*)lv_obj_get_user_data(row);
  if (vl) lv_obj_set_style_text_color(vl, lv_color_hex(s_ra->fst[f] == RA_FS_ERROR || s_ra->fst[f] == RA_FS_TIMEOUT
                                                         ? COLOR_STATUS_DANGER_TEXT : COLOR_SUB), LV_PART_MAIN);
}

// ---- page chrome ------------------------------------------------------------------------
bool raTwoCol() {
#if CAP_LARGE_SCREEN
  return lv_disp_get_hor_res(nullptr) > lv_disp_get_ver_res(nullptr) && lv_disp_get_hor_res(nullptr) >= 600;
#else
  return false;
#endif
}
lv_coord_t raColW() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  return raTwoCol() ? (sw - 34) / 2 : sw - 22;
}
// A card under a section name, s_settings_content_w set for the kit's rows.
lv_obj_t* raCard(const char* title_tr) {
  lv_obj_t* c = settingsCardIn(s_ra_page, title_tr ? title_tr : "");
  lv_obj_t* wrap = lv_obj_get_parent(lv_obj_get_parent(c));
  const lv_coord_t w = raColW();
  lv_obj_set_width(wrap, w);
  lv_obj_set_width(lv_obj_get_parent(c), w);
  lv_obj_set_width(c, w - 16);
  s_settings_content_w = w - 16 - 6;
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, SC(10), LV_PART_MAIN);
  return c;
}
lv_obj_t* raLabel(lv_obj_t* parent, const char* text, const lv_font_t* f, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_width(l, s_settings_content_w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text(l, text ? text : "");
  lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  return l;
}

void raSetTitle(uint8_t v) {
  static const char* const k_view_titles[RA_V_COUNT] = {
    nullptr, nullptr, "Neighbours", "Clients", "Identity & location", "Radio",
    "Repeating & limits", "Adverts", "Passwords & access", "Regions", "Actions"
  };
  if (v <= RA_V_OVERVIEW || !k_view_titles[v]) snprintf(s_ra_title, sizeof s_ra_title, "%s", s_ra->name);
  else snprintf(s_ra_title, sizeof s_ra_title, "%s", TR(k_view_titles[v]));
  s_apppage_title = s_ra_title;
  updateGlobalStatusBar();
}

void raClose();   // fwd
void raCloseEditSheet();

void raNewPage() {
  if (s_ra_page) {
    lv_indev_t* a = lv_indev_get_act();
    if (a) lv_indev_wait_release(a);
    lv_indev_reset(nullptr, nullptr);   // a flick in flight must not throw into the freed page
    raClearViewPtrs();
    lv_obj_del_async(s_ra_page);
    s_ra_page = nullptr;
  }
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  s_ra_page = lv_obj_create(s_ra_root);
  lv_obj_remove_style_all(s_ra_page);
  lv_obj_set_size(s_ra_page, sw, appPageContentH());
  lv_obj_set_pos(s_ra_page, 0, 0);
  // The tall bar's glass row covers the top of the page: start the content below it.
  const lv_coord_t bar_cover = (lv_coord_t)(STATUSBAR_H * 2) - appPageContentTop();
  lv_obj_set_style_pad_top(s_ra_page, LV_MAX(0, bar_cover) + 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(s_ra_page, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_left(s_ra_page, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(s_ra_page, 10, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_ra_page, raTwoCol() ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_ra_page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_ra_page, LV_SCROLLBAR_MODE_AUTO);
}

// ---- edit sheet -------------------------------------------------------------------
void raCloseEditSheet() {
  if (s_ra_edit_root) { hideKb(); popupClose(&s_ra_edit_root); }
  s_ra_edit_ta = s_ra_edit_choice = s_ra_pw_ta2 = nullptr;
  s_ra_edit_pick = -1;
}

// Check and normalise a typed value. Returns false (and toasts why) when it is not
// something the node accepts, so nothing goes on the air for a typo.
bool raValidate(int f, const char* in, char* out, size_t cap) {
  const RaFieldDef& d = k_ra_fields[f];
  while (*in == ' ') ++in;
  if (d.kind == RA_K_INT || d.kind == RA_K_FLOAT) {
    char* end = nullptr;
    const double x = strtod(in, &end);
    while (end && *end == ' ') ++end;
    if (!in[0] || !end || *end) { if (g_lv.task) g_lv.task->showAlert(TR("Enter a number"), 1400); return false; }
    bool ok = x >= d.lo - 1e-6 && x <= d.hi + 1e-6;
    if (d.kind == RA_K_INT && x != (double)(long)x) ok = false;
    if (f == RA_F_ADV && x != 0 && x < 60) ok = false;
    if (f == RA_F_FADV && x != 0 && x < 3) ok = false;
    if (!ok) { if (g_lv.task) g_lv.task->showAlert(TR(k_ra_helps[f]), 2200); return false; }
    if (d.kind == RA_K_INT) snprintf(out, cap, "%ld", (long)x);
    else snprintf(out, cap, "%s", in);
    return true;
  }
  if (f == RA_F_NAME) {
    if (!in[0]) { if (g_lv.task) g_lv.task->showAlert(TR("A name is required"), 1400); return false; }
    if (strpbrk(in, "[]\\:,?*")) { if (g_lv.task) g_lv.task->showAlert(TR(k_ra_helps[f]), 2200); return false; }
  }
  snprintf(out, cap, "%s", in);
  return true;
}

void raSendSet(int f, const char* value) {
  char cmd[150];
  snprintf(cmd, sizeof cmd, "set %s %s", k_ra_fields[f].key, value);
  s_ra->fst[f] = RA_FS_SAVING;
  raRenderField(f);
  raEnqueue(RA_OP_SET, (uint8_t)f, cmd);
}

void raEditSaveCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED && lv_event_get_code(e) != LV_EVENT_READY) return;
  if (!s_ra) return;
  const int f = s_ra->edit_field;
  char v[RA_VAL];
  if (k_ra_fields[f].kind == RA_K_BOOL || k_ra_fields[f].kind == RA_K_CHOICE) {
    if (s_ra_edit_pick < 0) { raCloseEditSheet(); return; }
    if (k_ra_fields[f].kind == RA_K_BOOL) snprintf(v, sizeof v, "%s", s_ra_edit_pick ? "on" : "off");
    else {
      const char* c = k_ra_fields[f].choices;
      for (int i = 0; i < s_ra_edit_pick && c; ++i) { c = strchr(c, '\n'); if (c) ++c; }
      if (!c) { raCloseEditSheet(); return; }
      size_t n = strcspn(c, "\n"); if (n >= sizeof v) n = sizeof v - 1;
      memcpy(v, c, n); v[n] = 0;
    }
  } else {
    if (!s_ra_edit_ta) return;
    kbMirrorSyncToReal();
    char t[RA_VAL];
    snprintf(t, sizeof t, "%s", lv_textarea_get_text(s_ra_edit_ta));
    if (f == RA_F_OWNER) for (char* p = t; *p; ++p) if (*p == '\n') *p = '|';
    if (!raValidate(f, t, v, sizeof v)) return;
  }
  raCloseEditSheet();
  raSendSet(f, v);
}
void raEditCancelCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raCloseEditSheet(); }
void raEditPickCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  s_ra_edit_pick = setChoiceIndex(e);
  setChoicePick(s_ra_edit_choice, s_ra_edit_pick);
}

// The sheet's frame: dimmed backdrop, a card at the top (the keyboard rises from
// the bottom), a title. Returns the card's content column.
lv_obj_t* raSheetBegin(const char* title) {
  raCloseEditSheet();
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_ra_edit_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_ra_edit_root);
  lv_obj_set_size(s_ra_edit_root, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(s_ra_edit_root, 0, STATUSBAR_H);
  lv_obj_set_style_bg_color(s_ra_edit_root, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_ra_edit_root, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(s_ra_edit_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(s_ra_edit_root, LV_DIR_VER);
  // The app's tall bar (title row + glass row) stays over the page: start below it.
  lv_obj_set_style_pad_top(s_ra_edit_root, LV_MAX(0, (lv_coord_t)(STATUSBAR_H * 2) - STATUSBAR_H) + 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(s_ra_edit_root, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_ra_edit_root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_ra_edit_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const lv_coord_t cw = LV_MIN((lv_coord_t)(sw - 16), (lv_coord_t)PSC(320));
  lv_obj_t* card = lv_obj_create(s_ra_edit_root);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, cw, LV_SIZE_CONTENT);
  styleSurface(card, COLOR_PANEL, 12);
  lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, SC(10), LV_PART_MAIN);
  lv_obj_set_style_pad_row(card, SC(8), LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  s_settings_content_w = cw - 2 * SC(10);
  lv_obj_t* t = raLabel(card, title, &g_font_semi_14, COLOR_TEXT);
  (void)t;
  return card;
}
void raSheetButtons(lv_obj_t* card, const char* ok_label, lv_event_cb_t ok_cb, uint8_t kind = SET_PRIMARY) {
  lv_obj_t* row = setFieldRow(card);
  const lv_coord_t bw = setFieldW(2);
  lv_obj_t* c = lv_btn_create(row);
  styleButton(c);
  lv_obj_set_size(c, bw, SC(34));
  lv_obj_add_event_cb(c, raEditCancelCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cl = lv_label_create(c); lv_label_set_text(cl, TR("Cancel")); useChainedFont(cl); lv_obj_center(cl);
  lv_obj_t* o = lv_btn_create(row);
  if (kind == SET_DANGER) styleDanger(o); else stylePrimary(o);
  lv_obj_set_size(o, bw, SC(34));
  lv_obj_add_event_cb(o, ok_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ol = lv_label_create(o); lv_label_set_text(ol, ok_label); useChainedFont(ol); lv_obj_center(ol);
}

void raOpenEdit(int f) {
  if (!s_ra || !raIsAdmin()) return;
  const RaFieldDef& d = k_ra_fields[f];
  s_ra->edit_field = (uint8_t)f;
  lv_obj_t* card = raSheetBegin(raFieldTitle(f));
  const char* help = k_ra_helps[f][0] ? TR(k_ra_helps[f]) : nullptr;
  if (f == RA_F_GUESTPW)
    help = s_ra->is_room ? TR("Members log in with it to post. Up to 15 characters.")
                         : TR("Logs in as a guest. Blank lets anyone in as a guest. Up to 15 characters.");
  if (help) raLabel(card, help, &g_font_12, COLOR_SUB);
  if (d.kind == RA_K_BOOL || d.kind == RA_K_CHOICE) {
    const char* opts[6]; char buf[6][16]; int n = 0;
    if (d.kind == RA_K_BOOL) { opts[0] = TR("Off"); opts[1] = TR("On"); n = 2; }
    else {
      for (const char* c = d.choices; c && *c && n < 6; ++n) {
        size_t k = strcspn(c, "\n"); if (k >= sizeof buf[n]) k = sizeof buf[n] - 1;
        memcpy(buf[n], c, k); buf[n][k] = 0;
        opts[n] = TR(raLoopLabel(n));
        c += k; if (*c == '\n') ++c;
      }
    }
    int cur = -1;
    if (s_ra->fst[f] == RA_FS_OK) {
      if (d.kind == RA_K_BOOL) cur = RAP::cliBool(s_ra->val[f]);
      else for (int i = 0; i < n; ++i) if (RAP::startsWithNoCase(s_ra->val[f], buf[i]) && strlen(s_ra->val[f]) == strlen(buf[i])) cur = i;
    }
    s_ra_edit_pick = cur;
    s_ra_edit_choice = setChoiceRow(card, "", "", opts, n, cur, raEditPickCb);
  } else {
    s_ra_edit_ta = setField(card, "", "", d.maxlen ? d.maxlen : 64);
    if (s_ra->fst[f] == RA_FS_OK) {
      char cur[RA_VAL];
      snprintf(cur, sizeof cur, "%s", s_ra->val[f]);
      if (f == RA_F_TX) { int t = atoi(cur); if (t > 127) t -= 256; snprintf(cur, sizeof cur, "%d", t); }
      lv_textarea_set_text(s_ra_edit_ta, cur);
    }
    if (f == RA_F_GUESTPW) lv_textarea_set_password_mode(s_ra_edit_ta, true);
    if (d.kind == RA_K_INT || d.kind == RA_K_FLOAT) lv_textarea_set_accepted_chars(s_ra_edit_ta, "0123456789.-");
    lv_obj_add_event_cb(s_ra_edit_ta, raEditSaveCb, LV_EVENT_READY, nullptr);
    if (g_lv.keyboard) kbMirrorBind(s_ra_edit_ta);
  }
  raSheetButtons(card, TR("Save"), raEditSaveCb);
}

void raRowEditCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  raOpenEdit((int)(intptr_t)lv_event_get_user_data(e));
}

// A field's row: name, current value, chevron (admin) or plain (guest).
lv_obj_t* raFieldRow(lv_obj_t* card, int f) {
  if (!raFieldShown(f)) return nullptr;
  const bool editable = raIsAdmin() && k_ra_fields[f].kind != RA_K_INFO;
  lv_obj_t* r = setActionRowRaw(card, nullptr, raFieldTitle(f), "", editable ? raRowEditCb : nullptr,
                                (void*)(intptr_t)f, SET_NORMAL, editable);
  s_ra_row[f] = r;
  raRenderField(f);
  if (s_ra->fst[f] == RA_FS_UNKNOWN || s_ra->fst[f] == RA_FS_TIMEOUT) raEnqueueGet(f);
  return r;
}

// ---- views ------------------------------------------------------------------------------
void raGoCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  raBuildView((uint8_t)(intptr_t)lv_event_get_user_data(e));
}

// Login
void raLoginSubmit(bool guest) {
  if (!s_ra || s_ra->logging_in) return;
  ContactInfo* c = raContact();
  if (!c) { if (g_lv.task) g_lv.task->showAlert(TR("Contact gone"), 1200); return; }
  const char* pw = "";
  if (!guest && s_ra_login_ta) { kbMirrorSyncToReal(); pw = lv_textarea_get_text(s_ra_login_ta); }
  snprintf(s_ra->login_pw, sizeof s_ra->login_pw, "%s", pw ? pw : "");
  s_ra->login_remember = s_ra_login_sw && lv_obj_has_state(s_ra_login_sw, LV_STATE_CHECKED);
  hideKb();
  roomReloginClear();   // an explicit login supersedes a background room re-login
  const int r = the_mesh.uiSendAdminLogin(*c, s_ra->login_pw);
  if (r != MSG_SEND_SENT_FLOOD && r != MSG_SEND_SENT_DIRECT) {
    if (s_ra_login_note) lv_label_set_text(s_ra_login_note, TR("Send failed"));
    return;
  }
  markMeshRequest();
  s_ra->logging_in = true;
  s_ra->login_deadline = millis() + 20000u;
  if (s_ra_login_note) lv_label_set_text(s_ra_login_note, TR("Logging in..."));
}
void raLoginCb(lv_event_t* e) {
  const lv_event_code_t c = lv_event_get_code(e);
  if (c != LV_EVENT_CLICKED && c != LV_EVENT_READY) return;
  raLoginSubmit(false);
}
void raGuestCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raLoginSubmit(true); }

void raBuildLogin() {
  lv_obj_t* card = raCard(TR("Log in"));
  raLabel(card, s_ra->is_room ? TR("Admin password for full control, or the room password.")
                              : TR("Admin password for full control. A guest sees status and neighbours."),
          &g_font_12, COLOR_SUB);
  s_ra_login_ta = setField(card, "Password", "Up to 15 characters", 15);
  lv_textarea_set_password_mode(s_ra_login_ta, true);
  lv_obj_add_event_cb(s_ra_login_ta, raLoginCb, LV_EVENT_READY, nullptr);
  bool has_saved = false;
#if defined(ESP32)
  { char saved[TOUCH_REPEATER_PW_LEN];
    if (touchPrefsGetRepeaterPassword(s_ra->pub, saved, sizeof saved) > 0) { lv_textarea_set_text(s_ra_login_ta, saved); has_saved = true; }
    memset(saved, 0, sizeof saved); }
#endif
  s_ra_login_sw = setSwitchRow(card, "Remember password", nullptr, true, nullptr);
  setButton(card, UI_ICON_LOCK_OPEN, "Log in", raLoginCb, nullptr, SET_PRIMARY);
  setButton(card, UI_ICON_USER, "Log in as guest", raGuestCb);
  s_ra_login_note = raLabel(card, s_ra->logging_in ? TR("Logging in...") : "", &g_font_12, COLOR_SUB);
  if (!has_saved && g_lv.keyboard) kbMirrorBind(s_ra_login_ta);
  // A remembered password: go straight on, the way the old prompt's one tap did.
#if !defined(DOC_ADMIN_AUDIT)   // the guest-only capture build must never send a remembered password
  if (has_saved && !s_ra->logging_in && s_ra->role < 0) raLoginSubmit(false);
#endif
}

// Overview
enum { RA_ST_BATT = 0, RA_ST_UP, RA_ST_NOISE, RA_ST_SIGNAL, RA_ST_RECV, RA_ST_SENT, RA_ST_FLOOD, RA_ST_DIRECT,
       RA_ST_AIR, RA_ST_QUEUE, RA_ST_DUPS, RA_ST_ERR, RA_ST_POSTS, RA_ST_FW, RA_ST_COUNT };

void raRenderStatus() {
  if (!s_ra) return;
  char b[64];
  const RAP::Status& s = s_ra->status;
  const bool h = s_ra->has_status;
  auto set = [&](int i, const char* t) { if (s_ra_stat_lbl[i] && lv_obj_is_valid(s_ra_stat_lbl[i])) setLabelIfChanged(s_ra_stat_lbl[i], t); };
  if (h) {
    snprintf(b, sizeof b, "%u.%02u V", s.batt_mv / 1000, (s.batt_mv % 1000) / 10); set(RA_ST_BATT, s.batt_mv ? b : "-");
    RAP::fmtDuration(s.uptime_secs, b, sizeof b); set(RA_ST_UP, b);
    snprintf(b, sizeof b, "%d dBm", (int)s.noise_floor); set(RA_ST_NOISE, b);
    snprintf(b, sizeof b, "%d dBm \xC2\xB7 %.1f dB", (int)s.last_rssi, (double)s.last_snr_q4 / 4.0); set(RA_ST_SIGNAL, b);
    snprintf(b, sizeof b, "%lu", (unsigned long)s.recv); set(RA_ST_RECV, b);
    snprintf(b, sizeof b, "%lu", (unsigned long)s.sent); set(RA_ST_SENT, b);
    snprintf(b, sizeof b, "%lu / %lu", (unsigned long)s.recv_flood, (unsigned long)s.sent_flood); set(RA_ST_FLOOD, b);
    snprintf(b, sizeof b, "%lu / %lu", (unsigned long)s.recv_direct, (unsigned long)s.sent_direct); set(RA_ST_DIRECT, b);
    char tx[16], rx[16] = "-"; RAP::fmtDuration(s.air_secs, tx, sizeof tx);
    if (s.has_rx) RAP::fmtDuration(s.rx_air_secs, rx, sizeof rx);
    snprintf(b, sizeof b, "%s / %s", tx, rx);
    set(RA_ST_AIR, b);
    snprintf(b, sizeof b, "%u", (unsigned)s.queue_len); set(RA_ST_QUEUE, b);
    snprintf(b, sizeof b, "%u %s \xC2\xB7 %u %s", (unsigned)s.flood_dups, TR("flood"), (unsigned)s.direct_dups, TR("direct")); set(RA_ST_DUPS, b);
    if (s.has_rx) snprintf(b, sizeof b, "%lu", (unsigned long)s.recv_errors); else snprintf(b, sizeof b, "0x%04X", (unsigned)s.err_events);
    set(RA_ST_ERR, b);
    snprintf(b, sizeof b, "%u \xC2\xB7 %u %s", (unsigned)s.posted, (unsigned)s.post_push, TR("pushed")); set(RA_ST_POSTS, b);
  } else {
    const bool waiting = s_ra->busy && s_ra->cur.kind == RA_OP_STATUS;
    for (int i = 0; i < RA_ST_FW; ++i) set(i, waiting ? TR("Reading...") : "-");
  }
  set(RA_ST_FW, s_ra->fw[0] ? s_ra->fw : "-");
  if (s_ra_owner_lbl && lv_obj_is_valid(s_ra_owner_lbl)) {
    if (s_ra->owner[0]) { setLabelIfChanged(s_ra_owner_lbl, s_ra->owner); lv_obj_clear_flag(setCardOuter(lv_obj_get_parent(s_ra_owner_lbl)), LV_OBJ_FLAG_HIDDEN); }
  }
}

void raRefreshStatusCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  raEnqueue(RA_OP_STATUS, 0xFF, "");
  raRenderStatus();
}
void raTelemetryCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
#if CAP_SD || defined(TLORA_PAGER)
  memcpy(s_telem_node, s_ra->pub, 6);
  snprintf(s_telem_name, sizeof s_telem_name, "%s", s_ra->name);
  s_telem_reading[0] = '\0';
  s_telem_manual_pending = false;
  openTelemetryWindow(s_telem_node, s_telem_name, TELEM_HISTORY);
#else
  ContactInfo* c = raContact();
  if (!c || !g_lv.task) return;
  const int r = the_mesh.sendTelemetryRequestForUI(*c);
  g_lv.task->showAlert((r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) ? TR("Telemetry req\xe2\x80\xa6") : TR("Telemetry req failed"), 1400);
#endif
}
void raConsoleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  ContactInfo* c = raContact();
  if (c) openAdminConsole(*c);
}
void raRelogCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  s_ra->role = -1;
  raBuildView(RA_V_LOGIN);
}

void raBuildOverview() {
  lv_obj_t* card = raCard(TR("Status"));
  s_ra_stat_lbl[RA_ST_BATT]   = setInfoRow(card, "Battery", "-");
  s_ra_stat_lbl[RA_ST_UP]     = setInfoRow(card, "Uptime", "-");
  s_ra_stat_lbl[RA_ST_NOISE]  = setInfoRow(card, "Noise floor", "-");
  s_ra_stat_lbl[RA_ST_SIGNAL] = setInfoRow(card, "Last signal", "-");
  s_ra_stat_lbl[RA_ST_RECV]   = setInfoRow(card, "Received", "-");
  s_ra_stat_lbl[RA_ST_SENT]   = setInfoRow(card, "Sent", "-");
  s_ra_stat_lbl[RA_ST_FLOOD]  = setInfoRow(card, "Flood in / out", "-");
  s_ra_stat_lbl[RA_ST_DIRECT] = setInfoRow(card, "Direct in / out", "-");
  s_ra_stat_lbl[RA_ST_AIR]    = setInfoRow(card, "Airtime TX / RX", "-");
  s_ra_stat_lbl[RA_ST_QUEUE]  = setInfoRow(card, "Queue", "-");
  s_ra_stat_lbl[RA_ST_DUPS]   = setInfoRow(card, "Duplicates", "-");
  s_ra_stat_lbl[RA_ST_ERR]    = setInfoRow(card, "Errors", "-");
  if (s_ra->is_room) s_ra_stat_lbl[RA_ST_POSTS] = setInfoRow(card, "Posts", "-");
  s_ra_updated_lbl = raLabel(card, "", &g_font_12, COLOR_TERTIARY);
  setButton(card, UI_ICON_REFRESH_CW, "Refresh", raRefreshStatusCb);

  // The firmware string is long ("meshcomod-r1.15.0.2-repeater-tc"): a card of its own.
  lv_obj_t* fc = raCard(TR("Firmware"));
  s_ra_stat_lbl[RA_ST_FW] = raLabel(fc, "-", &g_font_14, COLOR_TEXT);
  // The owner's note (repeaters answer it to anyone); hidden until there is one.
  lv_obj_t* oc = raCard(TR("Owner"));
  s_ra_owner_lbl = raLabel(oc, s_ra->owner, &g_font_14, COLOR_TEXT);
  if (!s_ra->owner[0]) lv_obj_add_flag(setCardOuter(oc), LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* nav = raCard(TR("Manage"));
  if (!s_ra->is_room) setActionRow(nav, UI_ICON_RADAR, "Neighbours", "", raGoCb, (void*)(intptr_t)RA_V_NEIGHBOURS);
  setActionRow(nav, UI_ICON_THERMOMETER, "Telemetry", "", raTelemetryCb);
  if (raIsAdmin()) {
    setActionRow(nav, UI_ICON_USERS, "Clients", "", raGoCb, (void*)(intptr_t)RA_V_CLIENTS);
    lv_obj_t* st = raCard(TR("Settings"));
    setActionRow(st, UI_ICON_MAP_PIN, "Identity & location", "", raGoCb, (void*)(intptr_t)RA_V_IDENTITY);
    setActionRow(st, UI_ICON_RADIO, "Radio", "", raGoCb, (void*)(intptr_t)RA_V_RADIO);
    if (!s_ra->is_room) setActionRow(st, UI_ICON_ROUTE, "Repeating & limits", "", raGoCb, (void*)(intptr_t)RA_V_FORWARD);
    setActionRow(st, UI_ICON_RADIO_TOWER, "Adverts", "", raGoCb, (void*)(intptr_t)RA_V_ADVERTS);
    setActionRow(st, UI_ICON_LOCK, "Passwords & access", "", raGoCb, (void*)(intptr_t)RA_V_ACCESS);
    if (!s_ra->is_room) setActionRow(st, UI_ICON_GLOBE, "Regions", "", raGoCb, (void*)(intptr_t)RA_V_REGIONS);
    lv_obj_t* ac = raCard(TR("Advanced"));
    setActionRow(ac, UI_ICON_ZAP, "Actions", "", raGoCb, (void*)(intptr_t)RA_V_ACTIONS);
    setActionRow(ac, UI_ICON_SQUARE_TERMINAL, "Console", "", raConsoleCb);
    setActionRow(ac, UI_ICON_LOCK_OPEN, "Log in again", "", raRelogCb);
  } else {
    lv_obj_t* g = raCard("");
    setNote(g, s_ra->role == RAP::ROLE_GUEST ? "Logged in as a guest: you can read the status and neighbours. Log in with the admin password to change settings."
                                             : "Logged in without admin rights. Log in with the admin password to change settings.",
            SET_TONE_INFO);
    setButton(g, UI_ICON_LOCK_OPEN, "Log in as admin", raRelogCb);
  }
  raRenderStatus();
  if (!s_ra->has_status) raEnqueue(RA_OP_STATUS, 0xFF, "");
  if (!s_ra->is_room && !s_ra->has_owner) raEnqueue(RA_OP_OWNER, 0xFF, "");
  if (s_ra->is_room && raIsAdmin() && !s_ra->fw[0]) raEnqueueGet(RA_F_VER);
}

// Neighbours / clients lists
const char* raNameForPrefix(const uint8_t* pfx, size_t n, char* out, size_t cap) {
  ContactInfo* c = the_mesh.lookupContactByPubKey((uint8_t*)pfx, (int)n);
  if (c && c->name[0]) { copyUtf8ReplacingMissingGlyphs(&g_font_14, out, cap, contactShownName(c->id.pub_key, c->name)); return out; }
  char hex[2 * 8 + 1];
  mesh::Utils::toHex(hex, (uint8_t*)pfx, (int)n);
  snprintf(out, cap, "%s", hex);
  return out;
}

void raNbTapCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_ra->nb_n) return;
  const int idx = [&]() {
    ContactInfo* c = the_mesh.lookupContactByPubKey(s_ra->nb[i].prefix, 6);
    return c ? the_mesh.uiContactIdxByPubKey(c->id.pub_key) : -1;
  }();
  if (idx < 0) { if (g_lv.task) g_lv.task->showAlert(TR("Not in your contacts"), 1400); return; }
  ContactInfo c;
  if (!the_mesh.getContactByIdx((uint32_t)idx, c)) return;
  char nm[32]; snprintf(nm, sizeof nm, "%s", c.name);
  raClose();
  openContactActionSheet((uint32_t)idx, c.type == ADV_TYPE_REPEATER, nm, false);
}
void raNbMoreCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  // The button lives in the list the reply rebuilds: no rebuild from its own click.
  lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
  raEnqueue(RA_OP_NEIGH, 0xFF, "");
}
void raNbOrderCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  const int i = setChoiceIndex(e);
  const uint8_t ord = i == 1 ? RAP::NB_STRONGEST : RAP::NB_NEWEST;
  if (ord == s_ra->nb_order && s_ra->nb_loaded) return;
  s_ra->nb_order = ord;
  s_ra->nb_n = 0; s_ra->nb_total = 0; s_ra->nb_loaded = false;
  raBuildView(RA_V_NEIGHBOURS);
}
void raNbReloadCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  s_ra->nb_n = 0; s_ra->nb_total = 0; s_ra->nb_loaded = false;
  raBuildView(RA_V_NEIGHBOURS);
}

void raAclTapCb(lv_event_t* e);   // fwd

void raRenderList() {
  if (!s_ra || !s_ra_list || !lv_obj_is_valid(s_ra_list)) return;
  lv_indev_reset(nullptr, nullptr);   // rebuilding a list a flick may be throwing (lvgl-sync-del-scroll-uaf)
  lv_obj_clean(s_ra_list);
  s_ra_more_btn = nullptr;
  char nm[40], meta[64], ago[16];
  if (s_ra->view == RA_V_NEIGHBOURS) {
    const bool waiting = s_ra->busy && s_ra->cur.kind == RA_OP_NEIGH;
    bool queued = false;
    for (int i = 0; i < s_ra->q_len; ++i) if (s_ra->q[(s_ra->q_head + i) % RA_Q].kind == RA_OP_NEIGH) queued = true;
    if (s_ra->nb_n == 0) {
      raLabel(s_ra_list, (waiting || queued) ? TR("Reading...") : s_ra->nb_loaded ? TR("No neighbours heard yet.") : TR("No reply"),
              &g_font_14, COLOR_SUB);
    }
    for (int i = 0; i < s_ra->nb_n; ++i) {
      const RAP::Neighbour& n = s_ra->nb[i];
      raNameForPrefix(n.prefix, 6, nm, sizeof nm);
      RAP::fmtDuration(n.heard_ago, ago, sizeof ago);
      snprintf(meta, sizeof meta, "%.1f dB \xC2\xB7 %s", (double)n.snr_q4 / 4.0, ago);
      setActionRowRaw(s_ra_list, UI_ICON_ANTENNA, nm, meta, raNbTapCb, (void*)(intptr_t)i, SET_NORMAL, false);
    }
    if (s_ra->nb_loaded && s_ra->nb_n < s_ra->nb_total && s_ra->nb_n < RA_NB_MAX) {
      char lbl[48]; snprintf(lbl, sizeof lbl, TR("Load more (%d of %u)"), s_ra->nb_n, (unsigned)s_ra->nb_total);
      s_ra_more_btn = setButton(s_ra_list, UI_ICON_PLUS, lbl, raNbMoreCb);
    }
  } else if (s_ra->view == RA_V_CLIENTS) {
    const bool waiting = s_ra->busy && s_ra->cur.kind == RA_OP_ACL;
    if (s_ra->acl_n == 0)
      raLabel(s_ra_list, waiting || !s_ra->acl_loaded ? TR("Reading...") : TR("Nobody on the list."), &g_font_14, COLOR_SUB);
    for (int i = 0; i < s_ra->acl_n; ++i) {
      const RAP::AclEntry& a = s_ra->acl[i];
      raNameForPrefix(a.prefix, 6, nm, sizeof nm);
      const bool me = memcmp(a.prefix, the_mesh.self_id.pub_key, 6) == 0;
      if (me) { const size_t L = strlen(nm); snprintf(nm + L, sizeof nm - L, " (%s)", TR("you")); }
      setActionRowRaw(s_ra_list, me ? UI_ICON_STAR : UI_ICON_USER, nm, TR(raRoleName(RAP::aclRole(a.perms))),
                      raAclTapCb, (void*)(intptr_t)i);
    }
  }
}

void raBuildNeighbours() {
  lv_obj_t* card = raCard("");
  const char* opts[2] = { TR("Newest"), TR("Strongest") };
  setChoiceRow(card, "Order", nullptr, opts, 2, s_ra->nb_order == RAP::NB_STRONGEST ? 1 : 0, raNbOrderCb);
  setNote(card, "Repeaters this node hears directly, with the signal it hears them at.");
  lv_obj_t* lc = raCard(TR("Neighbours"));
  s_ra_list = lv_obj_create(lc);
  lv_obj_remove_style_all(s_ra_list);
  lv_obj_set_size(s_ra_list, s_settings_content_w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_ra_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ra_list, SC(6), LV_PART_MAIN);
  lv_obj_clear_flag(s_ra_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  setButton(lc, UI_ICON_REFRESH_CW, "Refresh", raNbReloadCb);
  if (!s_ra->nb_loaded && s_ra->nb_n == 0) raEnqueue(RA_OP_NEIGH, 0xFF, "");
  raRenderList();
}

// Access list: a role per client; removing needs only the prefix, any other role the
// full key, which we have when the client is one of our contacts.
void raPermApply() {
  if (!s_ra || s_ra->perm_idx < 0 || s_ra->perm_idx >= s_ra->acl_n) return;
  const RAP::AclEntry& a = s_ra->acl[s_ra->perm_idx];
  char hex[65];
  if (s_ra->perm_role == RAP::ROLE_GUEST) mesh::Utils::toHex(hex, (uint8_t*)a.prefix, 6);
  else {
    ContactInfo* c = the_mesh.lookupContactByPubKey((uint8_t*)a.prefix, 6);
    if (!c) return;
    mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
  }
  char cmd[100];
  snprintf(cmd, sizeof cmd, "setperm %s %u", hex, (unsigned)s_ra->perm_role);
  raEnqueue(RA_OP_PERM, 0xFF, cmd);
}
void raPermPickCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  const int role = (int)(intptr_t)lv_event_get_user_data(e);
  raCloseEditSheet();
  s_ra->perm_role = (uint8_t)role;
  if (role == RAP::ROLE_GUEST) showConfirm(TR("Remove this client's access?"), TR("Remove"), raPermApply);
  else raPermApply();
}
void raAclTapCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_ra->acl_n) return;
  s_ra->perm_idx = i;
  char nm[40];
  raNameForPrefix(s_ra->acl[i].prefix, 6, nm, sizeof nm);
  lv_obj_t* card = raSheetBegin(nm);
  const bool known = the_mesh.lookupContactByPubKey(s_ra->acl[i].prefix, 6) != nullptr;
  if (!known) setNote(card, "Not in your contacts, so only removing is possible here.", SET_TONE_INFO);
  static const uint8_t k_role[3] = { RAP::ROLE_ADMIN, RAP::ROLE_READ_WRITE, RAP::ROLE_READ_ONLY };
  for (int k = 0; k < 3; ++k) {
    lv_obj_t* b = setActionRowRaw(card, nullptr, TR(raRoleName(k_role[k])), RAP::aclRole(s_ra->acl[i].perms) == k_role[k] ? TR("Current") : "",
                                  raPermPickCb, (void*)(intptr_t)k_role[k], SET_NORMAL, false);
    if (!known) lv_obj_add_state(b, LV_STATE_DISABLED);
  }
  setActionRow(card, UI_ICON_X, "Remove access", "", raPermPickCb, (void*)(intptr_t)RAP::ROLE_GUEST, SET_DANGER, false);
  raSheetButtons(card, TR("Close"), raEditCancelCb, SET_NORMAL);
}
void raAclReloadCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  s_ra->acl_loaded = false; s_ra->acl_n = 0;
  raEnqueue(RA_OP_ACL, 0xFF, "");
  raRenderList();
}
void raBuildClients() {
  lv_obj_t* lc = raCard(TR("Clients"));
  if (s_ra->is_room) setNote(lc, "A room server lists only its admins here.");
  s_ra_list = lv_obj_create(lc);
  lv_obj_remove_style_all(s_ra_list);
  lv_obj_set_size(s_ra_list, s_settings_content_w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_ra_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ra_list, SC(6), LV_PART_MAIN);
  lv_obj_clear_flag(s_ra_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  setButton(lc, UI_ICON_REFRESH_CW, "Refresh", raAclReloadCb);
  if (!s_ra->acl_loaded) raEnqueue(RA_OP_ACL, 0xFF, "");
  raRenderList();
}

// Settings groups
void raUseMyPosApply() {
  if (!s_ra || !g_lv.task) return;
  char v[24];
  snprintf(v, sizeof v, "%.6f", g_lv.task->getNodeLat()); raSendSet(RA_F_LAT, v);
  snprintf(v, sizeof v, "%.6f", g_lv.task->getNodeLon()); raSendSet(RA_F_LON, v);
}
void raUseMyPosCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !g_lv.task) return;
  if (g_lv.task->getNodeLat() == 0.0 && g_lv.task->getNodeLon() == 0.0) { g_lv.task->showAlert(TR("This device has no position"), 1600); return; }
  showConfirm(TR("Set the node's position to this device's position?"), TR("Set"), raUseMyPosApply);
}
void raBuildIdentity() {
  lv_obj_t* c = raCard(TR("Identity"));
  raFieldRow(c, RA_F_NAME);
  raFieldRow(c, RA_F_OWNER);
  lv_obj_t* l = raCard(TR("Location"));
  raFieldRow(l, RA_F_LAT);
  raFieldRow(l, RA_F_LON);
  setButton(l, UI_ICON_CROSSHAIR, "Use my position", raUseMyPosCb);
  setNote(l, "The position goes out in the node's adverts.");
}

// Radio: the current values, then an editor that only goes out through a confirm
// showing both sets, with "Try" (tempradio: reverts by itself) before "Save".
void raRadioFill(const RAP::Radio& r) {
  if (!s_ra_radio_ta[0]) return;
  char b[24];
  snprintf(b, sizeof b, "%.3f", (double)r.freq);
  lv_textarea_set_text(s_ra_radio_ta[0], b);
  for (int i = 0; i < (int)(sizeof RAP::kBandwidths / sizeof RAP::kBandwidths[0]); ++i)
    if (r.bw > RAP::kBandwidths[i] - 0.05f && r.bw < RAP::kBandwidths[i] + 0.05f) lv_dropdown_set_selected(s_ra_radio_ta[1], (uint16_t)i);
  if (r.sf >= 5 && r.sf <= 12) lv_dropdown_set_selected(s_ra_radio_ta[2], (uint16_t)(r.sf - 5));
  if (r.cr >= 5 && r.cr <= 8) lv_dropdown_set_selected(s_ra_radio_ta[3], (uint16_t)(r.cr - 5));
}
bool raRadioRead(RAP::Radio& r) {
  if (!s_ra_radio_ta[0]) return false;
  kbMirrorSyncToReal();
  const char* ft = lv_textarea_get_text(s_ra_radio_ta[0]);
  char* end = nullptr;
  r.freq = strtof(ft, &end);
  r.bw = RAP::kBandwidths[lv_dropdown_get_selected(s_ra_radio_ta[1])];
  r.sf = 5 + (int)lv_dropdown_get_selected(s_ra_radio_ta[2]);
  r.cr = 5 + (int)lv_dropdown_get_selected(s_ra_radio_ta[3]);
  if (!ft[0] || !end || *end || r.freq < 150.0f || r.freq > 2500.0f) {
    if (g_lv.task) g_lv.task->showAlert(TR("Frequency: 150 to 2500 MHz"), 1800);
    return false;
  }
  return true;
}
void raRadioFmt(const RAP::Radio& r, char* out, size_t cap) {
  snprintf(out, cap, "%.3f MHz, %g kHz, SF%d, CR%d", (double)r.freq, (double)r.bw, r.sf, r.cr);
}
void raRadioSendApply() {
  if (!s_ra) return;
  const RAP::Radio& r = s_ra->radio_new;
  char cmd[96];
  if (s_ra->radio_try_mins) {
    snprintf(cmd, sizeof cmd, "tempradio %.3f,%g,%d,%d,%u", (double)r.freq, (double)r.bw, r.sf, r.cr, (unsigned)s_ra->radio_try_mins);
    raEnqueue(RA_OP_ACT, 0xFF, cmd);
  } else {
    snprintf(cmd, sizeof cmd, "set radio %.3f,%g,%d,%d", (double)r.freq, (double)r.bw, r.sf, r.cr);
    raEnqueue(RA_OP_SET, RA_F_RADIO, cmd);
  }
}
void raRadioAsk(uint8_t try_mins) {
  if (!s_ra) return;
  RAP::Radio r;
  if (!raRadioRead(r)) return;
  s_ra->radio_new = r;
  s_ra->radio_try_mins = try_mins;
  char was[64] = "?", now[64];
  RAP::Radio cur;
  if (s_ra->fst[RA_F_RADIO] == RA_FS_OK && RAP::parseRadio(s_ra->val[RA_F_RADIO], cur)) raRadioFmt(cur, was, sizeof was);
  raRadioFmt(r, now, sizeof now);
  static char msg[320];
  if (try_mins)
    snprintf(msg, sizeof msg, TR("Try new radio settings for %u minutes?\nNow: %s\nTry: %s\nThe node goes back by itself. Until then you reach it only on the new settings."),
             (unsigned)try_mins, was, now);
  else
    snprintf(msg, sizeof msg, TR("Save new radio settings?\nNow: %s\nNew: %s\nThey apply after a reboot. Wrong values can cut this node off for good."),
             was, now);
  showConfirm(msg, try_mins ? TR("Try") : TR("Save"), raRadioSendApply);
}
void raRadioTryCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raRadioAsk(10); }
void raRadioSaveCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raRadioAsk(0); }
void raRadioFillMine() {
  NodePrefs* np = the_mesh.getNodePrefs();
  if (!np) return;
  RAP::Radio r = { np->freq, np->bw, (int)np->sf, (int)np->cr };
  raRadioFill(r);
}
void raRadioMineCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raRadioFillMine(); }
void raRadioReloadCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  raEnqueueGet(RA_F_RADIO); raEnqueueGet(RA_F_TX);
}
void raBuildRadio() {
  lv_obj_t* c = raCard(TR("Current"));
  raFieldRow(c, RA_F_RADIO);
  raFieldRow(c, RA_F_TX);
  setButton(c, UI_ICON_REFRESH_CW, "Read again", raRadioReloadCb);
  lv_obj_t* ed = raCard(TR("Change radio settings"));
  setNote(ed, "Wrong radio settings can cut this node off: you then reach it only on the new settings, or not at all. Try them first; the node goes back by itself.",
          SET_TONE_WARN);
  s_ra_radio_ta[0] = setField(ed, "Frequency (MHz)", "", 10);
  lv_textarea_set_accepted_chars(s_ra_radio_ta[0], "0123456789.");
  {
    static char bw_opts[96];
    if (!bw_opts[0]) {
      size_t o = 0;
      for (float b : RAP::kBandwidths) o += snprintf(bw_opts + o, sizeof bw_opts - o, "%s%g kHz", o ? "\n" : "", (double)b);
    }
    s_ra_radio_ta[1] = setDropdownRow(ed, "Bandwidth", nullptr, bw_opts, 8, nullptr);
    s_ra_radio_ta[2] = setDropdownRow(ed, "Spreading factor", nullptr, "SF5\nSF6\nSF7\nSF8\nSF9\nSF10\nSF11\nSF12", 6, nullptr);
    s_ra_radio_ta[3] = setDropdownRow(ed, "Coding rate", nullptr, "4/5\n4/6\n4/7\n4/8", 0, nullptr);
  }
  RAP::Radio cur;
  if (s_ra->fst[RA_F_RADIO] == RA_FS_OK && RAP::parseRadio(s_ra->val[RA_F_RADIO], cur)) raRadioFill(cur);
  else raRadioFillMine();
  setButton(ed, UI_ICON_COPY, "Use this device's settings", raRadioMineCb);
  setButton(ed, UI_ICON_TIMER, "Try for 10 minutes", raRadioTryCb);
  setButton(ed, UI_ICON_CHECK, "Save (after a reboot)", raRadioSaveCb, nullptr, SET_DANGER);
}

void raBuildFieldGroup(uint8_t v, const char* card_title) {
  lv_obj_t* c = raCard(TR(card_title));
  for (int f = 0; f < RA_F_COUNT; ++f)
    if (k_ra_fields[f].view == v && k_ra_fields[f].kind != RA_K_INFO) raFieldRow(c, f);
}
void raAdvertNowCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  raEnqueue(RA_OP_ACT, 0xFF, (const char*)lv_event_get_user_data(e));
}
void raBuildAdverts() {
  raBuildFieldGroup(RA_V_ADVERTS, "Advert intervals");
  lv_obj_t* c = raCard(TR("Send now"));
  setButton(c, UI_ICON_RADIO_TOWER, "Send flood advert", raAdvertNowCb, (void*)"advert");
  setButton(c, UI_ICON_RADIO, "Send zero-hop advert", raAdvertNowCb, (void*)"advert.zerohop");
}

void raAdminPwSaveApply() {
  if (!s_ra) return;
  char cmd[40];
  snprintf(cmd, sizeof cmd, "password %s", s_ra->admin_pw_new);
  raEnqueue(RA_OP_ACT, 0xFF, cmd);
}
void raAdminPwSaveCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra || !s_ra_edit_ta || !s_ra_pw_ta2) return;
  kbMirrorSyncToReal();
  const char* a = lv_textarea_get_text(s_ra_edit_ta);
  const char* b = lv_textarea_get_text(s_ra_pw_ta2);
  if (!a[0]) { if (g_lv.task) g_lv.task->showAlert(TR("Enter a password"), 1400); return; }
  if (strcmp(a, b) != 0) { if (g_lv.task) g_lv.task->showAlert(TR("The passwords do not match"), 1600); return; }
  if (strpbrk(a, " ")) { if (g_lv.task) g_lv.task->showAlert(TR("No spaces"), 1400); return; }
  snprintf(s_ra->admin_pw_new, sizeof s_ra->admin_pw_new, "%s", a);
  raCloseEditSheet();
  showConfirm(TR("Change the admin password? If you lose it you can no longer manage this node over the mesh."),
              TR("Change"), raAdminPwSaveApply);
}
void raAdminPwCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  lv_obj_t* card = raSheetBegin(TR("Admin password"));
  raLabel(card, TR("Up to 15 characters"), &g_font_12, COLOR_SUB);
  s_ra_edit_ta = setField(card, "New password", "", 15);
  lv_textarea_set_password_mode(s_ra_edit_ta, true);
  s_ra_pw_ta2 = setField(card, "Again", "", 15);
  lv_textarea_set_password_mode(s_ra_pw_ta2, true);
  if (g_lv.keyboard) kbMirrorBind(s_ra_edit_ta);
  raSheetButtons(card, TR("Save"), raAdminPwSaveCb);
}
void raBuildAccess() {
  lv_obj_t* c = raCard(TR("Passwords"));
  raFieldRow(c, RA_F_GUESTPW);
  setActionRow(c, UI_ICON_LOCK, "Admin password", TR("Change"), raAdminPwCb);
  if (s_ra->is_room) { lv_obj_t* r = raCard(TR("Access")); raFieldRow(r, RA_F_ALLOWRO); }
  lv_obj_t* n = raCard("");
  setActionRow(n, UI_ICON_USERS, "Clients", "", raGoCb, (void*)(intptr_t)RA_V_CLIENTS);
}

void raRegionsReloadCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raEnqueueGet(RA_F_REGIONS); }
void raBuildRegions() {
  lv_obj_t* c = raCard(TR("Regions"));
  setNote(c, "The node's region tree: ^ marks its home region, F that it repeats floods for that region. Edit regions in the Console.");
  lv_obj_t* l = raLabel(c, "", &g_font_14, COLOR_TEXT);
  s_ra_row[RA_F_REGIONS] = l;
  raRenderField(RA_F_REGIONS);
  if (s_ra->fst[RA_F_REGIONS] != RA_FS_OK) raEnqueueGet(RA_F_REGIONS);
  setButton(c, UI_ICON_REFRESH_CW, "Read again", raRegionsReloadCb);
}

// Actions: each through raEnqueue(RA_OP_ACT), the reply on the page. The dangerous
// ones (reboot, OTA, clear stats) go through a confirm first.
void raActConfirmed() {
  if (!s_ra) return;
  raEnqueue(s_ra->pend_kind, 0xFF, s_ra->pend_cmd);
  s_ra->pend_cmd[0] = 0;
}
void raActCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_ra) return;
  const char* cmd = (const char*)lv_event_get_user_data(e);
  if (!cmd) return;
  snprintf(s_ra->pend_cmd, sizeof s_ra->pend_cmd, "%s", cmd);
  s_ra->pend_kind = RA_OP_ACT;
  if (!strcmp(cmd, "reboot")) {
    s_ra->pend_kind = RA_OP_FIRE;
    showConfirm(TR("Reboot the node? It is off the air for about 10 s and forgets guest logins."), TR("Reboot"), raActConfirmed);
  } else if (!strcmp(cmd, "start ota")) {
    showConfirm(TR("Start a firmware update on the node? It opens its own Wi-Fi access point until it is updated or rebooted, and stops repeating meanwhile."),
                TR("Start"), raActConfirmed);
  } else if (!strcmp(cmd, "clear stats")) {
    showConfirm(TR("Reset the node's packet counters?"), TR("Reset"), raActConfirmed);
  } else {
    raActConfirmed();
  }
}
void raClockReadCb(lv_event_t* e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) raEnqueueGet(RA_F_CLOCK); }
void raBuildActions() {
  lv_obj_t* ck = raCard(TR("Clock"));
  lv_obj_t* l = raLabel(ck, "", &g_font_14, COLOR_TEXT);
  s_ra_row[RA_F_CLOCK] = l;
  raRenderField(RA_F_CLOCK);
  if (s_ra->fst[RA_F_CLOCK] != RA_FS_OK) raEnqueueGet(RA_F_CLOCK);
  {
    char mine[64] = "-";
    if (wallClockIsCurrent()) {
      time_t tt = (time_t)the_mesh.getRTCClock()->getCurrentTime();
      struct tm tmv;
      if (gmtime_r(&tt, &tmv)) strftime(mine, sizeof mine, "%H:%M - %d/%m/%Y UTC", &tmv);
    }
    char b[96]; snprintf(b, sizeof b, "%s: %s", TR("This device"), mine);
    raLabel(ck, b, &g_font_12, COLOR_SUB);
  }
  setButton(ck, UI_ICON_REFRESH_CW, "Read again", raClockReadCb);
  setButton(ck, UI_ICON_TIMER, "Sync clock to this device", raActCb, (void*)"clock sync");
  setNote(ck, "A node clock only moves forward: syncing cannot set it back.");

  lv_obj_t* m = raCard(TR("Mesh"));
  setActionRow(m, UI_ICON_RADIO_TOWER, "Send flood advert", "", raActCb, (void*)"advert", SET_NORMAL, false);
  setActionRow(m, UI_ICON_RADIO, "Send zero-hop advert", "", raActCb, (void*)"advert.zerohop", SET_NORMAL, false);
  if (!s_ra->is_room) setActionRow(m, UI_ICON_RADAR, "Discover neighbours", "", raActCb, (void*)"discover.neighbors", SET_NORMAL, false);
  setActionRow(m, UI_ICON_CHART_NO_AXES_COLUMN, "Reset counters", "", raActCb, (void*)"clear stats", SET_NORMAL, false);

  lv_obj_t* d = raCard(TR("Device"));
  setActionRow(d, UI_ICON_REFRESH_CW, "Reboot", "", raActCb, (void*)"reboot", SET_DANGER, false);
  setActionRow(d, UI_ICON_ARROW_UP, "Start firmware update", "", raActCb, (void*)"start ota", SET_DANGER, false);
  setNote(d, "Firmware update: the node opens a Wi-Fi access point; join it and upload the new firmware in a browser, as the node's own instructions say.");

  lv_obj_t* r = raCard(TR("Last reply"));
  s_ra_act_lbl = raLabel(r, s_ra->act_result[0] ? s_ra->act_result : "-", &g_font_12, COLOR_SUB);
}

// ---- view switching / lifecycle ----------------------------------------------------
void raBuildView(uint8_t v) {
  if (!s_ra || !s_ra_root) return;
  raCloseEditSheet();
  if (v != RA_V_LOGIN && s_ra->role < 0) v = RA_V_LOGIN;
  if (v > RA_V_NEIGHBOURS && v != RA_V_LOGIN && !raIsAdmin()) v = RA_V_OVERVIEW;   // admin-only pages
  hideKb();
  s_ra->view = v;
  raNewPage();
  raSetTitle(v);
  switch (v) {
    case RA_V_LOGIN:      raBuildLogin(); break;
    case RA_V_OVERVIEW:   raBuildOverview(); break;
    case RA_V_NEIGHBOURS: raBuildNeighbours(); break;
    case RA_V_CLIENTS:    raBuildClients(); break;
    case RA_V_IDENTITY:   raBuildIdentity(); break;
    case RA_V_RADIO:      raBuildRadio(); break;
    case RA_V_FORWARD:    raBuildFieldGroup(RA_V_FORWARD, "Repeating"); break;
    case RA_V_ADVERTS:    raBuildAdverts(); break;
    case RA_V_ACCESS:     raBuildAccess(); break;
    case RA_V_REGIONS:    raBuildRegions(); break;
    case RA_V_ACTIONS:    raBuildActions(); break;
    default: break;
  }
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
  lv_obj_scroll_to_y(s_ra_page, 0, LV_ANIM_OFF);
}

// The cheap per-change refresh of the open view (labels only, no rebuild).
void raRefreshView() {
  if (!s_ra || !s_ra_page) return;
  if (s_ra_act_lbl && lv_obj_is_valid(s_ra_act_lbl)) setLabelIfChanged(s_ra_act_lbl, s_ra->act_result[0] ? s_ra->act_result : "-");
  if (s_ra->view == RA_V_OVERVIEW) raRenderStatus();
}

void raAfterLogin() {
  s_ra->has_status = false; s_ra->has_owner = false;
  for (int f = 0; f < RA_F_COUNT; ++f) s_ra->fst[f] = RA_FS_UNKNOWN;
  s_ra->nb_n = 0; s_ra->nb_total = 0; s_ra->nb_loaded = false;
  s_ra->acl_n = 0; s_ra->acl_loaded = false;
  raBuildView(RA_V_OVERVIEW);
}

void raTimerCb(lv_timer_t*) {
  if (!s_ra || !s_ra->open) return;
  if (s_ra->logging_in && (int32_t)(millis() - s_ra->login_deadline) >= 0) {
    s_ra->logging_in = false;
    memset(s_ra->login_pw, 0, sizeof s_ra->login_pw);
    if (s_ra_login_note && lv_obj_is_valid(s_ra_login_note))
      lv_label_set_text(s_ra_login_note, TR("No reply. Check the password, that the node is in range, and this device's clock."));
  }
  raPump();
  if (s_ra_updated_lbl && lv_obj_is_valid(s_ra_updated_lbl)) {
    char b[48] = "";
    if (s_ra->has_status) {
      char ago[16]; RAP::fmtDuration((millis() - s_ra->status_ms) / 1000u, ago, sizeof ago);
      snprintf(b, sizeof b, TR("Updated %s ago"), ago);
    }
    setLabelIfChanged(s_ra_updated_lbl, b);
  }
}

void raClose() {
  if (!s_ra_root) return;
  raCloseEditSheet();
  hideKb();
  if (s_ra) {
    if (s_ra->busy) raFinishCur();   // frees the UI request slot if ours
    raQueueClear();
    s_ra->open = false;
    s_ra->logging_in = false;
    memset(s_ra->login_pw, 0, sizeof s_ra->login_pw);
    memset(s_ra->admin_pw_new, 0, sizeof s_ra->admin_pw_new);
  }
  if (s_ra_timer) { lv_timer_del(s_ra_timer); s_ra_timer = nullptr; }
  raClearViewPtrs();
  s_ra_page = nullptr;
  popupClose(&s_ra_root);
  appPageEnd(raBack);
}

// Back: a sub-page returns to the overview, the overview (or login) closes the app.
void raBack() {
  if (!s_ra || !s_ra_root) return;
  if (s_ra_edit_root) { raCloseEditSheet(); return; }
  if (raOverlayAboveClose()) return;
  if (s_ra->view == RA_V_LOGIN || s_ra->view == RA_V_OVERVIEW) { raClose(); return; }
  raBuildView(RA_V_OVERVIEW);
}

}  // namespace

// Open the app for a repeater or room server contact.
static void openRepeaterAdmin(const ContactInfo& c) {
  if (!s_ra) s_ra = (RaState*)psAlloc(sizeof(RaState));
  if (!s_ra) { if (g_lv.task) g_lv.task->showAlert(TR("Out of memory"), 1400); return; }
  raClose();
  closeAdminConsole();
  closeAdminPwPrompt();
  const bool same = memcmp(s_ra->pub, c.id.pub_key, 32) == 0;
  if (!same) {   // a different node: nothing of the last one carries over
    memset(s_ra, 0, sizeof *s_ra);
    memcpy(s_ra->pub, c.id.pub_key, 32);
    s_ra->role = -1;
  }
  // The same node again keeps its login and the values read so far, for a while: a
  // node forgets guests when it reboots, so an old session is logged in afresh.
  if (s_ra->role >= 0 && (uint32_t)(millis() - s_ra->login_ms) > 15u * 60u * 1000u) s_ra->role = -1;
  s_ra->open = true;
  s_ra->is_room = (c.type == ADV_TYPE_ROOM);
  copyUtf8ReplacingMissingGlyphs(&g_font_16, s_ra->name, sizeof s_ra->name, contactShownName(c.id.pub_key, c.name));
  s_ra->q_len = 0; s_ra->busy = false; s_ra->logging_in = false;
  s_ra_root = appPageCreateRoot(COLOR_BG);
  appPageBegin(s_ra_title, raBack);
  s_ra_timer = lv_timer_create(raTimerCb, 250, nullptr);
  raBuildView(s_ra->role >= 0 ? RA_V_OVERVIEW : RA_V_LOGIN);
  if (s_ra->role >= 0) {   // refresh what changes on its own
    s_ra->has_status = false;
    raEnqueue(RA_OP_STATUS, 0xFF, "");
  }
}
