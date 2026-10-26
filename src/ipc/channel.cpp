#include "ipc/channel.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cstring>
#include <algorithm>
#include <iostream>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
static const char* TAG = "Kube32Ipc";
#endif

namespace kube32::ipc {

// =============================================================================
// Hash & Checksum Utilities
// =============================================================================
uint32_t fnv1a_32(const char* data, size_t len) {
    uint32_t hash = 0x811C9DC5;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 0x01000193;
    }
    return hash;
}

uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t init) {
    uint16_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// =============================================================================
// Lock-Free SPSC RingBuffer Implementation
// =============================================================================
RingBuffer::RingBuffer(size_t capacity)
    : capacity_(capacity), mask_(capacity - 1), slots_(capacity) {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

bool RingBuffer::is_empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_relaxed);
}

bool RingBuffer::is_full() const {
    return (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire)) >= capacity_;
}

size_t RingBuffer::size() const {
    uint32_t h = head_.load(std::memory_order_acquire);
    uint32_t t = tail_.load(std::memory_order_relaxed);
    return (h >= t) ? (h - t) : (capacity_ - (t - h));
}

bool RingBuffer::push(uint32_t sender_hash, const uint8_t* data, size_t len) {
    if (!data || len > K32C_RING_SLOT_SIZE) return false;

    osal::LockGuard guard(producer_mutex_);

    uint32_t current_head = head_.load(std::memory_order_relaxed);
    uint32_t current_tail = tail_.load(std::memory_order_acquire);

    if (current_head - current_tail >= capacity_) {
        return false; // Buffer full
    }

    size_t index = current_head & mask_;
    slots_[index].sender_hash = sender_hash;
    slots_[index].len = static_cast<uint16_t>(len);
    std::memcpy(slots_[index].data, data, len);

    // Release fence guarantees payload is committed before updating head
    head_.store(current_head + 1, std::memory_order_release);

    // Notify waiting receiver
    cv_data_ready_.notify_one();
    return true;
}

int32_t RingBuffer::pop(uint8_t* out_buf, size_t max_len, uint32_t timeout_ms, uint32_t* out_sender_hash) {
    if (!out_buf) return IPC_ERR_FAULT;

    uint32_t current_tail = tail_.load(std::memory_order_relaxed);
    uint32_t current_head = head_.load(std::memory_order_acquire);

    // Fast path: data ready immediately
    if (current_head != current_tail) {
        size_t index = current_tail & mask_;
        size_t copy_len = std::min<size_t>(max_len, slots_[index].len);
        std::memcpy(out_buf, slots_[index].data, copy_len);
        if (out_sender_hash) *out_sender_hash = slots_[index].sender_hash;

        tail_.store(current_tail + 1, std::memory_order_release);
        return static_cast<int32_t>(copy_len);
    }

    // Non-blocking mode
    if (timeout_ms == 0) {
        return IPC_ERR_AGAIN;
    }

    // Blocking wait with timeout
    osal::UniqueLock<osal::Mutex> lock(cv_mutex_);
    bool ready = cv_data_ready_.wait_for(lock, timeout_ms, [this]() {
        return !is_empty();
    });

    if (!ready) {
        return IPC_ERR_TIMEDOUT;
    }

    current_tail = tail_.load(std::memory_order_relaxed);
    current_head = head_.load(std::memory_order_acquire);
    if (current_head == current_tail) return IPC_ERR_AGAIN;

    size_t index = current_tail & mask_;
    size_t copy_len = std::min<size_t>(max_len, slots_[index].len);
    std::memcpy(out_buf, slots_[index].data, copy_len);
    if (out_sender_hash) *out_sender_hash = slots_[index].sender_hash;

    tail_.store(current_tail + 1, std::memory_order_release);
    return static_cast<int32_t>(copy_len);
}

