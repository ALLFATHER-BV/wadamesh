// SPDX-License-Identifier: GPL-3.0-or-later
// wadamesh-cli's other end, and the on-message hook (Cardputer Zero).
//
// A Unix socket at $WADA_DATA_DIR/wadamesh.sock takes one command per line:
//   status | contacts | channels | send #channel text | send @contact text | advert
//   tail (stream incoming messages) | shot | hud | sac | help
// Sends go out through the same mesh object the UI uses and show in its history.
//
// The hook: an executable $WADA_DATA_DIR/hooks/on-message runs for every incoming DM,
// channel post and room post, with WADA_KIND (dm/channel/room), WADA_FROM, WADA_CHANNEL,
// WADA_TEXT, WADA_HOPS, WADA_SNR, WADA_RSSI and WADA_NODE in its environment. Each line it
// prints as "reply <text>" is sent back to the same chat: a bot in a few lines of shell.
// Replies are rate limited (one per chat per 10 s, six a minute) so a bot can never flood
// the mesh; a hook gets 10 s to finish.
#include "zero_ctl.h"
#include <Arduino.h>
#include "WebMesh.h"
#include "zero_battery.h"
#include "zero_gps.h"
#include "zero_sys.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <deque>
#include <string>
#include <vector>

extern MyMesh& the_mesh;

namespace {
int s_listen = -1;
std::string s_sock_path, s_hook_path;
struct Client { int fd; std::string in; bool tail = false; };
std::vector<Client> s_clients;
ZeroCtlActions s_actions;

struct Hook {
  pid_t pid; int out; std::string buf; uint32_t started;
  int kind; int channel_idx; std::string who;   // where a reply goes
};
std::vector<Hook> s_hooks;
std::deque<uint32_t> s_reply_times;              // the last minute's replies
std::vector<std::pair<std::string, uint32_t>> s_chat_last;   // per chat, the last reply

std::string dataDir() { const char* d = getenv("WADA_DATA_DIR"); return d && d[0] ? d : "."; }

void say(int fd, const std::string& s) {
  std::string t = s; if (t.empty() || t.back() != '\n') t += '\n';
  (void)!send(fd, t.data(), t.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
}

int findChannel(const char* name) {
  if (*name == '#') ++name;
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
    const char* n = cd.name[0] == '#' ? cd.name + 1 : cd.name;
    if (!strcasecmp(n, name)) return i;
  }
  return -1;
}
bool findContact(const char* name, ContactInfo& out) {
  if (*name == '@') ++name;
  const int n = the_mesh.getNumContacts();
  int prefix_hits = 0; ContactInfo prefix{};
  for (int i = 0; i < n; ++i) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(i, c)) continue;
    if (!strcasecmp(c.name, name)) { out = c; return true; }
    if (!strncasecmp(c.name, name, strlen(name))) { prefix = c; prefix_hits++; }
  }
  if (prefix_hits == 1) { out = prefix; return true; }
  return false;
}

bool sendToChannel(int idx, const std::string& text, std::string& err) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd)) { err = "no such channel"; return false; }
  const char* me = the_mesh.getNodePrefs()->node_name;
  if (!the_mesh.sendGroupMessage((uint32_t)time(nullptr), cd.channel, me, text.c_str(), (int)text.size())) {
    err = "the node refused it (link down?)"; return false;
  }
  the_mesh.zeroRecordSentChannel(idx, text.c_str());
  return true;
}
bool sendToContact(const ContactInfo& c, const std::string& text, std::string& err) {
  uint32_t ack = 0, timeout = 0;
  if (the_mesh.sendMessage(c, (uint32_t)time(nullptr), 0, text.c_str(), ack, timeout) == 0) {   // MSG_SEND_FAILED
    err = "the node refused it (link down?)"; return false;
  }
  the_mesh.zeroRecordSentDirect(c.id.pub_key, text.c_str(), ack);
  return true;
}

