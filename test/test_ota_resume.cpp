// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the on-device update's copy + Range resume loop (#689, cbeb5184):
// src/ui-touch/OtaResume.h, the loop otaWorkerRun runs, against fake HTTPClient,
// stream and Update objects and a virtual clock.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_ota_resume.cpp -o /tmp/test_ota_resume \
//       && /tmp/test_ota_resume
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "ui-touch/OtaResume.h"

// ---- virtual time -------------------------------------------------------------------
static uint32_t g_ms = 0;
struct FakeSys {
  std::vector<std::string> lines;
  uint32_t millis() { return g_ms; }
  void delayMs(uint32_t ms) { g_ms += ms; }
  void yieldTick() { g_ms += 1; }
  void log(const char* l) { lines.push_back(l); }
};

// ---- the server: one image; each connection is cut or stalls where the plan says ----
enum class Cut { None, Drop, Stall, Pause };   // Pause: a gap, then the data continues
enum class RangeReply { Ok206, Full200, Wrong206, NotFound };
struct Plan {
  std::vector<size_t> cut_at;        // per connection: absolute offset where it is cut
  Cut how = Cut::Drop;
  uint32_t pause_ms = 0;             // Cut::Pause
  RangeReply range = RangeReply::Ok206;
};

struct FakeClient {};

struct FakeHttp;
struct FakeStream {
  FakeHttp* h = nullptr;
  int available();
  int read(uint8_t* b, size_t n);
};

struct FakeHttp {
  const std::vector<uint8_t>* image = nullptr;
  Plan plan;
  // connection state
  bool open = false;
  size_t pos = 0, limit = 0;          // next byte served, where this connection is cut
  bool cut = false;                   // dropped (connected() false)
  bool paused = false; uint32_t pause_until = 0;
  int conn = 0;                       // connections served so far (the first GET is #0)
  int size = -1;
  std::string range_hdr;
  std::vector<std::string> ranges_asked;
  int begins = 0, ends = 0;
  bool begin_fails_on_resume = false;
  FakeStream stream;
  unsigned drip = 0;                  // vary the chunk sizes

  void startConn(size_t from) {
    open = true; cut = false; pos = from; paused = false;
    limit = (size_t)conn < plan.cut_at.size() ? plan.cut_at[conn] : image->size();
    if (limit < from) limit = from;
    ++conn;
  }
  // The first GET (the worker does it before the copy loop): 200, whole image.
  void firstGet() { startConn(0); size = (int)image->size(); }

  bool begin(FakeClient&, const char*) {
    ++begins;
    if (begin_fails_on_resume) return false;
    range_hdr.clear(); return true;
  }
  void addHeader(const char* k, const char* v) { if (!strcmp(k, "Range")) range_hdr = v; }
  int GET() {
    ranges_asked.push_back(range_hdr);
    size_t from = 0;
    assert(sscanf(range_hdr.c_str(), "bytes=%zu-", &from) == 1);
    switch (plan.range) {
      case RangeReply::Ok206:    startConn(from); size = (int)(image->size() - from); return 206;
      case RangeReply::Full200:  startConn(0);    size = (int)image->size();          return 200;
      case RangeReply::Wrong206: startConn(from); size = (int)(image->size() - from) + 7; return 206;
      case RangeReply::NotFound: size = -1; return 404;
    }
    return -1;
  }
  int getSize() { return size; }
  FakeStream* getStreamPtr() { stream.h = this; return open ? &stream : nullptr; }
  bool connected() { return open && !cut; }
  void end() { ++ends; open = false; }
};