// =============================================================================
// Transport Implementations: PeerUdpTransport (Host Sim)
// =============================================================================
class PeerUdpTransport : public IChannelTransport {
public:
    PeerUdpTransport(uint32_t node_id, uint16_t base_port)
        : node_id_(node_id), port_(base_port > 0 ? (base_port + node_id) : (7400 + node_id)) {}

    ~PeerUdpTransport() override { stop(); }

    bool start() override {
        if (running_.load()) return true;
        if (!sock_.create(osal::SocketType::Udp)) return false;
        sock_.set_reuse_addr(true);
        if (!sock_.bind("0.0.0.0", port_)) {
            sock_.close();
            return false;
        }
        sock_.set_timeout(100, 100);

        running_.store(true);
        osal::Thread::Config cfg;
        cfg.name = "udp_ipc_rx";
        cfg.stack_size = 32768;
        rx_thread_ = std::make_unique<osal::Thread>();
        return rx_thread_->start(rx_thread_entry, this, cfg);
    }

    void stop() override {
        if (!running_.load()) return;
        running_.store(false);
        sock_.close();
        if (rx_thread_ && rx_thread_->is_joinable()) {
            rx_thread_->join();
            rx_thread_.reset();
        }
    }

    int32_t send_frame(const NodeEndpoint& target, const uint8_t* frame, size_t len) override {
        if (!running_.load()) return IPC_ERR_IO;
        std::string ip = target.ip_address.empty() ? "127.0.0.1" : target.ip_address;
        uint16_t port = (target.udp_port > 0) ? target.udp_port : (7400 + target.node_id);
        ssize_t sent = sock_.sendto(frame, len, ip.c_str(), port);
        return (sent == static_cast<ssize_t>(len)) ? IPC_OK : IPC_ERR_IO;
    }

private:
    static void rx_thread_entry(void* arg) {
        static_cast<PeerUdpTransport*>(arg)->rx_worker();
    }

    void rx_worker() {
        uint8_t buffer[512];
        std::string sender_ip;
        uint16_t sender_port = 0;

        while (running_.load()) {
            ssize_t n = sock_.recvfrom(buffer, sizeof(buffer), &sender_ip, &sender_port);
            if (n >= static_cast<ssize_t>(K32C_HEADER_LEN + K32C_CRC_LEN)) {
                ChannelManager::instance().on_frame_received(buffer, static_cast<size_t>(n));
            }
            osal::Thread::sleep_ms(1);
        }
    }

    [[maybe_unused]] uint32_t node_id_;
    uint16_t port_;
    osal::Socket sock_;
    std::atomic<bool> running_{false};
    std::unique_ptr<osal::Thread> rx_thread_;
};

// =============================================================================
// Transport Implementations: EspNowTransport (ESP32-S3 Target)
// =============================================================================
#if defined(KUBE32_TARGET_ESP32)
class EspNowTransport : public IChannelTransport {
public:
    EspNowTransport() = default;
    ~EspNowTransport() override { stop(); }

    bool start() override {
        if (esp_now_init() != ESP_OK) return false;
        esp_now_register_recv_cb(recv_callback);
        running_ = true;
        return true;
    }

    void stop() override {
        if (running_) {
            esp_now_deinit();
            running_ = false;
        }
    }

    int32_t send_frame(const NodeEndpoint& target, const uint8_t* frame, size_t len) override {
        if (!running_) return IPC_ERR_IO;
        esp_now_peer_info_t peer_info = {};
        std::memcpy(peer_info.peer_addr, target.mac_address, 6);
        peer_info.channel = 0;
        peer_info.encrypt = false;
        if (!esp_now_is_peer_exist(target.mac_address)) {
            esp_now_add_peer(&peer_info);
        }
        esp_err_t err = esp_now_send(target.mac_address, frame, len);
        return (err == ESP_OK) ? IPC_OK : IPC_ERR_IO;
    }

private:
    static void recv_callback(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
        if (data && len >= static_cast<int>(K32C_HEADER_LEN + K32C_CRC_LEN)) {
            ChannelManager::instance().on_frame_received(data, static_cast<size_t>(len));
        }
    }
    bool running_{false};
};
#endif

