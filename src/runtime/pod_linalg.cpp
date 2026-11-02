// Native linear algebra for pods (include/runtime/pod_linalg.hpp).
//
// The inner loops are written for the ESP32-S3's Xtensa LX7 core: byte loads,
// 32-bit integer multiplies with two independent accumulators, and float
// vectors copied into blocks on the stack so that the loops load them
// aligned. The build compiles this file with -ffp-contract=off: a multiply
// followed by an add stays two roundings, as in a Wasm pod.
#include "runtime/pod_linalg.hpp"
#include "runtime/k32_app_abi.h"

#include "wasm3.h"
#include "m3_env.h"

#include <cstdint>

namespace kube32::runtime {

// Float operands are 4-byte aligned in practice: one load or store then,
// bytes otherwise (Xtensa faults on an unaligned 32-bit access). ESP-IDF
// builds with -fno-builtin-memcpy, so the copies name the builtin.
static inline bool aligned4(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 3) == 0; }

static inline float load_f32(const uint8_t* p) {
    float f;
    if (aligned4(p)) __builtin_memcpy(&f, __builtin_assume_aligned(p, 4), sizeof f);
    else __builtin_memcpy(&f, p, sizeof f);
    return f;
}

static inline void store_f32(uint8_t* p, float f) {
    if (aligned4(p)) __builtin_memcpy(__builtin_assume_aligned(p, 4), &f, sizeof f);
    else __builtin_memcpy(p, &f, sizeof f);
}

// ---- integer products ----------------------------------------------------------

// Sum of w[c] * x[c]; unsigned so that a sum beyond int32 wraps
static inline int32_t dot_i8(const int8_t* w, const int8_t* x, uint32_t n) {
    uint32_t s0 = 0, s1 = 0, c = 0;
    for (; c + 4 <= n; c += 4) {
        s0 += static_cast<uint32_t>(w[c] * x[c]) + static_cast<uint32_t>(w[c + 2] * x[c + 2]);
        s1 += static_cast<uint32_t>(w[c + 1] * x[c + 1]) + static_cast<uint32_t>(w[c + 3] * x[c + 3]);
    }
    for (; c < n; c++) s0 += static_cast<uint32_t>(w[c] * x[c]);
    return static_cast<int32_t>(s0 + s1);
}

// The low and the high nibble of a byte as signed 4-bit values
static inline int32_t lo4(int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(b) << 28) >> 28; }
static inline int32_t hi4(int32_t b) { return b >> 4; }

static inline int32_t dot_i4(const int8_t* w, const int8_t* x, uint32_t n) {
    uint32_t s0 = 0, s1 = 0, j = 0;
    const uint32_t pairs = n / 2;
    for (; j + 2 <= pairs; j += 2) {
        const int32_t a = w[j], b = w[j + 1];
        s0 += static_cast<uint32_t>(lo4(a) * x[2 * j]) + static_cast<uint32_t>(lo4(b) * x[2 * j + 2]);
        s1 += static_cast<uint32_t>(hi4(a) * x[2 * j + 1]) + static_cast<uint32_t>(hi4(b) * x[2 * j + 3]);
    }
    for (; j < pairs; j++) {
        const int32_t a = w[j];
        s0 += static_cast<uint32_t>(lo4(a) * x[2 * j]);
        s1 += static_cast<uint32_t>(hi4(a) * x[2 * j + 1]);
    }
    if (n & 1) s0 += static_cast<uint32_t>(lo4(w[pairs]) * x[n - 1]);
    return static_cast<int32_t>(s0 + s1);
}

// (float)acc * (w_scale * x_scale): two roundings, in this order
void matvec_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, float x_scale,
               uint32_t rows, uint32_t cols) {
    for (uint32_t r = 0; r < rows; r++, w += row_stride) {
        const float acc = static_cast<float>(dot_i8(w, x, cols));
        store_f32(out + 4 * r, acc * (load_f32(w_scale + 4 * r) * x_scale));
    }
}

void matvec_i4(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, float x_scale,
               uint32_t rows, uint32_t cols) {
    for (uint32_t r = 0; r < rows; r++, w += row_stride) {
        const float acc = static_cast<float>(dot_i4(w, x, cols));
        store_f32(out + 4 * r, acc * (load_f32(w_scale + 4 * r) * x_scale));
    }
}

// ---- float products ------------------------------------------------------------

