// SPDX-License-Identifier: GPL-3.0-or-later
// mbedtls_md subset: SHA-1 (built in, FIPS 180-4) and SHA-256 (rweather/Crypto), plus HMAC.
#include "mbedtls/md.h"
#include <SHA256.h>
#include <cstring>
#include <new>

namespace {
struct Sha1 {
  uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n;
  void reset() { h[0]=0x67452301; h[1]=0xEFCDAB89; h[2]=0x98BADCFE; h[3]=0x10325476; h[4]=0xC3D2E1F0; len=0; n=0; }
  static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4*i]<<24 | (uint32_t)p[4*i+1]<<16 | (uint32_t)p[4*i+2]<<8 | p[4*i+3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
  }
  void update(const uint8_t* p, size_t l) {
    len += l;
    while (l) { size_t k = 64 - n; if (k > l) k = l; memcpy(buf + n, p, k); n += k; p += k; l -= k; if (n == 64) { block(buf); n = 0; } }
  }
  void final(uint8_t out[20]) {
    uint64_t bits = len * 8; uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (n != 56) update(&z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lb, 8);
    for (int i = 0; i < 5; ++i) { out[4*i]=h[i]>>24; out[4*i+1]=h[i]>>16; out[4*i+2]=h[i]>>8; out[4*i+3]=h[i]; }
  }
};
struct Ctx {   // one running digest + the HMAC key pads
  mbedtls_md_type_t type;
  Sha1 s1; SHA256 s256;
  uint8_t opad[64];
  void reset() { if (type == MBEDTLS_MD_SHA1) s1.reset(); else s256.reset(); }
  void update(const uint8_t* p, size_t l) { if (type == MBEDTLS_MD_SHA1) s1.update(p, l); else s256.update(p, l); }
  void final(uint8_t* out) { if (type == MBEDTLS_MD_SHA1) s1.final(out); else s256.finalize(out, 32); }
  size_t size() const { return type == MBEDTLS_MD_SHA1 ? 20 : 32; }
};
}  // namespace

struct mbedtls_md_info_t { mbedtls_md_type_t type; unsigned char size; };
static const mbedtls_md_info_t k_sha1 = {MBEDTLS_MD_SHA1, 20};
static const mbedtls_md_info_t k_sha256 = {MBEDTLS_MD_SHA256, 32};

extern "C" {
const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t) {
  return t == MBEDTLS_MD_SHA1 ? &k_sha1 : t == MBEDTLS_MD_SHA256 ? &k_sha256 : nullptr;   // others: unsupported
}
unsigned char mbedtls_md_get_size(const mbedtls_md_info_t* info) { return info ? info->size : 0; }
void mbedtls_md_init(mbedtls_md_context_t* ctx) { memset(ctx, 0, sizeof *ctx); }
void mbedtls_md_free(mbedtls_md_context_t* ctx) { if (ctx && ctx->md_ctx) delete (Ctx*)ctx->md_ctx; if (ctx) memset(ctx, 0, sizeof *ctx); }
int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* info, int hmac) {
  if (!ctx || !info) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  Ctx* c = new (std::nothrow) Ctx();
  if (!c) return MBEDTLS_ERR_MD_ALLOC_FAILED;
  c->type = info->type;
  ctx->md_info = info; ctx->md_ctx = c; ctx->hmac_ctx = hmac ? c->opad : nullptr;
  return 0;
}
int mbedtls_md_starts(mbedtls_md_context_t* ctx) { if (!ctx || !ctx->md_ctx) return MBEDTLS_ERR_MD_BAD_INPUT_DATA; ((Ctx*)ctx->md_ctx)->reset(); return 0; }
int mbedtls_md_update(mbedtls_md_context_t* ctx, const unsigned char* in, size_t n) { if (!ctx || !ctx->md_ctx) return MBEDTLS_ERR_MD_BAD_INPUT_DATA; ((Ctx*)ctx->md_ctx)->update(in, n); return 0; }
int mbedtls_md_finish(mbedtls_md_context_t* ctx, unsigned char* out) { if (!ctx || !ctx->md_ctx) return MBEDTLS_ERR_MD_BAD_INPUT_DATA; ((Ctx*)ctx->md_ctx)->final(out); return 0; }
int mbedtls_md(const mbedtls_md_info_t* info, const unsigned char* in, size_t n, unsigned char* out) {
  mbedtls_md_context_t c; mbedtls_md_init(&c);
  int rc = mbedtls_md_setup(&c, info, 0);
  if (!rc) rc = mbedtls_md_starts(&c);
  if (!rc) rc = mbedtls_md_update(&c, in, n);
  if (!rc) rc = mbedtls_md_finish(&c, out);
  mbedtls_md_free(&c);
  return rc;
}
int mbedtls_md_hmac_starts(mbedtls_md_context_t* ctx, const unsigned char* key, size_t keylen) {
  if (!ctx || !ctx->md_ctx || !ctx->hmac_ctx) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  Ctx* c = (Ctx*)ctx->md_ctx;
  uint8_t k[64] = {0};
  if (keylen > 64) { c->reset(); c->update(key, keylen); c->final(k); } else memcpy(k, key, keylen);
  uint8_t ipad[64];
  for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; c->opad[i] = k[i] ^ 0x5c; }
  c->reset(); c->update(ipad, 64);
  return 0;
}
int mbedtls_md_hmac_update(mbedtls_md_context_t* ctx, const unsigned char* in, size_t n) { return mbedtls_md_update(ctx, in, n); }
int mbedtls_md_hmac_finish(mbedtls_md_context_t* ctx, unsigned char* out) {
  if (!ctx || !ctx->md_ctx || !ctx->hmac_ctx) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  Ctx* c = (Ctx*)ctx->md_ctx;
  uint8_t inner[32]; c->final(inner);
  c->reset(); c->update(c->opad, 64); c->update(inner, c->size()); c->final(out);
  return 0;
}
}
