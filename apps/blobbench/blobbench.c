/* blobbench: how fast a pod reads a blob on this node (docs/storage.md#blobs).
 *
 * The pod opens the blob named by env BLOB (default "bench"); its manifest
 * names it in kube32.io/blobs.
 *
 *   GET /info                               {"size":..,"mapped":0|1}
 *   GET /read?chunk=<bytes>&passes=<n>      the whole blob, k32_blob_read in
 *                                           chunks of <bytes> (at most 32768)
 *   GET /matvec?bits=8|4&cols=<n>&passes=<n>
 *       the whole blob as a matrix of <n> columns, int8 or int4, through
 *       k32_matvec_i8/i4 reading the weights in place (K32_BLOB_ADDR), 256
 *       rows a call; a mapped blob only
 *   GET /readmv?bits=8|4&cols=<n>&chunk=<bytes>
 *       the same product with the rows read into memory <bytes> at a time
 *       first: what a pod does with a blob that is not mapped
 *   GET /memmv?bits=8|4&cols=<n>&reps=<n>
 *       the product over 32 KiB of weights already in the pod's memory, <n>
 *       times: the kernel without the storage
 *
 * Each answers {"test":..,"bytes":<weights or bytes moved>,"us":..,
 * "mbps_x1000":<bytes per microsecond x 1000>} and logs the same line. */
#include "../../include/runtime/k32_app_abi.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);

/* wasm3 on the ESP32-S3 makes no tail calls: helpers stay functions of their own */
#define NOINLINE __attribute__((noinline))

#define BUF_BYTES 32768
#define BLOCK_ROWS 256
#define MAX_COLS 4096

static uint8_t buf[BUF_BYTES] __attribute__((aligned(16)));
static int8_t xv[MAX_COLS];
static float outv[BLOCK_ROWS];
static float scales[BLOCK_ROWS];
static unsigned char req_buf[512];
static char out[256];
static unsigned out_len;
static int32_t blob = -1;
static uint32_t blob_size;
static int32_t blob_mapped;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static NOINLINE void put(const char* s) { while (*s && out_len < sizeof out) out[out_len++] = *s++; }
static NOINLINE void put_uint(uint64_t v) {
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k && out_len < sizeof out) out[out_len++] = d[--k];
}
static NOINLINE void put_int(int64_t v) {
    if (v < 0) { put("-"); v = -v; }
    put_uint((uint64_t)v);
}
static NOINLINE void reply(int32_t status) {
    k32_log(out, out_len);
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
}
static NOINLINE void fail(const char* why, int64_t code) {
    out_len = 0;
    put("{\"error\":\""); put(why); put("\",\"code\":"); put_int(code); put("}");
    reply(400);
}

/* Value of `key` in the query [q, q + len) as a number, or dflt */
static NOINLINE uint32_t query_uint(const char* q, uint32_t len, const char* key, uint32_t dflt) {
    uint32_t klen = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i == 0 || q[i - 1] == '&') && q[i + klen] == '=') {
            uint32_t j = 0;
            while (j < klen && q[i + j] == key[j]) j++;
            if (j < klen) continue;
            uint32_t v = 0;
            for (uint32_t e = i + klen + 1; e < len && q[e] >= '0' && q[e] <= '9'; e++) v = v * 10u + (uint32_t)(q[e] - '0');
            return v;
        }
    }
    return dflt;
}

static NOINLINE void result(const char* test, uint64_t bytes, uint64_t us) {
    out_len = 0;
    put("{\"test\":\""); put(test); put("\",\"bytes\":"); put_uint(bytes);
    put(",\"us\":"); put_uint(us);
    put(",\"mbps_x1000\":"); put_uint(us ? bytes * 1000u / us : 0);
    put("}");
    reply(200);
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char name[K32_BLOB_NAME_MAX + 1];
    int32_t n = k32_env_get("BLOB", 4, name, sizeof name);
    if (n <= 0 || n > K32_BLOB_NAME_MAX) {
        name[0] = 'b'; name[1] = 'e'; name[2] = 'n'; name[3] = 'c'; name[4] = 'h';
        n = 5;
    }
    blob = k32_blob_open(name, (uint32_t)n);
    out_len = 0;
    put("blobbench: open ");
    for (int32_t i = 0; i < n; i++) if (out_len < sizeof out) out[out_len++] = name[i];
    put(": ");
    put_int(blob);
    if (blob >= 0) {
        blob_size = (uint32_t)k32_blob_size(blob);
        blob_mapped = k32_blob_mapped(blob);
        put(", size "); put_uint(blob_size);
        put(blob_mapped == 1 ? ", mapped" : ", read only");
    }
    k32_log(out, out_len);
    for (uint32_t i = 0; i < BLOCK_ROWS; i++) scales[i] = 1.0f;
    for (uint32_t i = 0; i < MAX_COLS; i++) xv[i] = (int8_t)(i * 37u + 11u);
    return 0;
}

static NOINLINE uint32_t row_bytes(uint32_t bits, uint32_t cols) { return bits == 4 ? (cols + 1) / 2 : cols; }

static NOINLINE int32_t product(uint32_t bits, const void* w, uint32_t rows, uint32_t cols) {
    return bits == 4 ? k32_matvec_i4(outv, w, scales, xv, 1.0f, rows, cols, 0)
                     : k32_matvec_i8(outv, (const int8_t*)w, scales, xv, 1.0f, rows, cols, 0);
}

