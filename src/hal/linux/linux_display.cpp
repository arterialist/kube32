// A Linux board's panel: the pods' RGB565 drawing calls on the framebuffer's
// memory, and the framebuffer device itself (Linux only).

#include "hal/linux_board.hpp"
#include "hal/display_font.hpp"
#include "hal/hal_types.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace kube32::hal {

namespace {

// An 8-bit channel at `length` bits, placed at `offset`
uint32_t place(uint32_t v8, uint8_t offset, uint8_t length) {
    if (length == 0) return 0;
    if (length > 8) length = 8;
    return (v8 >> (8 - length)) << offset;
}

}  // namespace

bool FrameCanvas::attach(uint8_t* pixels, int width, int height, size_t stride, const PixelLayout& layout) {
    if ((layout.bits != 16 && layout.bits != 32) || !pixels || width <= 0 || height <= 0 ||
        stride < static_cast<size_t>(width) * (layout.bits / 8)) {
        return false;
    }
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    stride_ = stride;
    layout_ = layout;
    return true;
}

uint32_t FrameCanvas::native(uint16_t c) const {
    // 8-bit channels whose low bits repeat the high ones: 0x1f becomes 0xff
    const uint32_t r5 = c >> 11, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
    const uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
    return place(r, layout_.red_offset, layout_.red_length) | place(g, layout_.green_offset, layout_.green_length) |
           place(b, layout_.blue_offset, layout_.blue_length);
}

void FrameCanvas::put(int x, int y, uint32_t pixel) {
    uint8_t* p = pixels_ + static_cast<size_t>(y) * stride_;
    if (layout_.bits == 16) {
        const uint16_t v = static_cast<uint16_t>(pixel);
        std::memcpy(p + static_cast<size_t>(x) * 2, &v, 2);
    } else {
        std::memcpy(p + static_cast<size_t>(x) * 4, &pixel, 4);
    }
}

bool FrameCanvas::clip(int& x, int& y, int& w, int& h) const {
    if (!pixels_ || w <= 0 || h <= 0) return false;
    // In 64 bits: a pod's coordinates may be anywhere in int32
    const int64_t x0 = std::max<int64_t>(x, 0), y0 = std::max<int64_t>(y, 0);
    const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(x) + w, width_);
    const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(y) + h, height_);
    if (x0 >= x1 || y0 >= y1) return false;
    x = static_cast<int>(x0);
    y = static_cast<int>(y0);
    w = static_cast<int>(x1 - x0);
    h = static_cast<int>(y1 - y0);
    return true;
}

void FrameCanvas::rect(int x, int y, int w, int h, uint16_t color) {
    if (!clip(x, y, w, h)) return;
    const uint32_t pixel = native(color);
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) put(col, row, pixel);
    }
}

void FrameCanvas::bar(int x, int y, int w, int h, uint32_t pct, uint16_t fg, uint16_t bg) {
    if (w <= 0 || h <= 0) return;
    if (pct > 100) pct = 100;
    const int filled = static_cast<int>(static_cast<int64_t>(w) * pct / 100);
    if (filled > 0) rect(x, y, filled, h, fg);
    if (filled < w) rect(static_cast<int>(std::min<int64_t>(static_cast<int64_t>(x) + filled, INT32_MAX)), y, w - filled, h, bg);
}

void FrameCanvas::blit(int x, int y, int w, int h, const uint16_t* pixels) {
    if (!pixels) return;
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip(cx, cy, cw, ch)) return;
    // The source rows and columns that stay on the panel
    const size_t skip_x = static_cast<size_t>(cx - x), skip_y = static_cast<size_t>(cy - y);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels);
    const bool rgb565 = layout_.bits == 16 && layout_.red_offset == 11 && layout_.red_length == 5 && layout_.green_offset == 5 &&
                        layout_.green_length == 6 && layout_.blue_offset == 0 && layout_.blue_length == 5;
    for (int r = 0; r < ch; ++r) {
        const uint8_t* row = src + ((skip_y + static_cast<size_t>(r)) * static_cast<size_t>(w) + skip_x) * 2;
        if (rgb565) {
            std::memcpy(pixels_ + static_cast<size_t>(cy + r) * stride_ + static_cast<size_t>(cx) * 2, row, static_cast<size_t>(cw) * 2);
            continue;
        }
        for (int i = 0; i < cw; ++i) {
            uint16_t v;
            std::memcpy(&v, row + static_cast<size_t>(i) * 2, 2);   // the pod's pixels may be unaligned
            put(cx + i, cy + r, native(v));
        }
    }
}

