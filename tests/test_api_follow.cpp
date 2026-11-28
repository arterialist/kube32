// A worker that loses its API server: the local address toward a host, the
// decision to follow the control plane's announced API address, the HELLO
// frames that carry it, and the clients that are pointed at the new address.

#include "app/service_client.hpp"
#include "ipc/channel.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/kubelet.hpp"
#include "net/api_follow.hpp"
#include "osal/osal.hpp"
#include "runtime/wasm_runtime.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kube32;

// ---------------------------------------------------------------------------
// The local address toward a host
// ---------------------------------------------------------------------------
static void test_local_address() {
    // The route to this host is the loopback interface
    assert(osal::Socket::local_address_toward("127.0.0.1", 6443) == "127.0.0.1");
    assert(osal::Socket::local_address_toward("127.0.0.1", 0) == "127.0.0.1");
    assert(osal::Socket::local_address_toward("localhost", 6443) == "127.0.0.1");
    // Nothing found: no address, not a stale or a wildcard one
    assert(osal::Socket::local_address_toward("", 6443).empty());
    assert(osal::Socket::local_address_toward(nullptr, 6443).empty());
    assert(osal::Socket::local_address_toward("not an address", 6443).empty());
    // Connecting a UDP socket sends nothing: a destination that nothing
    // listens at costs no answer to wait for
    const uint64_t t0 = osal::System::get_uptime_ms();
    (void)osal::Socket::local_address_toward("192.0.2.1", 6443);
    assert(osal::System::get_uptime_ms() - t0 < 1500);
    std::cout << "[PASS] test_local_address" << std::endl;
}

// ---------------------------------------------------------------------------
// The follow decision
// ---------------------------------------------------------------------------
using Announcements = std::vector<net::ApiAnnouncement>;

// Probes that answer at the addresses in `up`, and the addresses asked
struct Probes {
    std::vector<std::string> up;
    std::vector<std::string> asked;
    bool operator()(const std::string& host) {
        asked.push_back(host);
        for (const auto& u : up) if (u == host) return true;
        return false;
    }
};

static const std::string kHome = "198.51.100.1";
static constexpr uint64_t kT = 1000000;   // the uptime of the last answer

// A follower that dials its home address and heard `cp` announce it while it answered
static net::ApiFollower known_follower() {
    net::ApiFollower f(kHome, kHome);
    Probes p;
    assert(f.poll(kT, kT, 0, {{"cp", kHome, kT - 1000}}, p).empty());
    assert(f.control_plane() == "cp" && p.asked.empty());
    return f;
}

static void test_follow_waits_for_silence() {
    using net::kApiSilenceBeforeFollowMs;
    assert(kApiSilenceBeforeFollowMs == 30000);
    net::ApiFollower f(kHome, "");
    assert(f.home() == kHome && f.current() == kHome && f.control_plane().empty());
    Probes p{{"203.0.113.2"}, {}};
    auto heard = [](uint64_t now) { return Announcements{{"cp", "203.0.113.2", now - 1000}}; };
    // Not before 30 s of silence, nor on a clock that reads before the last answer
    assert(f.poll(kT + 5000, kT, 0, heard(kT + 5000), p).empty());
    assert(f.poll(kT + kApiSilenceBeforeFollowMs, kT, 0, heard(kT + kApiSilenceBeforeFollowMs), p).empty());
    assert(f.poll(kT - 1, kT, 0, heard(kT), p).empty());
    // A later start of the silence counts
    assert(f.poll(kT + 40000, kT, kT + 20000, heard(kT + 40000), p).empty());
    assert(p.asked.empty());
    // After it, to the announced address that answers; its announcer is the control plane
    assert(f.poll(kT + kApiSilenceBeforeFollowMs + 1, kT, 0, heard(kT + kApiSilenceBeforeFollowMs + 1), p) == "203.0.113.2");
    assert(f.current() == "203.0.113.2" && f.home() == kHome && f.control_plane() == "cp");
    assert(p.asked == std::vector<std::string>{"203.0.113.2"});
    // Never to nothing, never to the address already dialed
    net::ApiFollower g(kHome, kHome);
    Probes q{{kHome, "203.0.113.2"}, {}};
    assert(g.poll(kT + 600000, kT, 0, {{"cp", "", kT + 599000}, {"cp", kHome, kT + 599000}}, q).empty());
    assert(q.asked.empty() && g.current() == kHome);
    std::cout << "[PASS] test_follow_waits_for_silence" << std::endl;
}

