// Native linear algebra for pods (include/runtime/pod_linalg.hpp).
//
// On the ESP32-S3 the integer products run on the core's vector unit (PIE,
// below). The other loops are plain C for the Xtensa LX7 core and any host:
// float vectors copied into blocks on the stack so that the loops load them
// aligned. The build compiles this file with -ffp-contract=off: a multiply
// followed by an add stays two roundings, as in a Wasm pod.
#include "runtime/pod_linalg.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_blobs.hpp"
#include "runtime/k32_app_abi.h"

#include "wasm3.h"
#include "m3_env.h"

#include <cstdint>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define K32_PIE 1
#else
#define K32_PIE 0
#endif

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

// The low and the high nibble of a byte as signed 4-bit values
static inline int32_t lo4(int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(b) << 28) >> 28; }
static inline int32_t hi4(int32_t b) { return b >> 4; }

#if K32_PIE
// The ESP32-S3's vector unit (PIE). EE.VMULAS.S8.ACCX adds the 16 products of
// two 128-bit registers of int8 values to the 40-bit accumulator ACCX, whose
// low 32 bits (ACCX_0) are the int32 sum wrapped as the scalar loops wrap it:
// a row fits the board's memory, so it has fewer than 2^25 columns and the
// exact sum fits 40 bits.
//
// EE.VLD.128.IP loads 16 bytes from a 16-byte aligned address. An operand
// that is not aligned streams through EE.LD.128.USAR.IP, which loads the
// aligned block that holds the address and keeps the offset in SAR_BYTE, and
// EE.SRC.Q, which takes the 16 bytes at that offset out of two consecutive
// blocks. Such a stream loads the blocks that hold its bytes and no others,
// so a row that ends at the end of a mapped blob is read safely. The vector
// registers belong to the calling task (FreeRTOS saves them as coprocessor
// state) and hold nothing between calls.

static inline bool aligned16(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 15) == 0; }

// ACCX += the products of 16 * blocks bytes of w and x (blocks > 0); WA, XA:
// w, x aligned
template <bool WA, bool XA>
static inline void pie_mac(const int8_t* w, const int8_t* x, uint32_t blocks) {
    if (WA && XA) {
        do asm volatile("ee.vld.128.ip q0, %0, 16\n\t"
                        "ee.vld.128.ip q1, %1, 16\n\t"
                        "ee.vmulas.s8.accx q0, q1" : "+r"(w), "+r"(x) : : "memory");
        while (--blocks);
    } else if (XA) {
        asm volatile("ee.ld.128.usar.ip q0, %0, 16" : "+r"(w) : : "memory");
        do asm volatile("ee.ld.128.usar.ip q1, %0, 16\n\t"
                        "ee.src.q.qup q2, q0, q1\n\t"
                        "ee.vld.128.ip q3, %1, 16\n\t"
                        "ee.vmulas.s8.accx q2, q3" : "+r"(w), "+r"(x) : : "memory");
        while (--blocks);
    } else if (WA) {
        asm volatile("ee.ld.128.usar.ip q0, %0, 16" : "+r"(x) : : "memory");
        do asm volatile("ee.ld.128.usar.ip q1, %1, 16\n\t"
                        "ee.src.q.qup q2, q0, q1\n\t"
                        "ee.vld.128.ip q3, %0, 16\n\t"
                        "ee.vmulas.s8.accx q2, q3" : "+r"(w), "+r"(x) : : "memory");
        while (--blocks);
    } else {
        // each EE.SRC.Q follows the load of its own stream, which sets SAR_BYTE
        asm volatile("ee.ld.128.usar.ip q0, %0, 16\n\t"
                     "ee.ld.128.usar.ip q3, %1, 16" : "+r"(w), "+r"(x) : : "memory");
        do asm volatile("ee.ld.128.usar.ip q1, %0, 16\n\t"
                        "ee.src.q.qup q2, q0, q1\n\t"
                        "ee.ld.128.usar.ip q4, %1, 16\n\t"
                        "ee.src.q.qup q5, q3, q4\n\t"
                        "ee.vmulas.s8.accx q2, q5" : "+r"(w), "+r"(x) : : "memory");
        while (--blocks);
    }
}

