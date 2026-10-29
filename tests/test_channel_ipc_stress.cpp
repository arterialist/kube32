#include "ipc/channel.hpp"
#include "hal/capability_firewall.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/host_functions.hpp"
#include "wasm3.h"
#include "m3_env.h"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstring>
#include <chrono>
#include <random>

using namespace kube32;
using namespace kube32::ipc;
using namespace kube32::hal;
using namespace kube32::runtime;
using namespace kube32::osal;

// =============================================================================
// WebAssembly IPC Probe Module (323 bytes)
// Imports:
//   env.kube32_channel_open:   (i32, i32) -> i32
//   env.kube32_channel_send:   (i32, i32, i32) -> i32
//   env.kube32_channel_recv:   (i32, i32, i32, i32) -> i32
//   env.kube32_channel_close:  (i32) -> i32
// Exports:
//   call_channel_open(name_offset, flags) -> i32
//   call_channel_send(handle, data_offset, len) -> i32
//   call_channel_recv(handle, buf_offset, max_len, timeout) -> i32
//   call_channel_close(handle) -> i32
//   wo_mem_byte(offset, val) -> void
//   memory (1 page = 64KB)
// =============================================================================
static const uint8_t s_wasm_ipc_probe[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20, 0x05, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x02, 0x6a, 0x04, 0x03, 0x65, 0x6e,
    0x76, 0x13, 0x6b, 0x75, 0x62, 0x65, 0x33, 0x32, 0x5f, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c,
    0x5f, 0x6f, 0x70, 0x65, 0x6e, 0x00, 0x00, 0x03, 0x65, 0x6e, 0x76, 0x13, 0x6b, 0x75, 0x62, 0x65,
    0x33, 0x32, 0x5f, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x5f, 0x73, 0x65, 0x6e, 0x64, 0x00,
    0x01, 0x03, 0x65, 0x6e, 0x76, 0x13, 0x6b, 0x75, 0x62, 0x65, 0x33, 0x32, 0x5f, 0x63, 0x68, 0x61,
    0x6e, 0x6e, 0x65, 0x6c, 0x5f, 0x72, 0x65, 0x63, 0x76, 0x00, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x14,
    0x6b, 0x75, 0x62, 0x65, 0x33, 0x32, 0x5f, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x5f, 0x63,
    0x6c, 0x6f, 0x73, 0x65, 0x00, 0x03, 0x03, 0x06, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x03,
    0x01, 0x00, 0x01, 0x07, 0x69, 0x06, 0x11, 0x63, 0x61, 0x6c, 0x6c, 0x5f, 0x63, 0x68, 0x61, 0x6e,
    0x6e, 0x65, 0x6c, 0x5f, 0x6f, 0x70, 0x65, 0x6e, 0x00, 0x04, 0x11, 0x63, 0x61, 0x6c, 0x6c, 0x5f,
    0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x5f, 0x73, 0x65, 0x6e, 0x64, 0x00, 0x05, 0x11, 0x63,
    0x61, 0x6c, 0x6c, 0x5f, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x5f, 0x72, 0x65, 0x63, 0x76,
    0x00, 0x06, 0x12, 0x63, 0x61, 0x6c, 0x6c, 0x5f, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x5f,
    0x63, 0x6c, 0x6f, 0x73, 0x65, 0x00, 0x07, 0x0b, 0x73, 0x65, 0x74, 0x5f, 0x6d, 0x65, 0x6d, 0x5f,
    0x76, 0x61, 0x6c, 0x00, 0x08, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0a, 0x33,
    0x05, 0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x00, 0x0b, 0x0a, 0x00, 0x20, 0x00, 0x20, 0x01,
    0x20, 0x02, 0x10, 0x01, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03, 0x10,
    0x02, 0x0b, 0x06, 0x00, 0x20, 0x00, 0x10, 0x03, 0x0b, 0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0x3a,
    0x00, 0x00, 0x0b
};

// =============================================================================
// SPSC RingBuffer Concurrency, Thread Contention & Stress
// =============================================================================

