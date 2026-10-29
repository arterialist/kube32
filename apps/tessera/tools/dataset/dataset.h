#ifndef TESSERA_DATASET_H
#define TESSERA_DATASET_H
#include <stdint.h>

typedef struct {
    uint32_t issues, comments, updates, records;
    uint32_t bytes, segments, last_seq;
} dataset_stats;

/* Generates the sample data (seed.c) and writes it as tessera-db records into
 * the volume the k32_fs_* calls reach (host_test shim: shim_fs.root), as a
 * history in time order: each issue is created in the backlog, moves to its
 * final status later, and comments arrive at their own times. The volume
 * must not hold a log yet. Returns 0 or a negative error. */
int dataset_build(uint32_t n_issues, uint32_t n_comments, int64_t epoch_ms, dataset_stats* st);
#endif
