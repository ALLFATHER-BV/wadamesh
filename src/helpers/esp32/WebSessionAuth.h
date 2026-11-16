// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#define WSA_LOCK()   taskENTER_CRITICAL(&_mux)
#define WSA_UNLOCK() taskEXIT_CRITICAL(&_mux)
#else
#define WSA_LOCK()   ((void)0)
#define WSA_UNLOCK() ((void)0)
#endif
// Off-device (host tests) the caller provides millis().

// A browser the device user chose to "Always allow". `key` is a hash of the random id
// the browser keeps in localStorage (the id itself is never stored); `label` is the
// browser's own description ("Chrome on Mac") so the user can tell entries apart.
struct WebTrustedEntry {
  uint8_t key[16];
  char label[24];
};

// Who may use the web console. A browser opens every socket with "HELLO <id> <label>":
// a trusted browser, or one already allowed since the console came on, is let in at
// once; any other raises ONE request at a time that the device user answers with
// Allow once / Always allow / Deny (no answer within REQUEST_MS counts as Deny).
// The 6-digit code (WebAuthCode) stays as a fallback and counts as Allow once.
//
// Written from the web-server task (hello/status) and the UI task (pending/resolve,
// the trusted list), so every access takes the lock; each one is a few memcpys.
class WebSessionAuth {
public:
  static const int MAX_TRUSTED = 8;
  static const int MAX_SESSIONS = 8;
  static const uint32_t REQUEST_MS = 30000;
  static const uint32_t DENY_HOLD_MS = 10000;   // a denied browser's retries are refused this long

  enum Check : uint8_t { OK, WAIT, BUSY, DENIED };
  enum Decision : uint8_t { DENY, ALLOW_ONCE, ALLOW_ALWAYS };

  // A browser's first frame. OK: let it in. WAIT: the device is now asking (or already
  // was, for this browser). BUSY: another browser is being asked. DENIED: just refused.
  // `ip` (IPv4, first octet in the low byte; 0 = unknown) is shown with the request.
  Check hello(const uint8_t key[16], const char* label, uint32_t ip = 0) {
    WSA_LOCK();
    expireLocked();
    Check r;
    if (approvedLocked(key))                    r = OK;
    else if (deniedLocked(key))                 r = DENIED;
    else if (_pending && same(_pend_key, key))  r = WAIT;
    else if (_pending)                          r = BUSY;
    else {
      memcpy(_pend_key, key, 16);
      copyLabel(_pend_label, label);
      _pend_ip = ip;
      _pend_ms = millis();
      _pending = true;
      ++_pend_seq;
      r = WAIT;
    }
    WSA_UNLOCK();
    return r;
  }

  // Polled for a browser that got WAIT: OK once allowed, DENIED once refused or timed
  // out (or the request was dropped), else still WAIT.
  Check status(const uint8_t key[16]) {
    WSA_LOCK();
    expireLocked();
    Check r = approvedLocked(key) ? OK
            : (_pending && same(_pend_key, key)) ? WAIT : DENIED;
    WSA_UNLOCK();
    return r;
  }

  bool approved(const uint8_t key[16]) {
    WSA_LOCK();
    const bool r = approvedLocked(key);
    WSA_UNLOCK();
    return r;
  }

  // The code fallback: this browser is in for as long as the console stays on.
  void allowSession(const uint8_t key[16]) {
    WSA_LOCK();
    addSessionLocked(key);
    if (_pending && same(_pend_key, key)) _pending = false;
    WSA_UNLOCK();
  }

  // UI side: the request to show, if any. `seq` identifies it for resolve(); `ip` is
  // the address it came from (0 = unknown).
  bool pending(char* label, size_t cap, uint32_t* seq, uint32_t* ip = nullptr) {
    WSA_LOCK();
    expireLocked();
    const bool r = _pending;
    if (r) {
      if (label && cap) { strncpy(label, _pend_label, cap - 1); label[cap - 1] = '\0'; }
      if (seq) *seq = _pend_seq;
      if (ip) *ip = _pend_ip;
    }
    WSA_UNLOCK();
    return r;
  }

  // Answer request `seq`. Returns true when the trusted list changed (save it).
  bool resolve(uint32_t seq, Decision d) {
    WSA_LOCK();
    bool changed = false;
    if (_pending && seq == _pend_seq) {
      _pending = false;
      if (d == DENY) {
        memcpy(_deny_key, _pend_key, 16);
        _deny_ms = millis();
        _denied = true;
      } else {
        addSessionLocked(_pend_key);
        if (d == ALLOW_ALWAYS) changed = addTrustedLocked(_pend_key, _pend_label);
      }
    }
    WSA_UNLOCK();
    return changed;
  }

  // Console off / "End all sessions": forget every Allow-once and any open request,
  // and bump generation() so the server drops the browsers that are connected now.
  void endSessions() {
    WSA_LOCK();
    _n_sessions = 0;
    _pending = false;
    _denied = false;
    ++_gen;
    WSA_UNLOCK();
  }
  uint32_t generation() const { return _gen; }

