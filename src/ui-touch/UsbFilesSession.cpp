#include "UsbFilesSession.h"

#if CAP_USB_FILES

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ff.h"
#include "SdThreat.h"
#include "../helpers/esp32/WdtHeavyGuard.h"

#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include "hal/usb_serial_jtag_ll.h"
#endif

using namespace UsbFiles;

namespace {

constexpr uint32_t kBeaconMs = 1000;        // "USB Files is open", until a page answers
constexpr uint32_t kHostIdleMs = 6000;      // the page pings every 2 s while idle
constexpr uint32_t kUploadIdleMs = 30000;   // an upload with no data this long is dropped
constexpr uint32_t kSendingShowMs = 1500;   // how long "Sending" stays up after a chunk
constexpr uint32_t kReadIdleMs = 10000;     // then the cached read handle is released
constexpr uint32_t kTickBudgetMs = 25;      // longest one tick holds up the UI loop
constexpr uint32_t kMaxUpload = 512u * 1024u * 1024u;
constexpr size_t kRxQueue = kMaxFrame + 512;   // one whole request always fits
constexpr size_t kDefaultRxQueue = 256;        // what both USB serial drivers start with
constexpr size_t kParserCap = 2 * kMaxFrame;
constexpr const char* kTempRel = "/.wmup.part";
constexpr const char* kTempName = ".wmup.part";
constexpr Root kRoots[] = {ROOT_SD, ROOT_INTERNAL, ROOT_TILES};

// Resize the USB serial receive queue. Both drivers drop bytes once it is full,
// so it must hold one whole request frame for the session. Returns the size now
// in effect, 0 when that is unknown or unchanged (hardware UART).
size_t setRxQueue(size_t n) {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // HW-CDC deletes and recreates the queue its RX interrupt writes into. Mask the
  // interrupt around the swap; meanwhile packets wait in the 64-byte hardware
  // FIFO and the host is NAKed, so nothing is lost.
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  size_t got = Serial.setRxBufferSize(n);
  if (!got) got = Serial.setRxBufferSize(kDefaultRxQueue);   // never leave it without one
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  return got;
#elif ARDUINO_USB_CDC_ON_BOOT
  return Serial.setRxBufferSize(n);   // TinyUSB CDC keeps the old queue if this fails
#else
  (void)n;                            // hardware UART: fixed once running
  return 0;
#endif
}

// File bytes per frame that fit a receive queue of this size, whole frame included.
size_t chunkFor(size_t queue) {
  const size_t overhead = kHeaderBytes + 4 + kTrailerBytes + 16;   // + offset + slack
  if (queue >= kMaxChunk + overhead) return kMaxChunk;
  if (queue >= 1024 + overhead) return 1024;
  return 192;   // the 256-byte default
}

uint8_t* allocPreferPsram(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return static_cast<uint8_t*>(p);
}

const char* baseName(const char* rel) {
  const char* s = strrchr(rel, '/');
  return s ? s + 1 : rel;
}

// "/a/b" -> "/a", "/a" -> "/".
void parentOf(const char* rel, char* out, size_t cap) {
  const char* s = strrchr(rel, '/');
  size_t n = s ? static_cast<size_t>(s - rel) : 0;
  if (n == 0) n = 1;
  if (n >= cap) n = cap - 1;
  memcpy(out, rel, n);
  out[n] = '\0';
}

// child = rel + "/" + name, for the access check on a listed entry.
bool childOf(const char* rel, const char* name, char* out, size_t cap) {
  const bool at_root = rel[0] == '/' && rel[1] == '\0';
  const int n = at_root ? snprintf(out, cap, "/%s", name) : snprintf(out, cap, "%s/%s", rel, name);
  return n > 0 && static_cast<size_t>(n) < cap;
}

struct Json {
  char* b;
  size_t cap;
  size_t pos = 0;
  bool full = false;
  Json(uint8_t* buf, size_t c) : b(reinterpret_cast<char*>(buf)), cap(c) {}
  Json(char* buf, size_t c) : b(buf), cap(c) {}
  void raw(const char* s) {
    const size_t n = strlen(s);
    if (full || pos + n >= cap) { full = true; return; }
    memcpy(b + pos, s, n);
    pos += n;
  }
  void str(const char* s) {
    raw("\"");
    if (full) return;
    const size_t np = jsonAppendEscaped(b, pos, cap, s);
    if (np >= cap) { full = true; return; }
    pos = np;
    raw("\"");
  }
  void num(uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%llu", static_cast<unsigned long long>(v));
    raw(t);
  }
};

// SPIFFS garbage collection and LittleFS compaction can hold the flash for
// seconds: internal-flash writes run with the task watchdogs off, like every
// other internal-flash writer in the firmware (WdtHeavyGuard.h). The SD is FAT
// on its own bus and never needs it.
bool internalFlash(const UsbFilesRootInfo& info) { return info.id != ROOT_SD; }

}  // namespace

// ---- lifecycle ----------------------------------------------------------------

