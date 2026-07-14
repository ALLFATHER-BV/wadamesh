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
// storage decision): every namespace lives in a flat <dir>/<ns>.kv file on the
// chosen filesystem — SD (/meshcomod) when a card is the active data store, else
// SPIFFS (/prefs). Nothing new is written to NVS; NVS is read-only, used only to
// migrate settings written by an older NVS build (they move to the file on their
// next save). Legacy mode (before useFile, e.g. the early boot-rotation read):
// NVS if it works, else /meshcomod/<ns>.kv on SD — the previous behaviour.
//
// IMPORTANT: the monorepo lib ships a STALE copy of this header that other
// translation units may pick up via an angle include. To avoid an ODR/layout
// mismatch (which caused a free()-of-garbage bootloop), the DATA LAYOUT here must
// stay byte-identical to that stale copy: do NOT add/remove/reorder data members.
// The file backend therefore uses file-static state in the .cpp, not a member.
// Only the layout-neutral static useFile() is new.
//
// On-disk file format is unchanged: [keylen u8][key][vallen u16 LE][val].
class SdNvsPrefs {
public:
  static void useFile(fs::FS* fs, const char* dir);   // route prefs to <dir>/<ns>.kv
  static fs::FS* fileFs();   // the active file-mode fs, or nullptr when NVS-backed

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

private:
  // --- NVS-backed path --- (LAYOUT MUST MATCH the lib's stale header — see above)
  Preferences _nvs;
  bool        _nvs_open = false;
  bool        _read_only = false;

  // --- file-backed path ---
  struct Kv { char key[16]; std::vector<uint8_t> val; };
  std::vector<Kv> _sd;          // in-RAM mirror of <dir>/<ns>.kv
  char            _path[40] = {0};
  bool            _sd_loaded = false;

  bool   useNvs();              // legacy probe (NVS vs SD), cached globally
  Kv*    sdFind(const char* key);
  void   sdSet(const char* key, const uint8_t* data, size_t len);
  void   sdLoad();
  void   sdSave();
  uint64_t sdGetInt(const char* key, uint64_t def, int width);
  size_t sdPutInt(const char* key, uint64_t v, int width);
};

#endif  // ESP32
