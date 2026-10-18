// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MeshBackend: where the web build's mesh actually lives.
//
// The touch UI talks to `the_mesh` (web/mesh/WebMesh.h, the MyMesh stand-in). The
// stand-in keeps the same local mirror a real MyMesh keeps (prefs, contacts, channels,
// counters) and forwards everything that needs a network to ONE backend:
//
//   DemoBackend       a scripted neighbourhood for the website demo
//   CompanionBackend  a real MeshCore companion node (USB serial / TCP natively; Web
//                     Serial / Web Bluetooth / WebSocket in the browser), speaking the
//                     companion frame protocol that src/MyMesh.cpp's handleCmdFrame()
//                     implements on the device (CompanionBackend.cpp)
//
// The interface is deliberately the companion protocol's vocabulary (send a DM, send
// to a channel slot, advert, status/telemetry request, contact + channel sync, push
// events), so phase 3 is a transport + framing job, not a redesign.
//
// Threading: every call in both directions happens on the UI thread. A backend that
// has its own I/O thread queues and delivers from loop().
//
// Requests are fire-and-forget on a link: a call returns what the device's MyMesh would
// return at once (MSG_SEND_*, a tag), and the outcome arrives later as an event, exactly
// like the radio answering a device. Members the backend cannot serve keep the defaults
// below, which are the "unavailable" answers listed in WEB_BUILD.md.
#include <stdint.h>
#include <stddef.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include "NodePrefs.h"

// Events a backend delivers to the stand-in (implemented by MyMesh in WebMesh.cpp).
class MeshBackendEvents {
public:
  virtual ~MeshBackendEvents() {}
  // Contact table sync: an advert was heard / the node reported a contact.
  virtual void beContact(const ContactInfo& c, bool heard_advert, uint8_t path_len) = 0;
  // A node was heard but is NOT in the contact table (auto-add off / table full):
  // the UI lists it under Discovered.
  virtual void beDiscovered(const ContactInfo& c, uint8_t path_len) = 0;
  // Messages another client of the same node sent (the phone app; the demo's seeded
  // history): shown as outgoing bubbles, as on the device (issue #46).
  virtual void beOutgoingDirect(const uint8_t pub_key[32], const char* text, uint32_t expected_ack) = 0;
  virtual void beOutgoingChannel(int channel_idx, const char* text) = 0;
  // Channel slot sync (name + 16-byte secret).
  virtual void beChannel(int idx, const ChannelDetails& ch) = 0;
  // An incoming direct message. sender_ts = the sender's clock (0 = now).
  // path_len: hops for a flood, 0xFF for a direct route. snr_q4 = SNR*4, rssi in dBm.
  virtual void beDirectMessage(const uint8_t pub_key[32], uint32_t sender_ts, const char* text,
                               uint8_t path_len, bool flood, int8_t snr_q4, int8_t rssi) = 0;
  // An incoming channel post: `text` is the on-air "Sender: body" form, as the device sees it.
  virtual void beChannelMessage(int channel_idx, uint32_t sender_ts, const char* text,
                                uint8_t path_len, int8_t snr_q4, int8_t rssi) = 0;
  // A room-server post (thread = the room contact, author = the poster's name).
  virtual void beRoomMessage(const uint8_t room_pub_key[32], const char* author, uint32_t sender_ts,
                             const char* text, uint8_t path_len, int8_t snr_q4, int8_t rssi) = 0;
  // The ACK for a DM we sent (the expected_ack the backend returned from sendDirect).
  virtual void beAck(uint32_t expected_ack) = 0;
  // A flood we sent was heard repeated (fingerprint from sendDirect/sendChannel).
  // hop = the re-flooding repeater's path hash (the echo's last hop), when known.
  virtual void beRepeatHeard(uint32_t sent_fp, const uint8_t* hop = nullptr, uint8_t hop_sz = 0) = 0;
  // Replies to UI requests.
  virtual void beStatusReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) = 0;
  virtual void beTelemetryReply(const uint8_t pub_key[32], const uint8_t* lpp, size_t len) = 0;
  // their_snr / our_snr / extra_snrs are SNR*4, as on the wire (onTracePingResult).
  virtual void beTraceResult(uint32_t tag, int8_t their_snr, int8_t our_snr,
                             uint8_t extra_hops = 0, const int8_t* extra_snrs = nullptr) = 0;
  // Every received frame's signal, for the status-bar bars and the RF monitor.
  virtual void beSignal(int8_t snr_q4, int8_t rssi) = 0;

  // ---- events only a link to a real node produces (the demo never fires them) ----
  // The node said who it is: name, radio, the node-owned prefs, its public key. The
  // stand-in adopts these (a node's settings live on the node).
  virtual void beSelfInfo(const NodePrefs& node_prefs, const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) = 0;
  // The node's own position moved without a SELF_INFO (the demo placing its
  // neighbourhood around this computer's location): the map's "you are here".
  virtual void beSelfPosition(int32_t lat_e6, int32_t lon_e6) = 0;
  // The node dropped a contact (deleted, or overwritten when its table was full).
  virtual void beContactRemoved(const uint8_t pub_key[32]) = 0;
  // A raw received packet (MeshCore's PUSH_CODE_LOG_RX_DATA): the RX log, the live
  // signal, advert paths and "repeats heard" are computed from these as on a device.
  virtual void beRxRaw(int8_t snr_q4, int8_t rssi, const uint8_t* raw, size_t len) = 0;
  // A login the UI asked for (uiSendAdminLogin / uiRoomRelogin) was answered.
  virtual void beLoginResult(const uint8_t pub_key[32], bool ok, uint8_t perms, uint32_t server_clock) = 0;
  // What a modern LOGIN_OK carries beyond admin-or-not: the ACL permission byte and the
  // server's firmware version level (the repeater admin app); before beLoginResult.
  virtual void beLoginInfo(const uint8_t pub_key[32], uint8_t acl, uint8_t fw_level) = 0;
  // The reply to binaryRequest (after its 4-byte tag): neighbours, access list, owner info.
  virtual void beBinaryReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) = 0;
  // A CLI reply from a repeater / room (TXT_TYPE_CLI_DATA).
  virtual void beCliReply(const uint8_t pub_key[32], const char* text) = 0;
  // The REGIONS answer: reflected tag, the repeater's clock, then CSV names.
  virtual void beRegionsReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) = 0;
  // The link to the node came up / went away (status line, terminal).
  virtual void beLinkState(bool up, const char* detail) = 0;
  // A line for the Terminal (a node CLI reply).
  virtual void beTerminalLine(const char* text) = 0;
};

