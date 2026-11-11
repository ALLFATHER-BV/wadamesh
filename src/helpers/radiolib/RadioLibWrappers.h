#pragma once

#include <Mesh.h>
#include <RadioLib.h>

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;

  void idle();
  void startRecv();
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

  // Buffered-receive internals: one drained packet's link stats, returned by
  // getLastSNR/getLastRSSI while a popped ring slot is current.
  void rxqDrainOne();
  float _q_last_snr = 0;
  float _q_last_rssi = 0;
  bool  _q_has_last = false;
#if defined(ESP32)
  friend void rlwRxqTask(void*);
#endif

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board), _preamble_sf(0) { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff();
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override;   // in .cpp — takes the radio mutex (SPI reads)

  // ---- Polled receive, for boards whose DIO1 reaches no host GPIO ----------
  // The T-Display P4 wires the SX1262 IRQ to an I2C expander, so the setFlag()
  // ISR can never run: nothing sets STATE_INT_READY and BOTH receive paths
  // (buffered drain and legacy read) wait forever. pollRxDone() lets the
  // concrete wrapper report RX-done straight from the radio's IRQ register,
  // and pollRxIfNoIrq() turns that into exactly what the ISR would have done.
  // Compiled in only where MESH_RADIO_DIO1_POLLED is defined, so IRQ boards
  // pay nothing (not even the SPI read).
  virtual bool pollRxDone() { return false; }
  void pollRxIfNoIrq();

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  virtual uint32_t getRngSeed();
  void setTxPower(int8_t dbm);

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  // --- Buffered receive (experimental) ---------------------------------------
  // The stock RX path only lifts a packet out of the radio when the main loop
  // reaches recvRaw(); a loop stall (heavy UI frame, storage write) longer than
  // the gap between two packets silently loses all but the last one — the SX126x
  // buffers a single packet and DIO1 is a single flag. When enabled, a small
  // high-priority task woken by DIO1 drains each packet into a DRAM ring within
  // ~1 ms and re-arms RX; recvRaw() then pops from the ring. All radio SPI
  // access is serialized with a recursive mutex that exists only once the
  // feature has been enabled — disabled, the code path and timing are stock.
  void rxQueueEnable(bool en);
  bool rxQueueEnabled() const;
  // Spectrum analyzer drives the raw radio directly: park the drain task first.
  void rxQueueSuspend(bool s);
  // Serialize out-of-band radio access (setParams from settings, spectrum
  // enter/leave) against a possible in-flight drain. No-ops until first enable.
  void radioAcquire();
  void radioRelease();
  // DIO1 RX-done events counted in the ISR (only while in RX state, so TX-done
  // does not pollute it). events - packetsRecv - recvErrors ≈ packets lost to
  // late reads (the missed-messages class); transiently high by the 1-2 packets
  // currently queued/unread.
  uint32_t getRxEvents() const;
  uint32_t getRxQueueDrops() const;   // packets dropped because the ring was full

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }

  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    for (int i = 0; i < sz; i++) {
      dest[i] ^= _radio->randomByte() ^ (::random(0, 256) & 0xFF); // combine with Radio's entropy
    }
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};