  int trustedCount() const { return _n_trusted; }
  bool trustedAt(int i, WebTrustedEntry* out) {
    WSA_LOCK();
    const bool ok = i >= 0 && i < _n_trusted;
    if (ok && out) *out = _trusted[i];
    WSA_UNLOCK();
    return ok;
  }
  // Revoking also ends that browser's session and drops connected browsers (they
  // reconnect; the others still trusted or allowed get straight back in).
  bool revoke(int i) {
    WSA_LOCK();
    const bool ok = i >= 0 && i < _n_trusted;
    if (ok) {
      removeSessionLocked(_trusted[i].key);
      for (int j = i; j + 1 < _n_trusted; ++j) _trusted[j] = _trusted[j + 1];
      --_n_trusted;
      ++_gen;
    }
    WSA_UNLOCK();
    return ok;
  }
  void loadTrusted(const WebTrustedEntry* list, int n) {
    WSA_LOCK();
    _n_trusted = 0;
    for (int i = 0; list && i < n && _n_trusted < MAX_TRUSTED; ++i) {
      _trusted[_n_trusted] = list[i];
      _trusted[_n_trusted].label[sizeof _trusted[0].label - 1] = '\0';
      ++_n_trusted;
    }
    WSA_UNLOCK();
  }
  int exportTrusted(WebTrustedEntry* out, int cap) {
    WSA_LOCK();
    int n = 0;
    for (; out && n < _n_trusted && n < cap; ++n) out[n] = _trusted[n];
    WSA_UNLOCK();
    return n;
  }

private:
  static bool same(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 16) == 0; }

  // Printable ASCII only (it is drawn with the device fonts), at most 23 chars.
  static void copyLabel(char* dst, const char* src) {
    size_t n = 0;
    for (; src && src[n] && n < sizeof(WebTrustedEntry::label) - 1; ++n)
      dst[n] = (src[n] >= 0x20 && src[n] < 0x7F) ? src[n] : '?';
    if (n == 0) { strcpy(dst, "Browser"); return; }
    dst[n] = '\0';
  }

  void expireLocked() {
    const uint32_t now = millis();
    if (_pending && now - _pend_ms >= REQUEST_MS) {
      _pending = false;
      memcpy(_deny_key, _pend_key, 16);
      _deny_ms = now;
      _denied = true;
    }
    if (_denied && now - _deny_ms >= DENY_HOLD_MS) _denied = false;
  }
  bool deniedLocked(const uint8_t* key) const { return _denied && same(_deny_key, key); }
  bool approvedLocked(const uint8_t* key) const {
    for (int i = 0; i < _n_trusted; ++i) if (same(_trusted[i].key, key)) return true;
    for (int i = 0; i < _n_sessions; ++i) if (same(_sessions[i], key)) return true;
    return false;
  }
  void addSessionLocked(const uint8_t* key) {
    for (int i = 0; i < _n_sessions; ++i) if (same(_sessions[i], key)) return;
    if (_n_sessions == MAX_SESSIONS) {   // full: forget the oldest
      memmove(_sessions[0], _sessions[1], (MAX_SESSIONS - 1) * 16);
      --_n_sessions;
    }
    memcpy(_sessions[_n_sessions++], key, 16);
  }
  void removeSessionLocked(const uint8_t* key) {
    for (int i = 0; i < _n_sessions; ++i) {
      if (!same(_sessions[i], key)) continue;
      memmove(_sessions[i], _sessions[i + 1], (size_t)(_n_sessions - i - 1) * 16);
      --_n_sessions;
      return;
    }
  }
  bool addTrustedLocked(const uint8_t* key, const char* label) {
    for (int i = 0; i < _n_trusted; ++i) if (same(_trusted[i].key, key)) return false;
    if (_n_trusted == MAX_TRUSTED) {     // full: the oldest entry makes room
      for (int j = 0; j + 1 < MAX_TRUSTED; ++j) _trusted[j] = _trusted[j + 1];
      --_n_trusted;
    }
    memcpy(_trusted[_n_trusted].key, key, 16);
    copyLabel(_trusted[_n_trusted].label, label);
    ++_n_trusted;
    return true;
  }

#ifdef ARDUINO
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
#endif
  WebTrustedEntry _trusted[MAX_TRUSTED];
  uint8_t _sessions[MAX_SESSIONS][16];
  int _n_trusted = 0;
  int _n_sessions = 0;
  uint8_t _pend_key[16];
  char _pend_label[24];
  uint32_t _pend_ip = 0;
  uint32_t _pend_ms = 0;
  uint32_t _pend_seq = 0;
  bool _pending = false;
  uint8_t _deny_key[16];
  uint32_t _deny_ms = 0;
  bool _denied = false;
  volatile uint32_t _gen = 0;
};

extern WebSessionAuth g_web_session;
