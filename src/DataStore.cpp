#include <Arduino.h>
#include <string.h>
#include "DataStore.h"
#include <helpers/AdvertDataHelpers.h>   // ADV_TYPE_NONE (blank-record skip in loadContacts)
#if defined(ESP32)
#include <SD.h>
#include "helpers/esp32/WdtHeavyGuard.h"   // suspend core-0 idle WDT during the (SPIFFS-GC-prone) contact write
#endif
#if defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4) || defined(HAS_WIO_TRACKER_L2)
#include <SD_MMC.h>
#endif

#if defined(EXTRAFS) || defined(QSPIFLASH)
  #define MAX_BLOBRECS 100
#else
  #define MAX_BLOBRECS 20
#endif

DataStore::DataStore(FILESYSTEM& fs, mesh::RTCClock& clock) : _fs(&fs), _fsExtra(nullptr), _clock(&clock),
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    identity_store(fs, "")
#elif defined(RP2040_PLATFORM)
    identity_store(fs, "/identity")
#else
    identity_store(fs, "/identity")
#endif
{
}

#if defined(EXTRAFS) || defined(QSPIFLASH)
DataStore::DataStore(FILESYSTEM& fs, FILESYSTEM& fsExtra, mesh::RTCClock& clock) : _fs(&fs), _fsExtra(&fsExtra), _clock(&clock),
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    identity_store(fs, "")
#elif defined(RP2040_PLATFORM)
    identity_store(fs, "/identity")
#else
    identity_store(fs, "/identity")
#endif
{
}
#endif

const char* DataStore::_rp(const char* name) {
  if (!_root[0]) return name;            // default: filesystem root, zero change
  snprintf(_rpbuf, sizeof(_rpbuf), "%s%s", _root, name);
  return _rpbuf;
}

