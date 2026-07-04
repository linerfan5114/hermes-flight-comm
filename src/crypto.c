/*
 * crypto.c - Authenticated encryption for audio packets.
 *
 * Construction: Encrypt-then-MAC.
 *   1. AES-128-CBC encrypts the payload with an IV derived from a
 *      monotonically increasing per-node counter (never reused with
 *      the same key -> avoids classic CBC IV-reuse pitfalls).
 *   2. HMAC-SHA256 authenticates (iv || ciphertext), so tampering
 *      with either is detected before any decryption is attempted.
 *
 * This is a standard, well-understood pattern (see e.g. RFC 7366 for
 * the general idea, though this is a from-scratch implementation).
 * We use HMAC-SHA256 for the MAC instead of a dedicated AEAD mode
 * like AES-GCM to keep the codebase small and easy to audit; the
 * security property (confidentiality + integrity + authenticity) is
 * equivalent when implemented correctly.
 */
#include "hermes.h"
#include "aes.h"
#include "sha256.h"
#include <string.h>

/* Derive a 16-byte IV from a 32-bit counter. Deterministic but unique
 * per message as long as the counter never repeats for a given key,
 * which the caller (node.c) guarantees via a monotonic tx_counter. */
static void derive_iv(uint32_t counter, uint8_t iv[HERMES_IV_SIZE]) {
    memset(iv, 0, HERMES_IV_SIZE);
    iv[0] = (uint8_t)(counter >> 24);
    iv[1] = (uint8_t)(counter >> 16);
    iv[2] = (uint8_t)(counter >> 8);
    iv[3] = (uint8_t)(counter);
    /* Remaining bytes stay zero; for a real deployment you'd mix in
     * a per-session random salt here too. */
}

static bool constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

size_t hermes_encrypt(const uint8_t key[HERMES_KEY_SIZE],
                       uint32_t nonce_counter,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap) {
    uint8_t iv[HERMES_IV_SIZE];
    derive_iv(nonce_counter, iv);

    /* Layout: [iv][ciphertext][tag] */
    if (out_cap < HERMES_IV_SIZE + HERMES_MAC_SIZE) return 0;
    memcpy(out, iv, HERMES_IV_SIZE);

    size_t cipher_cap = out_cap - HERMES_IV_SIZE - HERMES_MAC_SIZE;
    size_t cipher_len = aes_cbc_encrypt(key, iv, in, in_len,
                                         out + HERMES_IV_SIZE, cipher_cap);
    if (cipher_len == 0) return 0;

    uint8_t tag[HERMES_MAC_SIZE];
    hmac_sha256(key, HERMES_KEY_SIZE,
                out, HERMES_IV_SIZE + cipher_len, tag);
    memcpy(out + HERMES_IV_SIZE + cipher_len, tag, HERMES_MAC_SIZE);

    return HERMES_IV_SIZE + cipher_len + HERMES_MAC_SIZE;
}

size_t hermes_decrypt(const uint8_t key[HERMES_KEY_SIZE],
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap) {
    if (in_len < HERMES_IV_SIZE + HERMES_MAC_SIZE) return 0;

    size_t cipher_len = in_len - HERMES_IV_SIZE - HERMES_MAC_SIZE;
    const uint8_t *iv = in;
    const uint8_t *ciphertext = in + HERMES_IV_SIZE;
    const uint8_t *received_tag = in + HERMES_IV_SIZE + cipher_len;

    uint8_t expected_tag[HERMES_MAC_SIZE];
    hmac_sha256(key, HERMES_KEY_SIZE, in, HERMES_IV_SIZE + cipher_len,
                expected_tag);

    if (!constant_time_eq(expected_tag, received_tag, HERMES_MAC_SIZE)) {
        return 0; /* authentication failed: reject before decrypting */
    }

    return aes_cbc_decrypt(key, iv, ciphertext, cipher_len, out, out_cap);
}
