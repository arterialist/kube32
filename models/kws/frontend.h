/* The keyword spotter's integer front end (frontend.c, frontend.py) */
#ifndef KWS_FRONTEND_H
#define KWS_FRONTEND_H

#include <stdint.h>

#define KWS_RATE   16000
#define KWS_WIN    480     /* 30 ms */
#define KWS_HOP    320     /* 20 ms */
#define KWS_NFFT   512
#define KWS_BINS   257
#define KWS_MELS   40
#define KWS_FRAMES 49      /* one second */

/* The tables of the model blob, right after its 64-byte header */
struct kws_tables {
    const int16_t* hann;   /* [KWS_WIN], Q15 */
    const int16_t* tw_re;  /* [KWS_NFFT / 2], Q15 cos(2 pi k / 512) */
    const int16_t* tw_im;  /* [KWS_NFFT / 2], Q15 -sin(2 pi k / 512) */
    const uint16_t* mel_start;  /* [KWS_MELS]: each triangle's first bin */
    const uint16_t* mel_len;    /* [KWS_MELS]: its bins */
    const int16_t* mel_w;       /* the triangles' Q10 weights, one after the other */
};

static inline void kws_tables_from_blob(struct kws_tables* t, const uint8_t* blob) {
    const int16_t* p = (const int16_t*)(blob + 64);
    t->hann = p;
    t->tw_re = p + KWS_WIN;
    t->tw_im = t->tw_re + KWS_NFFT / 2;
    t->mel_start = (const uint16_t*)(t->tw_im + KWS_NFFT / 2);
    t->mel_len = t->mel_start + KWS_MELS;
    t->mel_w = (const int16_t*)(t->mel_len + KWS_MELS);
}

/* 40 log-mel values (log2 in Q8) of the 480 samples at s */
void kws_frame(const struct kws_tables* t, const int16_t* s, int32_t out[KWS_MELS]);

/* The same with the FFT's 2 x 512 int32 in buf instead of on the stack */
void kws_frame_buf(const struct kws_tables* t, const int16_t* s, int32_t out[KWS_MELS], int32_t* buf);

/* 49 x 40 values of one second (16000 samples) */
void kws_features(const struct kws_tables* t, const int16_t* audio, int32_t* out);

#endif