std::string statusText() {
  const NodePrefs* p = the_mesh.getNodePrefs();
  const ZeroBattery& b = zeroBattery();
  const ZeroGpsStatus g = zeroGpsStatus();
  const ZeroSys s = zeroSys();
  char buf[1024];
  snprintf(buf, sizeof buf,
           "node     %s\n"
           "link     %s\n"
           "radio    %.3f MHz  BW %.1f  SF%u  CR%u  %d dBm\n"
           "contacts %d\n"
           "battery  %d%%  %.2f V  %+d mA%s\n"
           "gps      %s\n"
           "net      %s %s %s\n"
           "system   %s  up %s  load %.2f  %.0f C  mem %d/%d MB\n",
           p->node_name, zeroCtlLinkText().c_str(), p->freq, p->bw, (unsigned)p->sf, (unsigned)p->cr,
           (int)p->tx_power_dbm, the_mesh.getNumContacts(), b.soc, b.mv / 1000.0, b.ma,
           b.charging ? "  charging" : b.full ? "  full" : "",
           g.fix ? (std::to_string(g.lat).substr(0, 9) + ", " + std::to_string(g.lon).substr(0, 10) + "  " +
                    std::to_string(g.sats) + " sats").c_str()
                 : g.nmea ? "searching" : g.port ? "no data" : "off",
           s.iface.empty() ? "offline" : s.iface.c_str(), s.ip.c_str(), s.ssid.c_str(), s.host.c_str(),
           zeroUptimeText(s.uptime_s).c_str(), s.load1, s.cpu_temp_c, s.mem_total_mb - s.mem_avail_mb,
           s.mem_total_mb);
  return buf;
}

void command(Client& c, const std::string& line) {
  std::string cmd = line, rest;
  const size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); rest = line.substr(sp + 1); }
  if (cmd == "status") { say(c.fd, statusText()); }
  else if (cmd == "contacts") {
    std::string out;
    for (int i = 0; i < the_mesh.getNumContacts(); ++i) {
      ContactInfo ci;
      if (!the_mesh.getContactByIdx(i, ci)) continue;
      const char* t = ci.type == ADV_TYPE_REPEATER ? "repeater" : ci.type == ADV_TYPE_ROOM ? "room" :
                      ci.type == ADV_TYPE_SENSOR ? "sensor" : "chat";
      out += std::string(t) + "\t" + ci.name + "\n";
    }
    say(c.fd, out.empty() ? "(no contacts)" : out);
  } else if (cmd == "channels") {
    std::string out;
    for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
      ChannelDetails cd;
      if (the_mesh.getChannel(i, cd) && cd.name[0]) out += std::to_string(i) + "\t" + cd.name + "\n";
    }
    say(c.fd, out.empty() ? "(no channels)" : out);
  } else if (cmd == "send") {
    const size_t sp2 = rest.find(' ');
    if (sp2 == std::string::npos) { say(c.fd, "usage: send #channel text | send @contact text"); return; }
    const std::string to = rest.substr(0, sp2), text = rest.substr(sp2 + 1);
    std::string err;
    if (to[0] == '#' || findChannel(to.c_str()) >= 0) {
      const int idx = findChannel(to.c_str());
      if (idx < 0) { say(c.fd, "error: no channel " + to); return; }
      say(c.fd, sendToChannel(idx, text, err) ? "sent" : "error: " + err);
    } else {
      ContactInfo ci;
      if (!findContact(to.c_str(), ci)) { say(c.fd, "error: no single contact matches " + to); return; }
      say(c.fd, sendToContact(ci, text, err) ? std::string("sent to ") + ci.name : "error: " + err);
    }
  } else if (cmd == "advert") { say(c.fd, the_mesh.sendAdvert(true) ? "advert sent" : "error: advert failed"); }
  else if (cmd == "tail") { c.tail = true; say(c.fd, "# tailing incoming messages (Ctrl+C to stop)"); }
  else if (cmd == "shot") { if (s_actions.screenshot) say(c.fd, s_actions.screenshot()); }
  else if (cmd == "hud") { if (s_actions.hud) s_actions.hud(); say(c.fd, "ok"); }
  else if (cmd == "sac") { if (s_actions.sac) s_actions.sac(); say(c.fd, "the valley says hi"); }
  else if (cmd == "help" || cmd.empty()) {
    say(c.fd, "status | contacts | channels | send #channel text | send @contact text | advert | tail | shot | hud");
  } else say(c.fd, "unknown command: " + cmd + " (try help)");
}