File DataStore::openWrite(FILESYSTEM* fs, const char* filename) {
#if defined(TDP4_POKE_TRACE)
  printf("[SDW] %lu core%d store w %s\n", (unsigned long)millis(), xPortGetCoreID(), filename);
#endif
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(_rp(filename));
  return fs->open(_rp(filename), FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(_rp(filename), "w");
#else
  return fs->open(_rp(filename), "w", true);
#endif
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  static uint32_t _ContactsChannelsTotalBlocks = 0;
#endif

void DataStore::begin() {
#if defined(RP2040_PLATFORM)
  identity_store.begin();
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _ContactsChannelsTotalBlocks = _getContactsChannelsFS()->_getFS()->cfg->block_count;
  checkAdvBlobFile();
  #if defined(EXTRAFS) || defined(QSPIFLASH)
  migrateToSecondaryFS();
  #endif
#else
  // init 'blob store' support
  _fs->mkdir("/bl");
  #if defined(ESP32)
  // One-time migration: when a secondary FS is active (an SD card is present)
  // but contacts/channels still live on the primary FS from an earlier build,
  // copy them onto the card ONCE and delete the primary copies. This preserves
  // the user's contact list while getting the frequently-rewritten files off a
  // small/near-full SPIFFS — whose garbage collection can otherwise stall the
  // loop task for seconds during saveContacts and trip the task watchdog
  // (the beta_25 reboot loop). Identity + prefs stay on _fs and never move.
  // No-op once the card already holds the files, and a safe no-op on boards
  // whose primary-FS metadata can't confirm the source exists (e.g. P4 FFat).
  if (_fsExtra) {
    static const char* const kMoveFiles[] = { "/contacts3", "/channels2" };
    for (const char* nm : kMoveFiles) {
      if (_fsExtra->exists(nm)) continue;       // already on the card
      if (!_fs->exists(nm))     continue;       // nothing to move
      File in  = openRead(_fs, nm);
      File out = openWrite(_fsExtra, nm);
      bool ok = in && out;
      if (ok) {
        uint8_t buf[256];
        int n;
        while ((n = in.read(buf, sizeof(buf))) > 0) {
          if (out.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
        }
      }
      if (in)  in.close();
      if (out) out.close();
      if (ok)  _fs->remove(nm);                            // reclaim SPIFFS space
      else if (_fsExtra->exists(nm)) _fsExtra->remove(nm); // don't leave a partial copy
    }
  }
  #endif
#endif
}

#if defined(ESP32)
  #include <SPIFFS.h>
  #include <nvs_flash.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
  #elif defined(EXTRAFS)
    #include <CustomLFS.h>
  #else 
    #include <InternalFileSystem.h>
  #endif
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
int _countLfsBlock(void *p, lfs_block_t block){
      if (block > _ContactsChannelsTotalBlocks) {
        MESH_DEBUG_PRINTLN("ERROR: Block %d exceeds filesystem bounds - CORRUPTION DETECTED!", block);
        return LFS_ERR_CORRUPT;  // return error to abort lfs_traverse() gracefully
    }
  lfs_size_t *size = (lfs_size_t*) p;
  *size += 1;
    return 0;
}

lfs_ssize_t _getLfsUsedBlockCount(FILESYSTEM* fs) {
  lfs_size_t size = 0;
  int err = lfs_traverse(fs->_getFS(), _countLfsBlock, &size);
  if (err) {
    MESH_DEBUG_PRINTLN("ERROR: lfs_traverse() error: %d", err);
    return 0;
  }
  return size;
}
#endif

uint32_t DataStore::getStorageUsedKb() const {
#if defined(ESP32)
  return SPIFFS.usedBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.usedBytes = 0;
  _fs->info(info);
  return info.usedBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  const lfs_config* config = _getContactsChannelsFS()->_getFS()->cfg;
  int usedBlockCount = _getLfsUsedBlockCount(_getContactsChannelsFS());
  int usedBytes = config->block_size * usedBlockCount;
  return usedBytes / 1024;
#else
  return 0;
#endif
}

uint32_t DataStore::getStorageTotalKb() const {
#if defined(ESP32)
  return SPIFFS.totalBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.totalBytes = 0;
  _fs->info(info);
  return info.totalBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  const lfs_config* config = _getContactsChannelsFS()->_getFS()->cfg;
  int totalBytes = config->block_size * config->block_count;
  return totalBytes / 1024;
#else
  return 0;
#endif
}

File DataStore::openRead(const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(_rp(filename), FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  return _fs->open(_rp(filename), "r");
#else
  return _fs->open(_rp(filename), "r", false);
#endif
}

File DataStore::openRead(FILESYSTEM* fs, const char* filename) {
#if defined(TDP4_POKE_TRACE)
  printf("[SDR] %lu core%d store r %s\n", (unsigned long)millis(), (int)xPortGetCoreID(), filename);
#endif
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs->open(_rp(filename), FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  return fs->open(_rp(filename), "r");
#else
  return fs->open(_rp(filename), "r", false);
#endif
}

bool DataStore::removeFile(const char* filename) {
  return _fs->remove(_rp(filename));
}

bool DataStore::removeFile(FILESYSTEM* fs, const char* filename) {
  return fs->remove(filename);
}

#if defined(ESP32)
File DataStore::openAppend(FILESYSTEM* fs, const char* filename) {
  return fs->open(_rp(filename), "a", true);
}

bool DataStore::fileExists(FILESYSTEM* fs, const char* filename) {
  return fs->exists(_rp(filename));
}

bool DataStore::mkdirRooted(FILESYSTEM* fs, const char* dir) {
  const char* p = _rp(dir);
  return fs->exists(p) || fs->mkdir(p);
}

File DataStore::openWriteRootedFlatSafe(FILESYSTEM* fs, const char* filename) {
  // Do not pass create=true here. Arduino's VFS implementation interprets it
  // as "mkdir every parent first", but SPIFFS is flat and returns ENOTSUP for
  // mkdir while still accepting slash-containing file keys.
  return fs->open(_rp(filename), "w");
}

bool DataStore::removeRooted(FILESYSTEM* fs, const char* filename) {
  return fs->remove(_rp(filename));
}

bool DataStore::renameFile(FILESYSTEM* fs, const char* from, const char* to) {
  // _rp() hands back ONE shared scratch buffer, so the two paths must be built
  // into independent buffers (same trap savePrefs documents).
  char from_path[80], to_path[80];
  snprintf(from_path, sizeof(from_path), "%s%s", _root, from);
  snprintf(to_path,   sizeof(to_path),   "%s%s", _root, to);
  return fs->rename(from_path, to_path);
}
#endif

bool DataStore::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  if (_fsExtra == nullptr) {
    return _fs->format();
  } else {
    return _fs->format() && _fsExtra->format();
  }
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  // Only the internal store is SPIFFS. When useSdStorage() is active, _fs points
  // at the SD (SDFS) singleton — NOT a SPIFFSFS — so casting it and calling
  // format() dereferences garbage and crashes. That crash aborted the factory
  // reset before NVS was erased or the device restarted (everything came back).
  // On SD the data dir (_root) is wiped by the caller, so here we only format
  // SPIFFS when it's genuinely the active store.
  bool fs_success = true;
  if (_root[0] == '\0') fs_success = ((fs::SPIFFSFS *)_fs)->format();
  esp_err_t nvs_err = nvs_flash_erase(); // no need to reinit, will be done by reboot
  return fs_success && (nvs_err == ESP_OK);
#else
  #error "need to implement format()"
#endif
}

bool DataStore::loadMainIdentity(mesh::LocalIdentity &identity) {
  return identity_store.load("_main", identity);
}

bool DataStore::saveMainIdentity(const mesh::LocalIdentity &identity) {
  return identity_store.save("_main", identity);
}

void DataStore::loadPrefs(NodePrefs& prefs, double& node_lat, double& node_lon) {
  // Probe by OPENING, never exists(): on the Tanmatsu's internal FFat the FAT
  // metadata layer lies (f_stat garbage — the tile cache hit the same thing),
  // and WHICH lie you get shifts with the build (the -Og to -Os switch turned
  // "loaded fine by luck" into "no prefs found -> default name every boot").
  // open()+read is truthful there, and identical in behavior everywhere else.
  auto probe = [this](const char* nm) -> bool {
    File f = openRead(nm);
    if (!f) return false;
    uint8_t b;
    const bool ok = f.read(&b, 1) == 1;   // size() lies on the P4's FFat too — actually read
    f.close();
    return ok;
  };
  if (probe("/new_prefs")) {
    loadPrefsInt("/new_prefs", prefs, node_lat, node_lon); // new filename
  } else if (probe("/new_prefs.tmp")) {
    // Main file gone but a staged copy exists: a reboot landed between the temp
    // write and the swap (or the swap was torn). Recover from it — this is the
    // "device booted with the default name once" failure mode.
    MESH_DEBUG_PRINTLN("DataStore: /new_prefs missing, recovering from .tmp");
    loadPrefsInt("/new_prefs.tmp", prefs, node_lat, node_lon);
    savePrefs(prefs, node_lat, node_lon);                // re-establish the main file
  } else if (probe("/node_prefs")) {
    loadPrefsInt("/node_prefs", prefs, node_lat, node_lon);
    savePrefs(prefs, node_lat, node_lon);                // save to new filename
    _fs->remove(_rp("/node_prefs")); // remove old
  } else {
    MESH_DEBUG_PRINTLN("DataStore: no prefs file found — using defaults");
  }
}

namespace {

void default_rx_boosted_gain_pref(uint8_t& out) {
#if defined(USE_SX1262) || defined(USE_SX1268) || defined(USE_LR1121) || defined(SX126X_RX_BOOSTED_GAIN)
  #ifdef SX126X_RX_BOOSTED_GAIN
  out = (SX126X_RX_BOOSTED_GAIN != 0) ? 1 : 0;
  #else
  out = 1;
  #endif
#else
  out = 0;
#endif
}

// After multi_acks (byte offset 78): upstream meshcore-dev layout is 15 bytes tail; legacy Meshcomod
// used 13 bytes (path_hash, autoadd_max, ble_pin..autoadd without reserved pad or trailing rx_boost).
void load_prefs_tail_meshcomod_legacy(NodePrefs& p, const uint8_t* tail) {
  p.path_hash_mode = tail[0];
  p.autoadd_max_hops = tail[1];
  memcpy(&p.ble_pin, tail + 2, 4);
  p.buzzer_quiet = tail[6];
  p.gps_enabled = tail[7];
  memcpy(&p.gps_interval, tail + 8, 4);
  p.autoadd_config = tail[12];
  default_rx_boosted_gain_pref(p.rx_boosted_gain);
}

void load_prefs_tail_upstream(NodePrefs& p, const uint8_t* tail, size_t n) {
  p.path_hash_mode = tail[0];
  // tail[1] reserved
  memcpy(&p.ble_pin, tail + 2, 4);
  p.buzzer_quiet = tail[6];
  p.gps_enabled = tail[7];
  memcpy(&p.gps_interval, tail + 8, 4);
  p.autoadd_config = tail[12];
  p.autoadd_max_hops = tail[13];
  if (n >= 15) {
    p.rx_boosted_gain = tail[14] ? 1 : 0;
  } else {
    default_rx_boosted_gain_pref(p.rx_boosted_gain);
  }
}

}  // namespace

void DataStore::loadPrefsInt(const char *filename, NodePrefs& _prefs, double& node_lat, double& node_lon) {
  File file = openRead(_fs, filename);
  if (file) {
    uint8_t pad[8];

    file.read((uint8_t *)&_prefs.airtime_factor, sizeof(float));                           // 0
    file.read((uint8_t *)_prefs.node_name, sizeof(_prefs.node_name));                      // 4
    file.read(pad, 4);                                                                     // 36
    file.read((uint8_t *)&node_lat, sizeof(node_lat));                                     // 40
    file.read((uint8_t *)&node_lon, sizeof(node_lon));                                     // 48
    file.read((uint8_t *)&_prefs.freq, sizeof(_prefs.freq));                               // 56
    file.read((uint8_t *)&_prefs.sf, sizeof(_prefs.sf));                                   // 60
    file.read((uint8_t *)&_prefs.cr, sizeof(_prefs.cr));                                   // 61
    file.read((uint8_t *)&_prefs.client_repeat, sizeof(_prefs.client_repeat));             // 62
    file.read((uint8_t *)&_prefs.manual_add_contacts, sizeof(_prefs.manual_add_contacts)); // 63
    file.read((uint8_t *)&_prefs.bw, sizeof(_prefs.bw));                                   // 64
    file.read((uint8_t *)&_prefs.tx_power_dbm, sizeof(_prefs.tx_power_dbm));               // 68
    file.read((uint8_t *)&_prefs.telemetry_mode_base, sizeof(_prefs.telemetry_mode_base)); // 69
    file.read((uint8_t *)&_prefs.telemetry_mode_loc, sizeof(_prefs.telemetry_mode_loc));   // 70
    file.read((uint8_t *)&_prefs.telemetry_mode_env, sizeof(_prefs.telemetry_mode_env));   // 71
    file.read((uint8_t *)&_prefs.rx_delay_base, sizeof(_prefs.rx_delay_base));             // 72
    file.read((uint8_t *)&_prefs.advert_loc_policy, sizeof(_prefs.advert_loc_policy));     // 76
    file.read((uint8_t *)&_prefs.multi_acks, sizeof(_prefs.multi_acks));                   // 77
    uint8_t tail[15];
    size_t n = file.read(tail, sizeof(tail));
    bool migrated_legacy = false;
    if (n == 13) {
      load_prefs_tail_meshcomod_legacy(_prefs, tail);
      migrated_legacy = true;
    } else if (n >= 14) {
      load_prefs_tail_upstream(_prefs, tail, n);
    }
    memset(_prefs.default_scope_name, 0, sizeof(_prefs.default_scope_name));
    memset(_prefs.default_scope_key, 0, sizeof(_prefs.default_scope_key));
    file.read((uint8_t *)_prefs.default_scope_name, sizeof(_prefs.default_scope_name));    // 90
    file.read((uint8_t *)_prefs.default_scope_key, sizeof(_prefs.default_scope_key));      // 121

    file.close();

    if (migrated_legacy) {
      savePrefs(_prefs, node_lat, node_lon);
    }
  }
}

bool DataStore::savePrefs(const NodePrefs& _prefs, double node_lat, double node_lon) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; const NodePrefs* p; double la, lo; bool r; } a{ this, &_prefs, node_lat, node_lon, false };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->r = a->d->savePrefs(*a->p, a->la, a->lo); }, &a);
    return a.r;
  }