// Sum of w[c] * x[c]: 16 columns a step, the rest one at a time
static inline int32_t dot_i8(const int8_t* w, const int8_t* x, uint32_t n) {
    const uint32_t blocks = n / 16;
    uint32_t s = 0;
    if (blocks) {
        asm volatile("ee.zero.accx");
        if (aligned16(w)) aligned16(x) ? pie_mac<true, true>(w, x, blocks) : pie_mac<true, false>(w, x, blocks);
        else aligned16(x) ? pie_mac<false, true>(w, x, blocks) : pie_mac<false, false>(w, x, blocks);
        asm volatile("rur.accx_0 %0" : "=r"(s));
    }
    for (uint32_t c = 16 * blocks; c < n; c++) s += static_cast<uint32_t>(w[c] * x[c]);
    return static_cast<int32_t>(s);
}

static const uint8_t kNibble = 0x0F, kNibbleSign = 0x08, kNibbleBias = 0xF8;

// 16 * blocks bytes of int4 weights at w as 32 * blocks int8 values in column
// order at buf (aligned): ((b & 15) ^ 8) - 8 is the low nibble's value, the
// same of b >> 4 the high one's, and EE.VZIP.8 interleaves the two
template <bool WA>
static inline void pie_unpack_i4(const int8_t* w, int8_t* buf, uint32_t blocks) {
    asm volatile("ee.vldbc.8 q6, %0\n\t"
                 "ee.vldbc.8 q7, %1\n\t"
                 "ee.vldbc.8 q3, %2" : : "r"(&kNibble), "r"(&kNibbleSign), "r"(&kNibbleBias) : "memory");
    if (!WA) asm volatile("ee.ld.128.usar.ip q0, %0, 16" : "+r"(w) : : "memory");
    do {
        if (WA) asm volatile("ee.vld.128.ip q2, %0, 16" : "+r"(w) : : "memory");
        else asm volatile("ee.ld.128.usar.ip q1, %0, 16\n\t"
                          "ee.src.q.qup q2, q0, q1" : "+r"(w) : : "memory");
        // SAR is set here for EE.VSR.32: the compiler sets it anew before each shift it emits
        asm volatile("ee.andq q4, q2, q6\n\t"
                     "ee.xorq q4, q4, q7\n\t"
                     "ee.vadds.s8 q4, q4, q3\n\t"
                     "ssai 4\n\t"
                     "ee.vsr.32 q5, q2\n\t"
                     "ee.andq q5, q5, q6\n\t"
                     "ee.xorq q5, q5, q7\n\t"
                     "ee.vadds.s8 q5, q5, q3\n\t"
                     "ee.vzip.8 q4, q5\n\t"
                     "ee.vst.128.ip q4, %0, 16\n\t"
                     "ee.vst.128.ip q5, %0, 16" : "+r"(buf) : : "memory");
    } while (--blocks);
}

// Columns of an int4 row unpacked at a time, into a buffer on the stack
constexpr uint32_t kUnpackCols = 256;

// Columns c0..c0 + len of the int4 row at row (c0 even) as int8 values at buf
static inline void unpack_i4(const int8_t* row, uint32_t c0, uint32_t len, int8_t* buf) {
    const int8_t* p = row + c0 / 2;
    const uint32_t pairs = len / 2, blocks = pairs / 16;
    if (blocks) aligned16(p) ? pie_unpack_i4<true>(p, buf, blocks) : pie_unpack_i4<false>(p, buf, blocks);
    for (uint32_t j = 16 * blocks; j < pairs; j++) {
        const int32_t b = p[j];
        buf[2 * j] = static_cast<int8_t>(lo4(b));
        buf[2 * j + 1] = static_cast<int8_t>(hi4(b));
    }
    if (len & 1) buf[len - 1] = static_cast<int8_t>(lo4(p[pairs]));
}

