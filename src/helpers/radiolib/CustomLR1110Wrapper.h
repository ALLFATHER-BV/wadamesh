#pragma once

#include "CustomLR1110.h"
#include "RadioLibWrappers.h"
#include "LR11x0Reset.h"
#if defined(ESP32)
#include <esp_system.h>
#endif

#ifndef USE_LR1110
#define USE_LR1110
#endif

class CustomLR1110Wrapper : public RadioLibWrapper {
public:
  CustomLR1110Wrapper(CustomLR1110& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    ((CustomLR1110 *)_radio)->setFrequency(freq);
    ((CustomLR1110 *)_radio)->setSpreadingFactor(sf);
    ((CustomLR1110 *)_radio)->setBandwidth(bw);
    ((CustomLR1110 *)_radio)->setCodingRate(cr);
    updatePreamble(sf);
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    ((CustomLR1110 *)_radio)->setPreambleMillis(pm.preambleMillis);
    ((CustomLR1110 *)_radio)->setMaxPayloadMillis(pm.payloadMillis);
  }

  bool isReceivingPacket() override {
    return ((CustomLR1110 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    float rssi = -110;
    ((CustomLR1110 *)_radio)->getRssiInst(&rssi);
    return rssi;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    auto airtime = RadioLibWrapper::getEstAirtimeFor(len_bytes);
    return airtime < 200 ? 200 : airtime;   // at least 200 millis
  }

  void onSendFinished() override {
    radioAcquire();
    RadioLibWrapper::onSendFinished();
    _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor())); // overcomes weird issues with small and big pkts
    radioRelease();
  }

  float packetScore(float snr, int packet_len) override {
    int sf = getSpreadingFactor();
    return packetScoreInt(snr, sf, packet_len);
  }

  uint8_t getSpreadingFactor() const override { return ((CustomLR1110 *)_radio)->getSpreadingFactor(); }

  void powerOff() override {
    rxQueueEnable(false);
    radioAcquire();
    ((CustomLR1110 *)_radio)->sleep(false, 0);
    radioRelease();
  }

  // Recover from missed DIO1 rising edges during shared-SPI contention (see RadioLibWrapper::pollRxIfNoIrq).
  bool pollRxDone() override {
    return (((CustomLR1110 *)_radio)->getIrqStatus() & RADIOLIB_LR11X0_IRQ_RX_DONE) != 0;
  }

#if defined(ESP32)
  // Avoid 248 SPI RSSI mode-switching transactions at boot; use ESP32-S3 hardware TRNG.
  uint32_t getRngSeed() override {
    return esp_random() & 0x7FFFFFFFu;
  }
#endif

  bool setRxBoostedGainMode(bool en) override {
    return ((CustomLR1110 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomLR1110 *)_radio)->getRxBoostedGainMode();
  }

  void doResetAGC() override { lr11x0ResetAGC((LR11x0 *)_radio, ((CustomLR1110 *)_radio)->getFreqMHz(), getRxBoostedGainMode()); }
};
