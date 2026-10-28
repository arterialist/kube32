#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <map>
#include <functional>
#include <string_view>

#include "osal/mutex.hpp"
#include "osal/condition_variable.hpp"
#include "osal/socket.hpp"
#include "hal/hal_types.hpp"

namespace kube32::ipc {

// =============================================================================
// Constants & Error Codes
// =============================================================================
constexpr uint32_t K32C_MAGIC               = 0x4332334B; // "K32C" in wire byte order ('K', '3', '2', 'C') on little-endian
constexpr uint8_t  K32C_VERSION             = 0x01;
constexpr size_t   K32C_MAX_PACKET_LEN      = 250;        // ESP-NOW MTU
constexpr size_t   K32C_HEADER_LEN          = 24;
constexpr size_t   K32C_CRC_LEN             = 2;
constexpr size_t   K32C_MAX_PAYLOAD_CHUNK   = K32C_MAX_PACKET_LEN - K32C_HEADER_LEN - K32C_CRC_LEN; // 224 bytes
constexpr size_t   K32C_RING_SLOT_SIZE      = 1024;
constexpr size_t   K32C_RING_SLOT_COUNT     = 16;         // Must be power of 2
constexpr uint32_t K32C_REASSEMBLY_TIMEOUT  = 500;        // ms

// Packet Flags
constexpr uint8_t K32C_FLAG_FRAGMENT        = 0x01;
constexpr uint8_t K32C_FLAG_FIRST_FRAG      = 0x02;
constexpr uint8_t K32C_FLAG_LAST_FRAG       = 0x04;
constexpr uint8_t K32C_FLAG_ACK_REQ         = 0x08;
constexpr uint8_t K32C_FLAG_PING            = 0x10;

// POSIX-aligned Error Codes
constexpr int32_t IPC_OK                    = 0;
constexpr int32_t IPC_ERR_PERM              = -1;   // -EPERM
constexpr int32_t IPC_ERR_NOENT             = -2;   // -ENOENT
constexpr int32_t IPC_ERR_IO                = -5;   // -EIO
constexpr int32_t IPC_ERR_BADF              = -9;   // -EBADF
constexpr int32_t IPC_ERR_AGAIN             = -11;  // -EAGAIN / -EWOULDBLOCK
constexpr int32_t IPC_ERR_NOMEM             = -12;  // -ENOMEM
constexpr int32_t IPC_ERR_FAULT             = -14;  // -EFAULT
constexpr int32_t IPC_ERR_INVAL             = -22;  // -EINVAL
constexpr int32_t IPC_ERR_TIMEDOUT          = -110; // -ETIMEDOUT
constexpr int32_t IPC_ERR_HOSTUNREACH       = -113; // -EHOSTUNREACH

// =============================================================================
// Wire Protocol Struct (Packed 24-byte Header)
// =============================================================================
#pragma pack(push, 1)
struct K32CHeader {
    uint32_t magic;          // 0x4B333243 ("K32C")
    uint8_t  version;        // 0x01
    uint8_t  flags;          // Fragmentation & control flags
    uint16_t seq_num;        // Monotonic sequence number
    uint32_t channel_hash;   // Hash of channel identifier
    uint32_t sender_hash;    // Hash of sender pod name
    uint32_t target_hash;    // Hash of target pod name
    uint8_t  frag_index;     // Current fragment index (0-based)
    uint8_t  frag_count;     // Total fragment count
    uint16_t payload_len;    // Length of payload following header (0..224)
};

struct K32CPacket {
    K32CHeader header;
    uint8_t    payload[K32C_MAX_PAYLOAD_CHUNK];
    uint16_t   crc16;
};
#pragma pack(pop)

static_assert(sizeof(K32CHeader) == 24, "K32CHeader must be exactly 24 bytes");

// Helper Hash & CRC Functions
uint32_t fnv1a_32(const char* data, size_t len);
inline uint32_t fnv1a_32(const std::string& str) { return fnv1a_32(str.data(), str.size()); }
uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t init = 0xFFFF);

// =============================================================================
// Lock-Free SPSC Slotted Ring Buffer
// =============================================================================
struct RingSlot {
    uint32_t sender_hash{0};
    uint16_t len{0};
};

