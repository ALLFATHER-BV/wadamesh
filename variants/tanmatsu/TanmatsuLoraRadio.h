#pragma once
//
// TanmatsuLoraRadio — bridges MeshCore's mesh::Radio onto the Nicolai Electronics
// `tanmatsu-lora` component. On the Tanmatsu the LoRa modem (SX1262/SX1268) lives
// on the ESP32-C6 radio coprocessor and is reached "remote" over the C6<->P4 link,
// so we do NOT use MeshCore's RadioLib-over-SPI wrapper here — this class is the
// drop-in replacement. Everything above mesh::Radio (the whole wadamesh app) is
// unchanged; the Tanmatsu variant just instantiates this instead of a RadioLibWrapper.
//
// Written against esp32-component-tanmatsu-lora v0.3.0 (include/lora.h) WITHOUT the
// hardware. Anything marked TODO(device) needs confirming/tuning on the real board.
//
#include <Mesh.h>                 // mesh::Radio (declared in Dispatcher.h, pulled by Mesh.h)

extern "C" {
#include "lora.h"                  // nicolaielectronics/tanmatsu-lora
}

class TanmatsuLoraRadio : public mesh::Radio {
public:
  TanmatsuLoraRadio() {}

  // Hardware bring-up: open the remote LoRa link + push the mesh's PHY config.
  // Returns false if lora_init_remote() fails. The variant's radio_init() calls
  // this (mirrors RadioLib's radio.std_init()). Call setParams() first, or after —
  // it re-pushes config when already started.
  bool init();

  // The S3 boards configure their RadioLibWrapper through this same set of calls
  // (see MyMesh.cpp); TanmatsuLoraRadio is a drop-in for `radio_driver`, so it
  // implements them too. None are part of mesh::Radio itself.
  void setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
  void setTxPower(int8_t dbm);
  void setRxBoostedGainMode(bool on);
  bool getRxBoostedGainMode() const { return _rx_boost; }
  void idle();                                             // → standby (RadioLibWrapper::idle)
  uint32_t getPacketsRecvErrors() const { return _n_recv_errors; }

  // --- mesh::Radio contract (Dispatcher.h) ---
  void     begin() override {}                             // lifecycle hook; remote radio is RX-ready after init()
  int      recvRaw(uint8_t* bytes, int sz) override;       // drain one RX packet, 0 if none
  bool     startSendRaw(const uint8_t* bytes, int len) override;
  bool     isSendComplete() override;
  void     onSendFinished() override;
  bool     isInRecvMode() const override;
  uint32_t getEstAirtimeFor(int len_bytes) override;       // ms
  float    packetScore(float snr, int packet_len) override;
  float    getLastRSSI() const override { return _last_rssi; }
  float    getLastSNR()  const override { return _last_snr; }
  float    getCurrentRSSI() { return _last_rssi; }   // remote LoRa: no live channel-RSSI probe

  // Convention helpers the companion's radio glue expects (mirror ESPNOWRadio):
  void     powerOff();
  uint32_t getRngSeed();
  uint32_t getPacketsRecv() const { return _n_recv; }
  uint32_t getPacketsSent() const { return _n_sent; }

private:
  // Push _cfg to the modem — but ONLY while not yet polling RX. The remote link's
  // esp_hosted_send_custom_data() blocks indefinitely under continuous-RX load, so a
  // live reconfig from the UI thread would freeze the app. Boot bring-up (before the
  // RX loop starts) pushes fine; runtime changes update _cfg and apply on next init.
  void pushConfig() { if (_started && !_rx_polling) lora_set_config(&_h, &_cfg); }

  lora_handle_t                 _h   = {0};
  lora_protocol_config_params_t _cfg = {0};
  bool   _started    = false;
  bool   _rx_polling = false;   // set once recvRaw() starts draining — see pushConfig()
  bool   _sending    = false;
  bool   _rx_boost   = false;
  float  _last_rssi = 0.0f;
  float  _last_snr  = 0.0f;
  uint32_t _n_recv  = 0;
  uint32_t _n_sent  = 0;
  uint32_t _n_recv_errors = 0;
};
