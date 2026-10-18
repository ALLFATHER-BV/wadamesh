// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences/NVS over <data>/nvs.bin (see web/compat/Preferences.h).
#include "Preferences.h"
#include "nvs_flash.h"
#include "wada_host.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

namespace {
struct Entry { uint8_t type; std::vector<uint8_t> data; };
using Ns = std::map<std::string, Entry>;
std::recursive_mutex g_mx;
std::map<std::string, Ns> g_db;
bool g_loaded = false;

std::string dbPath() { return wadaHostDataDir() + "/nvs.bin"; }
// File: repeated records of [u8 nslen][ns][u8 keylen][key][u8 type][u32 len][data].
void load() {
  if (g_loaded) return;
  g_loaded = true;
  FILE* f = fopen(dbPath().c_str(), "rb");
  if (!f) return;
  for (;;) {
    uint8_t nl, kl, t; uint32_t len;
    if (fread(&nl, 1, 1, f) != 1) break;
    std::string ns(nl, '\0'); if (fread(&ns[0], 1, nl, f) != nl) break;
    if (fread(&kl, 1, 1, f) != 1) break;
    std::string key(kl, '\0'); if (fread(&key[0], 1, kl, f) != kl) break;
    if (fread(&t, 1, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
    Entry e; e.type = t; e.data.resize(len);
    if (len && fread(e.data.data(), 1, len, f) != len) break;
    g_db[ns][key] = std::move(e);
  }
  fclose(f);
}
void save() {
  const std::string tmp = dbPath() + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  for (auto& n : g_db) for (auto& kv : n.second) {
    uint8_t nl = (uint8_t)n.first.size(), kl = (uint8_t)kv.first.size(), t = kv.second.type;
    uint32_t len = (uint32_t)kv.second.data.size();
    fwrite(&nl, 1, 1, f); fwrite(n.first.data(), 1, nl, f);
    fwrite(&kl, 1, 1, f); fwrite(kv.first.data(), 1, kl, f);
    fwrite(&t, 1, 1, f); fwrite(&len, 4, 1, f);
    if (len) fwrite(kv.second.data.data(), 1, len, f);
  }
  fclose(f);
  rename(tmp.c_str(), dbPath().c_str());
  wadaHostFsTouched();
}
bool keyOk(const char* key) { return key && *key && strlen(key) <= 15; }   // NVS_KEY_NAME_MAX_SIZE-1
}  // namespace

Preferences::Preferences() : _handle(0), _started(false), _readOnly(false) { _ns[0] = 0; }
Preferences::~Preferences() { end(); }
bool Preferences::begin(const char* name, bool readOnly, const char*) {
  if (_started) return false;
  if (!name || strlen(name) > 15) return false;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  load();
  strncpy(_ns, name, sizeof _ns - 1); _ns[sizeof _ns - 1] = 0;
  if (!readOnly) g_db[_ns];   // NVS creates the namespace on a read-write open
  else if (!g_db.count(_ns)) return false;   // ...and a read-only open of a missing one fails
  _readOnly = readOnly; _started = true; _handle = 1;
  return true;
}
void Preferences::end() { _started = false; }
bool Preferences::clear() {
  if (!_started || _readOnly) return false;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  g_db[_ns].clear(); save();
  return true;
}
bool Preferences::remove(const char* key) {
  if (!_started || _readOnly || !keyOk(key)) return false;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  bool had = g_db[_ns].erase(key) > 0; if (had) save();
  return had;
}

#define WADA_PUT(T, PT, name) size_t Preferences::name(const char* key, T v) { \
    if (!_started || _readOnly || !keyOk(key)) return 0; \
    std::lock_guard<std::recursive_mutex> lk(g_mx); \
    Entry e; e.type = PT; e.data.assign((const uint8_t*)&v, (const uint8_t*)&v + sizeof v); \
    g_db[_ns][key] = std::move(e); save(); return sizeof v; }
WADA_PUT(int8_t, PT_I8, putChar)
WADA_PUT(uint8_t, PT_U8, putUChar)
WADA_PUT(int16_t, PT_I16, putShort)
WADA_PUT(uint16_t, PT_U16, putUShort)
WADA_PUT(int32_t, PT_I32, putInt)
WADA_PUT(uint32_t, PT_U32, putUInt)
WADA_PUT(int32_t, PT_I32, putLong)
WADA_PUT(uint32_t, PT_U32, putULong)
WADA_PUT(int64_t, PT_I64, putLong64)
WADA_PUT(uint64_t, PT_U64, putULong64)
#undef WADA_PUT
size_t Preferences::putFloat(const char* key, float_t v) { return putBytes(key, &v, sizeof v); }
size_t Preferences::putDouble(const char* key, double_t v) { return putBytes(key, &v, sizeof v); }
size_t Preferences::putBool(const char* key, bool v) { return putUChar(key, v ? 1 : 0); }
size_t Preferences::putString(const char* key, const char* v) {
  if (!_started || _readOnly || !keyOk(key) || !v) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  Entry e; e.type = PT_STR; e.data.assign((const uint8_t*)v, (const uint8_t*)v + strlen(v) + 1);
  g_db[_ns][key] = std::move(e); save();
  return strlen(v);
}
size_t Preferences::putString(const char* key, String v) { return putString(key, v.c_str()); }
size_t Preferences::putBytes(const char* key, const void* v, size_t len) {
  if (!_started || _readOnly || !keyOk(key) || (!v && len)) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  Entry e; e.type = PT_BLOB; e.data.assign((const uint8_t*)v, (const uint8_t*)v + len);
  g_db[_ns][key] = std::move(e); save();
  return len;
}
bool Preferences::isKey(const char* key) {
  if (!_started || !keyOk(key)) return false;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  return g_db[_ns].count(key) > 0;
}
PreferenceType Preferences::getType(const char* key) {
  if (!_started || !keyOk(key)) return PT_INVALID;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  auto& ns = g_db[_ns]; auto it = ns.find(key);
  return it == ns.end() ? PT_INVALID : (PreferenceType)it->second.type;
}
template <typename T> static T getScalar(const char* ns_name, bool started, const char* key, uint8_t type, T def) {
  if (!started || !keyOk(key)) return def;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  auto& ns = g_db[ns_name]; auto it = ns.find(key);
  if (it == ns.end() || it->second.type != type || it->second.data.size() != sizeof(T)) return def;
  T v; memcpy(&v, it->second.data.data(), sizeof v); return v;
}
int8_t Preferences::getChar(const char* k, int8_t d) { return getScalar<int8_t>(_ns, _started, k, PT_I8, d); }
uint8_t Preferences::getUChar(const char* k, uint8_t d) { return getScalar<uint8_t>(_ns, _started, k, PT_U8, d); }
int16_t Preferences::getShort(const char* k, int16_t d) { return getScalar<int16_t>(_ns, _started, k, PT_I16, d); }
uint16_t Preferences::getUShort(const char* k, uint16_t d) { return getScalar<uint16_t>(_ns, _started, k, PT_U16, d); }
int32_t Preferences::getInt(const char* k, int32_t d) { return getScalar<int32_t>(_ns, _started, k, PT_I32, d); }
uint32_t Preferences::getUInt(const char* k, uint32_t d) { return getScalar<uint32_t>(_ns, _started, k, PT_U32, d); }
int32_t Preferences::getLong(const char* k, int32_t d) { return getInt(k, d); }
uint32_t Preferences::getULong(const char* k, uint32_t d) { return getUInt(k, d); }
int64_t Preferences::getLong64(const char* k, int64_t d) { return getScalar<int64_t>(_ns, _started, k, PT_I64, d); }
uint64_t Preferences::getULong64(const char* k, uint64_t d) { return getScalar<uint64_t>(_ns, _started, k, PT_U64, d); }
float_t Preferences::getFloat(const char* k, float_t d) { float_t v = d; return getBytes(k, &v, sizeof v) == sizeof v ? v : d; }
double_t Preferences::getDouble(const char* k, double_t d) { double_t v = d; return getBytes(k, &v, sizeof v) == sizeof v ? v : d; }
bool Preferences::getBool(const char* k, bool d) { return getUChar(k, d ? 1 : 0) == 1; }
size_t Preferences::getString(const char* key, char* value, size_t maxLen) {
  if (!_started || !keyOk(key)) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  auto& ns = g_db[_ns]; auto it = ns.find(key);
  if (it == ns.end() || it->second.type != PT_STR) return 0;
  const size_t len = it->second.data.size();   // includes the NUL, like nvs_get_str
  if (!value) return len;
  if (len > maxLen) return 0;
  memcpy(value, it->second.data.data(), len);
  return len;
}
String Preferences::getString(const char* key, String def) {
  size_t len = getString(key, nullptr, 0);
  if (!len) return def;
  std::vector<char> buf(len);
  getString(key, buf.data(), len);
  return String(buf.data());
}
size_t Preferences::getBytesLength(const char* key) {
  if (!_started || !keyOk(key)) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  auto& ns = g_db[_ns]; auto it = ns.find(key);
  return (it == ns.end() || it->second.type != PT_BLOB) ? 0 : it->second.data.size();
}
size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
  if (!_started || !keyOk(key)) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  auto& ns = g_db[_ns]; auto it = ns.find(key);
  if (it == ns.end() || it->second.type != PT_BLOB) return 0;
  const size_t len = it->second.data.size();
  if (!buf) return len;
  if (len > maxLen) return 0;
  memcpy(buf, it->second.data.data(), len);
  return len;
}
size_t Preferences::freeEntries() { return 500; }

extern "C" esp_err_t nvs_get_stats(const char*, nvs_stats_t* s) {
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  load();
  size_t used = 0; for (auto& n : g_db) used += n.second.size();
  s->used_entries = used; s->total_entries = 630; s->free_entries = used < 630 ? 630 - used : 0;
  s->namespace_count = g_db.size();
  return ESP_OK;
}
extern "C" esp_err_t nvs_flash_init(void) { return ESP_OK; }
extern "C" esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  load();
  if (!name) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READONLY && !g_db.count(name)) return ESP_ERR_NVS_NOT_FOUND;
  if (mode != NVS_READONLY) g_db[name];
  if (out) *out = 1;
  return ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t) {}
extern "C" esp_err_t nvs_flash_erase(void) {
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  g_db.clear(); g_loaded = true; save();
  return ESP_OK;
}