void FrameCanvas::text(int x, int y, const char* s, size_t len, uint16_t fg, uint16_t bg) {
    if (!pixels_ || !s) return;
    if (y >= height_ || static_cast<int64_t>(y) + kFontCellHeight <= 0) return;
    const uint32_t on = native(fg), off = native(bg);
    for (size_t i = 0; i < len; ++i) {
        const int64_t cell = static_cast<int64_t>(x) + static_cast<int64_t>(i) * kFontCellWidth;
        if (cell >= width_) break;
        if (cell + kFontCellWidth <= 0) continue;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        for (int row = 0; row < kFontCellHeight; ++row) {
            const int64_t py = static_cast<int64_t>(y) + row;
            if (py < 0 || py >= height_) continue;
            for (int col = 0; col < kFontCellWidth; ++col) {
                const int64_t px = cell + col;
                if (px < 0 || px >= width_) continue;
                put(static_cast<int>(px), static_cast<int>(py), font_pixel(c, col, row) ? on : off);
            }
        }
    }
}

bool LinuxBoard::probe_display() {
    const std::string& dev = config_.framebuffer;
#if defined(__linux__)
    const int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("[BOARD] display %s: cannot open it (%s); the node offers no display\n", dev.c_str(), std::strerror(errno));
        return false;
    }
    fb_var_screeninfo var{};
    fb_fix_screeninfo fix{};
    if (::ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ::ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        printf("[BOARD] display %s: not a framebuffer (%s); the node offers no display\n", dev.c_str(), std::strerror(errno));
        ::close(fd);
        return false;
    }
    Panel p;
    p.width = static_cast<int>(var.xres);
    p.height = static_cast<int>(var.yres);
    p.stride = fix.line_length;
    // The visible part of a panned framebuffer
    p.offset = static_cast<size_t>(var.yoffset) * fix.line_length + static_cast<size_t>(var.xoffset) * (var.bits_per_pixel / 8);
    p.bytes = fix.smem_len ? fix.smem_len : static_cast<size_t>(fix.line_length) * var.yres_virtual;
    p.layout.bits = static_cast<uint8_t>(var.bits_per_pixel);
    p.layout.red_offset = static_cast<uint8_t>(var.red.offset);
    p.layout.red_length = static_cast<uint8_t>(var.red.length);
    p.layout.green_offset = static_cast<uint8_t>(var.green.offset);
    p.layout.green_length = static_cast<uint8_t>(var.green.length);
    p.layout.blue_offset = static_cast<uint8_t>(var.blue.offset);
    p.layout.blue_length = static_cast<uint8_t>(var.blue.length);
    const bool drawable = (p.layout.bits == 16 || p.layout.bits == 32) && p.width > 0 && p.height > 0 &&
                          p.stride >= static_cast<size_t>(p.width) * (p.layout.bits / 8) &&
                          p.offset + static_cast<size_t>(p.height) * p.stride <= p.bytes;
    if (!drawable) {
        printf("[BOARD] display %s: %u bits a pixel, %ux%u; the node draws on 16 and 32 and offers no display\n", dev.c_str(),
               var.bits_per_pixel, var.xres, var.yres);
        ::close(fd);
        return false;
    }
    framebuffer_fd_ = fd;
    panel_ = p;
    printf("[BOARD] display %s: %dx%d, %u bits a pixel\n", dev.c_str(), p.width, p.height, var.bits_per_pixel);
    return true;
#else
    printf("[BOARD] display %s: framebuffers are Linux devices; the node offers no display\n", dev.c_str());
    return false;
#endif
}