#endif
  // Write to a temp file first and swap it in afterwards: a reboot / power cut
  // mid-write can then never destroy the only copy (the loader recovers from
  // whichever file survived). SPIFFS has no atomic rename-over, so the swap is
  // remove+rename — the loader handles the tiny between-steps window too.
  File file = openWrite(_fs, "/new_prefs.tmp");
  if (file) {
    uint8_t pad[8];
    memset(pad, 0, sizeof(pad));

    file.write((uint8_t *)&_prefs.airtime_factor, sizeof(float));                           // 0
    file.write((uint8_t *)_prefs.node_name, sizeof(_prefs.node_name));                      // 4
    file.write(pad, 4);                                                                     // 36
    file.write((uint8_t *)&node_lat, sizeof(node_lat));                                     // 40
    file.write((uint8_t *)&node_lon, sizeof(node_lon));                                     // 48
    file.write((uint8_t *)&_prefs.freq, sizeof(_prefs.freq));                               // 56
    file.write((uint8_t *)&_prefs.sf, sizeof(_prefs.sf));                                   // 60
    file.write((uint8_t *)&_prefs.cr, sizeof(_prefs.cr));                                   // 61
    file.write((uint8_t *)&_prefs.client_repeat, sizeof(_prefs.client_repeat));             // 62
    file.write((uint8_t *)&_prefs.manual_add_contacts, sizeof(_prefs.manual_add_contacts)); // 63
    file.write((uint8_t *)&_prefs.bw, sizeof(_prefs.bw));                                   // 64
    file.write((uint8_t *)&_prefs.tx_power_dbm, sizeof(_prefs.tx_power_dbm));               // 68
    file.write((uint8_t *)&_prefs.telemetry_mode_base, sizeof(_prefs.telemetry_mode_base)); // 69
    file.write((uint8_t *)&_prefs.telemetry_mode_loc, sizeof(_prefs.telemetry_mode_loc));   // 70
    file.write((uint8_t *)&_prefs.telemetry_mode_env, sizeof(_prefs.telemetry_mode_env));   // 71
    file.write((uint8_t *)&_prefs.rx_delay_base, sizeof(_prefs.rx_delay_base));             // 72
    file.write((uint8_t *)&_prefs.advert_loc_policy, sizeof(_prefs.advert_loc_policy));     // 76
    file.write((uint8_t *)&_prefs.multi_acks, sizeof(_prefs.multi_acks));                   // 77
    file.write((uint8_t *)&_prefs.path_hash_mode, sizeof(_prefs.path_hash_mode));            // 78
    file.write(pad, 1);                                                                     // 79 reserved (upstream layout)
    file.write((uint8_t *)&_prefs.ble_pin, sizeof(_prefs.ble_pin));                         // 80
    file.write((uint8_t *)&_prefs.buzzer_quiet, sizeof(_prefs.buzzer_quiet));               // 84
    file.write((uint8_t *)&_prefs.gps_enabled, sizeof(_prefs.gps_enabled));                 // 85
    // gps_interval (4B) — was MISSING here; the loader (load_prefs_tail_*) reads
    // it at tail[8..11], so omitting it shifted autoadd_config/max_hops/rx_boost
    // and the scope fields by 4 bytes on every load (auto-add types read garbage
    // -> appeared to reset on reboot). Writing it realigns save with load.
    file.write((uint8_t *)&_prefs.gps_interval, sizeof(_prefs.gps_interval));               // 86
    file.write((uint8_t *)&_prefs.autoadd_config, sizeof(_prefs.autoadd_config));           // 90
    file.write((uint8_t *)&_prefs.autoadd_max_hops, sizeof(_prefs.autoadd_max_hops));       // 91
    file.write((uint8_t *)&_prefs.rx_boosted_gain, sizeof(_prefs.rx_boosted_gain));         // 92
    file.write((uint8_t *)_prefs.default_scope_name, sizeof(_prefs.default_scope_name));    // 93
    size_t last = file.write((uint8_t *)_prefs.default_scope_key, sizeof(_prefs.default_scope_key)); // 125

    file.close();
    if (last != sizeof(_prefs.default_scope_key)) {
      _fs->remove(_rp("/new_prefs.tmp"));   // short write (storage full?) — keep the old main file
      return false;
    }
    // _rp() returns a pointer to one shared scratch buffer when the store has
    // a root prefix (for example SD:/meshcomod). Calling it twice in rename()
    // therefore aliases both arguments to whichever path was formatted last,
    // leaving new_prefs.tmp stranded even though the data itself was written.
    // Keep the source and destination in independent buffers for the swap.
    char tmp_path[48];
    char live_path[48];
    snprintf(tmp_path, sizeof(tmp_path), "%s/new_prefs.tmp", _root);
    snprintf(live_path, sizeof(live_path), "%s/new_prefs", _root);
    _fs->remove(live_path);
    return _fs->rename(tmp_path, live_path);
  }
  return false;
}

