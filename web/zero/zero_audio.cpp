// SPDX-License-Identifier: GPL-3.0-or-later
// Cardputer Zero sound: the UI's Pager audio path (ES8311 codec + I2S, see
// sim_pager/driver/i2s.h) played on the Zero's ES8389 speaker through ALSA.
// One `aplay` per tone/WAV, fed raw 16-bit mono PCM; the UI writes from its own
// notification task, so a blocking pipe never stalls the screen.
//   WADA_ZERO_AUDIO=off          silence
//   WADA_ZERO_APLAY="aplay -D plughw:0,0"   another player / device
#include "Es8311Codec.h"
#include "driver/i2s.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex s_mu;
FILE* s_pipe = nullptr;
uint32_t s_rate = 16000;
int s_volume = 70;     // the codec's volume percent
bool s_mute = false;
bool s_off = false, s_checked = false;

// The ES8389 card's ALSA id from /proc/asound/cards (" 1 [ES8389Audio    ]: ... ES8389-Audio"),
// with its speaker switched on once.
std::string codecCard() {
  static std::string id;
  static bool looked = false;
  if (looked) return id;
  looked = true;
  if (FILE* f = fopen("/proc/asound/cards", "r")) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      if (!strstr(line, "ES8389") && !strstr(line, "es8389") && !strstr(line, "cardputer")) continue;
      const char* a = strchr(line, '['), *b = a ? strchr(a, ']') : nullptr;
      if (!a || !b) continue;
      id.assign(a + 1, b - a - 1);
      while (!id.empty() && id.back() == ' ') id.pop_back();
      break;
    }
    fclose(f);
  }
  if (!id.empty()) {
    char cmd[200];
    snprintf(cmd, sizeof cmd, "amixer -q -c %s sset Speaker on >/dev/null 2>&1", id.c_str());
    (void)!system(cmd);
    fprintf(stderr, "[zero] audio: ALSA card %s\n", id.c_str());
  } else {
    fprintf(stderr, "[zero] audio: no ES8389 card in /proc/asound/cards, using ALSA's default\n");
  }
  return id;
}

// PipeWire: when the user's sound server runs (it holds the ES8389 card open, so plain ALSA
// gets "device busy"), find the ES8389 sink by its description. Looked up once.
std::string s_rt_env;
bool s_have_paplay = false;
std::string pipewireSink() {
  static std::string sink;   // found once PipeWire is up; until then each tone checks again
  if (!sink.empty()) return sink;
  char rt[64];
  snprintf(rt, sizeof rt, "/run/user/%u", (unsigned)getuid());
  std::string sock = std::string(rt) + "/pipewire-0";
  if (access(sock.c_str(), F_OK) != 0) return sink;
  s_rt_env = std::string("XDG_RUNTIME_DIR=") + rt;
  s_have_paplay = system("command -v paplay >/dev/null 2>&1") == 0;
  const std::string q = s_rt_env + " pactl list sinks 2>/dev/null";
  std::string name;
  if (FILE* p = popen(q.c_str(), "r")) {
    char line[512];
    while (fgets(line, sizeof line, p)) {
      const char* t = line; while (*t == ' ' || *t == '\t') ++t;
      if (!strncmp(t, "Name: ", 6)) { name = t + 6; while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back(); }
      else if (!strncmp(t, "Description: ", 13) && (strstr(t, "8389") || strstr(t, "ardputer")) && sink.empty()) sink = name;
    }
    pclose(p);
  }
  if (sink.empty()) {   // no pactl or no match: let pw-cat pick the ES8389 node by its ALSA card
    sink = "alsa_output.platform-sound.stereo-fallback";
    if (FILE* p = popen((s_rt_env + " pw-cli ls Node 2>/dev/null").c_str(), "r")) {
      char line[512]; std::string cand;
      while (fgets(line, sizeof line, p)) {
        const char* n = strstr(line, "node.name = \"");
        if (n) { cand = n + 13; cand = cand.substr(0, cand.find('"')); }
        if ((strstr(line, "8389") || strstr(line, "ardputer")) && cand.rfind("alsa_output", 0) == 0) { sink = cand; break; }
      }
      pclose(p);
    }
  }
  fprintf(stderr, "[zero] audio: PipeWire sink %s (%s)\n", sink.c_str(), s_have_paplay ? "paplay" : "pw-cat");
  return sink;
}

