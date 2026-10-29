#pragma once

// Helpers shared by the pod-level tests: the svcprobe test pod
// (apps/svcprobe, built to images/sd/svcprobe.wasm), pods deployed into a
// WasmRuntime with their capabilities registered, and a one-shot HTTP client.

#include "runtime/wasm_runtime.hpp"
#include "runtime/app_pod.hpp"
#include "hal/capability_firewall.hpp"
#include "osal/socket.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace k32test {

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::vector<uint8_t> svcprobe() {
    std::vector<uint8_t> b = read_file(std::string(KUBE32_SOURCE_DIR) + "/images/sd/svcprobe.wasm");
    assert(!b.empty() && "images/sd/svcprobe.wasm (make -C apps/svcprobe)");
    return b;
}

// Pod "<ns>/<name>" running `wasm` with the capabilities in `caps`
// (securityContext names, e.g. "\"SERVICE_CALL\"")
inline void deploy(kube32::runtime::WasmRuntime& rt, const std::string& pod_id, const std::vector<uint8_t>& wasm, uint32_t memory,
                   const std::string& caps = "", kube32::runtime::DeploySpec spec = {}) {
    std::string manifest = "{\"metadata\":{\"name\":\"x\"},\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\"" +
                           (caps.empty() ? std::string() : ",\"securityContext\":{\"capabilities\":{\"add\":[" + caps + "]}}") + "}]}}";
    std::string err;
    kube32::hal::CapabilityFirewall::instance().unregister_pod(pod_id);
    bool ok = kube32::hal::CapabilityFirewall::instance().register_pod(pod_id, manifest, err);
    assert(ok);
    spec.memory_limit_bytes = memory;
    spec.stack_size_bytes = 4096;
    if (spec.env.empty()) spec.env.push_back({"HOSTNAME", pod_id.substr(pod_id.find('/') + 1)});
    ok = rt.deploy_workload_ex(pod_id, wasm.data(), wasm.size(), spec, err);
    if (!ok) std::printf("deploy %s: %s\n", pod_id.c_str(), err.c_str());
    assert(ok);
    assert(rt.start(pod_id));
}

// Collects a response in memory
class Collect : public kube32::runtime::AppResponseSink {
public:
    int status{0};
    std::string ctype, headers, body;
    size_t announced{0};
    bool ended{false};
    void begin(int s, std::string_view ct) override { status = s; ctype = std::string(ct); }
    void header(std::string_view n, std::string_view v) override { headers += std::string(n) + ": " + std::string(v) + "\n"; }
    bool write(const uint8_t* d, size_t l) override { body.append(reinterpret_cast<const char*>(d), l); return true; }
    void end() override { ended = true; }
    void body_length(size_t l) override { announced = l; }
};

// GET (or another method) on a local pod through AppPodHost
inline Collect call_pod(const std::string& pod_id, const std::string& target, const std::string& method = "GET",
                        const std::string& body = "", bool gzip = false) {
    kube32::runtime::AppRequest req;
    req.method = method;
    req.path_and_query = target;
    req.body = body;
    req.accepts_gzip = gzip;
    Collect c;
    kube32::runtime::AppPodHost::instance().handle(pod_id, req, c);
    return c;
}

struct HttpReply {
    int status{0};
    std::string head;
    std::string body;
    bool has(const std::string& h) const { return head.find(h) != std::string::npos; }
};

// One request on a fresh connection (Connection: close), the whole answer
inline HttpReply http(uint16_t port, const std::string& method, const std::string& target, const std::string& headers = "",
                      const std::string& body = "") {
    HttpReply r;
    kube32::osal::Socket s;
    if (!s.create(kube32::osal::SocketType::Tcp) || !s.connect("127.0.0.1", port, 2000)) return r;
    s.set_timeout(10000, 10000);
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
    if (!s.send_all(req.data(), req.size())) return r;
    std::string all;
    char buf[4096];
    for (;;) {
        ssize_t n = s.recv(buf, sizeof buf);
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
    size_t he = all.find("\r\n\r\n");
    if (he == std::string::npos) return r;
    r.head = all.substr(0, he);
    std::string raw = all.substr(he + 4);
    r.status = std::atoi(all.c_str() + 9);
    if (r.has("Transfer-Encoding: chunked")) {
        size_t p = 0;
        while (p < raw.size()) {
            size_t eol = raw.find("\r\n", p);
            if (eol == std::string::npos) break;
            size_t n = std::strtoul(raw.c_str() + p, nullptr, 16);
            if (n == 0) break;
            r.body.append(raw, eol + 2, n);
            p = eol + 2 + n + 2;
        }
    } else {
        r.body = raw;
    }
    return r;
}

} // namespace k32test