void DataStore::loadContacts(DataStoreHost* host) {
#if defined(ESP32)
  // Recover an atomic-save swap interrupted by power loss: if the live file is gone but the
  // fully-written temp survives, adopt it. A temp alongside an intact live file is a stale
  // leftover (save crashed after writing temp but before the swap) — keep live, drop temp.
  {
    FILESYSTEM* cfs = _getContactsChannelsFS();
    char live[80], tmp[80];
    if (_root[0]) { snprintf(live, sizeof live, "%s/contacts3", _root);
                    snprintf(tmp,  sizeof tmp,  "%s/contacts3.tmp", _root); }
    else          { strncpy(live, "/contacts3", sizeof live);
                    strncpy(tmp,  "/contacts3.tmp", sizeof tmp); }
    if (cfs->exists(tmp)) {
      if (!cfs->exists(live)) cfs->rename(tmp, live);   // interrupted swap -> recover the temp
      else                    cfs->remove(tmp);          // stale temp -> the live file wins
    }
  }
#endif
File file = openRead(_getContactsChannelsFS(), "/contacts3");
    if (file) {
      bool full = false;
      while (!full) {
        ContactInfo c;
        uint8_t pub_key[32];
        uint8_t unused;

        bool success = (file.read(pub_key, 32) == 32);
        success = success && (file.read((uint8_t *)&c.name, 32) == 32);
        success = success && (file.read(&c.type, 1) == 1);
        success = success && (file.read(&c.flags, 1) == 1);
        success = success && (file.read(&unused, 1) == 1);
        success = success && (file.read((uint8_t *)&c.sync_since, 4) == 4); // was 'reserved'
        success = success && (file.read((uint8_t *)&c.out_path_len, 1) == 1);
        success = success && (file.read((uint8_t *)&c.last_advert_timestamp, 4) == 4);
        success = success && (file.read(c.out_path, 64) == 64);
        success = success && (file.read((uint8_t *)&c.lastmod, 4) == 4);
        success = success && (file.read((uint8_t *)&c.gps_lat, 4) == 4);
        success = success && (file.read((uint8_t *)&c.gps_lon, 4) == 4);

        if (!success) break; // EOF

        // Skip blank records. beta_60 saved the core's reserved anon slots (the
        // save walked the raw table, which 1.17 had shifted), so an upgrade from
        // it carries up to MAX_ANON_CONTACTS empty entries that would otherwise
        // load as real, nameless contacts. The next save drops them for good.
        if (c.type == ADV_TYPE_NONE || !c.name[0]) continue;

        c.id = mesh::Identity(pub_key);
        if (!host->onContactLoaded(c)) full = true;
      }
      file.close();
    }
}