// The sums of an int4 row with g vectors x + i * cols, the row unpacked once
// for all of them
static inline void dots_i4(const int8_t* row, const int8_t* x, uint32_t cols, uint32_t g, uint32_t* acc) {
    alignas(16) int8_t buf[kUnpackCols];
    for (uint32_t i = 0; i < g; i++) acc[i] = 0;
    for (uint32_t c0 = 0; c0 < cols; c0 += kUnpackCols) {
        const uint32_t len = cols - c0 < kUnpackCols ? cols - c0 : kUnpackCols;
        unpack_i4(row, c0, len, buf);
        for (uint32_t i = 0; i < g; i++) acc[i] += static_cast<uint32_t>(dot_i8(buf, x + static_cast<size_t>(i) * cols + c0, len));
    }
}

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
        uint32_t acc;
        dots_i4(w, x, cols, 1, &acc);
        store_f32(out + 4 * r, static_cast<float>(static_cast<int32_t>(acc)) * (load_f32(w_scale + 4 * r) * x_scale));
    }
}

// Row after row, each applied to every vector: an int8 row from the cache
// after the first, an int4 row unpacked once for kGroup vectors
template <bool I4>
static void matmul(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
                   uint32_t rows, uint32_t cols, uint32_t n) {
    constexpr uint32_t kGroup = 8;
    for (uint32_t r = 0; r < rows; r++, w += row_stride) {
        const float ws = load_f32(w_scale + 4 * r);
        for (uint32_t p = 0; p < n; p += kGroup) {
            const uint32_t g = n - p < kGroup ? n - p : kGroup;
            const int8_t* xp = x + static_cast<size_t>(p) * cols;
            uint32_t acc[kGroup];
            if (I4) dots_i4(w, xp, cols, g, acc);
            else for (uint32_t i = 0; i < g; i++) acc[i] = static_cast<uint32_t>(dot_i8(w, xp + static_cast<size_t>(i) * cols, cols));
            for (uint32_t i = 0; i < g; i++) {
                const float acc_f = static_cast<float>(static_cast<int32_t>(acc[i]));
                store_f32(out + 4 * (static_cast<size_t>(p + i) * rows + r), acc_f * (ws * load_f32(x_scale + 4 * (p + i))));
            }
        }
    }
}

#else   // K32_PIE

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

// ---- products for several vectors ---------------------------------------------

// The sums of one row with G vectors (x + g * cols), each weight loaded and
// unpacked once: the G sums and the G vector pointers stay in registers
template <uint32_t G>
static inline void dots_i8(const int8_t* w, const int8_t* x, uint32_t cols, uint32_t* acc) {
    uint32_t a[G] = {};
    for (uint32_t c = 0; c < cols; c++) {
        const int32_t wv = w[c];
        for (uint32_t g = 0; g < G; g++) a[g] += static_cast<uint32_t>(wv * x[g * cols + c]);
    }
    for (uint32_t g = 0; g < G; g++) acc[g] = a[g];
}

template <uint32_t G>
static inline void dots_i4(const int8_t* w, const int8_t* x, uint32_t cols, uint32_t* acc) {
    uint32_t a[G] = {};
    const uint32_t pairs = cols / 2;
    for (uint32_t j = 0; j < pairs; j++) {
        const int32_t b = w[j], lo = lo4(b), hi = hi4(b);
        for (uint32_t g = 0; g < G; g++)
            a[g] += static_cast<uint32_t>(lo * x[g * cols + 2 * j]) + static_cast<uint32_t>(hi * x[g * cols + 2 * j + 1]);
    }
    if (cols & 1) {
        const int32_t lo = lo4(w[pairs]);
        for (uint32_t g = 0; g < G; g++) a[g] += static_cast<uint32_t>(lo * x[g * cols + cols - 1]);
    }
    for (uint32_t g = 0; g < G; g++) acc[g] = a[g];
}

