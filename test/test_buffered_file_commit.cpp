// Host test for helpers/BufferedFileCommit.h (#734: "Save update bin to SD").
// Build: g++ -std=c++17 -I src -o /tmp/t test/test_buffered_file_commit.cpp && /tmp/t
//
// The fake file behaves like a FAT file on a flaky card: a write can be short, and
// once a write fails the file stays failed (FatFs keeps the error on the file object)
// until it is closed and opened again.
//
// The second half drives FsSink (the production Sink) over a fake fs::FS/fs::File that
// copies arduino-esp32 2.0.17's VFSFileImpl rules: a 4 KB stdio buffer in front of
// FatFs, size() of a written file = stat(path) = the DIRECTORY ENTRY, which FAT only
// updates on close. beta_100 asked the open file for its size and read 0 every time.
#include <assert.h>
#include <stdio.h>
#include <map>
#include <memory>
#include <string>
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

// ---- fake arduino-esp32 FS (VFSFileImpl over FatFs, see the header comment) ----
struct FakeVfs {
  struct Entry { std::vector<uint8_t> data; size_t dir_size = 0; };  // data = FIL contents
  std::map<std::string, Entry> files;
  size_t fail_tail_at_close = 0;   // >0: the final flush in close() loses this many bytes
  int opens_w = 0, removes = 0;
};
struct FakeImpl {
  FakeVfs* vfs; std::string path; bool writing; std::vector<uint8_t> stdio_buf;
  bool written = false; size_t stat_size = 0; bool open = true;
  size_t write(const uint8_t* p, size_t n) {          // fwrite into a 4096-byte FILE buffer
    written = true;
    auto& e = vfs->files[path];
    for (size_t i = 0; i < n; ++i) {
      stdio_buf.push_back(p[i]);
      if (stdio_buf.size() == 4096) { e.data.insert(e.data.end(), stdio_buf.begin(), stdio_buf.end()); stdio_buf.clear(); }
    }
    return n;
  }
  size_t size() {                                     // VFSFileImpl::size(): stat(path) once written
    if (written) { stat_size = vfs->files[path].dir_size; written = false; }
    return stat_size;
  }
  void close() {                                      // fclose: flush, f_close writes the dir entry
    if (!open) return;
    open = false;
    if (!writing) return;
    auto& e = vfs->files[path];
    size_t keep = stdio_buf.size();
    if (vfs->fail_tail_at_close) { keep = keep > vfs->fail_tail_at_close ? keep - vfs->fail_tail_at_close : 0; vfs->fail_tail_at_close = 0; }
    e.data.insert(e.data.end(), stdio_buf.begin(), stdio_buf.begin() + keep);
    stdio_buf.clear();
    e.dir_size = e.data.size();
  }
};
struct FakeFile {
  std::shared_ptr<FakeImpl> p;
  explicit operator bool() const { return p && p->open; }
  size_t write(const uint8_t* b, size_t n) { return p ? p->write(b, n) : 0; }
  size_t size() { return p ? p->size() : 0; }
  bool isDirectory() const { return false; }
  void close() { if (p) p->close(); }
};
struct FakeFs {
  FakeVfs& v;
  FakeFile open(const char* path, const char* mode) {
    FakeFile f;
    const bool w = mode[0] == 'w';
    if (w) { ++v.opens_w; v.files[path] = FakeVfs::Entry(); }      // FA_CREATE_ALWAYS: size 0
    else if (!v.files.count(path)) return f;
    f.p = std::make_shared<FakeImpl>();
    f.p->vfs = &v; f.p->path = path; f.p->writing = w;
    f.p->stat_size = v.files[path].dir_size;                         // stat at open
    return f;
  }
  bool remove(const char* path) { ++v.removes; return v.files.erase(path) > 0; }
};
struct Hooks {
  int pauses = 0; size_t last = 0;
  void pause(int) { ++pauses; }
  void progress(size_t d, size_t) { last = d; }
};

static void arduinoFsCases() {
  const std::vector<uint8_t> img = payload(3212345);   // about a wadamesh app bin
  uint8_t bounce[4096];
  const char* path = "/BINS/wadamesh-beta_100-beta.bin";
  {  // the beta_100 check, reproduced: the open file reports 0 bytes, not the image size
    FakeVfs v; FakeFs fs{v};
    FakeFile f = fs.open(path, "w");
    assert(f.write(img.data(), img.size()) == img.size());
    assert(f.size() == 0);                 // directory entry not written yet
    f.close();
    FakeFile r = fs.open(path, "r");
    assert(r && r.size() == img.size());   // after close it is
  }
  {  // FsSink: one attempt, the card holds the exact image
    FakeVfs v; FakeFs fs{v}; Hooks h;
    BufferedFileCommit::FsSink<FakeFs, FakeFile, Hooks> sink{fs, path, img.size(), h, FakeFile()};
    assert(BufferedFileCommit::commit(sink, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(v.opens_w == 1 && h.pauses == 0 && h.last == img.size());
    assert(v.files[path].data == img);
  }
  {  // the tail is lost in close()'s final flush: caught, retried from byte 0
    FakeVfs v; FakeFs fs{v}; Hooks h; v.fail_tail_at_close = 100;
    BufferedFileCommit::FsSink<FakeFs, FakeFile, Hooks> sink{fs, path, img.size(), h, FakeFile()};
    assert(BufferedFileCommit::commit(sink, img.data(), img.size(), bounce, sizeof bounce, 3));
    assert(v.opens_w == 2 && v.removes == 1 && h.pauses == 1);
    assert(v.files[path].data == img);
  }
  {  // closeAndCheckSize on a short file says so (the streamed path uses it too)
    FakeVfs v; FakeFs fs{v};
    FakeFile f = fs.open(path, "w");
    f.write(img.data(), 5000);
    assert(!BufferedFileCommit::closeAndCheckSize(fs, f, path, img.size()));
    FakeFile g = fs.open(path, "w");
    g.write(img.data(), 5000);
    assert(BufferedFileCommit::closeAndCheckSize(fs, g, path, 5000));
  }
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
  arduinoFsCases();
  printf("buffered_file_commit: all passed\n");
  return 0;
}
