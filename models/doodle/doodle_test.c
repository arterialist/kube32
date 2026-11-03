/* Runs doodle_infer.c's sampler for the test vectors (doodle.py vectors):
 * every image must be the reference's bits.
 *   ./doodle_test doodle-s.bin vectors/doodle-s-samples.bin
 * The samples file: records of uint32 class, seed, steps, float guidance,
 * float image[784]. */
#include "doodle_infer.h"

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
    uint32_t cls, seed, steps;
    float guidance;
    float image[784];
};

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s doodle.bin samples.bin\n", argv[0]);
        return 2;
    }
    long nb, ns;
    uint8_t* blob = slurp(argv[1], &nb);
    struct record* r = slurp(argv[2], &ns);
    static struct doodle_model m;
    if (doodle_load(&m, blob, (uint32_t)nb) != 0) {
        fprintf(stderr, "%s: not a doodle blob this build runs\n", argv[1]);
        return 1;
    }
    const long n = ns / (long)sizeof *r;
    float x[784];
    for (long i = 0; i < n; i++) {
        if (doodle_sample(&m, r[i].cls, r[i].seed, r[i].steps, r[i].guidance, x) != 0) {
            fprintf(stderr, "sample %ld: no schedule of %u steps\n", i, r[i].steps);
            return 1;
        }
        if (memcmp(x, r[i].image, sizeof x) != 0) {
            for (int p = 0; p < 784; p++)
                if (x[p] != r[i].image[p] || memcmp(&x[p], &r[i].image[p], 4)) {
                    fprintf(stderr, "sample %ld (class %u seed %u): pixel %d %.9g, reference %.9g\n", i, r[i].cls, r[i].seed, p,
                            x[p], r[i].image[p]);
                    break;
                }
            return 1;
        }
    }
    printf("%s: %ld samples equal the reference's\n", argv[1], n);
    return 0;
}