// Four vectors: written out, since the compiler keeps an array of four sums
// on the stack
static inline void dots4_i8(const int8_t* w, const int8_t* x, uint32_t cols, uint32_t* acc) {
    const int8_t *x0 = x, *x1 = x + cols, *x2 = x + 2 * cols, *x3 = x + 3 * cols;
    uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (uint32_t c = 0; c < cols; c++) {
        const int32_t wv = w[c];
        a0 += static_cast<uint32_t>(wv * x0[c]);
        a1 += static_cast<uint32_t>(wv * x1[c]);
        a2 += static_cast<uint32_t>(wv * x2[c]);
        a3 += static_cast<uint32_t>(wv * x3[c]);
    }
    acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
}

static inline void dots4_i4(const int8_t* w, const int8_t* x, uint32_t cols, uint32_t* acc) {
    const int8_t *x0 = x, *x1 = x + cols, *x2 = x + 2 * cols, *x3 = x + 3 * cols;
    uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    const uint32_t pairs = cols / 2;
    for (uint32_t j = 0; j < pairs; j++, x0 += 2, x1 += 2, x2 += 2, x3 += 2) {
        const int32_t b = w[j], lo = lo4(b), hi = hi4(b);
        a0 += static_cast<uint32_t>(lo * x0[0]) + static_cast<uint32_t>(hi * x0[1]);
        a1 += static_cast<uint32_t>(lo * x1[0]) + static_cast<uint32_t>(hi * x1[1]);
        a2 += static_cast<uint32_t>(lo * x2[0]) + static_cast<uint32_t>(hi * x2[1]);
        a3 += static_cast<uint32_t>(lo * x3[0]) + static_cast<uint32_t>(hi * x3[1]);
    }
    if (cols & 1) {
        const int32_t lo = lo4(w[pairs]);
        a0 += static_cast<uint32_t>(lo * x0[0]);
        a1 += static_cast<uint32_t>(lo * x1[0]);
        a2 += static_cast<uint32_t>(lo * x2[0]);
        a3 += static_cast<uint32_t>(lo * x3[0]);
    }
    acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
}

// Row after row; the groups of a row (4, then 2, then 1 vector) read it
// from the cache after the first
template <bool I4>
static void matmul(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
                   uint32_t rows, uint32_t cols, uint32_t n) {
    static_assert(kMatmulGroup == 4, "dots4");
    for (uint32_t r = 0; r < rows; r++, w += row_stride) {
        const float ws = load_f32(w_scale + 4 * r);
        for (uint32_t p = 0; p < n;) {
            uint32_t acc[kMatmulGroup];
            const uint32_t g = n - p >= 4 ? 4 : n - p >= 2 ? 2 : 1;
            const int8_t* xp = x + static_cast<size_t>(p) * cols;
            if (g == 4) I4 ? dots4_i4(w, xp, cols, acc) : dots4_i8(w, xp, cols, acc);
            else if (g == 2) I4 ? dots_i4<2>(w, xp, cols, acc) : dots_i8<2>(w, xp, cols, acc);
            else I4 ? dots_i4<1>(w, xp, cols, acc) : dots_i8<1>(w, xp, cols, acc);
            for (uint32_t i = 0; i < g; i++) {
                const float acc_f = static_cast<float>(static_cast<int32_t>(acc[i]));
                store_f32(out + 4 * (static_cast<size_t>(p + i) * rows + r), acc_f * (ws * load_f32(x_scale + 4 * (p + i))));
            }
            p += g;
        }
    }
}

#endif  // K32_PIE

void matmul_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
               uint32_t rows, uint32_t cols, uint32_t n) {
    matmul<false>(out, w, row_stride, w_scale, x, x_scale, rows, cols, n);
}

void matmul_i4(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
               uint32_t rows, uint32_t cols, uint32_t n) {
    matmul<true>(out, w, row_stride, w_scale, x, x_scale, rows, cols, n);
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

// ---- element-wise operations -------------------------------------------------------

void rmsnorm_f32(uint8_t* out, const uint8_t* x, const uint8_t* w, uint32_t n, uint32_t count, float eps) {
    if (n == 0) return;
    for (uint32_t p = 0; p < count; p++, x += 4 * n, out += 4 * n) {
        float ss = 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            const float v = load_f32(x + 4 * i);
            ss = ss + v * v;
        }
        ss = ss / static_cast<float>(n);
        ss = ss + eps;
        ss = 1.0f / __builtin_sqrtf(ss);
        for (uint32_t i = 0; i < n; i++) store_f32(out + 4 * i, load_f32(w + 4 * i) * (ss * load_f32(x + 4 * i)));
    }
}