// =============================================================================
// ChannelManager Implementation
// =============================================================================
ChannelManager& ChannelManager::instance() {
    static ChannelManager s_mgr;
    return s_mgr;
}

ChannelManager::ChannelManager() = default;
ChannelManager::~ChannelManager() { shutdown(); }

bool ChannelManager::init(uint32_t node_id, uint16_t base_udp_port) {
    osal::LockGuard guard(mutex_);
    if (initialized_) return true;

#if defined(KUBE32_TARGET_ESP32)
    transport_ = std::make_unique<EspNowTransport>();
#else
    transport_ = std::make_unique<PeerUdpTransport>(node_id, base_udp_port);
#endif
    if (transport_->start()) {
        initialized_ = true;
        return true;
    }
    return false;
}

void ChannelManager::shutdown() {
    osal::LockGuard guard(mutex_);
    if (transport_) {
        transport_->stop();
        transport_.reset();
    }
    handles_.clear();
    VirtualChannelRoutingTable::instance().clear();
    {
        osal::LockGuard r_guard(reassembly_mutex_);
        reassembly_table_.clear();
    }
    initialized_ = false;
}

bool ChannelManager::is_initialized() const {
    return initialized_;
}

int32_t ChannelManager::open(const std::string& pod_id, const std::string& channel_uri, uint32_t flags) {
    osal::LockGuard guard(mutex_);
    int32_t hid = next_handle_++;

    ChannelHandle ch;
    ch.handle_id = hid;
    ch.pod_id = pod_id;
    ch.channel_uri = channel_uri;
    ch.channel_hash = fnv1a_32(channel_uri);
    ch.flags = flags;

    ChannelRoute existing_route;
    if (VirtualChannelRoutingTable::instance().lookup_route(channel_uri, existing_route) && existing_route.local_ring) {
        ch.in_ring = existing_route.local_ring;
    } else {
        ch.in_ring = std::make_shared<RingBuffer>(K32C_RING_SLOT_COUNT);
        VirtualChannelRoutingTable::instance().register_local_channel(pod_id, channel_uri, ch.in_ring);
    }

    handles_[hid] = ch;
    return hid;
}

int32_t ChannelManager::send(const std::string& pod_id, int32_t handle, const uint8_t* data, size_t len) {
    ChannelRoute route;
    std::string channel_uri;
    uint32_t channel_hash = 0;
    {
        osal::LockGuard guard(mutex_);
        auto it = handles_.find(handle);
        if (it == handles_.end() || it->second.pod_id != pod_id) {
            return IPC_ERR_BADF;
        }
        channel_hash = it->second.channel_hash;
        channel_uri = it->second.channel_uri;
    }

    if (!VirtualChannelRoutingTable::instance().lookup_route(channel_uri, route)) {
        return IPC_ERR_NOENT;
    }

    uint32_t sender_hash = fnv1a_32(pod_id);

    // Fast path: Local delivery
    if (route.is_local && route.local_ring) {
        if (!route.local_ring->push(sender_hash, data, len)) {
            return IPC_ERR_AGAIN;
        }
        return static_cast<int32_t>(len);
    }

    // Remote delivery: Frame packetization & transport dispatch
    if (!transport_) return IPC_ERR_IO;

    uint16_t seq = seq_generator_.fetch_add(1);
    uint32_t target_hash = fnv1a_32(route.target_pod);

    size_t frag_count = (len + K32C_MAX_PAYLOAD_CHUNK - 1) / K32C_MAX_PAYLOAD_CHUNK;
    if (frag_count == 0) frag_count = 1;

    for (size_t i = 0; i < frag_count; ++i) {
        size_t offset = i * K32C_MAX_PAYLOAD_CHUNK;
        size_t chunk_len = std::min(K32C_MAX_PAYLOAD_CHUNK, len - offset);

        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.seq_num = seq;
        pkt.header.channel_hash = channel_hash;
        pkt.header.sender_hash = sender_hash;
        pkt.header.target_hash = target_hash;
        pkt.header.frag_index = static_cast<uint8_t>(i);
        pkt.header.frag_count = static_cast<uint8_t>(frag_count);
        pkt.header.payload_len = static_cast<uint16_t>(chunk_len);

        if (frag_count > 1) pkt.header.flags |= K32C_FLAG_FRAGMENT;
        if (i == 0) pkt.header.flags |= K32C_FLAG_FIRST_FRAG;
        if (i == frag_count - 1) pkt.header.flags |= K32C_FLAG_LAST_FRAG;

        if (chunk_len > 0) {
            std::memcpy(pkt.payload, data + offset, chunk_len);
        }

        size_t frame_bytes = K32C_HEADER_LEN + chunk_len;
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), frame_bytes);
        std::memcpy(pkt.payload + chunk_len, &crc, 2);

        int32_t res = transport_->send_frame(route.remote_endpoint, reinterpret_cast<const uint8_t*>(&pkt), frame_bytes + 2);
        if (res != IPC_OK) return res;
    }

    return static_cast<int32_t>(len);
}

