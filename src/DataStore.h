#pragma once

#include <helpers/IdentityStore.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include "NodePrefs.h"

class DataStoreHost {
public:
  virtual bool onContactLoaded(const ContactInfo& contact) =0;
  virtual bool getContactForSave(uint32_t idx, ContactInfo& contact) =0;
  virtual bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) =0;
  virtual bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) =0;
};

class DataStore {
  FILESYSTEM* _fs;
  FILESYSTEM* _fsExtra;
  mesh::RTCClock* _clock;
  IdentityStore identity_store;
  // Path prefix for every data file. Empty = filesystem root (flash/SPIFFS, the
  // default — zero behaviour change). Set to "/meshcomod" by useSdStorage() so
  // that, when SPIFFS is unavailable (e.g. installed under Launcher) or the user
  // opts in, all data lives in one tidy folder on the SD card. ESP32 only.
  char _root[24] = "";
  char _rpbuf[80];
  const char* _rp(const char* name);   // returns _root-prefixed path (name as-is when _root empty)

  void loadPrefsInt(const char *filename, NodePrefs& prefs, double& node_lat, double& node_lon);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  void checkAdvBlobFile();
#endif

public:
  DataStore(FILESYSTEM& fs, mesh::RTCClock& clock);
  DataStore(FILESYSTEM& fs, FILESYSTEM& fsExtra, mesh::RTCClock& clock);
  void begin();
  bool formatFileSystem();
  File openWrite(FILESYSTEM* fs, const char* filename);   // root-aware (was a free static fn)
#if defined(ESP32)
  // Redirect all data storage to the SD card under /meshcomod (identity, prefs,
  // contacts, channels, blobs). Call after the SD card is mounted. Returns false
  // if the dir can't be created. No-op on non-ESP32.
  bool useSdStorage();
#endif
#if defined(HAS_TANMATSU)
  // Full-store adoption of the SD_MMC card (Tanmatsu): identity + prefs move off
  // the broken-metadata internal FFat (its exists()/f_stat lie, which made the
  // gated loads come up empty). Caller migrates FFat-resident files first.
  bool useSdMmcStorage();
#endif
  FILESYSTEM* getPrimaryFS() const { return _fs; }
  FILESYSTEM* getSecondaryFS() const { return _fsExtra; }
  // Route contacts + channels (see _getContactsChannelsFS) to a different FS than identity/prefs.
  // Used on the Tanmatsu to keep identity/prefs on the (working) internal FFat while putting the
  // frequently-rewritten contacts/channels on the reliable SD card. Call before begin().
  void setSecondaryFS(FILESYSTEM* fs) { _fsExtra = fs; }
  bool loadMainIdentity(mesh::LocalIdentity &identity);
  bool saveMainIdentity(const mesh::LocalIdentity &identity);
  void loadPrefs(NodePrefs& prefs, double& node_lat, double& node_lon);
  // Crash-safe: writes /new_prefs.tmp, then swaps it in. Returns false when the
  // write or the swap failed (storage full / torn) so callers can surface it —
  // the save used to fail silently and the UI toasted "saved" regardless.
  bool savePrefs(const NodePrefs& prefs, double node_lat, double node_lon);
  void loadContacts(DataStoreHost* host);
  void saveContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c) = NULL);
  void loadChannels(DataStoreHost* host);
  void saveChannels(DataStoreHost* host);
  void migrateToSecondaryFS();
  uint8_t getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]);
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len);
  bool deleteBlobByKey(const uint8_t key[], int key_len);
  File openRead(const char* filename);
  File openRead(FILESYSTEM* fs, const char* filename);
  bool removeFile(const char* filename);
  bool removeFile(FILESYSTEM* fs, const char* filename);
  uint32_t getStorageUsedKb() const;
  uint32_t getStorageTotalKb() const;
#if defined(ESP32)
  // True while contacts/channels live on the internal flash FS (SPIFFS/LittleFS),
  // whose garbage collection makes a full rewrite expensive — a multi-second GC
  // pass that freezes the loop. False once routed to an SD card (useSdStorage sets
  // _root; setSecondaryFS sets _fsExtra) — FAT has no such GC. Lets callers coalesce
  // the advert-driven contacts save on card-less devices without changing SD boards.
  bool contactsOnInternalFlash() const { return _root[0] == '\0' && _fsExtra == nullptr; }
#endif

private:
  FILESYSTEM* _getContactsChannelsFS() const { if (_fsExtra) return _fsExtra; return _fs;};
};
