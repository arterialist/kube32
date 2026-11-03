/* Checks frontend.c against the reference's features (kws.py vectors).
 *   cc -O2 -o frontend_test frontend_test.c frontend.c
 *   ./frontend_test kws.bin vectors/kws-audio.bin vectors/kws-features.bin */
#include "frontend.h"

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

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s kws.bin audio.bin features.bin\n", argv[0]);
        return 2;
    }
    long nb, na, nf;
    uint8_t* blob = slurp(argv[1], &nb);
    int16_t* audio = slurp(argv[2], &na);
    int32_t* want = slurp(argv[3], &nf);
    const long clips = na / (2 * KWS_RATE);
    if (nf != clips * 4 * KWS_FRAMES * KWS_MELS) {
        fprintf(stderr, "features: %ld bytes for %ld clips\n", nf, clips);
        return 2;
    }
    struct kws_tables t;
    kws_tables_from_blob(&t, blob);
    int32_t got[KWS_FRAMES * KWS_MELS];
    for (long c = 0; c < clips; c++) {
        kws_features(&t, audio + c * KWS_RATE, got);
        if (memcmp(got, want + c * KWS_FRAMES * KWS_MELS, sizeof got) != 0) {
            for (int i = 0; i < KWS_FRAMES * KWS_MELS; i++)
                if (got[i] != want[c * KWS_FRAMES * KWS_MELS + i]) {
                    fprintf(stderr, "clip %ld: frame %d mel %d: %d, reference %d\n", c, i / KWS_MELS, i % KWS_MELS,
                            got[i], want[c * KWS_FRAMES * KWS_MELS + i]);
                    break;
                }
            return 1;
        }
    }
    printf("%ld clips: features equal the reference's\n", clips);
    return 0;
}
