/*
 * audio.c - Audio pipeline + adaptive LMS noise-cancellation filter.
 *
 * The LMS filter here is a real, working adaptive filter (Widrow-Hoff
 * least-mean-squares algorithm): it learns, sample by sample, how the
 * reference noise signal maps onto the noise component mixed into the
 * primary signal, and subtracts its prediction. This is the same
 * basic technique used in headset noise-cancellation and echo
 * cancellation, simplified for a single reference channel.
 */
#include "hermes.h"
#include <string.h>
#include <math.h>

void lms_filter_init(lms_filter_t *f, float mu) {
    memset(f->weights, 0, sizeof(f->weights));
    memset(f->history, 0, sizeof(f->history));
    f->mu = mu;
}

float lms_filter_process(lms_filter_t *f,
                          const float *primary,
                          const float *reference,
                          float *out) {
    double sq_err_sum = 0.0;

    for (int n = 0; n < AUDIO_FRAME_SAMPLES; n++) {
        /* Shift the reference sample into the tapped delay line. */
        for (int i = LMS_FILTER_TAPS - 1; i > 0; i--)
            f->history[i] = f->history[i - 1];
        f->history[0] = reference[n];

        /* Predict the noise component from the reference history. */
        float predicted_noise = 0.0f;
        for (int i = 0; i < LMS_FILTER_TAPS; i++)
            predicted_noise += f->weights[i] * f->history[i];

        /* Error = (voice + noise) - predicted_noise ~= voice, once
         * the filter has converged. */
        float error = primary[n] - predicted_noise;
        out[n] = error;

        /* Weight update: classic LMS rule. */
        for (int i = 0; i < LMS_FILTER_TAPS; i++)
            f->weights[i] += f->mu * error * f->history[i];

        sq_err_sum += (double)error * (double)error;
    }

    return (float)(sq_err_sum / AUDIO_FRAME_SAMPLES);
}