// Columns a float block holds: 256 bytes of the interpreter's native stack
constexpr uint32_t kBlock = 64;

// Column blocks of x; between blocks a row's partial sum waits in out, a
// float as exact as the one the loop holds
void matvec_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, float alpha, uint32_t rows, uint32_t cols) {
    float xb[kBlock];
    uint32_t c0 = 0;
    do {
        const uint32_t n = cols - c0 < kBlock ? cols - c0 : kBlock;
        __builtin_memcpy(xb, x + 4 * c0, 4 * n);
        const bool last = c0 + n == cols;
        const int8_t* row = w + c0;
        for (uint32_t r = 0; r < rows; r++, row += row_stride) {
            float d = c0 ? load_f32(out + 4 * r) : 0.0f;
            for (uint32_t c = 0; c < n; c++) d = d + xb[c] * static_cast<float>(row[c]);
            if (last) d = d * load_f32(w_scale + r * scale_stride) * alpha;
            store_f32(out + 4 * r, d);
        }
        c0 += n;
    } while (c0 < cols);
}

void vecmat_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, uint32_t rows, uint32_t cols) {
    float acc[kBlock];
    for (uint32_t c0 = 0; c0 < cols; c0 += kBlock) {
        const uint32_t n = cols - c0 < kBlock ? cols - c0 : kBlock;
        for (uint32_t c = 0; c < n; c++) acc[c] = 0.0f;
        const int8_t* row = w + c0;
        for (uint32_t r = 0; r < rows; r++, row += row_stride) {
            const float a = load_f32(x + 4 * r) * load_f32(w_scale + r * scale_stride);
            for (uint32_t c = 0; c < n; c++) acc[c] = acc[c] + a * static_cast<float>(row[c]);
        }
        __builtin_memcpy(out + 4 * c0, acc, 4 * n);
    }
}

void softmax_f32(uint8_t* x, uint32_t n) {
    if (n == 0) return;
    float m = load_f32(x);
    for (uint32_t i = 1; i < n; i++) {
        const float v = load_f32(x + 4 * i);
        if (v > m) m = v;
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float e = k32_expf(load_f32(x + 4 * i) - m);
        store_f32(x + 4 * i, e);
        sum = sum + e;
    }
    for (uint32_t i = 0; i < n; i++) store_f32(x + 4 * i, load_f32(x + 4 * i) / sum);
}

// ---- the imports -------------------------------------------------------------

// An operand outside the linear memory traps with a message that names it;
// the container reports it as an out-of-bounds access (exit code 139)
#define K32_OOB(fn, operand) "[trap] out of bounds memory access: " operand " of " fn

// The four matrix imports share one implementation; their link carries this
enum Kind : uint8_t { kI8, kI4, kF32, kVecmat };
struct MatrixImport {
    const char* name;
    const char* signature;
    Kind kind;
    const char* trap_out;
    const char* trap_w;
    const char* trap_w_scale;
    const char* trap_x;
};
#define K32_MATRIX(fn, sig, kind) {fn, sig, kind, K32_OOB(fn, "out"), K32_OOB(fn, "w"), K32_OOB(fn, "w_scale"), K32_OOB(fn, "x")}
static const MatrixImport kMatrixImports[] = {
    // out, w, w_scale, x, x_scale, rows, cols, row_stride
    K32_MATRIX("k32_matvec_i8", "i(iiiifiii)", kI8),
    K32_MATRIX("k32_matvec_i4", "i(iiiifiii)", kI4),
    // out, w, w_scale, x, alpha, rows, cols, row_stride, scale_stride
    K32_MATRIX("k32_matvec_f32_i8", "i(iiiifiiii)", kF32),
    // out, w, w_scale, x, rows, cols, row_stride, scale_stride
    K32_MATRIX("k32_vecmat_f32_i8", "i(iiiiiiii)", kVecmat),
};
static const char kTrapSoftmax[] = K32_OOB("k32_softmax_f32", "x");

static inline uint32_t arg(const uint64_t* sp, int i) { return static_cast<uint32_t>(sp[i]); }
static const void* result(uint64_t* sp, int32_t rc) {
    *reinterpret_cast<int32_t*>(sp) = rc;
    return m3Err_none;
}

// count items, stride bytes apart, of `last` bytes each from off lie inside
// size bytes. With 32-bit counts and strides (count - 1) x stride stays below
// 2^64, and every sum below is under 2^35 once it fits size.
static bool fits(uint64_t size, uint32_t off, uint32_t count, uint64_t stride, uint64_t last) {
    if (count == 0) return off <= size;
    const uint64_t span = (count - 1) * stride;
    return span <= size && off + span + last <= size;
}

