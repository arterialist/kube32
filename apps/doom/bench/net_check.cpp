// SPDX-License-Identifier: GPL-2.0-or-later
// net_check: two-player lockstep (port/net.c) through a simulated network.
// The authority is the sim-only engine (doom-sim.wasm, DOOM_PLAYERS=2) driven
// as a node drives a pod: k32_tick every 10 ms, POST /join and POST /tics
// through k32_handle, its channel events captured. The client is the full
// engine (doom.wasm) driven as the browser page drives it: a frame every
// 16 ms runs the confirmed tics, draws, and posts player 2's ticcmds. Every
// message crosses a relay that delays it (base plus jitter, so messages
// overtake each other), loses some, and goes dark for outages. Both players
// follow scripted key courses.
//
// The game state digests of both sides are compared at every tic both ran.
// The course: lockstep under jitter and loss with an outage, then the client
// reloads and joins again (the map restarts), then the client goes silent
// past the timeout (player 2 leaves the game), then it joins once more.
//
//   net_check <doom-sim.wasm> <doom.wasm> <sim iwad> <iwad> [--tics N] [--delay D] [--jitter MS] [--base MS]
//             [--loss PERCENT] [--outage-ms MS] [--batch N] [--screenblocks N] [--pod-tick-ms MS] [--draw-rate B/s] [--publish-ms N] [--fps N] [--pod-env K=V] [--no-render] [--args "..."] [--client-args "..."] [--iwad-name doom1.wad]
//
// Exits 0 when every compared tic agrees, 4 at the first that does not.
#include "wasm3.h"
#include "m3_env.h"
#include "runtime/k32_app_abi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

static int64_t g_now_ms;
#if d_m3CountOpFrames
extern "C" u32 m3_opFrames, m3_opFramesPeak;
// The deepest call of each export, in operation frames: the native stack a board's interpreter
// takes grows with them (make verify-net builds build/net_check-frames and bounds the pod's)
static std::map<std::string, uint32_t> g_frames_peak;
#endif
// k32_publish as the runtime paces it toward a view that takes g_draw_rate bytes a second
// (--draw-rate): it waits for the last message to leave (at most 500 ms) before it sends,
// and the pod, its POSTs and its tick wait with it until g_pod_busy_until
// (--publish-ms N: a publish itself also takes 0 to 2N ms, as copying and sending it does on a board)
static int64_t g_draw_rate, g_pod_busy_until, g_publish_ms;
static std::mt19937 g_publish_rng{7};
static double g_next_send_ms, g_publish_wait_ms;
static uint64_t g_published;

struct Engine;
static std::map<IM3Runtime, Engine*> g_engines;

struct Engine {
    const char* name = "";
    bool paced = false;                        // its k32_publish is paced (the pod)
    IM3Environment env{};
    IM3Runtime rt{};
    IM3Module mod{};
    std::vector<uint8_t> wasm;
    const std::vector<uint8_t>* wad = nullptr;
    std::map<std::string, std::string> envs;
    int status = 0;
    std::string body;                          // the last response
    std::vector<std::string> events;           // channel messages published
    IM3Function fn(const char* n) {
        IM3Function f = nullptr;
        if (m3_FindFunction(&f, rt, n)) { std::printf("%s: no export %s\n", name, n); std::exit(1); }
        return f;
    }
    uint8_t* mem() { return m3_GetMemory(mod, nullptr, 0); }
    int64_t call(const char* n, std::vector<int64_t> args = {}) {
        IM3Function f = fn(n);
        std::vector<std::string> s;
        for (auto a : args) s.push_back(std::to_string(a));
        std::vector<const char*> argv;
        for (auto& x : s) argv.push_back(x.c_str());
#if d_m3CountOpFrames
        m3_opFramesPeak = m3_opFrames;
#endif
        M3Result r = m3_CallArgv(f, (uint32_t)argv.size(), argv.data());
#if d_m3CountOpFrames
        uint32_t& peak = g_frames_peak[std::string(name) + " " + n];
        if (m3_opFramesPeak > peak) peak = m3_opFramesPeak;
#endif
        if (r) {
            M3ErrorInfo info;
            m3_GetErrorInfo(rt, &info);
            std::printf("%s: %s: %s %s\n", name, n, r, info.message ? info.message : "");
            std::exit(1);
        }
        if (m3_GetRetCount(f) == 0) return 0;
        if (m3_GetRetType(f, 0) == c_m3Type_i64) { int64_t v = 0; m3_GetResultsV(f, &v); return v; }
        uint32_t v = 0;
        m3_GetResultsV(f, &v);
        return (int32_t)v;
    }
    // a request through k32_handle; the answer in status and body
    void request(const std::string& method, const std::string& path, const std::string& req_body, const std::string& headers = "") {
        const std::string req = method + " " + path + "\nContent-Type: text/plain\n" + headers + "\n" + req_body;
        const uint32_t p = (uint32_t)call("k32_alloc", {(int64_t)req.size() + 1});
        if (!p) { std::printf("%s: k32_alloc refused %zu bytes\n", name, req.size()); std::exit(1); }
        std::memcpy(mem() + p, req.data(), req.size());
        status = 0;
        body.clear();
        call("k32_handle", {p, (int64_t)req.size()});
    }
    std::string out(int32_t len) { return std::string((const char*)mem() + (uint32_t)call("doom_net_out"), (size_t)len); }
    int32_t recv(const std::string& msg) {
        std::memcpy(mem() + (uint32_t)call("doom_net_in"), msg.data(), msg.size());
        return (int32_t)call("doom_net_recv", {(int64_t)msg.size()});
    }
};

