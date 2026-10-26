#include "ipc/channel.hpp"
#include "hal/capability_firewall.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>

using namespace kube32::ipc;

void test_ring_buffer_lock_free() {
    RingBuffer ring(16);
    assert(ring.is_empty());
    assert(!ring.is_full());

    const uint8_t msg[] = "Kube32-Test-Payload";
    assert(ring.push(0x1234, msg, sizeof(msg)));
    assert(!ring.is_empty());
    assert(ring.size() == 1);

    uint8_t out_buf[64];
    uint32_t sender = 0;
    int32_t n = ring.pop(out_buf, sizeof(out_buf), 0, &sender);
    assert(n == sizeof(msg));
    assert(sender == 0x1234);
    assert(std::memcmp(out_buf, msg, sizeof(msg)) == 0);
    assert(ring.is_empty());

    // Test buffer full boundary
    for (int i = 0; i < 16; ++i) {
        assert(ring.push(i, msg, sizeof(msg)));
    }
    assert(ring.is_full());
    assert(!ring.push(99, msg, sizeof(msg))); // Rejected when full

    // Drain
    for (int i = 0; i < 16; ++i) {
        int32_t bytes = ring.pop(out_buf, sizeof(out_buf), 0, &sender);
        assert(bytes == sizeof(msg));
        assert(sender == static_cast<uint32_t>(i));
    }
    assert(ring.is_empty());

    std::cout << "[PASS] test_ring_buffer_lock_free" << std::endl;
}

void test_packet_wire_crc16() {
    K32CPacket pkt = {};
    pkt.header.magic = K32C_MAGIC;
    pkt.header.version = K32C_VERSION;
    pkt.header.seq_num = 42;
    pkt.header.channel_hash = 0xAABBCCDD;
    pkt.header.payload_len = 5;
    std::memcpy(pkt.payload, "hello", 5);

    uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 5);
    assert(crc != 0);

    // Corrupt one bit
    pkt.payload[0] ^= 0x01;
    uint16_t bad_crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 5);
    assert(bad_crc != crc);

    std::cout << "[PASS] test_packet_wire_crc16" << std::endl;
}

void test_capability_firewall_ipc_gate() {
    auto& fw = kube32::hal::CapabilityFirewall::instance();
    fw.reset();

    // Pod 1 has IPC permission via annotation
    std::string manifest_ipc = R"({
        "metadata": { "annotations": { "kube32.io/ipc": "true" } },
        "spec": { "containers": [{ "name": "c1" }] }
    })";
    std::string err;
    assert(fw.register_pod("ipc-pod", manifest_ipc, err));
    assert(fw.check_permission("ipc-pod", kube32::hal::PeripheralType::IPC, 0, kube32::hal::AccessMode::ReadWrite));

    // Pod 2 has NO IPC permission
    std::string manifest_plain = R"({
        "spec": { "containers": [{ "name": "c2" }] }
    })";
    assert(fw.register_pod("unauth-pod", manifest_plain, err));
    assert(!fw.check_permission("unauth-pod", kube32::hal::PeripheralType::IPC, 0, kube32::hal::AccessMode::ReadWrite));

    // Pod 3 has IPC permission via resources.requests
    std::string manifest_res = R"({
        "spec": {
            "containers": [{
                "name": "c3",
                "resources": { "requests": { "kube32.io/ipc": "1" } }
            }]
        }
    })";
    assert(fw.register_pod("ipc-res-pod", manifest_res, err));
    assert(fw.check_permission("ipc-res-pod", kube32::hal::PeripheralType::IPC, 0, kube32::hal::AccessMode::ReadWrite));

    fw.reset();
    std::cout << "[PASS] test_capability_firewall_ipc_gate" << std::endl;
}