bool UsbFilesSession::begin(UsbFilesHost* host) {
  if (_running) return true;
  if (!host) return false;
  _host = host;
  _rxbuf = allocPreferPsram(kParserCap);
  _tx = allocPreferPsram(kMaxFrame);
  // The Arduino SD driver short-writes when handed PSRAM, so every byte that
  // goes to or comes from a file passes through this internal buffer.
  _bounce = static_cast<uint8_t*>(
      heap_caps_malloc(kMaxChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (_rxbuf && _tx && _bounce) _parser = new (std::nothrow) FrameParser(_rxbuf, kParserCap);
  if (!_parser) {
    release();
    return false;
  }

  g_usb_files_owns_serial = true;   // companion link and console stay off the port
  // A big queue is internal RAM the V4 may not have to spare: settle for a smaller
  // one, and announce the chunk size that fits whatever was granted.
  size_t queue = setRxQueue(kRxQueue);
  if (queue && queue < kRxQueue) queue = setRxQueue(1024 + 512);
  _chunk = chunkFor(queue ? queue : kDefaultRxQueue);
  while (Serial.available() > 0) Serial.read();

  _tx_len = 0;
  _last_type = 0;
  _last_seq = 0;
  _host_seen = false;
  _last_rx_ms = millis();
  _last_beacon_ms = millis() - kBeaconMs;
  _up = false;
  _name[0] = '\0';
  _result = RESULT_NONE;
  _running = true;

  // A session that lost power mid-upload leaves its temp file behind.
  for (Root r : kRoots) {
    UsbFilesRootInfo info;
    if (!_host->root(r, &info, false) || !info.fs) continue;
    bool is_dir = false;
    uint32_t size = 0;
    if (statRel(info, kTempRel, &is_dir, &size) && !is_dir) {
      WdtHeavyGuard heavy;
      LoopWdtGuard loop;
      info.fs->remove(kTempRel);
    }
  }
  return true;
}

void UsbFilesSession::end() {
  if (!_running) return;
  abortUpload(nullptr);
  closeRead();
  closeList();
  sendBeacon(T_BYE);
  setRxQueue(kDefaultRxQueue);
  g_usb_files_owns_serial = false;
  _running = false;
  release();
}

void UsbFilesSession::release() {
  delete _parser;
  _parser = nullptr;
  heap_caps_free(_rxbuf);
  heap_caps_free(_tx);
  heap_caps_free(_bounce);
  _rxbuf = _tx = _bounce = nullptr;
}

UsbFilesSession::Activity UsbFilesSession::activity() const {
  if (_up) return RECEIVING;
  const uint32_t now = millis();
  if (_rd_file && now - _rd_ms < kSendingShowMs) return SENDING;
  if (_host_seen && now - _last_rx_ms < kHostIdleMs) return CONNECTED;
  return WAITING;
}

uint32_t UsbFilesSession::progressDone() const {
  if (_up) return _up_got;
  return _rd_file ? _rd_end : 0;
}

uint32_t UsbFilesSession::progressTotal() const {
  if (_up) return _up_size;
  return _rd_file ? _rd_size : 0;
}

void UsbFilesSession::setName(const char* rel) {
  snprintf(_name, sizeof _name, "%s", baseName(rel));
}

// ---- the loop ---------------------------------------------------------------

void UsbFilesSession::tick() {
  if (!_running) return;
  const uint32_t t0 = millis();
  uint32_t last_work = t0;
  for (;;) {
    pumpRx();
    bool any = false;
    while (_parser->next()) {
      handleFrame();
      any = true;
    }
    const uint32_t now = millis();
    if (any) last_work = now;
    if (now - t0 >= kTickBudgetMs) break;
    if (!any) {
      // Mid-transfer the next request follows within a millisecond or two: wait
      // for it here instead of paying a whole UI loop pass per chunk.
      const bool busy = _up || (_host_seen && now - _last_rx_ms < 50);
      if (!busy || now - last_work >= 5) break;
      delay(1);
    }
  }

  const uint32_t now = millis();
  if (_up && now - _up_ms > kUploadIdleMs) abortUpload("the computer stopped sending");
  if (_rd_file && now - _rd_ms > kReadIdleMs) closeRead();
  if (_host_seen && now - _last_rx_ms >= kHostIdleMs) {
    _host_seen = false;   // page closed or cable pulled
    closeRead();
    closeList();
    if (_up) abortUpload("the computer disconnected");
  }
  if (!_host_seen && now - _last_beacon_ms >= kBeaconMs) {
    _last_beacon_ms = now;
    sendBeacon(T_BEACON);
  }
}

void UsbFilesSession::pumpRx() {
  uint8_t tmp[256];
  for (int i = 0; i < 64; ++i) {
    const int avail = Serial.available();
    if (avail <= 0 || _parser->space() == 0) break;
    size_t want = static_cast<size_t>(avail);
    if (want > sizeof tmp) want = sizeof tmp;
    if (want > _parser->space()) want = _parser->space();
    const size_t got = Serial.read(tmp, want);
    if (got == 0) break;
    _parser->push(tmp, got);
  }
}

void UsbFilesSession::handleFrame() {
  const uint8_t type = _parser->type();
  const uint16_t seq = _parser->seq();
  const uint8_t* p = _parser->payload();
  const size_t len = _parser->length();
  if (type & T_REPLY) return;   // not a request
  _last_rx_ms = millis();
  _host_seen = true;

  // The page repeats a request when our reply got lost. Answer from the cache so
  // a finished upload, delete or rename is not attempted a second time.
  if (type != T_HELLO && _tx_len && seq == _last_seq && type == _last_type) {
    Serial.write(_tx, _tx_len);
    return;
  }

  switch (type) {
    case T_HELLO: doHello(seq); break;
    case T_LIST: doList(seq, p, len); break;
    case T_STAT: doStat(seq, p, len); break;
    case T_READ: doRead(seq, p, len); break;
    case T_WRITE_BEGIN: doWriteBegin(seq, p, len); break;
    case T_WRITE_DATA: doWriteData(seq, p, len); break;
    case T_WRITE_END: doWriteEnd(seq, p, len); break;
    case T_REMOVE: doRemove(seq, p, len); break;
    case T_MKDIR: doMkdir(seq, p, len); break;
    case T_RENAME: doRename(seq, p, len); break;
    case T_SPACE: doSpace(seq, p, len); break;
    case T_ABORT:
      abortUpload("cancelled on the computer");
      sendReply(type, seq, S_OK, 0);
      break;
    case T_PING: sendReply(type, seq, S_OK, 0); break;
    default: sendError(type, seq, S_UNSUPPORTED, "unknown request"); break;
  }
}

// ---- frames -------------------------------------------------------------------

bool UsbFilesSession::sendReply(uint8_t req_type, uint16_t seq, uint8_t status, size_t body_len) {
  uint8_t* payload = _tx + kHeaderBytes;   // body() already sits right after the status byte
  payload[0] = status;
  _tx_len = encodeFrame(_tx, kMaxFrame, req_type | T_REPLY, seq, payload, 1 + body_len);
  _last_type = req_type;
  _last_seq = seq;
  if (!_tx_len) return false;
  return Serial.write(_tx, _tx_len) == _tx_len;
}

bool UsbFilesSession::sendError(uint8_t req_type, uint16_t seq, uint8_t status, const char* msg) {
  size_t n = msg ? strlen(msg) : 0;
  if (n > 120) n = 120;
  if (n) memcpy(body(), msg, n);
  return sendReply(req_type, seq, status, n);
}

void UsbFilesSession::sendBeacon(uint8_t type) {
  char json[288];
  Json j(json, sizeof json);
  j.raw("{\"app\":\"wadamesh-usb-files\",\"v\":");
  j.num(kVersion);
  char desc[200] = {0};
  _host->describe(desc, sizeof desc);
  if (desc[0]) {
    j.raw(",");
    j.raw(desc);
  }
  j.raw("}");
  if (j.full) return;
  uint8_t frame[320];
  const size_t n = encodeFrame(frame, sizeof frame, type, 0,
                               reinterpret_cast<const uint8_t*>(json), j.pos);
  if (n) Serial.write(frame, n);
}

// ---- paths and files ------------------------------------------------------------

bool UsbFilesSession::resolve(uint8_t type, uint16_t seq, const uint8_t* p, size_t len,
                              bool mount, UsbFilesRootInfo* info, char* rel, size_t rel_cap) {
  Root r = ROOT_NONE;
  if (!splitPath(reinterpret_cast<const char*>(p), len, &r, rel, rel_cap) || r == ROOT_NONE) {
    sendError(type, seq, S_BAD_PATH, "not a valid path");
    return false;
  }
  *info = UsbFilesRootInfo();
  if (!_host->root(r, info, mount)) {
    sendError(type, seq, S_NOT_FOUND, "this device has no such storage");
    return false;
  }
  if (!info->fs) {
    sendError(type, seq, S_NO_MEDIA, r == ROOT_SD ? "no SD card" : "storage not available");
    return false;
  }
  if (info->max_rel && strlen(rel) > info->max_rel) {
    sendError(type, seq, S_BAD_PATH, "name too long for this storage");
    return false;
  }
  return true;
}

bool UsbFilesSession::fullPath(const UsbFilesRootInfo& info, const char* rel, char* out,
                               size_t cap) const {
  const bool at_root = rel[0] == '/' && rel[1] == '\0';
  const int n = at_root ? snprintf(out, cap, "%s", info.vfs)
                        : snprintf(out, cap, "%s%s", info.vfs, rel);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool UsbFilesSession::statRel(const UsbFilesRootInfo& info, const char* rel, bool* is_dir,
                              uint32_t* size) const {
  *is_dir = false;
  *size = 0;
  if (rel[0] == '/' && rel[1] == '\0') {
    *is_dir = true;
    return true;
  }
  char full[kMaxPath + 24];
  if (!fullPath(info, rel, full, sizeof full)) return false;
  struct stat st;
  if (stat(full, &st) == 0) {
    *is_dir = S_ISDIR(st.st_mode);
    *size = *is_dir ? 0 : static_cast<uint32_t>(st.st_size);
    return true;
  }
  if (info.flat && flatHasChildren(info, rel)) {
    *is_dir = true;
    return true;
  }
  return false;
}

// SPIFFS has no folders: "/msgs" exists exactly when some file is named "/msgs/...".
// Its opendir filters on that prefix.
bool UsbFilesSession::flatHasChildren(const UsbFilesRootInfo& info, const char* rel) const {
  char full[kMaxPath + 24];
  if (!fullPath(info, rel, full, sizeof full)) return false;
  DIR* d = opendir(full);
  if (!d) return false;
  const bool any = readdir(d) != nullptr;
  closedir(d);
  return any;
}

uint8_t UsbFilesSession::threatOf(const UsbFilesRootInfo& info, const char* rel,
                                  uint32_t size) {
  const SdThreat::Kind by_name = SdThreat::byName(baseName(rel));
  if (by_name != SdThreat::None || size < SdThreat::kHeadBytes) return by_name;
  // "MZ" plus a "PE\0\0" signature where the DOS header points: a real Windows
  // program, whatever it is called. Read through the DMA-safe buffer (SD).
  File f = info.fs->open(rel, FILE_READ);
  if (!f) return SdThreat::None;
  uint8_t* head = _bounce;
  uint8_t* sig = _bounce + SdThreat::kHeadBytes;
  SdThreat::Kind kind = SdThreat::None;
  const size_t n = f.read(head, SdThreat::kHeadBytes);
  if (SdThreat::mzHeader(head, n)) {
    const uint32_t off = SdThreat::peOffset(head);
    if (SdThreat::peOffsetPlausible(off, size) && f.seek(off) && f.read(sig, 4) == 4 &&
        SdThreat::peSignature(sig, 4))
      kind = SdThreat::RenamedProgram;
  }
  f.close();
  if (info.id == ROOT_SD) _host->noteIo(ROOT_SD);
  return kind;
}

bool UsbFilesSession::readEntry(const UsbFilesRootInfo& info, const char* rel, char* name,
                                size_t cap, bool* is_dir, uint32_t* size, bool want_size) {
  *size = 0;
  if (_ls_ff) {
    FILINFO fno;
    for (;;) {
      if (f_readdir(static_cast<FF_DIR*>(_ls_ff), &fno) != FR_OK || fno.fname[0] == '\0')
        return false;
      if (strcmp(fno.fname, ".") == 0 || strcmp(fno.fname, "..") == 0) continue;
      break;
    }
    ++_ls_index;
    snprintf(name, cap, "%s", fno.fname);
    *is_dir = (fno.fattrib & AM_DIR) != 0;
    if (!*is_dir) *size = static_cast<uint32_t>(fno.fsize);
    return true;
  }
  if (!_ls_dir) return false;
  struct dirent* de = nullptr;
  for (;;) {
    de = readdir(_ls_dir);
    if (!de) return false;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    break;
  }
  ++_ls_index;
  const char* nm = de->d_name;
  if (info.flat) {
    const char* slash = strchr(nm, '/');
    if (slash) {
      size_t n = static_cast<size_t>(slash - nm);
      if (n >= cap) n = cap - 1;
      memcpy(name, nm, n);
      name[n] = '\0';
      *is_dir = true;
      return true;
    }
    snprintf(name, cap, "%s", nm);
    *is_dir = false;
  } else {
    snprintf(name, cap, "%s", nm);
    *is_dir = de->d_type == DT_DIR;
  }
  if ((want_size && !*is_dir) || (!info.flat && de->d_type == DT_UNKNOWN)) {
    char child[kMaxPath + 1];
    bool d = false;
    uint32_t s = 0;
    if (childOf(rel, name, child, sizeof child) && statRel(info, child, &d, &s)) {
      *is_dir = d;
      *size = s;
    }
  }
  return true;
}

void UsbFilesSession::closeList() {
  if (_ls_dir) closedir(_ls_dir);
  _ls_dir = nullptr;
  if (_ls_ff) {
    f_closedir(static_cast<FF_DIR*>(_ls_ff));
    delete static_cast<FF_DIR*>(_ls_ff);
  }
  _ls_ff = nullptr;
  _ls_path[0] = '\0';
  _ls_index = 0;
}

void UsbFilesSession::closeRead() {
  if (_rd_file) _rd_file.close();
  _rd_path[0] = '\0';
  _rd_size = 0;
  _rd_end = 0;
}

void UsbFilesSession::abortUpload(const char* why) {
  if (!_up) return;
  _up = false;
  {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    if (_up_file) _up_file.close();
    if (_up_info.fs) _up_info.fs->remove(kTempRel);
  }
  // The page shows the reason; the device screen only says the upload stopped.
  if (why) _result = RESULT_STOPPED;
}

// ---- requests -----------------------------------------------------------------

void UsbFilesSession::doHello(uint16_t seq) {
  abortUpload(nullptr);
  closeRead();
  closeList();
  Json j(body(), bodyCap());
  j.raw("{\"app\":\"wadamesh-usb-files\",\"v\":");
  j.num(kVersion);
  char desc[200] = {0};
  _host->describe(desc, sizeof desc);
  if (desc[0]) {
    j.raw(",");
    j.raw(desc);
  }
  j.raw(",\"chunk\":");
  j.num(_chunk);
  j.raw(",\"roots\":[");
  bool first = true;
  for (Root r : kRoots) {
    UsbFilesRootInfo info;
    if (!_host->root(r, &info, true)) continue;
    if (!first) j.raw(",");
    first = false;
    j.raw("{\"id\":");
    j.str(rootName(r));
    j.raw(",\"label\":");
    j.str(info.label);
    j.raw(info.fs ? ",\"ok\":true}" : ",\"ok\":false}");
  }
  j.raw("]}");
  if (j.full) {
    sendError(T_HELLO, seq, S_IO, "reply too long");
    return;
  }
  sendReply(T_HELLO, seq, S_OK, j.pos);
}

// LIST reply: {"e":[[name,is_dir,size,writable,threat],...],"n":next_start or -1}.
// threat is SD Scan's verdict on the name (SdThreat::Kind, 0 = none); STAT adds
// the content check. A folder is read page by page; the handle stays open between pages.
void UsbFilesSession::doList(uint16_t seq, const uint8_t* p, size_t len) {
  if (len < 7) {
    sendError(T_LIST, seq, S_BAD_REQUEST, "short request");
    return;
  }
  const uint32_t start = getLe32(p);
  uint32_t max = getLe16(p + 4);
  if (max == 0 || max > 256) max = 64;
  const uint8_t* path = p + 6;
  const size_t plen = len - 6;
  Json j(body(), bodyCap());

  if (plen == 1 && path[0] == '/') {   // the storages themselves
    j.raw("{\"e\":[");
    bool first = true;
    for (Root r : kRoots) {
      UsbFilesRootInfo info;
      if (!_host->root(r, &info, false)) continue;
      if (!first) j.raw(",");
      first = false;
      j.raw("[");
      j.str(rootName(r));
      j.raw(",1,0,0,0]");
    }
    j.raw("],\"n\":-1}");
    sendReply(T_LIST, seq, S_OK, j.pos);
    return;
  }

  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_LIST, seq, path, plen, true, &info, rel, sizeof rel)) return;
  char vpath[kMaxPath + 1];
  memcpy(vpath, path, plen);
  vpath[plen] = '\0';

  const bool resume = (_ls_dir || _ls_ff) && strcmp(_ls_path, vpath) == 0 && _ls_index == start;
  if (!resume) {
    closeList();
    bool opened = false;
    if (info.fat && info.drive[0]) {
      FF_DIR* d = new (std::nothrow) FF_DIR;
      char ffp[kMaxPath + 8];
      snprintf(ffp, sizeof ffp, "%s%s", info.drive, rel);
      if (d && f_opendir(d, ffp) == FR_OK) {
        _ls_ff = d;
        opened = true;
      } else {
        delete d;
      }
    } else {
      char full[kMaxPath + 24];
      if (fullPath(info, rel, full, sizeof full)) _ls_dir = opendir(full);
      opened = _ls_dir != nullptr;
    }
    if (!opened) {
      bool is_dir = false;
      uint32_t size = 0;
      const bool exists = statRel(info, rel, &is_dir, &size);
      sendError(T_LIST, seq, exists ? S_BAD_PATH : S_NOT_FOUND,
                exists ? "not a folder" : "folder not found");
      return;
    }
    snprintf(_ls_path, sizeof _ls_path, "%s", vpath);
    _ls_index = 0;
    char skip[260];
    bool d = false;
    uint32_t s = 0;
    while (_ls_index < start && readEntry(info, rel, skip, sizeof skip, &d, &s, false)) {}
  }
  if (info.id == ROOT_SD) _host->noteIo(ROOT_SD);

  const bool at_root = rel[0] == '/' && rel[1] == '\0';
  const size_t limit = bodyCap() - 48;   // room for the closing members
  // Flat roots report a folder once per file inside it; drop repeats within a page
  // (the page merges across pages).
  char seen[16][32];
  uint8_t n_seen = 0;
  j.raw("{\"e\":[");
  uint32_t count = 0;
  bool first = true;
  bool more = true;
  int64_t next = -1;
  char name[260];
  while (count < max) {
    bool is_dir = false;
    uint32_t size = 0;
    if (!readEntry(info, rel, name, sizeof name, &is_dir, &size, true)) {
      more = false;
      break;
    }
    if (at_root && strcmp(name, kTempName) == 0) continue;
    if (info.flat && is_dir) {
      bool dup = false;
      for (uint8_t i = 0; i < n_seen && !dup; ++i) dup = strcmp(seen[i], name) == 0;
      if (dup) continue;
      if (n_seen < 16) snprintf(seen[n_seen++], sizeof seen[0], "%s", name);
    }
    char child[kMaxPath + 1];
    const bool writable = childOf(rel, name, child, sizeof child) &&
                          access(info.id, child, info.fat) == A_WRITE;
    const size_t mark = j.pos;
    if (!first) j.raw(",");
    j.raw("[");
    j.str(name);
    j.raw(is_dir ? ",1," : ",0,");
    j.num(size);
    j.raw(writable ? ",1," : ",0,");
    j.num(is_dir ? 0 : SdThreat::byName(name));
    j.raw("]");
    if (j.full || j.pos > limit) {
      // It does not fit: leave it for the next page, which reopens the folder and
      // skips to it, since this handle has already read past it.
      j.pos = mark;
      j.full = false;
      next = static_cast<int64_t>(_ls_index) - 1;
      closeList();
      break;
    }
    first = false;
    ++count;
  }
  if (!more) closeList();
  else if (next < 0) next = _ls_index;
  j.raw("],\"n\":");
  if (!more || next < 0) j.raw("-1");
  else j.num(static_cast<uint64_t>(next));
  j.raw("}");
  sendReply(T_LIST, seq, S_OK, j.pos);
}

void UsbFilesSession::doStat(uint16_t seq, const uint8_t* p, size_t len) {
  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_STAT, seq, p, len, true, &info, rel, sizeof rel)) return;
  bool is_dir = false;
  uint32_t size = 0;
  if (!statRel(info, rel, &is_dir, &size)) {
    sendError(T_STAT, seq, S_NOT_FOUND, "not found");
    return;
  }
  const uint8_t threat = is_dir ? 0 : threatOf(info, rel, size);
  Json j(body(), bodyCap());
  j.raw(is_dir ? "{\"d\":1,\"s\":" : "{\"d\":0,\"s\":");
  j.num(size);
  j.raw(access(info.id, rel, info.fat) == A_WRITE ? ",\"w\":1,\"t\":" : ",\"w\":0,\"t\":");
  j.num(threat);
  j.raw("}");
  sendReply(T_STAT, seq, S_OK, j.pos);
}