// Slot payloads live in one flat allocation of capacity * slot_size bytes so a
// ring sized for 200-byte events costs 2 KB instead of 16 KB.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = K32C_RING_SLOT_COUNT, size_t slot_size = K32C_RING_SLOT_SIZE);
    ~RingBuffer() = default;
    size_t slot_size() const { return slot_size_; }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Enqueue a message slot (Producer thread)
    bool push(uint32_t sender_hash, const uint8_t* data, size_t len);

    // Dequeue a message slot with timeout (Consumer thread)
    int32_t pop(uint8_t* out_buf, size_t max_len, uint32_t timeout_ms, uint32_t* out_sender_hash = nullptr);

    bool is_empty() const;
    bool is_full() const;
    size_t size() const;
    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    const size_t mask_;
    const size_t slot_size_;
    std::vector<RingSlot> slots_;
    std::vector<uint8_t> payload_;

    alignas(64) std::atomic<uint32_t> head_{0}; // Written by Producer
    alignas(64) std::atomic<uint32_t> tail_{0}; // Written by Consumer

    osal::Mutex producer_mutex_;
    osal::Mutex cv_mutex_;
    osal::ConditionVariable cv_data_ready_;
};

// =============================================================================
// Transport Abstraction Layer
// =============================================================================
struct NodeEndpoint {
    bool is_espnow{false};
    uint8_t mac_address[6]{0};
    std::string ip_address;
    uint16_t udp_port{0};
    uint32_t node_id{0};
};

class IChannelTransport {
public:
    virtual ~IChannelTransport() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual int32_t send_frame(const NodeEndpoint& target, const uint8_t* frame, size_t len) = 0;
};

// =============================================================================
// Virtual Channel Routing Table
// =============================================================================
struct ChannelRoute {
    uint32_t channel_hash{0};
    std::string channel_name;
    std::string target_pod;
    bool is_local{false};
    std::shared_ptr<RingBuffer> local_ring;
    NodeEndpoint remote_endpoint;
};

class VirtualChannelRoutingTable {
public:
    static VirtualChannelRoutingTable& instance();

    void register_local_channel(const std::string& target_pod, const std::string& channel_name, std::shared_ptr<RingBuffer> ring);
    void register_remote_route(const std::string& target_pod, const NodeEndpoint& endpoint);
    void unregister_channel(const std::string& target_pod, const std::string& channel_name);

    bool lookup_route(const std::string& channel_uri, ChannelRoute& out_route) const;
    bool lookup_route_by_hash(uint32_t channel_hash, ChannelRoute& out_route) const;
    void clear();

private:
    VirtualChannelRoutingTable() = default;
    mutable osal::Mutex mutex_;
    std::map<uint32_t, ChannelRoute> routes_by_hash_;
    std::map<std::string, NodeEndpoint> pod_to_remote_node_;
};

// =============================================================================
// Channel Manager
// =============================================================================
struct ChannelHandle {
    int32_t handle_id{0};
    std::string pod_id;
    std::string channel_uri;
    uint32_t channel_hash{0};
    uint32_t flags{0};
    std::shared_ptr<RingBuffer> in_ring; // Allocated for listening/recv
};

// Node peers discovered through HELLO frames (ESP-NOW broadcast) or added
// statically (host simulation).
struct PeerInfo {
    uint32_t node_id{0};
    std::string node_name;
    NodeEndpoint endpoint;
    uint64_t last_seen_ms{0};
    uint32_t hello_count{0};
};

constexpr const char* K32C_HELLO_TOPIC = "k32.hello";

class ChannelManager {
public:
    static ChannelManager& instance();

    bool init(uint32_t node_id = 0, uint16_t base_udp_port = 7400);
    void shutdown();
    bool is_initialized() const;

    // ---- Topic publish/subscribe (host-side consumers) --------------------
    using TopicCallback = std::function<void(std::string_view topic, uint32_t sender_hash, const uint8_t* data, size_t len)>;
    // Delivers to the local ring for the topic (if a pod opened it), to every
    // host subscriber, and to every known peer node. Returns bytes or error.
    int32_t publish(const std::string& pod_id, const std::string& topic, const uint8_t* data, size_t len);
    uint32_t subscribe(const std::string& topic, TopicCallback cb);
    void unsubscribe(uint32_t id);

    // ---- Peers -------------------------------------------------------------
    void set_local_identity(uint32_t node_id, const std::string& node_name);
    void add_peer(const PeerInfo& peer);
    std::vector<PeerInfo> peers() const;
    bool find_peer_by_name(const std::string& node_name, PeerInfo& out) const;
    uint32_t local_node_id() const { return local_node_id_; }
    // Starts the periodic HELLO announcement and the dispatcher thread.
    bool start_background(uint32_t hello_interval_ms = 3000, size_t dispatcher_stack_bytes = 8192, bool stack_in_psram = false);
    void send_hello();
    // Peers reachable over IP that cannot be found by ESP-NOW broadcast (nodes
    // on another access point of the same network): HELLO is sent to them
    // unicast until they answer and become regular peers.
    void add_seed_peer(const std::string& ip, uint16_t port);
    // ESP-NOW receive callback entry: copies the frame for the dispatcher thread.
    void enqueue_raw_frame(const uint8_t* src_mac, const uint8_t* frame, size_t len);