#if defined(ESP32)
// A full contacts write REWRITES THE WHOLE TABLE — 152 bytes per contact, so 304 KB at
// MAX_CONTACTS=2000 — and the atomic swap below keeps a same-sized .tmp alongside it, so
// the peak cost is ~608 KB of a card-less V4's 3.375 MB SPIFFS volume that is already
// carrying one blob file per contact. SPIFFS garbage collection suspends the flash cache,
// which stalls BOTH cores, and it gets dramatically worse the fuller the volume — that is
// the #222 freeze, and taking the eviction blob delete off the packet path only removed
// the smaller half of it (the reporter still saw stalls, and LONGER ones, because this
// rewrite is what was left).
//
// But the table only ever CHANGES one slot at a time: eviction replaces contacts[oldest]
// in place (the array is unsorted and never shifts), and an advert refresh touches a
// single entry. So compare each record we would write against the record already on disk
// and patch only the ones that actually differ — 152 bytes instead of 304 KB, with no
// .tmp and no free-space spike. Reads never trigger GC, so the scan itself is cheap.
//
// Returns true only if the live file is now fully up to date. False means "not
// applicable" — the caller must fall back to the full atomic rewrite. This never
// truncates or renames, and the fallback rewrite repairs a partial patch, so a false
// return always ends with a valid list on disk.
bool DataStore::saveContactsInPlace(DataStoreHost* host, bool (*filter)(const ContactInfo& c)) {
  static const size_t REC = 152;             // MUST match the record packing in saveContacts
  static const size_t CHUNK_RECS = 16;

  FILESYSTEM* fs = _getContactsChannelsFS();
  if (!fs->exists(_rp("/contacts3"))) return false;    // no live file yet -> full write

  // Count first, with no I/O at all. A table that SHRANK needs records dropped off the
  // end and this FS API has no truncate, so bail BEFORE touching the file — that keeps
  // the fallback a clean rewrite instead of a half-patched one.
  uint32_t nrec = 0;
  {
    uint32_t idx = 0;
    ContactInfo c;
    while (host->getContactForSave(idx, c)) {
      if (!filter || filter(c)) nrec++;
      idx++;
    }
  }

  File f = fs->open(_rp("/contacts3"), "r+");
  if (!f) return false;
  const size_t fsz = f.size();
  if (fsz == 0 || (fsz % REC) != 0 || (fsz / REC) > nrec) { f.close(); return false; }
  const uint32_t nfile = fsz / REC;          // records currently on disk

  uint8_t* wbuf = (uint8_t*)malloc(REC * CHUNK_RECS);
  uint8_t* rbuf = (uint8_t*)malloc(REC * CHUNK_RECS);
  if (!wbuf || !rbuf) { free(wbuf); free(rbuf); f.close(); return false; }

  const uint32_t t0 = millis();
  bool ok = true;
  uint32_t base = 0;                         // record number of wbuf[0]
  size_t fill = 0;                           // records packed in wbuf
  uint32_t written = 0;                      // records actually re-written (the cost)

  // Reconcile one slab: read what is there, and write back only the records that differ.
  // Records at or past the old end of file are appends (the table grew), written in
  // ascending order so each one extends the file contiguously.
  auto reconcile = [&](uint32_t at, size_t count) -> bool {
    const size_t off = (size_t)at * REC;
    const size_t bytes = count * REC;
    size_t in_file = (at < nfile) ? ((size_t)(nfile - at) * REC) : 0;
    if (in_file > bytes) in_file = bytes;
    if (in_file) {
      if (!f.seek(off)) return false;
      if ((size_t)f.read(rbuf, in_file) != in_file) return false;
      if (in_file == bytes && memcmp(rbuf, wbuf, bytes) == 0) return true;   // slab unchanged
    }
    bool touched = false;
    for (size_t k = 0; k < count; k++) {
      const size_t ro = k * REC;
      if (ro + REC <= in_file && memcmp(rbuf + ro, wbuf + ro, REC) == 0) continue;
      if (!f.seek(off + ro)) return false;
      if (f.write(wbuf + ro, REC) != REC) return false;
      written++;
      touched = true;
    }
    // Read back what we just wrote and PROVE it landed. Positioned writes through
    // fopen("r+") are a far less travelled path than the whole-file rewrite this
    // replaces, it differs per filesystem (SPIFFS / FAT / LittleFS), and the data at
    // stake is the operator's contact list — the one thing worth more than the freeze
    // this fixes. If the read-back disagrees for any reason at all, bail and let the
    // caller do the full atomic rewrite, which is the known-good path. Costs one extra
    // read of only the slab that changed, and reads never trigger GC.
    if (touched) {
      if (!f.seek(off)) return false;
      if ((size_t)f.read(rbuf, bytes) != bytes) return false;
      if (memcmp(rbuf, wbuf, bytes) != 0) return false;
    }
    return true;
  };

  uint32_t idx = 0;
  ContactInfo c;
  uint8_t unused = 0;
  while (ok) {
    if (!host->getContactForSave(idx, c)) break;
    idx++;
    if (filter && !filter(c)) continue;

    uint8_t* p = wbuf + fill * REC;
    memcpy(p, c.id.pub_key, 32);                       p += 32;
    memcpy(p, (uint8_t *)&c.name, 32);                 p += 32;
    *p++ = c.type;
    *p++ = c.flags;
    *p++ = unused;
    memcpy(p, (uint8_t *)&c.sync_since, 4);            p += 4;
    memcpy(p, (uint8_t *)&c.out_path_len, 1);          p += 1;
    memcpy(p, (uint8_t *)&c.last_advert_timestamp, 4); p += 4;
    memcpy(p, c.out_path, 64);                         p += 64;
    memcpy(p, (uint8_t *)&c.lastmod, 4);               p += 4;
    memcpy(p, (uint8_t *)&c.gps_lat, 4);               p += 4;
    memcpy(p, (uint8_t *)&c.gps_lon, 4);               p += 4;
    fill++;

    if (fill == CHUNK_RECS) { ok = reconcile(base, fill); base += fill; fill = 0; }
  }
  if (ok && fill > 0) { ok = reconcile(base, fill); base += fill; fill = 0; }

  f.close();
  free(wbuf);
  free(rbuf);
  // base is the number of records reconciled; if it disagrees with the pre-count the
  // table changed underneath us — let the caller rewrite rather than trust the file.
  if (!(ok && base == nrec)) return false;
  _cs_recs = (uint16_t)(written > 0xFFFF ? 0xFFFF : written);
  _cs_ms = (uint16_t)((millis() - t0) > 0xFFFF ? 0xFFFF : (millis() - t0));
  _cs_in_place = true;
  _cs_any = true;
  return true;
}
#endif

void DataStore::saveContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c)) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; DataStoreHost* h; bool (*f)(const ContactInfo&); } a{ this, host, filter };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->d->saveContacts(a->h, a->f); }, &a);
    return;
  }
#endif
#if defined(ESP32)
  // A full/fragmenting contacts write on SPIFFS can trigger a multi-second GC
  // pass that disables the flash cache and starves core 0's idle task, tripping
  // the task watchdog (the beta_25 reboot loop, confirmed by coredump: loopTask
  // stuck in spiffs_gc_clean under this exact call). Suspend that watchdog for
  // the write so a bounded-but-slow pass stalls the UI instead of rebooting. The
  // ref-count is shared with the UI history/backup guards, so a backup import
  // that calls saveContacts stays balanced. On SD-routed devices this is cheap
  // (FAT has no such GC); it matters most for card-less SPIFFS devices.
  WdtHeavyGuard _wdt;

  // Patch the live file in place when only individual records changed — the normal case
  // by far, and the one that was freezing card-less V4s (#222). Falls through to the full
  // atomic rewrite below whenever that is not provably safe.
  if (saveContactsInPlace(host, filter)) return;
  const uint32_t t0_full = millis();     // the expensive path — worth reporting when it runs
#endif
#if defined(ESP32)
  // Write to a TEMP file and swap it in only after it is FULLY written, so a partial
  // write — a multi-second SPIFFS GC stall or an SD hiccup mid-save — can never truncate
  // the operator's live contact list. Without this, openWrite("/contacts3") truncates the
  // real file up front, so any failed write permanently destroyed the list (the "lost all
  // my repeaters overnight" report). Other platforms keep the direct write (LittleFS, and
  // untested here). The swap + boot-recovery live after the write loop below.
  const char* kContactsWriteTarget = "/contacts3.tmp";