class MeshBackend {
public:
  // What a send returns: the MSG_SEND_* codes of BaseChatMesh.
  virtual ~MeshBackend() {}
  virtual const char* name() const = 0;          // "demo", "companion-serial", ...
  // Called once at boot. The backend fills the self identity + prefs it knows and
  // replays its contacts/channels through `ev`.
  virtual void begin(MeshBackendEvents& ev, NodePrefs& prefs, uint8_t self_pub_key[32]) = 0;
  virtual void loop(uint32_t now_ms) = 0;

  // ---- sending ----
  virtual int  sendDirect(const ContactInfo& to, uint32_t ts, const char* text,
                          uint32_t& expected_ack, uint32_t& est_timeout_ms, uint32_t& sent_fp) = 0;
  virtual bool sendChannel(int channel_idx, const ChannelDetails& ch, uint32_t ts,
                           const char* sender_name, const char* text, uint32_t& sent_fp) = 0;
  virtual bool sendAdvert(bool flood) = 0;

  // ---- requests (default: unavailable on this backend) ----
  virtual int  requestStatus(const ContactInfo& c) { (void)c; return 0; }       // MSG_SEND_FAILED
  virtual int  requestTelemetry(const ContactInfo& c) { (void)c; return 0; }
  virtual uint32_t tracePing(const uint8_t pub_key[32]) { (void)pub_key; return 0; }   // tag, 0 = failed
  // Along the contact's known route (one SNR per hop); default: a single-hop ping.
  virtual uint32_t traceRoute(const ContactInfo& c) { return tracePing(c.id.pub_key); }
  // The UI gave up waiting (its reply deadline): stop any follow-up (login fallback).
  virtual void cancelRequests() {}
  // Repeater / room admin: login (empty password = guest), a CLI line, session state.
  virtual int  login(const ContactInfo& c, const char* password) { (void)c; (void)password; return 0; }
  virtual int  sendCli(const ContactInfo& c, const char* text) { (void)c; (void)text; return 0; }
  virtual bool hasConnectionTo(const uint8_t pub_key[32]) { (void)pub_key; return false; }
  // A binary request (first byte REQ_TYPE_*, then its parameters) to a repeater / room we
  // are logged in to; the reply comes back as beBinaryReply. MSG_SEND_* result.
  virtual int  binaryRequest(const ContactInfo& c, const uint8_t* payload, uint8_t len) {
    (void)c; (void)payload; (void)len; return 0;
  }
  // The anonymous REGIONS request to a directly heard repeater.
  virtual int  requestRegions(const uint8_t pub_key[32], uint32_t& est_timeout) {
    (void)pub_key; est_timeout = 0; return 0;
  }
  // Zero-hop re-advert of a contact (CMD_SHARE_CONTACT).
  virtual bool shareContact(const uint8_t pub_key[32]) { (void)pub_key; return false; }
  // A command line for the node's own CLI (the Terminal); the reply comes back as
  // beTerminalLine. False = this backend has no CLI to run it on.
  virtual bool runNodeCli(const char* cmd) { (void)cmd; return false; }
  // The node's Bluetooth pairing PIN (0 = random per boot).
  virtual bool setBlePin(uint32_t pin) { (void)pin; return false; }

  // ---- configuration pushed back to the node ----
  virtual void prefsChanged(const NodePrefs& p) { (void)p; }
  virtual void channelChanged(int idx, const ChannelDetails& ch) { (void)idx; (void)ch; }
  virtual void contactRemoved(const uint8_t pub_key[32]) { (void)pub_key; }
  // A contact was added or edited here (favourite, telemetry permissions, position,
  // manual add): the node keeps the table, so the backend writes it back.
  virtual void contactChanged(const ContactInfo& c) { (void)c; }
  virtual void contactPathReset(const uint8_t pub_key[32]) { (void)pub_key; }
  // The position the node should advertise (CMD_SET_ADVERT_LATLON). Called ONLY while the
  // node's advert-location setting is "share", with the Location privacy displacement
  // applied (WebMesh.cpp syncAdvertPosition, WEB_BUILD.md "Location").
  virtual void advertPositionChanged(int32_t lat_e6, int32_t lon_e6) { (void)lat_e6; (void)lon_e6; }
  // Commands queued or in flight to a node (a restart lets them go out first).
  virtual bool busy() const { return false; }
  // True once the backend has the node's own settings (until then the stand-in's
  // prefs are placeholders and must not be written to a node).
  virtual bool ownsPrefs() const { return false; }

  // Radio counters, for the About/Radio pages (0 when the backend cannot know).
  virtual uint32_t packetsRecv() const { return 0; }
  virtual uint32_t packetsSent() const { return 0; }
};

// The backends (BackendSelect.h picks one at boot from the front end's choice).
MeshBackend* wadaCreateDemoBackend();
class CompanionTransport;
MeshBackend* wadaCreateCompanionBackend(CompanionTransport* transport, bool read_only);