void quantize_i8(int8_t* q, uint8_t* scale, const uint8_t* x, uint32_t n, uint32_t count) {
    for (uint32_t p = 0; p < count; p++, x += 4 * n, q += n) {
        float amax = 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            const float a = __builtin_fabsf(load_f32(x + 4 * i));
            if (a > amax) amax = a;
        }
        const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
        for (uint32_t i = 0; i < n; i++) {
            const float t = load_f32(x + 4 * i) * inv;
            int32_t r;
            if (!(t == t)) r = 0;                // NaN
            else if (t >= 127.0f) r = 127;       // also an infinite inv
            else if (t <= -127.0f) r = -127;
            else r = static_cast<int32_t>(t + (t >= 0.0f ? 0.5f : -0.5f));
            q[i] = static_cast<int8_t>(r > 127 ? 127 : r < -127 ? -127 : r);
        }
        store_f32(scale + 4 * p, amax / 127.0f);
    }
}

void rope_f32(uint8_t* x, const uint8_t* cs, uint32_t n, uint32_t head_size, uint32_t count) {
    for (uint32_t p = 0; p < count; p++, x += 4 * n, cs += 4 * head_size) {
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            const uint32_t j = i % head_size;
            const float c = load_f32(cs + 4 * j), s = load_f32(cs + 4 * j + 4);
            const float a0 = load_f32(x + 4 * i), a1 = load_f32(x + 4 * i + 4);
            store_f32(x + 4 * i, a0 * c - a1 * s);
            store_f32(x + 4 * i + 4, a0 * s + a1 * c);
        }
    }
}

void swiglu_f32(uint8_t* x, const uint8_t* g, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float a = load_f32(x + 4 * i);
        a = a * (1.0f / (1.0f + k32_expf(-a)));
        store_f32(x + 4 * i, a * load_f32(g + 4 * i));
    }
}

void add_f32(uint8_t* x, const uint8_t* y, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) store_f32(x + 4 * i, load_f32(x + 4 * i) + load_f32(y + 4 * i));
}

uint32_t argmax_f32(const uint8_t* x, uint32_t n) {
    uint32_t best = 0;
    float top = n ? load_f32(x) : 0.0f;
    for (uint32_t i = 1; i < n; i++) {
        const float v = load_f32(x + 4 * i);
        if (v > top) { top = v; best = i; }
    }
    return best;
}

// ---- the imports -------------------------------------------------------------

// An operand outside the linear memory traps with a message that names it;
// the container reports it as an out-of-bounds access (exit code 139)
#define K32_OOB(fn, operand) "[trap] out of bounds memory access: " operand " of " fn

// A blob address that names no open blob, or runs past the blob's end
#define K32_BLOB_OOB(fn, operand) K32_OOB(fn, operand) " (a blob address outside every open blob)"

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
    const char* trap_blob_w;
    const char* trap_blob_w_scale;
};
#define K32_MATRIX(fn, sig, kind) \
    {fn, sig, kind, K32_OOB(fn, "out"), K32_OOB(fn, "w"), K32_OOB(fn, "w_scale"), K32_OOB(fn, "x"), K32_BLOB_OOB(fn, "w"), K32_BLOB_OOB(fn, "w_scale")}
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

