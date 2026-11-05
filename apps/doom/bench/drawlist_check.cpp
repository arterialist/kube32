// SPDX-License-Identifier: GPL-2.0-or-later
// drawlist_check: the split renderer against the engine. Runs the full
// engine (doom.wasm) and the sim-only engine (doom-sim.wasm) in lockstep on
// the host's wasm3, same IWAD, keys and clock; every tic, draws the sim's
// draw list with the native k32_raster_* kernels (src/runtime/pod_raster.cpp)
// through the decoder the view pod uses (view/drawlist.c) and the render
// pack, and compares the pixels with the frame the full engine drew.
//
//   drawlist_check <doom.wasm> <doom-sim.wasm> <iwad> <pack> [--tics N] [--args "..."]
//                  [--iwad-name doom1.wad] [--screenblocks 10|11] [--detail 0|1] [--spin] [--dump frame.ppm]
//                  [--state] [--no-pixels] [--sim-args "..."] [--locate x,y,tic]
//
// --state also compares the two engines' game states (players, map objects,
// sectors, P_Random) every tic and the renderers' frames (drawsegs,
// vissprites, visplanes, openings) every frame, and names the first part
// that differs; --no-pixels skips the frames (the game states alone, fast).
// --sim-args gives the sim engine its own DOOM_ARGS (a smaller zone), --sim-wad its
// own IWAD (the sim pack, wad.py simpack).
//
// Every frame, the status bar's numbers the sim appends to its draw list are
// compared with the engine's player. A tap of the fire key, pressed and
// released before the same tic (as a keyboard that reports presses alone
// sends it), must fire a shot: the ammo the record shows drops.
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
static std::string g_env_iwad = "doom1.wad", g_env_args = "-warp 1 1 -skill 3 -kb 480", g_env_blocks = "10", g_env_detail = "0";
static std::string g_sim_args;   // --sim-args: the sim engine's DOOM_ARGS when they differ (its zone)
static std::vector<uint8_t> g_sim_wad;   // --sim-wad: the sim engine's IWAD when it differs (the sim pack)
static IM3Runtime g_sim_rt;
static const std::vector<uint8_t>& wad_of(IM3Runtime rt) { return rt == g_sim_rt && !g_sim_wad.empty() ? g_sim_wad : g_wad; }
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
    static const char* trace = std::getenv("DL_TRACE");   // "x,y": the records at column x and the pixel at y after the call
    int tx = -1, ty = -1;
    if (trace) std::sscanf(trace, "%d,%d", &tx, &ty);
    int32_t rc = kube32::runtime::raster_columns(dst, stride, y0, rows, static_cast<const uint8_t*>(records), n, cmap, translation, centery,
                                                 &resolve, nullptr, &bad);
    if (trace)
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* r = static_cast<const uint8_t*>(records) + 24 * i;
            const uint32_t x = r[16] | (r[17] << 8);
            if ((int)x != tx || ty < y0 || ty >= y0 + (int)rows) continue;
            std::printf("  record kind %u index %u x %u a %u b %u light %u src %u: band row %d now %u (rc %d)\n", r[23], r[18] | (r[19] << 8), x, r[20], r[21],
                        r[22], r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24), ty, dst[(ty - y0) * stride + x], rc);
        }
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
    const std::string* v = k == "DOOM_IWAD" ? &g_env_iwad : k == "DOOM_ARGS" ? &g_env_args : k == "DOOM_SCREENBLOCKS" ? &g_env_blocks : k == "DOOM_DETAIL" ? &g_env_detail : nullptr;
    if (!v) m3ApiReturn(-1);
    uint32_t n = std::min<uint32_t>(cap, (uint32_t)v->size());
    std::memcpy(out, v->data(), n);
    m3ApiReturn((int32_t)v->size());
}
m3ApiRawFunction(h_blob_open) { m3ApiReturnType(int32_t); m3ApiGetArgMem(const char*, n); m3ApiGetArg(uint32_t, l); (void)n; (void)l; m3ApiReturn(0); }
m3ApiRawFunction(h_blob_size) { m3ApiReturnType(int32_t); m3ApiGetArg(int32_t, h); (void)h; m3ApiReturn((int32_t)wad_of(runtime).size()); }
m3ApiRawFunction(h_blob_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(uint32_t, off);
    m3ApiGetArgMem(uint8_t*, dst);
    m3ApiGetArg(uint32_t, len);
    (void)h;
    m3ApiCheckMem(dst, len);
    const std::vector<uint8_t>& wad = wad_of(runtime);
    if (off >= wad.size()) m3ApiReturn(0);
    uint32_t n = std::min<uint32_t>(len, (uint32_t)wad.size() - off);
    std::memcpy(dst, wad.data() + off, n);
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
    IM3Function init{}, tic{}, render{}, key{}, screen{}, dl_ptr{}, dl_len{}, digest{}, mobj{}, rdigest{}, hud{};
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
        if (!m3_FindFunction(&digest, rt, "doom_state_digest")) m3_FindFunction(&mobj, rt, "doom_mobj_field"); else digest = nullptr;
        if (m3_FindFunction(&rdigest, rt, "doom_render_digest")) rdigest = nullptr;
        if (sim) r = m3_FindFunction(&dl_ptr, rt, "doom_drawlist_ptr"), r = r ? r : m3_FindFunction(&dl_len, rt, "doom_drawlist_len");
        else r = m3_FindFunction(&screen, rt, "doom_screen_ptr");
        if (!r) r = m3_FindFunction(&hud, rt, "doom_hud_ptr");
        if (r) { std::printf("%s: %s (built for this check?)\n", path, r); return false; }
        return true;
    }
    bool call(IM3Function fn, int argc = 0, const char** argv = nullptr) {
        M3Result r = m3_CallArgv(fn, argc, argv);
        if (r) { M3ErrorInfo info; m3_GetErrorInfo(rt, &info); std::printf("call: %s %s\n", r, info.message ? info.message : ""); return false; }
        return true;
    }
    uint32_t result(IM3Function fn) { uint32_t v = 0; m3_GetResultsV(fn, &v); return v; }
    uint32_t call1(IM3Function fn, int32_t a) { std::string s = std::to_string(a); const char* v[] = {s.c_str()}; call(fn, 1, v); return result(fn); }
    int32_t call2(IM3Function fn, int32_t a, int32_t b) {
        std::string s = std::to_string(a), t = std::to_string(b);
        const char* v[] = {s.c_str(), t.c_str()};
        call(fn, 2, v);
        return (int32_t)result(fn);
    }
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
                             "[--screenblocks N] [--detail 0|1] [--spin] [--dump frame.ppm]\n");
        return 2;
    }
    std::ifstream w(argv[3], std::ios::binary);
    g_wad.assign(std::istreambuf_iterator<char>(w), std::istreambuf_iterator<char>());
    std::ifstream pk(argv[4], std::ios::binary);
    g_pack.assign(std::istreambuf_iterator<char>(pk), std::istreambuf_iterator<char>());
    int tics = 350;
    bool spin = false;
    const char* dump = nullptr;
    int locate_x = -1, locate_y = -1, locate_tic = -1;   // --locate x,y,tic: which entry last writes that pixel
    bool state = false, no_pixels = false;   // --state: both engines' game-state digests every tic; --no-pixels: no frames
    for (int i = 5; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--tics") tics = std::atoi(argv[++i]);
        else if (a == "--args") g_env_args = argv[++i];
        else if (a == "--iwad-name") g_env_iwad = argv[++i];
        else if (a == "--screenblocks") g_env_blocks = argv[++i];
        else if (a == "--detail") g_env_detail = argv[++i];
        else if (a == "--spin") spin = true;
        else if (a == "--dump") dump = argv[++i];
        else if (a == "--locate") std::sscanf(argv[++i], "%d,%d,%d", &locate_x, &locate_y, &locate_tic);
        else if (a == "--state") state = true;
        else if (a == "--sim-args") g_sim_args = argv[++i];
        else if (a == "--sim-wad") { std::ifstream sw(argv[++i], std::ios::binary); g_sim_wad.assign(std::istreambuf_iterator<char>(sw), std::istreambuf_iterator<char>()); }
        else if (a == "--no-pixels") no_pixels = true;
    }
    Engine full, sim;
    if (!full.load(argv[1], false) || !sim.load(argv[2], true)) return 1;
    g_sim_rt = sim.rt;
    if (!full.call(full.init)) return 1;
    const std::string full_args = g_env_args;
    if (!g_sim_args.empty()) g_env_args = g_sim_args;
    if (!sim.call(sim.init)) return 1;
    g_env_args = full_args;
    dl_ctx ctx;
    const uint32_t hl = dl_head_len(g_pack.data());
    if (dl_init(&ctx, 0, g_pack.data(), hl)) { std::printf("not a version 2 render pack\n"); return 1; }

    // the bench's course, without its cheat: cheats are the status bar's, which the sim
    // leaves out (as a netgame does), and a player that dies in one engine only diverges
    const int UP = 0xad, LEFT = 0xac, RIGHT = 0xae, FIRE = 0xa3, USE = 0xa2;
    std::vector<std::vector<int>> course;
    const int pattern[][3] = {{UP, 0, 70}, {RIGHT, UP, 20}, {UP, FIRE, 40}, {LEFT, 0, 30}, {UP, USE, 50}, {RIGHT, FIRE, 35}, {UP, 0, 60}, {LEFT, UP, 25}};
    if (spin) course.push_back({10, 1, RIGHT});
    // a tap of the fire key once the pistol is up: pressed and released before the same tic
    const int tap_tic = 30, tap_seen_by = 45;
    course.push_back({tap_tic, 1, FIRE});
    course.push_back({tap_tic, 0, FIRE});
    int tap_ammo = -1;
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
        if (state && full.digest && sim.digest) {
            const uint32_t a = full.call1(full.digest, 0), b = sim.call1(sim.digest, 0);
            if (a != b) {
                static const char* groups[] = {"", "players", "mobjs", "sectors", "P_Random/time", "other thinkers", "M_Random"};
                std::printf("tic %d: the game states differ:", t);
                for (int g = 1; g <= 6; g++) if (full.call1(full.digest, g) != sim.call1(sim.digest, g)) std::printf(" %s", groups[g]);
                std::printf("\n");
                for (int i = 0; i < 4096; i++) {
                    if (full.call2(full.mobj, i, -1) != 1 && sim.call2(sim.mobj, i, -1) != 1) break;
                    for (int f = 0; f < 21; f++) {
                        const int32_t fa = full.call2(full.mobj, i, f), fb = sim.call2(sim.mobj, i, f);
                        if (fa != fb) std::printf("  mobj %d field %d: full %d sim %d (type %d, state %d)\n", i, f, fa, fb, full.call2(full.mobj, i, 4), full.call2(full.mobj, i, 6));
                    }
                }
                return 4;
            }
        }
        if (t == tap_tic - 1 || t == tap_seen_by) {   // the ammo the engine's record shows, before the tap and after
            full.call(full.hud);
            const uint8_t* h = full.mem() + full.result(full.hud);
            const int ammo = (int16_t)(h[6] | h[7] << 8);
            if (t == tap_tic - 1) tap_ammo = ammo;
            else if (ammo >= tap_ammo) { std::printf("tic %d: a tap of the fire key at tic %d fired nothing (ammo %d, then %d)\n", t, tap_tic, tap_ammo, ammo); return 5; }
            else std::printf("tic %d: a tap of the fire key at tic %d fired (ammo %d, then %d)\n", t, tap_tic, tap_ammo, ammo);
        }
        if (no_pixels) { frames++; continue; }
        if (!full.call(full.render) || !sim.call(sim.render)) return 1;
        if (state && full.rdigest && sim.rdigest && full.call1(full.rdigest, 0) != sim.call1(sim.rdigest, 0)) {
            static const char* parts[] = {"drawsegs", "vissprites", "visplanes", "openings"};
            std::printf("tic %d: the renderers' frames differ:", t);
            for (int g = 10; g <= 13; g++) if (full.call1(full.rdigest, g) != sim.call1(sim.rdigest, g)) std::printf(" %s", parts[g - 10]);
            std::printf("\n");
            return 4;
        }
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
        {   // the status bar's numbers after the entries, against the engine's player
            const uint32_t entries = list[4] | (list[5] << 8);
            full.call(full.hud);
            const uint8_t* want_hud = full.mem() + full.result(full.hud);
            if (ll != entries + 12 || std::memcmp(list + entries, want_hud, 12)) {
                std::printf("tic %d: the status bar's record differs from the engine's player\n", t);
                return 4;
            }
        }
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
        // the full engine's screen is SCREENWIDTH wide: the view's width here, rows 0 .. vh - 1; at low
        // detail (--detail 1) the list's columns are half as many, each two pixels wide on the screen
        const uint32_t ds = list[11] & 1;
        vw <<= ds;
        frame.assign((size_t)vw * vh, 0);
        band.assign((size_t)vw * 27, 0);
        const uint64_t t0 = now_us();
        for (uint32_t y0 = 0; y0 < vh; y0 += 27) {
            const uint32_t rows = std::min<uint32_t>(27, vh - y0);
            int32_t rc = dl_draw(&ctx, list, ll, band.data(), vw, (int32_t)y0, rows);
            if (rc) { std::printf("tic %d: decode rc %d (entry type %u at %u of %u)\n", t, rc, dl_error_type, dl_error_at, ll); return 1; }
            if (ds)   // each column two pixels wide, as the view widens it
                for (uint32_t r = 0; r < rows; r++)
                    for (uint32_t x = vw / 2; x-- > 0;) band[r * vw + 2 * x] = band[r * vw + 2 * x + 1] = band[r * vw + x];
            std::memcpy(frame.data() + (size_t)y0 * vw, band.data(), (size_t)rows * vw);
        }
        decode_us += now_us() - t0;
        const uint8_t* want = full.mem() + screen;
        if (t == locate_tic) {
            // draw the list entry by entry (its header's byte count cut after each) and watch the pixel
            std::vector<uint8_t> copy(list, list + ll), f2((size_t)vw * vh, 0);
            uint32_t at = 28, k = 0;
            uint8_t last = 0;
            while (at < (uint32_t)(list[4] | (list[5] << 8))) {
                const uint8_t* e = list + at;
                uint32_t size;
                if (e[0] == 1) size = 18 + 3 * e[3];
                else if (e[0] == 2) size = 9 + 13 * (e[7] | (e[8] << 8));
                else if (e[0] == 3) { const uint32_t x1 = e[3] | (e[4] << 8), x2 = e[5] | (e[6] << 8); size = 25 + 2 * (x2 >= x1 ? x2 - x1 + 1 : 0); }
                else size = 20;
                at += size;
                copy[4] = at & 255; copy[5] = at >> 8;
                std::fill(f2.begin(), f2.end(), 0);
                for (uint32_t y0 = 0; y0 < vh; y0 += 27) {
                    const uint32_t rows = std::min<uint32_t>(27, vh - y0);
                    dl_draw(&ctx, copy.data(), ll, band.data(), vw, (int32_t)y0, rows);
                    std::memcpy(f2.data() + (size_t)y0 * vw, band.data(), (size_t)rows * vw);
                }
                const uint8_t v = f2[(size_t)locate_y * vw + locate_x];
                if (e[0] == 3 || e[0] == 4) {
                    const uint32_t ex1 = e[0] == 3 ? (e[3] | (e[4] << 8)) : (e[1] | (e[2] << 8)), ex2 = e[0] == 3 ? (e[5] | (e[6] << 8)) : ex1;
                    if ((uint32_t)locate_x >= ex1 && (uint32_t)locate_x <= ex2) {
                        std::printf("entry %u (type %u) covers x %d: columns %u..%u", k, e[0], locate_x, ex1, ex2);
                        if (e[0] == 3) std::printf(", clips at x: ceiling %d floor %d", (int)e[25 + 2 * (locate_x - ex1)] - 2, (int)e[26 + 2 * (locate_x - ex1)] - 2);
                        std::printf("; pixel now %u\n", v);
                        if (e[0] == 3) {   // the column's posts as the kernel walks them
                            auto i32 = [&](const uint8_t* q) { return (int32_t)(q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24)); };
                            const uint32_t lump = e[1] | (e[2] << 8), idx = lump - ctx.first_sprite;
                            const uint32_t off = (uint32_t)(ctx.sprite_table[8 * idx] | (ctx.sprite_table[8 * idx + 1] << 8) | (ctx.sprite_table[8 * idx + 2] << 16) | ((uint32_t)ctx.sprite_table[8 * idx + 3] << 24));
                            const uint32_t size = (uint32_t)(ctx.sprite_table[8 * idx + 4] | (ctx.sprite_table[8 * idx + 5] << 8) | (ctx.sprite_table[8 * idx + 6] << 16) | ((uint32_t)ctx.sprite_table[8 * idx + 7] << 24));
                            const int32_t scale = i32(e + 9), xiscale = i32(e + 13), startfrac = i32(e + 17), texturemid = i32(e + 21);
                            const uint32_t col = (uint32_t)((int32_t)((uint32_t)startfrac + (uint32_t)xiscale * (uint32_t)(locate_x - ex1)) >> 16);
                            const uint8_t* patch = g_pack.data() + off;
                            const uint32_t w = patch[0] | (patch[1] << 8), colofs = (uint32_t)i32(patch + 8 + 4 * col);
                            const int32_t sprtop = (ctx.viewheight / 2 << 16) - (int32_t)(((int64_t)texturemid * scale) >> 16);
                            std::printf("  lump %u at pack %u (%u bytes) width %u column %u at +%u, sprtopscreen %d, iscale %d\n", lump, off, size, w, col, colofs, sprtop, xiscale);
                            {   // the kernel alone on this one column, into a band around the pixel
                                uint8_t rec[24] = {0};
                                auto st32 = [](uint8_t* q, uint32_t v) { q[0] = v; q[1] = v >> 8; q[2] = v >> 16; q[3] = v >> 24; };
                                st32(rec, off); st32(rec + 4, (uint32_t)(xiscale < 0 ? -xiscale : xiscale)); st32(rec + 8, (uint32_t)texturemid); st32(rec + 12, (uint32_t)scale);
                                rec[16] = locate_x & 255; rec[17] = locate_x >> 8; rec[18] = col & 255; rec[19] = col >> 8;
                                rec[20] = e[25 + 2 * (locate_x - ex1)]; rec[21] = e[26 + 2 * (locate_x - ex1)]; rec[22] = e[7]; rec[23] = 1;
                                std::vector<uint8_t> b1((size_t)vw * 8, 255);
                                uint32_t badr = 0;
                                const int32_t rc1 = kube32::runtime::raster_columns(b1.data(), vw, locate_y - 2, 8, rec, 1, g_pack.data() + ctx.colormap, nullptr,
                                                                                    ctx.viewheight / 2, &resolve, nullptr, &badr);
                                std::printf("  kernel alone: rc %d, rows %d..%d at x: ", rc1, locate_y - 2, locate_y + 5);
                                for (int r = 0; r < 8; r++) std::printf("%u ", b1[(size_t)r * vw + locate_x]);
                                std::printf("(texturemid %d scale %d light %u)\n", texturemid, scale, e[7]);
                            }
                            for (const uint8_t* post = patch + colofs; *post != 0xff; post += post[1] + 4) {
                                const int32_t top = sprtop + scale * post[0], bottom = top + scale * post[1];
                                std::printf("    post topdelta %u length %u: rows %d..%d (post at pack %u, %s lump end)\n", post[0], post[1], (top + 65535) >> 16, (bottom - 1) >> 16,
                                            (uint32_t)(post - g_pack.data()), (uint32_t)(post - g_pack.data()) + 4 + post[1] + 128 > off + size ? "window past" : "inside");
                            }
                        }
                    }
                }
                if (v != last) {
                    std::printf("entry %u (type %u at %u, %u bytes) writes pixel (%d,%d) = %u; the engine has %u\n", k, e[0], at - size, size,
                                locate_x, locate_y, v, want[(size_t)locate_y * vw + locate_x]);
                    if (e[0] == 3) std::printf("  sprite lump %u x %u..%u light %u trans %u scale %d xiscale %d startfrac %d texturemid %d\n",
                                               e[1] | (e[2] << 8), e[3] | (e[4] << 8), e[5] | (e[6] << 8), e[7], e[8],
                                               (int32_t)(e[9] | (e[10] << 8) | (e[11] << 16) | ((uint32_t)e[12] << 24)),
                                               (int32_t)(e[13] | (e[14] << 8) | (e[15] << 16) | ((uint32_t)e[16] << 24)),
                                               (int32_t)(e[17] | (e[18] << 8) | (e[19] << 16) | ((uint32_t)e[20] << 24)),
                                               (int32_t)(e[21] | (e[22] << 8) | (e[23] << 16) | ((uint32_t)e[24] << 24)));
                    if (e[0] == 1) {
                        const uint32_t x0 = e[1] | (e[2] << 8), n = e[3], tex = e[4], light = e[5];
                        auto i32 = [&](int o) { return (int32_t)(e[o] | (e[o + 1] << 8) | (e[o + 2] << 16) | ((uint32_t)e[o + 3] << 24)); };
                        std::printf("  strip x0 %u n %u tex %u light %u texturemid %d scale0 %d scalestep %d\n", x0, n, tex, light, i32(6), i32(10), i32(14));
                        if ((uint32_t)locate_x >= x0 && (uint32_t)locate_x < x0 + n) {
                            const uint32_t k = locate_x - x0;
                            const uint8_t* c = e + 18 + 3 * k;
                            const uint32_t scale = (uint32_t)i32(10) + k * (uint32_t)i32(14), iscale = 0xffffffffu / scale;
                            const uint32_t idx = ((uint32_t)i32(6) + (uint32_t)(locate_y - ctx.viewheight / 2) * iscale) >> 16;
                            const uint32_t table = ctx.tex_table[8 * tex + 4] | (ctx.tex_table[8 * tex + 5] << 8) | (ctx.tex_table[8 * tex + 6] << 16) | ((uint32_t)ctx.tex_table[8 * tex + 7] << 24);
                            const uint32_t coff = g_pack[table + 4 * c[2]] | (g_pack[table + 4 * c[2] + 1] << 8) | (g_pack[table + 4 * c[2] + 2] << 16) | ((uint32_t)g_pack[table + 4 * c[2] + 3] << 24);
                            const uint16_t tw = ctx.tex_table[8 * tex] | (ctx.tex_table[8 * tex + 1] << 8), th = ctx.tex_table[8 * tex + 2] | (ctx.tex_table[8 * tex + 3] << 8);
                            std::printf("  column %u: yl %u yh %u texture column %u (texture %ux%u, column table at %u, texels at %u), scale %u iscale %u, texel index %u (&127 %u), texel %u\n",
                                        locate_x, c[0], c[1], c[2], tw, th, table, coff, scale, iscale, idx, idx & 127, g_pack[coff + (idx & 127)]);
                            std::printf("  texels around:");
                            for (int d = -3; d <= 3; d++) std::printf(" [%d]=%u", (int)(idx & 127) + d, g_pack[coff + ((idx + d) & 127)]);
                            std::printf("\n");
                        }
                    }
                    if (e[0] == 4) std::printf("  masked x %u tex %u col %u light %u\n", e[1] | (e[2] << 8), e[3], e[4], e[5]);
                    last = v;
                }
                k++;
            }
        }
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
    if (no_pixels) { std::printf("%llu tics: the game states agree\n", (unsigned long long)frames); return 0; }
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
