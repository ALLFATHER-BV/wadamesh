// SPDX-License-Identifier: GPL-3.0-or-later
// wadaHttpRequest (compat/wada_http.h): no network yet, on either front end.
#include "wada_http.h"
#include "HTTPClient.h"
int wadaHttpRequest(const char*, const std::string&, const std::vector<std::string>&,
                    const uint8_t*, size_t, uint32_t, WadaHttpReply& out) {
  out = WadaHttpReply();
  return HTTPC_ERROR_CONNECTION_REFUSED;
}
