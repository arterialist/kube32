// SPDX-License-Identifier: GPL-2.0-or-later
// drawlist_check: the split renderer against the engine. Runs the full
// engine (doom.wasm) and the sim-only engine (doom-sim.wasm) in lockstep on
// the host's wasm3, same IWAD, keys and clock; every tic, draws the sim's
// draw list with the native k32_raster_* kernels (src/runtime/pod_raster.cpp)
// through the decoder the view pod uses (view/drawlist.c) and the render
// pack, and compares the pixels with the frame the full engine drew.
//
//   drawlist_check <doom.wasm> <doom-sim.wasm> <iwad> <pack> [--tics N] [--args "..."]
//                  [--iwad-name doom1.wad] [--screenblocks 10|11] [--spin] [--dump frame.ppm]
//
// Prints the frames compared, the pixels that differ, and the decode time
// (records plus kernels) a frame on this host.
#include "wasm3.h"
#include "m3_env.h"
#include "runtime/pod_raster.hpp"
#include "runtime/k32_app_abi.h"
extern "C" {
#include "../view/drawlist.h"
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> g_wad, g_pack;
static std::string g_env_iwad = "doom1.wad", g_env_args = "-warp 1 1 -skill 3 -kb 480", g_env_blocks = "10";
static int64_t g_clock_ms;

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the kernels over the pack as an arena: an address is a pack offset ----
static uint64_t g_kernel_us, g_column_records, g_span_records, g_kernel_calls;
static const uint8_t* resolve(void*, uint32_t addr, uint32_t len) {
    return addr <= g_pack.size() && len <= g_pack.size() - addr ? g_pack.data() + addr : nullptr;
}
extern "C" int32_t dl_host_columns(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap,
                                   const uint8_t* translation, int32_t centery) {
    uint32_t bad = 0;
    const uint8_t* cmap = resolve(nullptr, colormap, 34 * 256);
    if (!cmap) return K32_ERR_FAULT;
    const uint64_t t0 = now_us();
    int32_t rc = kube32::runtime::raster_columns(dst, stride, y0, rows, static_cast<const uint8_t*>(records), n, cmap, translation, centery,
                                                 &resolve, nullptr, &bad);
    g_kernel_us += now_us() - t0;
    g_column_records += n;
    g_kernel_calls++;
    if (rc) std::printf("columns: rc %d at record %u\n", rc, bad);
    return rc;
}
extern "C" int32_t dl_host_spans(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap) {
    uint32_t bad = 0;
    const uint8_t* cmap = resolve(nullptr, colormap, 34 * 256);
    if (!cmap) return K32_ERR_FAULT;
    const uint64_t t0 = now_us();
    int32_t rc = kube32::runtime::raster_spans(dst, stride, y0, rows, static_cast<const uint8_t*>(records), n, cmap, &resolve, nullptr, &bad);
    g_kernel_us += now_us() - t0;
    g_span_records += n;
    g_kernel_calls++;
    if (rc) std::printf("spans: rc %d at record %u\n", rc, bad);
    return rc;
}

// ---- the host imports both engines use ----
m3ApiRawFunction(h_log) {
    m3ApiGetArgMem(const char*, msg);
    m3ApiGetArg(uint32_t, len);
    if (std::getenv("DL_LOG")) std::printf("[pod] %.*s\n", (int)len, msg);
    m3ApiSuccess();
}
m3ApiRawFunction(h_time_ms) { m3ApiReturnType(int64_t); m3ApiReturn(g_clock_ms); }
m3ApiRawFunction(h_time_us) { m3ApiReturnType(int64_t); m3ApiReturn((int64_t)now_us()); }
m3ApiRawFunction(h_env_get) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, key);
    m3ApiGetArg(uint32_t, key_len);
    m3ApiGetArgMem(char*, out);
    m3ApiGetArg(uint32_t, cap);
    std::string k(key, key_len);
    const std::string* v = k == "DOOM_IWAD" ? &g_env_iwad : k == "DOOM_ARGS" ? &g_env_args : k == "DOOM_SCREENBLOCKS" ? &g_env_blocks : nullptr;
    if (!v) m3ApiReturn(-1);
    uint32_t n = std::min<uint32_t>(cap, (uint32_t)v->size());
    std::memcpy(out, v->data(), n);
    m3ApiReturn((int32_t)v->size());
}
m3ApiRawFunction(h_blob_open) { m3ApiReturnType(int32_t); m3ApiGetArgMem(const char*, n); m3ApiGetArg(uint32_t, l); (void)n; (void)l; m3ApiReturn(0); }
m3ApiRawFunction(h_blob_size) { m3ApiReturnType(int32_t); m3ApiGetArg(int32_t, h); (void)h; m3ApiReturn((int32_t)g_wad.size()); }
m3ApiRawFunction(h_blob_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(uint32_t, off);
    m3ApiGetArgMem(uint8_t*, dst);
    m3ApiGetArg(uint32_t, len);
    (void)h;
    m3ApiCheckMem(dst, len);
    if (off >= g_wad.size()) m3ApiReturn(0);
    uint32_t n = std::min<uint32_t>(len, (uint32_t)g_wad.size() - off);
    std::memcpy(dst, g_wad.data() + off, n);
    m3ApiReturn((int32_t)n);
}
m3ApiRawFunction(h_blob_close) { m3ApiReturnType(int32_t); m3ApiGetArg(int32_t, h); (void)h; m3ApiReturn(0); }
m3ApiRawFunction(h_resp_begin) { m3ApiGetArg(int32_t, s); m3ApiGetArg(uint32_t, a); m3ApiGetArg(uint32_t, b); (void)s; (void)a; (void)b; m3ApiSuccess(); }
m3ApiRawFunction(h_resp_header) { m3ApiGetArg(uint32_t, a); m3ApiGetArg(uint32_t, b); m3ApiGetArg(uint32_t, c); m3ApiGetArg(uint32_t, d); (void)a; (void)b; (void)c; (void)d; m3ApiSuccess(); }
m3ApiRawFunction(h_resp_write) { m3ApiReturnType(int32_t); m3ApiGetArgMem(const char*, p); m3ApiGetArg(uint32_t, len); (void)p; m3ApiReturn((int32_t)len); }
m3ApiRawFunction(h_resp_end) { m3ApiSuccess(); }

