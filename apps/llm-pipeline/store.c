/* llm-store: serves the files of llm-pipeline's model (the shards' slices
 * and the tokenizer) from its image bundle, the directory
 * kube32/images/llm-store/ next to its module on the node's storage.
 *
 *   GET /<file>   the file (k32_resp_asset streams it without copying it
 *                 into linear memory); 404 for a name that is not there */
#include "llm.h"

static unsigned char g_block[1024];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    const char* path;
    uint32_t body;
    uint32_t n = llm_request(req, len, &path, &body);
    /* the last path component: /llm-store/shard-1.bin and /shard-1.bin alike */
    uint32_t s = n;
    while (s && path[s - 1] != '/') s--;
    const char* name = path + s;
    n -= s;
    int ok = n > 0 && name[0] != '.';
    for (uint32_t i = 0; i < n && ok; i++) {
        const char c = name[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    }
    if (ok && k32_resp_asset(name, n, 0) == 0) return 200;
    k32_resp_begin(404, "application/json", 16);
    k32_resp_write("{\"error\":\"not_found\"}", 21);
    k32_resp_end();
    return 404;
}
