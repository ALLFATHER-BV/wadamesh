// SPDX-License-Identifier: GPL-3.0-or-later
//
// ThinkNode M9 LR1110 wrapper and AGC reset helper.
// Lives in variants/thinknode_m9/ (mirroring variants/lilygo_tlora_pager/'s
// CustomLR1121Wrapper.h) and includes the core library's headers via angle
// brackets so nothing under src/helpers/radiolib/ shadows core headers on
// SX1262 builds.
#pragma once

#include <helpers/radiolib/CustomLR1110.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#if defined(ESP32)
#include <esp_system.h>
#endif

#ifndef USE_LR1110
#define USE_LR1110 1
#endif

// Full receiver reset for the M9's LR1110.
// Per Semtech's lr11xx_driver reference (SWDR001 lr11xx_system.h):
// - Wake from warm sleep into STDBY_RC (0x00).
// - Re-apply SetTcxoMode in STDBY_RC because warm sleep powers down DIO3 VTCXO.
// - SetRegMode (DC-DC) MUST be called in STDBY_RC.
// - Calibrate(0x3F) runs directly from STDBY_RC (the chip powers TCXO/XOSC
//   internally as needed and returns to STDBY_RC on exit).
inline void m9Lr11x0ResetAGC(LR11x0* radio, float freqMHz, bool rx_boost_gain) {
  radio->sleep(true, 0);
  radio->standby(RADIOLIB_LR11X0_STANDBY_RC, true);
#if defined(LR11X0_DIO3_TCXO_VOLTAGE)
  if ((LR11X0_DIO3_TCXO_VOLTAGE) > 0.0f) {
    radio->setTCXO(LR11X0_DIO3_TCXO_VOLTAGE);
  }
#endif
  radio->setRegulatorDCDC();
  radio->calibrate(RADIOLIB_LR11X0_CALIBRATE_ALL);
  radio->calibrateImageRejection(freqMHz - 4.0f, freqMHz + 4.0f);
  radio->setRxBoostedGainMode(rx_boost_gain);
}

class M9LR1110Wrapper : public RadioLibWrapper {
public:
  M9LR1110Wrapper(CustomLR1110& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

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
    return airtime < 200 ? 200 : airtime;
  }

  void onSendFinished() override {
    radioAcquire();
    RadioLibWrapper::onSendFinished();
    _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor()));
    radioRelease();
  }

  float packetScore(float snr, int packet_len) override {
    int sf = getSpreadingFactor();
    return packetScoreInt(snr, sf, packet_len);
  }

  uint8_t getSpreadingFactor() const override {
    return ((CustomLR1110 *)_radio)->getSpreadingFactor();
  }

  void powerOff() override {
    rxQueueEnable(false);
    radioAcquire();
    ((CustomLR1110 *)_radio)->sleep(false, 0);
    radioRelease();
  }

  bool pollRxDone() override {
    return (((CustomLR1110 *)_radio)->getIrqStatus() & RADIOLIB_LR11X0_IRQ_RX_DONE) != 0;
  }

#if defined(ESP32)
  // Avoid 248 SPI RSSI mode-switching transactions at boot; use ESP32-S3 hardware TRNG.
  uint32_t getRngSeed() {
    return esp_random() & 0x7FFFFFFFu;
  }
#endif

  bool setRxBoostedGainMode(bool en) override {
    return ((CustomLR1110 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }

  bool getRxBoostedGainMode() const override {
    return ((CustomLR1110 *)_radio)->getRxBoostedGainMode();
  }

  void doResetAGC() override {
    m9Lr11x0ResetAGC((LR11x0 *)_radio, ((CustomLR1110 *)_radio)->getFreqMHz(), getRxBoostedGainMode());
  }
};
