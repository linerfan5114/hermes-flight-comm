/*
 * sha256.h - Minimal SHA-256 + HMAC-SHA256.
 * Standard FIPS-180-4 / RFC 2104. Used by crypto.c to authenticate
 * encrypted audio frames (Encrypt-then-MAC construction).
 */
#ifndef HERMES_SHA256_H
#define HERMES_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[SHA256_BLOCK_SIZE];
    size_t   buffer_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrapper. */
void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* HMAC-SHA256: out must be SHA256_DIGEST_SIZE bytes. */
void hmac_sha256(const uint8_t *key, size_t key_len,
                  const uint8_t *msg, size_t msg_len,
                  uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* HERMES_SHA256_H */