bool LinuxBoard::map_display_locked() {
#if defined(__linux__)
    void* map = ::mmap(nullptr, panel_.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer_fd_, 0);
    if (map == MAP_FAILED) {
        printf("[BOARD] display %s: cannot map it (%s)\n", config_.framebuffer.c_str(), std::strerror(errno));
        return false;
    }
    framebuffer_ = static_cast<uint8_t*>(map);
    canvas_.attach(framebuffer_ + panel_.offset, panel_.width, panel_.height, panel_.stride, panel_.layout);
    // Nothing the panel showed before stays beside a pod's drawing
    canvas_.rect(0, 0, panel_.width, panel_.height, 0x0000);
    if (!config_.backlight.empty()) backlight_on();
    return true;
#else
    return false;
#endif
}

void LinuxBoard::unmap_display_locked() {
#if defined(__linux__)
    if (framebuffer_) ::munmap(framebuffer_, panel_.bytes);
#endif
    framebuffer_ = nullptr;
    canvas_ = FrameCanvas{};
    showing_ready_ = false;
}

void LinuxBoard::backlight_on() {
    // A launcher that blanked the screen leaves its backlight at 0
    const std::string dir = config_.backlight;
    int level = -1, max = -1;
    std::ifstream(dir + "/brightness") >> level;
    std::ifstream(dir + "/max_brightness") >> max;
    if (level < 0 || max <= 0) {
        printf("[BOARD] backlight %s: cannot read it\n", dir.c_str());
        return;
    }
    if (level > 0) return;
    const int on = std::max(1, max * 3 / 4);
    std::ofstream out(dir + "/brightness");
    out << on << "\n";
    out.flush();
    if (!out) {
        printf("[BOARD] backlight %s: off, and cannot turn it on (%s)\n", dir.c_str(), std::strerror(errno));
        return;
    }
    printf("[BOARD] backlight %s: on at %d of %d\n", dir.c_str(), on, max);
}

void LinuxBoard::show_ready(const std::string& node_ip, const std::string& app_url) {
    if (!config_.screen_owner.empty()) return;
    constexpr uint16_t kBanner = 0x194B, kGreen = 0x07E0, kWhite = 0xFFFF, kCyan = 0x07FF, kDark = 0x10A2;
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (!framebuffer_) return;
    canvas_.rect(0, 0, canvas_.width(), canvas_.height(), kDark);
    canvas_.rect(0, 0, canvas_.width(), 18, kBanner);
    canvas_.text(6, 5, "ready", 5, kGreen, kBanner);
    canvas_.text(6, 28, node_ip.data(), node_ip.size(), kWhite, kDark);
    canvas_.text(6, 40, app_url.data(), app_url.size(), kCyan, kDark);
    showing_ready_ = true;
}

void LinuxBoard::leave_ready_locked() {
    if (!showing_ready_) return;
    showing_ready_ = false;
    canvas_.rect(0, 0, canvas_.width(), canvas_.height(), 0x0000);
}

DisplayInfo LinuxBoard::display_info() const {
    if (!display_present_) return {};
    return {panel_.width, panel_.height, KUBE32_DISPLAY_RGB565};
}

int32_t LinuxBoard::display_text(int32_t x, int32_t y, const char* text, size_t len, uint16_t fg, uint16_t bg) {
    if (!display_present_) return HAL_ERR_NODEV;
    take_from_owner();
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (!framebuffer_) return HAL_ERR_NODEV;
    leave_ready_locked();
    canvas_.text(x, y, text, len, fg, bg);
    return HAL_OK;
}

int32_t LinuxBoard::display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!display_present_) return HAL_ERR_NODEV;
    take_from_owner();
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (!framebuffer_) return HAL_ERR_NODEV;
    leave_ready_locked();
    canvas_.rect(x, y, w, h, color);
    return HAL_OK;
}

int32_t LinuxBoard::display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint16_t fg, uint16_t bg) {
    if (!display_present_) return HAL_ERR_NODEV;
    take_from_owner();
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (!framebuffer_) return HAL_ERR_NODEV;
    leave_ready_locked();
    canvas_.bar(x, y, w, h, pct, fg, bg);
    return HAL_OK;
}

int32_t LinuxBoard::display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) {
    if (!display_present_) return HAL_ERR_NODEV;
    take_from_owner();
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (!framebuffer_) return HAL_ERR_NODEV;
    leave_ready_locked();
    canvas_.blit(x, y, w, h, pixels);
    return HAL_OK;
}

}  // namespace kube32::hal