static NOINLINE void bench_read(uint32_t chunk, uint32_t passes) {
    if (chunk == 0 || chunk > BUF_BYTES) chunk = BUF_BYTES;
    uint64_t moved = 0;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t p = 0; p < passes; p++) {
        for (uint32_t off = 0; off < blob_size;) {
            int32_t n = k32_blob_read(blob, off, buf, chunk);
            if (n <= 0) { fail("k32_blob_read", n); return; }
            off += (uint32_t)n;
            moved += (uint32_t)n;
        }
    }
    result("read", moved, (uint64_t)(esp32_get_time_us() - t0));
}

static NOINLINE void bench_matvec(uint32_t bits, uint32_t cols, uint32_t passes) {
    const uint32_t rb = row_bytes(bits, cols);
    const uint32_t rows = blob_size / rb;
    uint64_t moved = 0;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t p = 0; p < passes; p++) {
        for (uint32_t r0 = 0; r0 < rows; r0 += BLOCK_ROWS) {
            const uint32_t n = rows - r0 < BLOCK_ROWS ? rows - r0 : BLOCK_ROWS;
            int32_t rc = product(bits, K32_BLOB_ADDR(blob, r0 * rb), n, cols);
            if (rc != K32_OK) { fail("matvec over the blob", rc); return; }
            moved += (uint64_t)n * rb;
        }
    }
    result(bits == 4 ? "matvec_i4_blob" : "matvec_i8_blob", moved, (uint64_t)(esp32_get_time_us() - t0));
}

static NOINLINE void bench_readmv(uint32_t bits, uint32_t cols, uint32_t chunk) {
    const uint32_t rb = row_bytes(bits, cols);
    uint32_t per = (chunk ? chunk : BUF_BYTES) / rb;   /* rows a read brings in */
    if (per > BUF_BYTES / rb) per = BUF_BYTES / rb;
    if (per > BLOCK_ROWS) per = BLOCK_ROWS;
    if (per == 0) { fail("a row is larger than the buffer", rb); return; }
    const uint32_t rows = blob_size / rb;
    uint64_t moved = 0;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t r0 = 0; r0 < rows; r0 += per) {
        const uint32_t n = rows - r0 < per ? rows - r0 : per;
        int32_t got = k32_blob_read(blob, r0 * rb, buf, n * rb);
        if (got != (int32_t)(n * rb)) { fail("k32_blob_read", got); return; }
        int32_t rc = product(bits, buf, n, cols);
        if (rc != K32_OK) { fail("matvec", rc); return; }
        moved += (uint64_t)n * rb;
    }
    result(bits == 4 ? "readmv_i4" : "readmv_i8", moved, (uint64_t)(esp32_get_time_us() - t0));
}

static NOINLINE void bench_memmv(uint32_t bits, uint32_t cols, uint32_t reps) {
    const uint32_t rb = row_bytes(bits, cols);
    uint32_t rows = BUF_BYTES / rb;
    if (rows > BLOCK_ROWS) rows = BLOCK_ROWS;
    for (uint32_t i = 0; i < BUF_BYTES; i++) buf[i] = (uint8_t)(i * 13u + 7u);
    uint64_t moved = 0;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t k = 0; k < reps; k++) {
        int32_t rc = product(bits, buf, rows, cols);
        if (rc != K32_OK) { fail("matvec", rc); return; }
        moved += (uint64_t)rows * rb;
    }
    result(bits == 4 ? "memmv_i4" : "memmv_i8", moved, (uint64_t)(esp32_get_time_us() - t0));
}

static NOINLINE int starts(const char* p, uint32_t len, const char* word) {
    uint32_t i = 0;
    while (word[i]) {
        if (i >= len || p[i] != word[i]) return 0;
        i++;
    }
    return i == len || p[i] == '?' || p[i] == ' ';
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    uint32_t i = 0;
    while (i < len && r[i] != ' ') i++;
    const char* path = r + i + 1;
    uint32_t plen = 0;
    while (i + 1 + plen < len && path[plen] != ' ' && path[plen] != '\n') plen++;
    const char* q = path;
    uint32_t qlen = 0;
    for (uint32_t k = 0; k < plen; k++) {
        if (path[k] == '?') { q = path + k + 1; qlen = plen - k - 1; break; }
    }
    if (blob < 0) { fail("no blob open", blob); return 400; }
    const uint32_t bits = query_uint(q, qlen, "bits", 8) == 4 ? 4 : 8;
    uint32_t cols = query_uint(q, qlen, "cols", 1024);
    if (cols == 0 || cols > MAX_COLS) cols = 1024;
    if (starts(path, plen, "/read")) bench_read(query_uint(q, qlen, "chunk", 4096), query_uint(q, qlen, "passes", 1));
    else if (starts(path, plen, "/matvec")) bench_matvec(bits, cols, query_uint(q, qlen, "passes", 1));
    else if (starts(path, plen, "/readmv")) bench_readmv(bits, cols, query_uint(q, qlen, "chunk", BUF_BYTES));
    else if (starts(path, plen, "/memmv")) bench_memmv(bits, cols, query_uint(q, qlen, "reps", 100));
    else {
        out_len = 0;
        put("{\"size\":"); put_uint(blob_size); put(",\"mapped\":"); put_int(blob_mapped); put("}");
        reply(200);
    }
    return 200;
}
