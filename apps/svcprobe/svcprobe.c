/* svcprobe: a small request pod for exercising the platform from outside:
 * Service calls (k32_http_call), bundle files (k32_resp_asset) and memory
 * limits under one Wasm page. It runs in 16 KiB of linear memory.
 *
 *   GET  /hello                 "hello from <HOSTNAME>"
 *   GET  /big?n=<bytes>         n bytes of 'x' (chunked)
 *   POST /echo                  the request body, unchanged
 *   GET  /call?u=<url>[&cap=<bytes>][&t=<ms>][&m=POST]
 *                               k32_http_call(u) with a cap-byte buffer (at most 4096);
 *                               {"rc":<status or error>,"len":<bytes>,"us":<call time>,"body":"<first 64 bytes>"}
 *                               a POST call sends this request's body
 *   GET  /asset/<path>          k32_resp_asset(path) with a Cache-Control header set
 *                               first; 404 {"error":"no such asset"} when the bundle lacks it
 *   GET  /mem                   {"used":<backing bytes>,"limit":<limit>,"pages":<memory.size>}
 *   GET  /grow                  {"grow":<memory.grow(1) result>}
 *   GET  /touch?a=<address>     reads one byte of linear memory (traps past the backing)
 *   GET  /spin?ms=<ms>          returns after ms milliseconds of work
 *
 * Built with the stack first and small buffers so everything sits in the
 * first 16 KiB: make -C apps/svcprobe (output images/sd/svcprobe.wasm).
 * Each route is a function of its own: wasm3 on Xtensa (no tail calls)
 * holds a native frame per instruction until a function returns, so one
 * large handler needs tens of KB of the node's stack. */
#include "../../include/runtime/k32_app_abi.h"

static unsigned char req_buf[4096];
static unsigned char resp_buf[4096];
static char out[512];
static unsigned out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static int starts(const char* s, unsigned len, const char* p) {
    unsigned n = slen(p);
    if (len < n) return 0;
    for (unsigned i = 0; i < n; i++) if (s[i] != p[i]) return 0;
    return 1;
}
static void put(const char* s, unsigned n) {
    for (unsigned i = 0; i < n && out_len < sizeof out - 1; i++) out[out_len++] = s[i];
}
static void puts_(const char* s) { put(s, slen(s)); }
static void putnum(long long v) {
    char tmp[24]; int t = 0;
    if (v < 0) { put("-", 1); v = -v; }
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) put(&tmp[--t], 1);
}
static void reply(int status, const char* ctype) {
    k32_resp_begin(status, ctype, slen(ctype));
    k32_resp_write(out, out_len);
    k32_resp_end();
}

