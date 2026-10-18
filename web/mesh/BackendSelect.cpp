// SPDX-License-Identifier: GPL-3.0-or-later
// BackendSelect.h: the demo, or a companion node over the transport the front end named.
#include "BackendSelect.h"
#include "MeshBackend.h"
#include "CompanionTransport.h"
#include <stdio.h>
#include <stdlib.h>

void (*g_wada_link_hook)(int, const char*, const char*, const char*) = nullptr;

MeshBackend* wadaCreateBackend(const WadaBackendChoice& c) {
  CompanionTransport* t = nullptr;
  if (c.kind == "serial") {
    t = wadaCreateSerialTransport(c.arg.c_str());
  } else if (c.kind == "tcp") {
    std::string host = c.arg;
    int port = 5000;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) { port = atoi(host.c_str() + colon + 1); host.resize(colon); }
    t = wadaCreateTcpTransport(host.c_str(), port);
  } else if (c.kind == "web-serial" || c.kind == "ble" || c.kind == "ws") {
    t = wadaCreateBrowserTransport(c.kind.c_str(), c.arg.c_str());
  } else if (c.kind != "demo" && !c.kind.empty()) {
    fprintf(stderr, "[web] unknown mesh backend '%s'\n", c.kind.c_str());
  }
  if (c.kind != "demo" && !t) fprintf(stderr, "[web] '%s' is not available in this build; using the demo\n", c.kind.c_str());
  if (t) return wadaCreateCompanionBackend(t, c.read_only);
  return wadaCreateDemoBackend();
}
