// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from the core fork's CustomLR1110{,Wrapper}.h
// (src/helpers/radiolib/, MIT-licensed MeshCore code) — LR1121 and LR1110 are
// both LR11x0-family chips sharing the same RadioLib base class, so this is a
// mechanical type swap, not new logic. Lives in the variant dir rather than
// the core fork because, as of 2026-07-06, upstream meshcore-dev/MeshCore has
// no CustomLR1121* or lilygo_tlora_pager target to crib instead (see
// TLORA_PAGER_PORT.md's Decision ② caveat) — this can move into the fork at
// the next core-* tag once it's proven on hardware.
#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

class CustomLR1121 : public LR1121 {
  bool _rx_boosted = false;

  public:
    CustomLR1121(Module *mod) : LR1121(mod) { }

    size_t getPacketLength(bool update) override {
      size_t len = LR1121::getPacketLength(update);
      if (len == 0 && getIrqStatus() & RADIOLIB_LR11X0_IRQ_HEADER_ERR) {
        // we've just received a corrupted packet
        // this may have triggered a bug causing subsequent packets to be shifted
        // call standby() to return radio to known-good state
        // recvRaw will call startReceive() to restart rx
        MESH_DEBUG_PRINTLN("LR1121: got header err, calling standby()");
        standby();
      }
      return len;
    }

    float getFreqMHz() const { return freqMHz; }

    int16_t setRxBoostedGainMode(bool en) {
      _rx_boosted = en;
      return LR1121::setRxBoostedGainMode(en);
    }

    bool getRxBoostedGainMode() const { return _rx_boosted; }

    bool isReceiving() {
      uint16_t irq = getIrqStatus();
      bool detected = ((irq & RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID) || (irq & RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED));
      return detected;
    }

    uint8_t getSpreadingFactor() const { return spreadingFactor; }
};