// The control plane joins a network the worker cannot reach and announces
// its address there: no API server answers at it, and the worker stays
static void test_unreachable_announcement_not_followed() {
    net::ApiFollower f = known_follower();
    Probes p;   // nothing answers
    const uint64_t now = kT + 31000;
    assert(f.poll(now, kT, 0, {{"cp", "203.0.113.9", now - 500}}, p).empty());
    assert(f.current() == kHome && p.asked == std::vector<std::string>{"203.0.113.9"});
    // Probed again 10 s later, not before
    assert(f.poll(now + 5000, kT, 0, {{"cp", "203.0.113.9", now + 4000}}, p).empty());
    assert(p.asked.size() == 1);
    assert(f.poll(now + net::kApiProbeIntervalMs, kT, 0, {{"cp", "203.0.113.9", now + 9000}}, p).empty());
    assert(p.asked.size() == 2 && f.current() == kHome);
    // An HTTP server that is not a kube32 API server is the same case
    assert(!net::is_kube32_version(200, "{\"major\":\"1\",\"minor\":\"33\",\"gitVersion\":\"v1.33.0\"}"));
    assert(!net::is_kube32_version(404, "{\"kube32Version\":\"x\"}"));
    assert(net::is_kube32_version(200, "{\"major\":\"1\",\"kube32Version\":\"0.1.0\"}"));
    std::cout << "[PASS] test_unreachable_announcement_not_followed" << std::endl;
}

static void test_stale_announcement_ignored() {
    net::ApiFollower f = known_follower();
    Probes p{{"203.0.113.2"}, {}};
    const uint64_t now = kT + 60000;
    // Its last HELLO with the address is older than kApiAnnouncementFreshMs
    assert(f.poll(now, kT, 0, {{"cp", "203.0.113.2", now - net::kApiAnnouncementFreshMs - 1}}, p).empty());
    assert(p.asked.empty() && f.current() == kHome);
    // Heard again: followed
    assert(f.poll(now + net::kApiProbeIntervalMs, kT, 0, {{"cp", "203.0.113.2", now + 9000}}, p) == "203.0.113.2");
    std::cout << "[PASS] test_stale_announcement_ignored" << std::endl;
}

static void test_other_peer_ignored() {
    net::ApiFollower f = known_follower();
    Probes p{{"203.0.113.7", "203.0.113.8"}, {}};
    const uint64_t now = kT + 31000;
    // Another node announces an address that answers: it is not this worker's control plane
    assert(f.poll(now, kT, 0, {{"cp", kHome, kT}, {"other", "203.0.113.7", now - 100}}, p).empty());
    assert(p.asked.empty() && f.current() == kHome && f.control_plane() == "cp");
    // The control plane's own announcement among them is followed
    assert(f.poll(now + net::kApiProbeIntervalMs, kT, 0, {{"other", "203.0.113.7", now + 9900}, {"cp", "203.0.113.8", now + 9000}}, p) ==
           "203.0.113.8");
    assert(p.asked == std::vector<std::string>{"203.0.113.8"});
    // While none is known, any peer's announcement counts, once it answers
    net::ApiFollower g(kHome, kHome);
    Probes q{{"203.0.113.7"}, {}};
    assert(g.poll(now, 0, kT, {{"other", "203.0.113.7", now - 100}}, q) == "203.0.113.7");
    assert(g.control_plane() == "other");
    std::cout << "[PASS] test_other_peer_ignored" << std::endl;
}

// A peer that announces an unreachable address more often than the control
// plane announces its own does not keep the worker from following it
static void test_noisy_peer_does_not_hide_control_plane() {
    // As after K32_CMD:API to an address where nothing answers: no control plane known
    net::ApiFollower f("192.0.2.1", "192.0.2.1");
    Probes p{{"198.51.100.5"}, {}};
    const uint64_t now = kT + 31000;
    const Announcements heard{{"cp", "198.51.100.5", now - 2000}, {"bogus", "192.0.2.77", now - 100}, {"bogus", "192.0.2.77", now - 1100}};
    assert(f.poll(now, kT, 0, heard, p) == "198.51.100.5");
    assert((p.asked == std::vector<std::string>{"192.0.2.77", "198.51.100.5"}));
    assert(f.control_plane() == "cp");
    // At most kApiProbesPerPoll announced addresses in one check
    net::ApiFollower g("192.0.2.1", "192.0.2.1");
    Probes q;
    Announcements many;
    for (int i = 0; i < 6; ++i) many.push_back({"n" + std::to_string(i), "192.0.2." + std::to_string(10 + i), now - 100 * i});
    assert(g.poll(now, kT, 0, many, q).empty());
    assert(q.asked.size() == net::kApiProbesPerPoll && q.asked[0] == "192.0.2.10");
    std::cout << "[PASS] test_noisy_peer_does_not_hide_control_plane" << std::endl;
}