void test_spsc_concurrent_stress() {
    std::cout << "\n=== [spsc] SPSC RingBuffer High-Contention Concurrency Stress ===" << std::endl;
    const size_t RING_CAPACITY = 16;
    RingBuffer ring(RING_CAPACITY);

    const uint32_t TOTAL_MSGS = 50000;
    std::atomic<bool> producer_done{false};
    std::atomic<uint32_t> produced_count{0};
    std::atomic<uint32_t> consumed_count{0};
    std::atomic<uint32_t> full_retries{0};
    std::atomic<bool> sequence_corrupted{false};
    std::atomic<bool> payload_corrupted{false};

    // Producer Thread
    std::thread producer([&]() {
        uint8_t payload[64];
        for (uint32_t i = 1; i <= TOTAL_MSGS; ++i) {
            // Encode message: seq (4B), magic (4B), i * 7 (4B), crc16 (2B)
            std::memcpy(payload, &i, 4);
            uint32_t magic = 0x55AA55AA;
            std::memcpy(payload + 4, &magic, 4);
            uint32_t val = i * 7;
            std::memcpy(payload + 8, &val, 4);
            uint16_t crc = crc16_ccitt(payload, 12);
            std::memcpy(payload + 12, &crc, 2);

            while (!ring.push(0xABCD, payload, 14)) {
                full_retries.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            produced_count.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer Thread
    std::thread consumer([&]() {
        uint32_t expected_seq = 1;
        uint8_t rx_buf[64];
        uint32_t sender = 0;

        while (expected_seq <= TOTAL_MSGS) {
            int32_t n = ring.pop(rx_buf, sizeof(rx_buf), 20, &sender);
            if (n == IPC_ERR_TIMEDOUT || n == IPC_ERR_AGAIN) {
                if (producer_done.load(std::memory_order_acquire) && ring.is_empty()) {
                    break;
                }
                continue;
            }

            if (n != 14 || sender != 0xABCD) {
                payload_corrupted.store(true);
            }

            uint32_t seq = 0;
            std::memcpy(&seq, rx_buf, 4);
            if (seq != expected_seq) {
                sequence_corrupted.store(true);
            }

            uint16_t expected_crc = crc16_ccitt(rx_buf, 12);
            uint16_t actual_crc = 0;
            std::memcpy(&actual_crc, rx_buf + 12, 2);
            if (expected_crc != actual_crc) {
                payload_corrupted.store(true);
            }

            expected_seq++;
            consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    std::cout << "  Produced: " << produced_count.load()
              << " | Consumed: " << consumed_count.load()
              << " | Full Retries (Backpressure Events): " << full_retries.load() << std::endl;

    assert(!sequence_corrupted.load() && "SPSC RingBuffer experienced sequence order corruption!");
    assert(!payload_corrupted.load() && "SPSC RingBuffer experienced payload corruption!");
    assert(consumed_count.load() == TOTAL_MSGS && "SPSC RingBuffer lost messages under high contention!");
    assert(ring.is_empty() && "RingBuffer not empty after consuming all messages!");
    std::cout << "[PASS] SPSC RingBuffer 50,000 message concurrency verified clean." << std::endl;
}

void test_multi_producer_channel_stress() {
    std::cout << "\n=== [multi-producer] Multi-Producer ChannelManager Contention Stress ===" << std::endl;
    ChannelManager::instance().init(0, 7400);

    const std::string channel_uri = "pod://service-worker/work-queue";
    int32_t rx_handle = ChannelManager::instance().open("service-worker", channel_uri, 0);
    assert(rx_handle > 0);

    const int NUM_PRODUCERS = 4;
    const int MSGS_PER_PRODUCER = 2500;
    std::atomic<bool> start_flag{false};
    std::atomic<uint32_t> total_sent{0};
    std::atomic<uint32_t> total_dropped{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            std::string pod_name = "producer-pod-" + std::to_string(p);
            int32_t tx_h = ChannelManager::instance().open(pod_name, channel_uri, 0);
            assert(tx_h > 0);

            while (!start_flag.load()) {
                std::this_thread::yield();
            }

            uint8_t msg[32];
            for (int i = 0; i < MSGS_PER_PRODUCER; ++i) {
                std::memcpy(msg, &p, 4);
                std::memcpy(msg + 4, &i, 4);
                int32_t res = ChannelManager::instance().send(pod_name, tx_h, msg, 8);
                if (res == 8) {
                    total_sent.fetch_add(1, std::memory_order_relaxed);
                } else {
                    total_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ChannelManager::instance().close(pod_name, tx_h);
        });
    }

    // Consumer drains messages
    std::atomic<bool> done_consuming{false};
    std::atomic<uint32_t> total_received{0};
    std::thread consumer([&]() {
        uint8_t rx[64];
        while (!done_consuming.load()) {
            int32_t n = ChannelManager::instance().recv("service-worker", rx_handle, rx, sizeof(rx), 5);
            if (n > 0) {
                total_received.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Final drain
        while (ChannelManager::instance().recv("service-worker", rx_handle, rx, sizeof(rx), 0) > 0) {
            total_received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    start_flag.store(true);
    for (auto& t : producers) {
        t.join();
    }
    // Allow consumer to finish draining remaining items
    osal::Thread::sleep_ms(50);
    done_consuming.store(true);
    consumer.join();

    std::cout << "  Multi-producer sent: " << total_sent.load()
              << " | Dropped due to full buffer: " << total_dropped.load()
              << " | Received: " << total_received.load() << std::endl;

    ChannelManager::instance().close("service-worker", rx_handle);
    ChannelManager::instance().shutdown();

    std::cout << "[PASS] Multi-producer ChannelManager completed without crash or ASan violation." << std::endl;
}

// =============================================================================
// Malformed Wire Packets & Packet Corruption over UDP
// =============================================================================

void test_wire_packet_corruption_fuzzing() {
    std::cout << "\n=== [wire] Wire Packet Corruption & Boundary Checks ===" << std::endl;

    const uint16_t UDP_PORT = 7450;
    ChannelManager::instance().init(0, UDP_PORT);

    int32_t rx_handle = ChannelManager::instance().open("victim-pod", "pod://victim-pod/telemetry", 0);
    assert(rx_handle > 0);

    // Route for target pod
    NodeEndpoint ep;
    ep.ip_address = "127.0.0.1";
    ep.udp_port = UDP_PORT;
    ep.node_id = 0;
    VirtualChannelRoutingTable::instance().register_remote_route("victim-pod", ep);

    osal::Socket tx_sock;
    assert(tx_sock.create(osal::SocketType::Udp));

    auto send_raw_udp = [&](const uint8_t* data, size_t len) {
        tx_sock.sendto(data, len, "127.0.0.1", UDP_PORT);
    };

    // 2.1 Truncated Packets (0 to 25 bytes)
    std::cout << "  [2.1] Testing truncated packets (0 to 25 bytes)..." << std::endl;
    uint8_t trunc_buf[32] = {0};
    for (size_t len = 0; len < K32C_HEADER_LEN + K32C_CRC_LEN; ++len) {
        send_raw_udp(trunc_buf, len);
    }
    osal::Thread::sleep_ms(20);

    // 2.2 Invalid Magic & Version
    std::cout << "  [2.2] Testing invalid magic and version..." << std::endl;
    {
        K32CPacket pkt = {};
        pkt.header.magic = 0xDEADBEEF; // Invalid
        pkt.header.version = K32C_VERSION;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.payload_len = 8;
        std::memcpy(pkt.payload, "BadMagic", 8);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8);
        std::memcpy(pkt.payload + 8, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8 + 2);
    }
    {
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = 0x99; // Invalid version
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.payload_len = 8;
        std::memcpy(pkt.payload, "BadVer00", 8);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8);
        std::memcpy(pkt.payload + 8, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8 + 2);
    }
    osal::Thread::sleep_ms(20);

    // 2.3 Corrupted CRC16 Checksum
    std::cout << "  [2.3] Testing corrupted CRC16 CCITT checksums..." << std::endl;
    {
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.payload_len = 10;
        std::memcpy(pkt.payload, "ValidFrame", 10);
        uint16_t bad_crc = 0x1234; // Invalid CRC
        std::memcpy(pkt.payload + 10, &bad_crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 10 + 2);
    }
    {
        // Single bit flip in CRC
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.payload_len = 8;
        std::memcpy(pkt.payload, "BitFlipC", 8);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8);
        crc ^= 0x0001; // Corrupt 1 bit
        std::memcpy(pkt.payload + 8, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 8 + 2);
    }
    osal::Thread::sleep_ms(20);

    // 2.4 Oversized & Inconsistent Lengths
    std::cout << "  [2.4] Testing oversized payload_len..." << std::endl;
    {
        // Declared payload_len > K32C_MAX_PAYLOAD_CHUNK (224)
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.payload_len = 240; // > 224
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN);
        std::memcpy(pkt.payload, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    }
    {
        // Declared payload_len > actual packet length
        uint8_t buf[64] = {0};
        auto* hdr = reinterpret_cast<K32CHeader*>(buf);
        hdr->magic = K32C_MAGIC;
        hdr->version = K32C_VERSION;
        hdr->payload_len = 100; // but packet is only 30 bytes
        send_raw_udp(buf, 30);
    }
    osal::Thread::sleep_ms(20);

    // 2.5 Out-of-bounds Fragment Indices
    std::cout << "  [2.5] Testing out-of-bounds fragment indices..." << std::endl;
    {
        // frag_index (5) >= frag_count (2)
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.flags = K32C_FLAG_FRAGMENT;
        pkt.header.seq_num = 101;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.frag_index = 5;
        pkt.header.frag_count = 2;
        pkt.header.payload_len = 10;
        std::memcpy(pkt.payload, "BadFragIdx", 10);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 10);
        std::memcpy(pkt.payload + 10, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 10 + 2);
    }
    {
        // frag_index >= 32 (UBSan shift exponent test)
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.flags = K32C_FLAG_FRAGMENT;
        pkt.header.seq_num = 102;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.frag_index = 33;
        pkt.header.frag_count = 40;
        pkt.header.payload_len = 10;
        std::memcpy(pkt.payload, "OobFragIdx", 10);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 10);
        std::memcpy(pkt.payload + 10, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 10 + 2);
    }
    osal::Thread::sleep_ms(20);

    // Assert that the victim pod's ring buffer received NONE of the corrupted frames!
    uint8_t dummy[64];
    int32_t pop_res = ChannelManager::instance().recv("victim-pod", rx_handle, dummy, sizeof(dummy), 0);
    assert(pop_res == IPC_ERR_AGAIN && "Victim pod received corrupted/invalid frames into its ring buffer!");

    // 2.6 Send a VALID frame afterwards to verify channel transport is still alive and healthy
    std::cout << "  [2.6] Verifying channel transport liveness after corrupt packet barrage..." << std::endl;
    {
        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.seq_num = 999;
        pkt.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt.header.sender_hash = fnv1a_32("clean-sender");
        pkt.header.frag_index = 0;
        pkt.header.frag_count = 1;
        pkt.header.payload_len = 12;
        std::memcpy(pkt.payload, "CleanPayload", 12);
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 12);
        std::memcpy(pkt.payload + 12, &crc, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt), K32C_HEADER_LEN + 12 + 2);
    }
    osal::Thread::sleep_ms(30);

    uint8_t valid_rx[64] = {0};
    int32_t recvd = ChannelManager::instance().recv("victim-pod", rx_handle, valid_rx, sizeof(valid_rx), 100);
    assert(recvd == 12 && "Legitimate frame not received after packet barrage!");
    // 2.7 Multi-fragment reassembly > 256 bytes boundary test
    std::cout << "  [2.7] Testing multi-fragment reassembly > 256 bytes (slot limit)..." << std::endl;
    {
        uint16_t seq_multi = 777;
        uint8_t large_payload[300];
        std::memset(large_payload, 0x7E, sizeof(large_payload));

        // Frag 0 of 2 (224 bytes)
        K32CPacket pkt0 = {};
        pkt0.header.magic = K32C_MAGIC;
        pkt0.header.version = K32C_VERSION;
        pkt0.header.flags = K32C_FLAG_FRAGMENT | K32C_FLAG_FIRST_FRAG;
        pkt0.header.seq_num = seq_multi;
        pkt0.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt0.header.sender_hash = fnv1a_32("frag-sender");
        pkt0.header.frag_index = 0;
        pkt0.header.frag_count = 2;
        pkt0.header.payload_len = 224;
        std::memcpy(pkt0.payload, large_payload, 224);
        uint16_t crc0 = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt0), K32C_HEADER_LEN + 224);
        std::memcpy(pkt0.payload + 224, &crc0, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt0), K32C_HEADER_LEN + 224 + 2);

        // Frag 1 of 2 (76 bytes)
        K32CPacket pkt1 = {};
        pkt1.header.magic = K32C_MAGIC;
        pkt1.header.version = K32C_VERSION;
        pkt1.header.flags = K32C_FLAG_FRAGMENT | K32C_FLAG_LAST_FRAG;
        pkt1.header.seq_num = seq_multi;
        pkt1.header.channel_hash = fnv1a_32("pod://victim-pod/telemetry");
        pkt1.header.sender_hash = fnv1a_32("frag-sender");
        pkt1.header.frag_index = 1;
        pkt1.header.frag_count = 2;
        pkt1.header.payload_len = 76;
        std::memcpy(pkt1.payload, large_payload + 224, 76);
        uint16_t crc1 = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt1), K32C_HEADER_LEN + 76);
        std::memcpy(pkt1.payload + 76, &crc1, 2);
        send_raw_udp(reinterpret_cast<const uint8_t*>(&pkt1), K32C_HEADER_LEN + 76 + 2);

        osal::Thread::sleep_ms(30);

        uint8_t large_rx[512] = {0};
        int32_t recvd_large = ChannelManager::instance().recv("victim-pod", rx_handle, large_rx, sizeof(large_rx), 100);
        std::cout << "  [2.7] Multi-fragment recv returned: " << recvd_large
                  << " (Expected 300, verified delivered)" << std::endl;
        assert(recvd_large == 300 && "Multi-fragment reassembled payload was not delivered!");
        assert(std::memcmp(large_rx, large_payload, 300) == 0 && "Corrupted payload in reassembled message");
    }

    tx_sock.close();
    ChannelManager::instance().close("victim-pod", rx_handle);
    ChannelManager::instance().shutdown();

    std::cout << "[PASS] Wire packet corruption fuzzing completed cleanly without server crash." << std::endl;
}

