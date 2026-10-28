#include "ipc/channel.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cstring>
#include <algorithm>
#include <cstdio>
#include <iostream>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
RingBuffer::RingBuffer(size_t capacity, size_t slot_size)
    : capacity_(capacity), mask_(capacity - 1), slot_size_(slot_size), slots_(capacity), payload_(capacity * slot_size) {
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
    if (!data || len > slot_size_) return false;

    osal::LockGuard guard(producer_mutex_);

    uint32_t current_head = head_.load(std::memory_order_relaxed);
    uint32_t current_tail = tail_.load(std::memory_order_acquire);

    if (current_head - current_tail >= capacity_) {
        return false; // Buffer full
    }

    size_t index = current_head & mask_;
    slots_[index].sender_hash = sender_hash;
    slots_[index].len = static_cast<uint16_t>(len);
    std::memcpy(payload_.data() + index * slot_size_, data, len);

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
        std::memcpy(out_buf, payload_.data() + index * slot_size_, copy_len);
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
    std::memcpy(out_buf, payload_.data() + index * slot_size_, copy_len);
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
        bool bound = false;
        for (int attempt = 0; attempt < 20 && !bound; ++attempt) {
            bound = sock_.bind("0.0.0.0", port_);
            if (!bound) osal::Thread::sleep_ms(100);
        }
        if (!bound) {
            std::cout << "[IPC] UDP transport failed to bind port " << port_ << std::endl;
            sock_.close();
            return false;
        }
        std::cout << "[IPC] UDP channel transport listening on port " << port_ << std::endl;
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
                NodeEndpoint src;
                src.is_espnow = false;
                src.ip_address = sender_ip;
                src.udp_port = sender_port;
                ChannelManager::instance().on_frame_received(buffer, static_cast<size_t>(n), &src);
            }
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
        esp_now_register_send_cb(send_callback);
        s_send_sem = xSemaphoreCreateBinary();
        // Default ESP-NOW rate is 1 Mbps; raise it so fragmented messages move
        // at tens of KB/s instead of a few. Both nodes accept any rate on RX.
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        wifi_interface_t ifx = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? WIFI_IF_AP : WIFI_IF_STA;
        esp_wifi_config_espnow_rate(ifx, WIFI_PHY_RATE_24M);
        if (mode == WIFI_MODE_APSTA) esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M);
        ifx_ = ifx;
        // Broadcast peer for HELLO frames
        static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        ensure_peer(bcast);
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
        ensure_peer(target.mac_address);
        // Serialize sends: ESP-NOW has one in-flight frame per peer and
        // returns ESP_ERR_ESPNOW_NO_MEM when flooded.
        osal::LockGuard guard(send_mutex_);
        xSemaphoreTake(s_send_sem, 0);
        esp_err_t err = esp_now_send(target.mac_address, frame, len);
        if (err != ESP_OK) return IPC_ERR_IO;
        if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(30)) != pdTRUE) return IPC_ERR_TIMEDOUT;
        return s_last_send_ok ? IPC_OK : IPC_ERR_HOSTUNREACH;
    }

private:
    static SemaphoreHandle_t s_send_sem;
    static volatile bool s_last_send_ok;
    osal::Mutex send_mutex_;
    wifi_interface_t ifx_{WIFI_IF_STA};

    void ensure_peer(const uint8_t* mac) {
        if (esp_now_is_peer_exist(mac)) return;
        esp_now_peer_info_t peer_info = {};
        std::memcpy(peer_info.peer_addr, mac, 6);
        peer_info.channel = 0;
        peer_info.encrypt = false;
        peer_info.ifidx = ifx_;
        esp_now_add_peer(&peer_info);
    }

    static void send_callback(const wifi_tx_info_t* info, esp_now_send_status_t status) {
        (void)info;
        s_last_send_ok = (status == ESP_NOW_SEND_SUCCESS);
        if (s_send_sem) xSemaphoreGive(s_send_sem);
    }

    static void recv_callback(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
        // Runs on the Wi-Fi task: copy out and let the dispatcher thread parse it
        if (data && len >= static_cast<int>(K32C_HEADER_LEN + K32C_CRC_LEN) && len <= 250) {
            ChannelManager::instance().enqueue_raw_frame((info && info->src_addr) ? info->src_addr : nullptr, data, static_cast<size_t>(len));
        }
    }
    bool running_{false};
};
SemaphoreHandle_t EspNowTransport::s_send_sem = nullptr;
volatile bool EspNowTransport::s_last_send_ok = false;
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
    std::cout << "[IPC] channel transport failed to start (node " << node_id << ")" << std::endl;
    transport_.reset();
    return false;
}

