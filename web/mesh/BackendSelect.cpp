// SPDX-License-Identifier: GPL-3.0-or-later
// Which mesh the web build drives: the demo by default, a companion node when a
// transport is named on the command line (or, in a browser, in Module.arguments).
//
//   --serial PATH          native: USB serial (e.g. /dev/cu.usbmodem101)
//   --tcp HOST[:PORT]      native: the node's TCP companion server (default port 5000)
//   --companion KIND[:ARG] browser: web-serial | ble | ws:ws://HOST:8765/
//   --read-only            never change the node's settings, contacts, channels or clock
//                          (messages, adverts and requests still go out)
#include "MeshBackend.h"
#include "CompanionTransport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

MeshBackend* wadaCreateBackendFromArgs(int argc, char** argv) {
  CompanionTransport* t = nullptr;
  bool read_only = false;
  std::string err;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_val = i + 1 < argc;
    if (a == "--read-only") {
      read_only = true;
    } else if (a == "--serial" && has_val) {
      t = wadaCreateSerialTransport(argv[++i]);
      if (!t) err = "--serial is not available in this build";
    } else if (a == "--tcp" && has_val) {
      std::string hp = argv[++i];
      int port = 5000;
      const size_t c = hp.rfind(':');
      if (c != std::string::npos) { port = atoi(hp.c_str() + c + 1); hp.resize(c); }
      t = wadaCreateTcpTransport(hp.c_str(), port);
      if (!t) err = "--tcp is not available in this build";
    } else if (a == "--companion" && has_val) {
      const std::string spec = argv[++i];
      const size_t c = spec.find(':');
      const std::string kind = spec.substr(0, c);
      const std::string arg = c == std::string::npos ? "" : spec.substr(c + 1);
      t = wadaCreateBrowserTransport(kind.c_str(), arg.c_str());
      if (!t) err = "--companion " + kind + " is not available in this build";
    }
  }
  if (!err.empty()) fprintf(stderr, "[web] %s; using the demo\n", err.c_str());
  if (t) return wadaCreateCompanionBackend(t, read_only);
  return wadaCreateDemoBackend();
}