void UsbFilesSession::doRead(uint16_t seq, const uint8_t* p, size_t len) {
  if (len < 7) {
    sendError(T_READ, seq, S_BAD_REQUEST, "short request");
    return;
  }
  const uint32_t offset = getLe32(p);
  uint32_t want = getLe16(p + 4);
  if (want == 0 || want > kMaxChunk) want = static_cast<uint32_t>(_chunk);
  const uint8_t* path = p + 6;
  const size_t plen = len - 6;
  char vpath[kMaxPath + 1];
  if (plen > kMaxPath) {
    sendError(T_READ, seq, S_BAD_PATH, "not a valid path");
    return;
  }
  memcpy(vpath, path, plen);
  vpath[plen] = '\0';

  if (!_rd_file || strcmp(_rd_path, vpath) != 0) {
    closeRead();
    UsbFilesRootInfo info;
    char rel[kMaxPath + 1];
    if (!resolve(T_READ, seq, path, plen, true, &info, rel, sizeof rel)) return;
    bool is_dir = false;
    uint32_t size = 0;
    if (!statRel(info, rel, &is_dir, &size)) {
      sendError(T_READ, seq, S_NOT_FOUND, "not found");
      return;
    }
    if (is_dir) {
      sendError(T_READ, seq, S_BAD_PATH, "that is a folder");
      return;
    }
    _rd_file = info.fs->open(rel, FILE_READ);
    if (!_rd_file) {
      _host->noteIoFailure(info.id);
      sendError(T_READ, seq, S_IO, "could not open the file");
      return;
    }
    _rd_info = info;
    snprintf(_rd_path, sizeof _rd_path, "%s", vpath);
    _rd_size = size;
    setName(rel);
  }
  if (offset > _rd_size) {
    sendError(T_READ, seq, S_BAD_OFFSET, "past the end of the file");
    return;
  }
  if (_rd_file.position() != offset && !_rd_file.seek(offset)) {
    closeRead();
    sendError(T_READ, seq, S_IO, "seek failed");
    return;
  }
  uint32_t n = _rd_size - offset;
  if (n > want) n = want;
  const size_t got = n ? _rd_file.read(_bounce, n) : 0;
  if (got != n) {
    _host->noteIoFailure(_rd_info.id);
    closeRead();
    sendError(T_READ, seq, S_IO, "read failed");
    return;
  }
  _host->noteIo(_rd_info.id);
  uint8_t* b = body();
  putLe32(b, offset);
  memcpy(b + 4, _bounce, got);
  _rd_end = offset + got;
  _rd_ms = millis();
  sendReply(T_READ, seq, S_OK, 4 + got);
}