void test_intra_node_channel_loopback() {
    ChannelManager::instance().init(0, 7400);

    int32_t rx_handle = ChannelManager::instance().open("consumer-pod", "pod://consumer-pod/telemetry", 0);
    assert(rx_handle > 0);

    int32_t tx_handle = ChannelManager::instance().open("producer-pod", "pod://consumer-pod/telemetry", 0);
    assert(tx_handle > 0);

    const char msg[] = "Stream-Frame-100";
    int32_t sent = ChannelManager::instance().send("producer-pod", tx_handle, reinterpret_cast<const uint8_t*>(msg), sizeof(msg));
    assert(sent == sizeof(msg));

    char rx_buf[64];
    int32_t recvd = ChannelManager::instance().recv("consumer-pod", rx_handle, reinterpret_cast<uint8_t*>(rx_buf), sizeof(rx_buf), 100);
    assert(recvd == sizeof(msg));
    assert(std::strcmp(rx_buf, msg) == 0);

    ChannelManager::instance().close("consumer-pod", rx_handle);
    ChannelManager::instance().close("producer-pod", tx_handle);
    ChannelManager::instance().shutdown();

    std::cout << "[PASS] test_intra_node_channel_loopback" << std::endl;
}

void test_channel_high_level_object() {
    ChannelManager::instance().init(0, 7400);

    Channel rx_chan("pod-recv");
    assert(rx_chan.open("pod://pod-recv/data", 0) > 0);

    Channel tx_chan("pod-send");
    assert(tx_chan.open("pod://pod-recv/data", 0) > 0);

    const char msg[] = "HighLevelPayload";
    assert(tx_chan.send(reinterpret_cast<const uint8_t*>(msg), sizeof(msg)) == sizeof(msg));

    char buf[32];
    assert(rx_chan.recv(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 100) == sizeof(msg));
    assert(std::strcmp(buf, msg) == 0);

    rx_chan.close();
    tx_chan.close();
    ChannelManager::instance().shutdown();

    std::cout << "[PASS] test_channel_high_level_object" << std::endl;
}

void test_channel_throughput_latency_benchmark() {
    ChannelManager::instance().init(0, 7400);
    int32_t rx_h = ChannelManager::instance().open("consumer", "pod://consumer/bench", 0);
    int32_t tx_h = ChannelManager::instance().open("producer", "pod://consumer/bench", 0);

    const size_t ITERATIONS = 20000;
    uint8_t payload[128];
    std::memset(payload, 0x5A, sizeof(payload));
    uint8_t rx_payload[128];

    auto t_start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        int32_t s = ChannelManager::instance().send("producer", tx_h, payload, sizeof(payload));
        assert(s == sizeof(payload));
        int32_t r = ChannelManager::instance().recv("consumer", rx_h, rx_payload, sizeof(rx_payload), 10);
        assert(r == sizeof(payload));
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    double duration_sec = std::chrono::duration<double>(t_end - t_start).count();
    double msgs_per_sec = ITERATIONS / duration_sec;
    double avg_latency_us = (duration_sec / ITERATIONS) * 1e6;

    std::cout << "[BENCHMARK] " << ITERATIONS << " msgs in " << duration_sec << "s | "
              << msgs_per_sec << " msgs/sec | Avg Latency: " << avg_latency_us << " us" << std::endl;

    assert(msgs_per_sec > 20000.0);

    ChannelManager::instance().close("consumer", rx_h);
    ChannelManager::instance().close("producer", tx_h);
    ChannelManager::instance().shutdown();

    std::cout << "[PASS] test_channel_throughput_latency_benchmark" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Virtual Channel IPC Tests     " << std::endl;
    std::cout << "========================================" << std::endl;

    test_ring_buffer_lock_free();
    test_packet_wire_crc16();
    test_capability_firewall_ipc_gate();
    test_intra_node_channel_loopback();
    test_channel_high_level_object();
    test_channel_throughput_latency_benchmark();

    std::cout << "All Virtual Channel IPC tests passed successfully!" << std::endl;
    return 0;
}