static Engine* eng(IM3Runtime rt) { return g_engines[rt]; }

m3ApiRawFunction(h_log) {
    m3ApiGetArgMem(const char*, msg);
    m3ApiGetArg(uint32_t, len);
    if (std::getenv("NET_LOG") || (len > 9 && !std::strncmp(msg, "doom-net:", 9))) std::printf("  [%s] %.*s\n", eng(runtime)->name, (int)len, msg);
    m3ApiSuccess();
}
// the pod's clock runs on through a paced k32_publish, so the pod sees how long it waited
m3ApiRawFunction(h_time_ms) { m3ApiReturnType(int64_t); m3ApiReturn(eng(runtime)->paced ? std::max(g_now_ms, g_pod_busy_until) : g_now_ms); }
m3ApiRawFunction(h_time_us) { m3ApiReturnType(int64_t); m3ApiReturn(g_now_ms * 1000); }
m3ApiRawFunction(h_env_get) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, key);
    m3ApiGetArg(uint32_t, key_len);
    m3ApiGetArgMem(char*, out);
    m3ApiGetArg(uint32_t, cap);
    auto& envs = eng(runtime)->envs;
    auto it = envs.find(std::string(key, key_len));
    if (it == envs.end()) m3ApiReturn(-1);
    const uint32_t n = std::min<uint32_t>(cap, (uint32_t)it->second.size());
    std::memcpy(out, it->second.data(), n);
    m3ApiReturn((int32_t)it->second.size());
}
m3ApiRawFunction(h_blob_open) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, n);
    m3ApiGetArg(uint32_t, l);
    m3ApiReturn(std::string(n, l) == eng(runtime)->envs["DOOM_WAD_BLOB"] ? 0 : K32_ERR_NOENT);
}
m3ApiRawFunction(h_blob_size) { m3ApiReturnType(int32_t); m3ApiGetArg(int32_t, h); (void)h; m3ApiReturn((int32_t)eng(runtime)->wad->size()); }
m3ApiRawFunction(h_blob_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(uint32_t, off);
    m3ApiGetArgMem(uint8_t*, dst);
    m3ApiGetArg(uint32_t, len);
    (void)h;
    m3ApiCheckMem(dst, len);
    const auto& wad = *eng(runtime)->wad;
    if (off >= wad.size()) m3ApiReturn(0);
    const uint32_t n = std::min<uint32_t>(len, (uint32_t)wad.size() - off);
    std::memcpy(dst, wad.data() + off, n);
    m3ApiReturn((int32_t)n);
}
m3ApiRawFunction(h_blob_close) { m3ApiReturnType(int32_t); m3ApiGetArg(int32_t, h); (void)h; m3ApiReturn(0); }
m3ApiRawFunction(h_resp_begin) {
    m3ApiGetArg(int32_t, s);
    m3ApiGetArg(uint32_t, a);
    m3ApiGetArg(uint32_t, b);
    (void)a; (void)b;
    eng(runtime)->status = s;
    m3ApiSuccess();
}
m3ApiRawFunction(h_resp_header) { m3ApiGetArg(uint32_t, a); m3ApiGetArg(uint32_t, b); m3ApiGetArg(uint32_t, c); m3ApiGetArg(uint32_t, d); (void)a; (void)b; (void)c; (void)d; m3ApiSuccess(); }
m3ApiRawFunction(h_resp_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiCheckMem(p, len);
    eng(runtime)->body.append(p, len);
    m3ApiReturn((int32_t)len);
}
m3ApiRawFunction(h_resp_end) { m3ApiSuccess(); }
m3ApiRawFunction(h_publish) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, topic);
    m3ApiGetArg(uint32_t, tlen);
    m3ApiGetArgMem(const char*, data);
    m3ApiGetArg(uint32_t, len);
    (void)topic; (void)tlen;
    m3ApiCheckMem(data, len);
    if (len > 16 * 1024) m3ApiReturn(K32_ERR_INVAL);
    g_published++;
    const double cost = g_publish_ms ? (double)(g_publish_rng() % (uint32_t)(2 * g_publish_ms + 1)) : 0.0;
    double now = (double)std::max(g_now_ms, g_pod_busy_until), wait = 0.0;
    if (g_draw_rate > 0) {
        wait = g_next_send_ms > now ? std::min(g_next_send_ms - now, 500.0) : 0.0;
        g_next_send_ms = std::max(g_next_send_ms, now) + len * 1000.0 / (double)g_draw_rate;
        g_publish_wait_ms += wait;
    }
    g_pod_busy_until = (int64_t)(now + wait + cost + 0.5);
    m3ApiReturn((int32_t)len);
}
m3ApiRawFunction(h_event_emit) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, topic);
    m3ApiGetArg(uint32_t, tlen);
    m3ApiGetArgMem(const char*, data);
    m3ApiGetArg(uint32_t, len);
    (void)topic; (void)tlen;
    m3ApiCheckMem(data, len);
    if (len > K32_MAX_EVENT_BYTES) m3ApiReturn(K32_ERR_INVAL);
    eng(runtime)->events.emplace_back(data, len);
    m3ApiReturn((int32_t)len);
}

