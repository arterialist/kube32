/* The keyword spotter in C (kws_infer.c): the model blob of kws.py, the
 * front end of frontend.c, and the network with the node's linear algebra
 * imports, computing the bits of kws.py's reference. */
#ifndef KWS_INFER_H
#define KWS_INFER_H

#include <stdint.h>

#include "frontend.h"

#define KWS_MAX_CONV 6
#define KWS_CLASSES  8
#define KWS_MAP_MAX  6144    /* int8 bytes of the largest map but the input */

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

#endif
