// SPDX-License-Identifier: GPL-3.0-or-later
// Host harness for test/history_segments/run.sh. Modes: 0 mixed chats, 1 many
// quiet chats (the ring fills), 2 one busy chat, 3 reboot-heavy, 4 power cuts mid-merge.
// Host harness for the chat-history segment scheduler (code extracted verbatim
// from UITask.cpp into SEG_INC). Simulated card = map<first_seq, records>.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <set>
#include <random>
#include <algorithm>

struct UITask { struct UIMessage { uint32_t seq; char thread[8]; int chat; }; };
struct { template<class... A> void printf(const char*, A...) {} } Serial;
struct UiSegHeader { char b[16]; };
struct UiSegMsg { char b[229]; };
#ifndef SEG_MAX
#define SEG_MAX 48
#endif
constexpr int k_ui_seg_soft = 24, k_ui_seg_max = SEG_MAX, k_ui_seg_records = 256;
enum : uint8_t { SEGJOB_NONE = 0, SEGJOB_APPEND, SEGJOB_COMPACT };
struct SegJob { uint8_t kind = SEGJOB_NONE; uint32_t first_seq = 0, last_seq = 0; bool create = false, repair = false; int n = 0; uint32_t absorb_seq = 0; };
struct UiSegInfo { uint32_t first_seq, last_seq, bytes; uint16_t disk_recs, live_recs; uint8_t compact_dirty, rewrite_open; };
static UiSegInfo s_seg[k_ui_seg_max]; static int s_seg_count = 0;
static uint32_t s_seg_total_bytes = 0, s_seg_flushed_seq = 0;
static uint8_t s_segjob_kind = SEGJOB_NONE; static uint32_t s_segjob_first_seq = 0, s_segjob_absorb_seq = 0;
static bool s_segjob_redirty = false, s_hist_flush_busy = false, s_hist_flush_req = false;
#include SEG_INC

using Msg = UITask::UIMessage;
static const int CAP = 5000, PER_CHAT = 250;
static std::vector<Msg> ring(CAP); static int count = 0, head = 0; static uint32_t seq_next = 1;
static std::map<uint32_t, std::vector<Msg>> disk;
static std::vector<Msg> buf(k_ui_seg_records);
static bool g_cut_merge = false; static int g_cuts = 0;
struct PowerCut {};

static void addMsg(int chat) {
  Msg m{}; m.seq = seq_next++; m.thread[0] = 'c'; m.chat = chat;
  if (count == CAP) segNoteEvicted(ring[head].seq);
  ring[head] = m; head = (head + 1) % CAP; if (count < CAP) ++count;
  int live = 0; for (int i = 0; i < count; ++i) { const Msg& r = ring[(head - count + i + CAP) % CAP]; if (r.thread[0] && r.chat == chat) ++live; }
  if (live > PER_CHAT) for (int i = 0; i < count; ++i) { Msg& r = ring[(head - count + i + CAP) % CAP];
    if (r.thread[0] && r.chat == chat) { r.thread[0] = 0; segMarkSeqDirty(r.seq); break; } }
}
// Run jobs until nothing builds. Returns false if records are still pending (stuck).
static bool flushAll() {
  for (int g = 0; g < 400; ++g) {
    SegJob job{};
    if (segBuildJob(ring.data(), CAP, count, head, buf.data(), &job) == 0)
      return !segMoreWorkPending(seq_next - 1);
    s_segjob_kind = job.kind; s_segjob_first_seq = job.first_seq; s_segjob_absorb_seq = job.absorb_seq; s_segjob_redirty = false;
    if (job.kind == SEGJOB_APPEND) { auto& f = disk[job.first_seq]; if (job.create) f.clear(); f.insert(f.end(), buf.begin(), buf.begin() + job.n); }
    else { if (job.n == 0) disk.erase(job.first_seq); else disk[job.first_seq].assign(buf.begin(), buf.begin() + job.n);
           if (job.absorb_seq && g_cut_merge) { ++g_cuts; throw PowerCut{}; }   // merged file landed, old one not yet removed
           if (job.absorb_seq) disk.erase(job.absorb_seq); }
    segCommitJob(job); s_segjob_kind = SEGJOB_NONE; s_segjob_absorb_seq = 0;
  }
  return false;
}
static void reboot() {   // mirrors loadMsgsFromSegments
  s_seg_count = 0; s_seg_total_bytes = 0; int w = 0; uint32_t max_seq = 0; int scanned = 0;
  std::vector<Msg> r(CAP);
  std::vector<uint32_t> husks;
  for (auto& kv : disk) {
    if (scanned++ >= k_ui_seg_max) break;
    UiSegInfo info{}; info.first_seq = kv.first; uint16_t nrec = 0;
    for (const Msg& m : kv.second) { if (m.seq <= max_seq) continue; max_seq = m.seq; info.last_seq = m.seq; r[w % CAP] = m; ++w; ++nrec; }
    info.disk_recs = info.live_recs = nrec;
    if (nrec == 0) { husks.push_back(kv.first); continue; }   // as uiSegRemoveFile in the loader
    if (s_seg_count < k_ui_seg_max) s_seg[s_seg_count++] = info;
  }
  for (uint32_t k : husks) disk.erase(k);
  const int keep = w > CAP ? CAP : w;
  if (w > CAP) std::rotate(r.begin(), r.begin() + (w % CAP), r.end());
  const uint32_t min_kept = keep ? r[0].seq : 0; int out = 0;
  for (int i = 0; i < s_seg_count; ++i) {
    if (!keep || s_seg[i].last_seq < min_kept) { disk.erase(s_seg[i].first_seq); continue; }
    if (s_seg[i].first_seq < min_kept) s_seg[i].compact_dirty = 1;
    s_seg[out++] = s_seg[i];
  }
  s_seg_count = out; ring = r; count = keep; head = keep % CAP; seq_next = max_seq + 1; s_seg_flushed_seq = max_seq;
}
static std::set<uint32_t> liveSet() { std::set<uint32_t> s; for (int i = 0; i < count; ++i) { const Msg& m = ring[(head - count + i + CAP) % CAP]; if (m.thread[0]) s.insert(m.seq); } return s; }

