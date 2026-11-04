// SPDX-License-Identifier: GPL-2.0-or-later
// doombench: plays the DOOM pod (build/doom.wasm) on the host's wasm3, with
// the interpreter built as the boards build it (no tail calls, operations
// counted), and reports what a game tic and a frame cost: wasm3 operations,
// gas units and host time, and the pod's memory.
//
//   doombench <doom.wasm> <iwad> [--tics N] [--memory BYTES] [--args "..."]
//             [--iwad-name freedoom1.wad] [--render-every N] [--quiet]
//             [--view-only] [--stats-mode 0|1|2] [--screenblocks 10|11] [--spin]
//
// --view-only draws the 3D view alone (doom_render_view), without the status
// bar and HUD. --stats-mode needs build/doom-stats.wasm (make stats): 1
// replaces the column and span rasterizers with counters, 2 also answers
// texture and flat lookups with dummy data; the bench then prints the draw
// list a frame would carry and the texture working set a frame touches.
//
// The clock advances 1/35 s a tic. The player types iddqd, then walks a
// fixed course: forward, turning, firing, using doors.
#include "wasm3.h"
#include "m3_env.h"
#include "runtime/pod_memory_pool.h"

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

#if d_m3CountOpFrames
extern "C" u64 m3_opCount;
#else
static u64 m3_opCount;   // this build does not count
#endif

static std::vector<uint8_t> g_wad;
static std::string g_env_iwad = "freedoom1.wad", g_env_args = "-warp 1 1 -skill 3", g_env_blocks = "10";
static int64_t g_clock_ms;
static bool g_quiet;
static std::string g_resp;

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

m3ApiRawFunction(h_log) {
    m3ApiGetArgMem(const char*, msg);
    m3ApiGetArg(uint32_t, len);
    if (!g_quiet) std::printf("[pod] %.*s\n", (int)len, msg);
    m3ApiSuccess();
}
m3ApiRawFunction(h_time_ms) {
    m3ApiReturnType(int64_t);
    m3ApiReturn(g_clock_ms);
}
m3ApiRawFunction(h_time_us) {
    m3ApiReturnType(int64_t);
    m3ApiReturn((int64_t)now_us());
}
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
m3ApiRawFunction(h_blob_open) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, name);
    m3ApiGetArg(uint32_t, len);
    (void)name; (void)len;
    m3ApiReturn(0);
}
m3ApiRawFunction(h_blob_size) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    (void)h;
    m3ApiReturn((int32_t)g_wad.size());
}
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
m3ApiRawFunction(h_blob_close) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    (void)h;
    m3ApiReturn(0);
}
m3ApiRawFunction(h_resp_begin) {
    m3ApiGetArg(int32_t, status);
    m3ApiGetArg(uint32_t, a);
    m3ApiGetArg(uint32_t, b);
    (void)status; (void)a; (void)b;
    g_resp.clear();
    m3ApiSuccess();
}
m3ApiRawFunction(h_resp_header) {
    m3ApiGetArg(uint32_t, a);
    m3ApiGetArg(uint32_t, b);
    m3ApiGetArg(uint32_t, c);
    m3ApiGetArg(uint32_t, d);
    (void)a; (void)b; (void)c; (void)d;
    m3ApiSuccess();
}
m3ApiRawFunction(h_resp_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, p);
    m3ApiGetArg(uint32_t, len);
    if (len < 4096) g_resp.append(p, len);
    m3ApiReturn((int32_t)len);
}
m3ApiRawFunction(h_resp_end) { m3ApiSuccess(); }

struct Cost { uint64_t ops, gas, us; };

static IM3Runtime g_rt;
static IM3Module g_mod;

static Cost call(IM3Function fn, int argc, const char** argv, const char* what) {
    m3_SetResourceLimit(g_rt, c_m3Limit_GasUnits, (uint64_t)INT64_MAX);
    const uint64_t ops0 = m3_opCount, t0 = now_us();
    M3Result r = m3_CallArgv(fn, argc, argv);
    Cost c{m3_opCount - ops0, m3_GetResourceUsage(g_rt, c_m3Limit_GasUnits), now_us() - t0};
    if (r) {
        M3ErrorInfo info;
        m3_GetErrorInfo(g_rt, &info);
        std::printf("%s: %s %s\n", what, r, info.message ? info.message : "");
        std::exit(1);
    }
    return c;
}

