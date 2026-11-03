/* The keyword spotter's front end (frontend.py) in C: integer operations
 * only, so a pod computes the features the reference computes.
 *
 * kws_frame() turns 480 samples into 40 log-mel values; a second of audio
 * is 49 frames 320 samples apart. The tables come from the model blob
 * (kws.py, frontend.h). */
#include "frontend.h"

#include "../../include/runtime/k32_app_abi.h"
#include "../common/k32_host.h"

/* floor(log2 e) in Q8 with the 8 bits after the leading one as fraction */
static int32_t log2_q8(int64_t e) {
    if (e <= 0) return 0;
    int32_t n = 63 - __builtin_clzll((uint64_t)e);
    int64_t frac = n >= 8 ? (e >> (n - 8)) & 255 : (e << (8 - n)) & 255;
    return 256 * n + (int32_t)frac;
}

void kws_frame(const struct kws_tables* t, const int16_t* s, int32_t out[KWS_MELS]) {
    int32_t buf[2 * KWS_NFFT];
    kws_frame_buf(t, s, out, buf);
}

void kws_frame_buf(const struct kws_tables* t, const int16_t* s, int32_t out[KWS_MELS], int32_t* re) {
    int32_t* im = re + KWS_NFFT;
    /* i in bit-reversed order: j counts with its bits mirrored */
    for (uint32_t i = 0, j = 0; i < KWS_NFFT; i++) {
        re[i] = j < KWS_WIN ? (int32_t)(((int64_t)s[j] * t->hann[j] + 16384) >> 15) : 0;
        im[i] = 0;
        uint32_t bit = KWS_NFFT >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
    }
    /* the butterflies natively (k32_fft_q15), with the bits of the loops
     * frontend.py runs */
    k32_fft_q15(re, im, KWS_NFFT, t->tw_re, t->tw_im);
    /* each bin's power where a filter takes it: no array of 257 powers on the stack */
    const int16_t* w = t->mel_w;
    for (uint32_t m = 0; m < KWS_MELS; m++) {
        int64_t e = 0;
        for (uint32_t k = 0; k < t->mel_len[m]; k++) {
            const uint32_t b = t->mel_start[m] + k;
            e += (((int64_t)re[b] * re[b] + (int64_t)im[b] * im[b]) >> 8) * w[k];
        }
        w += t->mel_len[m];
        out[m] = log2_q8(e);
    }
}

void kws_features(const struct kws_tables* t, const int16_t* audio, int32_t* out) {
    for (uint32_t f = 0; f < KWS_FRAMES; f++) kws_frame(t, audio + KWS_HOP * f, out + KWS_MELS * f);
}