#else
  const char* kContactsWriteTarget = "/contacts3";
#endif
  File file = openWrite(_getContactsChannelsFS(), kContactsWriteTarget);
  bool wrote_ok = false;                     // true only if every record reached the file
  uint32_t recs_out = 0;                     // records written by this (full) rewrite
  if (file) {
    bool ok = true;                          // cleared on any short write (partial file)
    uint32_t idx = 0;
    ContactInfo c;
    uint8_t unused = 0;

    // Field-by-field writes cost ~12 FS calls per contact — ~7000 tiny writes at
    // ~570 contacts measured 2.6-3.0 s on SD, freezing the loop/UI for the whole
    // save (issue #82). Pack the SAME 152-byte records into a heap chunk and
    // flush in ~5 KB slabs instead: identical file bytes, ~20 large writes, tens
    // of ms. Falls back to the original per-field path if the alloc fails.
    static const size_t REC = 152;           // 32+32+1+1+1+4+1+4+64+4+4+4
    static const size_t CHUNK_RECS = 32;
    uint8_t* buf = (uint8_t*)malloc(REC * CHUNK_RECS);
    if (buf) {
      size_t fill = 0;
      while (ok && host->getContactForSave(idx, c)) {
        if (filter && !filter(c)) {
          idx++;  // advance to next contact
          continue;
        }
        uint8_t* p = buf + fill;
        memcpy(p, c.id.pub_key, 32);                       p += 32;
        memcpy(p, (uint8_t *)&c.name, 32);                 p += 32;
        *p++ = c.type;
        *p++ = c.flags;
        *p++ = unused;
        memcpy(p, (uint8_t *)&c.sync_since, 4);            p += 4;
        memcpy(p, (uint8_t *)&c.out_path_len, 1);          p += 1;
        memcpy(p, (uint8_t *)&c.last_advert_timestamp, 4); p += 4;
        memcpy(p, c.out_path, 64);                         p += 64;
        memcpy(p, (uint8_t *)&c.lastmod, 4);               p += 4;
        memcpy(p, (uint8_t *)&c.gps_lat, 4);               p += 4;
        memcpy(p, (uint8_t *)&c.gps_lon, 4);               p += 4;
        fill += REC;
        recs_out++;
        if (fill == REC * CHUNK_RECS) {
          ok = (file.write(buf, fill) == fill);
          fill = 0;
        }
        idx++;  // advance to next contact
      }
      if (ok && fill > 0) ok = (file.write(buf, fill) == fill);
      free(buf);
    } else {
      while (host->getContactForSave(idx, c)) {
        if (filter && !filter(c)) {
          idx++;  // advance to next contact
          continue;
        }
        bool success = (file.write(c.id.pub_key, 32) == 32);
        success = success && (file.write((uint8_t *)&c.name, 32) == 32);
        success = success && (file.write(&c.type, 1) == 1);
        success = success && (file.write(&c.flags, 1) == 1);
        success = success && (file.write(&unused, 1) == 1);
        success = success && (file.write((uint8_t *)&c.sync_since, 4) == 4);
        success = success && (file.write((uint8_t *)&c.out_path_len, 1) == 1);
        success = success && (file.write((uint8_t *)&c.last_advert_timestamp, 4) == 4);
        success = success && (file.write(c.out_path, 64) == 64);
        success = success && (file.write((uint8_t *)&c.lastmod, 4) == 4);
        success = success && (file.write((uint8_t *)&c.gps_lat, 4) == 4);
        success = success && (file.write((uint8_t *)&c.gps_lon, 4) == 4);

        if (!success) { ok = false; break; } // write failed -> keep the live file

        recs_out++;
        idx++;  // advance to next contact
      }
    }
    file.close();
    wrote_ok = ok;
  }
#if defined(ESP32)
  // Commit the temp only if it was written IN FULL; otherwise keep the existing live list.
  // (_rp uses one shared buffer, so build both paths explicitly to avoid clobbering it.)
  {
    FILESYSTEM* cfs = _getContactsChannelsFS();
    char live[80], tmp[80];
    if (_root[0]) { snprintf(live, sizeof live, "%s/contacts3", _root);
                    snprintf(tmp,  sizeof tmp,  "%s/contacts3.tmp", _root); }
    else          { strncpy(live, "/contacts3", sizeof live);
                    strncpy(tmp,  "/contacts3.tmp", sizeof tmp); }
    if (wrote_ok) {
      cfs->remove(live);          // FAT/SPIFFS rename won't overwrite an existing target
      cfs->rename(tmp, live);     // if this rename fails, loadContacts recovers the temp at next boot
    } else {
      cfs->remove(tmp);           // discard the partial temp; the live contact list is untouched
    }
  }
  {
    const uint32_t el = millis() - t0_full;
    _cs_recs = (uint16_t)(recs_out > 0xFFFF ? 0xFFFF : recs_out);
    _cs_ms = (uint16_t)(el > 0xFFFF ? 0xFFFF : el);
    _cs_in_place = false;
    _cs_any = true;
  }
#endif
}

void DataStore::loadChannels(DataStoreHost* host) {
    File file = openRead(_getContactsChannelsFS(), "/channels2");
    if (file) {
      bool full = false;
      uint8_t channel_idx = 0;
      while (!full) {
        ChannelDetails ch;
        uint8_t unused[4];

        bool success = (file.read(unused, 4) == 4);
        success = success && (file.read((uint8_t *)ch.name, 32) == 32);
        success = success && (file.read((uint8_t *)ch.channel.secret, 32) == 32);

        if (!success) break; // EOF

        if (host->onChannelLoaded(channel_idx, ch)) {
          channel_idx++;
        } else {
          full = true;
        }
      }
      file.close();
    }
}

void DataStore::saveChannels(DataStoreHost* host) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; DataStoreHost* h; } a{ this, host };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->d->saveChannels(a->h); }, &a);
    return;
  }
#endif
  File file = openWrite(_getContactsChannelsFS(), "/channels2");
  if (file) {
    uint8_t channel_idx = 0;
    ChannelDetails ch;
    uint8_t unused[4];
    memset(unused, 0, 4);

    while (host->getChannelForSave(channel_idx, ch)) {
      bool success = (file.write(unused, 4) == 4);
      success = success && (file.write((uint8_t *)ch.name, 32) == 32);
      success = success && (file.write((uint8_t *)ch.channel.secret, 32) == 32);

      if (!success) break; // write failed
      channel_idx++;
    }
    file.close();
  }
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)

#define MAX_ADVERT_PKT_LEN   (2 + 32 + PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + MAX_ADVERT_DATA_SIZE)