// A read-only operand (weights, scales): count items stride bytes apart, the
// last `last` bytes long, in linear memory or, for a blob address
// (K32_BLOB_ADDR, never a linear memory address), in a blob the pod has open.
// Null with rc K32_OK when it lies outside either (the caller traps), or
// with rc K32_ERR_NOTSUP for a blob that is not mapped.
static const uint8_t* operand(IM3Runtime rt, uint8_t* base, uint64_t size, uint32_t addr, uint32_t count, uint64_t stride,
                              uint64_t last, bool& blob, int32_t& rc) {
    rc = K32_OK;
    blob = (addr & K32_BLOB_ADDR_FLAG) != 0 && addr >= size;
    if (!blob) return fits(size, addr, count, stride, last) ? base + addr : nullptr;
    const uint64_t span = count ? (count - 1) * stride + last : 0;
    const uint8_t* p = PodBlobs::instance().view(app_pod_id(rt), (addr >> K32_BLOB_HANDLE_SHIFT) & 7, addr & K32_BLOB_OFFSET_MASK, span, rc);
    if (!p && rc != K32_ERR_NOTSUP) rc = K32_OK;
    return p;
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
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!fits(size, out, kind == kVecmat ? cols : rows, 4, 4)) return im.trap_out;
    bool w_blob = false, s_blob = false;
    int32_t rc = K32_OK;
    const uint8_t* wb = operand(rt, base, size, w, rows, row_stride, row_bytes, w_blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!wb) return w_blob ? im.trap_blob_w : im.trap_w;
    const uint8_t* sb = operand(rt, base, size, w_scale, rows, scale_stride, 4, s_blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!sb) return s_blob ? im.trap_blob_w_scale : im.trap_w_scale;
    if (!fits(size, x, 1, 0, x_bytes)) return im.trap_x;
    if (M3Result r = charge(rt, matrix_units(rows, cols))) return r;

    const int8_t* wp = reinterpret_cast<const int8_t*>(wb);
    const size_t rs = static_cast<size_t>(row_stride), ss = static_cast<size_t>(scale_stride);
    switch (kind) {
    case kI8: matvec_i8(base + out, wp, rs, sb, reinterpret_cast<const int8_t*>(base + x), f, rows, cols); break;
    case kI4: matvec_i4(base + out, wp, rs, sb, reinterpret_cast<const int8_t*>(base + x), f, rows, cols); break;
    case kF32: matvec_f32_i8(base + out, wp, rs, sb, ss, base + x, f, rows, cols); break;
    case kVecmat: vecmat_f32_i8(base + out, wp, rs, sb, ss, base + x, rows, cols); break;
    }
    return result(sp, K32_OK);
}

// "i(iiiiiiiii)": out, w, w_scale, x, x_scale, rows, cols, row_stride, n
struct MatmulImport {
    const char* name;
    const char* signature;
    bool i4;
    const char* trap_out;
    const char* trap_w;
    const char* trap_w_scale;
    const char* trap_x;
    const char* trap_x_scale;
    const char* trap_blob_w;
    const char* trap_blob_w_scale;
};
#define K32_MATMUL(fn, sig, i4) \
    {fn, sig, i4, K32_OOB(fn, "out"), K32_OOB(fn, "w"), K32_OOB(fn, "w_scale"), K32_OOB(fn, "x"), K32_OOB(fn, "x_scale"), \
     K32_BLOB_OOB(fn, "w"), K32_BLOB_OOB(fn, "w_scale")}
static const MatmulImport kMatmulImports[] = {
    K32_MATMUL("k32_matmul_i8", "i(iiiiiiiii)", false),
    K32_MATMUL("k32_matmul_i4", "i(iiiiiiiii)", true),
};

