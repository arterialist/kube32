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
constexpr uint64_t kAddI8Units = 2;
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
// the bits of n matvec calls. Each row of w is read from the weights' memory
// once for all n vectors (kMatmulGroup vectors a group on a host).
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

// Attention of n query positions pos0 .. pos0 + n - 1 over an int8 KV cache:
// for position p and head h (KV head h / (heads / kvh)), the scores of q
// (heads x hs floats a position) against key rows 0 .. p (row r at
// kq + r x kv_stride + kh x hs, scale ks[r x kvh + kh]) as matvec_f32_i8
// with alpha = scale into att, softmax_f32 over them, and the weighted sum
// of the value rows (vq, vs, the same layout) as vecmat_f32_i8 into head h
// of out: the bits of those three calls per head. att holds pos0 + n floats.
void attention_f32_i8(uint8_t* out, const int8_t* kq, const uint8_t* ks, const int8_t* vq, const uint8_t* vs, const uint8_t* q,
                      float scale, uint32_t heads, uint32_t kvh, uint32_t hs, size_t kv_stride, uint32_t pos0, uint32_t n, uint8_t* att);
// Its units of work: the two products and the softmax of every head of every position
constexpr uint64_t attention_units(uint64_t heads, uint64_t hs, uint64_t pos0, uint64_t n) {
    uint64_t u = 0;
    for (uint64_t i = 0; i < n; i++) u += heads * (2 * matrix_units(pos0 + i + 1, hs) + kSoftmaxUnits * (pos0 + i + 1));
    return u;
}

// The same with every one of the n positions attending to key rows 0 ..
// rows - 1 (a window every position sees whole): the bits of the three
// calls per head over rows rows. att holds rows floats.
void attention_full_f32_i8(uint8_t* out, const int8_t* kq, const uint8_t* ks, const int8_t* vq, const uint8_t* vs, const uint8_t* q,
                           float scale, uint32_t heads, uint32_t kvh, uint32_t hs, size_t kv_stride, uint32_t rows, uint32_t n, uint8_t* att);
constexpr uint64_t attention_full_units(uint64_t heads, uint64_t hs, uint64_t rows, uint64_t n) {
    return n * heads * (2 * matrix_units(rows, hs) + kSoftmaxUnits * rows);
}

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

// x = x + (float)q * scale, a residual from an int8 map
void add_f32_i8(uint8_t* x, const int8_t* q, float scale, uint32_t n);

// q = round(x * inv), half away from zero, clamped to -127..127 (0 for NaN):
// quantization at a fixed scale
void quantize_i8_static(int8_t* q, const uint8_t* x, float inv, uint32_t n);

// A 3x3 convolution with padding 1 over n int8 maps of h x w pixels of cin
// channels (channels innermost), read as their 2x nearest-neighbour upsample
// when up is 1: output pixel (y, x) takes input pixels stride * y + ky - 1
// and stride * x + kx - 1 of the (h << up) x (w << up) map (zero outside it)
// for ky, kx in 0 .. 2. Output rows y0 .. y1 - 1 of the ho x wo map (the
// input's side << up, divided by stride, rounded up): out holds n x (y1 - y0)
// x wo pixels of cout floats, pixel after pixel,
//   out[o] = (float)(sum over ky, kx, c of w[o][ky][kx][c] * in[..][c]) * (w_scale[o] * in_scale) + bias[o]
// with w a cout x 9 cin matrix: the bits of k32_matvec_i8 over the pixel's
// patch, then one float add of the bias (none when bias is null). The weights
// are read once per call in blocks that fit the cache; cin is at most
// kConvMaxCin.
constexpr uint32_t kConvMaxCin = 256;
void conv3x3_i8(uint8_t* out, const int8_t* w, const uint8_t* w_scale, const uint8_t* bias, const int8_t* in, float in_scale,
                uint32_t cin, uint32_t cout, uint32_t h, uint32_t w_, uint32_t stride, uint32_t up, uint32_t n, uint32_t y0,
                uint32_t y1);
// The output map's side for an input side of s
constexpr uint32_t conv3x3_out(uint32_t s, uint32_t stride, uint32_t up) { return ((s << up) + stride - 1) / stride; }

// The index of the first largest of n floats (0 for n = 0)
uint32_t argmax_f32(const uint8_t* x, uint32_t n);
// The radix-2 decimation-in-time butterflies of an n-point FFT (n a power of
// two) in place over re and im (int32, in bit-reversed order), with the n / 2
// Q15 twiddles tw_re, tw_im (int16): at size 2, 4, .., n, butterfly (a, b =
// a + size / 2) with twiddle k = (a % size) x n / size takes t = W x[b], each
// part an int64 sum of products rounded as (p + 16384) >> 15, then x[b] =
// x[a] - t and x[a] = x[a] + t in int32. No scaling between stages. Operands
// may be unaligned.
void fft_q15(uint8_t* re, uint8_t* im, uint32_t n, const uint8_t* tw_re, const uint8_t* tw_im);
// A call's units of work: n / 2 x log2 n butterflies, kFftUnits each
constexpr uint64_t kFftUnits = 8;

// Links the imports into a pod's module
void link_linalg_host_functions(IM3Module module);

}  // namespace kube32::runtime
