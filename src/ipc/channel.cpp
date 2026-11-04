#include "ipc/channel.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "runtime/pod_memory_pool.h"

#include <cstring>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <new>
#include <cstdlib>
#include <deque>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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

namespace {
// CRC-16/CCITT a byte at a time (a 1444-byte fragment is checked on every
// frame received)
struct Crc16Table {
    uint16_t t[256];
    constexpr Crc16Table() : t() {
        for (int i = 0; i < 256; ++i) {
            uint16_t c = static_cast<uint16_t>(i << 8);
            for (int j = 0; j < 8; ++j) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
            t[i] = c;
        }
    }
};
constexpr Crc16Table kCrc16;
} // namespace

uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t init) {
    uint16_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>((crc << 8) ^ kCrc16.t[((crc >> 8) ^ data[i]) & 0xFF]);
    }
    return crc;
}

// =============================================================================
// Message buffers
// =============================================================================
namespace {
std::atomic<size_t> g_message_bytes{0};

#if defined(KUBE32_TARGET_ESP32)
// Without PSRAM a message comes from the pod pool block's small heap, and
// from the heap only while this much of it stays free: the Wi-Fi driver stops
// sending for good once an allocation of its own fails, and lwIP, the kubelet
// and the received frames waiting for the dispatcher need it too
constexpr size_t kHeapReserveNoPsram = 24 * 1024;
#endif

uint8_t* message_alloc(size_t n) {
    if (n == 0) n = 1;
#if defined(KUBE32_TARGET_ESP32)
    if (osal::System::get_total_psram_bytes() > 0) {
        void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return static_cast<uint8_t*>(p);
    }
    if (void* p = kube32_pool_sub_alloc(n)) return static_cast<uint8_t*>(p);
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= n + kHeapReserveNoPsram &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= n) {
        return static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return nullptr;
#else
    return static_cast<uint8_t*>(std::malloc(n));
#endif
}

void message_free(uint8_t* p) {
    if (!p) return;
#if defined(KUBE32_TARGET_ESP32)
    if (kube32_pool_sub_free(p)) return;
#endif
    std::free(p);
}
} // namespace

ChannelMessage::~ChannelMessage() {
    if (data) {
        message_free(data);
        g_message_bytes.fetch_sub(cap);
    }
}

MessagePtr make_message(size_t capacity) {
    uint8_t* p = message_alloc(capacity);
    if (!p) return nullptr;
    MessagePtr m;
    try {
        m = std::make_shared<ChannelMessage>();
    } catch (const std::bad_alloc&) {
        message_free(p);
        return nullptr;
    }
    m->data = p;
    m->cap = capacity;
    g_message_bytes.fetch_add(capacity);
    return m;
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
    // Host simulations share one machine, so each node takes base_port + id;
    // boards have their own address and all listen on the base port.
    PeerUdpTransport(uint32_t node_id, uint16_t base_port, bool fixed_port = false, size_t rx_stack = 32768)
        : node_id_(node_id),
          port_(fixed_port ? (base_port > 0 ? base_port : 7400) : (base_port > 0 ? (base_port + node_id) : (7400 + node_id))),
          rx_stack_(rx_stack) {}

    ~PeerUdpTransport() override { stop(); }

    bool start() override {
        if (running_.load()) return true;
        if (!sock_.create(osal::SocketType::Udp)) return false;
        sock_.set_reuse_addr(true);
#if !defined(KUBE32_TARGET_ESP32)
        // A simulated worker sends its HELLO to the broadcast address
        // (ChannelManager::add_seed_peer) to be found by a control plane
        // that does not know its address
        sock_.set_broadcast(true);
#endif
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
        cfg.stack_size = rx_stack_;
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
        // Large-message frames are up to 1470 bytes: off the thread's stack
        std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[K32C_MAX_FRAME_LEN]);
        if (!buf) return;
        uint8_t* buffer = buf.get();
        const size_t buffer_len = K32C_MAX_FRAME_LEN;
        std::string sender_ip;
        uint16_t sender_port = 0;

        while (running_.load()) {
            ssize_t n = sock_.recvfrom(buffer, buffer_len, &sender_ip, &sender_port);
            if (n >= static_cast<ssize_t>(K32C_HEADER_LEN + K32C_CRC_LEN)) {
                NodeEndpoint src;
                src.is_espnow = false;
                src.ip_address = sender_ip;
                src.udp_port = sender_port;
                try {
                    ChannelManager::instance().on_frame_received(buffer, static_cast<size_t>(n), &src);
                } catch (const std::exception&) {
                    // out of memory: the frame is dropped like a lost datagram
                }
            }
        }
    }

    [[maybe_unused]] uint32_t node_id_;
    uint16_t port_;
    size_t rx_stack_;
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
        if (data && len >= static_cast<int>(K32C_HEADER_LEN + K32C_CRC_LEN) && len <= static_cast<int>(K32C_MAX_FRAME_LEN)) {
            ChannelManager::instance().enqueue_raw_frame((info && info->src_addr) ? info->src_addr : nullptr, data, static_cast<size_t>(len));
        }
    }
    bool running_{false};
};
SemaphoreHandle_t EspNowTransport::s_send_sem = nullptr;
volatile bool EspNowTransport::s_last_send_ok = false;

