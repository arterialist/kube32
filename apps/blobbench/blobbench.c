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
 *   GET /check?what=products|elementwise&seed=<n>
 *       the kernels' results against this pod's own loops, bit for bit:
 *       k32_matvec_i8/i4 and k32_matmul_i8/i4 at many column counts, row
 *       strides and offsets of the weights and the vectors (weights in
 *       memory, and in place in the blob), or the element-wise imports
 *       (k32_rmsnorm_f32, k32_quantize_i8, k32_rope_f32, k32_swiglu_f32,
 *       k32_add_f32, k32_argmax_f32) on random vectors
 *
 * The benchmarks answer {"test":..,"bytes":<weights or bytes moved>,"us":..,
 * "mbps_x1000":<bytes per microsecond x 1000>} and log the same line; a check
 * answers {"test":..,"cases":..,"mismatches":..,"first":"<the first that
 * differs>"}. A path matches by its last segment, so the pod also answers
 * under an Ingress prefix (/blobbench/matvec). */
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

/* ---- checks ------------------------------------------------------------- */
static uint32_t rng = 1;
static NOINLINE uint32_t rnd(void) { return rng = rng * 1664525u + 1013904223u; }
/* a float of either sign, about -scale..scale */
static NOINLINE float rnd_f(float scale) { return (float)((int32_t)(rnd() >> 8) - (1 << 23)) * (scale / 8388608.0f); }
static NOINLINE uint32_t bits_of(float f) { uint32_t b; __builtin_memcpy(&b, &f, 4); return b; }

static uint32_t cases, mismatches;
static char first[96];
static uint32_t first_len;
static NOINLINE void note(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    mismatches++;
    if (first_len) return;
    out_len = 0;
    put(what); put(" "); put_uint(a); put(" "); put_uint(b); put(" "); put_uint(c); put(" "); put_uint(d);
    for (uint32_t i = 0; i < out_len && i < sizeof first; i++) first[first_len++] = out[i];
    out_len = 0;
}

static NOINLINE void check_result(const char* test) {
    out_len = 0;
    put("{\"test\":\""); put(test); put("\",\"cases\":"); put_uint(cases);
    put(",\"mismatches\":"); put_uint(mismatches); put(",\"first\":\"");
    for (uint32_t i = 0; i < first_len && out_len < sizeof out; i++) out[out_len++] = first[i];
    put("\"}");
    reply(mismatches ? 500 : 200);
}

/* weight c of a row: int8, or a signed nibble of int4 (column 2j low, 2j + 1 high) */
static NOINLINE int32_t weight_at(const int8_t* row, uint32_t bits, uint32_t c) {
    if (bits == 8) return row[c];
    const int32_t b = row[c / 2];
    return c & 1 ? b >> 4 : (int32_t)((uint32_t)b << 28) >> 28;
}

#define CK_ROWS 3
#define CK_N 9
#define CK_COLS 600
static float ck_ws[CK_ROWS], ck_xs[CK_N], ck_out[CK_ROWS * CK_N], ck_ref[CK_ROWS * CK_N];

/* out[p * rows + r] as the kernels define it, from rows in memory */
static NOINLINE void ref_products(const int8_t* w, uint32_t stride, const int8_t* x, uint32_t bits, uint32_t cols, uint32_t n) {
    for (uint32_t p = 0; p < n; p++)
        for (uint32_t r = 0; r < CK_ROWS; r++) {
            uint32_t acc = 0;
            for (uint32_t c = 0; c < cols; c++) acc += (uint32_t)(weight_at(w + r * stride, bits, c) * x[p * cols + c]);
            ck_ref[p * CK_ROWS + r] = (float)(int32_t)acc * (ck_ws[r] * ck_xs[p]);
        }
}

/* One case: CK_ROWS rows stride bytes apart at w (a blob address, or wm
 * itself), their copy in memory at wm, n vectors at x: matmul, then the
 * matvec of each vector */
static NOINLINE void product_case(uint32_t bits, uint32_t cols, uint32_t stride, const int8_t* wm, const void* w, const int8_t* x,
                                  uint32_t n, uint32_t tag) {
    const uint32_t rows = CK_ROWS;
    ref_products(wm, stride, x, bits, cols, n);
    for (uint32_t i = 0; i < rows * n; i++) ck_out[i] = -1.0f;
    int32_t rc = bits == 4 ? k32_matmul_i4(ck_out, w, ck_ws, x, ck_xs, rows, cols, stride, n)
                           : k32_matmul_i8(ck_out, (const int8_t*)w, ck_ws, x, ck_xs, rows, cols, stride, n);
    cases++;
    for (uint32_t i = 0; i < rows * n; i++)
        if (rc != K32_OK || bits_of(ck_out[i]) != bits_of(ck_ref[i])) { note("matmul bits cols stride tag", bits, cols, stride, tag); break; }
    for (uint32_t p = 0; p < n; p++) {
        rc = bits == 4 ? k32_matvec_i4(ck_out, w, ck_ws, x + p * cols, ck_xs[p], rows, cols, stride)
                       : k32_matvec_i8(ck_out, (const int8_t*)w, ck_ws, x + p * cols, ck_xs[p], rows, cols, stride);
        cases++;
        for (uint32_t r = 0; r < rows; r++)
            if (rc != K32_OK || bits_of(ck_out[r]) != bits_of(ck_ref[p * rows + r])) { note("matvec bits cols stride tag", bits, cols, stride, tag); break; }
    }
}

static const uint16_t kCols[] = {1, 2, 15, 16, 17, 31, 32, 33, 47, 64, 100, 172, 255, 256, 257, 300, 512, 513, 600};

/* The products at every column count, with the weights and the vectors at
 * each offset from 16-byte alignment (and a row stride that moves it from row
 * to row), from memory and, when the blob is mapped, from the blob in place */
static NOINLINE void check_products(void) {
    int8_t* wbuf = (int8_t*)buf;                     /* weights: 8 KiB, then a copy of blob rows */
    int8_t* xbuf = (int8_t*)buf + 16384;             /* the vectors */
    for (uint32_t i = 0; i < 8192; i++) wbuf[i] = (int8_t)(rnd() >> 24);
    for (uint32_t i = 0; i < CK_N * CK_COLS + 16; i++) xbuf[i] = (int8_t)(rnd() >> 24);
    for (uint32_t r = 0; r < CK_ROWS; r++) ck_ws[r] = rnd_f(0.05f);
    for (uint32_t p = 0; p < CK_N; p++) ck_xs[p] = rnd_f(0.02f);
    /* one vector at the int8 extremes: the largest sums */
    for (uint32_t c = 0; c < CK_COLS; c++) xbuf[CK_COLS + c] = (int8_t)(c & 1 ? -128 : 127);
    const int in_blob = blob >= 0 && blob_mapped == 1 && blob_size > 32768;
    for (uint32_t bits = 8; bits >= 4; bits -= 4)
        for (uint32_t k = 0; k < sizeof kCols / sizeof kCols[0]; k++) {
            const uint32_t cols = kCols[k], rb = bits == 4 ? (cols + 1) / 2 : cols;
            for (uint32_t off = 0; off < 16; off++) {
                const uint32_t stride = rb + (off & 4 ? 0 : off & 3), xo = (off * 7 + k) & 15;
                const uint32_t n = 1 + (off + k) % CK_N;
                product_case(bits, cols, stride, wbuf + off, wbuf + off, xbuf + xo, n, off * 16 + xo);
                if (in_blob) {
                    /* the same shape read in place from the blob, and copied for the reference */
                    const uint32_t bo = 4096 + k * 977 + off;
                    if (k32_blob_read(blob, bo, wbuf + 8192, CK_ROWS * stride) != (int32_t)(CK_ROWS * stride)) { note("blob read", bo, 0, 0, 0); continue; }
                    product_case(bits, cols, stride, wbuf + 8192, K32_BLOB_ADDR(blob, bo), xbuf + xo, n, 1000 + off);
                }
            }
        }
    /* every pair of offsets at one column count */
    for (uint32_t bits = 8; bits >= 4; bits -= 4)
        for (uint32_t wo = 0; wo < 16; wo++)
            for (uint32_t xo = 0; xo < 16; xo++) product_case(bits, 67, bits == 4 ? 34 : 67, wbuf + wo, wbuf + wo, xbuf + xo, 2, wo * 16 + xo);
    check_result("products");
}

/* The element-wise operations as pods compute them in Wasm */
static NOINLINE void ref_rmsnorm(float* o, const float* x, const float* w, uint32_t n, float eps) {
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (float)n;
    ss = ss + eps;
    ss = 1.0f / __builtin_sqrtf(ss);
    for (uint32_t i = 0; i < n; i++) o[i] = w[i] * (ss * x[i]);
}
static NOINLINE float ref_quantize(int8_t* q, const float* x, uint32_t n) {
    float amax = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float a = __builtin_fabsf(x[i]);
        if (a > amax) amax = a;
    }
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
    for (uint32_t i = 0; i < n; i++) {
        const float t = x[i] * inv;
        const int32_t r = (int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
        q[i] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    return amax / 127.0f;
}

static NOINLINE void compare(const char* what, const float* a, const float* b, uint32_t n, uint32_t tag) {
    cases++;
    for (uint32_t i = 0; i < n; i++)
        if (bits_of(a[i]) != bits_of(b[i])) { note(what, tag, i, bits_of(a[i]), bits_of(b[i])); return; }
}

static const uint16_t kLens[] = {1, 2, 3, 8, 16, 17, 64, 100, 172, 256, 300, 512};

static NOINLINE void check_elementwise(void) {
    float* x = (float*)buf;                 /* 2048 floats each: inputs, results, reference */
    float* got = x + 2048;
    float* ref = x + 4096;
    float* w = x + 6144;                    /* 512 */
    float* sc = x + 6656;                   /* 16 scales */
    int8_t* q = (int8_t*)(x + 6672);
    int8_t* qr = q + 2048;
    for (uint32_t k = 0; k < sizeof kLens / sizeof kLens[0]; k++) {
        const uint32_t n = kLens[k], most = 1024 / n ? 1024 / n : 1;
        const uint32_t count = 1 + k % 9 < most ? 1 + k % 9 : most;
        const uint32_t m = n * count;
        const float span = k & 1 ? 40.0f : 1.5f;
        for (uint32_t i = 0; i < 2 * m; i++) x[i] = rnd_f(span);
        if (k == 3) for (uint32_t i = 0; i < n; i++) x[i] = 0.0f;   /* a vector of zeros */
        for (uint32_t i = 0; i < n; i++) w[i] = rnd_f(2.0f);
        /* rmsnorm, into another vector and in place */
        k32_rmsnorm_f32(got, x, w, n, count, 1e-5f);
        for (uint32_t p = 0; p < count; p++) ref_rmsnorm(ref + p * n, x + p * n, w, n, 1e-5f);
        compare("rmsnorm n i got want", got, ref, m, n);
        for (uint32_t i = 0; i < m; i++) got[i] = x[i];
        k32_rmsnorm_f32(got, got, w, n, count, 1e-6f);
        for (uint32_t p = 0; p < count; p++) ref_rmsnorm(ref + p * n, x + p * n, w, n, 1e-6f);
        compare("rmsnorm in place n i got want", got, ref, m, n);
        /* quantize */
        k32_quantize_i8(q, sc, x, n, count);
        cases++;
        for (uint32_t p = 0; p < count; p++) {
            const float s = ref_quantize(qr + p * n, x + p * n, n);
            uint32_t bad = bits_of(s) != bits_of(sc[p]);
            for (uint32_t i = 0; i < n; i++) bad |= q[p * n + i] != qr[p * n + i];
            if (bad) { note("quantize n p got want", n, p, bits_of(sc[p]), bits_of(s)); break; }
        }
        /* rope with a head size that divides n, a row of (cos, sin) a vector */
        const uint32_t hs = n % 2 ? 0 : n % 8 == 0 ? 8 : 2;
        if (hs) {
            for (uint32_t i = 0; i < count * hs; i++) w[i] = rnd_f(1.0f);
            for (uint32_t i = 0; i < m; i++) got[i] = ref[i] = x[i];
            k32_rope_f32(got, w, n, hs, count);
            for (uint32_t p = 0; p < count; p++)
                for (uint32_t i = 0; i < n; i += 2) {
                    float* a = ref + p * n;
                    const float c = w[p * hs + i % hs], s = w[p * hs + i % hs + 1];
                    const float a0 = a[i], a1 = a[i + 1];
                    a[i] = a0 * c - a1 * s;
                    a[i + 1] = a0 * s + a1 * c;
                }
            compare("rope n i got want", got, ref, m, n);
        }
        /* swiglu with the next m floats as the gate, then add */
        const float* g = x + m;
        for (uint32_t i = 0; i < m; i++) got[i] = ref[i] = x[i];
        k32_swiglu_f32(got, g, m);
        for (uint32_t i = 0; i < m; i++) {
            float a = ref[i];
            a = a * (1.0f / (1.0f + k32_expf(-a)));
            ref[i] = a * g[i];
        }
        compare("swiglu n i got want", got, ref, m, n);
        k32_add_f32(got, x, m);
        for (uint32_t i = 0; i < m; i++) ref[i] += x[i];
        compare("add n i got want", got, ref, m, n);
        /* argmax, with a tie: the first */
        x[m / 2] = 1000.0f;
        x[m - 1] = 1000.0f;
        uint32_t best = 0;
        for (uint32_t i = 1; i < m; i++)
            if (x[i] > x[best]) best = i;
        cases++;
        const int32_t am = k32_argmax_f32(x, m);
        if (am != (int32_t)best) note("argmax m got want", m, (uint32_t)am, best, 0);
    }
    check_result("elementwise");
}

/* the path (before its query) ends with word ("/blobbench/matvec" and "/matvec" match "/matvec") */
static NOINLINE int starts(const char* p, uint32_t len, const char* word) {
    uint32_t wl = 0, end = 0;
    while (word[wl]) wl++;
    while (end < len && p[end] != '?' && p[end] != ' ') end++;
    if (end < wl) return 0;
    for (uint32_t i = 0; i < wl; i++)
        if (p[end - wl + i] != word[i]) return 0;
    return 1;
}

/* the query has key=value */
static NOINLINE int query_is(const char* q, uint32_t len, const char* kv) {
    uint32_t n = 0;
    while (kv[n]) n++;
    for (uint32_t i = 0; i + n <= len; i++) {
        uint32_t j = 0;
        while (j < n && q[i + j] == kv[j]) j++;
        if (j == n && (i == 0 || q[i - 1] == '&')) return 1;
    }
    return 0;
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
    if (starts(path, plen, "/check")) {
        rng = 1 + query_uint(q, qlen, "seed", 1);
        cases = mismatches = first_len = 0;
        if (query_is(q, qlen, "what=elementwise")) check_elementwise();
        else check_products();
        return 200;
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
