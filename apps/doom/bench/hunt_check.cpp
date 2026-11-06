// SPDX-License-Identifier: GPL-2.0-or-later
// hunt_check: the sim-only pod in hunt mode (port/hunt.c) on the host's wasm3,
// driven as a node drives it: k32_tick every 10 ms, events in k32_event, requests
// through k32_handle. The check stands in for the cluster: pods announce
// themselves, doom.kill stops one and its Deployment replaces it, and the pod's
// own kill restarts the module. The player plays through POST /key and reads
// GET /hunt, as a person reads the screen.
//
//   hunt_check <doom-sim.wasm> <sim iwad> [--kills N] [--self N] [--pods N] [--frames-max N] [--stack BYTES]
//
// Exits 0 when every kill deleted a pod, every replacement came back as a demon,
// every self-kill restarted the map, and each draw list's name record parses.
#include "wasm3.h"
#include "m3_env.h"
#include "runtime/k32_app_abi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

static int64_t g_now_ms = 1000;
#if d_m3CountOpFrames
extern "C" u32 m3_opFrames, m3_opFramesPeak;
static std::map<std::string, uint32_t> g_frames_peak;
#endif

static std::vector<uint8_t> g_wad;
static std::map<std::string, std::string> g_env;
static std::string g_body;
static int g_status;
static std::deque<std::string> g_kills;       // doom.kill, as the reaper receives it
static uint64_t g_lists, g_named_lists, g_tags;
static uint32_t g_stack_bytes = 16 * 1024;    // --stack: the operand stack, as kube32.io/wasm-stack sets it

m3ApiRawFunction(h_log) {
    m3ApiGetArgMem(const char*, msg);
    m3ApiGetArg(uint32_t, len);
    if (std::getenv("HUNT_LOG")) std::printf("  %.*s\n", (int)len, msg);
    m3ApiSuccess();
}
m3ApiRawFunction(h_time_ms) { m3ApiReturnType(int64_t); m3ApiReturn(g_now_ms); }
m3ApiRawFunction(h_time_us) { m3ApiReturnType(int64_t); m3ApiReturn(g_now_ms * 1000); }
m3ApiRawFunction(h_env_get) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, key);
    m3ApiGetArg(uint32_t, key_len);
    m3ApiGetArgMem(char*, out);
    m3ApiGetArg(uint32_t, cap);
    auto it = g_env.find(std::string(key, key_len));
    if (it == g_env.end()) m3ApiReturn(-1);
    const uint32_t n = std::min<uint32_t>(cap, (uint32_t)it->second.size());
    std::memcpy(out, it->second.data(), n);
    m3ApiReturn((int32_t)it->second.size());
}
m3ApiRawFunction(h_blob_open) { m3ApiReturnType(int32_t); m3ApiReturn(0); }
m3ApiRawFunction(h_blob_size) { m3ApiReturnType(int32_t); m3ApiReturn((int32_t)g_wad.size()); }
m3ApiRawFunction(h_blob_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(uint32_t, off);
    m3ApiGetArgMem(uint8_t*, dst);
    m3ApiGetArg(uint32_t, len);
    (void)h;
    m3ApiCheckMem(dst, len);
    if (off >= g_wad.size()) m3ApiReturn(0);
    const uint32_t n = std::min<uint32_t>(len, (uint32_t)g_wad.size() - off);
    std::memcpy(dst, g_wad.data() + off, n);
    m3ApiReturn((int32_t)n);
}
m3ApiRawFunction(h_blob_close) { m3ApiReturnType(int32_t); m3ApiReturn(0); }
m3ApiRawFunction(h_resp_begin) {
    m3ApiGetArg(int32_t, s);
    g_status = s;
    m3ApiSuccess();
}
m3ApiRawFunction(h_resp_header) { m3ApiSuccess(); }
m3ApiRawFunction(h_resp_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiCheckMem(p, len);
    g_body.append(p, len);
    m3ApiReturn((int32_t)len);
}
m3ApiRawFunction(h_resp_end) { m3ApiSuccess(); }

