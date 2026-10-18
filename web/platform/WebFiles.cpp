// SPDX-License-Identifier: GPL-3.0-or-later
// Files between the UI and the visitor (see WebFiles.h).
#include "WebFiles.h"
#include <Arduino.h>
#include <string>
#include <vector>
#include "wada_host.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace {
std::string s_picked;   // the SPIFFS path of a file the visitor chose (UI thread only)
}

#if defined(__EMSCRIPTEN__)
EM_JS(void, wada_js_download, (const uint8_t* p, int n, const char* name), {
  if (Module["wadaDownload"]) Module["wadaDownload"](HEAPU8.slice(p, p + n), UTF8ToString(name));
});
EM_JS(void, wada_js_pick, (const char* accept), {
  if (Module["wadaPickFile"]) Module["wadaPickFile"](UTF8ToString(accept));
});
extern "C" EMSCRIPTEN_KEEPALIVE void wadaWebFilePicked(const char* spiffs_path) {
  if (spiffs_path && spiffs_path[0] == '/') s_picked = spiffs_path;
}
#endif

void boardHandOffFile(fs::FS& fs, const char* path, const char* download_name) {
  File f = fs.open(path, "r");
  if (!f) return;
  std::vector<uint8_t> buf(f.size());
  const size_t n = buf.empty() ? 0 : f.read(buf.data(), buf.size());
  f.close();
#if defined(__EMSCRIPTEN__)
  wada_js_download(buf.data(), (int)n, download_name);
#else
  const std::string dir = wadaHostDataDir() + "/downloads";
  wadaHostMkdirs(dir);
  const std::string out = dir + "/" + download_name;
  if (FILE* o = fopen(out.c_str(), "wb")) {
    fwrite(buf.data(), 1, n, o);
    fclose(o);
    Serial.printf("[web] download: %s\n", out.c_str());
  }
#endif
}

void boardPickFile(const char* accept) {
#if defined(__EMSCRIPTEN__)
  wada_js_pick(accept ? accept : "");
#else
  (void)accept;
  Serial.println("[web] no file picker in the native build: put the file in the data dir's spiffs/");
#endif
}

bool boardTakePickedFile(char* path, size_t cap) {
  if (s_picked.empty() || !path || cap == 0) return false;
  snprintf(path, cap, "%s", s_picked.c_str());
  s_picked.clear();
  return true;
}