    // Workload API primitives
    int32_t open(const std::string& pod_id, const std::string& channel_uri, uint32_t flags);
    int32_t send(const std::string& pod_id, int32_t handle, const uint8_t* data, size_t len);
    int32_t recv(const std::string& pod_id, int32_t handle, uint8_t* out_buf, size_t max_len, uint32_t timeout_ms);
    int32_t close(const std::string& pod_id, int32_t handle);

    // Frame ingress hook (invoked by UDP or ESP-NOW receiver thread). The
    // source endpoint lets HELLO frames populate the peer table.
    void on_frame_received(const uint8_t* frame, size_t len, const NodeEndpoint* source = nullptr);

    struct ChannelStats {
        std::atomic<uint64_t> frames_received{0};
        std::atomic<uint64_t> frames_delivered{0};
        std::atomic<uint64_t> frames_sent{0};
        std::atomic<uint64_t> send_errors{0};
        std::atomic<uint64_t> crc_errors{0};
        std::atomic<uint64_t> bad_magic_errors{0};
        std::atomic<uint64_t> reassembled_msgs{0};
        std::atomic<uint64_t> hello_rx{0};
    };
    ChannelStats& stats() { return stats_; }

private:
    ChannelManager();
    ~ChannelManager();

    osal::Mutex mutex_;
    int32_t next_handle_{1};
    std::map<int32_t, ChannelHandle> handles_;
    std::unique_ptr<IChannelTransport> transport_;
    std::atomic<uint16_t> seq_generator_{1};
    ChannelStats stats_;
    bool initialized_{false};

    // Multi-fragment reassembly state
    struct ReassemblyEntry {
        uint32_t channel_hash{0};
        uint32_t sender_hash{0};
        uint16_t seq_num{0};
        uint8_t  total_frags{0};
        uint8_t  received_frags{0};
        uint32_t received_mask{0};
        uint32_t total_bytes{0};
        uint64_t last_activity_ms{0};
        uint8_t  buffer[K32C_RING_SLOT_SIZE];
    };
    osal::Mutex reassembly_mutex_;
    std::map<uint64_t, ReassemblyEntry> reassembly_table_;

    // Peers and identity
    mutable osal::Mutex peer_mutex_;
    std::map<uint32_t, PeerInfo> peers_;
    std::vector<NodeEndpoint> seed_peers_;   // guarded by peer_mutex_
    uint32_t local_node_id_{0};
    std::string local_node_name_;

    // Host subscribers and dispatcher
    struct Subscription {
        uint32_t id;
        uint32_t topic_hash;
        std::string topic;
        TopicCallback cb;
    };
    mutable osal::Mutex sub_mutex_;
    std::vector<Subscription> subs_;
    uint32_t next_sub_id_{1};
    std::shared_ptr<RingBuffer> dispatch_ring_;
    std::shared_ptr<RingBuffer> rx_ring_;      // raw ESP-NOW frames awaiting parsing
    std::unique_ptr<osal::Thread> dispatch_thread_;
    std::atomic<bool> background_running_{false};
    uint32_t hello_interval_ms_{3000};
    uint64_t last_hello_ms_{0};

    void deliver_local(uint32_t channel_hash, uint32_t sender_hash, const uint8_t* data, size_t len);
    int32_t send_frames_to(const NodeEndpoint& ep, uint32_t channel_hash, uint32_t sender_hash, uint32_t target_hash, const uint8_t* data, size_t len, uint8_t extra_flags);
    void dispatch_loop();
    void handle_hello(const K32CHeader* hdr, const uint8_t* payload, size_t len, const NodeEndpoint* source);
};

// =============================================================================
// High-Level Channel Object Conforming to PROJECT.md § 4.5
// =============================================================================
class Channel {
public:
    explicit Channel(const std::string& owner_pod_id);
    ~Channel();

    int32_t open(const std::string& channel_uri, uint32_t flags = 0);
    int32_t send(const uint8_t* data, size_t len);
    int32_t recv(uint8_t* out_buf, size_t max_len, uint32_t timeout_ms = 1000);
    void close();

    int32_t handle() const { return handle_; }
    bool is_valid() const { return handle_ > 0; }

private:
    std::string owner_pod_id_;
    int32_t handle_{-1};
};

} // namespace kube32::ipc
