#include "hal/mosaico_modules.hpp"
#include "runtime/k32_app_abi.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using kube32::hal::MosaicoModules;

namespace {

int s_inv_calls = 0;
void count_inv(const uint8_t*, size_t) {
    ++s_inv_calls;
}

void fill_uyvy_solid(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                     uint8_t y, uint8_t cb, uint8_t cr) {
    buf.resize(static_cast<size_t>(w) * h * 2U);
    for (size_t i = 0; i < buf.size(); i += 4U) {
        buf[i + 0] = cb;
        buf[i + 1] = y;
        buf[i + 2] = cr;
        buf[i + 3] = y;
    }
}

void fill_yuyv_solid(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                     uint8_t y, uint8_t cb, uint8_t cr) {
    buf.resize(static_cast<size_t>(w) * h * 2U);
    for (size_t i = 0; i < buf.size(); i += 4U) {
        buf[i + 0] = y;
        buf[i + 1] = cb;
        buf[i + 2] = y;
        buf[i + 3] = cr;
    }
}

// Fill quadrants in UYVY:
//   top-left (Q0):     Y=200, Cb=100, Cr=160 -> (+72, -28, +32)
//   top-right (Q1):    Y=60,  Cb=180, Cr=90  -> (-68, +52, -38)
//   bottom-left (Q2):  Y=128, Cb=0,   Cr=255 -> (0, -127, +127)
//   bottom-right (Q3): Y=0,   Cb=255, Cr=128 -> (-127, +127, 0)
void fill_uyvy_quadrants(std::vector<uint8_t>& buf, uint32_t w, uint32_t h) {
    buf.resize(static_cast<size_t>(w) * h * 2U);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = buf.data() + static_cast<size_t>(y) * w * 2U;
        for (uint32_t x = 0; x < w; x += 2U) {
            uint8_t yv, cb, cr;
            if (y < h / 2U && x < w / 2U) {
                yv = 200; cb = 100; cr = 160;
            } else if (y < h / 2U) {
                yv = 60; cb = 180; cr = 90;
            } else if (x < w / 2U) {
                yv = 128; cb = 0; cr = 255;
            } else {
                yv = 0; cb = 255; cr = 128;
            }
            row[x * 2U + 0] = cb;
            row[x * 2U + 1] = yv;
            row[x * 2U + 2] = cr;
            row[x * 2U + 3] = yv;
        }
    }
}

void check_px(const std::vector<int8_t>& out, uint32_t out_w, uint32_t ox, uint32_t oy,
              int8_t ey, int8_t ecb, int8_t ecr) {
    const size_t idx = (static_cast<size_t>(oy) * out_w + ox) * 3U;
    assert(out[idx + 0] == ey);
    assert(out[idx + 1] == ecb);
    assert(out[idx + 2] == ecr);
}

}  // namespace

int main() {
    // 1. Solid UYVY 1280x720 -> 48x32x3 + cache invalidation callback count (64 rows)
    std::vector<uint8_t> frame;
    fill_uyvy_solid(frame, 1280, 720, 192, 96, 160);
    std::vector<int8_t> out(48U * 32U * 3U, 0);
    s_inv_calls = 0;
    int32_t rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 1280, 720, 1280 * 2U, false, out.data(), 48, 32, 0, &count_inv);
    assert(rc == 48 * 32 * 3);
    assert(s_inv_calls == 64);
    for (uint32_t i = 0; i < 48U * 32U; ++i) {
        assert(out[i * 3U + 0] == 64);
        assert(out[i * 3U + 1] == -32);
        assert(out[i * 3U + 2] == 32);
    }

    // 2. Solid YUYV 640x480 -> 48x32x3 + clamping 0 -> -127 and 255 -> +127
    fill_yuyv_solid(frame, 640, 480, 0, 255, 128);
    rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 640, 480, 640 * 2U, true, out.data(), 48, 32, 0, nullptr);
    assert(rc == 48 * 32 * 3);
    for (uint32_t i = 0; i < 48U * 32U; ++i) {
        assert(out[i * 3U + 0] == -127);
        assert(out[i * 3U + 1] == 127);
        assert(out[i * 3U + 2] == 0);
    }

    // 3. Quadrant rotation verification (0, 90 CW, 180, 270 CW)
    fill_uyvy_quadrants(frame, 1280, 720);

    // 0 deg: TL=Q0, TR=Q1, BL=Q2, BR=Q3
    rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 1280, 720, 1280 * 2U, false, out.data(), 48, 32, 0);
    assert(rc == 48 * 32 * 3);
    check_px(out, 48, 4,  4,   72,  -28,   32);
    check_px(out, 48, 43, 4,  -68,   52,  -38);
    check_px(out, 48, 4,  27,   0, -127,  127);
    check_px(out, 48, 43, 27, -127, 127,    0);

    // 90 deg CW: Q0->TR, Q1->BR, Q2->TL, Q3->BL
    rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 1280, 720, 1280 * 2U, false, out.data(), 48, 32, 1);
    assert(rc == 48 * 32 * 3);
    check_px(out, 48, 43, 4,   72,  -28,   32);
    check_px(out, 48, 43, 27, -68,   52,  -38);
    check_px(out, 48, 4,  4,    0, -127,  127);
    check_px(out, 48, 4,  27, -127, 127,    0);

    // 180 deg: Q0->BR, Q1->BL, Q2->TR, Q3->TL
    rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 1280, 720, 1280 * 2U, false, out.data(), 48, 32, 2);
    assert(rc == 48 * 32 * 3);
    check_px(out, 48, 43, 27,  72,  -28,   32);
    check_px(out, 48, 4,  27, -68,   52,  -38);
    check_px(out, 48, 43, 4,    0, -127,  127);
    check_px(out, 48, 4,  4,  -127, 127,    0);

    // 270 deg CW: Q0->BL, Q1->TL, Q2->BR, Q3->TR
    rc = MosaicoModules::downsample_yuv422_to_ycbcr_i8(
        frame.data(), 1280, 720, 1280 * 2U, false, out.data(), 48, 32, 3);
    assert(rc == 48 * 32 * 3);
    check_px(out, 48, 4,  27,  72,  -28,   32);
    check_px(out, 48, 4,  4,  -68,   52,  -38);
    check_px(out, 48, 43, 27,   0, -127,  127);
    check_px(out, 48, 43, 4,  -127, 127,    0);

    // 4. Error handling & host stub returns HAL_ERR_NODEV
    assert(MosaicoModules::downsample_yuv422_to_ycbcr_i8(
               nullptr, 1280, 720, 0, false, out.data(), 48, 32, 0) == kube32::hal::HAL_ERR_INVAL);
    assert(MosaicoModules::downsample_yuv422_to_ycbcr_i8(
               frame.data(), 1280, 720, 0, false, out.data(), 0, 32, 0) == kube32::hal::HAL_ERR_INVAL);
    assert(MosaicoModules::downsample_yuv422_to_ycbcr_i8(
               frame.data(), 1280, 720, 0, false, out.data(), 48, 32, 4) == kube32::hal::HAL_ERR_INVAL);
    assert(MosaicoModules::instance().camera_capture_tensor(
               out.data(), 48, 32, K32_CAM_TENSOR_YCBCR_I8) == kube32::hal::HAL_ERR_NODEV);

    printf("test_camera_tensor: PASS\n");
    return 0;
}