// ---- hook replies ---------------------------------------------------------------------
bool replyAllowed(const std::string& chat) {
  const uint32_t now = millis();
  while (!s_reply_times.empty() && now - s_reply_times.front() > 60000) s_reply_times.pop_front();
  if (s_reply_times.size() >= 6) return false;
  for (auto& e : s_chat_last) if (e.first == chat && now - e.second < 10000) return false;
  return true;
}
void noteReply(const std::string& chat) {
  const uint32_t now = millis();
  s_reply_times.push_back(now);
  for (auto& e : s_chat_last) if (e.first == chat) { e.second = now; return; }
  s_chat_last.push_back({chat, now});
}
void hookReply(const Hook& h, std::string text) {
  if (text.size() > 140) text.resize(140);
  const std::string chat = (h.kind == 1 ? "#" + std::to_string(h.channel_idx) : "@" + h.who);
  if (!replyAllowed(chat)) { fprintf(stderr, "[hook] reply to %s dropped (rate limit)\n", chat.c_str()); return; }
  std::string err;
  bool ok = false;
  if (h.kind == 1) ok = sendToChannel(h.channel_idx, text, err);
  else if (h.kind == 0) { ContactInfo ci; ok = findContact(h.who.c_str(), ci) && sendToContact(ci, text, err); }
  if (ok) noteReply(chat);
  fprintf(stderr, "[hook] reply to %s: %s\n", chat.c_str(), ok ? "sent" : err.c_str());
}
void hookLines(Hook& h, bool final) {
  size_t nl;
  while ((nl = h.buf.find('\n')) != std::string::npos || (final && !h.buf.empty())) {
    std::string l = nl == std::string::npos ? h.buf : h.buf.substr(0, nl);
    h.buf = nl == std::string::npos ? "" : h.buf.substr(nl + 1);
    if (!l.compare(0, 6, "reply ") && h.kind != 2) hookReply(h, l.substr(6));
    if (nl == std::string::npos) break;
  }
}
}  // namespace

void zeroCtlBegin(const ZeroCtlActions& actions) {
  s_actions = actions;
  s_hook_path = dataDir() + "/hooks/on-message";
  mkdir((dataDir() + "/hooks").c_str(), 0755);
  s_sock_path = dataDir() + "/wadamesh.sock";
  unlink(s_sock_path.c_str());
  s_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  strncpy(a.sun_path, s_sock_path.c_str(), sizeof a.sun_path - 1);
  if (s_listen < 0 || bind(s_listen, (sockaddr*)&a, sizeof a) || listen(s_listen, 4)) {
    fprintf(stderr, "[ctl] %s: %s\n", s_sock_path.c_str(), strerror(errno));
    if (s_listen >= 0) close(s_listen);
    s_listen = -1; return;
  }
  chmod(s_sock_path.c_str(), 0660);
  fprintf(stderr, "[ctl] listening on %s\n", s_sock_path.c_str());
}