static const void* raw_matmul(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) {
    const MatmulImport& im = *static_cast<const MatmulImport*>(ctx->userdata);
    const uint32_t out = arg(sp, 1), w = arg(sp, 2), w_scale = arg(sp, 3), x = arg(sp, 4), x_scale = arg(sp, 5);
    const uint32_t rows = arg(sp, 6), cols = arg(sp, 7), row_arg = arg(sp, 8), n = arg(sp, 9);
    const uint64_t row_bytes = im.i4 ? (static_cast<uint64_t>(cols) + 1) / 2 : cols;
    const uint64_t row_stride = row_arg ? row_arg : row_bytes;
    if (row_stride < row_bytes) return result(sp, K32_ERR_INVAL);
    // out: n vectors of rows floats; x: n vectors of cols values; x_scale: n floats
    const uint64_t size = m3_GetMemorySizeAt(mem);
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!fits(size, out, 1, 0, 4ull * n * rows)) return im.trap_out;
    bool w_blob = false, s_blob = false;
    int32_t rc = K32_OK;
    const uint8_t* wb = operand(rt, base, size, w, rows, row_stride, row_bytes, w_blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!wb) return w_blob ? im.trap_blob_w : im.trap_w;
    const uint8_t* sb = operand(rt, base, size, w_scale, rows, 4, 4, s_blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!sb) return s_blob ? im.trap_blob_w_scale : im.trap_w_scale;
    if (!fits(size, x, 1, 0, static_cast<uint64_t>(n) * cols)) return im.trap_x;
    if (!fits(size, x_scale, 1, 0, 4ull * n)) return im.trap_x_scale;
    if (M3Result r = charge(rt, n * matrix_units(rows, cols))) return r;

    const int8_t* wp = reinterpret_cast<const int8_t*>(wb);
    const int8_t* xp = reinterpret_cast<const int8_t*>(base + x);
    (im.i4 ? matmul_i4 : matmul_i8)(base + out, wp, static_cast<size_t>(row_stride), sb, xp, base + x_scale, rows, cols, n);
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

// ---- element-wise imports: float vectors in linear memory -----------------------

static const char kTrapNormOut[] = K32_OOB("k32_rmsnorm_f32", "out");
static const char kTrapNormX[] = K32_OOB("k32_rmsnorm_f32", "x");
static const char kTrapNormW[] = K32_OOB("k32_rmsnorm_f32", "w");
static const char kTrapNormBlobW[] = K32_BLOB_OOB("k32_rmsnorm_f32", "w");
static const char kTrapQuantQ[] = K32_OOB("k32_quantize_i8", "q");
static const char kTrapQuantScale[] = K32_OOB("k32_quantize_i8", "scale");
static const char kTrapQuantX[] = K32_OOB("k32_quantize_i8", "x");
static const char kTrapRopeX[] = K32_OOB("k32_rope_f32", "x");
static const char kTrapRopeCs[] = K32_OOB("k32_rope_f32", "cs");
static const char kTrapRopeBlobCs[] = K32_BLOB_OOB("k32_rope_f32", "cs");
static const char kTrapSwigluX[] = K32_OOB("k32_swiglu_f32", "x");
static const char kTrapSwigluG[] = K32_OOB("k32_swiglu_f32", "g");
static const char kTrapAddX[] = K32_OOB("k32_add_f32", "x");
static const char kTrapAddY[] = K32_OOB("k32_add_f32", "y");
static const char kTrapArgmax[] = K32_OOB("k32_argmax_f32", "x");

// bytes bytes at off lie inside the linear memory
static bool inside(uint64_t size, uint32_t off, uint64_t bytes) { return fits(size, off, 1, 0, bytes); }

// "i(iiiiif)": out, x, w, n, count, eps
static const void* raw_rmsnorm(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t out = arg(sp, 1), x = arg(sp, 2), w = arg(sp, 3), n = arg(sp, 4), count = arg(sp, 5);
    float eps;
    __builtin_memcpy(&eps, &sp[6], sizeof eps);
    const uint64_t size = m3_GetMemorySizeAt(mem), bytes = 4ull * n * count;
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!inside(size, out, bytes)) return kTrapNormOut;
    if (!inside(size, x, bytes)) return kTrapNormX;
    bool blob = false;
    int32_t rc = K32_OK;
    const uint8_t* wb = operand(rt, base, size, w, 1, 0, 4ull * n, blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!wb) return blob ? kTrapNormBlobW : kTrapNormW;
    if (M3Result r = charge(rt, kRmsnormUnits * n * count)) return r;
    rmsnorm_f32(base + out, base + x, wb, n, count, eps);
    return result(sp, K32_OK);
}

