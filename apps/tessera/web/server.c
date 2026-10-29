/*
 * tessera-web: serves the Tessera web client from the pod's image bundle
 * (images/web/tessera-web/, written by build.py) with k32_resp_asset.
 *
 *   GET /                  index.html                 Cache-Control: no-cache
 *   GET /<name>.<ext>      that file of the bundle    max-age=31536000, immutable with ?v=
 *                                                     (index.html links them that way), else 3600
 *   GET /<no extension>    index.html (the client routes in the browser)
 *
 * Other methods get 405, files the bundle lacks 404. Linear memory stays
 * below WEB_MEM_CAP: the request block lives in [__heap_base, WEB_MEM_CAP).
 */
#include "../common/k32_host.h"

#ifndef WEB_MEM_CAP
#define WEB_MEM_CAP 8192u
#endif
#define REQ_MIN 2048u           /* smallest request block the cap must leave room for */

#if defined(__wasm32__)
extern unsigned char __heap_base;
static uint32_t heap_base(void) { return (uint32_t)(uintptr_t)&__heap_base; }
static const char* resolve(uint32_t ptr) { return (const char*)(uintptr_t)ptr; }
static uint32_t heap_room(void) { return heap_base() < WEB_MEM_CAP ? WEB_MEM_CAP - heap_base() : 0; }
#else
static char g_native_mem[16 + WEB_MEM_CAP];
static uint32_t heap_base(void) { return 16; }
static const char* resolve(uint32_t ptr) { return g_native_mem + ptr; }
static uint32_t heap_room(void) { return WEB_MEM_CAP - 16; }
char* web_resolve(uint32_t ptr) { return g_native_mem + ptr; }
#endif

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int eq(const char* a, uint32_t n, const char* lit) {
    uint32_t i = 0;
    for (; i < n; i++) if (lit[i] != a[i]) return 0;
    return lit[i] == 0;
}
static int starts(const char* a, uint32_t n, const char* lit) {
    uint32_t l = slen(lit);
    if (n < l) return 0;
    for (uint32_t i = 0; i < l; i++) if (a[i] != lit[i]) return 0;
    return n == l || a[l] == '/';
}

static int32_t text_resp(int32_t status, const char* text, int no_store) {
    k32_resp_begin(status, "text/plain; charset=utf-8", 25);
    if (no_store) k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_write(text, slen(text));
    k32_resp_end();
    return status;
}
static int32_t plain(int32_t status, const char* text) { return text_resp(status, text, 1); }
static int32_t asset(const char* file, uint32_t len, const char* cache) {
    k32_resp_header("Cache-Control", 13, cache, slen(cache));
    if (k32_resp_asset(file, len, 0) == 0) return 200;
    /* Cache-Control is already set for this response; a second one would conflict */
    return text_resp(404, "not found\n", 0);
}

/* A bundle path: "/" separated names of [A-Za-z0-9._-], no ".." and no empty names. */
static int safe_path(const char* p, uint32_t n) {
    if (n < 1 || p[0] != '/') return 0;
    uint32_t seg = 1;
    for (uint32_t i = 1; i <= n; i++) {
        if (i == n || p[i] == '/') {
            uint32_t l = i - seg;
            if (i < n && l == 0) return 0;
            if ((l == 1 && p[seg] == '.') || (l == 2 && p[seg] == '.' && p[seg + 1] == '.')) return 0;
            seg = i + 1;
            continue;
        }
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    if (heap_room() < REQ_MIN) { k32_log("tessera-web: no room for requests under the memory cap", 54); return -1; }
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return (size && size <= heap_room()) ? heap_base() : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = resolve(ptr);
    uint32_t i = 0;
    while (i < len && r[i] != ' ' && r[i] != '\n') i++;
    if (i >= len || r[i] != ' ') return plain(400, "bad request\n");
    const char* method = r; uint32_t mlen = i++;
    const char* path = r + i; uint32_t ps = i;
    while (i < len && r[i] != '?' && r[i] != ' ' && r[i] != '\n') i++;
    uint32_t plen = i - ps;
    int versioned = 0;
    if (i < len && r[i] == '?') {
        uint32_t q = ++i;
        while (i < len && r[i] != ' ' && r[i] != '\n') i++;
        versioned = i - q >= 2 && r[q] == 'v' && r[q + 1] == '=';
    }
    if (!eq(method, mlen, "GET") && !eq(method, mlen, "HEAD")) return plain(405, "use GET\n");
    while (plen > 1 && path[plen - 1] == '/') plen--;                 /* "/board/" is "/board" */
    if (!safe_path(path, plen) || starts(path, plen, "/api") || starts(path, plen, "/events")) return plain(404, "not found\n");
    if (plen == 1 || eq(path, plen, "/index.html")) return asset("index.html", 10, "no-cache");
    uint32_t last = plen;
    while (last > 0 && path[last - 1] != '/') last--;
    int has_ext = 0;
    for (uint32_t k = last; k < plen; k++) if (path[k] == '.') has_ext = 1;
    if (!has_ext) return asset("index.html", 10, "no-cache");          /* a route of the client */
    return asset(path + 1, plen - 1, versioned ? "public, max-age=31536000, immutable" : "public, max-age=3600");
}