void zeroCtlPoll() {
  if (s_listen >= 0) {
    for (int fd; (fd = accept4(s_listen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0;) s_clients.push_back({fd});
  }
  for (size_t i = 0; i < s_clients.size();) {
    Client& c = s_clients[i];
    char b[512];
    const ssize_t n = recv(c.fd, b, sizeof b, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      close(c.fd); s_clients.erase(s_clients.begin() + i); continue;
    }
    if (n > 0) {
      c.in.append(b, n);
      size_t nl;
      while ((nl = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, nl);
        c.in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        command(c, line);
      }
    }
    ++i;
  }
  // Hooks: collect output, reap, time out.
  for (size_t i = 0; i < s_hooks.size();) {
    Hook& h = s_hooks[i];
    char b[256];
    ssize_t n;
    while ((n = read(h.out, b, sizeof b)) > 0) h.buf.append(b, n);
    hookLines(h, false);
    int st;
    const pid_t r = waitpid(h.pid, &st, WNOHANG);
    if (r == h.pid || r < 0) {
      while ((n = read(h.out, b, sizeof b)) > 0) h.buf.append(b, n);
      hookLines(h, true);
      close(h.out); s_hooks.erase(s_hooks.begin() + i); continue;
    }
    if (millis() - h.started > 10000) { kill(h.pid, SIGKILL); }
    ++i;
  }
}

void zeroCtlShutdown() {
  for (auto& c : s_clients) close(c.fd);
  s_clients.clear();
  if (s_listen >= 0) { close(s_listen); unlink(s_sock_path.c_str()); s_listen = -1; }
}

namespace { std::string s_link = "connecting"; }
void zeroCtlSetLink(const std::string& t) { s_link = t; }
std::string zeroCtlLinkText() { return s_link; }

void zeroOnIncoming(int kind, const char* from, int channel_idx, const char* where, const char* text,
                    uint8_t hops, int8_t snr_q4, int8_t rssi) {
  std::string who = from ? from : "", body = text ? text : "";
  if (kind == 1) {   // a channel post reads "Sender: text"
    const size_t c = body.find(": ");
    if (c != std::string::npos && c < 32) { who = body.substr(0, c); body = body.substr(c + 2); }
  }
  // tail
  char line[400];
  if (kind == 0) snprintf(line, sizeof line, "dm  %s: %s", who.c_str(), body.c_str());
  else if (kind == 1) snprintf(line, sizeof line, "ch  %s  %s: %s", where ? where : "?", who.c_str(), body.c_str());
  else snprintf(line, sizeof line, "rm  %s  %s: %s", where ? where : "?", who.c_str(), body.c_str());
  for (auto& c : s_clients) if (c.tail) say(c.fd, line);
  // hook
  if (access(s_hook_path.c_str(), X_OK) != 0 || s_hooks.size() >= 4) return;
  // Everything the child needs is built before fork(): other threads may hold the heap
  // lock, so the child only does async-signal-safe calls (dup2, open, execve).
  std::vector<std::string> env;
  extern char** environ;
  for (char** e = environ; e && *e; ++e) if (strncmp(*e, "WADA_", 5)) env.push_back(*e);
  env.push_back(std::string("WADA_KIND=") + (kind == 0 ? "dm" : kind == 1 ? "channel" : "room"));
  env.push_back("WADA_FROM=" + who);
  env.push_back(std::string("WADA_CHANNEL=") + (where ? where : ""));
  env.push_back("WADA_TEXT=" + body);
  env.push_back("WADA_HOPS=" + std::to_string(hops == 0xFF ? 0 : hops));
  char snr[16]; snprintf(snr, sizeof snr, "%.2f", snr_q4 / 4.0);
  env.push_back(std::string("WADA_SNR=") + snr);
  env.push_back("WADA_RSSI=" + std::to_string(rssi));
  env.push_back(std::string("WADA_NODE=") + the_mesh.getNodePrefs()->node_name);
  env.push_back("WADA_DATA_DIR=" + dataDir());
  std::vector<char*> envp;
  for (auto& e : env) envp.push_back(&e[0]);
  envp.push_back(nullptr);
  char* argv[] = {&s_hook_path[0], nullptr};
  int p[2];
  if (pipe2(p, O_CLOEXEC)) return;
  const pid_t pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); return; }
  if (pid == 0) {
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);   // hooks run without the service's caps
    dup2(p[1], 1);
    const int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) dup2(nul, 0);
    execve(argv[0], argv, envp.data());
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  s_hooks.push_back({pid, p[0], "", millis(), kind, channel_idx, who});
}