// ESP-NOW reaches boards on the same radio channel; UDP reaches boards that
// sit on another access point of the same IP network. A frame goes out on
// whichever transport the peer was discovered through.
class DualTransport : public IChannelTransport {
public:
    DualTransport(uint32_t node_id, uint16_t udp_port) : udp_(node_id, udp_port, true, 4096) {}
    bool start() override {
        bool a = espnow_.start();
        bool b = udp_.start();
        return a || b;
    }
    void stop() override { espnow_.stop(); udp_.stop(); }
    int32_t send_frame(const NodeEndpoint& target, const uint8_t* frame, size_t len) override {
        return target.is_espnow ? espnow_.send_frame(target, frame, len) : udp_.send_frame(target, frame, len);
    }
private:
    EspNowTransport espnow_;
    PeerUdpTransport udp_;
};
#endif

// =============================================================================
// ChannelManager Implementation
// =============================================================================
ChannelManager& ChannelManager::instance() {
    static ChannelManager s_mgr;
    return s_mgr;
}

// ~ChannelManager clears the routing table, so the table must outlive the
// manager. Function-local statics are destroyed in the reverse order of their
// construction; constructing the table here, before the manager's own
// construction completes, makes it the later one to be destroyed.
ChannelManager::ChannelManager() { (void)VirtualChannelRoutingTable::instance(); }
ChannelManager::~ChannelManager() { shutdown(); }

bool ChannelManager::init(uint32_t node_id, uint16_t base_udp_port) {
    osal::LockGuard guard(mutex_);
    if (initialized_) return true;

#if defined(KUBE32_TARGET_ESP32)
    transport_ = std::make_unique<DualTransport>(node_id, base_udp_port);
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
    {
        osal::LockGuard d_guard(disp_mutex_);
        for (size_t i = 0; i < rx_count_; ++i) {
            RawFrame& f = rx_frames_[(rx_head_ + i) % rx_cap_];
            std::free(f.data);
            f = RawFrame{};
        }
        rx_head_ = rx_count_ = 0;
        msg_queue_.clear();
    }
    initialized_ = false;
}

bool ChannelManager::is_initialized() const {
    return initialized_;
}

int32_t ChannelManager::open(const std::string& pod_id, const std::string& channel_uri, uint32_t flags) {
    osal::LockGuard guard(mutex_);
    return open_locked(pod_id, channel_uri, flags, next_handle_++);
}

int32_t ChannelManager::open_as(const std::string& pod_id, const std::string& channel_uri, uint32_t flags, int32_t handle) {
    osal::LockGuard guard(mutex_);
    if (handle <= 0 || handles_.count({pod_id, handle})) return IPC_ERR_INVAL;
    if (handle >= next_handle_) next_handle_ = handle + 1;
    return open_locked(pod_id, channel_uri, flags, handle);
}

std::vector<std::pair<int32_t, std::string>> ChannelManager::handles_of(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    std::vector<std::pair<int32_t, std::string>> out;
    for (auto it = handles_.lower_bound({pod_id, 0}); it != handles_.end() && it->first.first == pod_id; ++it) {
        out.emplace_back(it->first.second, it->second.channel_uri);
    }
    return out;
}

void ChannelManager::close_all(const std::string& pod_id) {
    for (const auto& h : handles_of(pod_id)) close(pod_id, h.first);
}

int32_t ChannelManager::open_locked(const std::string& pod_id, const std::string& channel_uri, uint32_t flags, int32_t hid) {
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

    handles_[{pod_id, hid}] = ch;
    return hid;
}

