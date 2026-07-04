/*
 * hermes.h - Core declarations for the Hermes flight-comm simulation.
 *
 * IMPORTANT (read before assuming this flies on real hardware):
 * This project is a HOST-BASED SOFTWARE SIMULATION of concepts used
 * in redundant, secure spacecraft/EVA-suit communication systems.
 * It runs as a normal process on Linux/macOS/Windows using UDP
 * sockets to simulate a wireless mesh, and pthreads to simulate
 * concurrent flight-computer tasks.
 *
 * It is NOT flight-qualified software, is NOT MISRA-C certified,
 * and has NOT been tested on radiation-tolerant hardware. Porting
 * this to real embedded hardware (e.g. ESP32) would require
 * replacing the UDP mesh layer with a real radio driver (e.g.
 * ESP-NOW) and the pthread tasks with an RTOS task model.
 */
#ifndef HERMES_H
#define HERMES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------- Audio pipeline ---------- */

#define AUDIO_FRAME_SAMPLES 160   /* e.g. 20ms @ 8kHz */
#define LMS_FILTER_TAPS     32

typedef struct {
    float weights[LMS_FILTER_TAPS];
    float history[LMS_FILTER_TAPS];
    float mu; /* adaptation step size */
} lms_filter_t;

void lms_filter_init(lms_filter_t *f, float mu);

/*
 * Removes a known reference noise signal from a primary (voice+noise)
 * signal using an adaptive LMS filter. Both buffers have
 * AUDIO_FRAME_SAMPLES samples. `out` receives the cleaned signal.
 * Returns the mean squared error of the frame (useful for tests/telemetry).
 */
float lms_filter_process(lms_filter_t *f,
                          const float *primary,
                          const float *reference,
                          float *out);

/* Simple frame container used across the pipeline. */
typedef struct {
    float samples[AUDIO_FRAME_SAMPLES];
} audio_frame_t;

/* ---------- TMR (Triple Modular Redundancy) ---------- */

/*
 * A "critical" computation to be triplicated. In this simulation we
 * use it to protect the integrity check of an outgoing audio frame,
 * but it's generic: any function with this signature can be voted on.
 */
typedef uint32_t (*tmr_compute_fn)(const void *input, size_t len);

/*
 * Runs `fn` three times against copies of `input` (with an optional
 * injected bit-flip in one copy, for fault-injection testing) and
 * returns the majority-voted result. `mismatch_detected` is set to
 * true if the three results were not unanimous (i.e. the voter had
 * to break a tie), which is useful telemetry even when correction
 * succeeds.
 */
uint32_t tmr_vote_run(tmr_compute_fn fn, const void *input, size_t len,
                       bool inject_fault, bool *mismatch_detected);

/* ---------- Crypto (Encrypt-then-MAC) ---------- */

#define HERMES_KEY_SIZE   16 /* AES-128 key */
#define HERMES_IV_SIZE    16
#define HERMES_MAC_SIZE   32 /* HMAC-SHA256 tag */

/*
 * Encrypts `in` (in_len bytes) with AES-128-CBC using a random-ish IV
 * derived from `nonce_counter`, then appends an HMAC-SHA256 tag over
 * (iv || ciphertext). Output layout: [iv(16)][ciphertext][tag(32)].
 * Returns total output length, or 0 on error (e.g. out_cap too small).
 */
size_t hermes_encrypt(const uint8_t key[HERMES_KEY_SIZE],
                       uint32_t nonce_counter,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap);

/*
 * Verifies the HMAC tag (constant-time compare) and decrypts.
 * Returns plaintext length, or 0 if authentication/decryption failed.
 */
size_t hermes_decrypt(const uint8_t key[HERMES_KEY_SIZE],
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap);

/* ---------- Mesh networking (simulated over UDP) ---------- */

#define MESH_MAX_PEERS   8
#define MESH_MAX_PACKET  1024

typedef struct {
    int         sockfd;
    uint16_t    local_port;
    uint16_t    peer_ports[MESH_MAX_PEERS];
    int         peer_count;
} mesh_node_t;

bool mesh_node_init(mesh_node_t *node, uint16_t local_port);
bool mesh_node_add_peer(mesh_node_t *node, uint16_t peer_port);
bool mesh_node_send(mesh_node_t *node, const uint8_t *data, size_t len);

/*
 * Blocking receive with a timeout. Returns bytes received, 0 on
 * timeout, or -1 on error.
 */
int mesh_node_recv(mesh_node_t *node, uint8_t *buf, size_t buf_cap,
                    int timeout_ms);

void mesh_node_close(mesh_node_t *node);

/* ---------- Node state machine ---------- */

typedef enum {
    NODE_STATE_IDLE = 0,
    NODE_STATE_CAPTURING,
    NODE_STATE_ENCRYPTING,
    NODE_STATE_TRANSMITTING,
    NODE_STATE_RECEIVING,
    NODE_STATE_DECRYPTING,
    NODE_STATE_PLAYBACK,
    NODE_STATE_ERROR
} node_state_t;

typedef struct {
    const char   *name;
    mesh_node_t   mesh;
    lms_filter_t  noise_filter;
    uint8_t       key[HERMES_KEY_SIZE];
    uint32_t      tx_counter;
    node_state_t  state;
} hermes_node_t;

bool hermes_node_init(hermes_node_t *node, const char *name,
                       uint16_t local_port, const uint8_t key[HERMES_KEY_SIZE]);

/*
 * Captures a synthetic audio frame (voice + noise), filters the
 * noise out, encrypts it, and transmits it over the mesh to all
 * known peers. Returns false on any pipeline failure.
 */
bool hermes_node_transmit_frame(hermes_node_t *node,
                                 const float *voice, const float *noise);

/*
 * Waits (with timeout) for an incoming packet, decrypts and
 * authenticates it, and writes the recovered audio into `out_frame`.
 * Returns true if a valid frame was received and decoded.
 */
bool hermes_node_receive_frame(hermes_node_t *node, int timeout_ms,
                                audio_frame_t *out_frame);

void hermes_node_close(hermes_node_t *node);

#endif /* HERMES_H */
