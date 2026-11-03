/* The keyword spotter in C (kws_infer.c): the model blob of kws.py, the
 * front end of frontend.c, and the network with the node's linear algebra
 * imports, computing the bits of kws.py's reference. */
#ifndef KWS_INFER_H
#define KWS_INFER_H

#include <stdint.h>

#include "frontend.h"

#define KWS_MAX_CONV 6
#define KWS_CLASSES  8
#ifndef KWS_MAP_MAX
#define KWS_MAP_MAX  6144    /* int8 bytes of the largest map but the input (kws.bin: 5,760) */
#endif
#ifndef KWS_MAP_B_MAX
#define KWS_MAP_B_MAX KWS_MAP_MAX   /* the second convolution's output (kws.bin: 1,920) */
#endif

struct kws_conv {
    uint32_t ci, co, kh, kw, sh, sw, ph, pw;
    float scale, inv;        /* the input's int8 scale and 1 / scale */
    const float *ws, *b;     /* [co] */
    const int8_t* w;         /* [co][kh][kw][ci] */
};

struct kws_model {
    struct kws_tables fe;
    int32_t offset;
    uint32_t n_conv, n_classes;
    struct kws_conv conv[KWS_MAX_CONV];
    uint32_t fc_ci, fc_co;
    const float *fc_ws, *fc_b;
    const int8_t* fc_w;
};

/* 0, or -1 for a blob that is not a keyword spotter this code runs */
int kws_load(struct kws_model* m, const uint8_t* blob, uint32_t len);

/* logits of the 49 x 40 features; returns the class (the first largest) */
int kws_classify(const struct kws_model* m, const int32_t* features, float logits[KWS_CLASSES]);

/* The two halves of kws_classify: n features quantized to the network's
 * int8 input, and the network on 49 x 40 of them. A pod that keeps the
 * last second as int8 frames quantizes each frame once. */
/* The network's first map, free between classifications: room for
 * kws_frame_buf's FFT (2 x 512 int32), or 0 when the map is smaller */
int32_t* kws_scratch(void);

void kws_quantize(const struct kws_model* m, const int32_t* features, uint32_t n, int8_t* out);
int kws_classify_q(const struct kws_model* m, const int8_t* in, float logits[KWS_CLASSES]);

#endif