struct BlobRec {
  uint32_t timestamp;
  uint8_t  key[7];
  uint8_t  len;
  uint8_t  data[MAX_ADVERT_PKT_LEN];
};

void DataStore::checkAdvBlobFile() {
  if (!_getContactsChannelsFS()->exists("/adv_blobs")) {
    File file = openWrite(_getContactsChannelsFS(), "/adv_blobs");
    if (file) {
      BlobRec zeroes;
      memset(&zeroes, 0, sizeof(zeroes));
      for (int i = 0; i < MAX_BLOBRECS; i++) {     // pre-allocate to fixed size
        file.write((uint8_t *) &zeroes, sizeof(zeroes));
      }
      file.close();
    }
  }
}

void DataStore::migrateToSecondaryFS() {
  // migrate old adv_blobs, contacts3 and channels2 files to secondary FS if they don't already exist
  if (!_fsExtra->exists("/adv_blobs")) {
    if (_fs->exists("/adv_blobs")) {
    File oldAdvBlobs = openRead(_fs, "/adv_blobs");
    File newAdvBlobs = openWrite(_fsExtra, "/adv_blobs");

    if (oldAdvBlobs && newAdvBlobs) {
      BlobRec rec;
      size_t count = 0;

      // Copy 20 BlobRecs from old to new
      while (count < 20 && oldAdvBlobs.read((uint8_t *)&rec, sizeof(rec)) == sizeof(rec)) {
        newAdvBlobs.seek(count * sizeof(BlobRec));
        newAdvBlobs.write((uint8_t *)&rec, sizeof(rec));
        count++;
      }
    }
    if (oldAdvBlobs) oldAdvBlobs.close();
    if (newAdvBlobs) newAdvBlobs.close();
    _fs->remove("/adv_blobs");
    }
  }
  if (!_fsExtra->exists("/contacts3")) {
    if (_fs->exists("/contacts3")) {
      File oldFile = openRead(_fs, "/contacts3");
      File newFile = openWrite(_fsExtra, "/contacts3");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fs->remove("/contacts3");
    }
  }
  if (!_fsExtra->exists("/channels2")) {
    if (_fs->exists("/channels2")) {
      File oldFile = openRead(_fs, "/channels2");
      File newFile = openWrite(_fsExtra, "/channels2");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fs->remove("/channels2");
    }
  }
  // cleanup nodes which have been testing the extra fs, copy _main.id and new_prefs back to primary
  if (_fsExtra->exists("/_main.id")) {
      if (_fs->exists("/_main.id")) {_fs->remove("/_main.id");}
      File oldFile = openRead(_fsExtra, "/_main.id");
      File newFile = openWrite(_fs, "/_main.id");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fsExtra->remove("/_main.id");
  }
  if (_fsExtra->exists("/new_prefs")) {
    if (_fs->exists("/new_prefs")) {_fs->remove("/new_prefs");}
      File oldFile = openRead(_fsExtra, "/new_prefs");
      File newFile = openWrite(_fs, "/new_prefs");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fsExtra->remove("/new_prefs");
  }
  // remove files from where they should not be anymore
  if (_fs->exists("/adv_blobs")) {
    _fs->remove("/adv_blobs");
  }
  if (_fs->exists("/contacts3")) {
    _fs->remove("/contacts3");
  }
  if (_fs->exists("/channels2")) {
    _fs->remove("/channels2");
  }
  if (_fsExtra->exists("/_main.id")) {
    _fsExtra->remove("/_main.id");
  }
  if (_fsExtra->exists("/new_prefs")) {
    _fsExtra->remove("/new_prefs");
  }
}

uint8_t DataStore::getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) {
  File file = openRead(_getContactsChannelsFS(), "/adv_blobs");
  uint8_t len = 0;  // 0 = not found
  if (file) {
    BlobRec tmp;
    while (file.read((uint8_t *) &tmp, sizeof(tmp)) == sizeof(tmp)) {
      if (memcmp(key, tmp.key, sizeof(tmp.key)) == 0) {  // only match by 7 byte prefix
        len = tmp.len;
        memcpy(dest_buf, tmp.data, len);
        break;
      }
    }
    file.close();
  }
  return len;
}

bool DataStore::putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; const uint8_t* k; int kl; const uint8_t* b; uint8_t l; bool r; } a{ this, key, key_len, src_buf, len, false };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->r = a->d->putBlobByKey(a->k, a->kl, a->b, a->l); }, &a);
    return a.r;
  }
#endif
  if (len < PUB_KEY_SIZE+4+SIGNATURE_SIZE || len > MAX_ADVERT_PKT_LEN) return false;
  checkAdvBlobFile();
  File file = _getContactsChannelsFS()->open("/adv_blobs", FILE_O_WRITE);
  if (file) {
    uint32_t pos = 0, found_pos = 0;
    uint32_t min_timestamp = 0xFFFFFFFF;

    // search for matching key OR evict by oldest timestmap
    BlobRec tmp;
    file.seek(0);
    while (file.read((uint8_t *) &tmp, sizeof(tmp)) == sizeof(tmp)) {
      if (memcmp(key, tmp.key, sizeof(tmp.key)) == 0) {  // only match by 7 byte prefix
        found_pos = pos;
        break;
      }
      if (tmp.timestamp < min_timestamp) {
        min_timestamp = tmp.timestamp;
        found_pos = pos;
      }

      pos += sizeof(tmp);
    }

    memcpy(tmp.key, key, sizeof(tmp.key));  // just record 7 byte prefix of key
    memcpy(tmp.data, src_buf, len);
    tmp.len = len;
    tmp.timestamp = _clock->getCurrentTime();

    file.seek(found_pos);
    file.write((uint8_t *) &tmp, sizeof(tmp));

    file.close();
    return true;
  }
  return false; // error
}
bool DataStore::deleteBlobByKey(const uint8_t key[], int key_len) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; const uint8_t* k; int kl; bool r; } a{ this, key, key_len, false };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->r = a->d->deleteBlobByKey(a->k, a->kl); }, &a);
    return a.r;
  }
#endif
  return true; // this is just a stub on NRF52/STM32 platforms
}
#else
inline void makeBlobPath(const uint8_t key[], int key_len, char* path, size_t path_size) {
  char fname[18];
  if (key_len > 8) key_len = 8; // just use first 8 bytes (prefix)
  mesh::Utils::toHex(fname, key, key_len);
  sprintf(path, "/bl/%s", fname);
}