// Pays for the work before it runs, as wasm3 charges a segment: a metered pod
// whose budget does not cover the call traps out of gas with nothing written
static M3Result charge(IM3Runtime rt, uint64_t units) {
#if d_m3HasGasMetering
    if (rt->gasLimit == 0) return m3Err_none;   // an unmetered pod
    rt->gasRemaining -= static_cast<int64_t>(linalg_gas(units));
    if (rt->gasRemaining < 0) return m3Err_trapOutOfGas;
#else
    (void)rt;
    (void)units;
#endif
    return m3Err_none;
}

static const void* raw_matrix(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) {
    const MatrixImport& im = *static_cast<const MatrixImport*>(ctx->userdata);
    const Kind kind = im.kind;
    const uint32_t out = arg(sp, 1), w = arg(sp, 2), w_scale = arg(sp, 3), x = arg(sp, 4);
    float f = 1.0f;   // x_scale or alpha
    if (kind != kVecmat) __builtin_memcpy(&f, &sp[5], sizeof f);
    const int a = kind == kVecmat ? 5 : 6;   // rows
    const uint32_t rows = arg(sp, a), cols = arg(sp, a + 1), row_arg = arg(sp, a + 2);
    const uint32_t scale_arg = kind >= kF32 ? arg(sp, a + 3) : 0;

    // A row's bytes; strides of 0 pack the rows and the scales
    const uint64_t row_bytes = kind == kI4 ? (static_cast<uint64_t>(cols) + 1) / 2 : cols;
    const uint64_t row_stride = row_arg ? row_arg : row_bytes;
    const uint64_t scale_stride = scale_arg ? scale_arg : 4;
    if (row_stride < row_bytes || scale_stride < 4) return result(sp, K32_ERR_INVAL);
    // out: rows floats (cols for x^T W); x: cols int8 values, or cols floats (rows for x^T W)
    const uint64_t size = m3_GetMemorySizeAt(mem);
    const uint64_t x_bytes = kind <= kI4 ? cols : 4ull * (kind == kVecmat ? rows : cols);
    if (!fits(size, out, kind == kVecmat ? cols : rows, 4, 4)) return im.trap_out;
    if (!fits(size, w, rows, row_stride, row_bytes)) return im.trap_w;
    if (!fits(size, w_scale, rows, scale_stride, 4)) return im.trap_w_scale;
    if (!fits(size, x, 1, 0, x_bytes)) return im.trap_x;
    if (M3Result r = charge(rt, matrix_units(rows, cols))) return r;

    uint8_t* base = static_cast<uint8_t*>(mem);
    const int8_t* wp = reinterpret_cast<const int8_t*>(base + w);
    const size_t rs = static_cast<size_t>(row_stride), ss = static_cast<size_t>(scale_stride);
    switch (kind) {
    case kI8: matvec_i8(base + out, wp, rs, base + w_scale, reinterpret_cast<const int8_t*>(base + x), f, rows, cols); break;
    case kI4: matvec_i4(base + out, wp, rs, base + w_scale, reinterpret_cast<const int8_t*>(base + x), f, rows, cols); break;
    case kF32: matvec_f32_i8(base + out, wp, rs, base + w_scale, ss, base + x, f, rows, cols); break;
    case kVecmat: vecmat_f32_i8(base + out, wp, rs, base + w_scale, ss, base + x, rows, cols); break;
    }
    return result(sp, K32_OK);
}

// "i(ii)": x, n
static const void* raw_softmax(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t x = arg(sp, 1), n = arg(sp, 2);
    if (!fits(m3_GetMemorySizeAt(mem), x, n, 4, 4)) return kTrapSoftmax;
    if (M3Result r = charge(rt, kSoftmaxUnits * n)) return r;
    softmax_f32(static_cast<uint8_t*>(mem) + x, n);
    return result(sp, K32_OK);
}

void link_linalg_host_functions(IM3Module module) {
    for (const MatrixImport& im : kMatrixImports) m3_LinkRawFunctionEx(module, "env", im.name, im.signature, &raw_matrix, &im);
    m3_LinkRawFunction(module, "env", "k32_softmax_f32", "i(ii)", &raw_softmax);
}

}  // namespace kube32::runtime
