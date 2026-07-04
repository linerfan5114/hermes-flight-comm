/*
 * test_hermes.c - Basic unit tests for the core algorithms.
 * Run via `ctest` after building, or execute the binary directly.
 */
#include "hermes.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static void test_lms_reduces_noise(void) {
    lms_filter_t f;
    lms_filter_init(&f, 0.02f);

    float voice[AUDIO_FRAME_SAMPLES], noise[AUDIO_FRAME_SAMPLES], out[AUDIO_FRAME_SAMPLES];
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        float t = (float)i / AUDIO_FRAME_SAMPLES;
        noise[i] = sinf(2.0f * (float)M_PI * 20.0f * t);
        voice[i] = 0.5f * sinf(2.0f * (float)M_PI * 3.0f * t) + 0.8f * noise[i];
    }

    float mse_first = lms_filter_process(&f, voice, noise, out);
    float mse_last = 0;
    /* Run several frames so the adaptive filter has time to converge. */
    for (int iter = 0; iter < 50; iter++)
        mse_last = lms_filter_process(&f, voice, noise, out);

    CHECK(mse_last < mse_first, "LMS filter reduces error/noise energy over time");
}

static uint32_t dummy_checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = sum * 31 + p[i];
    return sum;
}

static void test_tmr_corrects_single_fault(void) {
    const char *payload = "test payload";
    size_t len = strlen(payload);

    uint32_t truth = dummy_checksum(payload, len);
    bool mismatch = false;
    uint32_t voted = tmr_vote_run(dummy_checksum, payload, len, true, &mismatch);

    CHECK(voted == truth, "TMR voter returns correct result despite injected fault");
    CHECK(mismatch == true, "TMR voter reports mismatch telemetry when a fault occurs");
}

static void test_crypto_roundtrip(void) {
    uint8_t key[HERMES_KEY_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const char *msg = "authenticated audio frame payload";
    size_t msg_len = strlen(msg);

    uint8_t packet[256];
    size_t packet_len = hermes_encrypt(key, /*nonce_counter=*/42,
                                        (const uint8_t *)msg, msg_len,
                                        packet, sizeof(packet));
    CHECK(packet_len > 0, "encryption succeeds");

    uint8_t recovered[256] = {0};
    size_t recovered_len = hermes_decrypt(key, packet, packet_len,
                                           recovered, sizeof(recovered));
    CHECK(recovered_len == msg_len &&
          memcmp(recovered, msg, msg_len) == 0,
          "decryption recovers the original plaintext");
}

static void test_crypto_detects_tampering(void) {
    uint8_t key[HERMES_KEY_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const char *msg = "do not tamper with me";
    size_t msg_len = strlen(msg);

    uint8_t packet[256];
    size_t packet_len = hermes_encrypt(key, 7, (const uint8_t *)msg, msg_len,
                                        packet, sizeof(packet));

    /* Flip one bit in the ciphertext. */
    packet[HERMES_IV_SIZE] ^= 0x01;

    uint8_t recovered[256];
    size_t recovered_len = hermes_decrypt(key, packet, packet_len,
                                           recovered, sizeof(recovered));

    CHECK(recovered_len == 0, "tampered packet is rejected by the MAC check");
}

int main(void) {
    test_lms_reduces_noise();
    test_tmr_corrects_single_fault();
    test_crypto_roundtrip();
    test_crypto_detects_tampering();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