void UsbFilesSession::doWriteBegin(uint16_t seq, const uint8_t* p, size_t len) {
  if (len < 6) {
    sendError(T_WRITE_BEGIN, seq, S_BAD_REQUEST, "short request");
    return;
  }
  const uint32_t size = getLe32(p);
  const uint8_t flags = p[4];
  abortUpload(nullptr);   // a new upload replaces an unfinished one
  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_WRITE_BEGIN, seq, p + 5, len - 5, true, &info, rel, sizeof rel)) return;
  if (access(info.id, rel, info.fat) != A_WRITE) {
    sendError(T_WRITE_BEGIN, seq, S_PROTECTED, "this folder is read-only here");
    return;
  }
  if (size > kMaxUpload) {
    sendError(T_WRITE_BEGIN, seq, S_TOO_BIG, "files are limited to 512 MB");
    return;
  }
  bool is_dir = false;
  uint32_t current = 0;
  const bool exists = statRel(info, rel, &is_dir, &current);
  if (exists && is_dir) {
    sendError(T_WRITE_BEGIN, seq, S_EXISTS, "a folder has that name");
    return;
  }
  if (exists && !(flags & kWriteOverwrite)) {
    sendError(T_WRITE_BEGIN, seq, S_EXISTS, "already exists");
    return;
  }
  if (!info.flat) {
    char parent[kMaxPath + 1];
    parentOf(rel, parent, sizeof parent);
    bool pdir = false;
    uint32_t psize = 0;
    if (!statRel(info, parent, &pdir, &psize) || !pdir) {
      sendError(T_WRITE_BEGIN, seq, S_NOT_FOUND, "folder not found");
      return;
    }
  }
  if (info.reserve) {
    uint64_t total = 0, used = 0;
    if (_host->space(info.id, &total, &used)) {
      uint64_t free_bytes = total > used ? total - used : 0;
      if (exists) free_bytes += current;
      if (static_cast<uint64_t>(size) + info.reserve > free_bytes) {
        sendError(T_WRITE_BEGIN, seq, S_NO_SPACE, "not enough free space");
        return;
      }
    }
  }
  closeRead();   // it may be the file about to be replaced
  closeList();
  {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    bool tdir = false;
    uint32_t tsize = 0;
    if (statRel(info, kTempRel, &tdir, &tsize)) info.fs->remove(kTempRel);
    _up_file = info.fs->open(kTempRel, FILE_WRITE);
  }
  if (!_up_file) {
    _host->noteIoFailure(info.id);
    sendError(T_WRITE_BEGIN, seq, S_IO, "could not create the file");
    return;
  }
  _up = true;
  _up_exists = exists;
  _up_info = info;
  snprintf(_up_rel, sizeof _up_rel, "%s", rel);
  _up_size = size;
  _up_got = 0;
  _up_crc = 0xFFFFFFFFu;
  _up_ms = millis();
  setName(rel);
  _result = RESULT_NONE;
  sendReply(T_WRITE_BEGIN, seq, S_OK, 0);
}

