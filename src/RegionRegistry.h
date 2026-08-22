#pragma once

#include <Arduino.h>
#include <Packet.h>
#include <helpers/RegionMap.h>
#include <helpers/TransportKeyStore.h>

// ---- User-managed region registry (#271) ------------------------------------
// A transport scope code (transport_codes[0]) is HMAC(region key, payload type +
// payload) truncated to 16 bits. It is per-PACKET, not a region id, so the same
// sender in the same region emits a different code on every message (#259) and
// no static code->name table can ever work.
//
// The only way to name a region is to recompute the code with a key we hold and
// compare, while the packet is still in hand. MyMesh has done exactly that since
// #259, but for only ONE key: our own default scope. That is why the Info popup
// can say no more than "my region" or "another region". This holds the other
// candidate keys so it can say WHICH region instead.
//
// Storage and key derivation are MeshCore's (RegionMap + TransportKeyStore):
//   * a public "#tag" region's key is plain SHA256("#tag"), so a name is all we
//     need to store. It matches what MyMesh::setRegionScope already derives for
//     our own region, byte for byte.
//   * "$private" regions ask TransportKeyStore for an explicit 16-byte key.
//     NOTE: that side of the core store is stubbed in MeshCore 1.10, where
//     loadKeysFor() returns 0 and saveKeysFor() returns false, so a private
//     region simply never matches today. Public tags are unaffected. When the
//     core implements it, getTransportKeysFor() starts returning keys and this
//     works with no change here.
//
// Two things this deliberately does NOT reuse from RegionMap:
//   * findMatch(), which returns the FIRST matching region. With N keys live,
//     a 16-bit tag collides at roughly N/65534 per packet, and reporting a
//     confident wrong name is worse than reporting nothing. So the loop below
//     counts every match and the caller renders "ambiguous" for >1.
//   * RegionEntry::id as the value stored per message. Those ids are uint16 and
//     climb as regions are added; message history has 4 spare bits. Hence SLOTS.
//
// SLOTS: 1..14 identify a region for the lifetime of chat history, and live in
// the free top nibble of UIMessage::meta_flags. That costs no record growth and
// no history version bump, and pre-existing messages read back slot 0 = "unknown", which is
// the honest state for a message received before its region was known. Slot 15
// is the ambiguous sentinel, 0 is none/unknown. A slot is NOT a list index: it
// is assigned once and never renumbered, so deleting or reordering regions can
// never silently relabel old messages (the failure mode called out on #271).
#define REGION_SLOT_NONE       0
#define REGION_SLOT_MAX       14   // usable slots are 1..14
#define REGION_SLOT_AMBIGUOUS 15

#define REGION_REG_MAP_PATH   "/region_map.dat"
#define REGION_REG_SLOT_PATH  "/region_slots.dat"
#define REGION_REG_SLOT_MAGIC 0x52475331UL   // "RGS1": slots only
#define REGION_REG_SLOT_MAGIC2 0x52475332UL  // "RGS2": slots + active mask

class RegionRegistry {
  TransportKeyStore _keys;
  RegionMap         _map;
  // index = slot (1..REGION_SLOT_MAX); value = RegionEntry::id, 0 = free.
  // A slot binding is PERMANENT once assigned. Removing a region clears its bit
  // in _slot_active, which stops it being matched at RX, but the binding stays.
  // Messages received while it WAS registered still resolve to the right name.
  // Freeing the slot for reuse would silently relabel that history, which is the
  // failure #271 explicitly warns about.
  uint16_t          _slot_id[REGION_SLOT_MAX + 1];
  uint16_t          _slot_active = 0;   // bit (slot-1) set => participates in matching
  FILESYSTEM*       _fs = nullptr;
  bool              _ready = false;

  // Canonical form, matching MyMesh::setRegionScope exactly: trim surrounding
  // whitespace, then force a leading '#'. Case is NOT touched, because the name
  // is key material (SHA256 over these bytes), so lowercasing would silently derive a
  // different key and the region would stop matching.
  static void canonicalise(const char* in, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t b = 0, e = strlen(in);
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) b++;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) e--;
    if (b >= e) return;                       // blank => unscoped, not a region
    size_t o = 0;
    if (in[b] != '#' && in[b] != '$' && o < out_sz - 1) out[o++] = '#';
    for (size_t i = b; i < e && o < out_sz - 1; ++i) out[o++] = in[i];
    out[o] = '\0';
    if (out[0] == '#' && out[1] == '\0') out[0] = '\0';   // a bare '#' is not a region
  }

  void loadSlots() {
    memset(_slot_id, 0, sizeof _slot_id);
    _slot_active = 0;
    if (!_fs) return;
    File f = _fs->open(REGION_REG_SLOT_PATH, "r");
    if (!f) return;
    uint32_t magic = 0;
    if (f.read((uint8_t*)&magic, sizeof magic) == (int)sizeof magic
        && (magic == REGION_REG_SLOT_MAGIC || magic == REGION_REG_SLOT_MAGIC2)) {
      f.read((uint8_t*)_slot_id, sizeof _slot_id);
      if (magic == REGION_REG_SLOT_MAGIC2) {
        f.read((uint8_t*)&_slot_active, sizeof _slot_active);
      } else {
        for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s)   // pre-mask file: all assigned slots were live
          if (_slot_id[s]) _slot_active |= (uint16_t)(1u << (s - 1));
      }
    } else {
      memset(_slot_id, 0, sizeof _slot_id);
    }
    f.close();
  }