static bool load(Engine& e, const char* path) {
    std::ifstream f(path, std::ios::binary);
    e.wasm.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (e.wasm.empty()) { std::printf("%s: cannot read\n", path); return false; }
    e.env = m3_NewEnvironment();
    e.rt = m3_NewRuntime(e.env, 64 * 1024, nullptr);
    M3Result r = m3_ParseModule(e.env, &e.mod, e.wasm.data(), (uint32_t)e.wasm.size());
    if (!r) r = m3_LoadModule(e.rt, e.mod);
    if (r) { std::printf("%s: %s\n", path, r); return false; }
    struct { const char* name; const char* sig; M3RawCall fn; } imports[] = {
        {"k32_log", "v(ii)", h_log}, {"k32_time_ms", "I()", h_time_ms}, {"esp32_get_time_us", "I()", h_time_us},
        {"k32_env_get", "i(iiii)", h_env_get}, {"k32_blob_open", "i(ii)", h_blob_open}, {"k32_blob_size", "i(i)", h_blob_size},
        {"k32_blob_read", "i(iiii)", h_blob_read}, {"k32_blob_close", "i(i)", h_blob_close}, {"k32_resp_begin", "v(iii)", h_resp_begin},
        {"k32_resp_header", "v(iiii)", h_resp_header}, {"k32_resp_write", "i(ii)", h_resp_write}, {"k32_resp_end", "v()", h_resp_end},
        {"k32_event_emit", "i(iiii)", h_event_emit},
        {"k32_publish", "i(iiii)", h_publish},      /* the draw list to the view pod: paced, not followed */
    };
    for (auto& im : imports) {
        r = m3_LinkRawFunction(e.mod, "env", im.name, im.sig, im.fn);
        if (r && r != m3Err_functionLookupFailed) { std::printf("link %s: %s\n", im.name, r); return false; }
    }
    g_engines[e.rt] = &e;
    return true;
}
static void unload(Engine& e) {
    g_engines.erase(e.rt);
    m3_FreeRuntime(e.rt);
    m3_FreeEnvironment(e.env);
    e.rt = nullptr;
    e.env = nullptr;
}

static long json_int(const std::string& s, const char* key, long def) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t at = s.find(pat);
    if (at == std::string::npos) return def;
    return std::strtol(s.c_str() + at + pat.size(), nullptr, 10);
}
static std::string json_str(const std::string& s, const char* key) {
    const std::string pat = std::string("\"") + key + "\":\"";
    size_t at = s.find(pat);
    if (at == std::string::npos) return "";
    at += pat.size();
    return s.substr(at, s.find('"', at) - at);
}

