// The kubelet's view of the API server on a node with little RAM: pod lists
// leave inline images out (?inlineImages=omit) and are split into pods as
// they stream in; an image comes with its pod, decoded while it streams. Also
// typed-client protobuf bodies (kubectl create configmap|secret|namespace).

#include "api/http_server.hpp"
#include "kubelet/kube_api_client.hpp"
#include "storage/store.hpp"
#include "models/kube_models.hpp"
#include "osal/socket.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

// Largest single allocation and number of allocations of the tracking
// thread (the client's, not the API server's threads in the same process)
static thread_local bool g_track = false;
static std::atomic<size_t> g_max_alloc{0};
static std::atomic<size_t> g_allocs{0};
void* operator new(size_t n) {
    if (g_track) {
        size_t m = g_max_alloc.load();
        while (n > m && !g_max_alloc.compare_exchange_weak(m, n)) {}
        g_allocs.fetch_add(1);
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace kube32;

static std::string b64(const std::vector<uint8_t>& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += a[n >> 18]; out += a[(n >> 12) & 63]; out += a[(n >> 6) & 63]; out += a[n & 63];
    }
    if (i < in.size()) {
        uint32_t n = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0);
        out += a[n >> 18]; out += a[(n >> 12) & 63];
        out += (i + 1 < in.size()) ? a[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static std::string pod_json(const std::string& name, const std::string& node, const std::string& image_b64) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"uid\":\"u-" + name + "\","
           "\"annotations\":{\"kube32.io/wasm-base64\":\"" + image_b64 + "\",\"kube32.io/wasm-size\":\"1\"}},"
           "\"spec\":{\"nodeName\":\"" + node + "\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://inline/" + name + "\","
           "\"resources\":{\"requests\":{\"memory\":\"16Ki\"}}}]},\"status\":{\"phase\":\"Pending\"}}";
}

// Answers one request on `port` with `response`, then closes
static std::thread serve_once(osal::Socket& listener, uint16_t port, std::string response) {
    assert(listener.create(osal::SocketType::Tcp));
    listener.set_reuse_addr(true);
    assert(listener.bind("127.0.0.1", port) && listener.listen(1));
    return std::thread([&listener, response]() {
        osal::Socket c = listener.accept();
        char b[1024];
        c.recv(b, sizeof b);
        c.send_all(response.data(), response.size());
        c.close();
    });
}

// One raw request to the API server
static std::string raw_http(uint16_t port, const std::string& method, const std::string& path, const std::string& ctype, const std::string& body) {
    osal::Socket s;
    assert(s.create(osal::SocketType::Tcp) && s.connect("127.0.0.1", port, 2000));
    s.set_timeout(5000, 5000);
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: x\r\nContent-Type: " + ctype + "\r\nAccept: application/vnd.kubernetes.protobuf,application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    assert(s.send_all(req.data(), req.size()));
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = s.recv(buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// Protobuf helpers: a length-delimited field
static std::string pb(int field, const std::string& payload) {
    std::string out(1, static_cast<char>((field << 3) | 2));
    size_t n = payload.size();
    while (n >= 0x80) { out += static_cast<char>((n & 0x7f) | 0x80); n >>= 7; }
    out += static_cast<char>(n);
    return out + payload;
}
static std::string k8s_pb(const std::string& kind, const std::string& raw) {
    return std::string("k8s\0", 4) + pb(1, pb(1, "v1") + pb(2, kind)) + pb(2, raw) + pb(3, "") + pb(4, "");
}

int main() {
    std::cout << "=== kubelet list streaming, protobuf bodies ===" << std::endl;
    osal::Socket::init_networking();
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_kubelet_api_stream", false});

    // Three pods on n1 with 96 KiB inline images, one on another node
    std::vector<std::vector<uint8_t>> images;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> img(96 * 1024);
        for (size_t k = 0; k < img.size(); ++k) img[k] = static_cast<uint8_t>(k * (i + 3) + i);
        images.push_back(img);
        assert(store->put("/registry/pods/default/p" + std::to_string(i), pod_json("p" + std::to_string(i), "n1", b64(img)), true).ok());
    }
    assert(store->put("/registry/pods/default/other", pod_json("other", "n2", b64(images[0])), true).ok());

    api::HttpServerOptions o;
    o.port = 18692;
    o.workers = 2;
    o.worker_stack_bytes = 262144;
    o.watch_streams = 0;
    api::HttpServer server(o, *store);
    assert(server.start());

    kubelet::HttpKubeClient client("127.0.0.1", o.port);
    client.set_node_filter("n1");
    std::vector<std::pair<models::KubePod, std::string>> pods;
    g_max_alloc = 0;
    g_track = true;
    assert(client.list_pods_raw(pods));
    g_track = false;
    std::cout << "[list] " << pods.size() << " pods, largest allocation " << g_max_alloc.load() << " bytes" << std::endl;
    assert(pods.size() == 3);
    for (const auto& p : pods) {
        assert(p.first.node_name == "n1");
        assert(p.first.memory_request_set && p.first.memory_request_bytes == 16 * 1024);
        auto ann = p.first.annotations.find("kube32.io/wasm-base64");
        assert(ann != p.first.annotations.end() && ann->second.empty());
        assert(p.second.size() < 1024);
    }
    // No buffer anywhere near an image (96 KiB, 128 KiB as base64), nor the list
    assert(g_max_alloc.load() < 4096);

    // The pod sync's pass: each pod is collected on the stack and read in
    // place, so a pass over the node's pods takes nothing from the heap
    {
        int visits = 0;
        bool fields_ok = true;
        auto check = [&](const kubelet::PodListEntry& e) {
            ++visits;
            fields_ok = fields_ok && e.node_name == "n1" && e.namespace_name == "default" && e.phase == "Pending" && !e.deleting &&
                        e.uid.size() == 4 && e.uid.substr(0, 2) == "u-" && e.uid.substr(2) == e.name && e.pod == nullptr &&
                        e.manifest.find("\"kube32.io/wasm-base64\":\"\"") != std::string_view::npos;
        };
        g_allocs = 0;
        g_track = true;
        const bool ok = client.for_each_node_pod(check);
        g_track = false;
        std::cout << "[sync pass] " << visits << " pods, " << g_allocs.load() << " allocations" << std::endl;
        assert(ok && visits == 3 && fields_ok);
        assert(g_allocs.load() == 0);
    }

    // The image comes with its pod, decoded as it streams
    std::vector<uint8_t> got;
    std::string why;
    g_max_alloc = 0;
    g_track = true;
    assert(client.get_pod_image("default", "p1", got, why));
    g_track = false;
    std::cout << "[image] " << got.size() << " bytes, largest allocation " << g_max_alloc.load() << " bytes" << std::endl;
    assert(got == images[1]);
    assert(g_max_alloc.load() <= images[1].size() * 105 / 100 + 1024);   // the image itself, reserved once
    assert(!client.get_pod_image("default", "missing", got, why) && !why.empty());

    // A list cut short is refused, not taken as fewer pods: a server that
    // closes after part of the body
    {
        osal::Socket l;
        assert(l.create(osal::SocketType::Tcp));
        l.set_reuse_addr(true);
        assert(l.bind("127.0.0.1", 18695) && l.listen(1));
        std::thread srv([&l]() {
            osal::Socket c = l.accept();
            char b[1024];
            c.recv(b, sizeof b);
            std::string r = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                            "{\"kind\":\"PodList\",\"items\":[{\"metadata\":{\"name\":\"a\"},\"spec\":{\"nodeName\":\"n1\"}},{\"metadata\":{\"na";
            c.send_all(r.data(), r.size());
            c.close();
        });
        kubelet::HttpKubeClient cut("127.0.0.1", 18695);
        std::vector<std::pair<models::KubePod, std::string>> partial;
        assert(!cut.list_pods_raw(partial) && partial.empty());
        srv.join();
        l.close();
    }
    {
        // The same for the pod sync's pass: no visit counts as the whole list
        const std::string r = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                              "{\"kind\":\"PodList\",\"items\":[{\"metadata\":{\"name\":\"a\"},\"spec\":{\"nodeName\":\"n1\"}},{\"metadata\":{\"na";
        osal::Socket l;
        std::thread srv = serve_once(l, 18696, r);
        kubelet::HttpKubeClient cut("127.0.0.1", 18696);
        int visits = 0;
        assert(!cut.for_each_node_pod([&](const kubelet::PodListEntry&) { ++visits; }));
        assert(visits == 1);
        srv.join();
        l.close();
    }
    {
        // Fields are found whatever surrounds them: brackets and quotes in
        // strings, nested arrays, numbers, null, a deletionTimestamp
        const std::string r = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        const std::string body =
            "{\"kind\":\"PodList\",\"metadata\":{\"resourceVersion\":\"9\"},\"items\":["
            "{\"metadata\":{\"labels\":{\"x\":\"[}{\"},\"name\":\"a\",\"namespace\":\"ns1\",\"uid\":\"u1\",\"deletionTimestamp\":\"2026-10-29T00:00:00Z\"},"
            "\"spec\":{\"containers\":[{\"name\":\"c\",\"args\":[\"}\",\"\\\"]\"]}],\"nodeName\":\"n1\"},\"status\":{\"phase\":\"Running\"}},"
            " { \"metadata\" : { \"name\" : \"b\", \"namespace\":\"ns1\", \"uid\":\"u2\", \"deletionTimestamp\":null,"
            "\"annotations\":{\"k\":\"v\\\"q\"}}, \"spec\":{\"priority\":-1.5e3,\"hostNetwork\":true,\"nodeName\":\"n2\"},"
            "\"status\":{\"conditions\":[{\"a\":[[1],[2,{\"b\":\"}\"}]]}],\"phase\":\"Succeeded\"}},"
            // larger than the stack buffer: carried on in the heap
            "{\"metadata\":{\"annotations\":{\"big\":\"" + std::string(5000, 'x') + "\"},\"name\":\"c\",\"namespace\":\"ns1\",\"uid\":\"u3\"},"
            "\"spec\":{\"nodeName\":\"n1\"},\"status\":{\"phase\":\"Pending\"}}]}";
        char size_line[16];
        snprintf(size_line, sizeof size_line, "%zx\r\n", body.size());
        osal::Socket l;
        std::thread srv = serve_once(l, 18697, r + size_line + body + "\r\n0\r\n\r\n");
        kubelet::HttpKubeClient lister("127.0.0.1", 18697);
        std::vector<std::string> seen, labels;
        assert(lister.for_each_node_pod([&](const kubelet::PodListEntry& e) {
            seen.push_back(std::string(e.namespace_name) + "/" + std::string(e.name) + " " + std::string(e.uid) + " " +
                           std::string(e.node_name) + " " + std::string(e.phase) + (e.deleting ? " deleting" : ""));
            labels.push_back(std::string(e.labels));
        }));
        srv.join();
        l.close();
        assert(seen.size() == 3);
        assert(labels[0] == "{\"x\":\"[}{\"}" && labels[1].empty() && labels[2].empty());   // as JSON text, for NetworkPolicy
        assert(seen[0] == "ns1/a u1 n1 Running deleting");
        assert(seen[1] == "ns1/b u2 n2 Succeeded");
        assert(seen[2] == "ns1/c u3 n1 Pending");
    }

    // A full list (kubectl) keeps the annotation
    std::string full;
    assert(client.get("/api/v1/pods?fieldSelector=spec.nodeName%3Dn1", full) == 200);
    assert(full.find(b64(images[2])) != std::string::npos);

    // kubectl create configmap: the body kubectl 1.33 sends, as captured
    const unsigned char cm[] = {0x6b,0x38,0x73,0x00,0x0a,0x0f,0x0a,0x02,0x76,0x31,0x12,0x09,0x43,0x6f,0x6e,0x66,0x69,0x67,0x4d,0x61,0x70,
                                0x12,0x20,0x0a,0x16,0x0a,0x06,0x63,0x6d,0x74,0x65,0x73,0x74,0x12,0x00,0x1a,0x00,0x22,0x00,0x2a,0x00,0x32,
                                0x00,0x38,0x00,0x42,0x00,0x12,0x06,0x0a,0x01,0x61,0x12,0x01,0x62,0x1a,0x00,0x22,0x00};
    std::string r = raw_http(o.port, "POST", "/api/v1/namespaces/default/configmaps?fieldManager=kubectl-create&fieldValidation=Strict",
                             "application/vnd.kubernetes.protobuf", std::string(reinterpret_cast<const char*>(cm), sizeof cm));
    std::cout << "[configmap] " << r.substr(0, r.find("\r\n")) << std::endl;
    assert(r.find("HTTP/1.1 201") == 0);
    std::string stored;
    assert(store->get("/registry/configmaps/default/cmtest", stored).ok());
    assert(stored.find("\"data\":{\"a\":\"b\"}") != std::string::npos);

    // kubectl create secret generic: bytes values travel as base64 in JSON
    std::string meta = pb(1, "s1") + pb(11, pb(1, "app") + pb(2, "x"));
    std::string secret = pb(1, meta) + pb(2, pb(1, "k") + pb(2, std::string("\x00\x01\xff", 3))) + pb(3, "Opaque");
    r = raw_http(o.port, "POST", "/api/v1/namespaces/default/secrets", "application/vnd.kubernetes.protobuf", k8s_pb("Secret", secret));
    std::cout << "[secret] " << r.substr(0, r.find("\r\n")) << std::endl;
    assert(r.find("HTTP/1.1 201") == 0);
    assert(store->get("/registry/secrets/default/s1", stored).ok());
    assert(stored.find("\"k\":\"AAH/\"") != std::string::npos && stored.find("\"type\":\"Opaque\"") != std::string::npos);
    assert(stored.find("\"labels\":{\"app\":\"x\"}") != std::string::npos);

    // kubectl create namespace
    r = raw_http(o.port, "POST", "/api/v1/namespaces", "application/vnd.kubernetes.protobuf", k8s_pb("Namespace", pb(1, pb(1, "team-a"))));
    assert(r.find("HTTP/1.1 201") == 0);

    // Other kinds are refused with a reason; malformed bodies too
    r = raw_http(o.port, "POST", "/api/v1/namespaces/default/pods", "application/vnd.kubernetes.protobuf", k8s_pb("Pod", pb(1, pb(1, "x"))));
    assert(r.find("HTTP/1.1 415") == 0 && r.find("v1 Pod in protobuf is not supported") != std::string::npos);
    r = raw_http(o.port, "POST", "/api/v1/namespaces/default/configmaps", "application/vnd.kubernetes.protobuf", std::string("k8s\0\x0a\x7f", 6));
    assert(r.find("HTTP/1.1 415") == 0);

    server.stop();
    std::cout << "[PASS] kubelet list streaming, inline image fetch, protobuf bodies" << std::endl;
    return 0;
}