// A worker that followed an address goes back to the one it was given
static void test_return_home() {
    // As after a restart with the address followed (a board stores both)
    net::ApiFollower f(kHome, "203.0.113.2", "cp");
    assert(f.current() == "203.0.113.2" && f.control_plane() == "cp");
    Probes p{{kHome}, {}};
    const uint64_t now = kT + 31000;
    // The followed address is silent and nobody announces it; the home address answers
    assert(f.poll(now, kT, 0, {}, p) == kHome);
    assert(f.current() == kHome && p.asked == std::vector<std::string>{kHome});
    // The home address is tried before an announced one
    net::ApiFollower g(kHome, "203.0.113.2", "cp");
    Probes q{{"203.0.113.3"}, {}};
    assert(g.poll(now, kT, 0, {{"cp", "203.0.113.3", now - 100}}, q) == "203.0.113.3");
    assert((q.asked == std::vector<std::string>{kHome, "203.0.113.3"}));
    assert(g.home() == kHome);
    std::cout << "[PASS] test_return_home" << std::endl;
}

// The control plane's DHCP lease changes: it announces its new address, which answers
static void test_control_plane_moves() {
    net::ApiFollower f = known_follower();
    Probes p{{"198.51.100.77"}, {}};
    const uint64_t now = kT + 45000;
    assert(f.poll(now, kT, 0, {{"cp", "198.51.100.77", now - 2000}}, p) == "198.51.100.77");
    assert(f.current() == "198.51.100.77" && f.home() == kHome && f.control_plane() == "cp");
    // It answers there, and stays the control plane
    assert(f.poll(now + 3000, now + 2000, now, {{"cp", "198.51.100.77", now + 2500}}, p).empty());
    assert(f.control_plane() == "cp" && f.current() == "198.51.100.77");
    std::cout << "[PASS] test_control_plane_moves" << std::endl;
}

static void test_loopback_host() {
    assert(net::is_loopback_host("127.0.0.1"));
    assert(net::is_loopback_host("127.0.0.2"));
    assert(net::is_loopback_host("localhost"));
    assert(!net::is_loopback_host("198.51.100.1"));
    assert(!net::is_loopback_host("127.example.com"));
    assert(!net::is_loopback_host("1127.0.0.1"));
    assert(!net::is_loopback_host(""));
    assert(!net::is_loopback_host("127."));
    std::cout << "[PASS] test_loopback_host" << std::endl;
}

// ---------------------------------------------------------------------------
// HELLO frames carry the API address
// ---------------------------------------------------------------------------
static std::vector<uint8_t> hello_frame(uint32_t id, const std::string& name, const std::string& api, bool with_api) {
    std::vector<uint8_t> payload(4);
    std::memcpy(payload.data(), &id, 4);
    payload.insert(payload.end(), name.begin(), name.end());
    if (with_api) {
        payload.push_back(0);
        const std::string tag = "api=" + api;
        payload.insert(payload.end(), tag.begin(), tag.end());
    }
    ipc::K32CHeader hdr{};
    hdr.magic = ipc::K32C_MAGIC;
    hdr.version = ipc::K32C_VERSION;
    hdr.flags = ipc::K32C_FLAG_PING;
    hdr.frag_count = 1;
    hdr.payload_len = static_cast<uint16_t>(payload.size());
    std::vector<uint8_t> frame(sizeof(hdr));
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    frame.insert(frame.end(), payload.begin(), payload.end());
    const uint16_t crc = ipc::crc16_ccitt(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>(crc >> 8));
    return frame;
}

static void hear(const std::vector<uint8_t>& frame, const char* from_ip) {
    ipc::NodeEndpoint src;
    src.ip_address = from_ip;
    src.udp_port = 7400;
    ipc::ChannelManager::instance().on_frame_received(frame.data(), frame.size(), &src);
}

static const ipc::PeerInfo* find_peer(const std::vector<ipc::PeerInfo>& peers, uint32_t id) {
    for (const auto& p : peers) if (p.node_id == id) return &p;
    return nullptr;
}