// ---- the network: each message lands after base + jitter, or never ----
struct Msg {
    int64_t at;
    uint64_t seq;
    int to;                 // 0 the authority (a POST), 1 the client (an event or an answer)
    int kind;               // 0 event, 1 POST, 2 answer
    int gen;                // the client it belongs to
    std::string body;
    uint64_t id;            // a POST and its answer
};
struct Net {
    std::mt19937 rng{20261105};
    int base_ms = 5, jitter_ms = 40, loss_pct = 2;
    int64_t outage_from = -1, outage_to = -1;
    uint64_t seq = 0, sent = 0, lost = 0, reordered = 0;
    std::vector<Msg> q;
    int64_t last_at[3] = {0, 0, 0};
    void send(int to, int kind, int gen, std::string body, uint64_t id = 0) {
        sent++;
        const bool dark = g_now_ms >= outage_from && g_now_ms < outage_to;
        if (dark || (int)(rng() % 100) < loss_pct) { lost++; return; }
        const int64_t at = g_now_ms + base_ms + (jitter_ms ? (int64_t)(rng() % (uint32_t)(jitter_ms + 1)) : 0);
        if (at < last_at[kind]) reordered++;
        last_at[kind] = std::max(last_at[kind], at);
        q.push_back({at, seq++, to, kind, gen, std::move(body), id});
    }
    // the messages due by now, in arrival order
    std::vector<Msg> due() {
        std::vector<Msg> d;
        for (size_t i = 0; i < q.size();) {
            if (q[i].at <= g_now_ms) { d.push_back(std::move(q[i])); q[i] = std::move(q.back()); q.pop_back(); }
            else i++;
        }
        std::sort(d.begin(), d.end(), [](const Msg& a, const Msg& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; });
        return d;
    }
};

