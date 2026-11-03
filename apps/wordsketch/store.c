/* wordsketch-store: serves the node's blobs over HTTP, for pods that load a
 * model file at start (llm-pipeline's shard and coordinator).
 *
 *   GET /<name>   the blob <name>, if the pod's kube32.io/blobs lists it,
 *                 streamed 4 KB at a time; 404 otherwise */
#include "../../include/runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static unsigned char g_block[512];
static uint8_t g_buf[4096];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    uint32_t i = 0;
    while (i < len && r[i] != ' ') i++;
    const char* path = r + i + 1;
    uint32_t n = 0;
    while (i + 1 + n < len && path[n] != ' ' && path[n] != '\n' && path[n] != '?') n++;
    uint32_t s = n;   /* the last path component */
    while (s && path[s - 1] != '/') s--;
    const int32_t b = r[0] == 'G' && n > s ? k32_blob_open(path + s, n - s) : K32_ERR_NOENT;
    if (b < 0) {
        k32_resp_begin(404, LIT("application/json"));
        k32_resp_write(LIT("{\"error\":\"not_found\"}"));
        k32_resp_end();
        return 404;
    }
    k32_resp_begin(200, LIT("application/octet-stream"));
    for (uint32_t off = 0;;) {
        const int32_t got = k32_blob_read(b, off, g_buf, sizeof g_buf);
        if (got <= 0) break;
        k32_resp_write(g_buf, (uint32_t)got);
        off += (uint32_t)got;
    }
    k32_resp_end();
    k32_blob_close(b);
    return 200;
}
