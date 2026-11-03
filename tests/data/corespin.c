/* corespin: the test pod of tests/test_pod_workers.cpp.
 *
 * k32_tick works for SPIN_MS milliseconds of the node's clock (integer work
 * and k32_matvec_i8 calls), then calls CALL with k32_http_call when it is
 * set, and returns 1 while it has work (ticked again after 10 ms).
 * k32_event takes "spin <ms>" (a new SPIN_MS) or a sequence number, and
 * counts numbers that arrive out of order. k32_handle answers GET /stats
 * with the counters and the last 16 ticks' start and end times, and any
 * other request (the in-process calls of another corespin) with 200.
 *
 * Build: clang --target=wasm32 -Os -nostdlib -ffreestanding -mcpu=mvp -c corespin.c -o corespin.o
 *        rust-lld -flavor wasm --no-entry --export-dynamic --allow-undefined --stack-first \
 *                 -z stack-size=4096 --initial-memory=65536 --max-memory=65536 -o corespin.wasm corespin.o */
#include "../../include/runtime/k32_app_abi.h"

static unsigned char req_buf[2048];
static char out[1024];
static unsigned out_len;
static char call_url[96];
static unsigned call_len;
static uint32_t spin_ms;
static uint32_t ticks, events, disorder, calls_ok, calls_in, last_seq, sink;
static uint32_t starts[16], ends[16];
static int8_t w[64 * 64], x[64];
static float scales[64], y[64];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static uint32_t atou(const char* s, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint32_t)(s[i] - '0');
    return v;
}

static void put(const char* s) {
    while (*s && out_len < sizeof out) out[out_len++] = *s++;
}

static void put_u(uint32_t v) {
    char d[10];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && out_len < sizeof out) out[out_len++] = d[--n];
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char v[16];
    int32_t n = k32_env_get("SPIN_MS", 7, v, sizeof v);
    spin_ms = n > 0 ? atou(v, (uint32_t)n) : 0;
    n = k32_env_get("CALL", 4, call_url, sizeof call_url);
    call_len = n > 0 ? (uint32_t)n : 0;
    for (uint32_t i = 0; i < sizeof w; i++) w[i] = (int8_t)(i * 7);
    for (uint32_t i = 0; i < 64; i++) { x[i] = (int8_t)i; scales[i] = 1.0f; }
    return 0;
}

__attribute__((noinline)) static void work(void) {
    for (uint32_t i = 0; i < 2000; i++) sink = sink * 1664525u + 1013904223u + i;
    k32_matvec_i8(y, w, scales, x, 1.0f, 64, 64, 64);
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    (void)now;
    if (!spin_ms && !call_len) return 0;
    const uint32_t t0 = (uint32_t)k32_time_ms();
    while ((uint32_t)k32_time_ms() - t0 < spin_ms) work();
    if (call_len) {
        uint8_t resp[16];
        uint32_t got = 0;
        if (k32_http_call("POST", 4, call_url, call_len, "", 0, (const uint8_t*)"x", 1, resp, sizeof resp, &got, 5000) == 200) calls_ok++;
    }
    starts[ticks % 16] = t0;
    ends[ticks % 16] = (uint32_t)k32_time_ms();
    ticks++;
    if (ticks % 16 == 0) k32_log("corespin: 16 ticks", 18);
    return 1;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    (void)topic;
    (void)topic_len;
    const char* d = (const char*)(uintptr_t)ptr;
    if (len > 5 && d[0] == 's' && d[4] == ' ') {
        spin_ms = atou(d + 5, len - 5);
        return 0;
    }
    const uint32_t seq = atou(d, len);
    if (events && seq <= last_seq) disorder++;
    last_seq = seq;
    events++;
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    (void)len;
    out_len = 0;
    if (r[0] == 'G') {
        put("{\"ticks\":"); put_u(ticks);
        put(",\"events\":"); put_u(events);
        put(",\"disorder\":"); put_u(disorder);
        put(",\"calls_ok\":"); put_u(calls_ok);
        put(",\"calls_in\":"); put_u(calls_in);
        put(",\"spans\":[");
        const uint32_t n = ticks < 16 ? ticks : 16;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t k = (ticks - n + i) % 16;
            if (i) put(",");
            put("["); put_u(starts[k]); put(","); put_u(ends[k]); put("]");
        }
        put("]}");
    } else {
        calls_in++;
        put("ok");
    }
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return 0;
}