// "i(iiiii)": q, scale, x, n, count
static const void* raw_quantize(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t q = arg(sp, 1), scale = arg(sp, 2), x = arg(sp, 3), n = arg(sp, 4), count = arg(sp, 5);
    const uint64_t size = m3_GetMemorySizeAt(mem), values = static_cast<uint64_t>(n) * count;
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!inside(size, q, values)) return kTrapQuantQ;
    if (!inside(size, scale, 4ull * count)) return kTrapQuantScale;
    if (!inside(size, x, 4 * values)) return kTrapQuantX;
    if (M3Result r = charge(rt, kQuantizeUnits * values)) return r;
    quantize_i8(reinterpret_cast<int8_t*>(base + q), base + scale, base + x, n, count);
    return result(sp, K32_OK);
}

// "i(iiiii)": x, cs, n, head_size, count
static const void* raw_rope(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t x = arg(sp, 1), cs = arg(sp, 2), n = arg(sp, 3), hs = arg(sp, 4), count = arg(sp, 5);
    if ((n & 1) || (hs & 1) || hs == 0) return result(sp, K32_ERR_INVAL);
    const uint64_t size = m3_GetMemorySizeAt(mem);
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!inside(size, x, 4ull * n * count)) return kTrapRopeX;
    bool blob = false;
    int32_t rc = K32_OK;
    const uint8_t* cb = operand(rt, base, size, cs, 1, 0, 4ull * hs * count, blob, rc);
    if (rc == K32_ERR_NOTSUP) return result(sp, rc);
    if (!cb) return blob ? kTrapRopeBlobCs : kTrapRopeCs;
    if (M3Result r = charge(rt, kRopeUnits * n * count)) return r;
    rope_f32(base + x, cb, n, hs, count);
    return result(sp, K32_OK);
}

// "i(iii)": x, g or y, n
template <bool Swiglu>
static const void* raw_pairwise(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t x = arg(sp, 1), y = arg(sp, 2), n = arg(sp, 3);
    const uint64_t size = m3_GetMemorySizeAt(mem);
    uint8_t* base = static_cast<uint8_t*>(mem);
    if (!inside(size, x, 4ull * n)) return Swiglu ? kTrapSwigluX : kTrapAddX;
    if (!inside(size, y, 4ull * n)) return Swiglu ? kTrapSwigluG : kTrapAddY;
    if (M3Result r = charge(rt, (Swiglu ? kSwigluUnits : kAddUnits) * n)) return r;
    (Swiglu ? swiglu_f32 : add_f32)(base + x, base + y, n);
    return result(sp, K32_OK);
}

// "i(ii)": x, n
static const void* raw_argmax(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t x = arg(sp, 1), n = arg(sp, 2);
    if (!inside(m3_GetMemorySizeAt(mem), x, 4ull * n)) return kTrapArgmax;
    if (M3Result r = charge(rt, kArgmaxUnits * n)) return r;
    return result(sp, static_cast<int32_t>(argmax_f32(static_cast<uint8_t*>(mem) + x, n)));
}

void link_linalg_host_functions(IM3Module module) {
    for (const MatrixImport& im : kMatrixImports) m3_LinkRawFunctionEx(module, "env", im.name, im.signature, &raw_matrix, &im);
    for (const MatmulImport& im : kMatmulImports) m3_LinkRawFunctionEx(module, "env", im.name, im.signature, &raw_matmul, &im);
    m3_LinkRawFunction(module, "env", "k32_softmax_f32", "i(ii)", &raw_softmax);
    m3_LinkRawFunction(module, "env", "k32_rmsnorm_f32", "i(iiiiif)", &raw_rmsnorm);
    m3_LinkRawFunction(module, "env", "k32_quantize_i8", "i(iiiii)", &raw_quantize);
    m3_LinkRawFunction(module, "env", "k32_rope_f32", "i(iiiii)", &raw_rope);
    m3_LinkRawFunction(module, "env", "k32_swiglu_f32", "i(iii)", &raw_pairwise<true>);
    m3_LinkRawFunction(module, "env", "k32_add_f32", "i(iii)", &raw_pairwise<false>);
    m3_LinkRawFunction(module, "env", "k32_argmax_f32", "i(ii)", &raw_argmax);
}

}  // namespace kube32::runtime