// a key course: (tic offset, down/up, key), repeated
struct Course {
    std::vector<std::vector<int>> events;
    size_t next = 0;
    Course(const int (*pattern)[3], int n, int tics, int start) {
        for (int t = start; t < tics;) {
            for (int i = 0; i < n; i++) {
                for (int k = 0; k < 2; k++)
                    if (pattern[i][k]) { events.push_back({t, 1, pattern[i][k]}); events.push_back({t + pattern[i][2], 0, pattern[i][k]}); }
                t += pattern[i][2] + 1;
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const std::vector<int>& a, const std::vector<int>& b) { return a[0] < b[0]; });
    }
    // the key events up to tic t
    template <class F> void feed(int t, F press) {
        while (next < events.size() && events[next][0] <= t) { press(events[next][1], events[next][2]); next++; }
    }
};

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: net_check <doom-sim.wasm> <doom.wasm> <sim iwad> <iwad> [--tics N] [--delay D] [--jitter MS] [--base MS] "
                             "[--loss PERCENT] [--outage-ms MS] [--batch N] [--screenblocks N] [--pod-tick-ms MS] [--draw-rate B/s] [--publish-ms N] [--fps N] [--pod-env K=V] [--no-render] [--args \"...\"] [--client-args \"...\"] [--iwad-name NAME]\n");
        return 2;
    }
    std::ifstream sw(argv[3], std::ios::binary), cw(argv[4], std::ios::binary);
    const std::vector<uint8_t> sim_wad((std::istreambuf_iterator<char>(sw)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> wad((std::istreambuf_iterator<char>(cw)), std::istreambuf_iterator<char>());
    int tics = 6000, delay = 4, outage_ms = 1500;
    std::string batch = "2", screenblocks = "10";   // --screenblocks 11 for a sim built 240 wide
    int pod_tick_ms = 10;                           // --pod-tick-ms: how often the node ticks the pod
    std::string fps = "8";                          // --fps: DOOM_FPS, the frames the sim publishes a second
    std::vector<std::pair<std::string, std::string>> pod_env;   // --pod-env KEY=VALUE
    bool render = true;
    uint32_t frames_max = 0;                        // --frames-max: the pod's deepest call, in operation frames (net_check-frames)
    const bool trace = std::getenv("NET_TRACE") != nullptr;
    std::string args = "-warp 1 1 -skill 3", iwad_name = "doom1.wad", client_args;   // --client-args: a client that plays another game
    Net net;
    for (int i = 5; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--tics") tics = std::atoi(argv[++i]);
        else if (a == "--delay") delay = std::atoi(argv[++i]);
        else if (a == "--jitter") net.jitter_ms = std::atoi(argv[++i]);
        else if (a == "--base") net.base_ms = std::atoi(argv[++i]);
        else if (a == "--loss") net.loss_pct = std::atoi(argv[++i]);
        else if (a == "--batch") batch = argv[++i];
        else if (a == "--screenblocks") screenblocks = argv[++i];
        else if (a == "--pod-tick-ms") pod_tick_ms = std::max(1, std::atoi(argv[++i]));
        else if (a == "--draw-rate") g_draw_rate = std::atoll(argv[++i]);
        else if (a == "--publish-ms") g_publish_ms = std::atoll(argv[++i]);
        else if (a == "--fps") fps = argv[++i];
        else if (a == "--pod-env") {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) pod_env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (a == "--outage-ms") outage_ms = std::atoi(argv[++i]);
        else if (a == "--no-render") render = false;
        else if (a == "--args") args = argv[++i];
        else if (a == "--iwad-name") iwad_name = argv[++i];
        else if (a == "--client-args") client_args = std::string(" ") + argv[++i];
        else if (a == "--frames-max") frames_max = (uint32_t)std::atol(argv[++i]);
    }

    // the authority: the sim-only engine as the pod
    Engine pod;
    pod.name = "pod";
    pod.paced = true;
    pod.wad = &sim_wad;
    pod.envs = {{"DOOM_IWAD", iwad_name}, {"DOOM_WAD_BLOB", "doom-wad"}, {"DOOM_ARGS", args + " -kb 256"}, {"DOOM_PLAYERS", "2"},
                {"DOOM_NET_DELAY", std::to_string(delay)}, {"DOOM_NET_TIMEOUT_MS", "5000"}, {"DOOM_NET_DIGEST", "1"}, {"DOOM_NET_BATCH", batch}, {"DOOM_SCREENBLOCKS", screenblocks}, {"DOOM_FPS", fps}};
    for (auto& kv : pod_env) pod.envs[kv.first] = kv.second;
    if (!load(pod, argv[1])) return 1;
    if (pod.call("k32_init")) { std::printf("pod: k32_init failed\n"); return 1; }

    // the client: the full engine as the browser page runs it
    Engine client;
    int gen = 0;
    const int UP = 0xad, DOWN = 0xaf, LEFT = 0xac, RIGHT = 0xae, FIRE = 0xa3, USE = 0xa2;
    const int p1_pattern[][3] = {{UP, 0, 70}, {RIGHT, UP, 20}, {UP, FIRE, 40}, {LEFT, 0, 30}, {UP, USE, 50}, {RIGHT, FIRE, 35}, {UP, 0, 60}, {LEFT, UP, 25}};
    const int p2_pattern[][3] = {{UP, 0, 50}, {LEFT, FIRE, 25}, {UP, USE, 45}, {RIGHT, 0, 40}, {DOWN, FIRE, 30}, {UP, RIGHT, 55}, {FIRE, 0, 20}, {LEFT, UP, 35}};
    Course p1(p1_pattern, 8, 1 << 20, 5);
    std::vector<Course> p2s;
    // the page (web/doom-net.js) steps on every message and every 4 ms: it runs the confirmed
    // tics, builds its ticcmds by the clock (doom_net_pace, with the POST round trip), and posts
    // them two at a time, or one that has waited 35 ms, or every 250 ms regardless, up to 4 POSTs
    // in flight; it gives up on one with no answer after 300 ms
    struct Side {
        int32_t session = 0, joined_at = -1;
        std::map<uint64_t, int64_t> in_flight;
        int64_t last_post = -1000, fresh_at = -1;
        double rtt = 0;
        bool alive = true, dirty = false;
    };
    uint64_t post_id = 0;
    Side cs;
    auto start_client = [&]() -> bool {
        if (client.rt) unload(client);
        gen++;
        client = Engine();
        client.name = "browser";
        client.wad = &wad;
        pod.request("GET", "/doom/net", "");
        const std::string hello = pod.body;
        client.envs = {{"DOOM_IWAD", json_str(hello, "iwad")}, {"DOOM_WAD_BLOB", "doom-wad"},
                       {"DOOM_ARGS", json_str(hello, "args") + " -lockstep 2 -consoleplayer 1 -kb 1024" + client_args}};
        if (!load(client, argv[2])) return false;
        if (client.call("k32_init")) { std::printf("browser: k32_init failed\n"); return false; }
        pod.request("POST", "/doom/join", "");
        if (pod.status != 200) { std::printf("join: %d %s\n", pod.status, pod.body.c_str()); return false; }
        cs = Side();
        cs.session = (int32_t)json_int(pod.body, "g", -1);
        cs.joined_at = (int32_t)json_int(pod.body, "s", -1);
        client.call("doom_net_join", {cs.session, cs.joined_at, json_int(pod.body, "delay", delay), json_int(pod.body, "digest", 35)});
        p2s.emplace_back(p2_pattern, 8, 1 << 20, cs.joined_at + 3);
        std::printf("session %d: player 2 joins at tic %d (input delay %ld tics)\n", cs.session, cs.joined_at, json_int(pod.body, "delay", delay));
        return true;
    };
    // Before anyone joins, player 1 plays alone for a while (net_tick_alone): its tics advance by the clock
    {
        uint32_t before = 0, after = 0;
        before = (uint32_t)pod.call("doom_state_digest", {4});
        for (g_now_ms = 0; g_now_ms < 2000; g_now_ms += 10) pod.call("k32_tick", {g_now_ms});
        after = (uint32_t)pod.call("doom_state_digest", {4});
        if (before == after) { std::printf("alone: the authority ran no tics in 2 s before a join\n"); return 1; }
        std::printf("alone: player 1 played 2 s before player 2 joined\n");
    }
    if (!start_client()) return 1;
    const int64_t joined_ms = g_now_ms;

    std::map<int32_t, uint32_t> pod_digest;      // the authority's state after each tic it ran
    uint64_t compared = 0, frames = 0;
    int32_t pod_tic = (int32_t)pod.call("doom_net_tic");
    int32_t total_tics = 0, phase = 0;
    int64_t silent_until = -1;
    const int32_t rejoin_at = tics * 2 / 3, silent_at = tics * 2 / 3 + 600;
    int64_t outage_set = 0;
    // after the authority ran (its tick or a POST): its digest, its tic, its channel messages
    auto pod_after = [&]() {
        const int32_t t = (int32_t)pod.call("doom_net_tic");
        if (t != pod_tic) {
            for (int32_t x = pod_tic; x < t; x++) {   // every tic it ran, from the ring (DOOM_NET_DIGEST=1)
                const uint32_t d = (uint32_t)pod.call("doom_net_digest_at", {x});
                if (d) pod_digest[x] = d;
            }
            total_tics += t - pod_tic;
            pod_tic = t;
        }
        for (auto& ev : pod.events) net.send(1, 0, gen, ev);
        pod.events.clear();
    };
    auto client_step = [&]() -> int {   // 4: the states differ
        int32_t ran = 0;
        for (; ran < 8; ran++) {
            const int32_t t = (int32_t)client.call("doom_net_tic");
            p2s.back().feed(t, [&](int d, int k) { client.call("doom_key", {d, k}); });
            if (client.call("doom_net_run", {1}) != 1) break;
            auto it = pod_digest.find(t);
            if (it != pod_digest.end()) {
                const uint32_t mine = (uint32_t)client.call("doom_state_digest", {0});
                compared++;
                if (mine != it->second) {
                    std::printf("tic %d: the game states differ (authority %08x, client %08x)\n", t, it->second, mine);
                    static const char* groups[] = {"", "players", "mobjs", "sectors", "P_Random/time", "other thinkers"};
                    // the authority has moved on; the groups name what differs now
                    for (int g = 1; g <= 5; g++) std::printf("  %s: %08x %08x\n", groups[g], (uint32_t)pod.call("doom_state_digest", {g}), (uint32_t)client.call("doom_state_digest", {g}));
                    return 4;
                }
            }
        }
        if (ran) cs.dirty = true;
        const int32_t fresh = (int32_t)client.call("doom_net_pace", {(int64_t)cs.rtt});
        if (fresh > 0 && cs.fresh_at < 0) cs.fresh_at = g_now_ms;
        for (auto it = cs.in_flight.begin(); it != cs.in_flight.end();) it = g_now_ms - it->second > 300 ? cs.in_flight.erase(it) : std::next(it);
        if ((fresh >= 2 || (fresh > 0 && g_now_ms - cs.fresh_at >= 35) || g_now_ms - cs.last_post >= 250) && cs.in_flight.size() < 4) {
            const std::string b = client.out((int32_t)client.call("doom_net_post"));
            net.send(0, 1, gen, b, ++post_id);
            cs.in_flight[post_id] = g_now_ms;
            cs.last_post = g_now_ms;
            cs.fresh_at = -1;
        }
        return 0;
    };
    bool tick_due = false;
    for (; total_tics < tics + 1200 && g_now_ms < 3600LL * 1000; g_now_ms++) {
        // the network: posts reach the authority, events and answers the client
        for (Msg& m : net.due()) {
            if (m.to == 0) {
                if (g_now_ms < g_pod_busy_until) {   // the pod is inside k32_publish: the POST waits
                    m.at = g_pod_busy_until;
                    net.q.push_back(std::move(m));
                    continue;
                }
                pod.request("POST", "/doom/tics", m.body);
                pod_after();
                net.send(1, 2, m.gen, pod.body, m.id);
            } else if (m.gen == gen && client.rt && cs.alive) {
                const int32_t r = client.recv(m.body);
                if (m.kind == 2) {
                    auto f = cs.in_flight.find(m.id);
                    if (f != cs.in_flight.end()) {
                        const double sample = (double)(g_now_ms - f->second);
                        cs.rtt = cs.rtt ? cs.rtt * 0.9 + sample * 0.1 : sample;
                        cs.in_flight.erase(f);
                    }
                }
                if (r == -2) { std::printf("t=%lld ms: the session is gone, the client joins again\n", (long long)g_now_ms); if (!start_client()) return 1; }
                else if (client_step()) return 4;
            }
        }
        // the node ticks the pod every pod_tick_ms (10 ms, as a node does)
        if (g_now_ms % pod_tick_ms == 0) tick_due = true;
        if (tick_due && g_now_ms >= g_pod_busy_until) {
            tick_due = false;
            p1.feed(pod_tic, [&](int d, int k) { pod.call("doom_key", {d, k}); });
            pod.call("k32_tick", {g_now_ms});
            pod_after();
        }
        // the page steps every 4 ms and draws every 16 ms
        if (g_now_ms % 4 == 0 && client.rt && cs.alive && client_step()) return 4;
        if (g_now_ms % 16 == 0 && client.rt && cs.alive && cs.dirty && render) {
            client.call("doom_render");
            frames++;
            cs.dirty = false;
        }
        if (trace) {   // NET_TRACE: both sides while the authority waits
            static int32_t last_t = -1;
            static int64_t since = 0;
            if (pod_tic != last_t) { last_t = pod_tic; since = g_now_ms; }
            else if (g_now_ms - since >= 120 && g_now_ms % 20 == 0 && client.rt && cs.alive) {
                size_t to_pod = 0, to_client = 0;
                for (auto& m : net.q) (m.to == 0 ? to_pod : to_client)++;
                std::printf("  t=%lld ms: tic %d waits %lld ms; %zu POSTs in flight, last %lld; %zu messages to the pod, %zu to the client; %s\n",
                            (long long)g_now_ms, pod_tic, (long long)(g_now_ms - since), cs.in_flight.size(), (long long)cs.last_post, to_pod, to_client,
                            client.out((int32_t)client.call("doom_net_stats")).c_str());
            }
        }
        // the course: an outage, a reload, a silence past the timeout
        if (phase == 0 && total_tics >= tics / 3) {
            // the steady state so far: jitter and loss alone
            pod.request("GET", "/doom/netstats", "");
            const std::string& s0 = pod.body;
            const long run0 = json_int(s0, "tics", 0), stalled0 = json_int(s0, "stalled_tics", 0);
            std::printf("steady state, %ld tics in %.1f s: %ld stalled (%.2f%%), %ld ms stalled, longest %ld ms; ticcmds arrive %.2f tics "
                        "ahead (min %ld); %ld posts, %ld events\n",
                        run0, (g_now_ms - joined_ms) / 1000.0, stalled0, run0 ? 100.0 * stalled0 / run0 : 0.0, json_int(s0, "stall_ms", 0),
                        json_int(s0, "longest_stall_ms", 0), json_int(s0, "lead_mean_x100", 0) / 100.0, json_int(s0, "lead_min", 0),
                        json_int(s0, "posts", 0), json_int(s0, "events", 0));
            net.outage_from = g_now_ms;
            net.outage_to = g_now_ms + outage_ms;
            outage_set = g_now_ms;
            std::printf("t=%lld ms, tic %d: the network goes dark for %d ms\n", (long long)g_now_ms, pod_tic, outage_ms);
            phase = 1;
        }
        if (phase == 1 && total_tics >= rejoin_at) {
            std::printf("t=%lld ms, tic %d: the browser reloads\n", (long long)g_now_ms, pod_tic);
            if (!start_client()) return 1;
            phase = 2;
        }
        if (phase == 2 && total_tics >= silent_at) {
            std::printf("t=%lld ms, tic %d: the browser goes silent for 6 s\n", (long long)g_now_ms, pod_tic);
            cs.alive = false;
            silent_until = g_now_ms + 6000;
            phase = 3;
        }
        if (phase == 3 && g_now_ms >= silent_until) {
            pod.request("GET", "/doom/netstats", "");
            std::printf("t=%lld ms, tic %d: the browser is back (player 2 in the game: %ld); it joins again\n", (long long)g_now_ms, pod_tic,
                        json_int(pod.body, "player2", -1));
            if (!start_client()) return 1;
            phase = 4;
        }
    }
    (void)outage_set;
    // The largest POST a page sends: after a stall, every ticcmd it holds ahead (64 at most), fire
    // held in each, behind a browser's headers (past the pod's 2 KiB request buffer), landing when the
    // pod's clock has a second of tics due: it runs 16 of them inside k32_handle, player 2 firing
    if (client.rt && cs.alive) {
        client.call("doom_key", {1, FIRE});
        g_now_ms = std::max(g_now_ms, g_pod_busy_until) + 1000;
        client.call("doom_net_pace", {2000});
        const std::string b = client.out((int32_t)client.call("doom_net_post"));
        const std::string headers = "User-Agent: " + std::string(400, 'u') + "\nCookie: " + std::string(1200, 'c') + "\n";
        const int32_t before = pod_tic;
        pod.request("POST", "/doom/tics", b, headers);
        pod_after();
        std::printf("the largest POST: %zu bytes, %zu ticcmds: answered %d, %zu bytes; ran tics %d to %d\n", b.size() + headers.size(),
                    (std::min(b.find('&', b.find("&c=") + 3), b.size()) - b.find("&c=") - 3) / 14, pod.status, pod.body.size(), before, pod_tic - 1);
        if (pod.status != 200 || pod_tic <= before) return 6;
    }
    pod.request("GET", "/doom/netstats", "");
    const std::string ps = pod.body;
    const std::string cst = client.out((int32_t)client.call("doom_net_stats"));
    std::printf("authority: %s\n", ps.c_str());
    std::printf("client:    %s\n", cst.c_str());
    const long tics_run = json_int(ps, "tics", 0), stalled = json_int(ps, "stalled_tics", 0);
    if (g_draw_rate)
        std::printf("draw lists: %llu published (%.1f a second), paced at %lld B/s: the pod waited %.0f ms in k32_publish\n",
                    (unsigned long long)g_published, g_published * 1000.0 / (double)g_now_ms, (long long)g_draw_rate, g_publish_wait_ms);
    std::printf("network: %llu messages, %llu lost (%d%% and a %d ms outage), %llu overtaken; latency %d to %d ms one way\n",
                (unsigned long long)net.sent, (unsigned long long)net.lost, net.loss_pct, outage_ms, (unsigned long long)net.reordered, net.base_ms,
                net.base_ms + net.jitter_ms);
    std::printf("lockstep: %ld tics run by the authority in %.1f s, %llu compared tic by tic with the client: all agree; "
                "%ld tics stalled (%.2f%%), %ld ms stalled, longest %ld ms; input delay %d tics, ticcmds arrive %.2f tics ahead (min %ld); "
                "digest checks: authority %ld (%ld bad), last client %ld (%ld bad); %ld events\n",
                tics_run, g_now_ms / 1000.0, (unsigned long long)compared, stalled, tics_run ? 100.0 * stalled / tics_run : 0.0,
                json_int(ps, "stall_ms", 0), json_int(ps, "longest_stall_ms", 0), delay, json_int(ps, "lead_mean_x100", 0) / 100.0,
                json_int(ps, "lead_min", 0), json_int(ps, "digests_checked", 0), json_int(ps, "digests_bad", 0), json_int(cst, "digests_checked", 0),
                json_int(cst, "digests_bad", 0), json_int(ps, "events", 0));
#if d_m3CountOpFrames
    uint32_t pod_frames = 0;
    for (auto& kv : g_frames_peak) {
        std::printf("operation frames, deepest call: %-24s %u\n", kv.first.c_str(), kv.second);
        if (!kv.first.compare(0, 4, "pod ")) pod_frames = std::max(pod_frames, kv.second);
    }
    if (frames_max && pod_frames > frames_max) {
        std::printf("the pod's deepest call takes %u operation frames, over %u\n", pod_frames, frames_max);
        return 7;
    }
#else
    (void)frames_max;
#endif
    if (json_int(ps, "digests_bad", 0) || json_int(cst, "digests_bad", 0)) return 4;
    return compared ? 0 : 3;
}