void closePipe() {
  if (s_pipe) { pclose(s_pipe); s_pipe = nullptr; }
}
bool openPipe() {
  if (!s_checked) {
    s_checked = true;
    const char* e = getenv("WADA_ZERO_AUDIO");
    s_off = e && !strcmp(e, "off");
    signal(SIGPIPE, SIG_IGN);   // a player that dies must not take us with it
  }
  if (s_off) return false;
  if (s_pipe) return true;
  const char* player = getenv("WADA_ZERO_APLAY");
  char cmd[512];
  const std::string pw = pipewireSink();
  if (player && player[0]) {
    snprintf(cmd, sizeof cmd, "%s -q -t raw -f S16_LE -c 1 -r %u - 2>/dev/null", player, (unsigned)s_rate);
  } else if (!pw.empty()) {
    // PipeWire owns the card (Raspberry Pi OS desktop): play through it, to the ES8389
    // sink by name, so HDMI being the default output doesn't matter.
    if (s_have_paplay)
      snprintf(cmd, sizeof cmd, "%s paplay --raw --format=s16le --channels=1 --rate=%u --device=%s 2>/dev/null",
               s_rt_env.c_str(), (unsigned)s_rate, pw.c_str());
    else
      snprintf(cmd, sizeof cmd, "%s pw-cat --playback --target %s --format s16 --channels 1 --rate %u - 2>/dev/null",
               s_rt_env.c_str(), pw.c_str(), (unsigned)s_rate);
  } else {
    // Straight to the Zero's own codec card ("ES8389-Audio"), not ALSA's default (HDMI).
    const std::string card = codecCard();
    if (card.empty()) snprintf(cmd, sizeof cmd, "aplay -q -t raw -f S16_LE -c 1 -r %u - 2>/dev/null", (unsigned)s_rate);
    else snprintf(cmd, sizeof cmd, "aplay -q -D plughw:CARD=%s,DEV=0 -t raw -f S16_LE -c 1 -r %u - 2>/dev/null",
                  card.c_str(), (unsigned)s_rate);
  }
  s_pipe = popen(cmd, "w");
  return s_pipe != nullptr;
}
}  // namespace

esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t* cfg, int, void*) {
  std::lock_guard<std::mutex> l(s_mu);
  closePipe();
  s_mute = false;   // the Pager mutes the codec on uninstall; a new tone starts unmuted
  if (cfg && cfg->sample_rate) s_rate = cfg->sample_rate;
  return ESP_OK;   // the player starts with the first samples
}
esp_err_t i2s_driver_uninstall(i2s_port_t) {
  std::lock_guard<std::mutex> l(s_mu);
  closePipe();     // aplay plays what it has, then exits
  return ESP_OK;
}
esp_err_t i2s_set_sample_rates(i2s_port_t, uint32_t rate) {
  std::lock_guard<std::mutex> l(s_mu);
  if (rate && rate != s_rate) { closePipe(); s_rate = rate; }
  return ESP_OK;
}
esp_err_t i2s_write(i2s_port_t, const void* data, size_t n, size_t* written, uint32_t) {
  if (written) *written = n;
  std::lock_guard<std::mutex> l(s_mu);
  if (s_mute || s_volume == 0 || !openPipe()) return ESP_OK;
  const int16_t* in = (const int16_t*)data;
  const size_t count = n / 2;
  std::vector<int16_t> out(count);
  for (size_t i = 0; i < count; ++i) out[i] = (int16_t)((int)in[i] * s_volume / 100);
  if (fwrite(out.data(), 2, count, s_pipe) != count) closePipe();
  return ESP_OK;
}

void Es8311Codec::setMute(bool m) { std::lock_guard<std::mutex> l(s_mu); s_mute = m; }
void Es8311Codec::setVolumePercent(uint8_t v) { std::lock_guard<std::mutex> l(s_mu); s_volume = v > 100 ? 100 : v; }