static const net::ApiAnnouncement* find_announcement(const Announcements& all, const std::string& name) {
    for (const auto& a : all) if (a.node_name == name) return &a;
    return nullptr;
}

static void test_hello_api_address() {
    auto& mgr = ipc::ChannelManager::instance();
    mgr.shutdown();                       // no peers left by an earlier test
    mgr.set_local_identity(7, "worker-a");
    assert(mgr.api_announcements().empty());

    // A worker's HELLO has no address; a control plane's has one
    hear(hello_frame(11, "worker-b", "", false), "198.51.100.20");
    assert(mgr.api_announcements().empty());
    const uint64_t before = osal::System::get_uptime_ms();
    hear(hello_frame(12, "cp-old", "198.51.100.1", true), "198.51.100.1");
    Announcements all = mgr.api_announcements();
    assert(all.size() == 1 && all[0].node_name == "cp-old" && all[0].address == "198.51.100.1" && all[0].heard_ms >= before);
    const auto peers = mgr.peers();
    const ipc::PeerInfo* cp = find_peer(peers, 12);
    assert(cp && cp->node_name == "cp-old" && cp->api_address == "198.51.100.1" && cp->endpoint.ip_address == "198.51.100.1");
    const ipc::PeerInfo* w = find_peer(peers, 11);
    assert(w && w->node_name == "worker-b" && w->api_address.empty());

    // Each peer's announcement, with the uptime of its last HELLO
    osal::Thread::sleep_ms(5);
    hear(hello_frame(13, "cp-new", "203.0.113.2", true), "203.0.113.2");
    all = mgr.api_announcements();
    assert(all.size() == 2);
    const net::ApiAnnouncement* old_cp = find_announcement(all, "cp-old");
    const net::ApiAnnouncement* new_cp = find_announcement(all, "cp-new");
    assert(old_cp && new_cp && new_cp->address == "203.0.113.2" && new_cp->heard_ms > old_cp->heard_ms);

    // A control plane that stops announcing takes its address back
    osal::Thread::sleep_ms(5);
    hear(hello_frame(13, "cp-new", "", false), "203.0.113.2");
    all = mgr.api_announcements();
    assert(all.size() == 1 && all[0].node_name == "cp-old");
    // The node's own HELLO, from another socket of this process, is ignored
    hear(hello_frame(7, "worker-a", "203.0.113.9", true), "203.0.113.9");
    assert(mgr.api_announcements().size() == 1);
    // A name followed by an empty address
    hear(hello_frame(14, "cp-x", "", true), "203.0.113.3");
    assert(mgr.api_announcements().size() == 1);
    std::cout << "[PASS] test_hello_api_address" << std::endl;
}

// A control plane announces the address it was given in the frames it sends
static void test_hello_roundtrip() {
    auto& mgr = ipc::ChannelManager::instance();
    mgr.shutdown();

    osal::Socket listener;
    assert(listener.create(osal::SocketType::Udp));
    assert(listener.bind("127.0.0.1", 0));
    listener.set_timeout(2000, 2000);
    const uint16_t listener_port = listener.local_port();
    assert(listener_port != 0);

    // The node's UDP port is the base port plus its id: pick one that is free
    osal::Socket probe;
    assert(probe.create(osal::SocketType::Udp) && probe.bind("127.0.0.1", 0));
    const uint16_t node_port = probe.local_port();
    probe.close();
    const uint32_t id = 40;
    assert(node_port > id);
    assert(mgr.init(id, static_cast<uint16_t>(node_port - id)));
    mgr.set_local_identity(id, "cp-test");
    mgr.add_seed_peer("127.0.0.1", listener_port);

    auto receive = [&] {
        uint8_t buf[512];
        const ssize_t n = listener.recvfrom(buf, sizeof(buf), nullptr, nullptr);
        assert(n > 0);
        return std::vector<uint8_t>(buf, buf + n);
    };

    mgr.set_announced_api("203.0.113.4");
    mgr.send_hello();
    const std::vector<uint8_t> with_api = receive();
    // This frame is the control plane's: another node hears it
    mgr.set_local_identity(41, "worker-c");
    hear(with_api, "203.0.113.4");
    const Announcements heard = mgr.api_announcements();
    assert(heard.size() == 1 && heard[0].node_name == "cp-test" && heard[0].address == "203.0.113.4");

    mgr.set_local_identity(id, "cp-test");
    mgr.set_announced_api("");
    mgr.send_hello();
    const std::vector<uint8_t> without_api = receive();
    assert(without_api.size() < with_api.size());
    mgr.set_local_identity(41, "worker-c");
    hear(without_api, "203.0.113.4");
    assert(mgr.api_announcements().empty());

    mgr.shutdown();
    std::cout << "[PASS] test_hello_roundtrip" << std::endl;
}