// The record after the status bar's must parse the way view/view.c reads it
static void check_names(const uint8_t* msg, uint32_t len) {
    const uint32_t entries = msg[4] | (uint32_t)msg[5] << 8;
    g_lists++;
    if (entries + 12 > len || msg[entries] != 'H') { std::printf("a draw list has no status bar record\n"); std::exit(4); }
    const uint32_t at = entries + msg[entries + 1];
    if (at == len) return;
    const uint8_t* p = msg + at;
    const uint32_t bytes = p[1];
    if (p[0] != 'P' || at + bytes != len || bytes < 5) { std::printf("the name record does not parse: %u bytes at %u of %u\n", bytes, at, len); std::exit(4); }
    uint32_t n = 4 + p[3];
    const uint32_t tags = p[n++];
    for (uint32_t t = 0; t < tags; t++) {
        if (n + 4 > bytes || n + 4 + p[n + 3] > bytes) { std::printf("a tag runs past the name record\n"); std::exit(4); }
        n += 4 + p[n + 3];
        g_tags++;
    }
    if (n != bytes) { std::printf("the name record ends at %u of %u\n", n, bytes); std::exit(4); }
    g_named_lists++;
}
m3ApiRawFunction(h_publish) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, topic);
    m3ApiGetArg(uint32_t, tlen);
    m3ApiGetArgMem(const uint8_t*, data);
    m3ApiGetArg(uint32_t, len);
    (void)topic; (void)tlen;
    m3ApiCheckMem(data, len);
    check_names(data, len);
    m3ApiReturn((int32_t)len);
}
m3ApiRawFunction(h_event_emit) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char*, topic);
    m3ApiGetArg(uint32_t, tlen);
    m3ApiGetArgMem(const char*, data);
    m3ApiGetArg(uint32_t, len);
    m3ApiCheckMem(data, len);
    if (std::string(topic, tlen) == "doom.kill") g_kills.emplace_back(data, len);
    m3ApiReturn((int32_t)len);
}

struct Pod {
    IM3Environment env{};
    IM3Runtime rt{};
    IM3Module mod{};
    std::vector<uint8_t> wasm;
    int64_t call(const char* n, std::vector<int64_t> args = {}) {
        IM3Function f = nullptr;
        if (M3Result e = m3_FindFunction(&f, rt, n)) { std::printf("%s: %s\n", n, e); std::exit(1); }
        std::vector<std::string> s;
        for (auto a : args) s.push_back(std::to_string(a));
        std::vector<const char*> argv;
        for (auto& x : s) argv.push_back(x.c_str());
#if d_m3CountOpFrames
        m3_opFramesPeak = m3_opFrames;
#endif
        M3Result r = m3_CallArgv(f, (uint32_t)argv.size(), argv.data());
#if d_m3CountOpFrames
        uint32_t& peak = g_frames_peak[n];
        if (m3_opFramesPeak > peak) peak = m3_opFramesPeak;
#endif
        if (r) {
            M3ErrorInfo info;
            m3_GetErrorInfo(rt, &info);
            std::printf("%s: %s %s\n", n, r, info.message ? info.message : "");
            std::exit(1);
        }
        if (m3_GetRetCount(f) == 0) return 0;
        uint32_t v = 0;
        m3_GetResultsV(f, &v);
        return (int32_t)v;
    }
    uint8_t* mem() { return m3_GetMemory(mod, nullptr, 0); }
    void start(const std::vector<uint8_t>& w) {
        wasm = w;
        env = m3_NewEnvironment();
        rt = m3_NewRuntime(env, g_stack_bytes, nullptr);
        M3Result r = m3_ParseModule(env, &mod, wasm.data(), (uint32_t)wasm.size());
        if (!r) r = m3_LoadModule(rt, mod);
        if (r) { std::printf("load: %s\n", r); std::exit(1); }
        struct { const char* name; const char* sig; M3RawCall fn; } imports[] = {
            {"k32_log", "v(ii)", h_log}, {"k32_time_ms", "I()", h_time_ms}, {"k32_unix_time_ms", "I()", h_time_ms}, {"esp32_get_time_us", "I()", h_time_us},
            {"k32_env_get", "i(iiii)", h_env_get}, {"k32_blob_open", "i(ii)", h_blob_open}, {"k32_blob_size", "i(i)", h_blob_size},
            {"k32_blob_read", "i(iiii)", h_blob_read}, {"k32_blob_close", "i(i)", h_blob_close}, {"k32_resp_begin", "v(iii)", h_resp_begin},
            {"k32_resp_header", "v(iiii)", h_resp_header}, {"k32_resp_write", "i(ii)", h_resp_write}, {"k32_resp_end", "v()", h_resp_end},
            {"k32_event_emit", "i(iiii)", h_event_emit}, {"k32_publish", "i(iiii)", h_publish},
        };
        for (auto& im : imports) {
            r = m3_LinkRawFunction(mod, "env", im.name, im.sig, im.fn);
            if (r && r != m3Err_functionLookupFailed) { std::printf("link %s: %s\n", im.name, r); std::exit(1); }
        }
        if (call("k32_init")) { std::printf("k32_init failed\n"); std::exit(1); }
    }
    void stop() {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
    }
    std::string request(const std::string& method, const std::string& path, const std::string& body = "") {
        const std::string req = method + " " + path + "\nContent-Type: text/plain\n\n" + body;
        const uint32_t p = (uint32_t)call("k32_alloc", {(int64_t)req.size() + 1});
        std::memcpy(mem() + p, req.data(), req.size());
        g_body.clear();
        call("k32_handle", {p, (int64_t)req.size()});
        return g_body;
    }
    void event(const std::string& topic, const std::string& msg) {
        const std::string all = topic + msg;
        const uint32_t p = (uint32_t)call("k32_alloc", {(int64_t)all.size() + 1});
        std::memcpy(mem() + p, all.data(), all.size());
        call("k32_event", {p, (int64_t)topic.size(), p + (int64_t)topic.size(), (int64_t)msg.size()});
    }
};

