#pragma once
#if defined(ESP32)

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

namespace fs { class FS; }   // forward decl only — keep this header's layout stable

// Drop-in, Preferences-compatible key/value store that survives Launcher and can
// keep the touch app's settings OFF NVS.
//
// File mode (set by SdNvsPrefs::useFile() once at boot, after the SD/SPIFFS
// storage decision): every namespace is held in RAM and committed to alternating
// <dir>/<ns>.kv / .alt snapshots. Setters only dirty the RAM copy; a worker writes
// the inactive slot after a short quiet period, keeping filesystem latency off
// the UI task. Each snapshot has a generation and CRC, so a reset can only lose
// the newest pending update — it cannot turn an interrupted write into defaults.
// Legacy <dir>/<ns>.kv and NVS data are read for migration.
//
// IMPORTANT: the monorepo lib ships a STALE copy of this header that other
// translation units may pick up via an angle include. To avoid an ODR/layout
// mismatch (which caused a free()-of-garbage bootloop), the DATA LAYOUT here must
// stay byte-identical to that stale copy: do NOT add/remove/reorder data members.
// The file backend therefore uses file-static state in the .cpp, not a member.
// Only the layout-neutral static useFile() is new.
//
class SdNvsPrefs {
public:
  static void useFile(fs::FS* fs, const char* dir);   // route prefs to <dir>/<ns>.kv
  static fs::FS* fileFs();   // the active file-mode fs, or nullptr when NVS-backed
  static void tick(uint32_t now_ms = 0);              // schedule due snapshots; never blocks on I/O
  static bool flush(uint32_t timeout_ms = 12000);     // force all queued snapshots before reset/sleep
  static bool busy();                                  // a worker currently owns a filesystem handle
  static bool slowIoOverlap(uint32_t start_ms, uint32_t end_ms,
                            const char*& phase, uint32_t& elapsed_ms);

  // Early boot needs `use_sd` before the regular backend is selected. These
  // helpers use the same A/B format on an explicitly supplied filesystem.
  static bool readFileBool(fs::FS* fs, const char* dir, const char* ns,
                           const char* key, bool& value);
  static bool writeFileBool(fs::FS* fs, const char* dir, const char* ns,
                            const char* key, bool value);

  bool begin(const char* ns, bool readOnly = false);
  void end();

  bool     isKey(const char* key);
  bool     remove(const char* key);
  bool     clear();

  uint8_t  getUChar (const char* key, uint8_t  def = 0);
  size_t   putUChar (const char* key, uint8_t  v);
  int8_t   getChar  (const char* key, int8_t   def = 0);
  size_t   putChar  (const char* key, int8_t   v);
  uint16_t getUShort(const char* key, uint16_t def = 0);
  size_t   putUShort(const char* key, uint16_t v);
  uint32_t getUInt  (const char* key, uint32_t def = 0);
  size_t   putUInt  (const char* key, uint32_t v);
  bool     getBool  (const char* key, bool     def = false);
  size_t   putBool  (const char* key, bool     v);

  String   getString(const char* key, const String& def = String());
  size_t   getString(const char* key, char* buf, size_t maxLen);   // char* overload
  size_t   putString(const char* key, const char* v);
  size_t   putString(const char* key, const String& v) { return putString(key, v.c_str()); }

  size_t   getBytes(const char* key, void* buf, size_t maxLen);
  size_t   putBytes(const char* key, const void* buf, size_t len);

  // Public only so the file-static snapshot worker can preserve this class's
  // ABI while sharing namespace caches across short-lived instances.
  struct Kv { char key[16]; std::vector<uint8_t> val; };

private:
  // --- NVS-backed path --- (LAYOUT MUST MATCH the lib's stale header — see above)
  Preferences _nvs;
  bool        _nvs_open = false;
  bool        _read_only = false;

  // --- file-backed path ---
  std::vector<Kv> _sd;          // in-RAM mirror of <dir>/<ns>.kv
  char            _path[40] = {0};
  bool            _sd_loaded = false;

  bool   useNvs();              // legacy probe (NVS vs SD), cached globally
  Kv*    sdFind(const char* key);
  bool   sdSet(const char* key, const uint8_t* data, size_t len);
  bool   sdLoad();
  bool   sdSave();              // queue current RAM mirror (no filesystem I/O)
  uint64_t sdGetInt(const char* key, uint64_t def, int width, bool* found = nullptr);
  size_t sdPutInt(const char* key, uint64_t v, int width);
};

#endif  // ESP32
