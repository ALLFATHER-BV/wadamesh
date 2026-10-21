#pragma once
// The web board's one door to the network: HTTPClient (compat/HTTPClient.h) sends every
// request through wadaHttpRequest() and serves the reply from memory (getString,
// getSize, getStreamPtr / writeToStream).
//
// Where requests go (compat/src/http_host.cpp):
//   * the firmware's map tile server (DEFAULT_TILE_SERVER in TouchPrefsStore.cpp, read by
//     web/CMakeLists.txt as WADA_TILE_SERVER): tiles + the line-of-sight elevation query
//   * the Store, http://firmware.wadamesh.com/apps (WADA_STORE_BASE, checked against
//     UITask.cpp at configure time): catalogs, app files, language files
//   * never the rest of firmware.wadamesh.com: the update check, OTA images, beta
//     reports and the install count are refused (CAP_OTA 0) and take their offline path
//   * anything else (a Lua app's wada.net, the Reader): fetched directly, which works
//     when that server sends CORS headers and fails cleanly otherwise. No open proxy.
//
//   browser  a synchronous emscripten_fetch from the calling pthread (the UI's net
//            worker, tile_fetch). The firmware's servers SAME-ORIGIN: <tile server>/z/x/y.jpg
//            becomes tiles/z/x/y.jpg and <store>/apps.json fw/apps/apps.json next to the
//            page (/tiles/... locally, /web/tiles/... on the site), which its host
//            proxies (web/scripts/serve.py; web/deploy/nginx-snippet.conf for the site).
//            A same-origin reply needs no CORS and satisfies COEP. Calls from the
//            browser main thread fail at once (a synchronous fetch would block the page).
//   native   plain HTTP/1.0 over a socket straight to the server (http:// only);
//            off in --headless runs unless WADA_NET=1, so the smoke tour stays offline.
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct WadaHttpReply {
  int status = 0;                 // HTTP status (200, 404, ...) when the request was made
  std::vector<uint8_t> body;
  std::string location;           // final URL after redirects (informational)
};
// method "GET"/"POST"; body may be null. Returns the HTTP status (> 0) or an
// HTTPC_ERROR_* code (< 0). headers: "Name: value" lines the UI added.
int wadaHttpRequest(const char* method, const std::string& url,
                    const std::vector<std::string>& headers,
                    const uint8_t* body, size_t body_len, uint32_t timeout_ms,
                    WadaHttpReply& out);

// Is the network up? WiFi.status() answers WL_CONNECTED from this (the browser is online),
// which is what the UI's tile download checks; WiFi.localIP() stays 0.0.0.0, so the
// features that need an address of their own (version check, File Transfer, Remote)
// stay off. Browser: navigator.onLine (the page reports changes); native: WADA_NET.
bool wadaNetOnline();
void wadaNetSetOnline(bool online);
