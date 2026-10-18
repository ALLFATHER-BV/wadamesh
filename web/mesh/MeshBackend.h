// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MeshBackend: where the web build's mesh actually lives.
//
// The touch UI talks to `the_mesh` (web/mesh/WebMesh.h, the MyMesh stand-in). The
// stand-in keeps the same local mirror a real MyMesh keeps (prefs, contacts, channels,
// counters) and forwards everything that needs a network to ONE backend:
//
//   DemoBackend       (phase 1) a scripted neighbourhood for the website demo
//   CompanionBackend  (phase 3) a real MeshCore companion node over Web Serial /
//                     Web Bluetooth / WebSocket, speaking the companion frame protocol
//                     that src/MyMesh.cpp's handleCmdFrame() implements on the device
//
// The interface is deliberately the companion protocol's vocabulary (send a DM, send
// to a channel slot, advert, status/telemetry request, contact + channel sync, push
// events), so phase 3 is a transport + framing job, not a redesign.
//
// Threading: every call in both directions happens on the UI thread. A backend that
// has its own I/O thread (phase 3) queues and delivers from loop().
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
  virtual void beRepeatHeard(uint32_t sent_fp) = 0;
  // Replies to UI requests.
  virtual void beStatusReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) = 0;
  virtual void beTelemetryReply(const uint8_t pub_key[32], const uint8_t* lpp, size_t len) = 0;
  virtual void beTraceResult(uint32_t tag, int8_t their_snr, int8_t our_snr) = 0;
  // Every received frame's signal, for the status-bar bars and the RF monitor.
  virtual void beSignal(int8_t snr_q4, int8_t rssi) = 0;
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

  // ---- configuration pushed back to the node ----
  virtual void prefsChanged(const NodePrefs& p) { (void)p; }
  virtual void channelChanged(int idx, const ChannelDetails& ch) { (void)idx; (void)ch; }
  virtual void contactRemoved(const uint8_t pub_key[32]) { (void)pub_key; }

  // Radio counters, for the About/Radio pages (0 when the backend cannot know).
  virtual uint32_t packetsRecv() const { return 0; }
  virtual uint32_t packetsSent() const { return 0; }
};

// The backend the web build runs with (web/mesh/WebMesh.cpp picks it at boot).
MeshBackend* wadaCreateDemoBackend();