void ChannelManager::shutdown() {
    background_running_.store(false);
    if (dispatch_thread_) {
        dispatch_thread_->join();
        dispatch_thread_.reset();
    }
    {
        osal::LockGuard sguard(sub_mutex_);
        subs_.clear();
    }
    {
        osal::LockGuard pguard(peer_mutex_);
        peers_.clear();
    }
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
#if defined(KUBE32_TARGET_ESP32)
        ch.in_ring = std::make_shared<RingBuffer>(8, 512);
#else
        ch.in_ring = std::make_shared<RingBuffer>(K32C_RING_SLOT_COUNT);
#endif
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
    uint32_t target_hash = fnv1a_32(route.target_pod);
    send_frames_to(route.remote_endpoint, channel_hash, sender_hash, target_hash, data, len, 0);
    return static_cast<int32_t>(len);
}

int32_t ChannelManager::send_frames_to(const NodeEndpoint& ep, uint32_t channel_hash, uint32_t sender_hash, uint32_t target_hash,
                                       const uint8_t* data, size_t len, uint8_t extra_flags) {
    if (!transport_) return IPC_ERR_IO;
    uint16_t seq = seq_generator_.fetch_add(1);
    size_t frag_count = (len + K32C_MAX_PAYLOAD_CHUNK - 1) / K32C_MAX_PAYLOAD_CHUNK;
    if (frag_count == 0) frag_count = 1;
    if (frag_count > 32) frag_count = 32; // receiver caps reassembly at 32 fragments

    for (size_t i = 0; i < frag_count; ++i) {
        size_t offset = i * K32C_MAX_PAYLOAD_CHUNK;
        size_t chunk_len = std::min(K32C_MAX_PAYLOAD_CHUNK, len - offset);

        K32CPacket pkt = {};
        pkt.header.magic = K32C_MAGIC;
        pkt.header.version = K32C_VERSION;
        pkt.header.flags = extra_flags;
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
        if (chunk_len > 0) std::memcpy(pkt.payload, data + offset, chunk_len);

        size_t frame_bytes = K32C_HEADER_LEN + chunk_len;
        uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), frame_bytes);
        std::memcpy(pkt.payload + chunk_len, &crc, 2);

        int32_t res = transport_->send_frame(ep, reinterpret_cast<const uint8_t*>(&pkt), frame_bytes + 2);
        if (res == IPC_OK) stats_.frames_sent.fetch_add(1, std::memory_order_relaxed);
        else { stats_.send_errors.fetch_add(1, std::memory_order_relaxed); return res; }
    }
    return IPC_OK;
}

// =============================================================================
// Topic publish/subscribe and peers
// =============================================================================
int32_t ChannelManager::publish(const std::string& pod_id, const std::string& topic, const uint8_t* data, size_t len) {
    if (!data && len > 0) return IPC_ERR_FAULT;
    if (len > K32C_RING_SLOT_SIZE) return IPC_ERR_INVAL;
    uint32_t channel_hash = fnv1a_32(topic);
    uint32_t sender_hash = fnv1a_32(pod_id);

    // Local pods and host subscribers
    deliver_local(channel_hash, sender_hash, data, len);

    // Remote peers
    std::vector<PeerInfo> targets = peers();
    for (const auto& p : targets) {
        if (p.node_id == local_node_id_) continue;
        int32_t rc = send_frames_to(p.endpoint, channel_hash, sender_hash, 0, data, len, 0);
        static int traced = 0;
        if (traced < 8) { traced++; printf("[TRACE] ipc tx -> node %u len=%u rc=%d\n", (unsigned)p.node_id, (unsigned)len, (int)rc); }
    }
    return static_cast<int32_t>(len);
}

void ChannelManager::deliver_local(uint32_t channel_hash, uint32_t sender_hash, const uint8_t* data, size_t len) {
    ChannelRoute route;
    if (VirtualChannelRoutingTable::instance().lookup_route_by_hash(channel_hash, route) && route.is_local && route.local_ring) {
        if (route.local_ring->push(sender_hash, data, len)) {
            stats_.frames_delivered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool has_sub = false;
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& sub : subs_) {
            if (sub.topic_hash == channel_hash) { has_sub = true; break; }
        }
    }
    if (has_sub && dispatch_ring_) {
        // Prefix the payload with the channel hash so the dispatcher can route it
        uint8_t tmp[264];
        if (len + 4 <= sizeof(tmp) && len + 4 <= dispatch_ring_->slot_size()) {
            std::memcpy(tmp, &channel_hash, 4);
            if (len > 0) std::memcpy(tmp + 4, data, len);
            dispatch_ring_->push(sender_hash, tmp, len + 4);
        }
    }
}

