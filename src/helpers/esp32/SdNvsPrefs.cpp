#include "SdNvsPrefs.h"
#if defined(ESP32)

#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <string.h>

// ----------------------------- backend selection -----------------------------
// File mode (set by SdNvsPrefs::useFile at boot): every namespace lives in a flat
// <dir>/<ns>.kv file on the chosen filesystem; NVS is read-only (migration only).
// Legacy mode (before useFile): NVS if it works, else /meshcomod/<ns>.kv on SD.
// All backend state is file-static so the class layout stays identical to the
// monorepo lib's stale header (see SdNvsPrefs.h — mismatching it bootloops).
static fs::FS* s_file_fs   = nullptr;
static char    s_file_dir[24] = {0};
static bool    s_file_mode = false;
static int     s_legacy    = -1;   // legacy probe cache: -1 undecided, 0 SD, 1 NVS
static int     s_migrate   = -1;   // -1 undecided, 0 = no NVS legacy data, 1 = yes

void SdNvsPrefs::useFile(fs::FS* fs, const char* dir) {
  if (!fs) return;
  s_file_fs = fs;
  strncpy(s_file_dir, (dir && dir[0]) ? dir : "/prefs", sizeof(s_file_dir) - 1);
  s_file_dir[sizeof(s_file_dir) - 1] = '\0';
  s_file_mode = true;
  Serial.printf("[PREFS] backend = FILE %s\n", s_file_dir);
}

static bool     fileMode()  { return s_file_mode && s_file_fs; }
static fs::FS*  activeFs()  { return fileMode() ? s_file_fs : (fs::FS*)&SD; }

fs::FS* SdNvsPrefs::fileFs() { return fileMode() ? s_file_fs : nullptr; }

// Does NVS hold settings from an older (NVS) build worth migrating? Probed ONCE
// (RO open of the main namespace) so fresh devices don't spam NOT_FOUND when the
// per-namespace migration fallback opens.
static bool nvsHasLegacyData() {
  if (s_migrate < 0) {
    Preferences p;
    bool ok = p.begin("touch", true);   // TOUCH_NS, read-only
    if (ok) p.end();
    s_migrate = ok ? 1 : 0;
  }
  return s_migrate == 1;
}

bool SdNvsPrefs::useNvs() {
  if (s_legacy < 0) {
    Preferences probe;
    bool ok = false;
    if (probe.begin("mc_kvprobe", false)) { ok = (probe.putUChar("v", 1) > 0); probe.end(); }
    s_legacy = ok ? 1 : 0;
    Serial.printf("[PREFS] legacy backend = %s\n", ok ? "NVS" : "SD /meshcomod");
  }
  return s_legacy == 1;
}

// ----------------------------- begin / end -----------------------------
bool SdNvsPrefs::begin(const char* ns, bool readOnly) {
  _read_only = readOnly;
  _sd.clear();
  _sd_loaded = false;
  if (_nvs_open) { _nvs.end(); _nvs_open = false; }

  if (fileMode()) {
    snprintf(_path, sizeof(_path), "%s/%s.kv", s_file_dir, ns);
    sdLoad();
    _sd_loaded = true;
    if (nvsHasLegacyData()) _nvs_open = _nvs.begin(ns, true);   // RO migration source
    return true;
  }

  if (useNvs()) { _nvs_open = _nvs.begin(ns, readOnly); return _nvs_open; }
  snprintf(_path, sizeof(_path), "/meshcomod/%s.kv", ns);
  sdLoad();
  _sd_loaded = true;
  return true;
}

void SdNvsPrefs::end() {
  if (_nvs_open) { _nvs.end(); _nvs_open = false; }
}

// ----------------------------- file helpers -----------------------------
SdNvsPrefs::Kv* SdNvsPrefs::sdFind(const char* key) {
  for (auto& e : _sd) if (strncmp(e.key, key, sizeof(e.key)) == 0) return &e;
  return nullptr;
}

void SdNvsPrefs::sdSet(const char* key, const uint8_t* data, size_t len) {
  Kv* e = sdFind(key);
  if (!e) {
    _sd.push_back(Kv{});
    e = &_sd.back();
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = '\0';
  }
  e->val.assign(data, data + len);
  sdSave();
}

void SdNvsPrefs::sdLoad() {
  _sd.clear();
  fs::FS* fs = activeFs();
  if (!fs) return;
  File f = fs->open(_path, FILE_READ);
  if (!f) return;
  // record = [keylen u8][key bytes][vallen u16 LE][val bytes]. Sanity caps stop a
  // corrupt file from a huge alloc; on any bad record we stop and keep what loaded.
  while (f.available() > 0 && _sd.size() < 256) {
    int kl = f.read();
    if (kl <= 0 || kl > 15) break;
    char k[16] = {0};
    if (f.read((uint8_t*)k, kl) != kl) break;
    int lo = f.read(), hi = f.read();
    if (lo < 0 || hi < 0) break;
    size_t vl = (size_t)lo | ((size_t)hi << 8);
    if (vl > 2048) break;
    std::vector<uint8_t> v(vl);
    if (vl && f.read(v.data(), vl) != (int)vl) break;
    Kv e; strncpy(e.key, k, sizeof(e.key) - 1); e.key[sizeof(e.key) - 1] = '\0';
    e.val = std::move(v);
    _sd.push_back(std::move(e));
  }
  f.close();
}

