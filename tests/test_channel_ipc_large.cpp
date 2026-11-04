// Large channel messages: fragmentation into frames of up to 1470 bytes,
// reassembly in any order, a lost fragment dropping the message (counted),
// subscription-aware fan-out, and publish's errors.
#include "ipc/channel.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace kube32;
using namespace kube32::ipc;

namespace {

constexpr uint16_t kBasePort = 7600;   // this node listens on kBasePort + 1 (node id 1); peers on kBasePort + 11 to + 41

struct Received {
    std::mutex m;
    std::vector<std::vector<uint8_t>> msgs;
    size_t count() { std::lock_guard<std::mutex> g(m); return msgs.size(); }
};

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 131u + seed * 7u) ^ (i >> 8));
    return v;
}

// A UDP socket standing in for a peer node: what the manager sends it
struct PeerSocket {
    osal::Socket sock;
    uint16_t port;
    explicit PeerSocket(uint16_t p) : port(p) {
        assert(sock.create(osal::SocketType::Udp));
        sock.set_reuse_addr(true);
        assert(sock.bind("127.0.0.1", port));
        sock.set_timeout(50, 50);
    }
    // Every frame that arrives within `ms`
    std::vector<std::vector<uint8_t>> drain(uint32_t ms) {
        std::vector<std::vector<uint8_t>> out;
        const uint64_t until = osal::System::get_uptime_ms() + ms;
        uint8_t buf[2048];
        while (osal::System::get_uptime_ms() < until) {
            std::string ip;
            uint16_t from = 0;
            ssize_t n = sock.recvfrom(buf, sizeof buf, &ip, &from);
            if (n > 0) out.emplace_back(buf, buf + n);
        }
        return out;
    }
};

const K32CHeader* hdr_of(const std::vector<uint8_t>& f) { return reinterpret_cast<const K32CHeader*>(f.data()); }

// The frames of `topic` among `frames` (HELLOs and announcements left out)
std::vector<std::vector<uint8_t>> frames_of(const std::vector<std::vector<uint8_t>>& frames, const std::string& topic) {
    std::vector<std::vector<uint8_t>> out;
    const uint32_t h = fnv1a_32(topic);
    for (const auto& f : frames) {
        if (f.size() >= K32C_HEADER_LEN && hdr_of(f)->channel_hash == h && !(hdr_of(f)->flags & K32C_FLAG_PING)) out.push_back(f);
    }
    return out;
}

void feed(const std::vector<uint8_t>& f) {
    NodeEndpoint src;
    src.ip_address = "127.0.0.1";
    src.udp_port = 7400;
    ChannelManager::instance().on_frame_received(f.data(), f.size(), &src);
}

// One frame in the wire format: fragment `idx` of `count`, chunk bytes a
// fragment (224 for the format before large messages)
std::vector<uint8_t> make_frame(uint32_t channel, uint32_t sender, uint16_t seq, uint8_t idx, uint8_t count, const uint8_t* data,
                                size_t len, uint8_t flags) {
    K32CHeader h = {};
    h.magic = K32C_MAGIC;
    h.version = K32C_VERSION;
    h.flags = flags | (count > 1 ? K32C_FLAG_FRAGMENT : 0);
    h.seq_num = seq;
    h.channel_hash = channel;
    h.sender_hash = sender;
    h.frag_index = idx;
    h.frag_count = count;
    h.payload_len = static_cast<uint16_t>(len);
    std::vector<uint8_t> f(K32C_HEADER_LEN + len + 2);
    std::memcpy(f.data(), &h, K32C_HEADER_LEN);
    if (len) std::memcpy(f.data() + K32C_HEADER_LEN, data, len);
    const uint16_t crc = crc16_ccitt(f.data(), K32C_HEADER_LEN + len);
    std::memcpy(f.data() + K32C_HEADER_LEN + len, &crc, 2);
    return f;
}

std::vector<std::vector<uint8_t>> split(const std::string& topic, uint32_t sender, uint16_t seq, const std::vector<uint8_t>& msg, size_t chunk,
                                        uint8_t flags) {
    std::vector<std::vector<uint8_t>> out;
    const size_t count = (msg.size() + chunk - 1) / chunk;
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * chunk, n = std::min(chunk, msg.size() - off);
        out.push_back(make_frame(fnv1a_32(topic), sender, seq, static_cast<uint8_t>(i), static_cast<uint8_t>(count), msg.data() + off, n, flags));
    }
    return out;
}