// =============================================================================
// Wasm3 Sandbox Boundary Checks
// =============================================================================

void test_wasm3_sandbox_boundary_attacks() {
    std::cout << "\n=== [sandbox] Wasm3 Sandbox Boundary Checks ===" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();


    // 3.1 Unauthorized Pod (No IPC Permission)
    {
        std::string unauth_pod = "unauth-pod";
        std::string manifest_unauth = R"({
            "spec": { "containers": [{ "name": "unauth-container" }] }
        })";
        std::string err;
        assert(fw.register_pod(unauth_pod, manifest_unauth, err));

        auto runtime = create_wasm_runtime();
        bool dep = runtime->deploy_workload(unauth_pod, s_wasm_ipc_probe, sizeof(s_wasm_ipc_probe), 64 * 1024);
        if (!dep) {
            std::cout << "Deploy failed: " << runtime->get_status(unauth_pod).exit_message << std::endl;
        }
        assert(dep);

        // Call call_channel_open: must return -EPERM (-1)
        int32_t res = runtime->execute_function(unauth_pod, "call_channel_open", {100, 0});
        std::cout << "  [3.1] Unauthorized call_channel_open returned: " << res << " (expected -1)" << std::endl;
        assert(res == HAL_ERR_PERM); // -1
        assert(runtime->terminate(unauth_pod));
    }

    // 3.2 Authorized Pod: Boundary and OOB Attacks
    {
        std::string auth_pod = "auth-pod";
        std::string manifest_auth = R"({
            "metadata": { "annotations": { "kube32.io/ipc": "true" } },
            "spec": { "containers": [{ "name": "auth-container" }] }
        })";
        std::string err;
        assert(fw.register_pod(auth_pod, manifest_auth, err));

        auto runtime = create_wasm_runtime();
        assert(runtime->deploy_workload(auth_pod, s_wasm_ipc_probe, sizeof(s_wasm_ipc_probe), 64 * 1024));

        // ChannelManager must be initialized
        ChannelManager::instance().init(0, 7400);

        // A. OOB on channel_open
        // 1 page = 65536 bytes. Offset 65536 is exactly OOB.
        int32_t oob_open_1 = runtime->execute_function(auth_pod, "call_channel_open", {65536, 0});
        std::cout << "  [3.2.A] call_channel_open with offset 65536 returned: " << oob_open_1 << " (expected -14)" << std::endl;
        assert(oob_open_1 == HAL_ERR_FAULT); // -14

        int32_t oob_open_2 = runtime->execute_function(auth_pod, "call_channel_open", {100000, 0});
        assert(oob_open_2 == HAL_ERR_FAULT); // -14

        // Unterminated string near memory boundary
        // Set last bytes to non-zero without null terminator
        for (int i = 65500; i < 65536; ++i) {
            runtime->execute_function(auth_pod, "set_mem_val", {i, 'x'});
        }
        int32_t oob_open_unterminated = runtime->execute_function(auth_pod, "call_channel_open", {65500, 0});
        std::cout << "  [3.2.B] call_channel_open with unterminated string returned: " << oob_open_unterminated << " (expected -14)" << std::endl;
        assert(oob_open_unterminated == HAL_ERR_FAULT); // -14

        // Legitimate open: write "pod://auth-pod/telemetry\0" at offset 1000
        const char chan_uri[] = "pod://auth-pod/telemetry";
        for (size_t i = 0; i < sizeof(chan_uri); ++i) {
            runtime->execute_function(auth_pod, "set_mem_val", {static_cast<int32_t>(1000 + i), static_cast<int32_t>(chan_uri[i])});
        }
        int32_t valid_handle = runtime->execute_function(auth_pod, "call_channel_open", {1000, 0});
        std::cout << "  [3.2.C] Valid call_channel_open returned handle: " << valid_handle << std::endl;
        assert(valid_handle > 0);

        // B. Attacks on channel_send
        // B.1 Invalid handle
        int32_t bad_h_send = runtime->execute_function(auth_pod, "call_channel_send", {-1, 500, 10});
        assert(bad_h_send == IPC_ERR_BADF); // -9
        bad_h_send = runtime->execute_function(auth_pod, "call_channel_send", {999, 500, 10});
        assert(bad_h_send == IPC_ERR_BADF); // -9

        // B.2 OOB data pointer
        int32_t oob_send_1 = runtime->execute_function(auth_pod, "call_channel_send", {valid_handle, 65536, 1});
        std::cout << "  [3.2.D] call_channel_send with offset 65536 returned: " << oob_send_1 << " (expected -14)" << std::endl;
        assert(oob_send_1 == HAL_ERR_FAULT); // -14

        // B.3 offset + len > mem_size
        int32_t oob_send_2 = runtime->execute_function(auth_pod, "call_channel_send", {valid_handle, 65530, 10});
        assert(oob_send_2 == HAL_ERR_FAULT); // -14

        // B.4 Integer overflow in offset + len (0xFFFFFFFF + 10)
        int32_t oob_send_overflow = runtime->execute_function(auth_pod, "call_channel_send", {valid_handle, static_cast<int32_t>(0xFFFFFFFF), 10});
        assert(oob_send_overflow == HAL_ERR_FAULT); // -14

        // C. Attacks on channel_recv
        // C.1 Invalid handle
        int32_t bad_h_recv = runtime->execute_function(auth_pod, "call_channel_recv", {-1, 2000, 32, 0});
        assert(bad_h_recv == IPC_ERR_BADF); // -9

        // C.2 OOB buffer pointer
        int32_t oob_recv_1 = runtime->execute_function(auth_pod, "call_channel_recv", {valid_handle, 65536, 1, 0});
        std::cout << "  [3.2.E] call_channel_recv with offset 65536 returned: " << oob_recv_1 << " (expected -14)" << std::endl;
        assert(oob_recv_1 == HAL_ERR_FAULT); // -14

        // C.3 offset + max_len > mem_size
        int32_t oob_recv_2 = runtime->execute_function(auth_pod, "call_channel_recv", {valid_handle, 65530, 20, 0});
        assert(oob_recv_2 == HAL_ERR_FAULT); // -14

        // C.4 Integer overflow in offset + max_len
        int32_t oob_recv_overflow = runtime->execute_function(auth_pod, "call_channel_recv", {valid_handle, static_cast<int32_t>(0xFFFFFFFF), 10, 0});
        assert(oob_recv_overflow == HAL_ERR_FAULT); // -14

        // D. Cross-Pod Handle Hijacking / Isolation Attack
        // Register a second authorized pod
        std::string pirate_pod = "pirate-pod";
        assert(fw.register_pod(pirate_pod, manifest_auth, err));
        auto runtime2 = create_wasm_runtime();
        assert(runtime2->deploy_workload(pirate_pod, s_wasm_ipc_probe, sizeof(s_wasm_ipc_probe), 64 * 1024));

        // Pirate attempts to send to valid_handle owned by auth_pod: must return -EBADF
        int32_t pirate_send = runtime2->execute_function(pirate_pod, "call_channel_send", {valid_handle, 100, 10});
        std::cout << "  [3.2.F] Pirate pod call_channel_send on other pod's handle returned: " << pirate_send << " (expected -9)" << std::endl;
        assert(pirate_send == IPC_ERR_BADF); // -9

        // Pirate attempts to recv from valid_handle owned by auth_pod: must return -EBADF
        int32_t pirate_recv = runtime2->execute_function(pirate_pod, "call_channel_recv", {valid_handle, 100, 10, 0});
        std::cout << "  [3.2.G] Pirate pod call_channel_recv on other pod's handle returned: " << pirate_recv << " (expected -9)" << std::endl;
        assert(pirate_recv == IPC_ERR_BADF); // -9

        // Pirate attempts to close auth_pod's handle: must return -EBADF
        int32_t pirate_close = runtime2->execute_function(pirate_pod, "call_channel_close", {valid_handle});
        assert(pirate_close == IPC_ERR_BADF); // -9

        // Clean close by owner
        int32_t owner_close = runtime->execute_function(auth_pod, "call_channel_close", {valid_handle});
        assert(owner_close == IPC_OK);

        // After close, handle is dead
        assert(runtime->execute_function(auth_pod, "call_channel_send", {valid_handle, 100, 10}) == IPC_ERR_BADF);

        assert(runtime->terminate(auth_pod));
        assert(runtime2->terminate(pirate_pod));
        ChannelManager::instance().shutdown();
    }

    std::cout << "[PASS] Wasm3 sandbox boundary and isolation verified." << std::endl;
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "   Kube32 Channel IPC Stress Tests                      " << std::endl;
    std::cout << "========================================================" << std::endl;

    test_spsc_concurrent_stress();
    test_multi_producer_channel_stress();
    test_wire_packet_corruption_fuzzing();
    test_wasm3_sandbox_boundary_attacks();

    std::cout << "\n========================================================" << std::endl;
    std::cout << " >>> ALL CHANNEL IPC STRESS TESTS PASSED <<< " << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