void UsbFilesSession::doWriteData(uint16_t seq, const uint8_t* p, size_t len) {
  if (!_up) {
    sendError(T_WRITE_DATA, seq, S_BAD_REQUEST, "no upload in progress");
    return;
  }
  if (len < 4 || len - 4 > kMaxChunk) {
    sendError(T_WRITE_DATA, seq, S_BAD_REQUEST, "bad chunk");
    return;
  }
  const uint32_t offset = getLe32(p);
  const size_t n = len - 4;
  if (n && offset + n == _up_got) {   // a chunk we already wrote, sent again
    putLe32(body(), _up_got);
    sendReply(T_WRITE_DATA, seq, S_OK, 4);
    return;
  }
  if (offset != _up_got) {
    putLe32(body(), _up_got);   // where the page has to continue
    sendReply(T_WRITE_DATA, seq, S_BAD_OFFSET, 4);
    return;
  }
  if (static_cast<uint64_t>(_up_got) + n > _up_size) {
    abortUpload("more data than announced");
    sendError(T_WRITE_DATA, seq, S_TOO_BIG, "more data than announced");
    return;
  }
  memcpy(_bounce, p + 4, n);
  size_t written = 0;
  if (internalFlash(_up_info)) {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    written = _up_file.write(_bounce, n);
  } else {
    written = _up_file.write(_bounce, n);
  }
  if (written != n) {
    _host->noteIoFailure(_up_info.id);
    abortUpload("write failed");
    sendError(T_WRITE_DATA, seq, S_IO, "write failed (storage full?)");
    return;
  }
  _host->noteIo(_up_info.id);
  _up_crc = WebFileTransferProtocol::crc32Update(_up_crc, _bounce, n);
  _up_got += n;
  _up_ms = millis();
  putLe32(body(), _up_got);
  sendReply(T_WRITE_DATA, seq, S_OK, 4);
}

