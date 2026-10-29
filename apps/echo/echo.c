/* Minimal request-handling pod: echoes the request block as JSON and
 * publishes {"type":"echo","n":<requests>} on the channel topic "echo.events"
 * for every POST. GET .../ui/<file> answers with a file of the image's
 * bundle (images/web/echo/) through k32_resp_asset. examples/echo.yaml routes
 * /echo to it with an Ingress and serves that topic as Server-Sent Events on
 * /echo/events. */
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

/* "GET <prefix>/ui/<file>[?query]": the bundle file, index.html for "/ui/" */
static int serve_ui(const unsigned char* req, uint32_t len) {
    if (len < 8 || req[0] != 'G' || req[1] != 'E' || req[2] != 'T' || req[3] != ' ') return 0;
    uint32_t end = 4;
    while (end < len && req[end] != ' ' && req[end] != '?' && req[end] != '\n') end++;
    for (uint32_t i = 4; i + 4 <= end; i++) {
        if (req[i] == '/' && req[i + 1] == 'u' && req[i + 2] == 'i' && (i + 3 == end || req[i + 3] == '/')) {
            uint32_t start = (i + 3 < end) ? i + 4 : end;
            const char* file = (start < end) ? (const char*)req + start : "index.html";
            uint32_t flen = (start < end) ? end - start : 10;
            if (k32_resp_asset(file, flen, 0) == 0) return 1;
            k32_resp_begin(404, "text/plain", 10);
            k32_resp_write("no such file\n", 13);
            k32_resp_end();
            return 1;
        }
    }
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const unsigned char* req = (const unsigned char*)(uintptr_t)ptr;
    if (serve_ui(req, len)) return 200;
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
        char ev[48];
        unsigned o = 0;
        const char* head = "{\"type\":\"echo\",\"n\":";
        for (unsigned i = 0; head[i]; i++) ev[o++] = head[i];
        v = requests; t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) ev[o++] = tmp[--t];
        ev[o++] = '}';
        k32_event_emit("echo.events", 11, ev, o);
    }
    return 200;
}

/* Topics named in the pod's kube32.io/event-topics annotation arrive here */
K32_EXPORT("k32_event") int32_t k32_event(uint32_t tptr, uint32_t tlen, uint32_t ptr, uint32_t len) {
    char line[96];
    unsigned o = 0;
    const char* head = "echo got event on ";
    for (unsigned i = 0; head[i]; i++) line[o++] = head[i];
    for (uint32_t i = 0; i < tlen && o < 60; i++) line[o++] = ((const char*)(uintptr_t)tptr)[i];
    line[o++] = ':';
    line[o++] = ' ';
    for (uint32_t i = 0; i < len && o < sizeof line; i++) line[o++] = ((const char*)(uintptr_t)ptr)[i];
    k32_log(line, o);
    return 0;
}
