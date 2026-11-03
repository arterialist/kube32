/* The keyword spotter's front end (frontend.py) in C: integer operations
 * only, so a pod computes the features the reference computes.
 *
 * kws_frame() turns 480 samples into 40 log-mel values; a second of audio
 * is 49 frames 320 samples apart. The tables come from the model blob
 * (kws.py, frontend.h). */
#include "frontend.h"

static uint32_t bitrev9(uint32_t i) {
    uint32_t r = 0;
    for (int b = 0; b < 9; b++) r |= ((i >> b) & 1u) << (8 - b);
    return r;
}

/* floor(log2 e) in Q8 with the 8 bits after the leading one as fraction */
static int32_t log2_q8(int64_t e) {
    if (e <= 0) return 0;
    int32_t n = 63 - __builtin_clzll((uint64_t)e);
    int64_t frac = n >= 8 ? (e >> (n - 8)) & 255 : (e << (8 - n)) & 255;
    return 256 * n + (int32_t)frac;
}

void kws_frame(const struct kws_tables* t, const int16_t* s, int32_t out[KWS_MELS]) {
    int32_t re[KWS_NFFT], im[KWS_NFFT];
    for (uint32_t i = 0; i < KWS_NFFT; i++) {
        const uint32_t j = bitrev9(i);
        re[i] = j < KWS_WIN ? (int32_t)(((int64_t)s[j] * t->hann[j] + 16384) >> 15) : 0;
        im[i] = 0;
    }
    for (uint32_t size = 2; size <= KWS_NFFT; size *= 2) {
        const uint32_t half = size / 2, step = KWS_NFFT / size;
        for (uint32_t start = 0; start < KWS_NFFT; start += size) {
            for (uint32_t j = 0; j < half; j++) {
                const int64_t wr = t->tw_re[j * step], wi = t->tw_im[j * step];
                const uint32_t a = start + j, b = a + half;
                const int32_t tr = (int32_t)(((int64_t)re[b] * wr - (int64_t)im[b] * wi + 16384) >> 15);
                const int32_t ti = (int32_t)(((int64_t)re[b] * wi + (int64_t)im[b] * wr + 16384) >> 15);
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] = re[a] + tr;
                im[a] = im[a] + ti;
            }
        }
    }
    int64_t p[KWS_BINS];
    for (uint32_t k = 0; k < KWS_BINS; k++) p[k] = ((int64_t)re[k] * re[k] + (int64_t)im[k] * im[k]) >> 8;
    const int16_t* w = t->mel_w;
    for (uint32_t m = 0; m < KWS_MELS; m++) {
        int64_t e = 0;
        for (uint32_t k = 0; k < t->mel_len[m]; k++) e += p[t->mel_start[m] + k] * w[k];
        w += t->mel_len[m];
        out[m] = log2_q8(e);
    }
}

void kws_features(const struct kws_tables* t, const int16_t* audio, int32_t* out) {
    for (uint32_t f = 0; f < KWS_FRAMES; f++) kws_frame(t, audio + KWS_HOP * f, out + KWS_MELS * f);
}
