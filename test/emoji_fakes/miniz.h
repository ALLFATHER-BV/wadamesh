// Host stand-in for the ESP ROM's tinfl, on top of zlib's raw inflate. It covers
// exactly the call shape the firmware uses: one call, the whole input, a
// non-wrapping output buffer large enough for everything.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zlib.h>
typedef uint8_t mz_uint8;
typedef uint32_t mz_uint32;
typedef struct { int m_state; } tinfl_decompressor;
typedef enum { TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0 } tinfl_status;
enum { TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4 };
#define tinfl_init(r) do { (r)->m_state = 0; } while (0)
static int g_fake_tinfl_corrupt = 0;   // flip a byte of the input: inflate must fail
static inline tinfl_status tinfl_decompress(tinfl_decompressor*, const mz_uint8* in, size_t* in_size,
                                            mz_uint8*, mz_uint8* out, size_t* out_size, mz_uint32 flags) {
  if (!(flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)) return TINFL_STATUS_FAILED;
  z_stream s = {};
  if (inflateInit2(&s, -15) != Z_OK) return TINFL_STATUS_FAILED;
  uint8_t* copy = nullptr;
  if (g_fake_tinfl_corrupt) {
    copy = (uint8_t*)malloc(*in_size);
    for (size_t i = 0; i < *in_size; ++i) copy[i] = in[i];
    for (size_t i = 0; i < *in_size; i += 97) copy[i] ^= 0x5A;
    in = copy;
  }
  s.next_in = (Bytef*)in; s.avail_in = (uInt)*in_size;
  s.next_out = out; s.avail_out = (uInt)*out_size;
  const int r = inflate(&s, Z_FINISH);
  *in_size -= s.avail_in; *out_size -= s.avail_out;
  inflateEnd(&s);
  free(copy);
  return r == Z_STREAM_END ? TINFL_STATUS_DONE : TINFL_STATUS_FAILED;
}