void subs_announcement(uint32_t node_id, const std::vector<std::string>& topics, uint32_t rate = 0, uint32_t max_frame = 0) {
    std::vector<uint8_t> p(12 + 4 * topics.size());
    std::memcpy(p.data(), &node_id, 4);
    std::memcpy(p.data() + 4, &rate, 4);
    std::memcpy(p.data() + 8, &max_frame, 4);
    for (size_t i = 0; i < topics.size(); ++i) {
        const uint32_t h = fnv1a_32(topics[i]);
        std::memcpy(p.data() + 12 + 4 * i, &h, 4);
    }
    feed(make_frame(fnv1a_32(K32C_SUBS_TOPIC), 0, 1, 0, 1, p.data(), p.size(), 0));
}

PeerInfo peer(uint32_t id, const char* name, uint16_t port) {
    PeerInfo p;
    p.node_id = id;
    p.node_name = name;
    p.endpoint.ip_address = "127.0.0.1";
    p.endpoint.udp_port = port;
    p.endpoint.node_id = id;
    return p;
}

void start_node() {
    auto& m = ChannelManager::instance();
    m.shutdown();
    assert(m.init(1, kBasePort));
    m.set_local_identity(1, "self");
    assert(m.start_background(100000));   // no HELLO round during a test
}

bool wait_for(const std::function<bool()>& pred, uint32_t ms) {
    const uint64_t until = osal::System::get_uptime_ms() + ms;
    while (osal::System::get_uptime_ms() < until) {
        if (pred()) return true;
        osal::Thread::sleep_ms(2);
    }
    return pred();
}

uint32_t subscribe_into(const std::string& topic, Received& r) {
    return ChannelManager::instance().subscribe_messages(topic, [&r](std::string_view, const MessagePtr& msg) {
        std::lock_guard<std::mutex> g(r.m);
        r.msgs.emplace_back(msg->data, msg->data + msg->len);
    });
}

// A 12.6 KB message leaves as 9 frames of at most 1470 bytes and comes back whole
void test_fragmentation_and_reassembly() {
    start_node();
    auto& m = ChannelManager::instance();
    PeerSocket a(kBasePort + 11);
    m.add_peer(peer(11, "a", a.port));
    const auto msg = pattern(12600, 3);
    assert(m.publish("pub", "doom.draw", msg.data(), msg.size()) == static_cast<int32_t>(msg.size()));
    const auto frames = frames_of(a.drain(150), "doom.draw");
    assert(frames.size() == 9);
    size_t total = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto* h = hdr_of(frames[i]);
        assert(frames[i].size() <= K32C_MAX_FRAME_LEN);
        assert(h->flags & K32C_FLAG_LARGE);
        assert(h->frag_index == i && h->frag_count == 9);
        assert(i + 1 == frames.size() || h->payload_len == K32C_LARGE_CHUNK);
        uint16_t crc = 0;
        std::memcpy(&crc, frames[i].data() + K32C_HEADER_LEN + h->payload_len, 2);
        assert(crc == crc16_ccitt(frames[i].data(), K32C_HEADER_LEN + h->payload_len));
        total += h->payload_len;
    }
    assert(total == msg.size());

    // Delivered whole to a subscriber
    Received r;
    subscribe_into("doom.draw", r);
    for (const auto& f : frames) feed(f);
    assert(wait_for([&] { return r.count() == 1; }, 1000));
    assert(r.msgs[0] == msg);

    // A message of up to 224 bytes is still one frame in the old format
    const auto small = pattern(200, 4);
    m.publish("pub", "small.topic", small.data(), small.size());
    const auto sf = frames_of(a.drain(100), "small.topic");
    assert(sf.size() == 1 && !(hdr_of(sf[0])->flags & K32C_FLAG_LARGE) && hdr_of(sf[0])->frag_count == 1);
    // The largest message is 16 KiB
    std::vector<uint8_t> big(K32C_MAX_MESSAGE_BYTES + 1);
    assert(m.publish("pub", "doom.draw", big.data(), big.size()) == IPC_ERR_INVAL);
    big.resize(K32C_MAX_MESSAGE_BYTES);
    assert(m.publish("pub", "doom.draw", big.data(), big.size()) == static_cast<int32_t>(big.size()));
    assert(frames_of(a.drain(150), "doom.draw").size() == K32C_MAX_FRAGMENTS);
    m.shutdown();
    std::cout << "[PASS] test_fragmentation_and_reassembly" << std::endl;
}