uint32_t ChannelManager::subscribe(const std::string& topic, TopicCallback cb) {
    osal::LockGuard guard(sub_mutex_);
    Subscription s;
    s.id = next_sub_id_++;
    s.topic_hash = fnv1a_32(topic);
    s.topic = topic;
    s.cb = std::move(cb);
    subs_.push_back(std::move(s));
    return subs_.back().id;
}

void ChannelManager::unsubscribe(uint32_t id) {
    osal::LockGuard guard(sub_mutex_);
    for (auto it = subs_.begin(); it != subs_.end(); ++it) {
        if (it->id == id) { subs_.erase(it); return; }
    }
}

void ChannelManager::enqueue_raw_frame(const uint8_t* src_mac, const uint8_t* frame, size_t len) {
    if (!rx_ring_ || len + 6 > rx_ring_->slot_size()) return;
    uint8_t tmp[6 + 250];
    if (src_mac) std::memcpy(tmp, src_mac, 6); else std::memset(tmp, 0, 6);
    std::memcpy(tmp + 6, frame, len);
    rx_ring_->push(0xFFFFFFFFu, tmp, len + 6);
}

void ChannelManager::dispatch_loop() {
    uint8_t buf[264];
    while (background_running_.load()) {
        uint32_t sender = 0;
        uint64_t now = osal::System::get_uptime_ms();
        if (hello_interval_ms_ > 0 && now - last_hello_ms_ >= hello_interval_ms_) {
            last_hello_ms_ = now;
            send_hello();
        }
        // Raw frames from the ESP-NOW callback
        if (rx_ring_) {
            int32_t rn;
            while ((rn = rx_ring_->pop(buf, sizeof(buf), 0, &sender)) > 6) {
                NodeEndpoint src;
                src.is_espnow = true;
                std::memcpy(src.mac_address, buf, 6);
                on_frame_received(buf + 6, static_cast<size_t>(rn - 6), &src);
            }
        }
        int32_t n = dispatch_ring_ ? dispatch_ring_->pop(buf, sizeof(buf), 20, &sender) : IPC_ERR_TIMEDOUT;
        if (n < 4) {
            osal::System::feed_watchdog();
            continue;
        }
        uint32_t hash = 0;
        std::memcpy(&hash, buf, 4);
        std::vector<Subscription> matched;
        {
            osal::LockGuard guard(sub_mutex_);
            for (const auto& sub : subs_) if (sub.topic_hash == hash) matched.push_back(sub);
        }
        for (const auto& sub : matched) {
            if (sub.cb) sub.cb(sub.topic, sender, buf + 4, static_cast<size_t>(n - 4));
        }
        osal::System::feed_watchdog();
    }
}

bool ChannelManager::start_background(uint32_t hello_interval_ms, size_t dispatcher_stack_bytes, bool stack_in_psram) {
    if (background_running_.load()) return true;
    hello_interval_ms_ = hello_interval_ms;
    // Events are capped at K32_MAX_EVENT_BYTES (200) + 4 bytes of topic hash
    if (!dispatch_ring_) dispatch_ring_ = std::make_shared<RingBuffer>(8, 256);
#if defined(KUBE32_TARGET_ESP32)
    if (!rx_ring_) rx_ring_ = std::make_shared<RingBuffer>(8, 264);
#endif
    background_running_.store(true);
    dispatch_thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = "k32_ipc_disp";
    cfg.stack_size = dispatcher_stack_bytes;
    cfg.stack_in_psram = stack_in_psram;
    cfg.priority = 5;
    bool ok = dispatch_thread_->start([](void* p) { static_cast<ChannelManager*>(p)->dispatch_loop(); }, this, cfg);
    if (!ok) background_running_.store(false);
    return ok;
}

void ChannelManager::set_local_identity(uint32_t node_id, const std::string& node_name) {
    local_node_id_ = node_id;
    local_node_name_ = node_name;
}

void ChannelManager::add_peer(const PeerInfo& peer) {
    osal::LockGuard guard(peer_mutex_);
    PeerInfo& slot = peers_[peer.node_id];
    uint32_t count = slot.hello_count;
    slot = peer;
    slot.hello_count = count;
    if (slot.last_seen_ms == 0) slot.last_seen_ms = osal::System::get_uptime_ms();
}

std::vector<PeerInfo> ChannelManager::peers() const {
    osal::LockGuard guard(peer_mutex_);
    std::vector<PeerInfo> out;
    for (const auto& kv : peers_) out.push_back(kv.second);
    return out;
}

bool ChannelManager::find_peer_by_name(const std::string& node_name, PeerInfo& out) const {
    osal::LockGuard guard(peer_mutex_);
    for (const auto& kv : peers_) {
        if (kv.second.node_name == node_name) { out = kv.second; return true; }
    }
    return false;
}

