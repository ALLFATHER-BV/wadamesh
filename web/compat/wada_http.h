#pragma once
// The web board's one door to the network: HTTPClient (compat/HTTPClient.h) sends every
// request through wadaHttpRequest() and serves the reply from memory (getString,
// getSize, getStreamPtr / writeToStream), so turning HTTP on is a single function.
//
// Today (native and browser) it returns HTTPC_ERROR_CONNECTION_REFUSED: map tiles, the
// version check, the Store and the Reader take their offline paths.
//
// Planned for map tiles in the browser (not built yet):
//   - implement it with emscripten_fetch() + EMSCRIPTEN_FETCH_SYNCHRONOUS. A synchronous
//     fetch is only allowed off the browser main thread, and the UI's tile downloads
//     already run on their own task (tile_fetch, a pthread here), so the UI's blocking
//     HTTP code is unchanged. Calls from the main thread must keep failing fast.
//   - the tile server must answer with CORS (Access-Control-Allow-Origin) and, because
//     the page is cross-origin isolated (COEP: require-corp), with
//     Cross-Origin-Resource-Policy: cross-origin; or proxy tiles same-origin.
//   - the UI only queues tile downloads while Wi-Fi is up: the WiFi compat must then
//     report "connected" (compat/WiFi.h), gated on this hook being live.
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
