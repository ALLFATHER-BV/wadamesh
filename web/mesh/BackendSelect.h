// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Which mesh the web board drives. The front ends fill a choice (host_main.cpp from
// --serial / --tcp / --read-only, web_main.cpp from the page's Connect chooser) and
// board.cpp's shared boot creates the backend from it.
#include <string>
class MeshBackend;

struct WadaBackendChoice {
  // "demo" | "serial" | "tcp" (native) | "web-serial" | "ble" | "ws" (browser)
  std::string kind = "demo";
  std::string arg;          // the device path, host[:port] or ws:// URL
  bool read_only = false;   // never change the node's settings, contacts, channels or clock
};

// The backend for a choice; the demo when the choice cannot be served by this build.
MeshBackend* wadaCreateBackend(const WadaBackendChoice& choice);

// The companion link's state, for a front end that shows it (the page's status bar).
//   state 0 = connecting / reconnecting, 1 = up; node_name / via / key_hex (the node's
//   public key, first 4 bytes as hex) are set when up. Called on the UI thread.
extern void (*g_wada_link_hook)(int state, const char* node_name, const char* via, const char* key_hex);
