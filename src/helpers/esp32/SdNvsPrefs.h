#pragma once
#if defined(ESP32)

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <vector>

// Drop-in, Preferences-compatible key/value store that survives Launcher and
// keeps the touch app's settings OFF NVS.
//
// Preferred mode ("file"): the app calls SdNvsPrefs::useFile() once at boot,
// after the SD/SPIFFS storage decision, to route every namespace to a flat
// <dir>/<ns>.kv file on that filesystem — SD (/meshcomod) when a card is the
// active data store, else SPIFFS (/prefs). Nothing new is ever written to NVS in
// this mode; NVS is opened READ-ONLY only as a migration source, so settings
// written by an older NVS build still load and move to the file on their next
// save. This makes the app independent of the tiny, shared 20 KB NVS partition
// (whose exhaustion under Launcher is what triggered the boot loop).
//
// Legacy mode (until useFile() is called — e.g. the very early boot-rotation read
// before the filesystems are mounted): NVS if it works, else a /meshcomod/<ns>.kv
// file on SD. Identical to the previous behaviour, so the device stays
// byte-compatible if useFile() is somehow never reached.
//
// On-disk file format is unchanged: [keylen u8][key][vallen u16 LE][val], so
// existing /meshcomod/<ns>.kv files from earlier builds keep working.
class SdNvsPrefs {
public:
  // Route all prefs to <dir>/<ns>.kv on `fs` (SD or SPIFFS). Call once at boot.
  static void useFile(fs::FS* fs, const char* dir);

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
  Preferences _nvs;
  bool        _nvs_open  = false;   // file mode: opened RO for migration; legacy NVS: RW
  bool        _read_only = false;

  // File-backed path (SD or SPIFFS). _fs is the chosen filesystem.
  fs::FS*         _fs = nullptr;
  struct Kv { char key[16]; std::vector<uint8_t> val; };
  std::vector<Kv> _sd;          // in-RAM mirror of <dir>/<ns>.kv
  char            _path[48] = {0};
  bool            _sd_loaded = false;

  static bool legacyUseNvs();   // legacy probe (NVS vs SD), cached globally
  bool   fileMode() const;      // true once useFile() picked a file backend
  Kv*    sdFind(const char* key);
  void   sdSet(const char* key, const uint8_t* data, size_t len);
  void   sdLoad();
  void   sdSave();
  uint64_t sdGetInt(const char* key, uint64_t def, int width);
  size_t sdPutInt(const char* key, uint64_t v, int width);
};

#endif  // ESP32
