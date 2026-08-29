#include "SdNvsPrefs.h"
#if defined(ESP32)

#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>
#include <stddef.h>
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

static constexpr uint32_t PREFS_MAGIC       = 0x32504657u;  // "WFP2"
static constexpr uint16_t PREFS_FORMAT      = 1;
static constexpr uint32_t PREFS_QUIET_MS    = 750;
static constexpr uint32_t PREFS_MAX_WAIT_MS = 5000;
static constexpr uint32_t PREFS_SLOW_IO_MS  = 200;
// These are settings namespaces, not a general object store. A hard ceiling
// bounds the RAM snapshot + worker copy on boards without PSRAM; current touch
// and discovered payloads are both well below 16 KB.
static constexpr size_t   PREFS_MAX_PAYLOAD = 64u * 1024u;

struct __attribute__((packed)) SnapshotFooter {
  uint32_t magic;
  uint16_t format;
  uint16_t footer_size;
  uint32_t generation;
  uint32_t payload_size;
  uint32_t payload_crc;
  uint32_t footer_crc;
};

struct FileCache {
  fs::FS* fs = nullptr;
  char legacy_path[40] = {0};
  std::vector<SdNvsPrefs::Kv> kv;
  uint32_t generation = 0;
  uint32_t revision = 0;
  uint32_t dirty_first_ms = 0;
  uint32_t dirty_last_ms = 0;
  uint32_t retry_after_ms = 0;
  char active_slot = 0;
  bool loaded = false;
  bool dirty = false;
  bool writing = false;
  bool legacy_present = false;
};

struct WriteJob {
  fs::FS* fs = nullptr;
  char legacy_path[40] = {0};
  std::vector<SdNvsPrefs::Kv> kv;
  uint32_t generation = 0;
  uint32_t revision = 0;
  char slot = 'a';
};

static std::vector<FileCache> s_caches;
static SemaphoreHandle_t s_cache_mutex = nullptr;
static QueueHandle_t s_write_queue = nullptr;
static TaskHandle_t s_write_task = nullptr;

struct PrefsIoTrace {
  const char* active_phase = nullptr;
  uint32_t active_started_ms = 0;
  const char* last_slow_phase = nullptr;
  uint32_t last_slow_started_ms = 0;
  uint32_t last_slow_ended_ms = 0;
  uint32_t last_slow_elapsed_ms = 0;
};

static portMUX_TYPE s_io_trace_mux = portMUX_INITIALIZER_UNLOCKED;
static PrefsIoTrace s_io_trace;

static bool msIntervalsOverlap(uint32_t lhs_start, uint32_t lhs_end,
                               uint32_t rhs_start, uint32_t rhs_end) {
  return (int32_t)(lhs_end - rhs_start) >= 0 && (int32_t)(rhs_end - lhs_start) >= 0;
}

class PrefsIoPhase {
public:
  PrefsIoPhase(bool enabled, const char* phase, const char* path, char slot)
      : _enabled(enabled), _phase(phase), _path(path), _slot(slot) {
    if (!_enabled) return;
    _started_ms = millis();
    portENTER_CRITICAL(&s_io_trace_mux);
    s_io_trace.active_phase = _phase;
    s_io_trace.active_started_ms = _started_ms;
    portEXIT_CRITICAL(&s_io_trace_mux);
  }

  ~PrefsIoPhase() {
    if (!_enabled) return;
    const uint32_t ended_ms = millis();
    const uint32_t elapsed_ms = ended_ms - _started_ms;
    portENTER_CRITICAL(&s_io_trace_mux);
    if (s_io_trace.active_phase == _phase &&
        s_io_trace.active_started_ms == _started_ms) {
      s_io_trace.active_phase = nullptr;
      s_io_trace.active_started_ms = 0;
    }
    if (elapsed_ms >= PREFS_SLOW_IO_MS) {
      s_io_trace.last_slow_phase = _phase;
      s_io_trace.last_slow_started_ms = _started_ms;
      s_io_trace.last_slow_ended_ms = ended_ms;
      s_io_trace.last_slow_elapsed_ms = elapsed_ms;
    }
    portEXIT_CRITICAL(&s_io_trace_mux);
    if (elapsed_ms >= PREFS_SLOW_IO_MS)
      Serial.printf("[PREFS] slow FATFS %s %s slot %c: %lums\n", _phase, _path, _slot,
                    (unsigned long)elapsed_ms);
  }

  PrefsIoPhase(const PrefsIoPhase&) = delete;
  PrefsIoPhase& operator=(const PrefsIoPhase&) = delete;

private:
  bool _enabled;
  const char* _phase;
  const char* _path;
  char _slot;
  uint32_t _started_ms = 0;
};

static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

static uint32_t crc32(const void* data, size_t len) {
  return crc32Update(0, static_cast<const uint8_t*>(data), len);
}