struct Engine {
    IM3Environment env{};
    IM3Runtime rt{};
    IM3Module mod{};
    IM3Function init{}, tic{}, render{}, key{}, screen{}, dl_ptr{}, dl_len{};
    std::vector<uint8_t> wasm;
    bool load(const char* path, bool sim) {
        std::ifstream f(path, std::ios::binary);
        wasm.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        env = m3_NewEnvironment();
        rt = m3_NewRuntime(env, 64 * 1024, nullptr);
        M3Result r = m3_ParseModule(env, &mod, wasm.data(), (uint32_t)wasm.size());
        if (!r) r = m3_LoadModule(rt, mod);
        if (r) { std::printf("%s: %s\n", path, r); return false; }
        struct { const char* name; const char* sig; M3RawCall fn; } imports[] = {
            {"k32_log", "v(ii)", h_log}, {"k32_time_ms", "I()", h_time_ms}, {"esp32_get_time_us", "I()", h_time_us},
            {"k32_env_get", "i(iiii)", h_env_get}, {"k32_blob_open", "i(ii)", h_blob_open}, {"k32_blob_size", "i(i)", h_blob_size},
            {"k32_blob_read", "i(iiii)", h_blob_read}, {"k32_blob_close", "i(i)", h_blob_close}, {"k32_resp_begin", "v(iii)", h_resp_begin},
            {"k32_resp_header", "v(iiii)", h_resp_header}, {"k32_resp_write", "i(ii)", h_resp_write}, {"k32_resp_end", "v()", h_resp_end},
        };
        for (auto& im : imports) {
            r = m3_LinkRawFunction(mod, "env", im.name, im.sig, im.fn);
            if (r && r != m3Err_functionLookupFailed) { std::printf("link %s: %s\n", im.name, r); return false; }
        }
        if ((r = m3_FindFunction(&init, rt, "k32_init")) || (r = m3_FindFunction(&tic, rt, "doom_run_tic")) ||
            (r = m3_FindFunction(&render, rt, "doom_render_view")) || (r = m3_FindFunction(&key, rt, "doom_key"))) {
            std::printf("%s exports: %s\n", path, r);
            return false;
        }
        if (sim) r = m3_FindFunction(&dl_ptr, rt, "doom_drawlist_ptr"), r = r ? r : m3_FindFunction(&dl_len, rt, "doom_drawlist_len");
        else r = m3_FindFunction(&screen, rt, "doom_screen_ptr");
        if (r) { std::printf("%s: %s (built for this check?)\n", path, r); return false; }
        return true;
    }
    bool call(IM3Function fn, int argc = 0, const char** argv = nullptr) {
        M3Result r = m3_CallArgv(fn, argc, argv);
        if (r) { M3ErrorInfo info; m3_GetErrorInfo(rt, &info); std::printf("call: %s %s\n", r, info.message ? info.message : ""); return false; }
        return true;
    }
    uint32_t result(IM3Function fn) { uint32_t v = 0; m3_GetResultsV(fn, &v); return v; }
    const uint8_t* mem() { return m3_GetMemory(mod, nullptr, 0); }
    void press(int down, int code) {
        std::string d = std::to_string(down), c = std::to_string(code);
        const char* a[] = {d.c_str(), c.c_str()};
        call(key, 2, a);
    }
};

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: drawlist_check <doom.wasm> <doom-sim.wasm> <iwad> <pack> [--tics N] [--args \"...\"] [--iwad-name NAME] "
                             "[--screenblocks N] [--spin] [--dump frame.ppm]\n");
        return 2;
    }
    std::ifstream w(argv[3], std::ios::binary);
    g_wad.assign(std::istreambuf_iterator<char>(w), std::istreambuf_iterator<char>());
    std::ifstream pk(argv[4], std::ios::binary);
    g_pack.assign(std::istreambuf_iterator<char>(pk), std::istreambuf_iterator<char>());
    int tics = 350;
    bool spin = false;
    const char* dump = nullptr;
    for (int i = 5; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--tics") tics = std::atoi(argv[++i]);
        else if (a == "--args") g_env_args = argv[++i];
        else if (a == "--iwad-name") g_env_iwad = argv[++i];
        else if (a == "--screenblocks") g_env_blocks = argv[++i];
        else if (a == "--spin") spin = true;
        else if (a == "--dump") dump = argv[++i];
    }
    Engine full, sim;
    if (!full.load(argv[1], false) || !sim.load(argv[2], true)) return 1;
    if (!full.call(full.init) || !sim.call(sim.init)) return 1;
    dl_ctx ctx;
    const uint32_t hl = dl_head_len(g_pack.data());
    if (dl_init(&ctx, 0, g_pack.data(), hl)) { std::printf("not a version 2 render pack\n"); return 1; }

    // the bench's course
    const int UP = 0xad, LEFT = 0xac, RIGHT = 0xae, FIRE = 0xa3, USE = 0xa2;
    std::vector<std::vector<int>> course;
    for (int c : {'i', 'd', 'd', 'q', 'd'}) { course.push_back({3, 1, c}); course.push_back({3, 0, c}); }
    const int pattern[][3] = {{UP, 0, 70}, {RIGHT, UP, 20}, {UP, FIRE, 40}, {LEFT, 0, 30}, {UP, USE, 50}, {RIGHT, FIRE, 35}, {UP, 0, 60}, {LEFT, UP, 25}};
    if (spin) course.push_back({10, 1, RIGHT});
    for (int t = 10; t < tics;) {
        for (auto& p : pattern) {
            for (int k = 0; k < 2; k++) if (p[k]) { course.push_back({t, 1, p[k]}); course.push_back({t + p[2], 0, p[k]}); }
            t += p[2] + 1;
        }
    }
    uint64_t frames = 0, bad_frames = 0, bad_pixels = 0, pixels = 0, decode_us = 0, max_frame_bad = 0;
    std::vector<uint8_t> frame, band;
    uint32_t vw = 0, vh = 0;
    for (int t = 0; t < tics; t++) {
        for (auto& e : course) if (e[0] == t) { full.press(e[1], e[2]); sim.press(e[1], e[2]); }
        g_clock_ms = (int64_t)(t + 1) * 1000 / 35;
        if (!full.call(full.tic) || !sim.call(sim.tic)) return 1;
        if (!full.call(full.render) || !sim.call(sim.render)) return 1;
        full.call(full.screen);
        const uint32_t screen = full.result(full.screen);
        sim.call(sim.dl_ptr);
        const uint32_t lp = sim.result(sim.dl_ptr);
        sim.call(sim.dl_len);
        const uint32_t ll = sim.result(sim.dl_len);
        const uint8_t* list = sim.mem() + lp;
        vw = list[6] | (list[7] << 8);
        vh = list[8];
        if (list[9]) std::printf("tic %d: the draw list was truncated\n", t);
        if (std::getenv("DL_DEBUG") && t == 0) {
            uint32_t at = 28;
            for (int k = 0; k < 6 && at < ll; k++) {
                const uint8_t* e = list + at;
                auto u16 = [&](int o) { return e[o] | (e[o + 1] << 8); };
                auto i32 = [&](int o) { return (int32_t)(e[o] | (e[o + 1] << 8) | (e[o + 2] << 16) | ((uint32_t)e[o + 3] << 24)); };
                if (e[0] == 1) {
                    for (int i = 0; i < 24; i++) std::printf("%02x ", e[i]);
                    std::printf("\n");
                    std::printf("strip x0 %u n %u tex %u light %u texturemid %d scale0 %d scalestep %d; col0 yl %u yh %u col %u\n", u16(1), e[3], e[4], e[5],
                                i32(6), i32(10), i32(14), e[18], e[19], e[20]);
                    at += 18 + 3 * e[3];
                } else if (e[0] == 2) { std::printf("plane flat %u light %u height %d nspans %u\n", e[1], e[2], i32(3), u16(7)); at += 9 + 13 * u16(7); }
                else if (e[0] == 3) { std::printf("sprite lump %u x %u..%u\n", u16(1), u16(3), u16(5)); at += 25 + 2 * (u16(5) - u16(3) + 1); }
                else { std::printf("masked\n"); at += 20; }
            }
        }
        // the full engine's screen is SCREENWIDTH wide: the view's width here, rows 0 .. vh - 1
        frame.assign((size_t)vw * vh, 0);
        band.assign((size_t)vw * 27, 0);
        const uint64_t t0 = now_us();
        for (uint32_t y0 = 0; y0 < vh; y0 += 27) {
            const uint32_t rows = std::min<uint32_t>(27, vh - y0);
            int32_t rc = dl_draw(&ctx, list, ll, band.data(), vw, (int32_t)y0, rows);
            if (rc) { std::printf("tic %d: decode rc %d (entry type %u at %u of %u)\n", t, rc, dl_error_type, dl_error_at, ll); return 1; }
            std::memcpy(frame.data() + (size_t)y0 * vw, band.data(), (size_t)rows * vw);
        }
        decode_us += now_us() - t0;
        const uint8_t* want = full.mem() + screen;
        uint64_t bad = 0;
        for (size_t i = 0; i < frame.size(); i++) bad += frame[i] != want[i];
        frames++;
        pixels += frame.size();
        bad_pixels += bad;
        if (bad) { bad_frames++; if (bad > max_frame_bad) max_frame_bad = bad; }
        if (bad && bad_frames <= 3) {
            size_t first = 0;
            while (first < frame.size() && frame[first] == want[first]) first++;
            std::printf("tic %d: %llu of %zu pixels differ, first at x %zu y %zu (got %u want %u)\n", t, (unsigned long long)bad, frame.size(),
                        first % vw, first / vw, frame[first], want[first]);
        }
        if (dump && t == tics - 1) {
            std::FILE* f = std::fopen(dump, "wb");
            const uint8_t* pal = g_pack.data() + ctx.palette;
            std::fprintf(f, "P6\n%u %u\n255\n", vw, 2 * vh);
            for (const uint8_t* src : {static_cast<const uint8_t*>(frame.data()), want}) for (size_t i = 0; i < frame.size(); i++) std::fwrite(pal + 3 * src[i], 1, 3, f);
            std::fclose(f);
        }
    }
    std::printf("%llu frames of %u x %u, %s: %llu frames differ, %llu of %llu pixels (worst frame %llu); decode %llu us a frame on this host; "
                "%u entries unsupported\n",
                (unsigned long long)frames, vw, vh, g_env_blocks == "11" ? "whole screen" : "status bar below", (unsigned long long)bad_frames,
                (unsigned long long)bad_pixels, (unsigned long long)pixels, (unsigned long long)max_frame_bad,
                (unsigned long long)(frames ? decode_us / frames : 0), ctx.unsupported);
    if (frames)
        std::printf("a frame in 5 bands: kernels %llu us, decoder %llu us, %llu kernel calls, %llu column records, %llu span records\n",
                    (unsigned long long)(g_kernel_us / frames), (unsigned long long)((decode_us - g_kernel_us) / frames),
                    (unsigned long long)(g_kernel_calls / frames), (unsigned long long)(g_column_records / frames),
                    (unsigned long long)(g_span_records / frames));
    return bad_pixels ? 3 : 0;
}