// ---------------------------------------------------------------------------
// Clients repointed at another address
// ---------------------------------------------------------------------------

// Answers every request with `name` as the body and keeps the requests' heads
class Listener {
public:
    explicit Listener(std::string name) : name_(std::move(name)) {
        assert(sock_.create(osal::SocketType::Tcp));
        sock_.set_reuse_addr(true);
        assert(sock_.bind("127.0.0.1", 0) && sock_.listen(8));
        port_ = sock_.local_port();
        assert(port_ != 0);
        // accept does not return on a timeout everywhere: poll, to be stopped
        sock_.set_nonblocking(true);
        thread_ = std::thread([this] {
            while (running_.load()) {
                osal::Socket c = sock_.accept();
                if (!c.is_valid()) {
                    osal::Thread::sleep_ms(2);
                    continue;
                }
                c.set_nonblocking(false);
                c.set_timeout(1000, 1000);
                std::string head;
                char buf[2048];
                while (head.find("\r\n\r\n") == std::string::npos) {
                    const ssize_t n = c.recv(buf, sizeof(buf));
                    if (n <= 0) break;
                    head.append(buf, static_cast<size_t>(n));
                }
                // A body that came with the head is not needed here
                {
                    std::lock_guard<std::mutex> g(mutex_);
                    heads_.push_back(head);
                }
                const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(name_.size()) +
                                          "\r\nConnection: close\r\n\r\n" + name_;
                c.send_all(reply.data(), reply.size());
            }
        });
    }
    ~Listener() {
        running_.store(false);
        thread_.join();
    }
    uint16_t port() const { return port_; }
    std::vector<std::string> heads() const {
        std::lock_guard<std::mutex> g(mutex_);
        return heads_;
    }

private:
    std::string name_;
    osal::Socket sock_;
    uint16_t port_{0};
    std::atomic<bool> running_{true};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> heads_;
};

static void test_http_client_repoint() {
    Listener a("A"), b("B");
    kubelet::HttpKubeClient client("127.0.0.1", a.port());
    std::string body;
    assert(client.get("/x", body) == 200 && body == "A");
    assert(a.heads().size() == 1 && b.heads().empty());
    assert(a.heads()[0].find("Host: 127.0.0.1:" + std::to_string(a.port()) + "\r\n") != std::string::npos);

    // The next request goes to the new port, with its Host header
    assert(client.set_server("127.0.0.1", b.port()));
    assert(client.get("/x", body) == 200 && body == "B");
    assert(a.heads().size() == 1 && b.heads().size() == 1);
    assert(b.heads()[0].find("GET /x HTTP/1.1\r\n") == 0);
    assert(b.heads()[0].find("Host: 127.0.0.1:" + std::to_string(b.port()) + "\r\n") != std::string::npos);
    // Not to a server that is not named
    assert(!client.set_server("", b.port()));
    assert(client.get("/x", body) == 200 && body == "B");

    // Through the interface the kubelet holds; a client that has one server says so
    kubelet::KubeApiClient& base = client;
    assert(base.set_server("127.0.0.1", a.port()));
    assert(client.get("/x", body) == 200 && body == "A");

    // Requests from other threads while the address changes: each one is
    // answered by one of the two servers, whole
    std::atomic<int> answered{0}, bad{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            for (int n = 0; n < 15; ++n) {
                std::string out;
                if (client.get("/x", out) == 200 && (out == "A" || out == "B")) answered++; else bad++;
            }
        });
    }
    for (int n = 0; n < 20; ++n) {
        client.set_server("127.0.0.1", (n % 2) ? a.port() : b.port());
        osal::Thread::sleep_ms(2);
    }
    for (auto& t : callers) t.join();
    assert(bad.load() == 0 && answered.load() == 60);
    std::cout << "[PASS] test_http_client_repoint" << std::endl;
}

