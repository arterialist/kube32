/* Reading a model blob into a pod: the small arrays (tables, scales,
 * biases) are copied into an arena in linear memory, the weight matrices
 * stay in the blob and the linear algebra imports read them in place
 * through K32_BLOB_ADDR. The walk follows the blob's layout the way the
 * model's own loader does: every array starts on a 4-byte boundary. */
#ifndef WORDSKETCH_BLOBLOAD_H
#define WORDSKETCH_BLOBLOAD_H

#include "../../include/runtime/k32_app_abi.h"

struct blobload {
    int32_t blob;
    uint32_t off, size;     /* the next array's offset, the blob's size */
    uint8_t* arena;
    uint32_t used, cap;
    int failed;
};

/* size: the blob's bytes, as its header states them */
static void blobload_begin(struct blobload* r, int32_t blob, uint32_t size, uint8_t* arena, uint32_t cap) {
    r->blob = blob;
    r->off = 0;
    r->size = size;
    r->arena = arena;
    r->used = 0;
    r->cap = cap;
    r->failed = 0;
}

/* n bytes copied into the arena */
static const void* blobload_copy(struct blobload* r, uint32_t n) {
    n = (n + 3u) & ~3u;
    if (r->failed || r->used + n > r->cap || r->off + n > r->size) {
        r->failed = 1;
        return 0;
    }
    void* p = r->arena + r->used;
    if (k32_blob_read(r->blob, r->off, p, n) != (int32_t)n) {
        r->failed = 1;
        return 0;
    }
    r->off += n;
    r->used += n;
    return p;
}

/* n bytes left in the blob: their blob address */
static const void* blobload_map(struct blobload* r, uint32_t n) {
    n = (n + 3u) & ~3u;
    if (r->failed || r->off + n > r->size) {
        r->failed = 1;
        return 0;
    }
    const void* p = K32_BLOB_ADDR(r->blob, r->off);
    r->off += n;
    return p;
}

/* 0 when every array was there and nothing is left over */
static int blobload_end(const struct blobload* r) { return r->failed || r->off != r->size ? -1 : 0; }

#endif
