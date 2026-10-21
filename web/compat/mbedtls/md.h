#pragma once
// The mbedtls_md subset wada uses (SHA-1/SHA-256 digests + HMAC), on rweather/Crypto's
// SHA256 and a small built-in SHA-1 (web/compat/src/md_host.cpp). Same return codes.
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_MD5, MBEDTLS_MD_SHA1, MBEDTLS_MD_SHA224,
               MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA384, MBEDTLS_MD_SHA512 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t mbedtls_md_info_t;
typedef struct mbedtls_md_context_t {
  const mbedtls_md_info_t* md_info;
  void* md_ctx;
  void* hmac_ctx;
} mbedtls_md_context_t;
#define MBEDTLS_ERR_MD_BAD_INPUT_DATA -0x5100
#define MBEDTLS_ERR_MD_ALLOC_FAILED   -0x5180
#define MBEDTLS_MD_MAX_SIZE 64
const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t);
unsigned char mbedtls_md_get_size(const mbedtls_md_info_t* info);
void mbedtls_md_init(mbedtls_md_context_t* ctx);
void mbedtls_md_free(mbedtls_md_context_t* ctx);
int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* info, int hmac);
int mbedtls_md_starts(mbedtls_md_context_t* ctx);
int mbedtls_md_update(mbedtls_md_context_t* ctx, const unsigned char* in, size_t ilen);
int mbedtls_md_finish(mbedtls_md_context_t* ctx, unsigned char* out);
int mbedtls_md(const mbedtls_md_info_t* info, const unsigned char* in, size_t ilen, unsigned char* out);
int mbedtls_md_hmac_starts(mbedtls_md_context_t* ctx, const unsigned char* key, size_t keylen);
int mbedtls_md_hmac_update(mbedtls_md_context_t* ctx, const unsigned char* in, size_t ilen);
int mbedtls_md_hmac_finish(mbedtls_md_context_t* ctx, unsigned char* out);
#ifdef __cplusplus
}
#endif
