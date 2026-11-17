// SPDX-License-Identifier: GPL-3.0-or-later
// A7682E modem service for the T-Deck Max. See CellularService.h for the
// threading model; everything below the "task side" marker runs on the cell
// task only and owns the UART.
#include "CellularService.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

#include "TDeckMaxBoard.h"

extern TDeckMaxBoard board;

#define CELL_LOG(fmt, ...) Serial.printf("[CELL] " fmt "\n", ##__VA_ARGS__)

namespace {

// ---------------------------------------------------------------------------
// Shared state (any thread)
// ---------------------------------------------------------------------------
SemaphoreHandle_t s_mx = nullptr;
QueueHandle_t     s_q  = nullptr;
TaskHandle_t      s_task = nullptr;
CellStatus        s_st = {};

constexpr int kEvRing = 12;
CellEvent s_ev[kEvRing];
int s_ev_head = 0, s_ev_tail = 0;   // head = next write, tail = next read

struct Settings {
  bool enabled;
  char apn[48], user[32], pass[32], pin[12];
} s_cfg = {};

enum CmdType : uint8_t {
  CMD_NONE = 0, CMD_SYNC_ENABLE, CMD_DIAL, CMD_ANSWER, CMD_HANGUP, CMD_DTMF,
  CMD_VOLUME, CMD_MUTE, CMD_SMS, CMD_DATA, CMD_HTTP, CMD_APN_CHANGED, CMD_PIN_CHANGED,
};
struct Cmd {
  uint8_t type;
  int16_t arg;
  char    num[32];
  char    text[284];   // UTF-8, <= 70 UTF-16 units checked before queueing
};

// HTTP: one request at a time, parameters parked here (too big for the queue).
struct Http {
  volatile bool busy;
  volatile bool done;
  char   method[8];
  char   url[400];
  char   ctype[64];
  char*  body;        // PSRAM copy
  size_t body_len;
  size_t max_bytes;
  int    status;      // HTTP status, or <0
  char*  resp;        // PSRAM
  size_t resp_len;
  bool   sync;        // started by cellHttpGetSync: no event, the caller waits on `done`
  bool   abandoned;   // that caller gave up: free the response on arrival
} s_http = {};

struct Lock {
  Lock()  { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
  ~Lock() { if (s_mx) xSemaphoreGive(s_mx); }
};

void copyStr(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void pushEvent(CellEventType type, int code, const char* text = nullptr, const char* body = nullptr,
               const char* when = nullptr) {
  Lock l;
  CellEvent& e = s_ev[s_ev_head];
  e.type = type;
  e.code = (int16_t)code;
  copyStr(e.text, sizeof e.text, text);
  copyStr(e.body, sizeof e.body, body);
  copyStr(e.when, sizeof e.when, when);
  s_ev_head = (s_ev_head + 1) % kEvRing;
  if (s_ev_head == s_ev_tail) s_ev_tail = (s_ev_tail + 1) % kEvRing;   // drop oldest
}

void loadSettings() {
  Preferences p;
  if (!p.begin("cell", true)) { s_cfg = {}; return; }
  s_cfg.enabled = p.getBool("en", false);
  p.getString("apn",  s_cfg.apn,  sizeof s_cfg.apn);
  p.getString("user", s_cfg.user, sizeof s_cfg.user);
  p.getString("pass", s_cfg.pass, sizeof s_cfg.pass);
  p.getString("pin",  s_cfg.pin,  sizeof s_cfg.pin);
  p.end();
}

void saveSettings() {
  Preferences p;
  if (!p.begin("cell", false)) return;
  p.putBool("en", s_cfg.enabled);
  p.putString("apn",  s_cfg.apn);
  p.putString("user", s_cfg.user);
  p.putString("pass", s_cfg.pass);
  p.putString("pin",  s_cfg.pin);
  p.end();
}

bool queueCmd(const Cmd& c) {
  if (!s_q) return false;
  return xQueueSend(s_q, &c, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UCS2 hex (SMS in AT+CSCS="UCS2" mode, so æ/ø/å and emoji survive)
// ---------------------------------------------------------------------------
// Decode one code point; returns bytes consumed (0 at end).
int utf8Next(const char* s, uint32_t* cp) {
  const uint8_t c = (uint8_t)s[0];
  if (!c) return 0;
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0 && s[1]) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
  if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
    *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3;
  }
  if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4;
  }
  *cp = '?'; return 1;
}

int utf16Units(const char* s) {
  int n = 0; uint32_t cp; int k;
  while ((k = utf8Next(s, &cp)) > 0) { n += cp > 0xFFFF ? 2 : 1; s += k; }
  return n;
}

// Writes uppercase hex; returns false if out is too small.
bool utf8ToUcs2Hex(const char* s, char* out, size_t cap) {
  static const char* hx = "0123456789ABCDEF";
  size_t o = 0; uint32_t cp; int k;
  auto put16 = [&](uint16_t u) -> bool {
    if (o + 4 >= cap) return false;
    out[o++] = hx[(u >> 12) & 15]; out[o++] = hx[(u >> 8) & 15];
    out[o++] = hx[(u >> 4) & 15];  out[o++] = hx[u & 15];
    return true;
  };
  while ((k = utf8Next(s, &cp)) > 0) {
    s += k;
    if (cp > 0xFFFF) {
      cp -= 0x10000;
      if (!put16(0xD800 | (cp >> 10)) || !put16(0xDC00 | (cp & 0x3FF))) return false;
    } else if (!put16((uint16_t)cp)) return false;
  }
  out[o] = '\0';
  return true;
}

int hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool looksLikeUcs2Hex(const char* s) {
  size_t n = strlen(s);
  if (n < 4 || n % 4) return false;
  for (size_t i = 0; i < n; i++) if (hexNib(s[i]) < 0) return false;
  return true;
}

void ucs2HexToUtf8(const char* hex, char* out, size_t cap) {
  size_t o = 0;
  auto put = [&](uint32_t cp) {
    char b[4]; int n;
    if (cp < 0x80)         { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (o + n >= cap) return;
    memcpy(out + o, b, n); o += n;
  };
  const size_t n = strlen(hex);
  for (size_t i = 0; i + 3 < n; i += 4) {
    uint32_t u = (hexNib(hex[i]) << 12) | (hexNib(hex[i + 1]) << 8) | (hexNib(hex[i + 2]) << 4) | hexNib(hex[i + 3]);
    if (u >= 0xD800 && u <= 0xDBFF && i + 7 < n) {
      uint32_t lo = (hexNib(hex[i + 4]) << 12) | (hexNib(hex[i + 5]) << 8) | (hexNib(hex[i + 6]) << 4) | hexNib(hex[i + 7]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) { put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); i += 4; continue; }
    }
    if (u == 0) break;
    put(u);
  }
  out[o] = '\0';
}

bool validNumber(const char* n) {
  if (!n || !*n || strlen(n) > 30) return false;
  for (const char* p = n; *p; p++) {
    const char c = *p;
    if (!((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#')) return false;
  }
  return true;
}

// ===========================================================================
// Task side: everything below runs on the cell task only.
// ===========================================================================
HardwareSerial& at = Serial2;
bool s_uart_open = false;

constexpr size_t kLineCap = 300;   // one UCS2 SMS body is 280 hex chars
char   s_line[512];
size_t s_line_len = 0;

char     s_clip[32] = "";          // number from the last +CLIP
uint32_t s_next_poll_ms = 0;
uint32_t s_next_clcc_ms = 0;
uint32_t s_retry_ms = 0;
int      s_sms_pending[8];
int      s_sms_pending_n = 0;
volatile bool s_http_action_done = false;
int      s_http_action_status = 0;
int      s_http_action_len = 0;

void setStatusError(const char* e) {
  Lock l; copyStr(s_st.last_error, sizeof s_st.last_error, e);
}

void setCall(CellCall c, const char* number) {
  CellCall prev;
  {
    Lock l;
    prev = s_st.call;
    s_st.call = c;
    if (number && *number) copyStr(s_st.call_number, sizeof s_st.call_number, number);
    if (c == CELL_CALL_ACTIVE && prev != CELL_CALL_ACTIVE) s_st.call_started_ms = millis();
    if (c == CELL_CALL_IDLE) s_st.call_started_ms = 0;
  }
  if (c == prev) return;
  // Speaker: the modem owns it from dialing until hang-up. While INCOMING the
  // ES8311 keeps it, so the firmware's own chime can ring.
  const bool modem_audio = (c == CELL_CALL_DIALING || c == CELL_CALL_ALERTING ||
                            c == CELL_CALL_ACTIVE  || c == CELL_CALL_HELD);
  if (modem_audio != board.modemAudioActive()) board.setModemAudio(modem_audio);
  char num[32];
  { Lock l; copyStr(num, sizeof num, s_st.call_number); }
  CELL_LOG("call %s -> %s (%s)", cellCallName(prev), cellCallName(c), num);
  pushEvent(CELL_EV_CALL, c, num);
  if (c == CELL_CALL_IDLE) { Lock l; s_st.call_number[0] = '\0'; s_clip[0] = '\0'; }
  if (c != CELL_CALL_IDLE) s_next_clcc_ms = millis() + 1500;
}

bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

// Copy the n-th (0-based) double-quoted field of s into out.
bool quotedField(const char* s, int n, char* out, size_t cap) {
  for (int i = 0; ; i++) {
    const char* a = strchr(s, '"');
    if (!a) return false;
    const char* b = strchr(a + 1, '"');
    if (!b) return false;
    if (i == n) {
      size_t len = (size_t)(b - a - 1);
      if (len >= cap) len = cap - 1;
      memcpy(out, a + 1, len); out[len] = '\0';
      return true;
    }
    s = b + 1;
  }
}

void handleClcc(const char* line) {
  // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,"<number>",<type>]
  int id, dir, stat, mode;
  if (sscanf(line + 6, " %d,%d,%d,%d", &id, &dir, &stat, &mode) != 4) return;
  if (mode != 0) return;   // voice only
  char num[32] = "";
  quotedField(line, 0, num, sizeof num);
  CellCall c;
  switch (stat) {
    case 0: c = CELL_CALL_ACTIVE; break;
    case 1: c = CELL_CALL_HELD; break;
    case 2: c = CELL_CALL_DIALING; break;
    case 3: c = CELL_CALL_ALERTING; break;
    case 4: case 5: c = CELL_CALL_INCOMING; break;
    default: c = CELL_CALL_IDLE; break;   // 6 = disconnected
  }
  setCall(c, num);
}

bool isUrc(const char* l) {
  static const char* const k[] = {
    "RING", "+CLIP:", "+CLCC:", "NO CARRIER", "BUSY", "NO ANSWER", "VOICE CALL:",
    "MISSED_CALL", "+CMTI:", "+CREG:", "+CEREG:", "+CGREG:", "+HTTPACTION:", "SMS DONE",
    "PB DONE", "*ATREADY", "+CGEV:", "+CPIN:", "RDY", "+CTZV:", "*PSUTTZ", "DST:", "+CNTP:",
  };
  for (const char* p : k) if (startsWith(l, p)) return true;
  return false;
}

void handleUrc(const char* l) {
  if (startsWith(l, "RING")) {
    CellCall c; { Lock lk; c = s_st.call; }
    if (c == CELL_CALL_IDLE) setCall(CELL_CALL_INCOMING, s_clip);
    char num[32]; { Lock lk; copyStr(num, sizeof num, s_st.call_number); }
    pushEvent(CELL_EV_RING, 0, num);
  } else if (startsWith(l, "+CLIP:")) {
    quotedField(l, 0, s_clip, sizeof s_clip);
    CellCall c; { Lock lk; c = s_st.call; }
    if (c == CELL_CALL_INCOMING) { Lock lk; copyStr(s_st.call_number, sizeof s_st.call_number, s_clip); }
  } else if (startsWith(l, "+CLCC:")) {
    handleClcc(l);
  } else if (startsWith(l, "NO CARRIER") || startsWith(l, "BUSY") || startsWith(l, "NO ANSWER") ||
             startsWith(l, "VOICE CALL: END") || startsWith(l, "MISSED_CALL")) {
    if (startsWith(l, "BUSY")) setStatusError("busy");
    else if (startsWith(l, "NO ANSWER")) setStatusError("no answer");
    setCall(CELL_CALL_IDLE, nullptr);
  } else if (startsWith(l, "VOICE CALL: BEGIN")) {
    setCall(CELL_CALL_ACTIVE, nullptr);
  } else if (startsWith(l, "+CMTI:")) {
    const char* comma = strrchr(l, ',');
    if (comma && s_sms_pending_n < 8) s_sms_pending[s_sms_pending_n++] = atoi(comma + 1);
  } else if (startsWith(l, "+HTTPACTION:")) {
    int m = 0, st = 0, len = 0;
    if (sscanf(l + 12, " %d,%d,%d", &m, &st, &len) >= 2) {
      s_http_action_status = st; s_http_action_len = len; s_http_action_done = true;
    }
  } else if (startsWith(l, "+CREG:") || startsWith(l, "+CEREG:")) {
    s_next_poll_ms = millis();   // re-read registration on the next pass
  }
}

// Read one line (CR/LF terminated). The ">" SMS prompt has no terminator and
// is returned as a line of its own when prompt_ok. Returns false on timeout.
bool readLine(uint32_t timeout_ms, bool prompt_ok = false) {
  const uint32_t t0 = millis();
  for (;;) {
    while (at.available()) {
      const char c = (char)at.read();
      if (c == '\r') continue;
      if (c == ' ' && s_line_len == 0) continue;   // e.g. the space after the "> " SMS prompt
      if (c == '\n') {
        if (!s_line_len) continue;   // blank line
        s_line[s_line_len] = '\0'; s_line_len = 0;
        return true;
      }
      if (s_line_len < sizeof s_line - 1) s_line[s_line_len++] = c;
      if (prompt_ok && s_line_len == 1 && c == '>') {
        s_line[1] = '\0'; s_line_len = 0;
        return true;
      }
    }
    if ((uint32_t)(millis() - t0) >= timeout_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Drain whatever is buffered, dispatching URCs.
void pumpUrcs() {
  while (at.available() || s_line_len) {
    if (!readLine(20)) break;
    if (isUrc(s_line)) handleUrc(s_line);
    else CELL_LOG("<< %s", s_line);
  }
}

// Send a command and wait for OK/ERROR. Lines starting with `expect` (or, when
// expect is null, the first non-URC line) are copied into resp. Lines can also
// be collected into `lines` (up to max_lines) for multi-line answers.
// Returns 1 = OK, 0 = ERROR, -1 = timeout.
int atCmd(const char* cmd, uint32_t timeout_ms = 2000, const char* expect = nullptr,
          char* resp = nullptr, size_t resp_cap = 0,
          char (*lines)[kLineCap] = nullptr, int max_lines = 0, int* n_lines = nullptr) {
  pumpUrcs();
  at.print(cmd);
  at.print("\r");
  if (resp && resp_cap) resp[0] = '\0';
  if (n_lines) *n_lines = 0;
  bool got = false;
  const uint32_t t0 = millis();
  for (;;) {
    const uint32_t el = millis() - t0;
    if (el >= timeout_ms) { CELL_LOG("%s -> timeout", cmd); return -1; }
    if (!readLine(timeout_ms - el)) continue;
    const char* l = s_line;
    if (!strcmp(l, cmd)) continue;              // echo
    if (!strcmp(l, "OK")) return 1;
    if (!strcmp(l, "ERROR") || startsWith(l, "+CME ERROR") || startsWith(l, "+CMS ERROR")) {
      CELL_LOG("%s -> %s", cmd, l);
      setStatusError(l);
      return 0;
    }
    if (expect && startsWith(l, expect)) {
      if (resp && !got) { copyStr(resp, resp_cap, l); got = true; }
      if (lines && n_lines && *n_lines < max_lines) copyStr(lines[(*n_lines)++], kLineCap, l);
      continue;
    }
    if (isUrc(l)) { handleUrc(l); continue; }
    if (resp && !expect && !got) { copyStr(resp, resp_cap, l); got = true; }
    if (lines && n_lines && *n_lines < max_lines) copyStr(lines[(*n_lines)++], kLineCap, l);
  }
}

bool testAt(uint32_t ms) {
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    if (atCmd("AT", 600) == 1) return true;
  }
  return false;
}

void openUart(int rx, int tx) {
  if (s_uart_open) at.end();
  at.setRxBufferSize(4096);
  at.begin(115200, SERIAL_8N1, rx, tx);
  s_uart_open = true;
  s_line_len = 0;
}

void setPower(CellPower p) {
  { Lock l; s_st.power = p; }
  pushEvent(CELL_EV_STATUS, p);
}

void queryRegistration() {
  char r[64];
  int best = -1;
  // LTE first (A7682E is Cat-1), then 2G.
  const char* q[] = { "AT+CEREG?", "AT+CREG?" };
  const char* p[] = { "+CEREG:", "+CREG:" };
  for (int i = 0; i < 2; i++) {
    if (atCmd(q[i], 2000, p[i], r, sizeof r) != 1) continue;
    const char* c = strchr(r, ',');
    if (!c) continue;
    const int stat = atoi(c + 1);
    if (stat == 1 || stat == 5) { best = stat; break; }
    if (best < 0 || stat == 2 || stat == 3) best = stat;
  }
  CellReg reg = CELL_REG_NONE;
  if (best == 1) reg = CELL_REG_HOME;
  else if (best == 5) reg = CELL_REG_ROAMING;
  else if (best == 2) reg = CELL_REG_SEARCHING;
  else if (best == 3) reg = CELL_REG_DENIED;

  int csq = -1;
  if (atCmd("AT+CSQ", 2000, "+CSQ:", r, sizeof r) == 1) {
    csq = atoi(r + 5);
    if (csq == 99) csq = -1;
  }
  char op[24] = "";
  if (reg == CELL_REG_HOME || reg == CELL_REG_ROAMING) {
    if (atCmd("AT+COPS?", 3000, "+COPS:", r, sizeof r) == 1) quotedField(r, 0, op, sizeof op);
  }
  bool changed;
  {
    Lock l;
    changed = (s_st.reg != reg) || strcmp(s_st.op, op);
    s_st.reg = reg;
    s_st.csq = (int8_t)csq;
    s_st.rssi_dbm = csq < 0 ? 0 : (int8_t)(-113 + 2 * csq);
    copyStr(s_st.op, sizeof s_st.op, op);
  }
  if (changed) {
    CELL_LOG("reg=%s op=%s csq=%d", cellRegName(reg), op, csq);
    pushEvent(CELL_EV_STATUS, reg);
  }
}

void simReadySetup();   // below: the setup that needs a working SIM

void querySim() {
  char r[48];
  CellSim sim = CELL_SIM_UNKNOWN;
  const int rc = atCmd("AT+CPIN?", 5000, "+CPIN:", r, sizeof r);
  if (rc == 1) {
    if (strstr(r, "READY")) sim = CELL_SIM_READY;
    else if (strstr(r, "SIM PIN")) sim = CELL_SIM_PIN;
    else if (strstr(r, "SIM PUK")) sim = CELL_SIM_PUK;
  } else {
    // +CME ERROR: SIM not inserted (10) / SIM failure (13)
    char e[48]; { Lock l; copyStr(e, sizeof e, s_st.last_error); }
    if (strstr(e, "not inserted") || strstr(e, ": 10") || strstr(e, "failure")) sim = CELL_SIM_ABSENT;
  }
  if (sim == CELL_SIM_PIN && s_cfg.pin[0]) {
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CPIN=\"%s\"", s_cfg.pin);
    if (atCmd(cmd, 5000) == 1) {
      // The card needs a moment before it reports READY.
      vTaskDelay(pdMS_TO_TICKS(2000));
      if (atCmd("AT+CPIN?", 5000, "+CPIN:", r, sizeof r) == 1 && strstr(r, "READY")) sim = CELL_SIM_READY;
    } else {
      // Do not retry a wrong PIN: three attempts lock the card behind the PUK.
      CELL_LOG("SIM PIN rejected -- clearing saved PIN");
      s_cfg.pin[0] = '\0';
      saveSettings();
    }
  }
  bool changed;
  { Lock l; changed = s_st.sim != sim; s_st.sim = sim; }
  // Caller ID, SMS indications and the APN are refused (+CME ERROR) while the
  // card is missing or locked, so they are (re)applied every time it becomes
  // READY -- at power-on, after a PIN, or a card inserted with the modem running.
  if (changed && sim == CELL_SIM_READY) simReadySetup();
  if (changed) {   // polled every few seconds until READY: report changes only
    CELL_LOG("sim=%s", cellSimName(sim));
    pushEvent(CELL_EV_STATUS, sim);
  }
}

void applyApn() {
  if (!s_cfg.apn[0]) return;
  char cmd[96];
  snprintf(cmd, sizeof cmd, "AT+CGDCONT=1,\"IP\",\"%s\"", s_cfg.apn);
  atCmd(cmd, 3000);
  if (s_cfg.user[0]) {
    snprintf(cmd, sizeof cmd, "AT+CGAUTH=1,1,\"%s\",\"%s\"", s_cfg.pass, s_cfg.user);
    atCmd(cmd, 3000);
  }
}

void simReadySetup() {
  atCmd("AT+COPS=3,0");                   // AT+COPS? answers with the operator NAME, not "23802"
  atCmd("AT+CLIP=1");                     // caller ID on RING
  atCmd("AT+CLCC=1");                     // unsolicited call-state reports (polled as a fallback)
  atCmd("AT+CMGF=1");                     // SMS text mode
  atCmd("AT+CNMI=2,1,0,0,0");             // new SMS -> +CMTI index
  applyApn();
  CELL_LOG("sim ready: caller id, sms and apn set up");
}

bool powerOn() {
  setPower(CELL_POWER_BOOTING);
  pinMode(PIN_MODEM_DTR, OUTPUT);
  digitalWrite(PIN_MODEM_DTR, LOW);       // keep the modem awake
  pinMode(PIN_MODEM_RI, INPUT_PULLUP);
  board.setModemRail(true);
  openUart(PIN_MODEM_RX, PIN_MODEM_TX);
  vTaskDelay(pdMS_TO_TICKS(1500));        // SGM6609 rail settle (LilyGo uses 1.5 s)

  bool ok = testAt(1500);
  if (!ok) {
    CELL_LOG("no AT -- pulsing PWRKEY");
    board.modemPowerKeyPulse();
    ok = testAt(35000);
  }
  if (!ok) {
    // Cheap safety net: if a board revision crosses the UART lines, try them swapped.
    CELL_LOG("no AT on rx=%d tx=%d -- trying swapped", PIN_MODEM_RX, PIN_MODEM_TX);
    openUart(PIN_MODEM_TX, PIN_MODEM_RX);
    ok = testAt(4000);
    if (ok) CELL_LOG("modem answers with SWAPPED UART pins (rx=%d tx=%d)", PIN_MODEM_TX, PIN_MODEM_RX);
  }
  if (!ok) {
    CELL_LOG("modem did not answer (is a battery connected? the modem needs it)");
    setStatusError("modem not responding");
    board.setModemRail(false);
    at.end(); s_uart_open = false;
    setPower(CELL_POWER_ERROR);
    return false;
  }

  atCmd("ATE0");
  atCmd("AT+CMEE=2");
  char r[48];
  if (atCmd("AT+CGSN", 2000, nullptr, r, sizeof r) == 1) { Lock l; copyStr(s_st.imei, sizeof s_st.imei, r); }
  atCmd("AT+CSCS=\"IRA\"");
  atCmd("AT+CREG=1");
  atCmd("AT+CEREG=1");
  { Lock l; s_st.sim = CELL_SIM_UNKNOWN; }   // so a READY card counts as a change and gets its setup
  querySim();
  CELL_LOG("modem ready imei=%s", s_st.imei);
  setPower(CELL_POWER_READY);
  s_next_poll_ms = millis();
  return true;
}

void powerOff() {
  if (s_uart_open) {
    if (atCmd("AT+CPOF", 3000) != 1) board.modemPowerKeyPulse();
    vTaskDelay(pdMS_TO_TICKS(2500));
  }
  board.setModemRail(false);
  if (s_uart_open) { at.end(); s_uart_open = false; }
  {
    Lock l;
    s_st.sim = CELL_SIM_UNKNOWN; s_st.reg = CELL_REG_NONE; s_st.csq = -1; s_st.rssi_dbm = 0;
    s_st.data_up = false; s_st.op[0] = '\0'; s_st.ip[0] = '\0';
  }
  setCall(CELL_CALL_IDLE, nullptr);
  setPower(CELL_POWER_OFF);
  CELL_LOG("modem off");
}

bool dataUp() {
  {
    Lock l;
    if (s_st.data_up) return true;
  }
  applyApn();
  if (atCmd("AT+CGATT=1", 75000) != 1) return false;
  if (atCmd("AT+CGACT=1,1", 150000) != 1) {
    // Many networks auto-activate the default bearer; an ERROR here can just
    // mean "already active". Fall through to the address check.
  }
  char r[96];
  char ip[40] = "";
  if (atCmd("AT+CGPADDR=1", 3000, "+CGPADDR:", r, sizeof r) == 1) {
    const char* c = strchr(r, ',');
    if (c) {
      c++;
      if (*c == '"') c++;
      copyStr(ip, sizeof ip, c);
      char* q = strchr(ip, '"'); if (q) *q = '\0';
    }
  }
  const bool up = ip[0] && strcmp(ip, "0.0.0.0");
  { Lock l; s_st.data_up = up; copyStr(s_st.ip, sizeof s_st.ip, ip); }
  CELL_LOG("data %s ip=%s", up ? "up" : "FAILED", ip);
  pushEvent(CELL_EV_DATA, up ? 1 : 0, ip);
  return up;
}

void dataDown() {
  atCmd("AT+CGACT=0,1", 40000);
  { Lock l; s_st.data_up = false; s_st.ip[0] = '\0'; }
  pushEvent(CELL_EV_DATA, 0);
}

void sendSms(const char* number, const char* text) {
  char num_hex[132], cmd[160];
  static char body_hex[290];
  bool ok = false;
  if (utf8ToUcs2Hex(number, num_hex, sizeof num_hex) && utf8ToUcs2Hex(text, body_hex, sizeof body_hex)) {
    atCmd("AT+CSCS=\"UCS2\"");
    atCmd("AT+CSMP=17,167,0,8");          // DCS 8 = UCS2
    snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", num_hex);
    pumpUrcs();
    at.print(cmd); at.print("\r");
    bool prompt = false;
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 8000) {
      if (!readLine(8000 - (millis() - t0), true)) continue;
      if (!strcmp(s_line, ">")) { prompt = true; break; }
      if (startsWith(s_line, "ERROR") || startsWith(s_line, "+CMS ERROR")) { setStatusError(s_line); break; }
      if (isUrc(s_line)) handleUrc(s_line);
    }
    if (prompt) {
      at.print(body_hex);
      at.write((uint8_t)0x1A);
      const uint32_t t1 = millis();
      while ((uint32_t)(millis() - t1) < 60000) {
        if (!readLine(60000 - (millis() - t1))) continue;
        if (!strcmp(s_line, "OK")) { ok = true; break; }
        if (startsWith(s_line, "+CMGS:")) continue;
        if (startsWith(s_line, "ERROR") || startsWith(s_line, "+CMS ERROR")) { setStatusError(s_line); break; }
        if (isUrc(s_line)) handleUrc(s_line);
      }
    } else {
      at.write((uint8_t)0x1B);            // ESC aborts a half-open CMGS
    }
    atCmd("AT+CSCS=\"IRA\"");
    atCmd("AT+CSMP=17,167,0,0");
  }
  CELL_LOG("sms to %s %s", number, ok ? "sent" : "FAILED");
  pushEvent(CELL_EV_SMS_SENT, ok ? 1 : 0, number);
}

void readSms(int idx) {
  static char lines[4][kLineCap];
  int n = 0;
  char cmd[24];
  atCmd("AT+CSCS=\"UCS2\"");
  snprintf(cmd, sizeof cmd, "AT+CMGR=%d", idx);
  const int rc = atCmd(cmd, 5000, nullptr, nullptr, 0, lines, 4, &n);
  atCmd("AT+CSCS=\"IRA\"");
  if (rc != 1 || n < 1 || !startsWith(lines[0], "+CMGR:")) return;
  char sender_raw[132] = "", sender[40] = "";
  quotedField(lines[0], 1, sender_raw, sizeof sender_raw);
  if (looksLikeUcs2Hex(sender_raw)) ucs2HexToUtf8(sender_raw, sender, sizeof sender);
  else copyStr(sender, sizeof sender, sender_raw);
  char body[284] = "";
  if (n >= 2) {
    if (looksLikeUcs2Hex(lines[1])) ucs2HexToUtf8(lines[1], body, sizeof body);
    else copyStr(body, sizeof body, lines[1]);
  }
  char when[24] = "";
  quotedField(lines[0], 3, when, sizeof when);   // "REC UNREAD","<from>","<alpha>","<scts>"
  CELL_LOG("sms from %s at %s: %s", sender, when, body);
  pushEvent(CELL_EV_SMS, 0, sender, body, when);
  snprintf(cmd, sizeof cmd, "AT+CMGD=%d", idx);
  atCmd(cmd, 5000);   // keep the SIM from filling up
}

// Read exactly n raw bytes (after "+HTTPREAD: n").
size_t readRaw(char* dst, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  const uint32_t t0 = millis();
  while (got < n && (uint32_t)(millis() - t0) < timeout_ms) {
    while (at.available() && got < n) dst[got++] = (char)at.read();
    if (got < n) vTaskDelay(pdMS_TO_TICKS(2));
  }
  return got;
}

void runHttp() {
  int status = -1;
  char* resp = nullptr;
  size_t resp_len = 0;
  char cmd[460];

  if (!dataUp()) { status = -2; goto done; }
  atCmd("AT+HTTPTERM", 2000);              // clear a session left over from an earlier failure
  if (atCmd("AT+HTTPINIT", 5000) != 1) { status = -3; goto done; }
  snprintf(cmd, sizeof cmd, "AT+HTTPPARA=\"URL\",\"%s\"", s_http.url);
  if (atCmd(cmd, 5000) != 1) { status = -4; goto term; }
  if (s_http.ctype[0]) {
    snprintf(cmd, sizeof cmd, "AT+HTTPPARA=\"CONTENT\",\"%s\"", s_http.ctype);
    atCmd(cmd, 2000);
  }
  if (s_http.body && s_http.body_len) {
    snprintf(cmd, sizeof cmd, "AT+HTTPDATA=%u,10", (unsigned)s_http.body_len);
    pumpUrcs();
    at.print(cmd); at.print("\r");
    bool dl = false;
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 5000) {
      if (!readLine(5000 - (millis() - t0))) continue;
      if (startsWith(s_line, "DOWNLOAD")) { dl = true; break; }
      if (startsWith(s_line, "ERROR") || startsWith(s_line, "+CME")) break;
    }
    if (!dl) { status = -5; goto term; }
    at.write((const uint8_t*)s_http.body, s_http.body_len);
    if (!readLine(10000) || strcmp(s_line, "OK")) { /* tolerate: some firmwares skip OK */ }
  }
  s_http_action_done = false;
  snprintf(cmd, sizeof cmd, "AT+HTTPACTION=%d", strcmp(s_http.method, "POST") == 0 ? 1 : 0);
  if (atCmd(cmd, 5000) != 1) { status = -6; goto term; }
  {
    const uint32_t t0 = millis();
    while (!s_http_action_done && (uint32_t)(millis() - t0) < 90000) {
      if (readLine(500)) { if (isUrc(s_line)) handleUrc(s_line); }
    }
  }
  if (!s_http_action_done) { status = -7; goto term; }
  status = s_http_action_status;
  {
    size_t want = (size_t)(s_http_action_len > 0 ? s_http_action_len : 0);
    if (want > s_http.max_bytes) want = s_http.max_bytes;
    resp = (char*)heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) { status = -8; goto term; }
    while (resp_len < want) {
      size_t chunk = want - resp_len;
      if (chunk > 2048) chunk = 2048;
      snprintf(cmd, sizeof cmd, "AT+HTTPREAD=%u,%u", (unsigned)resp_len, (unsigned)chunk);
      pumpUrcs();
      at.print(cmd); at.print("\r");
      bool okc = false;
      const uint32_t t0 = millis();
      while ((uint32_t)(millis() - t0) < 10000) {
        if (!readLine(10000 - (millis() - t0))) continue;
        if (startsWith(s_line, "+HTTPREAD:")) {
          const int n = atoi(s_line + 10);
          if (n <= 0) { okc = true; break; }          // "+HTTPREAD: 0" = end marker
          const size_t take = (size_t)n > want - resp_len ? want - resp_len : (size_t)n;
          resp_len += readRaw(resp + resp_len, take, 10000);
          continue;
        }
        if (!strcmp(s_line, "OK")) continue;
        if (startsWith(s_line, "ERROR") || startsWith(s_line, "+CME")) break;
        if (isUrc(s_line)) handleUrc(s_line);
      }
      if (!okc) break;
    }
    resp[resp_len] = '\0';
  }
term:
  atCmd("AT+HTTPTERM", 3000);
done:
  if (s_http.body) { heap_caps_free(s_http.body); s_http.body = nullptr; }
  CELL_LOG("http %s %s -> %d (%u bytes)", s_http.method, s_http.url, status, (unsigned)resp_len);
  bool notify;
  {
    Lock l;
    if (s_http.resp) heap_caps_free(s_http.resp);
    if (s_http.sync && s_http.abandoned) {   // nobody is waiting any more
      if (resp) heap_caps_free(resp);
      resp = nullptr; resp_len = 0;
    }
    s_http.resp = resp; s_http.resp_len = resp_len; s_http.status = status;
    s_http.done = true; s_http.busy = false;
    notify = !s_http.sync;
  }
  if (notify) pushEvent(CELL_EV_HTTP, status);
}

void handleCmd(const Cmd& c) {
  CellPower pw; { Lock l; pw = s_st.power; }
  if (c.type == CMD_SYNC_ENABLE) return;   // the main loop reconciles power
  if (c.type == CMD_APN_CHANGED) { if (pw == CELL_POWER_READY) { applyApn(); Lock l; s_st.data_up = false; } return; }
  if (c.type == CMD_PIN_CHANGED) { if (pw == CELL_POWER_READY) querySim(); return; }
  if (pw != CELL_POWER_READY) {
    if (c.type == CMD_SMS) pushEvent(CELL_EV_SMS_SENT, 0, c.num);
    if (c.type == CMD_HTTP) {
      bool notify;
      { Lock l; s_http.status = -1; s_http.done = true; s_http.busy = false; notify = !s_http.sync; }
      if (s_http.body) { heap_caps_free(s_http.body); s_http.body = nullptr; }
      if (notify) pushEvent(CELL_EV_HTTP, -1);
    }
    if (c.type == CMD_DIAL) { setStatusError("modem off"); pushEvent(CELL_EV_CALL, CELL_CALL_IDLE, c.num); }
    return;
  }
  char cmd[64];
  switch (c.type) {
    case CMD_DIAL:
      snprintf(cmd, sizeof cmd, "ATD%s;", c.num);
      { Lock l; copyStr(s_st.call_number, sizeof s_st.call_number, c.num); }
      setCall(CELL_CALL_DIALING, c.num);
      // ERROR = refused (no network, barred). A timeout is NOT a failure: some
      // firmwares hold the OK until the call connects; the CLCC poll decides.
      if (atCmd(cmd, 20000) == 0) setCall(CELL_CALL_IDLE, nullptr);
      break;
    case CMD_ANSWER:
      if (atCmd("ATA", 20000) == 1) setCall(CELL_CALL_ACTIVE, nullptr);
      break;
    case CMD_HANGUP:
      if (atCmd("AT+CHUP", 5000) != 1) atCmd("ATH", 5000);
      setCall(CELL_CALL_IDLE, nullptr);
      break;
    case CMD_DTMF:
      snprintf(cmd, sizeof cmd, "AT+VTS=%c", (char)c.arg);
      atCmd(cmd, 3000);
      break;
    case CMD_VOLUME:
      snprintf(cmd, sizeof cmd, "AT+CLVL=%d", c.arg);
      atCmd(cmd, 2000);
      break;
    case CMD_MUTE:
      snprintf(cmd, sizeof cmd, "AT+CMUT=%d", c.arg ? 1 : 0);
      atCmd(cmd, 2000);
      break;
    case CMD_SMS:
      sendSms(c.num, c.text);
      break;
    case CMD_DATA:
      if (c.arg) dataUp(); else dataDown();
      break;
    case CMD_HTTP:
      runHttp();
      break;
    default: break;
  }
}

void cellTask(void*) {
  Cmd c;
  // Let the boot (display, radio, SD, Wi-Fi) finish before the modem's inrush
  // and the I2C traffic to its power lines join in.
  vTaskDelay(pdMS_TO_TICKS(8000));
  for (;;) {
    if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) == pdTRUE) handleCmd(c);

    // Reconcile power with the user setting.
    CellPower pw; bool en;
    { Lock l; pw = s_st.power; en = s_cfg.enabled; }
    const uint32_t now = millis();
    if (en && pw == CELL_POWER_OFF) { powerOn(); continue; }
    if (en && pw == CELL_POWER_ERROR && (int32_t)(now - s_retry_ms) >= 0) {
      s_retry_ms = now + 120000;       // a missing battery is the usual cause; retry slowly
      powerOn();
      continue;
    }
    if (!en && pw != CELL_POWER_OFF) { powerOff(); continue; }
    if (pw != CELL_POWER_READY) continue;

    pumpUrcs();

    while (s_sms_pending_n > 0) readSms(s_sms_pending[--s_sms_pending_n]);

    CellCall call; CellReg reg;
    { Lock l; call = s_st.call; reg = s_st.reg; }
    if (call != CELL_CALL_IDLE && (int32_t)(now - s_next_clcc_ms) >= 0) {
      // Poll as well as listening for +CLCC: not every modem firmware sends
      // the URC for every transition, and a missed hang-up would leave the
      // speaker routed to the modem.
      s_next_clcc_ms = now + 1500;
      static char lines[4][kLineCap];
      int n = 0;
      if (atCmd("AT+CLCC", 2000, "+CLCC:", nullptr, 0, lines, 4, &n) == 1) {
        if (n == 0) setCall(CELL_CALL_IDLE, nullptr);
        else handleClcc(lines[0]);
      }
    }
    if (call == CELL_CALL_IDLE && (int32_t)(now - s_next_poll_ms) >= 0) {
      const bool settled = (reg == CELL_REG_HOME || reg == CELL_REG_ROAMING);
      s_next_poll_ms = now + (settled ? 20000 : 4000);
      CellSim sim; { Lock l; sim = s_st.sim; }
      if (sim != CELL_SIM_READY) querySim();
      queryRegistration();
    }
  }
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================
void cellBegin() {
  if (s_task) return;
  s_mx = xSemaphoreCreateMutex();
  s_q  = xQueueCreate(6, sizeof(Cmd));
  loadSettings();
  {
    Lock l;
    s_st = {};
    s_st.csq = -1;
    s_st.enabled = s_cfg.enabled;
    copyStr(s_st.apn, sizeof s_st.apn, s_cfg.apn);
  }
  // Core 0, low priority: the task spends nearly all its time blocked on the
  // UART or the queue, and must never compete with LVGL on core 1.
  xTaskCreatePinnedToCore(cellTask, "cell", 8192, nullptr, 2, &s_task, 0);
  CELL_LOG("service started (enabled=%d apn=%s)", s_cfg.enabled, s_cfg.apn);
}

void cellSetEnabled(bool on) {
  { Lock l; s_cfg.enabled = on; s_st.enabled = on; }
  saveSettings();
  Cmd c = {}; c.type = CMD_SYNC_ENABLE; queueCmd(c);
}

void cellSetApn(const char* apn, const char* user, const char* pass) {
  {
    Lock l;
    copyStr(s_cfg.apn, sizeof s_cfg.apn, apn);
    copyStr(s_cfg.user, sizeof s_cfg.user, user);
    copyStr(s_cfg.pass, sizeof s_cfg.pass, pass);
    copyStr(s_st.apn, sizeof s_st.apn, apn);
  }
  saveSettings();
  Cmd c = {}; c.type = CMD_APN_CHANGED; queueCmd(c);
}

void cellSetPin(const char* pin) {
  { Lock l; copyStr(s_cfg.pin, sizeof s_cfg.pin, pin); }
  saveSettings();
  Cmd c = {}; c.type = CMD_PIN_CHANGED; queueCmd(c);
}

void cellGetStatus(CellStatus* out) {
  if (!out) return;
  Lock l;
  *out = s_st;
}

bool cellPollEvent(CellEvent* out) {
  Lock l;
  if (s_ev_tail == s_ev_head) return false;
  *out = s_ev[s_ev_tail];
  s_ev_tail = (s_ev_tail + 1) % kEvRing;
  return true;
}

bool cellDial(const char* number) {
  if (!validNumber(number)) return false;
  Cmd c = {}; c.type = CMD_DIAL; copyStr(c.num, sizeof c.num, number);
  return queueCmd(c);
}
bool cellAnswer() { Cmd c = {}; c.type = CMD_ANSWER; return queueCmd(c); }
bool cellHangup() { Cmd c = {}; c.type = CMD_HANGUP; return queueCmd(c); }
bool cellDtmf(char d) {
  if (!((d >= '0' && d <= '9') || d == '*' || d == '#' || (d >= 'A' && d <= 'D'))) return false;
  Cmd c = {}; c.type = CMD_DTMF; c.arg = d; return queueCmd(c);
}
bool cellSetVolume(int level) {
  if (level < 0) level = 0;
  if (level > 5) level = 5;
  Cmd c = {}; c.type = CMD_VOLUME; c.arg = (int16_t)level; return queueCmd(c);
}
bool cellSetMute(bool mute) { Cmd c = {}; c.type = CMD_MUTE; c.arg = mute; return queueCmd(c); }

bool cellSmsSend(const char* number, const char* text) {
  if (!validNumber(number) || !text || !*text) return false;
  if (utf16Units(text) > 70) return false;     // one UCS2 SMS; no concatenation yet
  if (strlen(text) >= sizeof(Cmd::text)) return false;
  Cmd c = {}; c.type = CMD_SMS;
  copyStr(c.num, sizeof c.num, number);
  copyStr(c.text, sizeof c.text, text);
  return queueCmd(c);
}

bool cellDataConnect(bool up) { Cmd c = {}; c.type = CMD_DATA; c.arg = up; return queueCmd(c); }

static bool httpStart(const char* method, const char* url, const char* ctype,
                      const char* body, size_t body_len, size_t max_bytes, bool sync) {
  if (!url || strlen(url) >= sizeof s_http.url || strchr(url, '"')) return false;
  if (!(startsWith(url, "http://") || startsWith(url, "https://"))) return false;
  if (ctype && (strlen(ctype) >= sizeof s_http.ctype || strchr(ctype, '"'))) return false;
  {
    Lock l;
    if (s_http.busy) return false;
    s_http.busy = true;
    s_http.done = false;
    s_http.sync = sync;
    s_http.abandoned = false;
  }
  copyStr(s_http.method, sizeof s_http.method, (method && !strcmp(method, "POST")) ? "POST" : "GET");
  copyStr(s_http.url, sizeof s_http.url, url);
  copyStr(s_http.ctype, sizeof s_http.ctype, ctype);
  s_http.body = nullptr; s_http.body_len = 0;
  if (body && body_len) {
    s_http.body = (char*)heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_http.body) { Lock l; s_http.busy = false; return false; }
    memcpy(s_http.body, body, body_len);
    s_http.body_len = body_len;
  }
  s_http.max_bytes = max_bytes ? max_bytes : 32 * 1024;
  Cmd c = {}; c.type = CMD_HTTP;
  if (!queueCmd(c)) {
    if (s_http.body) { heap_caps_free(s_http.body); s_http.body = nullptr; }
    Lock l; s_http.busy = false;
    return false;
  }
  return true;
}

bool cellHttpStart(const char* method, const char* url, const char* ctype,
                   const char* body, size_t body_len, size_t max_bytes) {
  return httpStart(method, url, ctype, body, body_len, max_bytes, false);
}

int cellHttpGetSync(const char* url, char** buf, size_t* len, size_t max_bytes, uint32_t timeout_ms) {
  if (buf) *buf = nullptr;
  if (len) *len = 0;
  if (xTaskGetCurrentTaskHandle() == s_task) return -12;   // would wait on itself
  if (!httpStart("GET", url, "", nullptr, 0, max_bytes, true)) return -10;   // busy or invalid
  const uint32_t t0 = millis();
  for (;;) {
    { Lock l; if (s_http.done) break; }
    if ((uint32_t)(millis() - t0) >= timeout_ms) {
      Lock l;
      if (s_http.done) break;
      s_http.abandoned = true;   // runHttp frees the late response
      return -11;
    }
    vTaskDelay(pdMS_TO_TICKS(20));   // lets IDLE feed the watchdog on this core
  }
  return cellHttpTake(buf, len);
}

bool cellDataIsUp() {
  Lock l;
  return s_st.power == CELL_POWER_READY && s_st.data_up;
}

bool cellHttpBusy() { Lock l; return s_http.busy; }

int cellHttpTake(char** buf, size_t* len) {
  Lock l;
  if (buf) *buf = s_http.resp; else if (s_http.resp) heap_caps_free(s_http.resp);
  if (len) *len = s_http.resp_len;
  s_http.resp = nullptr; s_http.resp_len = 0;
  s_http.done = false;
  return s_http.status;
}

const char* cellPowerName(CellPower p) {
  switch (p) {
    case CELL_POWER_OFF: return "off";
    case CELL_POWER_BOOTING: return "booting";
    case CELL_POWER_READY: return "ready";
    case CELL_POWER_ERROR: return "error";
  }
  return "?";
}
const char* cellSimName(CellSim s) {
  switch (s) {
    case CELL_SIM_UNKNOWN: return "unknown";
    case CELL_SIM_READY: return "ready";
    case CELL_SIM_PIN: return "pin";
    case CELL_SIM_PUK: return "puk";
    case CELL_SIM_ABSENT: return "absent";
  }
  return "?";
}
const char* cellRegName(CellReg r) {
  switch (r) {
    case CELL_REG_NONE: return "none";
    case CELL_REG_SEARCHING: return "searching";
    case CELL_REG_HOME: return "home";
    case CELL_REG_ROAMING: return "roaming";
    case CELL_REG_DENIED: return "denied";
  }
  return "?";
}
const char* cellCallName(CellCall c) {
  switch (c) {
    case CELL_CALL_IDLE: return "idle";
    case CELL_CALL_DIALING: return "dialing";
    case CELL_CALL_ALERTING: return "alerting";
    case CELL_CALL_INCOMING: return "incoming";
    case CELL_CALL_ACTIVE: return "active";
    case CELL_CALL_HELD: return "held";
  }
  return "?";
}