void UsbFilesSession::doWriteEnd(uint16_t seq, const uint8_t* p, size_t len) {
  if (!_up) {
    sendError(T_WRITE_END, seq, S_BAD_REQUEST, "no upload in progress");
    return;
  }
  if (len < 4) {
    sendError(T_WRITE_END, seq, S_BAD_REQUEST, "short request");
    return;
  }
  if (_up_got != _up_size) {
    putLe32(body(), _up_got);
    sendReply(T_WRITE_END, seq, S_BAD_OFFSET, 4);
    return;
  }
  if ((_up_crc ^ 0xFFFFFFFFu) != getLe32(p)) {
    abortUpload("checksum mismatch");
    sendError(T_WRITE_END, seq, S_BAD_CRC, "checksum mismatch, send it again");
    return;
  }
  fs::FS& fs = *_up_info.fs;
  bool ok = true;
  {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    _up_file.close();
    if (_up_exists) ok = fs.remove(_up_rel);
    if (ok) ok = fs.rename(kTempRel, _up_rel);
    if (!ok) fs.remove(kTempRel);
  }
  _up = false;
  if (!ok) {
    _host->noteIoFailure(_up_info.id);
    sendError(T_WRITE_END, seq, S_IO, "could not save the file");
    return;
  }
  _host->noteChanged(_up_info.id);
  _result = RESULT_RECEIVED;
  sendReply(T_WRITE_END, seq, S_OK, 0);
}