static bool slotPath(const char* legacy_path, char slot, char* out, size_t cap) {
  if (!legacy_path || !out || cap == 0) return false;
  if (slot == 'a') return snprintf(out, cap, "%s", legacy_path) < (int)cap;
  const size_t n = strlen(legacy_path);
  if (n >= 3 && strcmp(legacy_path + n - 3, ".kv") == 0)
    return snprintf(out, cap, "%.*s.alt", (int)(n - 3), legacy_path) < (int)cap;
  return snprintf(out, cap, "%s.alt", legacy_path) < (int)cap;
}

static bool serializeKv(const std::vector<SdNvsPrefs::Kv>& kv, std::vector<uint8_t>& out) {
  size_t total = 0;
  for (const auto& e : kv) {
    const size_t kl = strnlen(e.key, sizeof(e.key));
    if (kl == 0 || kl > 15 || e.val.size() > 2048) return false;
    total += 1 + kl + 2 + e.val.size();
    if (total > PREFS_MAX_PAYLOAD) return false;
  }
  out.clear();
  out.reserve(total);
  for (const auto& e : kv) {
    const size_t kl = strnlen(e.key, sizeof(e.key));
    const size_t vl = e.val.size();
    out.push_back((uint8_t)kl);
    out.insert(out.end(), e.key, e.key + kl);
    out.push_back((uint8_t)(vl & 0xFF));
    out.push_back((uint8_t)((vl >> 8) & 0xFF));
    out.insert(out.end(), e.val.begin(), e.val.end());
  }
  return true;
}

static bool parsePayload(const uint8_t* data, size_t len, std::vector<SdNvsPrefs::Kv>& out) {
  out.clear();
  size_t pos = 0;
  while (pos < len) {
    if (out.size() >= 256 || pos + 1 > len) return false;
    const size_t kl = data[pos++];
    if (kl == 0 || kl > 15 || pos + kl + 2 > len) return false;
    SdNvsPrefs::Kv e{};
    memcpy(e.key, data + pos, kl);
    e.key[kl] = '\0';
    pos += kl;
    const size_t vl = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
    pos += 2;
    if (vl > 2048 || pos + vl > len) return false;
    e.val.assign(data + pos, data + pos + vl);
    pos += vl;
    out.push_back(std::move(e));
  }
  return true;
}

static bool readLegacyFile(fs::FS* fs, const char* path, std::vector<SdNvsPrefs::Kv>& out) {
  if (!fs || !fs->exists(path)) return false;
  File f = fs->open(path, FILE_READ);
  if (!f) return false;
  const size_t size = f.size();
  if (size > PREFS_MAX_PAYLOAD) { f.close(); return false; }
  std::vector<uint8_t> payload(size);
  const bool ok = (size == 0 || f.read(payload.data(), size) == (int)size);
  f.close();
  return ok && parsePayload(payload.data(), payload.size(), out);
}

static bool readSnapshot(fs::FS* fs, const char* path, std::vector<SdNvsPrefs::Kv>& out,
                         uint32_t& generation, bool trace_io = false, char slot = 0) {
  if (!fs) return false;
  bool exists = false;
  {
    PrefsIoPhase io(trace_io, "verify-stat", path, slot);
    exists = fs->exists(path);
  }
  if (!exists) return false;
  File f;
  {
    PrefsIoPhase io(trace_io, "verify-open", path, slot);
    f = fs->open(path, FILE_READ);
  }
  if (!f) return false;
  size_t size = 0;
  {
    PrefsIoPhase io(trace_io, "verify-size", path, slot);
    size = f.size();
  }
  if (size < sizeof(SnapshotFooter) || size - sizeof(SnapshotFooter) > PREFS_MAX_PAYLOAD) {
    PrefsIoPhase io(trace_io, "verify-close", path, slot);
    f.close();
    return false;
  }
  const size_t payload_size = size - sizeof(SnapshotFooter);
  std::vector<uint8_t> payload(payload_size);
  SnapshotFooter footer{};
  bool ok = false;
  {
    PrefsIoPhase io(trace_io, "verify-read", path, slot);
    ok = (payload_size == 0 || f.read(payload.data(), payload_size) == (int)payload_size) &&
         f.read((uint8_t*)&footer, sizeof(footer)) == (int)sizeof(footer);
  }
  {
    PrefsIoPhase io(trace_io, "verify-close", path, slot);
    f.close();
  }
  if (!ok || footer.magic != PREFS_MAGIC || footer.format != PREFS_FORMAT ||
      footer.footer_size != sizeof(footer) || footer.payload_size != payload_size ||
      footer.payload_crc != crc32(payload.data(), payload.size()) ||
      footer.footer_crc != crc32(&footer, offsetof(SnapshotFooter, footer_crc))) return false;
  if (!parsePayload(payload.data(), payload.size(), out)) return false;
  generation = footer.generation;
  return true;
}