uint8_t DataStore::getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) {
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  if (_fs->exists(_rp(path))) {
    File f = openRead(_fs, path);
    if (f) {
      int len = f.read(dest_buf, 255); // currently MAX 255 byte blob len supported!!
      f.close();
      return len;
    }
  }
  return 0; // not found
}

bool DataStore::putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; const uint8_t* k; int kl; const uint8_t* b; uint8_t l; bool r; } a{ this, key, key_len, src_buf, len, false };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->r = a->d->putBlobByKey(a->k, a->kl, a->b, a->l); }, &a);
    return a.r;
  }
#endif
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  File f = openWrite(_fs, path);
  if (f) {
    int n = f.write(src_buf, len);
    f.close();
    if (n == len) return true; // success!

    _fs->remove(_rp(path)); // blob was only partially written!
  }
  return false; // error
}

bool DataStore::deleteBlobByKey(const uint8_t key[], int key_len) {
#if defined(HAS_TDISPLAY_P4)
  if (!p4OnStorageTask()) {
    struct A { DataStore* d; const uint8_t* k; int kl; bool r; } a{ this, key, key_len, false };
    p4StorageCall([](void* p){ auto* a = (A*)p; a->r = a->d->deleteBlobByKey(a->k, a->kl); }, &a);
    return a.r;
  }
#endif
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  _fs->remove(_rp(path));

  return true; // return true even if file did not exist
}
#endif

#if defined(ESP32)
// Redirect all data storage to the SD card under /meshcomod (identity, prefs,
// contacts, channels, blobs). The SD card must already be mounted by the caller.
// Creates the folders, repoints _fs + the identity store, and sets the path
// prefix so every subsequent read/write/exists/remove lands under /meshcomod.
bool DataStore::useSdStorage() {
  if (!SD.exists("/meshcomod"))          SD.mkdir("/meshcomod");
  if (!SD.exists("/meshcomod/bl"))       SD.mkdir("/meshcomod/bl");
  if (!SD.exists("/meshcomod/identity")) SD.mkdir("/meshcomod/identity");
  strncpy(_root, "/meshcomod", sizeof(_root) - 1);
  _root[sizeof(_root) - 1] = '\0';
  _fs = &SD;
  _fsExtra = nullptr;
  identity_store.use(SD, "/meshcomod/identity");
  return true;
}
#endif

#if defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4) || defined(HAS_WIO_TRACKER_L2)
// Full-store adoption on an SD_MMC slot. P4 callers migrate internal files
// first; the Wio Tracker L2 mounts and adopts the card directly at boot.
bool DataStore::useSdMmcStorage() {
  if (!SD_MMC.exists("/meshcomod"))          SD_MMC.mkdir("/meshcomod");
  if (!SD_MMC.exists("/meshcomod/bl"))       SD_MMC.mkdir("/meshcomod/bl");
  if (!SD_MMC.exists("/meshcomod/identity")) SD_MMC.mkdir("/meshcomod/identity");
  // The old secondary-FS layout kept contacts/channels/blobs at the CARD ROOT;
  // pull them under /meshcomod so the rooted store keeps reading them.
  static const char* k_move[] = { "/contacts3", "/channels2", "/adv_blobs" };
  for (const char* nm : k_move) {
    char dst[40];
    snprintf(dst, sizeof dst, "/meshcomod%s", nm);
    if (SD_MMC.exists(nm) && !SD_MMC.exists(dst)) SD_MMC.rename(nm, dst);
  }
  strncpy(_root, "/meshcomod", sizeof(_root) - 1);
  _root[sizeof(_root) - 1] = '\0';
  _fs = &SD_MMC;
  _fsExtra = nullptr;
  identity_store.use(SD_MMC, "/meshcomod/identity");
  return true;
}
#endif

#if defined(HAS_TDISPLAY_P4)
// #167: core-0 storage dispatcher. The DSI panel's frame-restart ISR lives on core 1 (where
// Arduino's loop task also runs); SDMMC/FATFS write paths entered FROM core 1 mask that core's
// interrupts inside their critical sections long enough to drop a display frame -- the
// whole-screen flash. Tiles write the same card from the core-0 tile task without ever
// flashing; this moves every hot store write onto the same quiet core. The caller BLOCKS until
// the write completes, so call-site semantics are exactly as before.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

struct P4StoreJob { void (*fn)(void*); void* arg; SemaphoreHandle_t done; };
static QueueHandle_t s_p4store_q = nullptr;

static void p4StoreTaskFn(void*) {
  P4StoreJob j;
  for (;;) {
    if (xQueueReceive(s_p4store_q, &j, portMAX_DELAY) == pdTRUE) {
      j.fn(j.arg);
      xSemaphoreGive(j.done);
    }
  }
}

static TaskHandle_t s_p4store_task = nullptr;

bool p4OnStorageTask() { return xTaskGetCurrentTaskHandle() == s_p4store_task && s_p4store_task != nullptr; }

void p4StorageCall(void (*fn)(void*), void* arg) {
  // Guard on TASK IDENTITY, not core. The original core guard was written believing the main
  // task ran on core 1; it runs on core 0 (ESP_MAIN_TASK_AFFINITY_CPU0), so the guard
  // short-circuited and every "hopped" write actually ran inline on the main task -- proven by
  // the sector-level trace (all SDIO on task 'main'). The storage task is pinned to CORE 1,
  // which is essentially empty on this board: the DSI frame-restart ISR, the main (UI+mesh)
  // task, the SDMMC ISR and the tile task all live on core 0, so SD critical sections executed
  // on core 1 can never mask the display's interrupt regardless of what the UI is rendering.
  if (p4OnStorageTask()) { fn(arg); return; }   // re-entry on the storage task
  static SemaphoreHandle_t s_mtx = nullptr;
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();      // races only at first-ever call during boot (single-threaded)
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (!s_p4store_q) {
    s_p4store_q = xQueueCreate(1, sizeof(P4StoreJob));
    xTaskCreatePinnedToCore(p4StoreTaskFn, "store1", 8192, nullptr, 3, &s_p4store_task, 0);   // core 0: the SDMMC completion ISR lives there; core-1 I/O crawls on cross-core wakeups
  }
  static SemaphoreHandle_t s_done = nullptr;        // one in flight (serialized by s_mtx)
  if (!s_done) s_done = xSemaphoreCreateBinary();
  P4StoreJob j{ fn, arg, s_done };
  xQueueSend(s_p4store_q, &j, portMAX_DELAY);
  xSemaphoreTake(s_done, portMAX_DELAY);
  xSemaphoreGive(s_mtx);
}
#endif
