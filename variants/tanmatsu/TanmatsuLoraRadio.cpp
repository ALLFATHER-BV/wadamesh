//
// TanmatsuLoraRadio implementation — see TanmatsuLoraRadio.h.
// TODO(device) markers = needs confirming/tuning once the Tanmatsu is in hand.
//
#include "TanmatsuLoraRadio.h"
#include <string.h>
#include <math.h>
#include <esp_random.h>

bool TanmatsuLoraRadio::init() {
  // The radio is on the coprocessor → "remote". 32 = RX packet-queue depth.
  if (lora_init_remote(&_h, 32) != ESP_OK) return false;
  _started = true;
  lora_set_config(&_h, &_cfg);   // push whatever setParams()/setTxPower() captured
  return true;
}

void TanmatsuLoraRadio::setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) {
  _cfg.frequency        = (uint32_t)(freq_mhz * 1000000.0f);
  _cfg.bandwidth        = (uint16_t)bw_khz;     // confirmed kHz; valid {7,10,15,20,31,41,62,125,250,500} (62.5->62)
  _cfg.spreading_factor = sf;
  _cfg.coding_rate      = cr;                    // confirmed: 5..8 -> 4/5..4/8 (matches RadioLib's cr)
  _cfg.sync_word        = 0x12;                  // CONFIRMED: sx126x_set_sync_word expands 0x12 -> on-air
                                                 // 0x1424 (ctrl bits 0x44) == RADIOLIB_SX126X_SYNC_WORD_PRIVATE
  _cfg.preamble_length  = (sf <= 8) ? 32 : 16;   // match MeshCore RadioLibWrapper::preambleLengthForSF(sf)
  _cfg.power            = 22;                     // dBm; TODO(device): drive from region/prefs
  _cfg.crc_enabled      = true;
  _cfg.invert_iq        = false;
  pushConfig();
}

void TanmatsuLoraRadio::setTxPower(int8_t dbm) {
  _cfg.power = (uint8_t)dbm;      // TODO(device): confirm the component clamps to the SX1262 range
  pushConfig();
}

void TanmatsuLoraRadio::setRxBoostedGainMode(bool on) {
  _rx_boost = on;
  _cfg.rx_boost = on;
  pushConfig();
}

void TanmatsuLoraRadio::idle() {
  if (_started) lora_set_mode(&_h, LORA_PROTOCOL_MODE_STANDBY_RC);
}

int TanmatsuLoraRadio::recvRaw(uint8_t* bytes, int sz) {
  if (!_started) return 0;
  if (!_rx_polling) {
    // First poll: init() + setParams() are settled now, so ARM continuous RX on the C6. Nothing
    // else does this — lora_set_config() leaves the modem in STANDBY, and begin()/onSendFinished()
    // were no-ops built on a false "auto-RX" assumption (that was the "TX works, nothing received"
    // bug). Once armed, the C6 re-arms after every RX packet and restores RX after every TX, so this
    // one-shot is enough. It also gates pushConfig from here on (the remote link blocks under RX load).
    _rx_polling = true;
    lora_set_mode(&_h, LORA_PROTOCOL_MODE_RX);
  } else if (_cfg_dirty) {
    // A runtime setParams()/setTxPower()/setRxBoostedGainMode() (the wadamesh radio
    // settings save, from the UI or the companion app) changed the PHY config. Apply it
    // HERE, on the RX-poll thread, so the C6 remote link is never touched cross-thread:
    // leave RX to STANDBY (so the link isn't under active-RX load and lora_set_config
    // can't block), push the new config, then re-arm continuous RX. This is what makes a
    // live frequency / SF / BW / CR / power change actually reach the modem (it used to
    // need a reboot) AND lets wadamesh's settings override whatever the Tanmatsu OS set.
    _cfg_dirty = false;
    lora_set_mode(&_h, LORA_PROTOCOL_MODE_STANDBY_RC);
    lora_set_config(&_h, &_cfg);
    lora_set_mode(&_h, LORA_PROTOCOL_MODE_RX);
  }
  lora_protocol_lora_packet_t pkt;
  // Non-blocking poll of the RX queue (timeout 0).
  if (lora_receive_packet(&_h, &pkt, 0) != ESP_OK) return 0;
  int n = pkt.length;
  if (n > sz) n = sz;
  memcpy(bytes, pkt.data, n);
  // Confirmed against lora.c: stats = {rssi_pkt_raw u8, snr_pkt_raw i8, signal_rssi_pkt_raw u8};
  // rssi_dbm = -rssi_pkt_raw/2, snr_db = snr_pkt_raw/4.
  _last_rssi = pkt.stats.rssi_pkt_raw / -2.0f;
  _last_snr  = pkt.stats.snr_pkt_raw  /  4.0f;
  _n_recv++;
  return n;
}

