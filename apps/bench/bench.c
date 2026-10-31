/* bench: the benchmark kernels of src/bench/bench_kernels.c as a pod, to
 * compare with the same kernels run natively (K32_CMD:BENCH on the node's
 * console). See docs/benchmarks.md.
 *
 *   GET /run?k=<kernel>&n=<size>&r=<runs>   runs the kernel r times
 *       {"kernel":"crc32","n":8,"runs":5,"us_min":..,"us_total":..,"sum":..}
 *   GET /hostcall?n=<calls>                 n calls of k32_time_ms
 *       {"calls":n,"us":..}
 *   GET /                                   "ok" (request round trip)
 *
 * Built into images/sd/bench.wasm (not embedded); deploy it inline with
 * kube32-cli deploy. */
#include "../../include/runtime/k32_app_abi.h"
#include "../../include/bench/bench_kernels.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);

/* wasm3 on the ESP32-S3 makes no tail calls: each instruction keeps a native
 * frame until its function returns, so helpers stay functions of their own */
#define NOINLINE __attribute__((noinline))

static unsigned char req_buf[1024];
static uint32_t work[BENCH_WORK_BYTES / 4];
static char out[192];
static unsigned out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static NOINLINE void put(const char* s) { while (*s && out_len < sizeof out) out[out_len++] = *s++; }
static NOINLINE void put_uint(uint64_t v) {
    char d[20]; int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k && out_len < sizeof out) out[out_len++] = d[--k];
}
static NOINLINE void reply(int32_t status) {
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
}

/* Value of `key` in the query [q, q + len), or 0 when absent */
static NOINLINE const char* query(const char* q, uint32_t len, const char* key, uint32_t* vlen) {
    uint32_t klen = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i == 0 || q[i - 1] == '&') && q[i + klen] == '=') {
            uint32_t j = 0;
            while (j < klen && q[i + j] == key[j]) j++;
            if (j < klen) continue;
            uint32_t s = i + klen + 1, e = s;
            while (e < len && q[e] != '&') e++;
            *vlen = e - s;
            return q + s;
        }
    }
    *vlen = 0;
    return 0;
}
static NOINLINE uint32_t query_uint(const char* q, uint32_t len, const char* key, uint32_t def) {
    uint32_t vlen;
    const char* v = query(q, len, key, &vlen);
    if (!v || !vlen) return def;
    uint32_t n = 0;
    for (uint32_t i = 0; i < vlen && v[i] >= '0' && v[i] <= '9'; i++) n = n * 10u + (uint32_t)(v[i] - '0');
    return n;
}

static NOINLINE int32_t run(const char* q, uint32_t qlen) {
    uint32_t klen;
    const char* k = query(q, qlen, "k", &klen);
    const int index = k ? bench_kernel_index(k, klen) : -1;
    if (index < 0) {
        put("{\"error\":\"k is one of fib, crc32, sieve, sort, mandel, json\"}");
        reply(400);
        return 400;
    }
    const uint32_t n = query_uint(q, qlen, "n", 1), runs = query_uint(q, qlen, "r", 1);
    uint64_t total = 0, best = ~0ull;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < (runs ? runs : 1); i++) {
        const int64_t t0 = esp32_get_time_us();
        sum = bench_kernel_run(index, n, work);
        const uint64_t us = (uint64_t)(esp32_get_time_us() - t0);
        total += us;
        if (us < best) best = us;
    }
    put("{\"kernel\":\""); put(bench_kernel_name(index));
    put("\",\"n\":"); put_uint(n);
    put(",\"runs\":"); put_uint(runs ? runs : 1);
    put(",\"us_min\":"); put_uint(best);
    put(",\"us_total\":"); put_uint(total);
    put(",\"sum\":"); put_uint(sum);
    put("}");
    reply(200);
    return 200;
}

static NOINLINE int32_t hostcall(const char* q, uint32_t qlen) {
    const uint32_t n = query_uint(q, qlen, "n", 1000);
    int64_t sink = 0;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t i = 0; i < n; i++) sink += k32_time_ms();
    const uint64_t us = (uint64_t)(esp32_get_time_us() - t0);
    put("{\"calls\":"); put_uint(n);
    put(",\"us\":"); put_uint(us);
    put(",\"sink\":"); put_uint((uint64_t)sink & 1u);
    put("}");
    reply(200);
    return 200;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    out_len = 0;
    /* "METHOD SP target LF ..." */
    uint32_t sp = 0;
    while (sp < len && m[sp] != ' ') sp++;
    const char* t = m + sp + 1;
    uint32_t tlen = 0;
    while (sp + 1 + tlen < len && t[tlen] != '\n' && t[tlen] != ' ') tlen++;
    uint32_t qpos = 0;
    while (qpos < tlen && t[qpos] != '?') qpos++;
    const char* q = qpos < tlen ? t + qpos + 1 : t + tlen;
    const uint32_t qlen = qpos < tlen ? tlen - qpos - 1 : 0;
    if (qpos == 4 && t[0] == '/' && t[1] == 'r' && t[2] == 'u' && t[3] == 'n') return run(q, qlen);
    if (qpos == 9 && t[1] == 'h' && t[5] == 'c') return hostcall(q, qlen);
    put("\"ok\"");
    reply(200);
    return 200;
}
