// SPDX-License-Identifier: GPL-3.0-or-later
// wadaHttpRequest (compat/wada_http.h): the map tile server, and nothing else.
#include "wada_http.h"
#include "HTTPClient.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/threading.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef WADA_TILE_SERVER
#error "WADA_TILE_SERVER: web/CMakeLists.txt reads it from src/helpers/esp32/TouchPrefsStore.cpp"
#endif

namespace {
std::atomic<int> g_online{-1};   // -1: not set yet (native: from WADA_NET)
// WADA_HTTP_DEBUG=1 (native) logs every request and its outcome.
void dbg(const std::string& url, int code, size_t n) {
  static const bool on = [] { const char* e = getenv("WADA_HTTP_DEBUG"); return e && *e == '1'; }();
  if (on) fprintf(stderr, "[http] %s -> %d (%u bytes)\n", url.c_str(), code, (unsigned)n);
}

// The part of `url` after the tile server's base, or false when the URL is not the
// tile server's (only "<base>/..." counts; "<base>.evil.example/..." does not).
bool tilePath(const std::string& url, std::string& rest) {
  static const std::string base = WADA_TILE_SERVER;
  if (url.compare(0, base.size(), base) != 0) return false;
  if (url.size() == base.size() || url[base.size()] != '/') return false;
  rest = url.substr(base.size());
  return rest.find("..") == std::string::npos;
}
}  // namespace

bool wadaNetOnline() {
  int v = g_online.load();
  if (v < 0) {
#if defined(__EMSCRIPTEN__)
    v = 1;   // until the page says otherwise
#else
    const char* e = getenv("WADA_NET");
    v = (e && *e == '0') ? 0 : 1;   // host_main.cpp sets WADA_NET=0 for --headless unless given
#endif
    g_online.store(v);
  }
  return v == 1;
}
void wadaNetSetOnline(bool online) { g_online.store(online ? 1 : 0); }

#if defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE void wadaWebOnline(int up) { wadaNetSetOnline(up != 0); }

int wadaHttpRequest(const char* method, const std::string& url, const std::vector<std::string>& headers,
                    const uint8_t* body, size_t body_len, uint32_t timeout_ms, WadaHttpReply& out) {
  (void)headers;   // the proxy sets the User-Agent (a page may not)
  out = WadaHttpReply();
  std::string rest;
  if (!wadaNetOnline() || !tilePath(url, rest) || emscripten_is_main_browser_thread())
    return HTTPC_ERROR_CONNECTION_REFUSED;
  // Same origin, relative to the page's folder (a pthread resolves it against wadamesh.js,
  // which sits next to index.html): /tiles/... locally, /web/tiles/... on the site.
  const std::string local = "tiles" + rest;
  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  strncpy(attr.requestMethod, method && *method ? method : "GET", sizeof(attr.requestMethod) - 1);
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;
  // The device bounds each socket read (2 s); here one call is the whole request through
  // a proxy that may fetch the tile first, and this pthread has no watchdog.
  attr.timeoutMSecs = timeout_ms > 15000 ? timeout_ms : 15000;
  if (body && body_len) { attr.requestData = (const char*)body; attr.requestDataSize = body_len; }
  emscripten_fetch_t* f = emscripten_fetch(&attr, local.c_str());
  if (!f) return HTTPC_ERROR_CONNECTION_REFUSED;
  const int status = f->status;
  if (status > 0) {
    out.status = status;
    if (f->data && f->numBytes) out.body.assign((const uint8_t*)f->data, (const uint8_t*)f->data + f->numBytes);
    out.location = local;
  }
  emscripten_fetch_close(f);
  return status > 0 ? status : HTTPC_ERROR_CONNECTION_LOST;
}

#else  // native: HTTP/1.0 straight to the tile server

static int nativeRequest(const char* method, const std::string& url, const std::vector<std::string>& headers,
                         const uint8_t* body, size_t body_len, uint32_t timeout_ms, WadaHttpReply& out);
int wadaHttpRequest(const char* method, const std::string& url, const std::vector<std::string>& headers,
                    const uint8_t* body, size_t body_len, uint32_t timeout_ms, WadaHttpReply& out) {
  const int code = nativeRequest(method, url, headers, body, body_len, timeout_ms, out);
  dbg(url, code, out.body.size());
  return code;
}
static int nativeRequest(const char* method, const std::string& url, const std::vector<std::string>& headers,
                         const uint8_t* body, size_t body_len, uint32_t timeout_ms, WadaHttpReply& out) {
  out = WadaHttpReply();
  std::string rest;
  if (!wadaNetOnline() || !tilePath(url, rest)) return HTTPC_ERROR_CONNECTION_REFUSED;
  // WADA_TILE_SERVER is "http://host[:port]"
  std::string hostport = std::string(WADA_TILE_SERVER).substr(7), port = "80";
  const size_t colon = hostport.find(':');
  std::string host = hostport;
  if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
  addrinfo hints{}, *ai = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) != 0 || !ai) return HTTPC_ERROR_CONNECTION_REFUSED;
  int fd = -1;
  for (addrinfo* p = ai; p && fd < 0; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    timeval tv{};
    const uint32_t ms = timeout_ms > 8000 ? timeout_ms : 8000;
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, p->ai_addr, p->ai_addrlen) != 0) { close(fd); fd = -1; }
  }
  freeaddrinfo(ai);
  if (fd < 0) return HTTPC_ERROR_CONNECTION_REFUSED;
  std::string req = std::string(method && *method ? method : "GET") + " " + rest + " HTTP/1.0\r\nHost: " + hostport +
                    "\r\nConnection: close\r\n";
  bool ua = false;
  for (const std::string& h : headers) {
    if (strncasecmp(h.c_str(), "User-Agent:", 11) == 0) ua = true;
    req += h + "\r\n";
  }
  if (!ua) req += "User-Agent: wadamesh-web (https://github.com/ALLFATHER-BV/wadamesh)\r\n";
  if (body && body_len) req += "Content-Length: " + std::to_string(body_len) + "\r\n";
  req += "\r\n";
  bool ok = send(fd, req.data(), req.size(), 0) == (ssize_t)req.size();
  if (ok && body && body_len) ok = send(fd, body, body_len, 0) == (ssize_t)body_len;
  std::vector<uint8_t> raw;
  uint8_t buf[8192];
  while (ok) {
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    raw.insert(raw.end(), buf, buf + n);
    if (raw.size() > 4u * 1024 * 1024) break;
  }
  close(fd);
  if (!ok) return HTTPC_ERROR_SEND_HEADER_FAILED;
  // "HTTP/1.x NNN ...\r\n" headers "\r\n\r\n" body (HTTP/1.0: no chunking)
  const std::string head(raw.begin(), raw.begin() + (long)std::min<size_t>(raw.size(), 16384));
  const size_t eoh = head.find("\r\n\r\n");
  if (raw.size() < 12 || eoh == std::string::npos) return HTTPC_ERROR_CONNECTION_LOST;
  const int status = atoi(head.c_str() + 9);
  if (status <= 0) return HTTPC_ERROR_NO_HTTP_SERVER;
  out.status = status;
  out.body.assign(raw.begin() + (long)(eoh + 4), raw.end());
  out.location = url;
  return status;
}
#endif
