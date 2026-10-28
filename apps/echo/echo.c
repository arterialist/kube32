/* Minimal request-handling pod used to test the ingress, proxy and SSE path
 * before the real application is ready. Echoes the request block as JSON and
 * emits an event on every POST. */
#include "../../include/runtime/k32_app_abi.h"

static unsigned char scratch[8192];
static unsigned long requests;

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char buf[64];
    int n = k32_env_get("GREETING", 8, buf, sizeof buf);
    k32_log("echo pod init", 13);
    if (n > 0) k32_log(buf, (uint32_t)n);
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    if (size > sizeof scratch) return 0;
    return (uint32_t)(uintptr_t)scratch;
}

static void put_escaped(const unsigned char* p, uint32_t len) {
    char out[256];
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = p[i];
        if (o > sizeof(out) - 8) { k32_resp_write(out, o); o = 0; }
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    if (o) k32_resp_write(out, o);
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const unsigned char* req = (const unsigned char*)(uintptr_t)ptr;
    requests++;
    int is_post = (len > 4 && req[0] == 'P' && req[1] == 'O');
    k32_resp_begin(200, "application/json", 16);
    k32_resp_header("X-Echo-Count", 12, "1", 1);
    const char* a = "{\"pod\":\"echo\",\"requests\":";
    k32_resp_write(a, slen(a));
    char num[16]; int n = 0; unsigned long v = requests; char tmp[16]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) num[n++] = tmp[--t];
    k32_resp_write(num, (uint32_t)n);
    const char* b = ",\"uptime_ms\":";
    k32_resp_write(b, slen(b));
    long long ms = k32_time_ms(); t = 0; v = (unsigned long)ms;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    n = 0; while (t) num[n++] = tmp[--t];
    k32_resp_write(num, (uint32_t)n);
    const char* c = ",\"request\":\"";
    k32_resp_write(c, slen(c));
    put_escaped(req, len > 600 ? 600 : len);
    k32_resp_write("\"}", 2);
    k32_resp_end();
    if (is_post) {
        const char* ev = "{\"op\":\"echo\",\"n\":1}";
        k32_event_emit("tessera.events", 14, ev, slen(ev));
    }
    return 200;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tptr, uint32_t tlen, uint32_t ptr, uint32_t len) {
    (void)tptr; (void)tlen; (void)ptr; (void)len;
    k32_log("echo got event", 14);
    return 0;
}