int32_t ChannelManager::recv(const std::string& pod_id, int32_t handle, uint8_t* out_buf, size_t max_len, uint32_t timeout_ms) {
    std::shared_ptr<RingBuffer> ring;
    {
        osal::LockGuard guard(mutex_);
        auto it = handles_.find(handle);
        if (it == handles_.end() || it->second.pod_id != pod_id) {
            return IPC_ERR_BADF;
        }
        ring = it->second.in_ring;
    }
    if (!ring) return IPC_ERR_BADF;
    return ring->pop(out_buf, max_len, timeout_ms);
}

int32_t ChannelManager::close(const std::string& pod_id, int32_t handle) {
    osal::LockGuard guard(mutex_);
    auto it = handles_.find(handle);
    if (it == handles_.end() || it->second.pod_id != pod_id) {
        return IPC_ERR_BADF;
    }
    std::string uri = it->second.channel_uri;
    handles_.erase(it);

    bool still_in_use = false;
    for (const auto& kv : handles_) {
        if (kv.second.channel_uri == uri) {
            still_in_use = true;
            break;
        }
    }
    if (!still_in_use) {
        VirtualChannelRoutingTable::instance().unregister_channel(pod_id, uri);
    }
    return IPC_OK;
}

void ChannelManager::on_frame_received(const uint8_t* frame, size_t len) {
    stats_.frames_received.fetch_add(1, std::memory_order_relaxed);

    if (len < K32C_HEADER_LEN + K32C_CRC_LEN) return;

    const auto* hdr = reinterpret_cast<const K32CHeader*>(frame);
    if (hdr->magic != K32C_MAGIC || hdr->version != K32C_VERSION) {
        stats_.bad_magic_errors.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[IPC-RX] Dropped frame: invalid magic 0x" << std::hex << hdr->magic
                  << " (expected 0x" << K32C_MAGIC << ")" << std::dec << std::endl << std::flush;
        return;
    }
    if (hdr->payload_len > K32C_MAX_PAYLOAD_CHUNK || (K32C_HEADER_LEN + hdr->payload_len + K32C_CRC_LEN) > len) return;

    // CRC Verification
    uint16_t expected_crc = crc16_ccitt(frame, K32C_HEADER_LEN + hdr->payload_len);
    uint16_t packet_crc = 0;
    std::memcpy(&packet_crc, frame + K32C_HEADER_LEN + hdr->payload_len, 2);
    if (expected_crc != packet_crc) {
        stats_.crc_errors.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[IPC-RX] Dropped frame: CRC mismatch (expected=0x" << std::hex << expected_crc
                  << " got=0x" << packet_crc << std::dec << ")" << std::endl << std::flush;
        return; // Drop corrupted frame
    }

    std::cout << "[IPC-RX] Accepted valid K32C frame: chan=0x" << std::hex << hdr->channel_hash
              << " sender=0x" << hdr->sender_hash
              << " len=" << std::dec << hdr->payload_len << std::endl << std::flush;

    // Route lookup
    ChannelRoute route;
    if (!VirtualChannelRoutingTable::instance().lookup_route_by_hash(hdr->channel_hash, route)) return;
    if (!route.is_local || !route.local_ring) return;

    // Single-frame message fast path
    if (hdr->frag_count <= 1) {
        if (route.local_ring->push(hdr->sender_hash, frame + K32C_HEADER_LEN, hdr->payload_len)) {
            stats_.frames_delivered.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // Multi-fragment reassembly bounds check (Fix Item 4)
    if (hdr->frag_count == 0 || hdr->frag_count > 32 ||
        hdr->frag_index >= hdr->frag_count || hdr->frag_index >= 32) {
        return;
    }

    size_t offset = static_cast<size_t>(hdr->frag_index) * K32C_MAX_PAYLOAD_CHUNK;
    if (offset + hdr->payload_len > K32C_RING_SLOT_SIZE) {
        return;
    }

    uint64_t reassembly_key = (static_cast<uint64_t>(hdr->channel_hash) << 32) | hdr->seq_num;
    uint64_t now_ms = osal::System::get_uptime_ms();

    osal::LockGuard guard(reassembly_mutex_);

    // Prune stale entries (Fix Item 5: 500ms timeout reaper)
    for (auto it = reassembly_table_.begin(); it != reassembly_table_.end(); ) {
        if (now_ms >= it->second.last_activity_ms &&
            (now_ms - it->second.last_activity_ms) > K32C_REASSEMBLY_TIMEOUT) {
            it = reassembly_table_.erase(it);
        } else {
            ++it;
        }
    }

    // Capacity cap (Fix Item 5: max 8 entries)
    constexpr size_t MAX_REASSEMBLY_ENTRIES = 8;
    if (reassembly_table_.find(reassembly_key) == reassembly_table_.end()) {
        if (reassembly_table_.size() >= MAX_REASSEMBLY_ENTRIES) {
            auto oldest_it = reassembly_table_.begin();
            for (auto it = reassembly_table_.begin(); it != reassembly_table_.end(); ++it) {
                if (it->second.last_activity_ms < oldest_it->second.last_activity_ms) {
                    oldest_it = it;
                }
            }
            if (oldest_it != reassembly_table_.end()) {
                reassembly_table_.erase(oldest_it);
            }
        }
    }

    auto& entry = reassembly_table_[reassembly_key];
    if (entry.total_frags == 0) {
        entry.channel_hash = hdr->channel_hash;
        entry.sender_hash = hdr->sender_hash;
        entry.seq_num = hdr->seq_num;
        entry.total_frags = hdr->frag_count;
        entry.received_frags = 0;
        entry.received_mask = 0;
        entry.total_bytes = 0;
    }

    entry.last_activity_ms = now_ms;
    if ((entry.received_mask & (1U << hdr->frag_index)) == 0) {
        entry.received_mask |= (1U << hdr->frag_index);
        entry.received_frags++;
        if (offset + hdr->payload_len <= sizeof(entry.buffer)) {
            std::memcpy(entry.buffer + offset, frame + K32C_HEADER_LEN, hdr->payload_len);
            entry.total_bytes += hdr->payload_len;
        }
    }

    if (entry.received_frags == entry.total_frags) {
        if (route.local_ring->push(entry.sender_hash, entry.buffer, entry.total_bytes)) {
            stats_.frames_delivered.fetch_add(1, std::memory_order_relaxed);
            stats_.reassembled_msgs.fetch_add(1, std::memory_order_relaxed);
        }
        reassembly_table_.erase(reassembly_key);
    }
}

// =============================================================================
// VirtualChannelRoutingTable Implementation
// =============================================================================
VirtualChannelRoutingTable& VirtualChannelRoutingTable::instance() {
    static VirtualChannelRoutingTable s_table;
    return s_table;
}

void VirtualChannelRoutingTable::register_local_channel(const std::string& target_pod, const std::string& channel_name, std::shared_ptr<RingBuffer> ring) {
    osal::LockGuard guard(mutex_);
    uint32_t h = fnv1a_32(channel_name);
    ChannelRoute r;
    r.channel_hash = h;
    r.channel_name = channel_name;
    r.target_pod = target_pod;
    r.is_local = true;
    r.local_ring = ring;
    routes_by_hash_[h] = r;
}

void VirtualChannelRoutingTable::register_remote_route(const std::string& target_pod, const NodeEndpoint& endpoint) {
    osal::LockGuard guard(mutex_);
    pod_to_remote_node_[target_pod] = endpoint;
}

void VirtualChannelRoutingTable::unregister_channel(const std::string& target_pod, const std::string& channel_name) {
    (void)target_pod;
    osal::LockGuard guard(mutex_);
    uint32_t h = fnv1a_32(channel_name);
    routes_by_hash_.erase(h);
}

bool VirtualChannelRoutingTable::lookup_route_by_hash(uint32_t channel_hash, ChannelRoute& out_route) const {
    osal::LockGuard guard(mutex_);
    auto it = routes_by_hash_.find(channel_hash);
    if (it != routes_by_hash_.end()) {
        out_route = it->second;
        return true;
    }
    return false;
}

bool VirtualChannelRoutingTable::lookup_route(const std::string& channel_uri, ChannelRoute& out_route) const {
    osal::LockGuard guard(mutex_);
    uint32_t h = fnv1a_32(channel_uri);
    auto it = routes_by_hash_.find(h);
    if (it != routes_by_hash_.end()) {
        out_route = it->second;
        return true;
    }

    if (channel_uri.rfind("pod://", 0) == 0) {
        std::string rem = channel_uri.substr(6);
        size_t slash = rem.find('/');
        std::string target_pod = (slash != std::string::npos) ? rem.substr(0, slash) : rem;
        auto remote_it = pod_to_remote_node_.find(target_pod);
        if (remote_it != pod_to_remote_node_.end()) {
            out_route.channel_hash = h;
            out_route.channel_name = channel_uri;
            out_route.target_pod = target_pod;
            out_route.is_local = false;
            out_route.remote_endpoint = remote_it->second;
            return true;
        }
    }
    return false;
}

void VirtualChannelRoutingTable::clear() {
    osal::LockGuard guard(mutex_);
    routes_by_hash_.clear();
    pod_to_remote_node_.clear();
}

// =============================================================================
// High-Level Channel Wrapper Implementation
// =============================================================================
Channel::Channel(const std::string& owner_pod_id) : owner_pod_id_(owner_pod_id) {}
Channel::~Channel() { close(); }

int32_t Channel::open(const std::string& channel_uri, uint32_t flags) {
    close();
    handle_ = ChannelManager::instance().open(owner_pod_id_, channel_uri, flags);
    return handle_;
}

int32_t Channel::send(const uint8_t* data, size_t len) {
    if (handle_ <= 0) return IPC_ERR_BADF;
    return ChannelManager::instance().send(owner_pod_id_, handle_, data, len);
}

int32_t Channel::recv(uint8_t* out_buf, size_t max_len, uint32_t timeout_ms) {
    if (handle_ <= 0) return IPC_ERR_BADF;
    return ChannelManager::instance().recv(owner_pod_id_, handle_, out_buf, max_len, timeout_ms);
}

void Channel::close() {
    if (handle_ > 0) {
        ChannelManager::instance().close(owner_pod_id_, handle_);
        handle_ = -1;
    }
}

} // namespace kube32::ipc