int main(int argc, char** argv) {
  const unsigned seed = argc > 1 ? atoi(argv[1]) : 1;
  const int mode = argc > 2 ? atoi(argv[2]) : 0;   // 0 mixed, 1 many quiet chats, 2 one busy chat, 3 reboot-heavy
  std::mt19937 rng(seed);
  int stuck = 0, lost_after_reboot = 0, lost_total = 0, max_files = 0, reboots = 0;
  long msgs = 0; int next_reboot = 300 + rng() % 1500;
  for (int step = 0; step < 60000; ++step) {
    // a few busy chats (trimmed hard by the per-chat cap) and many quiet ones
    int chat;
    if (mode == 1) chat = (int)(rng() % 400);                 // nobody hits the per-chat cap: the ring fills
    else if (mode == 2) chat = (rng() % 100 < 98) ? 0 : 1 + (int)(rng() % 30);
    else chat = (rng() % 100 < 85) ? (int)(rng() % 4) : 4 + (int)(rng() % 60);
    addMsg(chat); ++msgs;
    if (msgs % 25 == 0) {
      const std::set<uint32_t> before = liveSet();
      g_cut_merge = (mode == 4) && (rng() % 3 == 0);
      try { if (!flushAll()) ++stuck; }
      catch (PowerCut&) {   // power cut mid-merge: reboot from whatever the card holds
        // records that were never flushed are lost by any power cut; check only what was on disk
        std::set<uint32_t> ondisk; for (auto& kv : disk) for (auto& m : kv.second) ondisk.insert(m.seq);
        reboot(); ++reboots;
        const std::set<uint32_t> after = liveSet();
        int lost = 0; for (uint32_t q : before) if (ondisk.count(q) && !after.count(q)) ++lost;
        std::set<uint32_t> seen; int dup = 0; for (int i = 0; i < count; ++i) { uint32_t q = ring[(head - count + i + CAP) % CAP].seq; if (!seen.insert(q).second) ++dup; }
        if (lost || dup) { ++lost_after_reboot; lost_total += lost + dup; }
      }
      g_cut_merge = false;
      max_files = std::max(max_files, (int)disk.size());
    }
    if (msgs == next_reboot) {
      if (!flushAll()) ++stuck;
      const std::set<uint32_t> before = liveSet();
      reboot(); ++reboots;
      const std::set<uint32_t> after = liveSet();
      int lost = 0; for (uint32_t s : before) if (!after.count(s)) ++lost;   // the ring never holds more than CAP, so 'before' is what must survive
      if (lost) { ++lost_after_reboot; lost_total += lost; }
      next_reboot = msgs + (mode == 3 ? 20 + rng() % 200 : 200 + rng() % 1500);
    }
  }
  printf("cuts=%d seed %u: msgs=%ld reboots=%d max_files=%d stuck_flushes=%d reboots_losing_msgs=%d msgs_lost=%d\n",
         g_cuts, seed, msgs, reboots, max_files, stuck, lost_after_reboot, lost_total);
  return (stuck || lost_total) ? 1 : 0;
}
