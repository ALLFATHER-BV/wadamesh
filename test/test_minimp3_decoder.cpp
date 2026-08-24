#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void* testMp3Scratch();
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_SCRATCH ((mp3dec_scratch_t*)testMp3Scratch())
#define MINIMP3_IMPLEMENTATION
#include "../lib/minimp3/minimp3.h"

static mp3dec_scratch_t g_scratch;
static void* testMp3Scratch() { return &g_scratch; }

struct DecodeBuffer {
  uint8_t input[16 * 1024];
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

static uint32_t readLe32(const uint8_t* value) {
  return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
         ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static long mp3DataEnd(FILE* file) {
  assert(fseek(file, 0, SEEK_END) == 0);
  long end = ftell(file);
  assert(end >= 0);
  uint8_t footer[128];
  if (end >= 128 && fseek(file, end - 128, SEEK_SET) == 0 &&
      fread(footer, 1, 3, file) == 3 && !memcmp(footer, "TAG", 3)) {
    end -= 128;
  }
  if (end >= 32 && fseek(file, end - 32, SEEK_SET) == 0 &&
      fread(footer, 1, 32, file) == 32 && !memcmp(footer, "APETAGEX", 8)) {
    const uint32_t version = readLe32(footer + 8);
    const uint32_t tag_size = readLe32(footer + 12);
    const uint32_t flags = readLe32(footer + 20);
    const uint64_t total_size = (uint64_t)tag_size + ((flags & 0x80000000u) ? 32u : 0u);
    if ((version == 1000 || version == 2000) && tag_size >= 32 && total_size <= (uint64_t)end)
      end -= (long)total_size;
  }
  rewind(file);
  return end;
}

static bool decodeFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "%s: could not open\n", path);
    return false;
  }
  const long data_end = mp3DataEnd(file);
  if (data_end <= 0) { fclose(file); return false; }

  DecodeBuffer buffer;
  mp3dec_t decoder;
  mp3dec_init(&decoder);
  size_t input_start = 0, input_count = 0;
  uint64_t samples_total = 0;
  int frames = 0, sample_rate = 0, channels = 0, bitrate_changes = 0;
  int last_bitrate = 0;
  bool eof = false, ok = true;

  while (ok) {
    if (!eof && input_count < 4096) {
      if (input_start && input_start + input_count + 4096 > sizeof buffer.input) {
        memmove(buffer.input, buffer.input + input_start, input_count);
        input_start = 0;
      }
      size_t room = sizeof buffer.input - (input_start + input_count);
      const long position = ftell(file);
      const size_t remaining = position < data_end ? (size_t)(data_end - position) : 0;
      if (room > remaining) room = remaining;
      const size_t got = fread(buffer.input + input_start + input_count, 1, room, file);
      if (got > 0) {
        input_count += got;
        eof = ftell(file) >= data_end;
      }
      else if (ferror(file)) ok = false;
      else eof = true;
    }

    if (!ok || !input_count) break;

    mp3dec_frame_info_t info = {};
    const int samples = mp3dec_decode_frame(&decoder, buffer.input + input_start,
      (int)input_count, buffer.pcm, &info);
    if (info.frame_bytes < 0 || (size_t)info.frame_bytes > input_count) {
      ok = false;
      break;
    }
    if (info.frame_bytes > 0) {
      input_start += (size_t)info.frame_bytes;
      input_count -= (size_t)info.frame_bytes;
      if (!input_count) input_start = 0;
    } else if (eof) {
      break;
    } else if (input_start + input_count == sizeof buffer.input) {
      ok = false;
      break;
    } else {
      continue;
    }

    if (samples <= 0) continue;
    if (info.layer != 3 || (info.channels != 1 && info.channels != 2) ||
        info.hz < 8000 || info.hz > 48000) {
      ok = false;
      break;
    }
    if (!sample_rate) {
      sample_rate = info.hz;
      channels = info.channels;
    } else if (sample_rate != info.hz) {
      ok = false;
      break;
    }
    if (last_bitrate && last_bitrate != info.bitrate_kbps) ++bitrate_changes;
    last_bitrate = info.bitrate_kbps;
    samples_total += (uint64_t)samples;
    ++frames;
  }

  fclose(file);
  if (!ok || frames == 0 || samples_total == 0) {
    fprintf(stderr, "%s: decode failed after %d frames\n", path, frames);
    return false;
  }
  printf("%s: %d frames, %llu samples/channel, %d Hz, %d channel(s), %d bitrate changes\n",
         path, frames, (unsigned long long)samples_total, sample_rate, channels,
         bitrate_changes);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.mp3 [file.mp3 ...]\n", argv[0]);
    return 2;
  }
  for (int i = 1; i < argc; ++i) assert(decodeFile(argv[i]));
  return 0;
}