public:
  RegionRegistry() : _map(_keys) { memset(_slot_id, 0, sizeof _slot_id); }

  bool isReady() const { return _ready; }

  void begin(FILESYSTEM* fs) {
    _fs = fs;
    if (!_fs) return;
    _map.load(_fs, REGION_REG_MAP_PATH);
    loadSlots();
    _ready = true;
  }

  void save() {
    if (!_fs || !_ready) return;
    _map.save(_fs, REGION_REG_MAP_PATH);
    File f = _fs->open(REGION_REG_SLOT_PATH, "w");
    if (!f) return;
    const uint32_t magic = REGION_REG_SLOT_MAGIC2;
    f.write((const uint8_t*)&magic, sizeof magic);
    f.write((const uint8_t*)_slot_id, sizeof _slot_id);
    f.write((const uint8_t*)&_slot_active, sizeof _slot_active);
    f.close();
  }

  /** Register `name` if absent. Returns its slot, or REGION_SLOT_NONE when the
   *  name is blank/unscoped or all 14 slots are taken. Idempotent. */
  uint8_t ensureRegion(const char* name) {
    if (!_ready) return REGION_SLOT_NONE;
    char tag[31];
    canonicalise(name, tag, sizeof tag);
    if (tag[0] == '\0') return REGION_SLOT_NONE;

    RegionEntry* e = _map.findByName(tag);
    if (!e) e = _map.putRegion(tag, 0);
    if (!e) return REGION_SLOT_NONE;

    for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s) {       // already slotted?
      if (_slot_id[s] == e->id) {
        // Re-adding a retired region revives its ORIGINAL slot rather than
        // taking a new one, so its old messages and its new ones stay one region.
        if (!isActive(s)) {
          _slot_active |= (uint16_t)(1u << (s - 1));
          save();
        }
        return s;
      }
    }
    for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s) {        // take the first free
      if (_slot_id[s] == 0) {
        _slot_id[s] = e->id;
        _slot_active |= (uint16_t)(1u << (s - 1));
        save();
        return s;
      }
    }
    return REGION_SLOT_NONE;   // full: the message records "unknown", never a wrong name
  }

  /** Recompute every registered region's transport code for `pkt` and compare.
   *  Returns the matching slot, REGION_SLOT_AMBIGUOUS when more than one region
   *  matches, or REGION_SLOT_NONE. MUST be called while the packet is in hand.
   *  `code` is the received transport code being explained. */
  uint8_t matchPacket(const mesh::Packet* pkt, uint16_t code) const {
    if (!_ready || !pkt || code == 0) return REGION_SLOT_NONE;
    uint8_t  hit = REGION_SLOT_NONE;
    uint8_t  n   = 0;
    for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s) {
      if (_slot_id[s] == 0 || !isActive(s)) continue;   // retired regions keep their
                                                       // name for history, but stop matching
      RegionEntry* e = const_cast<RegionMap&>(_map).findById(_slot_id[s]);
      if (!e) continue;
      TransportKey keys[4];
      int nk = const_cast<RegionMap&>(_map).getTransportKeysFor(*e, keys, 4);
      for (int k = 0; k < nk; ++k) {
        if (keys[k].isNull()) continue;
        if (keys[k].calcTransportCode(pkt) == code) {
          if (++n > 1) return REGION_SLOT_AMBIGUOUS;
          hit = s;
          break;            // one region matching on two of its own keys is still one region
        }
      }
    }
    return hit;
  }

  /** Canonical name for a slot, or nullptr. Never returns a name for the
   *  ambiguous sentinel, so callers must render that state explicitly. */
  const char* nameForSlot(uint8_t slot) const {
    if (!_ready || slot == REGION_SLOT_NONE || slot > REGION_SLOT_MAX) return nullptr;
    if (_slot_id[slot] == 0) return nullptr;
    RegionEntry* e = const_cast<RegionMap&>(_map).findById(_slot_id[slot]);
    return (e && e->name[0]) ? e->name : nullptr;
  }

  /** True when this slot currently takes part in matching. A retired slot still
   *  resolves via nameForSlot() so old messages keep their region name. */
  bool isActive(uint8_t slot) const {
    if (slot == REGION_SLOT_NONE || slot > REGION_SLOT_MAX) return false;
    return (_slot_active & (uint16_t)(1u << (slot - 1))) != 0;
  }

  /** Stop matching this region. The slot binding and name are KEPT on purpose --
   *  see the note on _slot_id. Returns false if the slot was never assigned. */
  bool retire(uint8_t slot) {
    if (!_ready || slot == REGION_SLOT_NONE || slot > REGION_SLOT_MAX) return false;
    if (_slot_id[slot] == 0) return false;
    _slot_active &= (uint16_t)~(1u << (slot - 1));
    save();
    return true;
  }

  /** Live regions (what the user sees as "the list"). */
  int count() const {
    int n = 0;
    for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s) if (_slot_id[s] && isActive(s)) n++;
    return n;
  }

  /** True when every slot is assigned, so no NEW region can be added. Retired
   *  slots still count: their bindings are permanent. */
  bool isFull() const {
    for (uint8_t s = 1; s <= REGION_SLOT_MAX; ++s) if (_slot_id[s] == 0) return false;
    return true;
  }
};
