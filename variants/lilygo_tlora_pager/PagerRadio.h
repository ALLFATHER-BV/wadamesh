// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include "CustomLR1121Wrapper.h"
#include "PagerRadioProbe.h"

// Both RadioLib objects use the same pins and SPI bus, but only the detected
// one is initialized. This facade also supplies the spectrum analyzer's raw
// radio operations without exposing an inactive chip.
class PagerRadio {
  CustomSX1262 sx_;
  CustomLR1121 lr_;
  PagerRadioFamily family_ = PagerRadioFamily::None;
public:
  PagerRadio(Module* sx, Module* lr) : sx_(sx), lr_(lr) {}
  CustomSX1262& sx() { return sx_; }
  CustomLR1121& lr() { return lr_; }
  PagerRadioFamily family() const { return family_; }
  void select(PagerRadioFamily family) { family_ = family; }
  PhysicalLayer* physical() {
    if (family_ == PagerRadioFamily::SX1262) return &sx_;
    if (family_ == PagerRadioFamily::LR1121) return &lr_;
    return nullptr;
  }
  const char* name() const {
    return family_ == PagerRadioFamily::SX1262 ? "SX1262" :
           family_ == PagerRadioFamily::LR1121 ? "LR1121" : "Unknown";
  }

  int16_t setFrequency(float f) {
    return family_ == PagerRadioFamily::LR1121 ? lr_.setFrequency(f, true) : sx_.setFrequency(f);
  }
  int16_t setBandwidth(float bw) {
    return family_ == PagerRadioFamily::LR1121 ? lr_.setBandwidth(bw) : sx_.setBandwidth(bw);
  }
  int16_t setRxBoostedGainMode(bool en) {
    return family_ == PagerRadioFamily::LR1121 ? lr_.setRxBoostedGainMode(en) : sx_.setRxBoostedGainMode(en);
  }
  int16_t standby() { return family_ == PagerRadioFamily::LR1121 ? lr_.standby() : sx_.standby(); }
  int16_t startReceive() { return family_ == PagerRadioFamily::LR1121 ? lr_.startReceive() : sx_.startReceive(); }
  int16_t startSpectrumReceive() {
    if (family_ == PagerRadioFamily::LR1121)
      return lr_.LR11x0::startReceive(RADIOLIB_LR11X0_RX_TIMEOUT_INF, 0, 0, 0);
    return sx_.startReceive();
  }
  float getRSSI(bool packet) {
    // LR11xx's one-argument read restarts RX on each sample; keep the sweep armed.
    return family_ == PagerRadioFamily::LR1121 ? lr_.getRSSI(packet, true) : sx_.getRSSI(packet);
  }
  void spectrumStandby() { if (family_ == PagerRadioFamily::LR1121) lr_.standby(0x01); }
  void spectrumCalibrate(float low, float high) {
    if (family_ == PagerRadioFamily::LR1121) lr_.calibrateImageRejection(low, high);
  }
};

// RadioLibWrapper owns the receive queue, IRQ state and the mesh-facing radio
// interface. Bind that single shared state to the detected physical layer and
// dispatch only the operations that differ between the existing wrappers.
class PagerRadioWrapper : public CustomSX1262Wrapper {
  PagerRadio& pager_;
  bool lr() const { return pager_.family() == PagerRadioFamily::LR1121; }
public:
  PagerRadioWrapper(PagerRadio& radio, mesh::MainBoard& board)
      : CustomSX1262Wrapper(radio.sx(), board), pager_(radio) {}
  bool bindSelected() {
    PhysicalLayer* selected = pager_.physical();
    if (!selected) return false;
    _radio = selected;
    return true;
  }
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    if (!lr()) return CustomSX1262Wrapper::setParams(freq, bw, sf, cr);
    pager_.lr().setFrequency(freq);
    pager_.lr().setSpreadingFactor(sf);
    pager_.lr().setBandwidth(bw);
    pager_.lr().setCodingRate(cr);
    updatePreamble(sf);
  }
  bool isReceivingPacket() override {
    return lr() ? pager_.lr().isReceiving() : CustomSX1262Wrapper::isReceivingPacket();
  }
  float getCurrentRSSI() override {
    if (!lr()) return CustomSX1262Wrapper::getCurrentRSSI();
    float rssi = -110;
    pager_.lr().getRssiInst(&rssi);
    return rssi;
  }
  float getLastRSSI() const override {
    return lr() ? static_cast<CustomLR1121*>(_radio)->getRSSI() : CustomSX1262Wrapper::getLastRSSI();
  }
  float getLastSNR() const override {
    return lr() ? static_cast<CustomLR1121*>(_radio)->getSNR() : CustomSX1262Wrapper::getLastSNR();
  }
  uint8_t getSpreadingFactor() const override {
    return lr() ? static_cast<CustomLR1121*>(_radio)->getSpreadingFactor() : CustomSX1262Wrapper::getSpreadingFactor();
  }
  float packetScore(float snr, int len) override {
    return lr() ? RadioLibWrapper::packetScore(snr, len) : CustomSX1262Wrapper::packetScore(snr, len);
  }
  void onSendFinished() override {
    RadioLibWrapper::onSendFinished();
    if (lr()) _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor()));
  }
  void powerOff() override {
    if (lr()) RadioLibWrapper::powerOff(); else CustomSX1262Wrapper::powerOff();
  }
  bool pollRxDone() override { return !lr() && CustomSX1262Wrapper::pollRxDone(); }
  bool setRxBoostedGainMode(bool en) override {
    return lr() ? pager_.lr().setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE : CustomSX1262Wrapper::setRxBoostedGainMode(en);
  }
  bool getRxBoostedGainMode() const override {
    return lr() ? static_cast<CustomLR1121*>(_radio)->getRxBoostedGainMode() : CustomSX1262Wrapper::getRxBoostedGainMode();
  }
  void doResetAGC() override {
    if (lr()) lr11x0ResetAGC(&pager_.lr(), pager_.lr().getFreqMHz(), getRxBoostedGainMode());
    else CustomSX1262Wrapper::doResetAGC();
  }
};
