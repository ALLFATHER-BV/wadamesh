#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "NodePrefs.h"

// Forward decl — defined in helpers/ContactInfo.h, included by MyMesh.h users.
struct ContactInfo;

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  BaseSerialInterface* _serial;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, BaseSerialInterface* serial) : _board(board), _serial(serial) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isSerialEnabled() const { return _serial->isEnabled(); }
  void enableSerial() { _serial->enable(); }
  void disableSerial() { _serial->disable(); }
  bool isTcpEnabled() const { return _serial->isTcpEnabled(); }
  void enableTcp() { _serial->enableTcp(); }
  void disableTcp() { _serial->disableTcp(); }
  bool isWsStarted() const { return _serial->isWsStarted(); }
  uint16_t getWsPort() const { return _serial->getWsPort(); }
  int getWsConnectedCount() const { return _serial->getWsConnectedCount(); }
  bool hasBleCapability() const { return _serial->hasBleCapability(); }
  bool isBleEnabled() const { return _serial->isBleEnabled(); }
  void enableBle() { _serial->enableBle(); }
  void disableBle() { _serial->disableBle(); }
  bool getBlePeerAddress(char* buf, size_t len) const { return _serial->getBlePeerAddress(buf, len); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) = 0;
  virtual void newMsgFromPub(uint8_t path_len, const uint8_t* from_pub, const char* from_name, const char* text, int msgcount) {
    (void)from_pub;
    newMsg(path_len, from_name, text, msgcount);
  }
  /** Same as newMsgFromPub but carries the LoRa RX metadata so the touch UI
   *  can surface it in the per-message Info popup.
   *  - path_len: hop count when is_flood is true, otherwise 0xFF (direct/routed).
   *  - is_flood: matches mesh::Packet::isRouteFlood() at receive time.
   *  - snr_q4 / rssi: same encoding the wire protocol uses (SNR × 4, RSSI dBm).
   *  Default delegates to newMsgFromPub so non-touch UIs ignore the metadata. */
  virtual void newMsgFromPubWithMeta(uint8_t path_len, bool is_flood,
                                     const uint8_t* from_pub, const char* from_name,
                                     const char* text, int msgcount,
                                     int8_t snr_q4, int8_t rssi) {
    (void)is_flood; (void)snr_q4; (void)rssi;
    newMsgFromPub(path_len, from_pub, from_name, text, msgcount);
  }
  /** Room-server post. Like newMsgFromPubWithMeta, but from_pub/from_name
   *  identify the ROOM (the chat thread) while `author_name` is the resolved
   *  display name of the original poster — a room signed message attributes
   *  the author only by a pubkey prefix, never in the text body. Default
   *  ignores the author and behaves like a normal message, so non-touch UIs
   *  are unaffected. */
  virtual void newRoomMsgFromPubWithMeta(uint8_t path_len, bool is_flood,
                                         const uint8_t* from_pub, const char* from_name,
                                         const char* author_name,
                                         const char* text, int msgcount,
                                         int8_t snr_q4, int8_t rssi) {
    (void)author_name;
    newMsgFromPubWithMeta(path_len, is_flood, from_pub, from_name, text, msgcount, snr_q4, rssi);
  }
  /** A message the COMPANION APP sent on our behalf (CMD_SEND_TXT_MSG). The wire protocol
   *  deliberately doesn't echo sent messages back to clients, so the on-device UI is otherwise
   *  the only consumer that never sees app-originated sends — this lets it mirror one as a local
   *  outgoing bubble so it shows on-device too (issue #46). Default no-op for non-touch UIs. */
  virtual void appSentMsgToContact(const uint8_t* to_pub, const char* to_name, const char* text,
                                   uint32_t ack_hash) {
    (void)to_pub; (void)to_name; (void)text; (void)ack_hash;
  }
  /** Mirror an app-originated CHANNEL message into the on-device UI — the channel-send path, like the
   *  DM path above, never shows companion-originated sends on screen otherwise. text is NUL-terminated
   *  by the caller. Default no-op for non-touch UIs. */
  virtual void appSentMsgToChannel(const char* channel_name, const char* text) {
    (void)channel_name; (void)text;
  }
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void appendDiag(const char* message) { (void)message; }
  /** Notify UI of an advert reception. `is_new=true` means the contact is NOT in
   *  the persistent contacts[] table (filtered by auto-add rules or contacts
   *  full); `is_new=false` means the contact is in contacts[] and was just
   *  updated. Used by the touch UI to build a "Discovered" list of pending
   *  contacts when auto-add is off. Default impl is a no-op. */
  virtual void discoveredContact(const ContactInfo& contact, bool is_new, uint8_t path_len) {
    (void)contact;
    (void)is_new;
    (void)path_len;
  }
  /** Notify UI of a status response from a repeater that was pinged from the
   *  UI side (e.g. via the Contacts action sheet). `data`/`len` is the raw
   *  reply payload (typically a JSON blob from StatsFormatHelper). Default is
   *  a no-op so non-touch UIs can ignore. */
  virtual void onPingReply(const ContactInfo& contact, const uint8_t* data, size_t len) {
    (void)contact;
    (void)data;
    (void)len;
  }
  /** Notify UI of a TELEMETRY reply (CayenneLPP payload). Same payload format
   *  the companion app receives via PUSH_CODE_TELEMETRY_RESPONSE. Default
   *  hands off to onPingReply so an UI that doesn't differentiate (or that
   *  was built before the split) still sees the bytes. */
  virtual void onTelemetryReply(const ContactInfo& contact, const uint8_t* data, size_t len) {
    onPingReply(contact, data, len);
  }
  /** Admin-login finished. `success=true` means the repeater accepted the
   *  password and added us to its ACL; `perms` is the granted permission
   *  bitmask (e.g. PERM_ACL_ADMIN | PERM_ACL_GUEST). On `success=false` the
   *  password was wrong or the response timed out. Default no-op so
   *  non-touch UIs ignore. */
  virtual void onAdminLoginResult(const ContactInfo& contact, bool success, uint8_t perms) {
    (void)contact; (void)success; (void)perms;
  }
  /** The server's own clock, from the timestamp prefix every server RESPONSE
   *  carries (first 4 bytes). Fired on LOGIN_OK so the UI can warn when the
   *  device clock and the server clock disagree — a skewed device clock makes
   *  the server's replay guard silently drop our logins/posts/keep-alives
   *  (issue #89). Default no-op. */
  virtual void onServerClock(const ContactInfo& contact, uint32_t server_epoch) {
    (void)contact; (void)server_epoch;
  }
  /** A CLI command reply from a repeater / room server / sensor (the
   *  TXT_TYPE_CLI_DATA path). `text` is the decrypted command output text
   *  the server returned. Default no-op; the touch UI overrides to append
   *  the line to the open admin console. */
  virtual void onAdminCommandReply(const ContactInfo& contact, const char* text) {
    (void)contact; (void)text;
  }
  /** Notify UI that channels[] or contacts[] changed (e.g. app added a channel
   *  via CMD_SET_CHANNEL). Default: no-op; touch UI overrides to bump the
   *  periodic thread-refresh deadline so the new entry shows up immediately. */
  virtual void onThreadsChanged() {}
  /** ACK arrived for a previously-sent DM. `ack_hash` is the 4-byte
   *  expected-ack value the dispatcher returned from `sendMessage`. Touch
   *  UI flips the matching outgoing bubble to DELIVERED; default no-op for
   *  non-touch builds. */
  virtual void onMessageAcked(uint32_t ack_hash) { (void)ack_hash; }
  /** A UI-initiated trace ping bounced back. `tag` is the 4-byte tag we
   *  set when creating the trace. `their_snr` is the SNR the remote node
   *  observed when receiving our outgoing packet (in dB, signed). `our_snr`
   *  is the SNR our radio observed when we picked up the remote's
   *  retransmission (also in dB). For multi-hop traces, `extra_hops` may
   *  be >0 and `extra_snrs[]` is one byte per intermediate hop (each
   *  reading is SNR*4 as stored on the wire). Default no-op for non-touch
   *  builds. */
  virtual void onTracePingResult(uint32_t tag, int8_t their_snr,
                                 int8_t our_snr, uint8_t extra_hops,
                                 const int8_t* extra_snrs) {
    (void)tag; (void)their_snr; (void)our_snr;
    (void)extra_hops; (void)extra_snrs;
  }
  virtual void logRxFrame(float snr, float rssi, const uint8_t* raw, int len) {
    (void)snr;
    (void)rssi;
    (void)raw;
    (void)len;
  }
  virtual void loop() = 0;
};