int32_t ChannelManager::send(const std::string& pod_id, int32_t handle, const uint8_t* data, size_t len) {
    ChannelRoute route;
    std::string channel_uri;
    uint32_t channel_hash = 0;
    {
        osal::LockGuard guard(mutex_);
        auto it = handles_.find({pod_id, handle});
        if (it == handles_.end()) {
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
    if (len > K32C_MAX_MESSAGE_BYTES) return IPC_ERR_INVAL;
    uint32_t target_hash = fnv1a_32(route.target_pod);
    const int32_t rc = send_frames_to(route.remote_endpoint, channel_hash, sender_hash, target_hash, data, len, 0);
    return rc == IPC_OK ? static_cast<int32_t>(len) : rc;
}

int32_t ChannelManager::send_frames_to(const NodeEndpoint& ep, uint32_t channel_hash, uint32_t sender_hash, uint32_t target_hash,
                                       const uint8_t* data, size_t len, uint8_t extra_flags) {
    if (!transport_) return IPC_ERR_IO;
    if (len > K32C_MAX_MESSAGE_BYTES) return IPC_ERR_INVAL;
    // A message of up to 224 bytes is one frame every node reads; a larger
    // one goes out in frames of up to 1444 bytes of it
    const bool large = len > K32C_MAX_PAYLOAD_CHUNK;
    const size_t chunk = large ? K32C_LARGE_CHUNK : K32C_MAX_PAYLOAD_CHUNK;
    size_t frag_count = (len + chunk - 1) / chunk;
    if (frag_count == 0) frag_count = 1;

    osal::LockGuard guard(send_mutex_);
    if (!tx_frame_) tx_frame_.reset(new (std::nothrow) uint8_t[K32C_MAX_FRAME_LEN]);
    if (!tx_frame_) return IPC_ERR_NOMEM;
    uint8_t* const frame = tx_frame_.get();
    const uint16_t seq = seq_generator_.fetch_add(1);
    const uint32_t gap_us = fragment_gap_us_.load();

    for (size_t i = 0; i < frag_count; ++i) {
        const size_t offset = i * chunk;
        const size_t chunk_len = std::min(chunk, len - offset);

        K32CHeader hdr = {};
        hdr.magic = K32C_MAGIC;
        hdr.version = K32C_VERSION;
        hdr.flags = extra_flags;
        if (large) hdr.flags |= K32C_FLAG_LARGE;
        if (frag_count > 1) hdr.flags |= K32C_FLAG_FRAGMENT;
        if (i == 0) hdr.flags |= K32C_FLAG_FIRST_FRAG;
        if (i == frag_count - 1) hdr.flags |= K32C_FLAG_LAST_FRAG;
        hdr.seq_num = seq;
        hdr.channel_hash = channel_hash;
        hdr.sender_hash = sender_hash;
        hdr.target_hash = target_hash;
        hdr.frag_index = static_cast<uint8_t>(i);
        hdr.frag_count = static_cast<uint8_t>(frag_count);
        hdr.payload_len = static_cast<uint16_t>(chunk_len);
        std::memcpy(frame, &hdr, K32C_HEADER_LEN);
        if (chunk_len > 0) std::memcpy(frame + K32C_HEADER_LEN, data + offset, chunk_len);
        const size_t frame_bytes = K32C_HEADER_LEN + chunk_len;
        const uint16_t crc = crc16_ccitt(frame, frame_bytes);
        std::memcpy(frame + frame_bytes, &crc, 2);

        if (i > 0 && gap_us > 0) {
            if (gap_us >= 1000) osal::Thread::sleep_ms((gap_us + 500) / 1000);
            else {
                const uint64_t until = osal::Thread::get_time_us() + gap_us;
                while (osal::Thread::get_time_us() < until) {
                }
            }
        }
        int32_t res = transport_->send_frame(ep, frame, frame_bytes + 2);
        if (res != IPC_OK) {
            // One more try: a lost frame loses the whole message
            stats_.frames_retried.fetch_add(1, std::memory_order_relaxed);
            res = transport_->send_frame(ep, frame, frame_bytes + 2);
        }
        if (res == IPC_OK) stats_.frames_sent.fetch_add(1, std::memory_order_relaxed);
        else { stats_.send_errors.fetch_add(1, std::memory_order_relaxed); return res; }
    }
    return IPC_OK;
}

// =============================================================================
// Topic publish/subscribe and peers
// =============================================================================
namespace {
uint32_t subs_topic_hash() {
    static const uint32_t h = fnv1a_32(K32C_SUBS_TOPIC, std::strlen(K32C_SUBS_TOPIC));
    return h;
}
bool wants_locked(const PeerInfo& p, uint32_t topic_hash, uint64_t now_ms) {
    if (p.subs_ms == 0 || now_ms - p.subs_ms > K32C_SUBS_FRESH_MS) return true;   // announces none: every topic
    return std::binary_search(p.subs.begin(), p.subs.end(), topic_hash);
}
} // namespace

NodeEndpoint ChannelManager::route_to(const PeerInfo& p, uint64_t now_ms) const {
    const Route r = route();
    const bool has_mac = p.espnow_seen_ms != 0;
    const bool has_ip = !p.ip_address.empty();
    bool espnow;
    if (r == Route::Udp) espnow = !has_ip;
    else if (r == Route::EspNow) espnow = has_mac;
    else espnow = (has_mac && now_ms - p.espnow_seen_ms < K32C_ESPNOW_FRESH_MS) || !has_ip;
    NodeEndpoint ep;
    ep.node_id = p.node_id;
    if (espnow && has_mac) {
        ep.is_espnow = true;
        std::memcpy(ep.mac_address, p.mac_address, 6);
        return ep;
    }
    if (has_ip) {
        ep.ip_address = p.ip_address;
        ep.udp_port = p.udp_port ? p.udp_port : 7400;
        return ep;
    }
    return p.endpoint;   // a peer added by hand (simulation)
}

bool ChannelManager::peer_wants(uint32_t node_id, uint32_t topic_hash) const {
    osal::LockGuard guard(peer_mutex_);
    auto it = peers_.find(node_id);
    return it == peers_.end() || wants_locked(it->second, topic_hash, osal::System::get_uptime_ms());
}

int32_t ChannelManager::publish(const std::string& pod_id, const std::string& topic, const uint8_t* data, size_t len) {
    if (!data && len > 0) return IPC_ERR_FAULT;
    if (len > K32C_MAX_MESSAGE_BYTES) return IPC_ERR_INVAL;
    const uint32_t channel_hash = fnv1a_32(topic);
    const uint32_t sender_hash = fnv1a_32(pod_id);
    stats_.msgs_published.fetch_add(1, std::memory_order_relaxed);
    int32_t err = IPC_OK;

    // Local pods and subscribers
    bool nomem = false;
    if (!deliver_local(channel_hash, sender_hash, data, len, &nomem)) err = nomem ? IPC_ERR_NOMEM : IPC_ERR_AGAIN;

    // Peers that subscribe to the topic
    std::vector<NodeEndpoint> targets;
    {
        const uint64_t now = osal::System::get_uptime_ms();
        osal::LockGuard guard(peer_mutex_);
        for (const auto& kv : peers_) {
            if (kv.first == local_node_id_) continue;
            if (!wants_locked(kv.second, channel_hash, now)) {
                stats_.fanout_skipped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            targets.push_back(route_to(kv.second, now));
        }
    }
    for (const auto& ep : targets) {
        const int32_t rc = send_frames_to(ep, channel_hash, sender_hash, 0, data, len, 0);
        if (rc != IPC_OK && err == IPC_OK) err = rc;
    }
    if (err != IPC_OK) {
        stats_.publish_errors.fetch_add(1, std::memory_order_relaxed);
        return err;
    }
    return static_cast<int32_t>(len);
}

size_t ChannelManager::rx_held_bytes() const { return g_message_bytes.load(); }

bool ChannelManager::deliver_local(uint32_t channel_hash, uint32_t sender_hash, const uint8_t* data, size_t len, bool* nomem) {
    bool ok = true;
    ChannelRoute route;
    if (VirtualChannelRoutingTable::instance().lookup_route_by_hash(channel_hash, route) && route.is_local && route.local_ring) {
        if (route.local_ring->push(sender_hash, data, len)) stats_.frames_delivered.fetch_add(1, std::memory_order_relaxed);
        else ok = false;
    }
    bool has_sub = false;
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& sub : subs_) {
            if (sub.topic_hash == channel_hash) { has_sub = true; break; }
        }
    }
    if (!has_sub || !background_running_.load()) return ok;
    MessagePtr msg;
    if (g_message_bytes.load() + len <= rx_budget_bytes_.load()) msg = make_message(len);
    if (!msg) {
        stats_.rx_nomem.fetch_add(1, std::memory_order_relaxed);
        if (nomem) *nomem = true;
        return false;
    }
    if (len > 0) std::memcpy(msg->data, data, len);
    msg->len = len;
    msg->channel_hash = channel_hash;
    msg->sender_hash = sender_hash;
    return deliver_local(msg) && ok;
}

bool ChannelManager::deliver_local(const MessagePtr& msg) {
    {
        osal::LockGuard guard(disp_mutex_);
        if (msg_queue_.size() >= msg_cap_) {
            stats_.dispatch_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        msg_queue_.push_back(msg);
        disp_ready_ = true;
    }
    stats_.rx_messages.fetch_add(1, std::memory_order_relaxed);
    disp_cv_.notify_one();
    return true;
}

uint32_t ChannelManager::subscribe(const std::string& topic, TopicCallback cb) {
    uint32_t id;
    {
        osal::LockGuard guard(sub_mutex_);
        Subscription s;
        s.id = id = next_sub_id_++;
        s.topic_hash = fnv1a_32(topic);
        s.topic = topic;
        s.cb = std::move(cb);
        subs_.push_back(std::move(s));
    }
    subs_changed_.store(true);
    disp_cv_.notify_one();
    return id;
}

uint32_t ChannelManager::subscribe_messages(const std::string& topic, MessageCallback cb) {
    uint32_t id;
    {
        osal::LockGuard guard(sub_mutex_);
        Subscription s;
        s.id = id = next_sub_id_++;
        s.topic_hash = fnv1a_32(topic);
        s.topic = topic;
        s.msg_cb = std::move(cb);
        subs_.push_back(std::move(s));
    }
    subs_changed_.store(true);
    disp_cv_.notify_one();
    return id;
}

void ChannelManager::unsubscribe(uint32_t id) {
    {
        osal::LockGuard guard(sub_mutex_);
        for (auto it = subs_.begin(); it != subs_.end(); ++it) {
            if (it->id == id) { subs_.erase(it); break; }
        }
    }
    subs_changed_.store(true);
    disp_cv_.notify_one();
}

std::vector<uint32_t> ChannelManager::local_topic_hashes() const {
    std::vector<uint32_t> out = VirtualChannelRoutingTable::instance().local_hashes();
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& s : subs_) out.push_back(s.topic_hash);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void ChannelManager::set_rx_queue_frames(size_t n) {
    if (n == 0) n = 1;
    if (n > 256) n = 256;
    osal::LockGuard guard(disp_mutex_);
    std::vector<RawFrame> next(n);
    size_t kept = 0;
    for (size_t i = 0; i < rx_count_; ++i) {
        RawFrame& f = rx_frames_[(rx_head_ + i) % rx_cap_];
        if (kept < n) next[kept++] = f;
        else std::free(f.data);
    }
    rx_frames_.swap(next);
    rx_cap_ = n;
    rx_head_ = 0;
    rx_count_ = kept;
}

size_t ChannelManager::rx_queue_frames() const {
    return rx_cap_;
}

// Runs on the Wi-Fi task (ESP-NOW receive callback): a copy of the frame
// waits for the dispatcher
void ChannelManager::enqueue_raw_frame(const uint8_t* src_mac, const uint8_t* frame, size_t len) {
    if (!frame || len > K32C_MAX_FRAME_LEN) return;
#if defined(KUBE32_TARGET_ESP32)
    // Without PSRAM a frame of a large message waits in the heap only above
    // its reserve; a small frame (a HELLO, an event) above 6 KiB
    if (!has_psram_ && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < len + (len > K32C_MAX_PACKET_LEN ? kHeapReserveNoPsram : 6 * 1024)) {
        stats_.rx_nomem.fetch_add(1, std::memory_order_relaxed);
        return;
    }
#endif
    auto* p = static_cast<uint8_t*>(std::malloc(len + 6));
    if (!p) {
        stats_.rx_nomem.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (src_mac) std::memcpy(p, src_mac, 6); else std::memset(p, 0, 6);
    std::memcpy(p + 6, frame, len);
    {
        osal::LockGuard guard(disp_mutex_);
        if (rx_count_ >= rx_cap_ || rx_frames_.size() != rx_cap_) {
            std::free(p);
            stats_.rx_queue_full.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        rx_frames_[(rx_head_ + rx_count_) % rx_cap_] = RawFrame{p, static_cast<uint16_t>(len + 6)};
        rx_count_++;
        disp_ready_ = true;
    }
    disp_cv_.notify_one();
}

void ChannelManager::dispatch_message(const MessagePtr& msg) {
    std::vector<Subscription> matched;
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& sub : subs_) if (sub.topic_hash == msg->channel_hash) matched.push_back(sub);
    }
    for (const auto& sub : matched) {
        if (sub.msg_cb) sub.msg_cb(sub.topic, msg);
        else if (sub.cb) sub.cb(sub.topic, msg->sender_hash, msg->data, msg->len);
    }
}

void ChannelManager::prune_reassembly(uint64_t now_ms) {
    osal::LockGuard guard(reassembly_mutex_);
    for (auto it = reassembly_table_.begin(); it != reassembly_table_.end();) {
        if (now_ms >= it->last_activity_ms && now_ms - it->last_activity_ms > K32C_REASSEMBLY_TIMEOUT) {
            if (it->msg) stats_.rx_incomplete.fetch_add(1, std::memory_order_relaxed);
            it = reassembly_table_.erase(it);
        } else {
            ++it;
        }
    }
}

void ChannelManager::dispatch_loop() {
    while (background_running_.load()) {
        // Out of memory a frame or a hello is dropped; the dispatcher (and
        // with it every channel subscription on this node) keeps running
        try {
            uint64_t now = osal::System::get_uptime_ms();
            if (hello_interval_ms_ > 0 && now - last_hello_ms_ >= hello_interval_ms_) {
                last_hello_ms_ = now;
                send_hello();
                send_subs();
                subs_changed_.store(false);
                prune_reassembly(now);
            } else if (subs_changed_.exchange(false)) {
                send_subs();
            }
            // Frames from the ESP-NOW callback, then the messages they and
            // the UDP receiver completed
            for (;;) {
                RawFrame f;
                {
                    osal::LockGuard guard(disp_mutex_);
                    if (rx_count_ == 0) break;
                    f = rx_frames_[rx_head_];
                    rx_frames_[rx_head_] = RawFrame{};
                    rx_head_ = (rx_head_ + 1) % rx_cap_;
                    rx_count_--;
                }
                std::unique_ptr<uint8_t, void (*)(void*)> hold(f.data, std::free);
                if (f.len > 6) {
                    NodeEndpoint src;
                    src.is_espnow = true;
                    std::memcpy(src.mac_address, f.data, 6);
                    on_frame_received(f.data + 6, f.len - 6u, &src);
                }
            }
            MessagePtr msg;
            {
                osal::UniqueLock<osal::Mutex> lock(disp_mutex_);
                if (!msg_queue_.empty()) {
                    msg = std::move(msg_queue_.front());
                    msg_queue_.pop_front();
                } else if (rx_count_ == 0 && !subs_changed_.load()) {
                    disp_ready_ = false;
                    const uint64_t since = osal::System::get_uptime_ms() - last_hello_ms_;
                    const uint32_t wait = hello_interval_ms_ == 0 ? 100 : static_cast<uint32_t>(since >= hello_interval_ms_ ? 1 : std::min<uint64_t>(100, hello_interval_ms_ - since));
                    disp_cv_.wait_for(lock, wait, [this] { return disp_ready_ || subs_changed_.load() || !background_running_.load(); });
                }
            }
            if (msg) dispatch_message(msg);
            osal::System::feed_watchdog();
        } catch (const std::bad_alloc&) {
            osal::System::feed_watchdog();
        }
    }
}

bool ChannelManager::start_background(uint32_t hello_interval_ms, size_t dispatcher_stack_bytes, bool stack_in_psram) {
    if (background_running_.load()) return true;
    hello_interval_ms_ = hello_interval_ms;
    // Without PSRAM: 16 received frames, 8 messages for subscribers, two
    // reassemblies and 19.5 KiB of messages at once; with PSRAM 32, 32, 8 and
    // 256 KiB (two pod workers publishing at once, large messages arriving)
    const bool psram = osal::System::get_total_psram_bytes() > 0;
    {
        osal::LockGuard guard(disp_mutex_);
        msg_cap_ = psram ? 32 : 8;
    }
    if (rx_frames_.empty()) set_rx_queue_frames(psram ? 32 : 16);
    {
        osal::LockGuard guard(reassembly_mutex_);
        max_reassembly_ = psram ? 8 : 2;
    }
    // Without PSRAM: one message of the largest size held at once
    rx_budget_bytes_.store(psram ? 256 * 1024 : K32C_MAX_MESSAGE_BYTES + K32C_LARGE_CHUNK + 2048);
    has_psram_ = psram;
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
    const PeerInfo before = slot;
    slot = peer;
    slot.hello_count = before.hello_count;
    if (slot.subs_ms == 0) {
        slot.subs = before.subs;
        slot.subs_ms = before.subs_ms;
    }
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

// The node's subscriptions go to every node a HELLO goes to
void ChannelManager::send_subs() {
    if (!transport_ || !initialized_) return;
    const std::vector<uint32_t> hashes = local_topic_hashes();
    const size_t n = std::min(hashes.size(), (K32C_MAX_MESSAGE_BYTES - 4) / 4);
    std::vector<uint8_t> payload(4 + 4 * n);
    std::memcpy(payload.data(), &local_node_id_, 4);
    for (size_t i = 0; i < n; ++i) std::memcpy(payload.data() + 4 + 4 * i, &hashes[i], 4);
    const uint32_t hash = subs_topic_hash();
    const uint32_t sender = fnv1a_32(local_node_name_);
#if defined(KUBE32_TARGET_ESP32)
    NodeEndpoint bcast;
    bcast.is_espnow = true;
    std::memset(bcast.mac_address, 0xff, 6);
    send_frames_to(bcast, hash, sender, 0, payload.data(), payload.size(), 0);
#endif
    std::vector<NodeEndpoint> targets;
    {
        const uint64_t now = osal::System::get_uptime_ms();
        osal::LockGuard guard(peer_mutex_);
        for (const auto& kv : peers_) {
            if (kv.first == local_node_id_) continue;
            targets.push_back(route_to(kv.second, now));
        }
        for (const auto& ep : seed_peers_) targets.push_back(ep);
    }
    for (const auto& ep : targets) send_frames_to(ep, hash, sender, 0, payload.data(), payload.size(), 0);
}

void ChannelManager::handle_subs(const uint8_t* payload, size_t len) {
    if (len < 4) return;
    uint32_t node_id = 0;
    std::memcpy(&node_id, payload, 4);
    if (node_id == local_node_id_) return;
    std::vector<uint32_t> hashes((len - 4) / 4);
    for (size_t i = 0; i < hashes.size(); ++i) std::memcpy(&hashes[i], payload + 4 + 4 * i, 4);
    std::sort(hashes.begin(), hashes.end());
    stats_.subs_rx.fetch_add(1, std::memory_order_relaxed);
    osal::LockGuard guard(peer_mutex_);
    auto it = peers_.find(node_id);
    if (it == peers_.end()) return;   // its HELLO comes first
    it->second.subs.swap(hashes);
    it->second.subs_ms = osal::System::get_uptime_ms();
}

void ChannelManager::send_hello() {
    if (!transport_ || !initialized_) return;
    uint8_t payload[64];
    std::memcpy(payload, &local_node_id_, 4);
    size_t name_len = std::min<size_t>(local_node_name_.size(), sizeof(payload) - 4);
    std::memcpy(payload + 4, local_node_name_.data(), name_len);
    {
        osal::LockGuard guard(peer_mutex_);
        if (!announced_api_.empty() && 4 + name_len + 5 + announced_api_.size() <= sizeof(payload)) {
            payload[4 + name_len] = 0;
            std::memcpy(payload + 5 + name_len, "api=", 4);
            std::memcpy(payload + 9 + name_len, announced_api_.data(), announced_api_.size());
            name_len += 5 + announced_api_.size();   // sent as one block after the id
        }
    }
    uint32_t hash = fnv1a_32(K32C_HELLO_TOPIC, std::strlen(K32C_HELLO_TOPIC));
    uint32_t sender = fnv1a_32(local_node_name_);
#if defined(KUBE32_TARGET_ESP32)
    NodeEndpoint bcast;
    bcast.is_espnow = true;
    std::memset(bcast.mac_address, 0xff, 6);
    send_frames_to(bcast, hash, sender, 0, payload, 4 + name_len, K32C_FLAG_PING);
#endif
    // A HELLO goes over UDP to every node whose address is known: a worker
    // takes the control plane's HELLOs over IP as proof that its link works
    std::vector<NodeEndpoint> targets;
    {
        osal::LockGuard guard(peer_mutex_);
        for (const auto& kv : peers_) {
            if (kv.first == local_node_id_) continue;
            if (!kv.second.ip_address.empty()) {
                NodeEndpoint ep;
                ep.node_id = kv.first;
                ep.ip_address = kv.second.ip_address;
                ep.udp_port = kv.second.udp_port ? kv.second.udp_port : 7400;
                targets.push_back(ep);
            } else {
                targets.push_back(kv.second.endpoint);
            }
        }
        for (const auto& ep : seed_peers_) targets.push_back(ep);
    }
    for (const auto& ep : targets) send_frames_to(ep, hash, sender, 0, payload, 4 + name_len, K32C_FLAG_PING);
}

void ChannelManager::set_announced_api(const std::string& address) {
    osal::LockGuard guard(peer_mutex_);
    announced_api_ = address;
}

std::string ChannelManager::announced_api() const {
    osal::LockGuard guard(peer_mutex_);
    const PeerInfo* latest = nullptr;
    for (const auto& kv : peers_) {
        if (kv.second.api_address.empty()) continue;
        if (!latest || kv.second.last_seen_ms > latest->last_seen_ms) latest = &kv.second;
    }
    return latest ? latest->api_address : std::string();
}

uint64_t ChannelManager::last_ip_hello_ms(const std::string& ip) const {
    osal::LockGuard guard(peer_mutex_);
    auto it = ip_hello_ms_.find(ip);
    return it == ip_hello_ms_.end() ? 0 : it->second;
}

void ChannelManager::add_seed_peer(const std::string& ip, uint16_t port) {
    if (ip.empty()) return;
    osal::LockGuard guard(peer_mutex_);
    for (const auto& ep : seed_peers_) {
        if (ep.ip_address == ip && ep.udp_port == port) return;
    }
    NodeEndpoint ep;
    ep.is_espnow = false;
    ep.ip_address = ip;
    ep.udp_port = port ? port : 7400;
    seed_peers_.push_back(ep);
    std::cout << "[IPC] seed peer " << ip << ":" << ep.udp_port << std::endl;
}

void ChannelManager::handle_hello(const K32CHeader* hdr, const uint8_t* payload, size_t len, const NodeEndpoint* source) {
    (void)hdr;
    if (len < 4) return;
    uint32_t node_id = 0;
    std::memcpy(&node_id, payload, 4);
    if (node_id == local_node_id_) return;
    stats_.hello_rx.fetch_add(1, std::memory_order_relaxed);
    const char* text = reinterpret_cast<const char*>(payload + 4);
    const size_t text_len = len - 4;
    const void* nul = std::memchr(text, 0, text_len);
    const size_t name_len = nul ? static_cast<size_t>(static_cast<const char*>(nul) - text) : text_len;
    std::string api_address;
    if (nul && text_len - name_len > 5 && std::memcmp(text + name_len + 1, "api=", 4) == 0) {
        api_address.assign(text + name_len + 5, text_len - name_len - 5);
    }
    const uint64_t now = osal::System::get_uptime_ms();
    osal::LockGuard guard(peer_mutex_);
    if (source && !source->is_espnow && !source->ip_address.empty() && (ip_hello_ms_.size() < 16 || ip_hello_ms_.count(source->ip_address))) {
        ip_hello_ms_[source->ip_address] = now;
    }
    const bool is_new = peers_.find(node_id) == peers_.end();
    PeerInfo& slot = peers_[node_id];
    slot.node_id = node_id;
    slot.node_name.assign(text, name_len);
    slot.api_address = api_address;
    if (source) {
        slot.endpoint = *source;
        if (source->is_espnow) {
            slot.espnow_seen_ms = now;
            std::memcpy(slot.mac_address, source->mac_address, 6);
        } else if (!source->ip_address.empty()) {
            slot.udp_seen_ms = now;
            slot.ip_address = source->ip_address;
            slot.udp_port = source->udp_port ? source->udp_port : 7400;
        }
    }
    slot.endpoint.node_id = node_id;
    if (slot.endpoint.udp_port == 0) slot.endpoint.udp_port = 7400;
    slot.hello_count++;
    slot.last_seen_ms = now;
    if (is_new) {
        std::cout << "[IPC] Discovered peer node " << slot.node_name << " (id " << node_id << ")" << std::endl << std::flush;
    }
}

int32_t ChannelManager::recv(const std::string& pod_id, int32_t handle, uint8_t* out_buf, size_t max_len, uint32_t timeout_ms) {
    std::shared_ptr<RingBuffer> ring;
    {
        osal::LockGuard guard(mutex_);
        auto it = handles_.find({pod_id, handle});
        if (it == handles_.end()) {
            return IPC_ERR_BADF;
        }
        ring = it->second.in_ring;
    }
    if (!ring) return IPC_ERR_BADF;
    return ring->pop(out_buf, max_len, timeout_ms);
}

int32_t ChannelManager::close(const std::string& pod_id, int32_t handle) {
    osal::LockGuard guard(mutex_);
    auto it = handles_.find({pod_id, handle});
    if (it == handles_.end()) {
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
    const size_t max_payload = (hdr->flags & K32C_FLAG_LARGE) ? K32C_LARGE_CHUNK : K32C_MAX_PAYLOAD_CHUNK;
    if (hdr->payload_len > max_payload || (K32C_HEADER_LEN + hdr->payload_len + K32C_CRC_LEN) > len) return;

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

    // A message in one frame
    if (hdr->frag_count <= 1) {
        if (hdr->channel_hash == subs_topic_hash()) {
            handle_subs(frame + K32C_HEADER_LEN, hdr->payload_len);
            return;
        }
        deliver_local(hdr->channel_hash, hdr->sender_hash, frame + K32C_HEADER_LEN, hdr->payload_len);
        return;
    }
    on_fragment(hdr, frame + K32C_HEADER_LEN);
}

// A fragment lands in place in its message's buffer; the message is
// delivered when its last missing fragment arrives, in whatever order they
// come, and dropped (counted) when one never does
void ChannelManager::on_fragment(const K32CHeader* hdr, const uint8_t* payload) {
    const size_t chunk = (hdr->flags & K32C_FLAG_LARGE) ? K32C_LARGE_CHUNK : K32C_MAX_PAYLOAD_CHUNK;
    const size_t count = hdr->frag_count;
    if (count > 32 || hdr->frag_index >= count || count * chunk >= K32C_MAX_MESSAGE_BYTES + chunk ||
        (hdr->frag_index + 1u < count && hdr->payload_len != chunk)) {
        stats_.rx_malformed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Only a message this node takes is held
    bool wanted = false;
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& s : subs_) {
            if (s.topic_hash == hdr->channel_hash) { wanted = true; break; }
        }
    }
    if (!wanted) {
        ChannelRoute route;
        wanted = VirtualChannelRoutingTable::instance().lookup_route_by_hash(hdr->channel_hash, route) && route.is_local && route.local_ring;
    }
    if (!wanted) return;

    const uint64_t now_ms = osal::System::get_uptime_ms();
    MessagePtr done;
    {
        osal::LockGuard guard(reassembly_mutex_);
        for (auto it = reassembly_table_.begin(); it != reassembly_table_.end();) {
            if (now_ms >= it->last_activity_ms && now_ms - it->last_activity_ms > K32C_REASSEMBLY_TIMEOUT) {
                if (it->msg) stats_.rx_incomplete.fetch_add(1, std::memory_order_relaxed);
                it = reassembly_table_.erase(it);
            } else {
                ++it;
            }
        }
        ReassemblyEntry* entry = nullptr;
        for (auto& e : reassembly_table_) {
            if (e.channel_hash == hdr->channel_hash && e.sender_hash == hdr->sender_hash && e.seq_num == hdr->seq_num) { entry = &e; break; }
        }
        if (!entry) {
            if (reassembly_table_.size() >= max_reassembly_) {
                // The slots are taken: the least recently active message is dropped
                auto oldest = reassembly_table_.begin();
                for (auto it = reassembly_table_.begin(); it != reassembly_table_.end(); ++it) {
                    if (it->last_activity_ms < oldest->last_activity_ms) oldest = it;
                }
                if (oldest->msg) stats_.rx_incomplete.fetch_add(1, std::memory_order_relaxed);
                reassembly_table_.erase(oldest);
            }
            ReassemblyEntry e;
            e.channel_hash = hdr->channel_hash;
            e.sender_hash = hdr->sender_hash;
            e.seq_num = hdr->seq_num;
            e.total_frags = static_cast<uint8_t>(count);
            e.chunk = static_cast<uint32_t>(chunk);
            const size_t cap = count * chunk;
            if (g_message_bytes.load() + cap <= rx_budget_bytes_.load()) e.msg = make_message(cap);
            // Without memory the message is dropped once; its other fragments
            // are swallowed by the empty entry
            if (!e.msg) stats_.rx_nomem.fetch_add(1, std::memory_order_relaxed);
            reassembly_table_.push_back(std::move(e));
            entry = &reassembly_table_.back();
        }
        entry->last_activity_ms = now_ms;
        if (!entry->msg) return;
        if (entry->total_frags != count || entry->chunk != chunk) {
            stats_.rx_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint32_t bit = 1u << hdr->frag_index;
        if (entry->received_mask & bit) return;   // a duplicate
        entry->received_mask |= bit;
        entry->received_frags++;
        std::memcpy(entry->msg->data + static_cast<size_t>(hdr->frag_index) * chunk, payload, hdr->payload_len);
        if (hdr->frag_index + 1u == count) entry->last_len = hdr->payload_len;
        if (entry->received_frags < entry->total_frags) return;
        done = std::move(entry->msg);
        done->len = (count - 1) * chunk + entry->last_len;
        done->channel_hash = entry->channel_hash;
        done->sender_hash = entry->sender_hash;
        reassembly_table_.erase(reassembly_table_.begin() + (entry - reassembly_table_.data()));
    }
    stats_.reassembled_msgs.fetch_add(1, std::memory_order_relaxed);
    ChannelRoute route;
    if (VirtualChannelRoutingTable::instance().lookup_route_by_hash(done->channel_hash, route) && route.is_local && route.local_ring) {
        if (route.local_ring->push(done->sender_hash, done->data, done->len)) stats_.frames_delivered.fetch_add(1, std::memory_order_relaxed);
    }
    bool has_sub = false;
    {
        osal::LockGuard guard(sub_mutex_);
        for (const auto& s : subs_) {
            if (s.topic_hash == done->channel_hash) { has_sub = true; break; }
        }
    }
    if (has_sub && background_running_.load()) deliver_local(done);
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

std::vector<uint32_t> VirtualChannelRoutingTable::local_hashes() const {
    osal::LockGuard guard(mutex_);
    std::vector<uint32_t> out;
    for (const auto& kv : routes_by_hash_) if (kv.second.is_local) out.push_back(kv.first);
    return out;
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
