#pragma once

// Native linear algebra for pods: the k32_matvec_*, k32_matmul_*, k32_vecmat_f32_i8,
// k32_softmax_f32 and element-wise imports (include/runtime/k32_app_abi.h). A
// pod passes quantized matrices and vectors in its linear memory, and the node
// computes the products natively.
//
// Integer sums are exact (int32, wrapping modulo 2^32). Every float step is
// one IEEE binary32 operation in a fixed order, and this file's
// implementation is built without fused multiply-adds (-ffp-contract=off):
// a board and kube32-sim compute the same bits. Float operands may be
// unaligned; they are passed as bytes.

#include <cstddef>
#include <cstdint>

typedef struct M3Module* IM3Module;

namespace kube32::runtime {

// Gas a metered pod pays for a call: what a bulk memory instruction costs,
// plus one arithmetic instruction per unit of work (wasm3's c_gasHeavy and
// c_gasArith). A matrix call is rows x (cols + 1) units, a weight each and
// one per output row; a softmax is kSoftmaxUnits per element (its exp), and
// the element-wise calls the units below per element.
constexpr uint64_t kLinalgGasPerCall = 10000;
constexpr uint64_t kLinalgGasPerUnit = 45;
constexpr uint64_t kSoftmaxUnits = 32;
constexpr uint64_t kRmsnormUnits = 4;
constexpr uint64_t kQuantizeUnits = 4;
constexpr uint64_t kRopeUnits = 3;
constexpr uint64_t kSwigluUnits = 32;
constexpr uint64_t kAddUnits = 1;
constexpr uint64_t kArgmaxUnits = 1;
constexpr uint64_t linalg_gas(uint64_t units) { return kLinalgGasPerCall + kLinalgGasPerUnit * units; }
constexpr uint64_t matrix_units(uint64_t rows, uint64_t cols) { return rows * (cols + 1); }

// out[r] = (float)(sum over c of w[r][c] * x[c]) * (w_scale[r] * x_scale).
// Row r of w starts row_stride bytes after row r - 1. int8 weights; for
// int4, two weights a byte, column 2j in the low nibble and 2j + 1 in the
// high one, each a signed 4-bit value.
void matvec_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, float x_scale,
               uint32_t rows, uint32_t cols);
void matvec_i4(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, float x_scale,
               uint32_t rows, uint32_t cols);

// The same products for n vectors at once: x holds n vectors of cols int8
// values one after another and x_scale n floats, and
// out[p * rows + r] = (float)(sum over c of w[r][c] * x[p * cols + c]) * (w_scale[r] * x_scale[p]),
// the bits of n matvec calls. Each row of w is read once for kMatmulGroup
// vectors (from the weights' memory once for all n: the next group finds it
// in the cache).
constexpr uint32_t kMatmulGroup = 4;
void matmul_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
               uint32_t rows, uint32_t cols, uint32_t n);
void matmul_i4(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, const int8_t* x, const uint8_t* x_scale,
               uint32_t rows, uint32_t cols, uint32_t n);

// out[r] = ((sum over c of x[c] * (float)w[r][c]) * w_scale[r]) * alpha, the
// sum from 0 in column order. Scale r is scale_stride bytes after scale r - 1.
void matvec_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, float alpha, uint32_t rows, uint32_t cols);

// out[c] = sum over r of (x[r] * w_scale[r]) * (float)w[r][c], from 0 in row order
void vecmat_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, uint32_t rows, uint32_t cols);

// x = e^(x - max x) / their sum, with k32_expf and the sum in order
void softmax_f32(uint8_t* x, uint32_t n);

// ---- element-wise operations over count vectors of n floats ----

// out = w * (x * r) with r = 1 / sqrt(sum of x^2 / n + eps), the sum from 0 in
// order, for each of count vectors; out may be x
void rmsnorm_f32(uint8_t* out, const uint8_t* x, const uint8_t* w, uint32_t n, uint32_t count, float eps);

// q = round(x * (127 / max |x|)), half away from zero, clamped to -127..127
// (0 for NaN), and scale = max |x| / 127, for each of count vectors: q holds
// count x n values, scale count floats. A vector of zeros gets q 0, scale 0.
void quantize_i8(int8_t* q, uint8_t* scale, const uint8_t* x, uint32_t n, uint32_t count);

// The rotary embedding of count vectors of n floats in place: vector p turns
// each pair (x[i], x[i + 1]) by row p of cs, head_size floats of (cos, sin)
// pairs, taken at i % head_size. n and head_size are even.
void rope_f32(uint8_t* x, const uint8_t* cs, uint32_t n, uint32_t head_size, uint32_t count);

// x = (x * (1 / (1 + k32_expf(-x)))) * g, the gate of a SwiGLU feed-forward block
void swiglu_f32(uint8_t* x, const uint8_t* g, uint32_t n);

// x = x + y
void add_f32(uint8_t* x, const uint8_t* y, uint32_t n);

// The index of the first largest of n floats (0 for n = 0)
uint32_t argmax_f32(const uint8_t* x, uint32_t n);

// Links the imports into a pod's module
void link_linalg_host_functions(IM3Module module);

}  // namespace kube32::runtime