int FakeStream::available() {
  FakeHttp& s = *h;
  if (!s.open || s.cut) return 0;
  if (s.paused) {
    if (g_ms < s.pause_until) return 0;
    s.paused = false; s.limit = s.image->size();
  }
  if (s.pos >= s.limit) {
    if (s.pos >= s.image->size()) return 0;
    switch (s.plan.how) {
      case Cut::Drop:  s.cut = true; return 0;
      case Cut::Stall: return 0;                       // connected, nothing comes, ever
      case Cut::Pause: if (s.plan.pause_ms) { s.paused = true; s.pause_until = g_ms + s.plan.pause_ms; s.plan.pause_ms = 0; } return 0;
      case Cut::None:  return 0;
    }
  }
  // Uneven TCP-ish chunks, sometimes a zero first (a slow link).
  static const int k[] = { 1460, 0, 536, 2920, 4096, 7, 1460, 6000 };
  const int want = k[s.drip++ % 8];
  if (want == 0) { g_ms += 3; return 0; }
  const size_t left = s.limit - s.pos;
  return (int)((size_t)want < left ? (size_t)want : left);
}
int FakeStream::read(uint8_t* b, size_t n) {
  FakeHttp& s = *h;
  const size_t left = s.limit - s.pos;
  if (n > left) n = left;
  memcpy(b, s.image->data() + s.pos, n);
  s.pos += n;
  g_ms += 2;
  return (int)n;
}

struct FakeUpdate {
  std::vector<uint8_t> data;
  size_t fail_at = (size_t)-1;   // a flash write that comes up short once this many are in
  size_t write(const uint8_t* b, size_t n) {
    if (data.size() + n > fail_at) return n / 2;
    data.insert(data.end(), b, b + n);
    return n;
  }
};

struct Result {
  size_t written; bool write_failed; FakeHttp http; FakeUpdate upd; FakeSys sys; uint32_t took_ms;
};

static std::vector<uint8_t> makeImage(size_t n) {
  std::vector<uint8_t> v(n);
  uint32_t x = 0x12345678u;
  for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
  return v;
}

static Result run(const std::vector<uint8_t>& img, const Plan& plan, size_t fail_at = (size_t)-1,
                  bool begin_fails = false) {
  Result r{};
  r.http.image = &img; r.http.plan = plan; r.http.begin_fails_on_resume = begin_fails;
  r.upd.fail_at = fail_at;
  r.http.firstGet();
  FakeClient client;
  uint8_t buf[4096];
  g_ms = 1000;
  const uint32_t t0 = g_ms;
  r.written = OtaResume::copy(r.http, client, "http://x/y.bin", (int)img.size(), r.upd, buf, sizeof buf,
                              r.write_failed, r.sys);
  r.took_ms = g_ms - t0;
  return r;
}

static bool sameBytes(const Result& r, const std::vector<uint8_t>& img, size_t n) {
  return r.upd.data.size() == n && memcmp(r.upd.data.data(), img.data(), n) == 0;
}

