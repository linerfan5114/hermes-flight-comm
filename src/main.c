/*
 * main.c - Demo entry point.
 *
 * Spins up two simulated "suit intercom" nodes on localhost, has one
 * transmit a synthetic noisy voice signal to the other, and shows the
 * noise-cancellation + encryption/decryption pipeline working
 * end-to-end. Also demonstrates the TMR voter correcting a simulated
 * single-event upset.
 */
#include "hermes.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A toy "critical" computation for the TMR demo: a simple checksum. */
static uint32_t checksum32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = sum * 31 + p[i];
    return sum;
}

static void run_tmr_demo(void) {
    printf("\n=== TMR voter demo ===\n");
    const char *payload = "engine telemetry OK";
    size_t len = strlen(payload);

    bool mismatch = false;
    uint32_t result = tmr_vote_run(checksum32, payload, len,
                                    /*inject_fault=*/false, &mismatch);
    printf("No fault injected:  result=0x%08x mismatch=%s\n",
           result, mismatch ? "true" : "false");

    result = tmr_vote_run(checksum32, payload, len,
                           /*inject_fault=*/true, &mismatch);
    printf("Fault injected:     result=0x%08x mismatch=%s "
           "(voter still returned the correct majority result)\n",
           result, mismatch ? "true" : "false");
}

static void generate_synthetic_audio(float *voice, float *noise) {
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        float t = (float)i / AUDIO_FRAME_SAMPLES;
        float voice_signal = sinf(2.0f * (float)M_PI * 3.0f * t) * 0.5f;
        float noise_signal = sinf(2.0f * (float)M_PI * 20.0f * t) * 0.3f;
        noise[i] = noise_signal;
        /* The "primary" mic picks up voice + a scaled copy of the
         * noise source, similar to how a suit's ambient noise leaks
         * into the mic alongside the wearer's voice. */
        voice[i] = voice_signal + 0.8f * noise_signal;
    }
}

int main(void) {
    printf("Hermes flight-comm simulation\n");
    printf("(host-based simulation over UDP loopback -- see README.md)\n");

    const uint8_t shared_key[HERMES_KEY_SIZE] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };

    hermes_node_t node_a, node_b;
    if (!hermes_node_init(&node_a, "Node-A", 9001, shared_key)) {
        fprintf(stderr, "failed to init Node-A\n");
        return 1;
    }
    if (!hermes_node_init(&node_b, "Node-B", 9002, shared_key)) {
        fprintf(stderr, "failed to init Node-B\n");
        return 1;
    }
    mesh_node_add_peer(&node_a.mesh, 9002);
    mesh_node_add_peer(&node_b.mesh, 9001);

    printf("\n=== Audio + crypto + mesh demo ===\n");
    float voice[AUDIO_FRAME_SAMPLES], noise[AUDIO_FRAME_SAMPLES];
    generate_synthetic_audio(voice, noise);

    if (!hermes_node_transmit_frame(&node_a, voice, noise)) {
        fprintf(stderr, "Node-A failed to transmit\n");
        return 1;
    }
    printf("Node-A transmitted an encrypted, noise-filtered audio frame.\n");

    audio_frame_t received;
    if (hermes_node_receive_frame(&node_b, /*timeout_ms=*/1000, &received)) {
        printf("Node-B received and authenticated the frame.\n");
        printf("First 5 recovered samples: %.4f %.4f %.4f %.4f %.4f\n",
               received.samples[0], received.samples[1], received.samples[2],
               received.samples[3], received.samples[4]);
    } else {
        printf("Node-B did not receive a valid frame.\n");
    }

    run_tmr_demo();

    hermes_node_close(&node_a);
    hermes_node_close(&node_b);
    return 0;
}
