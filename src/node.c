/*
 * node.c - Ties audio, crypto, TMR, and mesh together into a single
 * "communication node" with an explicit state machine:
 *
 *   IDLE -> CAPTURING -> ENCRYPTING -> TRANSMITTING -> IDLE   (send path)
 *   IDLE -> RECEIVING -> DECRYPTING -> PLAYBACK -> IDLE        (recv path)
 *   any state -> ERROR on unrecoverable failure
 *
 * Modeling this explicitly (rather than one big function) mirrors how
 * a real embedded system would structure this as a task with clear
 * states, which matters for debuggability and for eventually porting
 * to an RTOS task.
 */
#include "hermes.h"
#include "aes.h"
#include <string.h>
#include <stdio.h>

bool hermes_node_init(hermes_node_t *node, const char *name,
                       uint16_t local_port, const uint8_t key[HERMES_KEY_SIZE]) {
    memset(node, 0, sizeof(*node));
    node->name = name;
    node->tx_counter = 0;
    node->state = NODE_STATE_IDLE;

    memcpy(node->key, key, HERMES_KEY_SIZE);
    lms_filter_init(&node->noise_filter, 0.01f);

    if (!mesh_node_init(&node->mesh, local_port)) {
        node->state = NODE_STATE_ERROR;
        return false;
    }
    return true;
}

bool hermes_node_transmit_frame(hermes_node_t *node,
                                 const float *voice, const float *noise) {
    node->state = NODE_STATE_CAPTURING;
    audio_frame_t clean;
    lms_filter_process(&node->noise_filter, voice, noise, clean.samples);

    node->state = NODE_STATE_ENCRYPTING;
    uint8_t packet[sizeof(audio_frame_t) + HERMES_IV_SIZE + HERMES_MAC_SIZE + 16];
    size_t packet_len = hermes_encrypt(node->key, node->tx_counter,
                                        (const uint8_t *)&clean, sizeof(clean),
                                        packet, sizeof(packet));
    if (packet_len == 0) {
        node->state = NODE_STATE_ERROR;
        fprintf(stderr, "[%s] encryption failed\n", node->name);
        return false;
    }
    node->tx_counter++; /* never reuse an IV with this key */

    node->state = NODE_STATE_TRANSMITTING;
    if (!mesh_node_send(&node->mesh, packet, packet_len)) {
        node->state = NODE_STATE_ERROR;
        fprintf(stderr, "[%s] mesh send failed\n", node->name);
        return false;
    }

    node->state = NODE_STATE_IDLE;
    return true;
}

bool hermes_node_receive_frame(hermes_node_t *node, int timeout_ms,
                                audio_frame_t *out_frame) {
    node->state = NODE_STATE_RECEIVING;
    uint8_t packet[MESH_MAX_PACKET];
    int n = mesh_node_recv(&node->mesh, packet, sizeof(packet), timeout_ms);
    if (n <= 0) {
        node->state = NODE_STATE_IDLE;
        return false; /* timeout or error, not necessarily fatal */
    }

    node->state = NODE_STATE_DECRYPTING;
    /* The decrypt scratch buffer must be large enough for the full
     * padded plaintext (up to one extra 16-byte block from PKCS#7),
     * even though the final unpadded result should equal exactly
     * sizeof(audio_frame_t). */
    uint8_t scratch[sizeof(audio_frame_t) + AES_BLOCK_SIZE];
    size_t plain_len = hermes_decrypt(node->key, packet, (size_t)n,
                                       scratch, sizeof(scratch));
    if (plain_len != sizeof(audio_frame_t)) {
        fprintf(stderr, "[%s] dropped packet: auth/decrypt failed\n", node->name);
        node->state = NODE_STATE_IDLE;
        return false;
    }
    memcpy(out_frame, scratch, sizeof(audio_frame_t));

    node->state = NODE_STATE_PLAYBACK;
    node->state = NODE_STATE_IDLE;
    return true;
}

void hermes_node_close(hermes_node_t *node) {
    mesh_node_close(&node->mesh);
}
