#pragma once

// Native linear algebra for pods: the k32_matvec_*, k32_vecmat_f32_i8 and
// k32_softmax_f32 imports (include/runtime/k32_app_abi.h). A pod passes
// quantized matrices and vectors in its linear memory, and the node computes
// the products natively.
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
// one per output row; a softmax is kSoftmaxUnits per element (its exp).
constexpr uint64_t kLinalgGasPerCall = 10000;
constexpr uint64_t kLinalgGasPerUnit = 45;
constexpr uint64_t kSoftmaxUnits = 32;
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

// out[r] = ((sum over c of x[c] * (float)w[r][c]) * w_scale[r]) * alpha, the
// sum from 0 in column order. Scale r is scale_stride bytes after scale r - 1.
void matvec_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, float alpha, uint32_t rows, uint32_t cols);

// out[c] = sum over r of (x[r] * w_scale[r]) * (float)w[r][c], from 0 in row order
void vecmat_f32_i8(uint8_t* out, const int8_t* w, size_t row_stride, const uint8_t* w_scale, size_t scale_stride,
                   const uint8_t* x, uint32_t rows, uint32_t cols);

// x = e^(x - max x) / their sum, with k32_expf and the sum in order
void softmax_f32(uint8_t* x, uint32_t n);

// Links the imports into a pod's module
void link_linalg_host_functions(IM3Module module);

}  // namespace kube32::runtime
