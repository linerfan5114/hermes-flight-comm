/*
 * aes.h - Minimal AES-128 (ECB core) implementation.
 *
 * Standard FIPS-197 AES-128. Provided as a small, dependency-free
 * building block for hermes-flight-comm's authenticated encryption
 * layer (see src/crypto.c for the CBC + HMAC wrapper).
 *
 * This is NOT a hardened, side-channel-resistant implementation.
 * It is suitable for a learning/simulation project, not for
 * production use with real secrets.
 */
#ifndef HERMES_AES_H
#define HERMES_AES_H

#include <stdint.h>
#include <stddef.h>

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE   16   /* AES-128 */

typedef struct {
    uint8_t round_key[176]; /* 11 round keys * 16 bytes for AES-128 */
} aes_ctx_t;

/* Expand a 16-byte key into the round key schedule. */
void aes_init(aes_ctx_t *ctx, const uint8_t key[AES_KEY_SIZE]);

/* Encrypt/decrypt a single 16-byte block in place. */
void aes_encrypt_block(const aes_ctx_t *ctx, uint8_t block[AES_BLOCK_SIZE]);
void aes_decrypt_block(const aes_ctx_t *ctx, uint8_t block[AES_BLOCK_SIZE]);

/*
 * CBC mode helpers with PKCS#7 padding.
 * `out` must have room for at least in_len rounded up to the next
 * block boundary. Returns the output length in bytes, or 0 on error.
 */
size_t aes_cbc_encrypt(const uint8_t key[AES_KEY_SIZE],
                        const uint8_t iv[AES_BLOCK_SIZE],
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap);

/*
 * Decrypts and strips PKCS#7 padding. Returns the plaintext length,
 * or 0 on error (bad padding / bad input length).
 */
size_t aes_cbc_decrypt(const uint8_t key[AES_KEY_SIZE],
                        const uint8_t iv[AES_BLOCK_SIZE],
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap);

#endif /* HERMES_AES_H */
