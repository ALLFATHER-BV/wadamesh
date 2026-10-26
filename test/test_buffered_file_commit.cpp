// Host test for helpers/BufferedFileCommit.h (#734: "Save update bin to SD").
// Build: g++ -std=c++17 -I src -o /tmp/t test/test_buffered_file_commit.cpp && /tmp/t
//
// The fake file behaves like a FAT file on a flaky card: a write can be short, and
// once a write fails the file stays failed (FatFs keeps the error on the file object)
// until it is closed and opened again.
#include <assert.h>
#include <stdio.h>
#include <vector>

#include "helpers/BufferedFileCommit.h"

struct FakeCard {
  std::vector<uint8_t> file;     // what is on the card
  bool   exists = false;
  bool   is_open = false;
  bool   sticky_error = false;
  int    opens = 0, discards = 0, pauses = 0;
  size_t max_take = 0;           // >0: each write takes at most this many bytes
  std::vector<long> fail_at;     // per open: byte offset whose write fails (-1 = never)
  std::vector<int>  open_fails;  // per open: 1 = open() fails
  size_t last_progress = 0;
  bool   progress_monotonic = true;

  bool open() {
    const int k = opens++;
    if (k < (int)open_fails.size() && open_fails[k]) return false;
    file.clear(); exists = true; is_open = true; sticky_error = false;
    return true;
  }
  size_t write(const uint8_t* p, size_t n) {
    assert(is_open);
    if (sticky_error) return 0;
    const int k = opens - 1;
    const long fa = k < (int)fail_at.size() ? fail_at[k] : -1;
    size_t take = n;
    if (max_take && take > max_take) take = max_take;
    if (fa >= 0 && (long)file.size() <= fa && (long)(file.size() + take) > fa) {
      sticky_error = true;
      return 0;
    }
    file.insert(file.end(), p, p + take);
    return take;
  }
  bool close() { is_open = false; return !sticky_error; }
  void discard() { assert(!is_open); file.clear(); exists = false; ++discards; }
  void pause(int) { ++pauses; }
  void progress(size_t done, size_t total) {
    assert(total > 0 && done <= total);
    if (done < last_progress) progress_monotonic = false;
    last_progress = done;
  }
};

static std::vector<uint8_t> payload(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 31u + 7u);
  return v;
}

int main() {
  const std::vector<uint8_t> img = payload(10000);
  uint8_t bounce[1024];

  {  // clean card: one open, exact bytes, no retries
    FakeCard c;
    assert(BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 1 && c.discards == 0 && c.pauses == 0);
    assert(c.file == img);
    assert(c.progress_monotonic && c.last_progress == img.size());
  }
  {  // short writes are pushed through, not treated as failure
    FakeCard c; c.max_take = 100;
    assert(BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 1 && c.file == img);
  }
  {  // the #734 case: a write fails mid-file and the file stays failed; the retry
     // starts over from byte 0 and the card ends up with the whole image
    FakeCard c; c.fail_at = {4500, -1};
    assert(BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 2 && c.discards == 1 && c.pauses == 1);
    assert(c.exists && c.file == img);
  }
  {  // fails twice, third attempt succeeds
    FakeCard c; c.fail_at = {0, 9999, -1};
    assert(BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 3 && c.discards == 2 && c.file == img);
  }
  {  // fails every time: false, and no partial file is left for the Launcher
    FakeCard c; c.fail_at = {100, 200, 300};
    assert(!BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 3 && c.discards == 3 && !c.exists && c.file.empty());
  }
  {  // open fails once, then works
    FakeCard c; c.open_fails = {1, 0};
    assert(BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(c.opens == 2 && c.discards == 0 && c.file == img);
  }
  {  // a payload that is an exact multiple of the bounce size, and an empty one
    const std::vector<uint8_t> even = payload(4096);
    FakeCard c;
    assert(BufferedFileCommit::commit(c, even.data(), even.size(), bounce, sizeof bounce, 1));
    assert(c.file == even);
    FakeCard e;
    assert(BufferedFileCommit::commit(e, even.data(), 0, bounce, sizeof bounce, 1));
    assert(e.exists && e.file.empty());
  }
  {  // bad arguments never touch the card
    FakeCard c;
    assert(!BufferedFileCommit::commit(c, nullptr, 10, bounce, sizeof bounce, 3));
    assert(!BufferedFileCommit::commit(c, img.data(), img.size(), nullptr, 16, 3));
    assert(!BufferedFileCommit::commit(c, img.data(), img.size(), bounce, 0, 3));
    assert(!BufferedFileCommit::commit(c, img.data(), img.size(), bounce, sizeof bounce, 0));
    assert(c.opens == 0);
  }
  printf("buffered_file_commit: all passed\n");
  return 0;
}
