/*
 * dataset: writes Tessera's sample data in tessera-db's on-card format.
 *
 *   dataset [--issues N] [--comments N] [--epoch MS] [--out DIR]
 *
 * DIR (default build/card/kube32/volumes/tessera-db) receives the segment
 * files; copy the kube32/ tree to the root of the microSD card so they land
 * in /kube32/volumes/tessera-db/, the volume of the claim tessera-db.
 */
#define _POSIX_C_SOURCE 200809L
#include "dataset.h"
#include "../../common/host/shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

int main(int argc, char** argv) {
    unsigned long issues = 2400, comments = 4800;
    long long epoch = 1793059200000ll;
    const char* out = "build/card/kube32/volumes/tessera-db";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--issues") && v) { issues = strtoul(v, 0, 10); i++; }
        else if (!strcmp(a, "--comments") && v) { comments = strtoul(v, 0, 10); i++; }
        else if (!strcmp(a, "--epoch") && v) { epoch = strtoll(v, 0, 10); i++; }
        else if (!strcmp(a, "--out") && v) { out = v; i++; }
        else { fprintf(stderr, "usage: %s [--issues N] [--comments N] [--epoch MS] [--out DIR]\n", argv[0]); return 2; }
    }
    if (issues < 1 || issues > 60000 || comments > 60000) { fprintf(stderr, "issues 1..60000, comments 0..60000\n"); return 2; }
    if (shim_fs_reset_dir(out) != 0) { fprintf(stderr, "cannot create %s\n", out); return 1; }
    snprintf(shim_fs.root, sizeof shim_fs.root, "%s", out);
    shim.quiet = 1;
    dataset_stats st;
    double t0 = now_ms();
    int rc = dataset_build((uint32_t)issues, (uint32_t)comments, epoch, &st);
    double dt = now_ms() - t0;
    if (rc != 0) { fprintf(stderr, "dataset: failed (%d)\n", rc); return 1; }
    printf("dataset: %u issues, %u comments, %u status changes -> %u records\n", st.issues, st.comments, st.updates, st.records);
    printf("dataset: %u bytes in %u segments in %s (%.0f ms)\n", st.bytes, st.segments, out, dt);
    return 0;
}
