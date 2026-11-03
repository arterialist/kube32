/* The doodle denoiser in C (doodle_infer.c): the blob of doodle.py and its
 * DDIM sampler with the node's convolution and element-wise imports,
 * computing the bits of doodle.py's reference. DOODLE_C0_MAX and
 * DOODLE_C1_MAX size the maps (default: the larger model's 48 and 96). */
#ifndef DOODLE_INFER_H
#define DOODLE_INFER_H

#include <stdint.h>

#ifndef DOODLE_C0_MAX
#define DOODLE_C0_MAX 48
#endif
#ifndef DOODLE_C1_MAX
#define DOODLE_C1_MAX 96
#endif
#define DOODLE_MAX_CONV 10
#define DOODLE_NONE 6          /* the "no class" condition */

struct doodle_conv {
    uint32_t cin, cout, stride, flags;
    float scale, inv;
    const float *ws, *b;
    const int8_t* w;            /* [cout][3][3][cin] */
};

struct doodle_sched {
    uint32_t steps;
    const float *alpha, *sigma; /* [steps + 1] */
    const int8_t* tq;           /* [steps][width] */
    const float* ts;            /* [steps] */
};

struct doodle_model {
    uint32_t c0, c1, mids, n_conv, n_cond, n_sched, width;
    struct doodle_conv conv[DOODLE_MAX_CONV];
    const int8_t* class_q;      /* [n_cond][width] */
    const float* class_s;
    struct doodle_sched sched[4];
};

int doodle_load(struct doodle_model* m, const uint8_t* blob, uint32_t len);

/* One image: 784 floats in [-1, 1], row-major. Returns 0, or -1 for a
 * schedule the blob does not have. */
/* When set, doodle_sample calls it with the starting noise (step 0) and
 * with the image after each step (1 .. steps), so a pod can show the
 * drawing as it forms. */
extern void (*doodle_on_step)(uint32_t step, uint32_t steps, const float* x);

/* doodle_sample a step at a time: doodle_begin draws the starting noise,
 * doodle_step(i) takes the image from step i to i + 1 (i < steps). */
int doodle_begin(const struct doodle_model* m, uint32_t cls, uint32_t seed, uint32_t steps, float x[784]);
int doodle_step(const struct doodle_model* m, uint32_t cls, uint32_t steps, uint32_t i, float guidance, float x[784]);

int doodle_sample(const struct doodle_model* m, uint32_t cls, uint32_t seed, uint32_t steps, float guidance, float x[784]);

#endif
