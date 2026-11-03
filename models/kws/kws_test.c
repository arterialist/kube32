/* Runs kws_infer.c on the test vectors (kws.py vectors) from the audio:
 * features and logits must be the reference's bits.
 *   ./kws_test kws.bin vectors/kws-audio.bin vectors/kws-features.bin vectors/kws-logits.bin */
#include "kws_infer.h"

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
    if (argc != 5) {
        fprintf(stderr, "usage: %s kws.bin audio.bin features.bin logits.bin\n", argv[0]);
        return 2;
    }
    long nb, na, nf, nl;
    uint8_t* blob = slurp(argv[1], &nb);
    int16_t* audio = slurp(argv[2], &na);
    int32_t* feats = slurp(argv[3], &nf);
    float* want = slurp(argv[4], &nl);
    struct kws_model m;
    if (kws_load(&m, blob, (uint32_t)nb) != 0) {
        fprintf(stderr, "%s: not a keyword spotter blob\n", argv[1]);
        return 1;
    }
    const long clips = na / (2 * KWS_RATE);
    int32_t f[KWS_FRAMES * KWS_MELS];
    float logits[KWS_CLASSES];
    for (long c = 0; c < clips; c++) {
        kws_features(&m.fe, audio + c * KWS_RATE, f);
        if (memcmp(f, feats + c * KWS_FRAMES * KWS_MELS, sizeof f) != 0) {
            fprintf(stderr, "clip %ld: features differ\n", c);
            return 1;
        }
        const int k = kws_classify(&m, f, logits);
        if (memcmp(logits, want + c * KWS_CLASSES, sizeof logits) != 0) {
            for (int i = 0; i < KWS_CLASSES; i++)
                fprintf(stderr, "clip %ld logit %d: %.9g, reference %.9g\n", c, i, logits[i], want[c * KWS_CLASSES + i]);
            return 1;
        }
        printf("clip %ld: class %d\n", c, k);
    }
    printf("%ld clips: features and logits equal the reference's\n", clips);
    return 0;
}