// Fragments in any order, repeated, and two messages interleaved
void test_out_of_order_and_duplicates() {
    start_node();
    Received r;
    subscribe_into("doom.draw", r);
    const auto m1 = pattern(4600, 10), m2 = pattern(9000, 11);
    auto f1 = split("doom.draw", 0xA1, 7, m1, K32C_LARGE_CHUNK, K32C_FLAG_LARGE);
    auto f2 = split("doom.draw", 0xB2, 7, m2, K32C_LARGE_CHUNK, K32C_FLAG_LARGE);   // same seq, another sender
    std::reverse(f1.begin(), f1.end());
    std::vector<std::vector<uint8_t>> order;
    for (size_t i = 0; i < std::max(f1.size(), f2.size()); ++i) {
        if (i < f2.size()) order.push_back(f2[f2.size() - 1 - i]);
        if (i < f1.size()) order.push_back(f1[i]);
        if (i == 1) order.push_back(f1[0]);   // a duplicate
    }
    for (const auto& f : order) feed(f);
    assert(wait_for([&] { return r.count() == 2; }, 1000));
    osal::Thread::sleep_ms(20);
    assert(r.count() == 2);
    const bool a_first = r.msgs[0].size() == m1.size();
    assert((a_first ? r.msgs[0] : r.msgs[1]) == m1);
    assert((a_first ? r.msgs[1] : r.msgs[0]) == m2);

    // Fragments of 224 bytes (the format before large messages) still reassemble
    const auto legacy = pattern(900, 12);
    for (const auto& f : split("doom.draw", 0xC3, 9, legacy, K32C_MAX_PAYLOAD_CHUNK, 0)) feed(f);
    assert(wait_for([&] { return r.count() == 3; }, 1000));
    assert(r.msgs[2] == legacy);
    ChannelManager::instance().shutdown();
    std::cout << "[PASS] test_out_of_order_and_duplicates" << std::endl;
}

// A lost fragment drops the message after the reassembly timeout; it is
// counted, and the next message arrives whole
void test_loss() {
    start_node();
    auto& m = ChannelManager::instance();
    Received r;
    subscribe_into("doom.draw", r);
    const uint64_t incomplete0 = m.stats().rx_incomplete.load();
    const auto lost = pattern(12600, 20);
    auto frames = split("doom.draw", 0xA1, 30, lost, K32C_LARGE_CHUNK, K32C_FLAG_LARGE);
    frames.erase(frames.begin() + 4);
    for (const auto& f : frames) feed(f);
    osal::Thread::sleep_ms(K32C_REASSEMBLY_TIMEOUT + 100);
    // A fragment of a later message finds the stale one and drops it
    const auto next = pattern(3000, 21);
    for (const auto& f : split("doom.draw", 0xA1, 31, next, K32C_LARGE_CHUNK, K32C_FLAG_LARGE)) feed(f);
    assert(wait_for([&] { return r.count() == 1; }, 1000));
    assert(r.msgs[0] == next);
    assert(m.stats().rx_incomplete.load() == incomplete0 + 1);

    // A corrupted frame is a lost frame
    auto bad = split("doom.draw", 0xA1, 32, next, K32C_LARGE_CHUNK, K32C_FLAG_LARGE);
    bad[1][K32C_HEADER_LEN + 5] ^= 0x40;
    const uint64_t crc0 = m.stats().crc_errors.load();
    for (const auto& f : bad) feed(f);
    osal::Thread::sleep_ms(50);
    assert(r.count() == 1 && m.stats().crc_errors.load() == crc0 + 1);

    // Without room in the receive budget a message is dropped once, counted
    m.set_rx_budget_bytes(4096);
    const uint64_t nomem0 = m.stats().rx_nomem.load();
    for (const auto& f : split("doom.draw", 0xA1, 33, lost, K32C_LARGE_CHUNK, K32C_FLAG_LARGE)) feed(f);
    osal::Thread::sleep_ms(50);
    assert(r.count() == 1 && m.stats().rx_nomem.load() == nomem0 + 1);
    m.shutdown();
    std::cout << "[PASS] test_loss" << std::endl;
}

