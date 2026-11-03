/* The linear algebra imports of k32_app_abi.h for a host build of a model's
 * C code (k32_host.cpp links them to the node's kernels). A wasm32 build
 * gets them from k32_app_abi.h instead. */
#ifndef K32_HOST_H
#define K32_HOST_H

#include <stdint.h>

#if !defined(__wasm32__)
#ifdef __cplusplus
extern "C" {
#endif
int32_t k32_matvec_i8(float* out, const int8_t* w, const float* w_scale, const int8_t* x, float x_scale, uint32_t rows,
                      uint32_t cols, uint32_t row_stride);
int32_t k32_matvec_i4(float* out, const void* w, const float* w_scale, const int8_t* x, float x_scale, uint32_t rows,
                      uint32_t cols, uint32_t row_stride);
int32_t k32_matvec_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x, float alpha, uint32_t rows,
                          uint32_t cols, uint32_t row_stride, uint32_t scale_stride);
int32_t k32_vecmat_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x, uint32_t rows, uint32_t cols,
                          uint32_t row_stride, uint32_t scale_stride);
int32_t k32_softmax_f32(float* x, uint32_t n);
int32_t k32_swiglu_f32(float* x, const float* g, uint32_t n);
int32_t k32_add_f32(float* x, const float* y, uint32_t n);
int32_t k32_add_f32_i8(float* x, const int8_t* q, float scale, uint32_t n);
int32_t k32_quantize_i8_static(int8_t* q, const float* x, float inv, uint32_t n);
int32_t k32_conv3x3_i8(float* out, const int8_t* w, const float* w_scale, const float* bias, const int8_t* in, float in_scale,
                       uint32_t cin, uint32_t cout, uint32_t h, uint32_t w_, uint32_t stride, uint32_t up, uint32_t n, uint32_t y0,
                       uint32_t y1);
int32_t k32_fft_q15(int32_t* re, int32_t* im, uint32_t n, const int16_t* tw_re, const int16_t* tw_im);
#ifdef __cplusplus
}
#endif
#endif

#endif
