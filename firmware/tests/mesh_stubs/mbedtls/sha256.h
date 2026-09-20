#pragma once
/* Native SHA-256 adapter only: wire.c still performs the real HMAC construction
 * and comparison. The radio/mesh implementation is compiled unchanged. */
#include <stddef.h>
#include <string.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX mbedtls_sha256_context;
#define native_sha256_init CC_SHA256_Init
#define native_sha256_update CC_SHA256_Update
#define native_sha256_final CC_SHA256_Final
#else
#include <openssl/sha.h>
typedef SHA256_CTX mbedtls_sha256_context;
#define native_sha256_init SHA256_Init
#define native_sha256_update SHA256_Update
#define native_sha256_final SHA256_Final
#endif
static inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}
static inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    return is224 || native_sha256_init(ctx) != 1 ? -1 : 0;
}
static inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx,
    const unsigned char *input, size_t len)
{
    return native_sha256_update(ctx, input, len) == 1 ? 0 : -1;
}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32])
{
    return native_sha256_final(output, ctx) == 1 ? 0 : -1;
}
static inline int mbedtls_sha256(const unsigned char *input, size_t len,
    unsigned char output[32], int is224)
{
    mbedtls_sha256_context ctx;
    int result = mbedtls_sha256_starts(&ctx, is224);
    if (!result) result = mbedtls_sha256_update(&ctx, input, len);
    if (!result) result = mbedtls_sha256_finish(&ctx, output);
    mbedtls_sha256_free(&ctx);
    return result;
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