void UsbFilesSession::doRemove(uint16_t seq, const uint8_t* p, size_t len) {
  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_REMOVE, seq, p, len, true, &info, rel, sizeof rel)) return;
  if (access(info.id, rel, info.fat) != A_WRITE) {
    sendError(T_REMOVE, seq, S_PROTECTED, "this is read-only here");
    return;
  }
  bool is_dir = false;
  uint32_t size = 0;
  if (!statRel(info, rel, &is_dir, &size)) {
    sendError(T_REMOVE, seq, S_NOT_FOUND, "not found");
    return;
  }
  closeRead();
  closeList();
  bool ok = true;
  if (is_dir) {
    if (info.flat) {
      if (flatHasChildren(info, rel)) {
        sendError(T_REMOVE, seq, S_NOT_EMPTY, "the folder is not empty");
        return;
      }
    } else {
      char full[kMaxPath + 24];
      if (!fullPath(info, rel, full, sizeof full)) ok = false;
      if (ok) {
        DIR* d = opendir(full);
        bool empty = true;
        if (d) {
          struct dirent* de;
          while ((de = readdir(d)) != nullptr) {
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
              empty = false;
              break;
            }
          }
          closedir(d);
        }
        if (!empty) {
          sendError(T_REMOVE, seq, S_NOT_EMPTY, "the folder is not empty");
          return;
        }
        WdtHeavyGuard heavy;
        LoopWdtGuard loop;
        ok = rmdir(full) == 0;
      }
    }
  } else {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    ok = info.fs->remove(rel);
  }
  if (!ok) {
    _host->noteIoFailure(info.id);
    sendError(T_REMOVE, seq, S_IO, "could not delete it");
    return;
  }
  _host->noteChanged(info.id);
  sendReply(T_REMOVE, seq, S_OK, 0);
}