static std::string request(IM3Function alloc, IM3Function handle, const std::string& req) {
    std::string n = std::to_string(req.size() + 1);
    const char* a[] = {n.c_str()};
    call(alloc, 1, a, "k32_alloc");
    uint32_t ptr = 0;
    m3_GetResultsV(alloc, &ptr);
    uint8_t* mem = m3_GetMemory(g_mod, nullptr, 0);
    std::memcpy(mem + ptr, req.data(), req.size());
    std::string p = std::to_string(ptr), l = std::to_string(req.size());
    const char* h[] = {p.c_str(), l.c_str()};
    call(handle, 2, h, "k32_handle");
    return g_resp;
}

static void summary(const char* name, std::vector<Cost>& v) {
    if (v.empty()) return;
    auto stat = [&](auto get, const char* unit) {
        std::vector<uint64_t> x;
        for (auto& c : v) x.push_back(get(c));
        std::sort(x.begin(), x.end());
        uint64_t sum = 0;
        for (auto e : x) sum += e;
        std::printf("  %-6s %-4s mean %10llu  p50 %10llu  p95 %10llu  max %10llu\n", name, unit, (unsigned long long)(sum / x.size()),
                    (unsigned long long)x[x.size() / 2], (unsigned long long)x[x.size() * 95 / 100], (unsigned long long)x.back());
    };
    stat([](const Cost& c) { return c.ops; }, "ops");
    stat([](const Cost& c) { return c.gas; }, "gas");
    stat([](const Cost& c) { return c.us; }, "us");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: doombench <doom.wasm> <iwad> [--tics N] [--memory BYTES] [--args \"...\"] [--iwad-name NAME] [--render-every N]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> wasm((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::ifstream w(argv[2], std::ios::binary);
    g_wad.assign(std::istreambuf_iterator<char>(w), std::istreambuf_iterator<char>());
    int tics = 1050, render_every = 1, stats_mode = -1;
    bool view_only = false, spin = false;
    uint64_t memory = 0;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--tics") tics = std::atoi(argv[++i]);
        else if (a == "--memory") memory = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--args") g_env_args = argv[++i];
        else if (a == "--iwad-name") g_env_iwad = argv[++i];
        else if (a == "--render-every") render_every = std::atoi(argv[++i]);
        else if (a == "--quiet") g_quiet = true;
        else if (a == "--view-only") view_only = true;
        else if (a == "--spin") spin = true;   // the player also turns right the whole time: the view sweeps the room
        else if (a == "--stats-mode") stats_mode = std::atoi(argv[++i]);
        else if (a == "--screenblocks") g_env_blocks = argv[++i];   // 11: the whole screen, no status bar
    }
    // everything wasm3 allocates for the pod is charged to one account, as on a node
    const int account = kube32_pod_account_open();
    kube32_pod_account_enter(account);
    IM3Environment env = m3_NewEnvironment();
    g_rt = m3_NewRuntime(env, 64 * 1024, nullptr);
    if (memory) m3_SetResourceLimit(g_rt, c_m3Limit_MemoryBytes, memory);
    IM3Module mod = nullptr;
    M3Result r = m3_ParseModule(env, &mod, wasm.data(), (uint32_t)wasm.size());
    if (!r) r = m3_LoadModule(g_rt, mod);
    g_mod = mod;
    if (r) { std::printf("load: %s\n", r); return 1; }
    struct { const char* name; const char* sig; M3RawCall fn; } imports[] = {
        {"k32_log", "v(ii)", h_log}, {"k32_time_ms", "I()", h_time_ms}, {"esp32_get_time_us", "I()", h_time_us},
        {"k32_env_get", "i(iiii)", h_env_get}, {"k32_blob_open", "i(ii)", h_blob_open}, {"k32_blob_size", "i(i)", h_blob_size},
        {"k32_blob_read", "i(iiii)", h_blob_read}, {"k32_blob_close", "i(i)", h_blob_close}, {"k32_resp_begin", "v(iii)", h_resp_begin},
        {"k32_resp_header", "v(iiii)", h_resp_header}, {"k32_resp_write", "i(ii)", h_resp_write}, {"k32_resp_end", "v()", h_resp_end},
    };
    for (auto& im : imports) {
        r = m3_LinkRawFunction(mod, "env", im.name, im.sig, im.fn);
        if (r && r != m3Err_functionLookupFailed) { std::printf("link %s: %s\n", im.name, r); return 1; }
    }
    IM3Function init, tic, render, key, alloc, handle;
    if ((r = m3_FindFunction(&init, g_rt, "k32_init")) || (r = m3_FindFunction(&tic, g_rt, "doom_run_tic")) ||
        (r = m3_FindFunction(&render, g_rt, "doom_render")) || (r = m3_FindFunction(&key, g_rt, "doom_key")) ||
        (r = m3_FindFunction(&alloc, g_rt, "k32_alloc")) || (r = m3_FindFunction(&handle, g_rt, "k32_handle"))) {
        std::printf("exports: %s\n", r);
        return 1;
    }
    IM3Function render_view = nullptr, stats_set = nullptr, stats_read = nullptr, dl_len = nullptr, dl_count = nullptr, dl_trunc = nullptr;
    if (m3_FindFunction(&dl_len, g_rt, "doom_drawlist_len")) dl_len = nullptr;
    if (dl_len && (m3_FindFunction(&dl_count, g_rt, "doom_drawlist_count") || m3_FindFunction(&dl_trunc, g_rt, "doom_drawlist_truncated"))) dl_len = nullptr;
    if (view_only && (r = m3_FindFunction(&render_view, g_rt, "doom_render_view"))) { std::printf("doom_render_view: %s\n", r); return 1; }
    if (stats_mode >= 0 && ((r = m3_FindFunction(&stats_set, g_rt, "doom_stats_mode")) || (r = m3_FindFunction(&stats_read, g_rt, "doom_stats_read")))) {
        std::printf("stats exports (build with make stats): %s\n", r);
        return 1;
    }
    Cost ci = call(init, 0, nullptr, "k32_init");
    if (stats_set) {
        std::string m = std::to_string(stats_mode);
        const char* a[] = {m.c_str()};
        call(stats_set, 1, a, "doom_stats_mode");
    }
    std::printf("init: %llu ops, %llu gas, %llu us\n", (unsigned long long)ci.ops, (unsigned long long)ci.gas, (unsigned long long)ci.us);
    std::printf("stats after init: %s\n", request(alloc, handle, "GET /stats\n\n").c_str());
    std::printf("memory after init: runtime account %zu bytes, linear memory %zu bytes, module %zu bytes\n",
                kube32_pod_account_bytes(account), m3_GetMemorySize(g_mod, 0), wasm.size());

    auto press = [&](int down, int code) {
        std::string d = std::to_string(down), c = std::to_string(code);
        const char* a[] = {d.c_str(), c.c_str()};
        call(key, 2, a, "doom_key");
    };
    // The course: (tic, down, key)
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
    // the draw-list counters of port/drawlist_stats.c, in its field order
    enum { WALL_COLS, WALL_PX, SKY_COLS, SPRITE_POSTS, SPRITE_PX, FUZZ_POSTS, TRANS_POSTS, SPRITES, SPANS, SPAN_PX, GETCOLUMN_CALLS,
           TEX_COLS, TEX_COL_BYTES, FLATS, FLAT_BYTES, SPRITE_LUMPS, SPRITE_LUMP_BYTES, TEXTURES, NSTATS };
    static const char* stat_names[NSTATS] = {"wall_cols", "wall_px", "sky_cols", "sprite_posts", "sprite_px", "fuzz_posts", "trans_posts",
                                             "sprites", "spans", "span_px", "getcolumn_calls", "tex_cols", "tex_col_bytes", "flats",
                                             "flat_bytes", "sprite_lumps", "sprite_lump_bytes", "textures"};
    std::vector<std::vector<uint32_t>> frame_stats;
    std::vector<uint32_t> dl_bytes, dl_counts;
    uint32_t dl_truncated = 0;
    std::vector<Cost> tic_costs, frame_costs;
    for (int t = 0; t < tics; t++) {
        for (auto& e : course) if (e[0] == t) press(e[1], e[2]);
        g_clock_ms = (int64_t)(t + 1) * 1000 / 35;
        tic_costs.push_back(call(tic, 0, nullptr, "doom_run_tic"));
        if (render_every && t % render_every == 0) {
            frame_costs.push_back(call(view_only ? render_view : render, 0, nullptr, "doom_render"));
            if (dl_len) {
                uint32_t v = 0;
                call(dl_len, 0, nullptr, "doom_drawlist_len"); m3_GetResultsV(dl_len, &v); dl_bytes.push_back(v);
                call(dl_count, 0, nullptr, "doom_drawlist_count"); m3_GetResultsV(dl_count, &v); dl_counts.push_back(v);
                call(dl_trunc, 0, nullptr, "doom_drawlist_truncated"); m3_GetResultsV(dl_trunc, &v); dl_truncated += v;
            }
            if (stats_read) {
                std::string n = std::to_string(NSTATS * 4);
                const char* a[] = {n.c_str()};
                call(alloc, 1, a, "k32_alloc");
                uint32_t ptr = 0;
                m3_GetResultsV(alloc, &ptr);
                std::string p = std::to_string(ptr);
                const char* b[] = {p.c_str()};
                call(stats_read, 1, b, "doom_stats_read");
                std::vector<uint32_t> v(NSTATS);
                std::memcpy(v.data(), m3_GetMemory(g_mod, nullptr, 0) + ptr, NSTATS * 4);
                frame_stats.push_back(v);
            }
        }
    }
    size_t size = m3_GetMemorySize(g_mod, 0);
    std::printf("played %d tics, %zu frames; linear memory %zu bytes; runtime account %zu bytes (linear memory, wasm3 runtime and compiled code); module %zu bytes\n",
                tics, frame_costs.size(), size, kube32_pod_account_bytes(account), wasm.size());
    // the first 35 tics load the level's lumps on first sight; the rest is play
    std::printf("all tics:\n");
    summary("tic", tic_costs);
    summary("frame", frame_costs);
    if (tic_costs.size() > 70) {
        std::vector<Cost> t2(tic_costs.begin() + 35, tic_costs.end()), f2(frame_costs.size() > 35 ? frame_costs.begin() + 35 : frame_costs.end(), frame_costs.end());
        std::printf("after the first second:\n");
        summary("tic", t2);
        summary("frame", f2);
    }
    if (!dl_bytes.empty()) {
        auto show = [&](const char* name, std::vector<uint32_t> x) {
            size_t from = x.size() > 35 ? 35 : 0;
            x.erase(x.begin(), x.begin() + from);
            std::sort(x.begin(), x.end());
            uint64_t sum = 0;
            for (auto e : x) sum += e;
            std::printf("  %-18s mean %8llu  p50 %8llu  p95 %8llu  max %8llu\n", name, (unsigned long long)(sum / x.size()),
                        (unsigned long long)x[x.size() / 2], (unsigned long long)x[x.size() * 95 / 100], (unsigned long long)x.back());
        };
        std::printf("draw list a frame (the pod's own list, after the first second; %u frames truncated at 16 KiB):\n", dl_truncated);
        show("bytes", dl_bytes);
        show("entries", dl_counts);
    }
    if (!frame_stats.empty()) {
        std::printf("draw list a frame (mode %d, %zu frames, after the first second):\n", stats_mode, frame_stats.size());
        size_t from = frame_stats.size() > 35 ? 35 : 0;
        for (int k = 0; k < NSTATS; k++) {
            std::vector<uint32_t> x;
            for (size_t i = from; i < frame_stats.size(); i++) x.push_back(frame_stats[i][k]);
            std::sort(x.begin(), x.end());
            uint64_t sum = 0;
            for (auto e : x) sum += e;
            std::printf("  %-18s mean %8llu  p50 %8llu  p95 %8llu  max %8llu\n", stat_names[k], (unsigned long long)(sum / x.size()),
                        (unsigned long long)x[x.size() / 2], (unsigned long long)x[x.size() * 95 / 100], (unsigned long long)x.back());
        }
        // A packed draw list: wall column 10 B (x u16, yl u8, yh u8, texture u16, column u8, scale u16, texturemid i16, light u8),
        // sky column 4 B, span 12 B (y u8, x1 u16, x2 u16, flat u8, xfrac i16, yfrac i16, xstep i16, ystep i16, light u8),
        // sprite 16 B (lump u16, x1 u16, x2 u16, scale u16, xiscale i16, startfrac i16, texturemid i16, colormap/flags u16).
        // Sprite posts are not sent: the rasterizer walks the patch's posts itself.
        std::vector<uint64_t> bytes;
        for (size_t i = from; i < frame_stats.size(); i++) {
            auto& v = frame_stats[i];
            bytes.push_back((uint64_t)(v[WALL_COLS] - v[SKY_COLS]) * 10 + (uint64_t)v[SKY_COLS] * 4 + (uint64_t)v[SPANS] * 12 + (uint64_t)v[SPRITES] * 16);
        }
        std::sort(bytes.begin(), bytes.end());
        uint64_t sum = 0;
        for (auto e : bytes) sum += e;
        std::printf("  packed draw list bytes: mean %llu  p50 %llu  p95 %llu  max %llu\n", (unsigned long long)(sum / bytes.size()),
                    (unsigned long long)bytes[bytes.size() / 2], (unsigned long long)bytes[bytes.size() * 95 / 100], (unsigned long long)bytes.back());
    }
    std::printf("stats: %s\n", request(alloc, handle, "GET /stats\n\n").c_str());
    m3_FreeRuntime(g_rt);
    m3_FreeEnvironment(env);
    return 0;
}