// A topic goes only to the peers that subscribe to it, and to peers that
// announce no subscriptions
void test_subscription_fanout() {
    start_node();
    auto& m = ChannelManager::instance();
    PeerSocket a(kBasePort + 21), b(kBasePort + 22), c(kBasePort + 23);
    m.add_peer(peer(21, "a", a.port));
    m.add_peer(peer(22, "b", b.port));
    m.add_peer(peer(23, "c", c.port));
    subs_announcement(21, {"doom.draw", "k32.kubelet.sync"});
    subs_announcement(22, {"wordsketch.frame"});
    assert(m.peer_wants(21, fnv1a_32("doom.draw")) && !m.peer_wants(22, fnv1a_32("doom.draw")) && m.peer_wants(23, fnv1a_32("doom.draw")));

    const uint64_t skipped0 = m.stats().fanout_skipped.load();
    const auto msg = pattern(4600, 30);
    assert(m.publish("pub", "doom.draw", msg.data(), msg.size()) == static_cast<int32_t>(msg.size()));
    assert(frames_of(a.drain(100), "doom.draw").size() == 4);
    assert(frames_of(b.drain(50), "doom.draw").empty());
    assert(frames_of(c.drain(50), "doom.draw").size() == 4);
    assert(m.stats().fanout_skipped.load() == skipped0 + 1);

    // An announcement replaces the one before
    subs_announcement(22, {"doom.draw"});
    m.publish("pub", "doom.draw", msg.data(), 100);
    assert(frames_of(b.drain(100), "doom.draw").size() == 1);
    assert(frames_of(a.drain(50), "doom.draw").size() == 1);

    // This node announces its own subscriptions to its peers when they change
    Received r;
    const uint32_t sub = subscribe_into("my.topic", r);
    const uint32_t mine = fnv1a_32("my.topic");
    auto announced = [&](PeerSocket& s) {
        for (const auto& f : frames_of(s.drain(200), K32C_SUBS_TOPIC)) {
            const auto* h = hdr_of(f);
            uint32_t node = 0;
            std::memcpy(&node, f.data() + K32C_HEADER_LEN, 4);
            for (size_t off = 12; off + 4 <= h->payload_len; off += 4) {
                uint32_t v = 0;
                std::memcpy(&v, f.data() + K32C_HEADER_LEN + off, 4);
                if (node == 1 && v == mine) return true;
            }
        }
        return false;
    };
    assert(announced(a) && announced(b) && announced(c));
    const auto local = m.local_topic_hashes();
    assert(std::binary_search(local.begin(), local.end(), mine));
    m.unsubscribe(sub);
    const auto after = m.local_topic_hashes();
    assert(!std::binary_search(after.begin(), after.end(), mine));
    m.shutdown();
    std::cout << "[PASS] test_subscription_fanout" << std::endl;
}

// A peer that announces the bytes a second it takes gets messages no faster
void test_announced_rate() {
    start_node();
    auto& m = ChannelManager::instance();
    PeerSocket a(kBasePort + 31);
    m.add_peer(peer(31, "slow", a.port));
    subs_announcement(31, {"doom.draw"}, 40000);   // 40 KB a second
    const auto msg = pattern(4000, 50);
    const uint64_t t0 = osal::System::get_uptime_ms();
    for (int i = 0; i < 6; ++i) assert(m.publish("pub", "doom.draw", msg.data(), msg.size()) == static_cast<int32_t>(msg.size()));
    const uint64_t took = osal::System::get_uptime_ms() - t0;
    assert(took >= 450);   // 6 x 4000 bytes at 40 KB/s: the last one starts after 500 ms
    assert(frames_of(a.drain(100), "doom.draw").size() == 6 * 3);
    assert(m.stats().paced_us.load() > 0);
    // This node announces its own rate
    m.set_rx_rate_bytes(12345);
    bool seen = false;
    for (const auto& f : frames_of(a.drain(300), K32C_SUBS_TOPIC)) {
        uint32_t node = 0, rate = 0;
        std::memcpy(&node, f.data() + K32C_HEADER_LEN, 4);
        std::memcpy(&rate, f.data() + K32C_HEADER_LEN + 4, 4);
        if (node == 1 && rate == 12345) seen = true;
    }
    assert(seen);
    m.shutdown();
    std::cout << "[PASS] test_announced_rate" << std::endl;
}