static void test_kubelet_repoint() {
    Listener a("{}"), b("{}");
    auto runtime = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "follower";
    cfg.api_server_host = "127.0.0.1";
    cfg.api_server_port = a.port();
    cfg.node_ip = "203.0.113.5";
    std::string provided = "203.0.113.5";
    cfg.node_ip_provider = [&] { return provided; };
    kubelet::Kubelet kubelet(cfg, *runtime);

    // The node's address goes to the server the kubelet dials
    kubelet.refresh_node_ip();
    auto patches = [](const Listener& l) {
        size_t n = 0;
        for (const auto& h : l.heads()) if (h.find("PATCH /api/v1/nodes/follower HTTP") == 0) ++n;
        return n;
    };
    assert(patches(a) == 1 && patches(b) == 0);

    // The network changed and the API server moved: the next address change
    // is written to the new server
    assert(kubelet.set_api_server("127.0.0.1", b.port()));
    assert(kubelet.config().api_server_port == b.port());
    provided = "203.0.113.6";
    kubelet.refresh_node_ip();
    assert(patches(a) == 1 && patches(b) == 1);
    std::cout << "[PASS] test_kubelet_repoint" << std::endl;
}

static void test_service_gateway_repoint() {
    Listener a("{}"), b("{}");
    app::ServiceClientOptions opts;
    opts.gateway_host = "127.0.0.1";
    opts.gateway_port = a.port();
    opts.connect_timeout_ms = 1000;
    app::ServiceClient client(app::make_api_service_resolver([](const std::string&, std::string&) { return 404; }), opts);

    auto forwarded = [](const Listener& l) {
        size_t n = 0;
        for (const auto& h : l.heads()) if (h.find("POST /apis/kube32.io/v1/egress?pod=") == 0) ++n;
        return n;
    };
    auto call = [&] {
        uint8_t buf[256];
        runtime::BufferResponseSink sink(buf, sizeof buf);
        runtime::ServiceCallRequest req;
        req.caller_pod = "default/p-0";
        req.method = "GET";
        req.url = "https://upstream.example/";
        req.timeout_ms = 1500;
        (void)client.call(req, sink);
    };
    call();
    assert(forwarded(a) == 1 && forwarded(b) == 0);
    client.set_gateway("127.0.0.1", b.port());
    call();
    assert(forwarded(a) == 1 && forwarded(b) == 1);
    const auto heads = b.heads();
    assert(heads.back().find("Host: 127.0.0.1:" + std::to_string(b.port()) + "\r\n") != std::string::npos);
    std::cout << "[PASS] test_service_gateway_repoint" << std::endl;
}

// ---------------------------------------------------------------------------
// The probe: GET /version, answered as a kube32 API server answers it
// ---------------------------------------------------------------------------
static void test_api_server_probe() {
    Listener kube32_api("{\"major\":\"1\",\"minor\":\"33\",\"gitVersion\":\"v1.33.0-kube32\",\"kube32Version\":\"0.1.0\"}");
    Listener other_http("{\"major\":\"1\",\"minor\":\"33\",\"gitVersion\":\"v1.33.0\"}");
    assert(net::api_server_answers("127.0.0.1", kube32_api.port()));
    assert(kube32_api.heads().size() == 1 && kube32_api.heads()[0].find("GET /version HTTP/1.") == 0);
    assert(!net::api_server_answers("127.0.0.1", other_http.port()));
    assert(other_http.heads().size() == 1);

    // A port nothing listens on is refused at once
    osal::Socket probe;
    assert(probe.create(osal::SocketType::Tcp) && probe.bind("127.0.0.1", 0));
    const uint16_t closed = probe.local_port();
    probe.close();
    assert(!net::api_server_answers("127.0.0.1", closed));
    // An address nothing answers at takes no longer than the probe's timeout
    const uint64_t t0 = osal::System::get_uptime_ms();
    assert(!net::api_server_answers("192.0.2.1", 6443, 500));
    assert(osal::System::get_uptime_ms() - t0 < 1500);
    assert(!net::api_server_answers("", 6443, 500));
    std::cout << "[PASS] test_api_server_probe" << std::endl;
}

int main() {
    osal::Socket::init_networking();
    test_local_address();
    test_follow_waits_for_silence();
    test_unreachable_announcement_not_followed();
    test_stale_announcement_ignored();
    test_other_peer_ignored();
    test_noisy_peer_does_not_hide_control_plane();
    test_return_home();
    test_control_plane_moves();
    test_loopback_host();
    test_hello_api_address();
    test_hello_roundtrip();
    test_http_client_repoint();
    test_kubelet_repoint();
    test_service_gateway_repoint();
    test_api_server_probe();
    osal::Socket::cleanup_networking();
    std::cout << "[ALL PASS] test_api_follow" << std::endl;
    return 0;
}
