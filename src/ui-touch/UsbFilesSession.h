#pragma once
// =============================================================================
// USB Files, device side: serves files.wadamesh.com over the USB serial port
// while the USB Files app is open. Protocol in helpers/esp32/UsbFilesProtocol.h.
//
// The session owns the port for as long as it runs (g_usb_files_owns_serial
// keeps the companion link and the plain-text console off it) and does every
// file operation itself, on the loop task, so the page can only do what the
// access rules allow. Board specifics (which roots exist, mounting the card,
// free space, the SD activity LED) come from a UsbFilesHost the UI provides.
// =============================================================================

#include "device_caps.h"

// True while USB Files owns the USB serial port. Defined in UITask.cpp (built on
// every target) and read by the companion link and MyMesh's console check.
extern volatile bool g_usb_files_owns_serial;

#if CAP_USB_FILES

#include <FS.h>
#include <dirent.h>
#include <new>        // the UI places a session in PSRAM with placement new
#include <stddef.h>
#include <stdint.h>

#include "../helpers/esp32/UsbFilesProtocol.h"

struct UsbFilesRootInfo {
  UsbFiles::Root id = UsbFiles::ROOT_NONE;
  const char* label = "";
  fs::FS* fs = nullptr;     // nullptr: not available right now (no card)
  const char* vfs = "";     // VFS mount point ("/sd"), for stat/opendir/rename
  bool flat = false;        // SPIFFS: no real folders, "a/b" is one file name
  bool fat = false;         // FAT: names ignore case, 8.3 aliases exist
  uint32_t reserve = 0;     // bytes an upload must leave free (0 = no check)
  uint16_t max_rel = 0;     // longest path inside the root (0 = no limit)
  char drive[4] = {0};      // FAT: FatFs drive prefix ("0:"), for fast listings
};

class UsbFilesHost {
 public:
  virtual ~UsbFilesHost() {}
  // Describe root r. mount=true may mount a card first. false: no such root here.
  virtual bool root(UsbFiles::Root r, UsbFilesRootInfo* out, bool mount) = 0;
  // Total and used bytes. false: unknown (or too slow to compute right now).
  virtual bool space(UsbFiles::Root r, uint64_t* total, uint64_t* used) = 0;
  virtual void noteIo(UsbFiles::Root r) = 0;          // activity LED
  virtual void noteIoFailure(UsbFiles::Root r) = 0;   // card may be gone
  virtual void noteChanged(UsbFiles::Root r) = 0;     // something was written/removed
  // JSON members (no braces) describing the device: "name":"..","board":"..","fw":".."
  virtual void describe(char* out, size_t cap) = 0;
};

class UsbFilesSession {
 public:
  enum Activity : uint8_t { WAITING, CONNECTED, RECEIVING, SENDING };
  enum Result : uint8_t { RESULT_NONE, RESULT_RECEIVED, RESULT_STOPPED };

  bool begin(UsbFilesHost* host);   // false: out of memory
  void end();                       // tells the page, drops an unfinished upload
  void tick();                      // call from the UI loop

  bool running() const { return _running; }
  Activity activity() const;
  const char* fileName() const { return _name; }
  uint32_t progressDone() const;
  uint32_t progressTotal() const;
  Result lastResult() const { return _result; }   // about fileName()

 private:
  void pumpRx();
  void handleFrame();
  void sendBeacon(uint8_t type);
  bool sendReply(uint8_t req_type, uint16_t seq, uint8_t status, size_t body_len);
  bool sendError(uint8_t req_type, uint16_t seq, uint8_t status, const char* msg);
  uint8_t* body() { return _tx + UsbFiles::kHeaderBytes + 1; }
  size_t _chunk = UsbFiles::kMaxChunk;   // file bytes per frame; what the receive queue holds
  size_t bodyCap() const { return UsbFiles::kMaxPayload - 1; }