static long json_int(const std::string& s, const char* key, long def, size_t from = 0) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t at = s.find(pat, from);
    return at == std::string::npos ? def : std::strtol(s.c_str() + at + pat.size(), nullptr, 10);
}
static std::string json_str(const std::string& s, const char* key, size_t from = 0) {
    const std::string pat = std::string("\"") + key + "\":\"";
    size_t at = s.find(pat, from);
    if (at == std::string::npos) return "";
    at += pat.size();
    return s.substr(at, s.find('"', at) - at);
}

struct Target { std::string name; int64_t stops_at = -1; };

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hunt_check <doom-sim.wasm> <sim iwad> [--kills N] [--self N] [--pods N] [--frames-max N] [--stack BYTES]\n");
        return 2;
    }
    std::ifstream wf(argv[1], std::ios::binary), iw(argv[2], std::ios::binary);
    const std::vector<uint8_t> module((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
    g_wad.assign(std::istreambuf_iterator<char>(iw), std::istreambuf_iterator<char>());
    int want_kills = 40, want_self = 3, npods = 7;
    uint32_t frames_max = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--kills") want_kills = std::atoi(argv[i + 1]);
        else if (a == "--self") want_self = std::atoi(argv[i + 1]);
        else if (a == "--pods") npods = std::atoi(argv[i + 1]);
        else if (a == "--frames-max") frames_max = (uint32_t)std::atol(argv[i + 1]);
        else if (a == "--stack") g_stack_bytes = (uint32_t)std::atol(argv[i + 1]);
    }
    g_env = {{"DOOM_HUNT", "1"}, {"DOOM_IWAD", "doom1.wad"}, {"DOOM_WAD_BLOB", "doom-wad"}, {"DOOM_ARGS", "-warp 1 1 -skill 3 -kb 224"},
             {"DOOM_SCREENBLOCKS", "11"}, {"DOOM_DETAIL", "1"}, {"DOOM_FPS", "4"}, {"DOOM_DRAW_RATE", "0"}, {"NODE_NAME", "devkit"}};
    int sim_serial = 0;
    const auto sim_name = [&] { return "doom-sim-7kp45tc6-" + std::to_string(10000 + sim_serial); };
    g_env["POD_NAME"] = sim_name();

    Pod pod;
    pod.start(module);
    std::vector<Target> targets;
    int serial = 0;
    const auto new_target = [&] {
        const int n = serial++;
        return std::string(n % 7 == 6 ? "imp" : "zombie") + "-58gs7cf6-" + std::to_string(20000 + n);
    };
    for (int i = 0; i < npods; i++) targets.push_back({new_target()});
    std::vector<std::pair<int64_t, std::string>> starts, reaped;   // when a replacement announces itself; when the reaper answers
    int64_t next_announce = 0, next_look = 0, restart_at = -1;
    int kills = 0, selfs = 0, replacements = 0, restarts = 0;
    long spawns_seen = 0;
    std::vector<std::string> keys;
    int key_wait = 0;
    std::string last_stats;
    const int64_t give_up = 60 * 60 * 1000;
    const int aim_self_every = want_self ? std::max(1, want_kills / want_self) : 1 << 30;

    while (kills < want_kills || selfs < want_self) {
        if (g_now_ms > give_up) { std::printf("%d kills and %d self-kills in an hour of game time\n", kills, selfs); return 4; }
        g_now_ms += 10;
        if (restart_at >= 0) {
            if (g_now_ms < restart_at) continue;
            restart_at = -1;
            sim_serial++;
            g_env["POD_NAME"] = sim_name();
            pod.start(module);
            restarts++;
            next_look = g_now_ms + 400;
        }
        pod.call("k32_tick", {g_now_ms});
        if (g_now_ms >= next_announce) {
            next_announce = g_now_ms + 2000;
            for (auto& t : targets) pod.event("doom.pods", t.name + (t.name[0] == 'i' ? " cardputer" : " devkit"));
        }
        for (size_t i = 0; i < starts.size();) {
            if (starts[i].first <= g_now_ms) {
                targets.push_back({starts[i].second});
                pod.event("doom.pods", starts[i].second + (starts[i].second[0] == 'i' ? " cardputer" : " devkit"));
                starts.erase(starts.begin() + (long)i);
                replacements++;
            } else i++;
        }
        for (size_t i = 0; i < reaped.size();) {
            if (reaped[i].first <= g_now_ms) {
                pod.event("doom.reaped", reaped[i].second + " 200 40");
                reaped.erase(reaped.begin() + (long)i);
            } else i++;
        }
        while (!g_kills.empty()) {
            const std::string name = g_kills.front();
            g_kills.pop_front();
            if (name == g_env["POD_NAME"]) {
                last_stats = pod.request("GET", "/stats");
                pod.stop();
                restart_at = g_now_ms + 3000;
                selfs++;
                break;
            }
            auto it = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.name == name; });
            if (it == targets.end()) continue;
            reaped.push_back({g_now_ms + 100, name});
            it->stops_at = g_now_ms + 500;
            starts.push_back({g_now_ms + 3500, new_target()});
            kills++;
        }
        if (restart_at >= 0) continue;
        targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const Target& t) { return t.stops_at >= 0 && t.stops_at <= g_now_ms; }), targets.end());
        if (g_now_ms < next_look) continue;
        next_look = g_now_ms + 250;
        // the player: what a person sees on the screen, and the keys a person presses
        const std::string st = pod.request("GET", "/hunt");
        spawns_seen = std::max(spawns_seen, json_int(st, "spawns", 0));
        const std::string aim = json_str(st, "aim");
        const bool want_sim = kills >= (selfs + 1) * aim_self_every || kills >= want_kills;
        if (!keys.empty()) { pod.request("POST", "/key", keys.front()); keys.erase(keys.begin()); continue; }
        if (key_wait > 0) { key_wait--; continue; }
        size_t at = st.find("\"pods\":");
        int bearing = 0, dist = 1 << 30;
        while ((at = st.find("{\"pod\":", at)) != std::string::npos) {
            const std::string name = json_str(st, "pod", at);
            const long state = json_int(st, "state", 0, at), kind = json_int(st, "kind", 0, at), b = json_int(st, "bearing", 0, at), d = json_int(st, "dist", 0, at);
            at += 8;
            if (state != 2 || (kind == 2) != want_sim) continue;
            if (std::abs(b) < std::abs(bearing) || dist == 1 << 30) { bearing = (int)b; dist = (int)d; }
        }
        if (dist == 1 << 30) continue;
        const bool on_target = !aim.empty() && (aim.rfind("doom-sim", 0) == 0) == want_sim;
        if (on_target) {
            keys.push_back("u 163");
            pod.request("POST", "/key", "d 163");
            key_wait = 2;
        } else {
            const int k = bearing > 0 ? 172 : 174;
            pod.request("POST", "/key", "d " + std::to_string(k));
            keys.push_back("u " + std::to_string(k));
        }
    }
    std::printf("hunt: %d kills, %d replacements, %d self-kills and restarts, %ld demons in the last game; %llu draw lists, %llu with names, %llu tags\n",
                kills, replacements, restarts, spawns_seen, (unsigned long long)g_lists, (unsigned long long)g_named_lists, (unsigned long long)g_tags);
    if (!g_named_lists) { std::printf("no draw list named a pod\n"); return 4; }
    if (json_int(last_stats, "zone_free", 0) < 4096) { std::printf("the zone ran out: %s\n", last_stats.c_str()); return 4; }
#if d_m3CountOpFrames
    uint32_t deepest = 0;
    for (const auto& kv : g_frames_peak) {
        std::printf("operation frames, deepest call: %-12s %u\n", kv.first.c_str(), kv.second);
        deepest = std::max(deepest, kv.second);
    }
    if (frames_max && deepest > frames_max) {
        std::printf("the pod's deepest call takes %u operation frames, over %u\n", deepest, frames_max);
        return 4;
    }
#else
    (void)frames_max;
#endif
    return 0;
}
