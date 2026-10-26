#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <map>

#include "osal/mutex.hpp"
#include "osal/condition_variable.hpp"
#include "osal/socket.hpp"
#include "hal/hal_types.hpp"

namespace kube32::ipc {

// =============================================================================
// Constants & Error Codes
// =============================================================================
constexpr uint32_t K32C_MAGIC               = 0x4B333243; // "K32C"
constexpr uint8_t  K32C_VERSION             = 0x01;
constexpr size_t   K32C_MAX_PACKET_LEN      = 250;        // ESP-NOW MTU
constexpr size_t   K32C_HEADER_LEN          = 24;
constexpr size_t   K32C_CRC_LEN             = 2;
constexpr size_t   K32C_MAX_PAYLOAD_CHUNK   = K32C_MAX_PACKET_LEN - K32C_HEADER_LEN - K32C_CRC_LEN; // 224 bytes
constexpr size_t   K32C_RING_SLOT_SIZE      = 256;
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
    uint8_t  data[K32C_RING_SLOT_SIZE];
};

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = K32C_RING_SLOT_COUNT);
    ~RingBuffer() = default;

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
    std::vector<RingSlot> slots_;

    alignas(64) std::atomic<uint32_t> head_{0}; // Written by Producer
    alignas(64) std::atomic<uint32_t> tail_{0}; // Written by Consumer

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

class ChannelManager {
public:
    static ChannelManager& instance();

    bool init(uint32_t node_id = 0, uint16_t base_udp_port = 7400);
    void shutdown();
    bool is_initialized() const;

    // Workload API primitives
    int32_t open(const std::string& pod_id, const std::string& channel_uri, uint32_t flags);
    int32_t send(const std::string& pod_id, int32_t handle, const uint8_t* data, size_t len);
    int32_t recv(const std::string& pod_id, int32_t handle, uint8_t* out_buf, size_t max_len, uint32_t timeout_ms);
    int32_t close(const std::string& pod_id, int32_t handle);

    // Frame ingress hook (invoked by UDP or ESP-NOW receiver thread)
    void on_frame_received(const uint8_t* frame, size_t len);

private:
    ChannelManager();
    ~ChannelManager();

    osal::Mutex mutex_;
    int32_t next_handle_{1};
    std::map<int32_t, ChannelHandle> handles_;
    std::unique_ptr<IChannelTransport> transport_;
    std::atomic<uint16_t> seq_generator_{1};
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
        uint8_t  buffer[4096];
    };
    osal::Mutex reassembly_mutex_;
    std::map<uint64_t, ReassemblyEntry> reassembly_table_;
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