bool TanmatsuLoraRadio::startSendRaw(const uint8_t* bytes, int len) {
  if (!_started || len < 0 || len > 256) return false;
  lora_protocol_lora_packet_t pkt = {};
  pkt.length = (uint8_t)len;
  memcpy(pkt.data, bytes, len);
  if (lora_send_packet(&_h, &pkt) != ESP_OK) return false;
  _sending = true;
  _n_sent++;
  return true;
}

bool TanmatsuLoraRadio::isSendComplete() {
  // Confirmed against lora.c: lora_send_packet() blocks on the transaction semaphore
  // (waits up to ~2 s for the coprocessor TX), so TX is done on return — latch is correct.
  if (_sending) { _sending = false; return true; }
  return true;
}

void TanmatsuLoraRadio::onSendFinished() { /* radio auto-returns to RX */ }

bool TanmatsuLoraRadio::isInRecvMode() const { return _started && !_sending; }

uint32_t TanmatsuLoraRadio::getEstAirtimeFor(int len_bytes) {
  // Standard Semtech LoRa time-on-air (ms) computed from the active PHY config,
  // since there's no RadioLib object here to call getTimeOnAir() on.
  const int   sf    = _cfg.spreading_factor ? _cfg.spreading_factor : 11;
  const float bw    = (_cfg.bandwidth ? _cfg.bandwidth : 250) * 1000.0f;   // Hz (TODO: confirm units)
  const int   crDen = _cfg.coding_rate ? _cfg.coding_rate : 5;             // 4/crDen (TODO: confirm 5..8)
  const int   cr    = (crDen > 4) ? (crDen - 4) : 1;                        // 1..4 for the formula
  const float tSym  = (float)(1u << sf) / bw;
  const int   de    = (tSym > 0.016f) ? 1 : 0;                              // low-data-rate optimize
  const float preamble = _cfg.preamble_length ? _cfg.preamble_length : 16;
  const float tPreamble = (preamble + 4.25f) * tSym;
  const int num = 8 * len_bytes - 4 * sf + 28 + (_cfg.crc_enabled ? 16 : 0);
  const int den = 4 * (sf - 2 * de);
  int payloadSymb = 8 + (int)fmaxf(ceilf((float)num / (float)den) * (cr + 4), 0.0f);
  return (uint32_t)((tPreamble + payloadSymb * tSym) * 1000.0f);
}

float TanmatsuLoraRadio::packetScore(float snr, int packet_len) {
  // Same model as RadioLibWrapper::packetScoreInt (keep scoring consistent across boards).
  static const float snr_threshold[] = { -7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f }; // SF7..SF12
  int sf = _cfg.spreading_factor ? _cfg.spreading_factor : 11;
  if (sf < 7 || sf > 12) return 0.0f;
  const float thr = snr_threshold[sf - 7];
  if (snr < thr) return 0.0f;
  const float success   = (snr - thr) / 10.0f;
  const float collision = 1.0f - (packet_len / 256.0f);
  float s = success * collision;
  if (s < 0.0f) s = 0.0f;
  if (s > 1.0f) s = 1.0f;
  return s;
}

void TanmatsuLoraRadio::powerOff() {
  if (_started) lora_set_mode(&_h, LORA_PROTOCOL_MODE_STANDBY_RC);
}

uint32_t TanmatsuLoraRadio::getRngSeed() {
  float r = 0.0f;
  if (_started) lora_get_rssi_inst(&_h, &r);
  return esp_random() ^ (uint32_t)(r * 1000.0f);
}
