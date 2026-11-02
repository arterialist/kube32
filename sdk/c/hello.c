/* hello: the smallest useful request pod in C. Answers every request with a
 * counter and the request line. */
#include "runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static unsigned char request_buf[K32_MAX_REQUEST_BYTES + 512];
static uint32_t hits;

/* The host copies each request block here before calling k32_handle. */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof request_buf ? (uint32_t)(uintptr_t)request_buf : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    k32_log(LIT("hello pod started"));
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;   /* "GET /path?query\n..." */
    uint32_t line = 0;
    while (line < len && req[line] != '\n') line++;

    char num[10], tmp[10];
    uint32_t v = ++hits, n = 0, t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) num[n++] = tmp[--t];

    k32_resp_begin(200, LIT("text/plain"));
    k32_resp_write(LIT("hello from C, request "));
    k32_resp_write(num, n);
    k32_resp_write(LIT(": "));
    k32_resp_write(req, line);
    k32_resp_write(LIT("\n"));
    k32_resp_end();
    return 200;
}