void SdNvsPrefs::sdSave() {
  if (_read_only) return;
  fs::FS* fs = activeFs();
  if (!fs) return;
  // Create the parent dir (SD needs it; SPIFFS is flat and treats the whole path
  // as a filename, so mkdir is a harmless no-op there).
  char dir[40];
  strncpy(dir, _path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
  char* slash = strrchr(dir, '/');
  if (slash && slash != dir) { *slash = '\0'; fs->mkdir(dir); }
  File f = fs->open(_path, FILE_WRITE);   // truncate + rewrite the whole file
  if (!f) return;
  for (auto& e : _sd) {
    size_t kl = strnlen(e.key, sizeof(e.key));
    size_t vl = e.val.size();
    f.write((uint8_t)kl);
    f.write((const uint8_t*)e.key, kl);
    f.write((uint8_t)(vl & 0xFF));
    f.write((uint8_t)((vl >> 8) & 0xFF));
    if (vl) f.write(e.val.data(), vl);
  }
  f.close();
}

uint64_t SdNvsPrefs::sdGetInt(const char* key, uint64_t def, int width) {
  Kv* e = sdFind(key);
  if (!e || e->val.empty()) return def;
  uint64_t v = 0;
  int n = (int)e->val.size(); if (n > width) n = width;
  for (int i = 0; i < n; i++) v |= (uint64_t)e->val[i] << (8 * i);
  return v;
}

size_t SdNvsPrefs::sdPutInt(const char* key, uint64_t v, int width) {
  uint8_t b[8];
  for (int i = 0; i < width; i++) b[i] = (uint8_t)(v >> (8 * i));
  sdSet(key, b, width);
  return width;
}

// ----------------------------- API -----------------------------
// File mode: read file first, fall back to the legacy NVS value (read-only) so
// old settings still load and migrate to the file on the next save. Putters only
// ever touch the file — nothing new is written to NVS.

bool SdNvsPrefs::isKey(const char* key) {
  if (fileMode()) return sdFind(key) != nullptr || (_nvs_open && _nvs.isKey(key));
  return useNvs() ? (_nvs_open && _nvs.isKey(key)) : (sdFind(key) != nullptr);
}

bool SdNvsPrefs::remove(const char* key) {
  if (!fileMode() && useNvs()) return _nvs_open && _nvs.remove(key);
  for (size_t i = 0; i < _sd.size(); i++)
    if (strncmp(_sd[i].key, key, sizeof(_sd[i].key)) == 0) { _sd.erase(_sd.begin() + i); sdSave(); return true; }
  return false;
}

bool SdNvsPrefs::clear() {
  if (fileMode()) {
    _sd.clear();
    sdSave();
    // Wipe any legacy NVS copy too, so a factory reset is permanent. Derive the
    // namespace from "<dir>/<ns>.kv".
    if (nvsHasLegacyData()) {
      char ns[16] = {0};
      const char* slash = strrchr(_path, '/');
      const char* base  = slash ? slash + 1 : _path;
      size_t n = 0; while (base[n] && base[n] != '.' && n < sizeof(ns) - 1) { ns[n] = base[n]; n++; }
      ns[n] = '\0';
      if (_nvs_open) { _nvs.end(); _nvs_open = false; }
      Preferences p; if (p.begin(ns, false)) { p.clear(); p.end(); }
    }
    return true;
  }
  if (useNvs()) return _nvs_open && _nvs.clear();
  _sd.clear(); sdSave(); return true;
}

uint8_t SdNvsPrefs::getUChar(const char* k, uint8_t def) {
  if (fileMode()) {
    if (sdFind(k)) return (uint8_t)sdGetInt(k, def, 1);
    if (_nvs_open && _nvs.isKey(k)) return _nvs.getUChar(k, def);
    return def;
  }
  return useNvs() ? _nvs.getUChar(k, def) : (uint8_t)sdGetInt(k, def, 1);
}
size_t SdNvsPrefs::putUChar(const char* k, uint8_t v) {
  if (fileMode()) return sdPutInt(k, v, 1);
  return useNvs() ? _nvs.putUChar(k, v) : sdPutInt(k, v, 1);
}
int8_t SdNvsPrefs::getChar(const char* k, int8_t def) {
  if (fileMode()) {
    if (sdFind(k)) return (int8_t)sdGetInt(k, (uint8_t)def, 1);
    if (_nvs_open && _nvs.isKey(k)) return _nvs.getChar(k, def);
    return def;
  }
  return useNvs() ? _nvs.getChar(k, def) : (int8_t)sdGetInt(k, (uint8_t)def, 1);
}
size_t SdNvsPrefs::putChar(const char* k, int8_t v) {
  if (fileMode()) return sdPutInt(k, (uint8_t)v, 1);
  return useNvs() ? _nvs.putChar(k, v) : sdPutInt(k, (uint8_t)v, 1);
}
uint16_t SdNvsPrefs::getUShort(const char* k, uint16_t def) {
  if (fileMode()) {
    if (sdFind(k)) return (uint16_t)sdGetInt(k, def, 2);
    if (_nvs_open && _nvs.isKey(k)) return _nvs.getUShort(k, def);
    return def;
  }
  return useNvs() ? _nvs.getUShort(k, def) : (uint16_t)sdGetInt(k, def, 2);
}
size_t SdNvsPrefs::putUShort(const char* k, uint16_t v) {
  if (fileMode()) return sdPutInt(k, v, 2);
  return useNvs() ? _nvs.putUShort(k, v) : sdPutInt(k, v, 2);
}
uint32_t SdNvsPrefs::getUInt(const char* k, uint32_t def) {
  if (fileMode()) {
    if (sdFind(k)) return (uint32_t)sdGetInt(k, def, 4);
    if (_nvs_open && _nvs.isKey(k)) return _nvs.getUInt(k, def);
    return def;
  }
  return useNvs() ? _nvs.getUInt(k, def) : (uint32_t)sdGetInt(k, def, 4);
}
size_t SdNvsPrefs::putUInt(const char* k, uint32_t v) {
  if (fileMode()) return sdPutInt(k, v, 4);
  return useNvs() ? _nvs.putUInt(k, v) : sdPutInt(k, v, 4);
}
bool SdNvsPrefs::getBool(const char* k, bool def) {
  if (fileMode()) {
    if (sdFind(k)) return sdGetInt(k, def ? 1 : 0, 1) != 0;
    if (_nvs_open && _nvs.isKey(k)) return _nvs.getBool(k, def);
    return def;
  }
  return useNvs() ? _nvs.getBool(k, def) : (sdGetInt(k, def ? 1 : 0, 1) != 0);
}
size_t SdNvsPrefs::putBool(const char* k, bool v) {
  if (fileMode()) return sdPutInt(k, v ? 1 : 0, 1);
  return useNvs() ? _nvs.putBool(k, v) : sdPutInt(k, v ? 1 : 0, 1);
}

String SdNvsPrefs::getString(const char* k, const String& def) {
  Kv* e = nullptr;
  if (fileMode()) {
    e = sdFind(k);
    if (!e) return (_nvs_open && _nvs.isKey(k)) ? _nvs.getString(k, def) : def;
  } else {
    if (useNvs()) return _nvs.getString(k, def);
    e = sdFind(k);
    if (!e) return def;
  }
  String s; s.reserve(e->val.size());
  for (uint8_t c : e->val) s += (char)c;
  return s;
}
size_t SdNvsPrefs::getString(const char* k, char* buf, size_t maxLen) {
  if (!buf || !maxLen) return 0;
  Kv* e = nullptr;
  if (fileMode()) {
    e = sdFind(k);
    if (!e) { if (_nvs_open && _nvs.isKey(k)) return _nvs.getString(k, buf, maxLen); buf[0] = '\0'; return 0; }
  } else {
    if (useNvs()) return _nvs.getString(k, buf, maxLen);
    e = sdFind(k);
  }
  size_t n = e ? e->val.size() : 0;
  if (n > maxLen - 1) n = maxLen - 1;
  if (e && n) memcpy(buf, e->val.data(), n);
  buf[n] = '\0';
  return n;
}
size_t SdNvsPrefs::putString(const char* k, const char* v) {
  if (fileMode()) { size_t n = v ? strlen(v) : 0; sdSet(k, (const uint8_t*)v, n); return n; }
  if (useNvs()) return _nvs.putString(k, v);
  size_t n = v ? strlen(v) : 0; sdSet(k, (const uint8_t*)v, n); return n;
}

size_t SdNvsPrefs::getBytes(const char* k, void* buf, size_t maxLen) {
  Kv* e = nullptr;
  if (fileMode()) {
    e = sdFind(k);
    if (!e) return (_nvs_open && _nvs.isKey(k)) ? _nvs.getBytes(k, buf, maxLen) : 0;
  } else {
    if (useNvs()) return _nvs.getBytes(k, buf, maxLen);
    e = sdFind(k);
    if (!e) return 0;
  }
  size_t n = e->val.size();
  if (buf && maxLen) { size_t c = n > maxLen ? maxLen : n; memcpy(buf, e->val.data(), c); }
  return n;
}
size_t SdNvsPrefs::putBytes(const char* k, const void* buf, size_t len) {
  if (fileMode()) { sdSet(k, (const uint8_t*)buf, len); return len; }
  if (useNvs()) return _nvs.putBytes(k, buf, len);
  sdSet(k, (const uint8_t*)buf, len); return len;
}

#endif  // ESP32