int main() {
  const std::vector<uint8_t> img = makeImage(1234567);   // an odd size, about a V4 app image
  const size_t p40 = img.size() * 40 / 100;

  // ---- clean download: no Range request, the image byte for byte ----
  {
    Plan p; p.how = Cut::None;
    Result r = run(img, p);
    assert(r.written == img.size() && !r.write_failed && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.empty() && r.sys.lines.empty());
    assert(!r.http.open);   // http.end() after the copy
  }

  // ---- the connection drops at 40%: a Range from exactly there, 206, completes ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop;
    Result r = run(img, p);
    assert(r.written == img.size() && !r.write_failed && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.size() == 1);
    char want[40]; snprintf(want, sizeof want, "bytes=%zu-", p40);
    assert(r.http.ranges_asked[0] == want);
    assert(r.sys.lines.size() == 1 && r.sys.lines[0].find("stopped at") != std::string::npos);
    assert(!r.http.open);
  }

  // ---- the server ignores Range and answers 200 (the whole file again): NOT appended ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop; p.range = RangeReply::Full200;
    Result r = run(img, p);
    assert(r.written == p40);                       // the caller reports "short read" + Update.abort
    assert(sameBytes(r, img, p40));                 // nothing from the 200 body went in
    assert(r.http.ranges_asked.size() == 1);
    assert(r.sys.lines.back().find("resume -> 200") != std::string::npos);
    assert(!r.http.open);
  }

  // ---- a 206 whose length is not exactly the rest: not appended either ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop; p.range = RangeReply::Wrong206;
    Result r = run(img, p);
    assert(r.written == p40 && sameBytes(r, img, p40));
    assert(r.sys.lines.back().find("resume -> 206") != std::string::npos);
  }

  // ---- dropped before the first byte: the resume asks from 0 and completes ----
  {
    Plan p; p.cut_at = { 0 }; p.how = Cut::Drop;
    Result r = run(img, p);
    assert(r.written == img.size() && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.size() == 1 && r.http.ranges_asked[0] == "bytes=0-");
  }

  // ---- a 404 on the resume: ends, nothing appended ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop; p.range = RangeReply::NotFound;
    Result r = run(img, p);
    assert(r.written == p40 && sameBytes(r, img, p40));
  }

  // ---- the link stalls (still "connected", no data) for longer than 15 s: resumes ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Stall;
    Result r = run(img, p);
    assert(r.written == img.size() && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.size() == 1);
    assert(r.took_ms >= 15000 + 1000);              // waited out the 15 s, then the 1 s pause
    assert(r.took_ms < 15000 + 1000 + 5000);
  }

  // ---- a 10 s gap on the same connection is not a stall: no resume ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Pause; p.pause_ms = 10000;
    Result r = run(img, p);
    assert(r.written == img.size() && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.empty());
  }

  // ---- 3 drops, then the 4th connection completes ----
  {
    Plan p; p.how = Cut::Drop;
    p.cut_at = { img.size() / 5, img.size() * 2 / 5, img.size() * 3 / 5 };
    Result r = run(img, p);
    assert(r.written == img.size() && sameBytes(r, img, img.size()));
    assert(r.http.ranges_asked.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
      char want[40]; snprintf(want, sizeof want, "bytes=%zu-", p.cut_at[i]);
      assert(r.http.ranges_asked[i] == want);
    }
  }

  // ---- 4 drops: gives up after the 3rd resume, short ----
  {
    Plan p; p.how = Cut::Drop;
    p.cut_at = { img.size() / 5, img.size() * 2 / 5, img.size() * 3 / 5, img.size() * 4 / 5 };
    Result r = run(img, p);
    assert(r.written == img.size() * 4 / 5 && sameBytes(r, img, r.written));
    assert(r.http.ranges_asked.size() == 3);
    assert(!r.http.open);
  }

  // ---- a drop that sends nothing new on a resumed connection still counts ----
  {
    Plan p; p.how = Cut::Drop;
    p.cut_at = { p40, p40, p40, p40 };              // every resume dies at once
    Result r = run(img, p);
    assert(r.written == p40 && r.http.ranges_asked.size() == 3);
  }

  // ---- a flash write that fails: stop at once, never ask for a Range ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop;
    Result r = run(img, p, 100000);
    assert(r.write_failed && r.written < 100000 && r.http.ranges_asked.empty());
  }

  // ---- http.begin() fails on the resume: stop, short ----
  {
    Plan p; p.cut_at = { p40 }; p.how = Cut::Drop;
    Result r = run(img, p, (size_t)-1, true);
    assert(r.written == p40 && r.http.ranges_asked.empty());
  }

  // ---- no buffer (both mallocs failed): nothing read, the caller reports short read ----
  {
    Plan p;
    Result r{};
    r.http.image = &img; r.http.plan = p; r.http.firstGet();
    FakeClient c; bool wf = true;
    const size_t w = OtaResume::copy(r.http, c, "u", (int)img.size(), r.upd, (uint8_t*)nullptr, 4096, wf, r.sys);
    assert(w == 0 && !wf && r.upd.data.empty());
    assert(!r.http.open && r.http.ends == 1);   // the connection is ended, not left open
  }

  printf("test_ota_resume: ok\n");
  return 0;
}