// A peer that takes frames of at most 546 bytes gets 520-byte fragments,
// and they reassemble in any order
void test_announced_frame_size() {
    start_node();
    auto& m = ChannelManager::instance();
    PeerSocket a(kBasePort + 41);
    m.add_peer(peer(41, "small", a.port));
    subs_announcement(41, {"doom.draw"}, 0, K32C_MID_FRAME_LEN);
    const auto msg = pattern(12600, 60);
    assert(m.publish("pub", "doom.draw", msg.data(), msg.size()) == static_cast<int32_t>(msg.size()));
    auto frames = frames_of(a.drain(150), "doom.draw");
    assert(frames.size() == (msg.size() + K32C_MID_CHUNK - 1) / K32C_MID_CHUNK);   // 25
    for (const auto& f : frames) {
        assert(f.size() <= K32C_MID_FRAME_LEN);
        assert((hdr_of(f)->flags & K32C_FLAG_MID) && !(hdr_of(f)->flags & K32C_FLAG_LARGE));
    }
    Received r;
    subscribe_into("doom.draw", r);
    std::reverse(frames.begin(), frames.end());
    for (const auto& f : frames) feed(f);
    assert(wait_for([&] { return r.count() == 1; }, 1000));
    assert(r.msgs[0] == msg);
    // The largest message fits 32 fragments of 520 bytes
    std::vector<uint8_t> big = pattern(K32C_MAX_MESSAGE_BYTES, 61);
    for (const auto& f : split("doom.draw", 0xD4, 70, big, K32C_MID_CHUNK, K32C_FLAG_MID)) feed(f);
    assert(wait_for([&] { return r.count() == 2; }, 1000));
    assert(r.msgs[1] == big);
    // This node announces its own largest frame
    m.set_rx_max_frame(777);
    bool seen = false;
    for (const auto& f : frames_of(a.drain(300), K32C_SUBS_TOPIC)) {
        uint32_t node = 0, mf = 0;
        std::memcpy(&node, f.data() + K32C_HEADER_LEN, 4);
        std::memcpy(&mf, f.data() + K32C_HEADER_LEN + 8, 4);
        if (node == 1 && mf == 777) seen = true;
    }
    assert(seen);
    m.shutdown();
    std::cout << "[PASS] test_announced_frame_size" << std::endl;
}

// publish reports a message a local subscriber's full queue dropped, and one
// no memory could hold
void test_publish_errors() {
    start_node();
    auto& m = ChannelManager::instance();
    std::atomic<bool> release{false};
    std::atomic<int> got{0};
    m.subscribe("slow.topic", [&](std::string_view, uint32_t, const uint8_t*, size_t) {
        while (!release.load()) osal::Thread::sleep_ms(1);
        got++;
    });
    const auto msg = pattern(300, 40);
    int ok = 0, again = 0;
    for (int i = 0; i < 64; ++i) {
        const int32_t rc = m.publish("pub", "slow.topic", msg.data(), msg.size());
        if (rc == static_cast<int32_t>(msg.size())) ok++;
        else if (rc == IPC_ERR_AGAIN) again++;
        else assert(false);
    }
    assert(again > 0 && ok > 0);
    release = true;
    assert(wait_for([&] { return got.load() == ok; }, 2000));

    m.set_rx_budget_bytes(100);
    assert(m.publish("pub", "slow.topic", msg.data(), msg.size()) == IPC_ERR_NOMEM);
    m.shutdown();
    std::cout << "[PASS] test_publish_errors" << std::endl;
}

} // namespace

int main() {
    test_fragmentation_and_reassembly();
    test_out_of_order_and_duplicates();
    test_loss();
    test_subscription_fanout();
    test_announced_rate();
    test_announced_frame_size();
    test_publish_errors();
    std::cout << "All large channel message tests passed" << std::endl;
    return 0;
}
