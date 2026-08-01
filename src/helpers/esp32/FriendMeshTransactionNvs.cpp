#include "FriendMeshTransactionNvs.h"
#include "friendmesh/core/FriendMeshTransaction.h"

#if defined(ESP32)

#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* slotKey(uint8_t slot) {
  return slot == 0 ? "journal0" : slot == 1 ? "journal1" : nullptr;
}

}  // namespace

bool friendmeshTransactionNvsLoad(uint8_t slot, uint8_t* destination,
                                  size_t capacity, size_t& length) {
  length = 0;
  const char* key = slotKey(slot);
  if (!key || !destination || capacity == 0) return false;
  Preferences preferences;
  if (!preferences.begin("fm_txn", true)) return false;
  const size_t stored = preferences.getBytesLength(key);
  if (stored == 0 || stored > capacity) {
    preferences.end();
    return false;
  }
  const size_t read = preferences.getBytes(key, destination, capacity);
  preferences.end();
  if (read != stored) {
    memset(destination, 0, capacity);
    return false;
  }
  length = read;
  return true;
}

bool friendmeshTransactionNvsPresent(uint8_t slot) {
  const char* key = slotKey(slot);
  if (!key) return false;
  Preferences preferences;
  if (!preferences.begin("fm_txn", true)) return false;
  const bool present = preferences.isKey(key);
  preferences.end();
  return present;
}

bool friendmeshTransactionNvsSave(uint8_t slot, const uint8_t* source,
                                  size_t length) {
  const char* key = slotKey(slot);
  if (!key || !source || length == 0) return false;
  Preferences preferences;
  if (!preferences.begin("fm_txn", false)) return false;
  const bool written = preferences.putBytes(key, source, length) == length;
  preferences.end();
  if (!written) return false;

  uint8_t* verify = static_cast<uint8_t*>(malloc(length));
  if (!verify) return false;
  memset(verify, 0, length);
  size_t verifiedLength = 0;
  const bool verified = friendmeshTransactionNvsLoad(
      slot, verify, length, verifiedLength) &&
      verifiedLength == length && memcmp(verify, source, length) == 0;
  memset(verify, 0, length);
  free(verify);
  return verified;
}

#else

bool friendmeshTransactionNvsLoad(uint8_t, uint8_t*, size_t, size_t& length) {
  length = 0;
  return false;
}

bool friendmeshTransactionNvsPresent(uint8_t) { return false; }

bool friendmeshTransactionNvsSave(uint8_t, const uint8_t*, size_t) {
  return false;
}

#endif
