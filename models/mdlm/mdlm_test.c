/* Runs mdlm_infer.c's sampler for the test vectors (mdlm.py vectors): every
 * window must be the reference's tokens.
 *   ./mdlm_test mdlm.bin vectors/mdlm-samples.bin
 * The samples file: records of uint32 steps, seed, prompt_len, float
 * temperature, uint32 prompt[64], uint32 window[64]. */
#include "mdlm_infer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* p = malloc((size_t)*n);
    if (fread(p, 1, (size_t)*n, f) != (size_t)*n) { perror(path); exit(2); }
    fclose(f);
    return p;
}

struct record {
    uint32_t steps, seed, prompt_len;
    float temperature;
    uint32_t prompt[MDLM_WINDOW], window[MDLM_WINDOW];
};

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s mdlm.bin samples.bin\n", argv[0]);
        return 2;
    }
    long nb, ns;
    uint8_t* blob = slurp(argv[1], &nb);
    struct record* r = slurp(argv[2], &ns);
    static struct mdlm_model m;
    if (mdlm_load(&m, blob, (uint32_t)nb) != 0) {
        fprintf(stderr, "%s: not a masked diffusion blob this build runs\n", argv[1]);
        return 1;
    }
    const long n = ns / (long)sizeof *r;
    uint32_t x[MDLM_WINDOW];
    for (long i = 0; i < n; i++) {
        if (mdlm_sample(&m, r[i].prompt, r[i].prompt_len, r[i].steps, r[i].seed, r[i].temperature, x) != 0) {
            fprintf(stderr, "sample %ld: bad arguments\n", i);
            return 1;
        }
        if (memcmp(x, r[i].window, sizeof x) != 0) {
            for (int p = 0; p < MDLM_WINDOW; p++)
                if (x[p] != r[i].window[p]) {
                    fprintf(stderr, "sample %ld (steps %u seed %u): position %d token %u, reference %u\n", i, r[i].steps,
                            r[i].seed, p, x[p], r[i].window[p]);
                    break;
                }
            return 1;
        }
    }
    printf("%s: %ld samples equal the reference's\n", argv[1], n);
    return 0;
}
