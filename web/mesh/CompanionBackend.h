// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// CompanionBackend: the web build's mesh is a real MeshCore companion node.
//
// It speaks the companion frame protocol, the one the MeshCore phone apps use and
// src/MyMesh.cpp's handleCmdFrame() serves, over any CompanionTransport (USB serial and
// TCP natively; Web Serial, Web Bluetooth and WebSocket in a browser). It matches
// STOCK MeshCore 1.17 / 1.18 companion behaviour; the wadamesh extras it meets (live
// message pushes alongside MSG_WAITING, the trailing "Meshcomod" pseudo contact, the
// per-client sync cursor) are handled where they differ.
//
// The stand-in (WebMesh) keeps the same local mirror a device's MyMesh keeps; this
// backend fills it from the node and turns the node's replies and pushes into the
// MeshBackendEvents the stand-in turns into UI callbacks. See WEB_BUILD.md for what
// maps to what, and what the protocol cannot do.
#include <stdint.h>

namespace companion {
// Commands (host -> node). Names as in src/MyMesh.cpp / MeshCore's companion_radio.
enum : uint8_t {
  CMD_APP_START = 1, CMD_SEND_TXT_MSG = 2, CMD_SEND_CHANNEL_TXT_MSG = 3, CMD_GET_CONTACTS = 4,
  CMD_GET_DEVICE_TIME = 5, CMD_SET_DEVICE_TIME = 6, CMD_SEND_SELF_ADVERT = 7, CMD_SET_ADVERT_NAME = 8,
  CMD_ADD_UPDATE_CONTACT = 9, CMD_SYNC_NEXT_MESSAGE = 10, CMD_SET_RADIO_PARAMS = 11,
  CMD_SET_RADIO_TX_POWER = 12, CMD_RESET_PATH = 13, CMD_SET_ADVERT_LATLON = 14, CMD_REMOVE_CONTACT = 15,
  CMD_SHARE_CONTACT = 16, CMD_EXPORT_CONTACT = 17, CMD_IMPORT_CONTACT = 18, CMD_REBOOT = 19,
  CMD_GET_BATT_AND_STORAGE = 20, CMD_SET_TUNING_PARAMS = 21, CMD_DEVICE_QUERY = 22,
  CMD_SEND_LOGIN = 26, CMD_SEND_STATUS_REQ = 27, CMD_HAS_CONNECTION = 28, CMD_LOGOUT = 29,
  CMD_GET_CONTACT_BY_KEY = 30, CMD_GET_CHANNEL = 31, CMD_SET_CHANNEL = 32, CMD_SEND_TRACE_PATH = 36,
  CMD_SET_DEVICE_PIN = 37, CMD_SET_OTHER_PARAMS = 38, CMD_SEND_TELEMETRY_REQ = 39,
  CMD_SEND_BINARY_REQ = 50, CMD_FACTORY_RESET = 51, CMD_SET_FLOOD_SCOPE = 54, CMD_GET_STATS = 56,
  CMD_SEND_ANON_REQ = 57, CMD_SET_PATH_HASH_MODE = 61, CMD_SET_DEFAULT_FLOOD_SCOPE = 63,
  CMD_RUN_CLI_COMMAND = 66,   // MeshCore 1.18 (protocol v14)
};
// Replies (node -> host, answer the command in flight).
enum : uint8_t {
  RESP_OK = 0, RESP_ERR = 1, RESP_CONTACTS_START = 2, RESP_CONTACT = 3, RESP_END_OF_CONTACTS = 4,
  RESP_SELF_INFO = 5, RESP_SENT = 6, RESP_CONTACT_MSG_RECV = 7, RESP_CHANNEL_MSG_RECV = 8,
  RESP_CURR_TIME = 9, RESP_NO_MORE_MESSAGES = 10, RESP_BATT_AND_STORAGE = 12, RESP_DEVICE_INFO = 13,
  RESP_DISABLED = 15, RESP_CONTACT_MSG_RECV_V3 = 16, RESP_CHANNEL_MSG_RECV_V3 = 17,
  RESP_CHANNEL_INFO = 18, RESP_STATS = 24, RESP_CLI_REPLY = 29,
};
// Pushes (node -> host, unsolicited).
enum : uint8_t {
  PUSH_ADVERT = 0x80, PUSH_PATH_UPDATED = 0x81, PUSH_SEND_CONFIRMED = 0x82, PUSH_MSG_WAITING = 0x83,
  PUSH_RAW_DATA = 0x84, PUSH_LOGIN_SUCCESS = 0x85, PUSH_LOGIN_FAIL = 0x86, PUSH_STATUS_RESPONSE = 0x87,
  PUSH_LOG_RX_DATA = 0x88, PUSH_TRACE_DATA = 0x89, PUSH_NEW_ADVERT = 0x8A, PUSH_TELEMETRY_RESPONSE = 0x8B,
  PUSH_BINARY_RESPONSE = 0x8C, PUSH_PATH_DISCOVERY_RESPONSE = 0x8D, PUSH_CONTROL_DATA = 0x8E,
  PUSH_CONTACT_DELETED = 0x8F, PUSH_CONTACTS_FULL = 0x90,
};
// The protocol version this client speaks (CMD_DEVICE_QUERY): v3+ message frames carry SNR.
static const uint8_t APP_TARGET_VER = 3;
// The per-client sync cursor id wadamesh / meshcomod keep for us (stock ignores it): one per
// browser profile and page origin (each keeps its own copy of the chats), so every copy gets
// every message it has not had yet, whichever app or browser picked it up first.
const char* clientId();
static const char* const APP_NAME = "wadamesh web";
}  // namespace companion
