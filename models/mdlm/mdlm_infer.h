/* The masked diffusion language model in C (mdlm_infer.c): the model slice
 * of models/lm/mdlm.py (llm-pipeline's blob with EMBED, HEAD, TIED and
 * BIDIR) and its sampler, computing the bits of mdlm.py's reference with
 * the node's linear algebra imports. */
#ifndef MDLM_INFER_H
#define MDLM_INFER_H

#include <stdint.h>

#define MDLM_WINDOW 64
#ifndef MDLM_MAX_DIM
#define MDLM_MAX_DIM 256       /* the buffers' sizes: -DMDLM_MAX_DIM=128 -DMDLM_MAX_HIDDEN=320 fit mdlm.bin */
#endif
#ifndef MDLM_MAX_HIDDEN
#define MDLM_MAX_HIDDEN 1024
#endif
#define MDLM_MAX_LAYERS 8
#define MDLM_MAX_VOCAB 1024

struct mdlm_layer {
    const float *rms_att, *wq_s, *wk_s, *wv_s, *wo_s, *rms_ffn, *w1_s, *w2_s, *w3_s;
    const int8_t *wq, *wk, *wv, *wo, *w1, *w2, *w3;
};

struct mdlm_model {
    uint32_t dim, hidden, n_heads, n_kv_heads, vocab, window, n_layers;
    const int8_t* tok_q;     /* [vocab][dim], also the classifier */
    const float *tok_s, *rope, *rms_final;
    struct mdlm_layer layer[MDLM_MAX_LAYERS];
};

int mdlm_load(struct mdlm_model* m, const uint8_t* blob, uint32_t len);

/* Fills window[0..window) from prompt (BOS first) in `steps` steps.
 * temperature 0: greedy; otherwise sampled with the xorshift32 stream of
 * seed. Returns 0, or -1 for arguments the model cannot take. */
int mdlm_sample(const struct mdlm_model* m, const uint32_t* prompt, uint32_t prompt_len, uint32_t steps, uint32_t seed,
                float temperature, uint32_t* window);

#endif
