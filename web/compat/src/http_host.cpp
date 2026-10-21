// SPDX-License-Identifier: GPL-3.0-or-later
// wadaHttpRequest (compat/wada_http.h): the firmware's tile server and Store, same-origin;
// other servers directly (CORS); never the update check, OTA or reports.
#include "wada_http.h"
#include "HTTPClient.h"
#include "wada_host.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <strings.h>
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

#if !defined(WADA_TILE_SERVER) || !defined(WADA_STORE_BASE)
#error "WADA_TILE_SERVER / WADA_STORE_BASE: web/CMakeLists.txt reads them from the firmware's sources"
#endif

namespace {
std::atomic<int> g_online{-1};   // -1: not set yet (native: from WADA_NET)
// WADA_HTTP_DEBUG=1 (native) logs every request and its outcome.
void dbg(const std::string& url, int code, size_t n) {
  static const bool on = [] { const char* e = getenv("WADA_HTTP_DEBUG"); return e && *e == '1'; }();
  if (on) fprintf(stderr, "[http] %s -> %d (%u bytes)\n", url.c_str(), code, (unsigned)n);
}

// Where a request may go. The firmware's own servers are reached through the page's
// host, same-origin (local path next to the page, proxied by serve.py / nginx):
//   the map tile server (tiles, line-of-sight elevation)
//   the Store (app + language catalogs, app files, .lang files)
// The rest of the firmware server stays unreachable: the update check, OTA images,
// beta reports and the install count (CAP_OTA 0; nothing phones home from a page).
// Any other URL (a Lua app's wada.net request, the Reader) is fetched directly: it
// works when that server allows it (CORS), and fails cleanly otherwise. No open proxy.
struct Route { const char* base; const char* local; };
const Route kRoutes[] = {
  { WADA_TILE_SERVER, "tiles" },
  { WADA_STORE_BASE, "fw/apps" },
};
const char* const kNever[] = { "http://firmware.wadamesh.com", "https://firmware.wadamesh.com",
                               WADA_TILE_SERVER };
enum class Way { Refused, Proxied, Direct };
bool under(const std::string& url, const char* base) {
  const size_t n = strlen(base);
  return url.compare(0, n, base) == 0 && (url.size() == n || url[n] == '/' || url[n] == '?');
}
// For Proxied: `route` is the matching route, `rest` the part after its base.
Way classify(const std::string& url, const Route** route, std::string& rest) {
  for (const Route& r : kRoutes) {
    if (!under(url, r.base)) continue;
    rest = url.substr(strlen(r.base));
    if (rest.find("..") != std::string::npos) return Way::Refused;
    *route = &r;
    return Way::Proxied;
  }
  for (const char* b : kNever) if (under(url, b)) return Way::Refused;
  if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) return Way::Direct;
  return Way::Refused;
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
  out = WadaHttpReply();
  const Route* route = nullptr;
  std::string rest;
  const Way way = classify(url, &route, rest);
#if defined(WADA_NO_THREADS)
  // One thread: only a task (the net worker fiber) may wait for the network.
  if (way == Way::Refused || !wadaNetOnline() || !wadaCoopInTask()) return HTTPC_ERROR_CONNECTION_REFUSED;
#else
  if (way == Way::Refused || !wadaNetOnline() || emscripten_is_main_browser_thread())
    return HTTPC_ERROR_CONNECTION_REFUSED;
#endif
  // Proxied: same origin, relative to the page's folder (a pthread resolves it against
  // wadamesh.js, which sits next to index.html): /tiles/... locally, /web/tiles/... on
  // the site. Direct: the URL itself (CORS decides; an http:// one from an https page
  // is blocked by the browser).
  const std::string target = way == Way::Proxied ? std::string(route->local) + rest : url;
  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  strncpy(attr.requestMethod, method && *method ? method : "GET", sizeof(attr.requestMethod) - 1);
#if defined(WADA_NO_THREADS)
  // Asynchronous, and the task yields until it lands (a synchronous fetch would block
  // the page's only thread).
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;
  static volatile int s_done;   // one request at a time: only the net worker fetches
  s_done = 0;
  attr.onsuccess = [](emscripten_fetch_t*) { s_done = 1; };
  attr.onerror = [](emscripten_fetch_t*) { s_done = 1; };
#else
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;
#endif
  // The device bounds each socket read (2 s); here one call is the whole request through
  // a proxy that may fetch the file first, and this pthread has no watchdog.
  attr.timeoutMSecs = timeout_ms > 15000 ? timeout_ms : 15000;
  // Request headers a page may set (not User-Agent / Host: the proxy sets those).
  std::vector<std::string> kv;
  for (const std::string& h : headers) {
    const size_t c = h.find(':');
    if (c == std::string::npos) continue;
    std::string k = h.substr(0, c), v = h.substr(c + 1);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    if (strcasecmp(k.c_str(), "Content-Type") == 0 || strcasecmp(k.c_str(), "Accept") == 0) { kv.push_back(k); kv.push_back(v); }
  }
  std::vector<const char*> hp;
  for (const std::string& x : kv) hp.push_back(x.c_str());
  hp.push_back(nullptr);
  if (kv.size()) attr.requestHeaders = hp.data();
  if (body && body_len) { attr.requestData = (const char*)body; attr.requestDataSize = body_len; }
  emscripten_fetch_t* f = emscripten_fetch(&attr, target.c_str());
  if (!f) return HTTPC_ERROR_CONNECTION_REFUSED;
#if defined(WADA_NO_THREADS)
  while (!s_done) wadaCoopSleep(5);
#endif
  const int status = f->status;
  if (status > 0) {
    out.status = status;
    if (f->data && f->numBytes) out.body.assign((const uint8_t*)f->data, (const uint8_t*)f->data + f->numBytes);
    out.location = target;
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
  const Route* route = nullptr;
  std::string rest;
  const Way way = classify(url, &route, rest);
  if (way == Way::Refused || !wadaNetOnline()) return HTTPC_ERROR_CONNECTION_REFUSED;
  // Natively straight to the server, http:// only (no TLS here).
  const std::string full = way == Way::Proxied ? std::string(route->base) + rest : url;
  if (full.compare(0, 7, "http://") != 0) return HTTPC_ERROR_CONNECTION_REFUSED;
  const size_t slash = full.find('/', 7);
  std::string hostport = full.substr(7, slash == std::string::npos ? std::string::npos : slash - 7), port = "80";
  rest = slash == std::string::npos ? "/" : full.substr(slash);
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