/* Value of query parameter `key` in [q, q+qlen), %XX decoded into dst */
static int param(const char* q, unsigned qlen, const char* key, char* dst, unsigned cap) {
    unsigned kl = slen(key);
    for (unsigned i = 0; i < qlen; ) {
        unsigned j = i;
        while (j < qlen && q[j] != '&') j++;
        if (j - i > kl && q[i + kl] == '=' && starts(q + i, j - i, key)) {
            unsigned o = 0;
            for (unsigned k = i + kl + 1; k < j && o < cap - 1; k++) {
                char c = q[k];
                if (c == '%' && k + 2 < j) {
                    int v = 0;
                    for (int d = 1; d <= 2; d++) {
                        char h = q[k + d];
                        v = v * 16 + ((h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0);
                    }
                    c = (char)v;
                    k += 2;
                }
                dst[o++] = c;
            }
            dst[o] = 0;
            return (int)o;
        }
        i = j + 1;
    }
    return -1;
}
static long num(const char* s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

struct req {
    const char* target; unsigned qpos;   /* path is target[0, qpos) */
    const char* q; unsigned qlen;        /* query */
    const char* body; unsigned blen;
};
#define ROUTE static __attribute__((noinline)) int32_t

ROUTE r_hello(const struct req* r) {
    (void)r;
    char host[64];
    int n = k32_env_get("HOSTNAME", 8, host, sizeof host);
    puts_("hello from ");
    if (n > 0) put(host, (unsigned)n);
    puts_("\n");
    reply(200, "text/plain");
    return 200;
}

ROUTE r_big(const struct req* r) {
    char arg[24];
    long n = param(r->q, r->qlen, "n", arg, sizeof arg) > 0 ? num(arg) : 1024;
    k32_resp_begin(200, "application/octet-stream", 24);
    for (unsigned i = 0; i < sizeof resp_buf; i++) resp_buf[i] = 'x';
    while (n > 0) {
        long k = n < (long)sizeof resp_buf ? n : (long)sizeof resp_buf;
        k32_resp_write(resp_buf, (uint32_t)k);
        n -= k;
    }
    k32_resp_end();
    return 200;
}

ROUTE r_echo(const struct req* r) {
    k32_resp_begin(200, "application/octet-stream", 24);
    k32_resp_write(r->body, r->blen);
    k32_resp_end();
    return 200;
}

static __attribute__((noinline)) void put_body_excerpt(uint32_t got) {
    for (uint32_t i = 0; i < got && i < 64; i++) {
        char c = (char)resp_buf[i];
        if (c == '"' || c == '\\') put("\\", 1);
        if ((unsigned char)c < 32) c = ' ';
        put(&c, 1);
    }
}

ROUTE r_call(const struct req* r) {
    char url[200], arg[24];
    if (param(r->q, r->qlen, "u", url, sizeof url) <= 0) { puts_("{\"error\":\"u missing\"}"); reply(400, "application/json"); return 400; }
    uint32_t cap = param(r->q, r->qlen, "cap", arg, sizeof arg) > 0 ? (uint32_t)num(arg) : sizeof resp_buf;
    if (cap > sizeof resp_buf) cap = sizeof resp_buf;
    uint32_t t = param(r->q, r->qlen, "t", arg, sizeof arg) > 0 ? (uint32_t)num(arg) : 0;
    int post = param(r->q, r->qlen, "m", arg, sizeof arg) > 0 && arg[0] == 'P';
    uint32_t got = 0;
    int64_t t0 = k32_time_ms();
    const char* hdr = "Content-Type: application/octet-stream\r\n";
    int32_t rc = k32_http_call(post ? "POST" : "GET", post ? 4 : 3, url, slen(url), hdr, post ? slen(hdr) : 0,
                               (const uint8_t*)r->body, post ? r->blen : 0, resp_buf, cap, &got, t);
    int64_t ms = k32_time_ms() - t0;
    puts_("{\"rc\":"); putnum(rc);
    puts_(",\"len\":"); putnum(got);
    puts_(",\"ms\":"); putnum(ms);
    puts_(",\"body\":\"");
    put_body_excerpt(got);
    puts_("\"}");
    reply(200, "application/json");
    return 200;
}

ROUTE r_asset(const struct req* r) {
    k32_resp_header("Cache-Control", 13, "no-cache", 8);
    int32_t rc = k32_resp_asset(r->target + 7, r->qpos - 7, 0);
    if (rc == 0) return 200;
    k32_resp_header("Cache-Control", 13, "", 0);   /* withdrawn: the 404 is not cacheable */
    puts_("{\"error\":\"no such asset\",\"rc\":"); putnum(rc); puts_("}");
    reply(404, "application/json");
    return 404;
}

ROUTE r_mem(const struct req* r) {
    (void)r;
    uint32_t used = 0, limit = 0;
    k32_mem_stats(&used, &limit);
    puts_("{\"used\":"); putnum(used);
    puts_(",\"limit\":"); putnum(limit);
    puts_(",\"pages\":"); putnum((long long)__builtin_wasm_memory_size(0));
    puts_("}");
    reply(200, "application/json");
    return 200;
}

ROUTE r_grow(const struct req* r) {
    (void)r;
    puts_("{\"grow\":"); putnum((long long)(int)__builtin_wasm_memory_grow(0, 1)); puts_("}");
    reply(200, "application/json");
    return 200;
}

ROUTE r_touch(const struct req* r) {
    char arg[24];
    long a = param(r->q, r->qlen, "a", arg, sizeof arg) > 0 ? num(arg) : 0;
    volatile unsigned char* p = (volatile unsigned char*)(uintptr_t)a;
    puts_("{\"byte\":"); putnum(*p); puts_("}");
    reply(200, "application/json");
    return 200;
}

ROUTE r_spin(const struct req* r) {
    char arg[24];
    long ms = param(r->q, r->qlen, "ms", arg, sizeof arg) > 0 ? num(arg) : 100;
    int64_t end = k32_time_ms() + ms;
    unsigned long loops = 0;
    while (k32_time_ms() < end) loops++;
    puts_("{\"loops\":"); putnum((long long)loops); puts_("}");
    reply(200, "application/json");
    return 200;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    out_len = 0;
    /* "METHOD SP target LF headers LF LF body" */
    unsigned sp = 0;
    while (sp < len && m[sp] != ' ') sp++;
    unsigned le = sp + 1;
    while (le < len && m[le] != '\n') le++;
    struct req r;
    r.target = m + sp + 1;
    unsigned tlen = le - sp - 1;
    r.qpos = 0;
    while (r.qpos < tlen && r.target[r.qpos] != '?') r.qpos++;
    r.q = r.qpos < tlen ? r.target + r.qpos + 1 : r.target + tlen;
    r.qlen = r.qpos < tlen ? tlen - r.qpos - 1 : 0;
    r.body = m + len;
    for (unsigned i = le; i + 1 < len; i++) {
        if (m[i] == '\n' && m[i + 1] == '\n') { r.body = m + i + 2; break; }
    }
    r.blen = (unsigned)(m + len - r.body);

    static const struct { const char* path; int32_t (*fn)(const struct req*); } routes[] = {
        {"/hello", r_hello}, {"/big", r_big}, {"/echo", r_echo}, {"/call", r_call}, {"/asset/", r_asset},
        {"/mem", r_mem}, {"/grow", r_grow}, {"/touch", r_touch}, {"/spin", r_spin},
    };
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        if (starts(r.target, r.qpos, routes[i].path)) return routes[i].fn(&r);
    }
    puts_("{\"error\":\"not found\"}");
    reply(404, "application/json");
    return 404;
}