void UsbFilesSession::doMkdir(uint16_t seq, const uint8_t* p, size_t len) {
  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_MKDIR, seq, p, len, true, &info, rel, sizeof rel)) return;
  if (access(info.id, rel, info.fat) != A_WRITE) {
    sendError(T_MKDIR, seq, S_PROTECTED, "this folder is read-only here");
    return;
  }
  if (info.flat) {   // folders on internal storage exist once a file is inside
    sendReply(T_MKDIR, seq, S_OK, 0);
    return;
  }
  bool is_dir = false;
  uint32_t size = 0;
  if (statRel(info, rel, &is_dir, &size)) {
    if (is_dir) sendReply(T_MKDIR, seq, S_OK, 0);
    else sendError(T_MKDIR, seq, S_EXISTS, "a file has that name");
    return;
  }
  char parent[kMaxPath + 1];
  parentOf(rel, parent, sizeof parent);
  if (!statRel(info, parent, &is_dir, &size) || !is_dir) {
    sendError(T_MKDIR, seq, S_NOT_FOUND, "folder not found");
    return;
  }
  char full[kMaxPath + 24];
  bool ok = fullPath(info, rel, full, sizeof full);
  if (ok) {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    ok = mkdir(full, 0775) == 0;
  }
  if (!ok) {
    _host->noteIoFailure(info.id);
    sendError(T_MKDIR, seq, S_IO, "could not create the folder");
    return;
  }
  _host->noteChanged(info.id);
  sendReply(T_MKDIR, seq, S_OK, 0);
}

void UsbFilesSession::doRename(uint16_t seq, const uint8_t* p, size_t len) {
  if (len < 4) {
    sendError(T_RENAME, seq, S_BAD_REQUEST, "short request");
    return;
  }
  const size_t flen = getLe16(p);
  if (2 + flen >= len) {
    sendError(T_RENAME, seq, S_BAD_REQUEST, "bad request");
    return;
  }
  UsbFilesRootInfo from_info, to_info;
  char from[kMaxPath + 1], to[kMaxPath + 1];
  if (!resolve(T_RENAME, seq, p + 2, flen, true, &from_info, from, sizeof from)) return;
  if (!resolve(T_RENAME, seq, p + 2 + flen, len - 2 - flen, true, &to_info, to, sizeof to)) return;
  if (from_info.id != to_info.id) {
    sendError(T_RENAME, seq, S_UNSUPPORTED, "cannot move between storages");
    return;
  }
  if (access(from_info.id, from, from_info.fat) != A_WRITE ||
      access(to_info.id, to, to_info.fat) != A_WRITE) {
    sendError(T_RENAME, seq, S_PROTECTED, "this is read-only here");
    return;
  }
  bool is_dir = false;
  uint32_t size = 0;
  if (!statRel(from_info, from, &is_dir, &size)) {
    sendError(T_RENAME, seq, S_NOT_FOUND, "not found");
    return;
  }
  if (from_info.flat && is_dir) {
    sendError(T_RENAME, seq, S_UNSUPPORTED, "folders on internal storage cannot be renamed");
    return;
  }
  bool to_dir = false;
  uint32_t to_size = 0;
  if (statRel(to_info, to, &to_dir, &to_size)) {
    sendError(T_RENAME, seq, S_EXISTS, "that name is taken");
    return;
  }
  if (!to_info.flat) {
    char parent[kMaxPath + 1];
    parentOf(to, parent, sizeof parent);
    if (!statRel(to_info, parent, &to_dir, &to_size) || !to_dir) {
      sendError(T_RENAME, seq, S_NOT_FOUND, "folder not found");
      return;
    }
  }
  closeRead();
  closeList();
  char full_from[kMaxPath + 24], full_to[kMaxPath + 24];
  bool ok = fullPath(from_info, from, full_from, sizeof full_from) &&
            fullPath(to_info, to, full_to, sizeof full_to);
  if (ok) {
    WdtHeavyGuard heavy;
    LoopWdtGuard loop;
    ok = rename(full_from, full_to) == 0;
  }
  if (!ok) {
    _host->noteIoFailure(from_info.id);
    sendError(T_RENAME, seq, S_IO, "could not rename it");
    return;
  }
  _host->noteChanged(from_info.id);
  sendReply(T_RENAME, seq, S_OK, 0);
}

void UsbFilesSession::doSpace(uint16_t seq, const uint8_t* p, size_t len) {
  UsbFilesRootInfo info;
  char rel[kMaxPath + 1];
  if (!resolve(T_SPACE, seq, p, len, true, &info, rel, sizeof rel)) return;
  uint64_t total = 0, used = 0;
  if (!_host->space(info.id, &total, &used)) {
    sendError(T_SPACE, seq, S_UNSUPPORTED, "unknown");
    return;
  }
  Json j(body(), bodyCap());
  j.raw("{\"t\":");
  j.num(total);
  j.raw(",\"u\":");
  j.num(used);
  j.raw(",\"r\":");
  j.num(info.reserve);
  j.raw("}");
  sendReply(T_SPACE, seq, S_OK, j.pos);
}

#endif  // CAP_USB_FILES