void ChannelManager::send_hello() {
    if (!transport_ || !initialized_) return;
    uint8_t payload[64];
    std::memcpy(payload, &local_node_id_, 4);
    size_t name_len = std::min<size_t>(local_node_name_.size(), sizeof(payload) - 4);
    std::memcpy(payload + 4, local_node_name_.data(), name_len);
    uint32_t hash = fnv1a_32(K32C_HELLO_TOPIC, std::strlen(K32C_HELLO_TOPIC));
    uint32_t sender = fnv1a_32(local_node_name_);
#if defined(KUBE32_TARGET_ESP32)
    NodeEndpoint bcast;
    bcast.is_espnow = true;
    std::memset(bcast.mac_address, 0xff, 6);
    send_frames_to(bcast, hash, sender, 0, payload, 4 + name_len, K32C_FLAG_PING);
#else
    for (const auto& p : peers()) {
        if (p.node_id == local_node_id_) continue;
        send_frames_to(p.endpoint, hash, sender, 0, payload, 4 + name_len, K32C_FLAG_PING);
    }
#endif
}

void ChannelManager::handle_hello(const K32CHeader* hdr, const uint8_t* payload, size_t len, const NodeEndpoint* source) {
    (void)hdr;
    if (len < 4) return;
    uint32_t node_id = 0;
    std::memcpy(&node_id, payload, 4);
    if (node_id == local_node_id_) return;
    stats_.hello_rx.fetch_add(1, std::memory_order_relaxed);
    PeerInfo info;
    info.node_id = node_id;
    info.node_name.assign(reinterpret_cast<const char*>(payload + 4), len - 4);
    if (source) info.endpoint = *source;
    info.endpoint.node_id = node_id;
    if (info.endpoint.udp_port == 0) info.endpoint.udp_port = 7400;
    osal::LockGuard guard(peer_mutex_);
    auto it = peers_.find(node_id);
    bool is_new = (it == peers_.end());
    PeerInfo& slot = peers_[node_id];
    uint32_t count = is_new ? 0 : slot.hello_count;
    slot = info;
    slot.hello_count = count + 1;
    slot.last_seen_ms = osal::System::get_uptime_ms();
    if (is_new) {
        std::cout << "[IPC] Discovered peer node " << info.node_name << " (id " << node_id << ")" << std::endl << std::flush;
    }
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

void ChannelManager::on_frame_received(const uint8_t* frame, size_t len, const NodeEndpoint* source) {
    stats_.frames_received.fetch_add(1, std::memory_order_relaxed);

    if (len < K32C_HEADER_LEN + K32C_CRC_LEN) return;

    const auto* hdr = reinterpret_cast<const K32CHeader*>(frame);
    if (hdr->magic != K32C_MAGIC || hdr->version != K32C_VERSION) {
        stats_.bad_magic_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (hdr->payload_len > K32C_MAX_PAYLOAD_CHUNK || (K32C_HEADER_LEN + hdr->payload_len + K32C_CRC_LEN) > len) return;

    // CRC Verification
    uint16_t expected_crc = crc16_ccitt(frame, K32C_HEADER_LEN + hdr->payload_len);
    uint16_t packet_crc = 0;
    std::memcpy(&packet_crc, frame + K32C_HEADER_LEN + hdr->payload_len, 2);
    if (expected_crc != packet_crc) {
        stats_.crc_errors.fetch_add(1, std::memory_order_relaxed);
        return; // Drop corrupted frame
    }

    // Peer discovery frames never reach pods
    if (hdr->flags & K32C_FLAG_PING) {
        handle_hello(hdr, frame + K32C_HEADER_LEN, hdr->payload_len, source);
        return;
    }

    // Single-frame message fast path
    if (hdr->frag_count <= 1) {
        static int traced = 0;
        if (traced < 8) { traced++; printf("[TRACE] ipc rx frame chan=%08x len=%u\n", (unsigned)hdr->channel_hash, (unsigned)hdr->payload_len); }
        deliver_local(hdr->channel_hash, hdr->sender_hash, frame + K32C_HEADER_LEN, hdr->payload_len);
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
        uint8_t done[K32C_RING_SLOT_SIZE];
        size_t total = entry.total_bytes;
        std::memcpy(done, entry.buffer, total);
        uint32_t ch = entry.channel_hash;
        uint32_t snd = entry.sender_hash;
        reassembly_table_.erase(reassembly_key);
        stats_.reassembled_msgs.fetch_add(1, std::memory_order_relaxed);
        deliver_local(ch, snd, done, total);
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
