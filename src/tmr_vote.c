/*
 * tmr_vote.c - Triple Modular Redundancy voter.
 *
 * Real flight computers often run the same computation on three
 * independent execution units and vote on the result, so that a
 * single-event upset (e.g. a cosmic-ray-induced bit flip) in one
 * unit doesn't corrupt the output. We simulate that here by running
 * `fn` three times against independent copies of the input buffer,
 * optionally flipping one bit in one copy to simulate a fault, and
 * majority-voting the three results.
 */
#include "hermes.h"
#include <string.h>
#include <stdlib.h>

uint32_t tmr_vote_run(tmr_compute_fn fn, const void *input, size_t len,
                       bool inject_fault, bool *mismatch_detected) {
    /* Three independent copies of the input, standing in for three
     * physically separate memory banks / execution lanes. */
    uint8_t *copy_a = (uint8_t *)malloc(len);
    uint8_t *copy_b = (uint8_t *)malloc(len);
    uint8_t *copy_c = (uint8_t *)malloc(len);

    if (!copy_a || !copy_b || !copy_c) {
        free(copy_a); free(copy_b); free(copy_c);
        if (mismatch_detected) *mismatch_detected = true;
        return 0;
    }

    memcpy(copy_a, input, len);
    memcpy(copy_b, input, len);
    memcpy(copy_c, input, len);

    if (inject_fault && len > 0) {
        /* Simulate a single-event upset: flip one bit in lane B. */
        copy_b[0] ^= 0x01;
    }

    uint32_t result_a = fn(copy_a, len);
    uint32_t result_b = fn(copy_b, len);
    uint32_t result_c = fn(copy_c, len);

    free(copy_a); free(copy_b); free(copy_c);

    bool mismatch = !(result_a == result_b && result_b == result_c);
    if (mismatch_detected) *mismatch_detected = mismatch;

    /* Majority vote: if any two agree, that's the trusted result. */
    if (result_a == result_b) return result_a;
    if (result_a == result_c) return result_a;
    if (result_b == result_c) return result_b;

    /* All three disagree: no safe majority. Fall back to lane A but
     * the caller has already been told via mismatch_detected that
     * this result should not be trusted blindly. */
    return result_a;
}
