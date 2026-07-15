// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from the core fork's CustomLR1110Wrapper.h — see CustomLR1121.h for
// why this lives in the variant dir instead of the core fork.
#pragma once

#include "CustomLR1121.h"
// Angle brackets with the full core-lib subpath, NOT the core's own bare
// quoted "RadioLibWrappers.h" -- CustomLR1110Wrapper.h gets away with that
// because it lives in the same src/helpers/radiolib/ directory (quote-include
// searches the including file's own dir first); this file lives in the
// variant dir instead, so a bare quoted include can't find them.
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/LR11x0Reset.h>

class CustomLR1121Wrapper : public RadioLibWrapper {
public:
  CustomLR1121Wrapper(CustomLR1121& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    ((CustomLR1121 *)_radio)->setFrequency(freq);
    ((CustomLR1121 *)_radio)->setSpreadingFactor(sf);
    ((CustomLR1121 *)_radio)->setBandwidth(bw);
    ((CustomLR1121 *)_radio)->setCodingRate(cr);
    updatePreamble(sf);
  }

  void doResetAGC() override { lr11x0ResetAGC((LR11x0 *)_radio, ((CustomLR1121 *)_radio)->getFreqMHz()); }
  bool isReceivingPacket() override {
    return ((CustomLR1121 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    float rssi = -110;
    ((CustomLR1121 *)_radio)->getRssiInst(&rssi);
    return rssi;
  }

  void onSendFinished() override {
    RadioLibWrapper::onSendFinished();
    _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor())); // overcomes weird issues with small and big pkts
  }

  float getLastRSSI() const override { return ((CustomLR1121 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomLR1121 *)_radio)->getSNR(); }

  uint8_t getSpreadingFactor() const override { return ((CustomLR1121 *)_radio)->getSpreadingFactor(); }

  void setRxBoostedGainMode(bool en) override {
    ((CustomLR1121 *)_radio)->setRxBoostedGainMode(en);
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomLR1121 *)_radio)->getRxBoostedGainMode();
  }
};
