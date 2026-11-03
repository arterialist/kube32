// The linear algebra imports for a host build of a model's C code: the
// node's own kernels (src/runtime/pod_linalg.cpp, built with
// -ffp-contract=off), so a host test computes the bits a pod computes.
//
//   c++ -std=c++17 -O2 -ffp-contract=off -I include -I third_party/wasm3/source \
//       -c src/runtime/pod_linalg.cpp models/common/k32_host.cpp
#include "k32_host.h"
#include "runtime/pod_linalg.hpp"

#include <cstdint>

using namespace kube32::runtime;

static uint8_t* b(void* p) { return static_cast<uint8_t*>(p); }
static const uint8_t* cb(const void* p) { return static_cast<const uint8_t*>(p); }

extern "C" {

int32_t k32_matvec_i8(float* out, const int8_t* w, const float* w_scale, const int8_t* x, float x_scale, uint32_t rows,
                      uint32_t cols, uint32_t row_stride) {
    matvec_i8(b(out), w, row_stride ? row_stride : cols, cb(w_scale), x, x_scale, rows, cols);
    return 0;
}

int32_t k32_matvec_i4(float* out, const void* w, const float* w_scale, const int8_t* x, float x_scale, uint32_t rows,
                      uint32_t cols, uint32_t row_stride) {
    matvec_i4(b(out), static_cast<const int8_t*>(w), row_stride ? row_stride : (cols + 1) / 2, cb(w_scale), x, x_scale, rows,
              cols);
    return 0;
}

int32_t k32_matvec_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x, float alpha, uint32_t rows,
                          uint32_t cols, uint32_t row_stride, uint32_t scale_stride) {
    matvec_f32_i8(b(out), w, row_stride ? row_stride : cols, cb(w_scale), scale_stride ? scale_stride : 4, cb(x), alpha, rows,
                  cols);
    return 0;
}

int32_t k32_vecmat_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x, uint32_t rows, uint32_t cols,
                          uint32_t row_stride, uint32_t scale_stride) {
    vecmat_f32_i8(b(out), w, row_stride ? row_stride : cols, cb(w_scale), scale_stride ? scale_stride : 4, cb(x), rows, cols);
    return 0;
}

int32_t k32_softmax_f32(float* x, uint32_t n) {
    softmax_f32(b(x), n);
    return 0;
}

int32_t k32_fft_q15(int32_t* re, int32_t* im, uint32_t n, const int16_t* tw_re, const int16_t* tw_im) {
    fft_q15(b(re), b(im), n, cb(tw_re), cb(tw_im));
    return 0;
}

}  // extern "C"