static bool writeSnapshot(fs::FS* fs, const char* legacy_path, char slot, uint32_t generation,
                          const std::vector<SdNvsPrefs::Kv>& kv) {
  if (!fs) return false;
  char path[44];
  if (!slotPath(legacy_path, slot, path, sizeof(path))) return false;
  char dir[40];
  strncpy(dir, legacy_path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
  char* slash = strrchr(dir, '/');
  if (slash && slash != dir) {
    *slash = '\0';
    PrefsIoPhase io(true, "mkdir", path, slot);
    fs->mkdir(dir);
  }

  std::vector<uint8_t> payload;
  if (!serializeKv(kv, payload)) return false;
  SnapshotFooter footer{PREFS_MAGIC, PREFS_FORMAT, (uint16_t)sizeof(SnapshotFooter),
                        generation, (uint32_t)payload.size(), crc32(payload.data(), payload.size()), 0};
  footer.footer_crc = crc32(&footer, offsetof(SnapshotFooter, footer_crc));

#if defined(TDP4_POKE_TRACE)
  printf("[SDW] %lu core%d prefs %s\n", (unsigned long)millis(), (int)xPortGetCoreID(), path);
#endif
  File f;
  {
    PrefsIoPhase io(true, "open", path, slot);
    f = fs->open(path, FILE_WRITE);
  }
  if (!f) return false;
  bool ok = false;
  {
    PrefsIoPhase io(true, "write", path, slot);
    ok = (payload.empty() || f.write(payload.data(), payload.size()) == payload.size()) &&
         f.write((const uint8_t*)&footer, sizeof(footer)) == sizeof(footer);
  }
  {
    PrefsIoPhase io(true, "flush", path, slot);
    f.flush();
  }
  {
    PrefsIoPhase io(true, "close", path, slot);
    f.close();
  }
  if (!ok) return false;

  std::vector<SdNvsPrefs::Kv> verify;
  uint32_t verify_generation = 0;
  return readSnapshot(fs, path, verify, verify_generation, true, slot) &&
         verify_generation == generation;
}

static bool newerGeneration(uint32_t lhs, uint32_t rhs) {
  return (int32_t)(lhs - rhs) > 0;
}

static FileCache* findCacheLocked(const char* path) {
  for (auto& cache : s_caches)
    if (strncmp(cache.legacy_path, path, sizeof(cache.legacy_path)) == 0) return &cache;
  return nullptr;
}

// File-mode caches are shared by every SdNvsPrefs instance and by both the UI
// and worker cores. Copy into caller-owned storage while holding the cache
// mutex; returning a Kv* would let a concurrent setter invalidate the vector
// element or its payload as soon as the lock was released. Scalar and buffer
// getters stay allocation-free, avoiding tiny heap churn on every preference
// read. value_size reports the stored length even when the output is truncated.
static bool copyCachedValue(const char* path, const char* key, void* out,
                            size_t out_size, size_t& value_size) {
  value_size = 0;
  if (!s_cache_mutex || !path || !key) return false;
  bool found = false;
  xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
  if (FileCache* cache = findCacheLocked(path)) {
    for (const auto& e : cache->kv) {
      if (strncmp(e.key, key, sizeof(e.key)) != 0) continue;
      value_size = e.val.size();
      const size_t copy_size = value_size < out_size ? value_size : out_size;
      if (out && copy_size) memcpy(out, e.val.data(), copy_size);
      found = true;
      break;
    }
  }
  xSemaphoreGive(s_cache_mutex);
  return found;
}

static void prefsWriterTask(void*) {
  for (;;) {
    WriteJob* job = nullptr;
    if (xQueueReceive(s_write_queue, &job, portMAX_DELAY) != pdTRUE || !job) continue;
    const uint32_t started = millis();
    const bool ok = writeSnapshot(job->fs, job->legacy_path, job->slot, job->generation, job->kv);
    const uint32_t elapsed = millis() - started;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (FileCache* cache = findCacheLocked(job->legacy_path)) {
      cache->writing = false;
      if (ok) {
        cache->generation = job->generation;
        cache->active_slot = job->slot;
        if (cache->revision == job->revision) cache->dirty = false;
        cache->retry_after_ms = 0;
      } else {
        cache->dirty = true;
        cache->dirty_first_ms = cache->dirty_last_ms = millis();
        cache->retry_after_ms = millis() + 30000;
        Serial.printf("[PREFS] snapshot write failed %s.%c\n", job->legacy_path, job->slot);
      }
    }
    xSemaphoreGive(s_cache_mutex);
    if (ok && elapsed >= PREFS_SLOW_IO_MS)
      Serial.printf("[PREFS] slow snapshot %s slot %c: %lums\n", job->legacy_path,
                    job->slot, (unsigned long)elapsed);
    delete job;
  }
}

static bool ensureWorker() {
  if (!s_cache_mutex) s_cache_mutex = xSemaphoreCreateMutex();
  if (!s_write_queue) s_write_queue = xQueueCreate(2, sizeof(WriteJob*));
  if (!s_cache_mutex || !s_write_queue) return false;
  if (!s_write_task) {
    if (xTaskCreatePinnedToCore(prefsWriterTask, "prefs_writer", 6144, nullptr, 2,
                                &s_write_task, 0) != pdPASS) s_write_task = nullptr;
  }
  return s_write_task != nullptr;
}

void SdNvsPrefs::useFile(fs::FS* fs, const char* dir) {
  if (!fs) return;
  s_file_fs = fs;
  strncpy(s_file_dir, (dir && dir[0]) ? dir : "/prefs", sizeof(s_file_dir) - 1);
  s_file_dir[sizeof(s_file_dir) - 1] = '\0';
  s_file_mode = true;
  ensureWorker();
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
  if (_nvs_open) { _nvs.end(); _nvs_open = false; }

  if (fileMode()) {
    if (!ns || snprintf(_path, sizeof(_path), "%s/%s.kv", s_file_dir, ns) >= (int)sizeof(_path))
      return false;
    const bool loaded = sdLoad();
    if (nvsHasLegacyData()) _nvs_open = _nvs.begin(ns, true);   // RO migration source
    return loaded;
  }

  if (useNvs()) {
    _sd.clear();
    _sd_loaded = false;
    _nvs_open = _nvs.begin(ns, readOnly);
    return _nvs_open;
  }
  if (!ns || snprintf(_path, sizeof(_path), "/meshcomod/%s.kv", ns) >= (int)sizeof(_path))
    return false;
  const bool had_cache = _sd_loaded;
  const bool loaded = sdLoad();
  if (loaded) _sd_loaded = true;
  return loaded || (readOnly && had_cache);
}

void SdNvsPrefs::end() {
  if (_nvs_open) { _nvs.end(); _nvs_open = false; }
}

// ----------------------------- file helpers -----------------------------
SdNvsPrefs::Kv* SdNvsPrefs::sdFind(const char* key) {
  // Shared file-mode entries must never escape s_cache_mutex. File-mode
  // callers use copyCachedValue() instead; this pointer helper is only for the
  // legacy per-instance vector below.
  if (fileMode()) return nullptr;
  for (auto& e : _sd) if (strncmp(e.key, key, sizeof(e.key)) == 0) return &e;
  return nullptr;
}

bool SdNvsPrefs::sdSet(const char* key, const uint8_t* data, size_t len) {
  if (_read_only || !key || key[0] == '\0' || strlen(key) > 15 || len > 2048 ||
      (len > 0 && !data)) return false;
  if (fileMode()) {
    if (!s_cache_mutex) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    FileCache* cache = findCacheLocked(_path);
    if (!cache || !cache->loaded) { xSemaphoreGive(s_cache_mutex); return false; }
    Kv* e = nullptr;
    for (auto& item : cache->kv)
      if (strncmp(item.key, key, sizeof(item.key)) == 0) { e = &item; break; }
    // Same-value writes are a successful no-op. Boot-time normalize/repair
    // setters re-put unchanged values every boot; without this check each one
    // dirtied the cache and bought a full snapshot rewrite — on the Pager's
    // internal SPIFFS that's a 0.7-1.5 s flash burst that stalls both cores
    // (cache-suspend) and widens the interrupted-write loss window (#246).
    if (e && e->val.size() == len && (len == 0 || memcmp(e->val.data(), data, len) == 0)) {
      xSemaphoreGive(s_cache_mutex);
      return true;
    }
    if (!e) {
      if (cache->kv.size() >= 256) { xSemaphoreGive(s_cache_mutex); return false; }
      cache->kv.push_back(Kv{});
      e = &cache->kv.back();
      strncpy(e->key, key, sizeof(e->key) - 1);
    }
    if (len == 0) e->val.clear(); else e->val.assign(data, data + len);
    const uint32_t now = millis();
    if (!cache->dirty) cache->dirty_first_ms = now;
    cache->dirty = true;
    cache->dirty_last_ms = now;
    cache->retry_after_ms = 0;
    ++cache->revision;
    xSemaphoreGive(s_cache_mutex);
    return true;
  }
  Kv* e = sdFind(key);
  bool added = false;
  std::vector<uint8_t> old;
  if (!e) {
    _sd.push_back(Kv{});
    e = &_sd.back();
    added = true;
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = '\0';
  } else {
    // Same-value no-op for the legacy path too — skips a synchronous full-file
    // rewrite on SD for a value that hasn't changed.
    if (e->val.size() == len && (len == 0 || memcmp(e->val.data(), data, len) == 0))
      return true;
    old = e->val;
  }
  if (len == 0) e->val.clear();
  else          e->val.assign(data, data + len);
  if (sdSave()) return true;
  if (added) _sd.pop_back();
  else       e->val = std::move(old);
  return false;
}

bool SdNvsPrefs::sdLoad() {
  fs::FS* fs = activeFs();
  if (!fs) return false;
  if (!fileMode()) {
    // Legacy fallback before useFile(): retain the old single-file behaviour.
    std::vector<Kv> loaded;
    if (!fs->exists(_path)) { _sd.clear(); return true; }
    if (!readLegacyFile(fs, _path, loaded)) return false;
    _sd = std::move(loaded);
    return true;
  }

  if (!ensureWorker()) return false;
  xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
  if (FileCache* existing = findCacheLocked(_path)) {
    const bool ok = existing->loaded;
    xSemaphoreGive(s_cache_mutex);
    return ok;
  }
  xSemaphoreGive(s_cache_mutex);

  char path_a[44], path_b[44];
  if (!slotPath(_path, 'a', path_a, sizeof(path_a)) ||
      !slotPath(_path, 'b', path_b, sizeof(path_b))) return false;
  std::vector<Kv> a, b, loaded;
  uint32_t gen_a = 0, gen_b = 0;
  const bool ok_a = readSnapshot(fs, path_a, a, gen_a);
  const bool ok_b = readSnapshot(fs, path_b, b, gen_b);
  uint32_t generation = 0;
  char active = 0;
  bool legacy_present = false;
  bool migrate = false;
  if (ok_a && (!ok_b || newerGeneration(gen_a, gen_b))) {
    loaded = std::move(a); generation = gen_a; active = 'a';
  } else if (ok_b) {
    loaded = std::move(b); generation = gen_b; active = 'b';
  } else {
    // Migrate the released single-file format. Parsing is all-or-nothing;
    // defaults never replace a corrupt namespace.
    if (readLegacyFile(fs, _path, loaded)) {
      legacy_present = true;
      migrate = true;
    } else if (fs->exists(_path) || fs->exists(path_a) || fs->exists(path_b)) {
      Serial.printf("[PREFS] no valid snapshot for %s\n", _path);
      return false;
    }
    // No file at all is a valid fresh namespace.
  }

  FileCache cache;
  cache.fs = fs;
  strncpy(cache.legacy_path, _path, sizeof(cache.legacy_path) - 1);
  cache.kv = std::move(loaded);
  cache.generation = generation;
  cache.active_slot = active;
  cache.loaded = true;
  cache.legacy_present = legacy_present;
  if (migrate) {
    cache.dirty = true;
    cache.dirty_first_ms = cache.dirty_last_ms = millis();
    ++cache.revision;
  }
  xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
  // Another task may have loaded this namespace while file I/O ran outside the
  // mutex. Keep a single authoritative cache entry instead of inserting an
  // invisible duplicate that the writer would still iterate.
  if (FileCache* existing = findCacheLocked(_path)) {
    const bool ok = existing->loaded;
    xSemaphoreGive(s_cache_mutex);
    return ok;
  }
  s_caches.push_back(std::move(cache));
  xSemaphoreGive(s_cache_mutex);
  return true;
}

bool SdNvsPrefs::sdSave() {
  if (_read_only) return false;
  if (fileMode()) return true;   // sdSet/remove/clear already dirtied the shared cache
  fs::FS* fs = activeFs();
  if (!fs) return false;
  std::vector<uint8_t> payload;
  if (!serializeKv(_sd, payload)) return false;
  File f = fs->open(_path, FILE_WRITE);
  if (!f) return false;
  const bool ok = payload.empty() || f.write(payload.data(), payload.size()) == payload.size();
  f.close();
  return ok;
}

static bool scheduleOne(uint32_t now, bool force) {
  if (!fileMode() || !ensureWorker()) return false;
  WriteJob* job = nullptr;
  char armed_path[40] = {0};
  xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
  for (auto& cache : s_caches) {
    if (!cache.loaded || !cache.dirty || cache.writing) continue;
    if (!force && cache.retry_after_ms && (int32_t)(now - cache.retry_after_ms) < 0) continue;
    const bool due = force || (uint32_t)(now - cache.dirty_last_ms) >= PREFS_QUIET_MS ||
                     (uint32_t)(now - cache.dirty_first_ms) >= PREFS_MAX_WAIT_MS;
    if (!due) continue;
    job = new (std::nothrow) WriteJob();
    if (!job) break;
    job->fs = cache.fs;
    strncpy(job->legacy_path, cache.legacy_path, sizeof(job->legacy_path) - 1);
    job->kv = cache.kv;
    job->generation = cache.generation + 1;
    job->revision = cache.revision;
    job->slot = cache.active_slot == 'a' ? 'b'
              : cache.active_slot == 'b' ? 'a'
              : cache.legacy_present ? 'b' : 'a';
    cache.writing = true;
    strncpy(armed_path, cache.legacy_path, sizeof(armed_path) - 1);
    break;
  }
  xSemaphoreGive(s_cache_mutex);
  if (!job) return false;
  if (xQueueSend(s_write_queue, &job, 0) != pdTRUE) {
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (FileCache* cache = findCacheLocked(armed_path)) cache->writing = false;
    xSemaphoreGive(s_cache_mutex);
    delete job;
    return false;
  }
  return true;
}

void SdNvsPrefs::tick(uint32_t now_ms) {
  if (!fileMode()) return;
  if (now_ms == 0) now_ms = millis();
  scheduleOne(now_ms, false);
}

bool SdNvsPrefs::flush(uint32_t timeout_ms) {
  if (!fileMode()) return true;
  if (!ensureWorker()) {
    // A writer-allocation failure is only a flush failure when RAM contains
    // work that still needs landing. A clean store has nothing to wait for and
    // must not permanently block unrelated migration/reboot workflows.
    if (!s_cache_mutex) return true;
    bool pending = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (const auto& cache : s_caches) {
      if (cache.dirty || cache.writing) { pending = true; break; }
    }
    xSemaphoreGive(s_cache_mutex);
    return !pending;
  }
  const uint32_t start = millis();
  for (;;) {
    scheduleOne(millis(), true);
    bool pending = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (const auto& cache : s_caches)
      if (cache.dirty || cache.writing) { pending = true; break; }
    xSemaphoreGive(s_cache_mutex);
    if (!pending) return true;
    if ((uint32_t)(millis() - start) >= timeout_ms) return false;
    delay(10);
  }
}

bool SdNvsPrefs::busy() {
  if (!fileMode() || !s_cache_mutex) return false;
  bool result = false;
  xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
  for (const auto& cache : s_caches)
    if (cache.writing) { result = true; break; }
  xSemaphoreGive(s_cache_mutex);
  return result;
}

bool SdNvsPrefs::slowIoOverlap(uint32_t start_ms, uint32_t end_ms,
                               const char*& phase, uint32_t& elapsed_ms) {
  phase = nullptr;
  elapsed_ms = 0;
  portENTER_CRITICAL(&s_io_trace_mux);
  if (s_io_trace.active_phase) {
    const uint32_t active_elapsed = end_ms - s_io_trace.active_started_ms;
    if (active_elapsed >= PREFS_SLOW_IO_MS &&
        msIntervalsOverlap(start_ms, end_ms, s_io_trace.active_started_ms, end_ms)) {
      phase = s_io_trace.active_phase;
      elapsed_ms = active_elapsed;
    }
  }
  if (s_io_trace.last_slow_phase &&
      s_io_trace.last_slow_elapsed_ms > elapsed_ms &&
      msIntervalsOverlap(start_ms, end_ms, s_io_trace.last_slow_started_ms,
                         s_io_trace.last_slow_ended_ms)) {
    phase = s_io_trace.last_slow_phase;
    elapsed_ms = s_io_trace.last_slow_elapsed_ms;
  }
  portEXIT_CRITICAL(&s_io_trace_mux);
  return phase != nullptr;
}

static bool explicitNamespacePath(const char* dir, const char* ns, char* out, size_t cap) {
  if (!dir || !ns || !out) return false;
  return snprintf(out, cap, "%s/%s.kv", dir, ns) < (int)cap;
}

static bool loadExplicitNamespace(fs::FS* fs, const char* legacy_path,
                                  std::vector<SdNvsPrefs::Kv>& kv,
                                  uint32_t& generation, char& active) {
  char path_a[44], path_b[44];
  if (!slotPath(legacy_path, 'a', path_a, sizeof(path_a)) ||
      !slotPath(legacy_path, 'b', path_b, sizeof(path_b))) return false;
  std::vector<SdNvsPrefs::Kv> a, b;
  uint32_t gen_a = 0, gen_b = 0;
  const bool ok_a = readSnapshot(fs, path_a, a, gen_a);
  const bool ok_b = readSnapshot(fs, path_b, b, gen_b);
  if (ok_a && (!ok_b || newerGeneration(gen_a, gen_b))) {
    kv = std::move(a); generation = gen_a; active = 'a'; return true;
  }
  if (ok_b) { kv = std::move(b); generation = gen_b; active = 'b'; return true; }
  generation = 0;
  active = 0;
  return readLegacyFile(fs, legacy_path, kv);
}

bool SdNvsPrefs::readFileBool(fs::FS* fs, const char* dir, const char* ns,
                              const char* key, bool& value) {
  char legacy_path[40];
  if (!fs || !key || !explicitNamespacePath(dir, ns, legacy_path, sizeof(legacy_path))) return false;
  std::vector<Kv> kv;
  uint32_t generation = 0;
  char active = 0;
  if (!loadExplicitNamespace(fs, legacy_path, kv, generation, active)) return false;
  for (const auto& e : kv) {
    if (strncmp(e.key, key, sizeof(e.key)) != 0) continue;
    if (e.val.empty()) return false;
    value = e.val[0] != 0;
    return true;
  }
  return false;
}

bool SdNvsPrefs::writeFileBool(fs::FS* fs, const char* dir, const char* ns,
                               const char* key, bool value) {
  char legacy_path[40];
  if (!fs || !key || key[0] == '\0' || strlen(key) > 15 ||
      !explicitNamespacePath(dir, ns, legacy_path, sizeof(legacy_path))) return false;
  std::vector<Kv> kv;
  uint32_t generation = 0;
  char active = 0;
  const bool existed = loadExplicitNamespace(fs, legacy_path, kv, generation, active);
  Kv* found = nullptr;
  for (auto& e : kv)
    if (strncmp(e.key, key, sizeof(e.key)) == 0) { found = &e; break; }
  if (!found) {
    kv.push_back(Kv{});
    found = &kv.back();
    strncpy(found->key, key, sizeof(found->key) - 1);
  }
  found->val.assign(1, value ? 1 : 0);
  const char slot = active == 'a' ? 'b' : active == 'b' ? 'a' : existed ? 'b' : 'a';
  if (!writeSnapshot(fs, legacy_path, slot, generation + 1, kv)) return false;
  return true;
}

uint64_t SdNvsPrefs::sdGetInt(const char* key, uint64_t def, int width, bool* found) {
  uint8_t cached[8] = {};
  if (fileMode()) {
    size_t value_size = 0;
    const bool exists = copyCachedValue(_path, key, cached, sizeof cached, value_size);
    if (found) *found = exists;
    if (!exists || value_size == 0) return def;
    uint64_t v = 0;
    int n = (int)value_size; if (n > width) n = width;
    for (int i = 0; i < n; i++) v |= (uint64_t)cached[i] << (8 * i);
    return v;
  }
  Kv* e = sdFind(key);
  if (found) *found = e != nullptr;
  if (!e || e->val.empty()) return def;
  uint64_t v = 0;
  int n = (int)e->val.size(); if (n > width) n = width;
  for (int i = 0; i < n; i++) v |= (uint64_t)e->val[i] << (8 * i);
  return v;
}

size_t SdNvsPrefs::sdPutInt(const char* key, uint64_t v, int width) {
  uint8_t b[8];
  for (int i = 0; i < width; i++) b[i] = (uint8_t)(v >> (8 * i));
  return sdSet(key, b, width) ? width : 0;
}

// ----------------------------- API -----------------------------
// File mode: read file first, fall back to the legacy NVS value (read-only) so
// old settings still load and migrate to the file on the next save. Putters only
// ever touch the file — nothing new is written to NVS.

bool SdNvsPrefs::isKey(const char* key) {
  if (fileMode()) {
    size_t value_size = 0;
    return copyCachedValue(_path, key, nullptr, 0, value_size) ||
           (_nvs_open && _nvs.isKey(key));
  }
  return useNvs() ? (_nvs_open && _nvs.isKey(key)) : (sdFind(key) != nullptr);
}

bool SdNvsPrefs::remove(const char* key) {
  if (!fileMode() && useNvs()) return _nvs_open && _nvs.remove(key);
  if (fileMode()) {
    if (_read_only || !s_cache_mutex) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    FileCache* cache = findCacheLocked(_path);
    if (!cache) { xSemaphoreGive(s_cache_mutex); return false; }
    for (size_t i = 0; i < cache->kv.size(); ++i) {
      if (strncmp(cache->kv[i].key, key, sizeof(cache->kv[i].key)) != 0) continue;
      cache->kv.erase(cache->kv.begin() + i);
      const uint32_t now = millis();
      if (!cache->dirty) cache->dirty_first_ms = now;
      cache->dirty = true;
      cache->dirty_last_ms = now;
      cache->retry_after_ms = 0;
      ++cache->revision;
      xSemaphoreGive(s_cache_mutex);
      return true;
    }
    xSemaphoreGive(s_cache_mutex);
    return false;
  }
  for (size_t i = 0; i < _sd.size(); i++) {
    if (strncmp(_sd[i].key, key, sizeof(_sd[i].key)) != 0) continue;
    Kv old = _sd[i];
    _sd.erase(_sd.begin() + i);
    if (sdSave()) return true;
    _sd.insert(_sd.begin() + i, std::move(old));
    return false;
  }
  return false;
}

bool SdNvsPrefs::clear() {
  if (fileMode()) {
    if (_read_only || !s_cache_mutex) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    FileCache* cache = findCacheLocked(_path);
    if (!cache) { xSemaphoreGive(s_cache_mutex); return false; }
    cache->kv.clear();
    const uint32_t now = millis();
    if (!cache->dirty) cache->dirty_first_ms = now;
    cache->dirty = true;
    cache->dirty_last_ms = now;
    cache->retry_after_ms = 0;
    ++cache->revision;
    xSemaphoreGive(s_cache_mutex);
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
  std::vector<Kv> old = _sd;
  _sd.clear();
  if (sdSave()) return true;
  _sd = std::move(old);
  return false;
}

uint8_t SdNvsPrefs::getUChar(const char* k, uint8_t def) {
  if (fileMode()) {
    bool found = false;
    const uint8_t value = (uint8_t)sdGetInt(k, def, 1, &found);
    if (found) return value;
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
    bool found = false;
    const int8_t value = (int8_t)sdGetInt(k, (uint8_t)def, 1, &found);
    if (found) return value;
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
    bool found = false;
    const uint16_t value = (uint16_t)sdGetInt(k, def, 2, &found);
    if (found) return value;
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
    bool found = false;
    const uint32_t value = (uint32_t)sdGetInt(k, def, 4, &found);
    if (found) return value;
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
    bool found = false;
    const bool value = sdGetInt(k, def ? 1 : 0, 1, &found) != 0;
    if (found) return value;
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
  if (fileMode()) {
    if (!s_cache_mutex) return (_nvs_open && _nvs.isKey(k)) ? _nvs.getString(k, def) : def;
    String value;
    bool found = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (FileCache* cache = findCacheLocked(_path)) {
      for (const auto& e : cache->kv) {
        if (strncmp(e.key, k, sizeof(e.key)) != 0) continue;
        value.reserve(e.val.size());
        for (uint8_t c : e.val) value += (char)c;
        found = true;
        break;
      }
    }
    xSemaphoreGive(s_cache_mutex);
    if (!found)
      return (_nvs_open && _nvs.isKey(k)) ? _nvs.getString(k, def) : def;
    return value;
  }
  if (useNvs()) return _nvs.getString(k, def);
  Kv* e = sdFind(k);
  if (!e) return def;
  String s; s.reserve(e->val.size());
  for (uint8_t c : e->val) s += (char)c;
  return s;
}
size_t SdNvsPrefs::getString(const char* k, char* buf, size_t maxLen) {
  if (!buf || !maxLen) return 0;
  if (fileMode()) {
    size_t value_size = 0;
    if (!copyCachedValue(_path, k, buf, maxLen - 1, value_size)) {
      if (_nvs_open && _nvs.isKey(k)) return _nvs.getString(k, buf, maxLen);
      buf[0] = '\0';
      return 0;
    }
    size_t n = value_size;
    if (n > maxLen - 1) n = maxLen - 1;
    buf[n] = '\0';
    return n;
  }
  if (useNvs()) return _nvs.getString(k, buf, maxLen);
  Kv* e = sdFind(k);
  size_t n = e ? e->val.size() : 0;
  if (n > maxLen - 1) n = maxLen - 1;
  if (e && n) memcpy(buf, e->val.data(), n);
  buf[n] = '\0';
  return n;
}
size_t SdNvsPrefs::putString(const char* k, const char* v) {
  if (fileMode()) { size_t n = v ? strlen(v) : 0; return sdSet(k, (const uint8_t*)v, n) ? n : 0; }
  if (useNvs()) return _nvs.putString(k, v);
  size_t n = v ? strlen(v) : 0; return sdSet(k, (const uint8_t*)v, n) ? n : 0;
}

size_t SdNvsPrefs::getBytes(const char* k, void* buf, size_t maxLen) {
  if (fileMode()) {
    size_t value_size = 0;
    if (!copyCachedValue(_path, k, buf, maxLen, value_size))
      return (_nvs_open && _nvs.isKey(k)) ? _nvs.getBytes(k, buf, maxLen) : 0;
    return value_size;
  }
  if (useNvs()) return _nvs.getBytes(k, buf, maxLen);
  Kv* e = sdFind(k);
  if (!e) return 0;
  size_t n = e->val.size();
  if (buf && maxLen) { size_t c = n > maxLen ? maxLen : n; memcpy(buf, e->val.data(), c); }
  return n;
}
size_t SdNvsPrefs::putBytes(const char* k, const void* buf, size_t len) {
  if (fileMode()) return sdSet(k, (const uint8_t*)buf, len) ? len : 0;
  if (useNvs()) return _nvs.putBytes(k, buf, len);
  return sdSet(k, (const uint8_t*)buf, len) ? len : 0;
}

#endif  // ESP32