  // Resolve a request path into root + path inside it. Replies with the error
  // itself and returns false when the path is unusable.
  bool resolve(uint8_t type, uint16_t seq, const uint8_t* p, size_t len, bool mount,
               UsbFilesRootInfo* info, char* rel, size_t rel_cap);
  bool fullPath(const UsbFilesRootInfo& info, const char* rel, char* out, size_t cap) const;
  bool statRel(const UsbFilesRootInfo& info, const char* rel, bool* is_dir, uint32_t* size) const;
  bool flatHasChildren(const UsbFilesRootInfo& info, const char* rel) const;
  // SD Scan's rules (SdThreat.h): the name, and for a harmless name the first
  // bytes too, so a Windows program under another name is caught before download.
  uint8_t threatOf(const UsbFilesRootInfo& info, const char* rel, uint32_t size);

  void doHello(uint16_t seq);
  void doList(uint16_t seq, const uint8_t* p, size_t len);
  void doStat(uint16_t seq, const uint8_t* p, size_t len);
  void doRead(uint16_t seq, const uint8_t* p, size_t len);
  void doWriteBegin(uint16_t seq, const uint8_t* p, size_t len);
  void doWriteData(uint16_t seq, const uint8_t* p, size_t len);
  void doWriteEnd(uint16_t seq, const uint8_t* p, size_t len);
  void doRemove(uint16_t seq, const uint8_t* p, size_t len);
  void doMkdir(uint16_t seq, const uint8_t* p, size_t len);
  void doRename(uint16_t seq, const uint8_t* p, size_t len);
  void doSpace(uint16_t seq, const uint8_t* p, size_t len);

  // One raw directory entry from the open listing; false at the end. Flat
  // roots report the first component of "a/b" as folder "a".
  bool readEntry(const UsbFilesRootInfo& info, const char* rel, char* name, size_t cap,
                 bool* is_dir, uint32_t* size, bool want_size);

  void abortUpload(const char* why);
  void closeRead();
  void closeList();
  void release();
  void setName(const char* rel);

  UsbFilesHost* _host = nullptr;
  bool _running = false;

  uint8_t* _rxbuf = nullptr;     // parser storage, 2 frames (PSRAM when there is some)
  uint8_t* _tx = nullptr;        // one encoded frame; kept for retransmits
  uint8_t* _bounce = nullptr;    // DMA-capable internal RAM: all SD data goes through it
  UsbFiles::FrameParser* _parser = nullptr;

  // The last reply, resent as-is when the page repeats a request whose reply got
  // lost, so a repeated delete/rename/finish is not executed twice.
  size_t _tx_len = 0;
  uint8_t _last_type = 0;
  uint16_t _last_seq = 0;

  uint32_t _last_rx_ms = 0;
  uint32_t _last_beacon_ms = 0;
  bool _host_seen = false;

  // Upload in progress (one at a time, into a temp file at the root).
  bool _up = false;
  bool _up_exists = false;       // replacing a file: remove it when the upload completes
  UsbFilesRootInfo _up_info;
  File _up_file;
  char _up_rel[UsbFiles::kMaxPath + 1] = {0};
  uint32_t _up_size = 0;
  uint32_t _up_got = 0;
  uint32_t _up_crc = 0xFFFFFFFFu;
  uint32_t _up_ms = 0;

  // Cached read handle, so a download does not reopen the file per chunk.
  File _rd_file;
  UsbFilesRootInfo _rd_info;
  char _rd_path[UsbFiles::kMaxPath + 1] = {0};   // virtual path
  uint32_t _rd_size = 0;
  uint32_t _rd_end = 0;
  uint32_t _rd_ms = 0;

  // Open directory for paged listings: POSIX, or FatFs on FAT (whose entries
  // carry their size, where a stat per entry would rescan the whole folder).
  DIR* _ls_dir = nullptr;
  void* _ls_ff = nullptr;
  char _ls_path[UsbFiles::kMaxPath + 1] = {0};   // virtual path
  uint32_t _ls_index = 0;

  char _name[72] = {0};
  Result _result = RESULT_NONE;
};

#endif  // CAP_USB_FILES
