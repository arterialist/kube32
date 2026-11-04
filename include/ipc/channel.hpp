#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <map>
#include <deque>
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
constexpr size_t   K32C_MAX_PACKET_LEN      = 250;        // ESP-NOW v1 MTU: a small message is one such frame
constexpr size_t   K32C_HEADER_LEN          = 24;
constexpr size_t   K32C_CRC_LEN             = 2;
constexpr size_t   K32C_MAX_PAYLOAD_CHUNK   = K32C_MAX_PACKET_LEN - K32C_HEADER_LEN - K32C_CRC_LEN; // 224 bytes
// A large message goes out in frames of up to 1470 bytes (an ESP-NOW v2
// frame, and a UDP datagram that fits a 1500-byte Ethernet MTU)
constexpr size_t   K32C_MAX_FRAME_LEN       = 1470;
constexpr size_t   K32C_LARGE_CHUNK         = K32C_MAX_FRAME_LEN - K32C_HEADER_LEN - K32C_CRC_LEN; // 1444 bytes
// The largest channel message (topic publish or channel send)
constexpr size_t   K32C_MAX_MESSAGE_BYTES   = 16384;
constexpr size_t   K32C_MAX_FRAGMENTS       = (K32C_MAX_MESSAGE_BYTES + K32C_LARGE_CHUNK - 1) / K32C_LARGE_CHUNK; // 12
constexpr size_t   K32C_RING_SLOT_SIZE      = 1024;
constexpr size_t   K32C_RING_SLOT_COUNT     = 16;         // Must be power of 2
constexpr uint32_t K32C_REASSEMBLY_TIMEOUT  = 500;        // ms

// Packet Flags
constexpr uint8_t K32C_FLAG_FRAGMENT        = 0x01;
constexpr uint8_t K32C_FLAG_FIRST_FRAG      = 0x02;
constexpr uint8_t K32C_FLAG_LAST_FRAG       = 0x04;
constexpr uint8_t K32C_FLAG_ACK_REQ         = 0x08;
constexpr uint8_t K32C_FLAG_PING            = 0x10;
// Fragments of K32C_LARGE_CHUNK bytes (fragment i starts at i * 1444);
// without it a fragment holds K32C_MAX_PAYLOAD_CHUNK bytes
constexpr uint8_t K32C_FLAG_LARGE           = 0x20;

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
    uint16_t payload_len;    // Length of payload following header (0..224, 0..1444 with K32C_FLAG_LARGE)
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
// A received or published message in one buffer
// =============================================================================
// The buffer comes from PSRAM where the node has it, else from the heap,
// else from the pod pool block's small heap (a node without PSRAM keeps a
// reserve of its heap for Wi-Fi and lwIP). Subscribers share it: a pod's
// queued event holds the message, not a copy.
struct ChannelMessage {
    uint32_t channel_hash{0};
    uint32_t sender_hash{0};
    size_t len{0};
    size_t cap{0};
    uint8_t* data{nullptr};
    ChannelMessage() = default;
    ChannelMessage(const ChannelMessage&) = delete;
    ChannelMessage& operator=(const ChannelMessage&) = delete;
    ~ChannelMessage();
};
using MessagePtr = std::shared_ptr<ChannelMessage>;
// A message with room for `capacity` bytes (len 0), or nullptr when no
// memory can hold it
MessagePtr make_message(size_t capacity);

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
    // Hashes of the channels pods on this node opened by name
    std::vector<uint32_t> local_hashes() const;
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
    // The API server address a control plane announces in its HELLO
    // ("" from a worker)
    std::string api_address;
    // Where the node was last heard over ESP-NOW and over UDP (0: never).
    // A node heard over ESP-NOW in the last K32C_ESPNOW_FRESH_MS is sent
    // to over ESP-NOW (one hop, acknowledged by the radio), else over UDP.
    uint64_t espnow_seen_ms{0};
    uint64_t udp_seen_ms{0};
    uint8_t mac_address[6]{0};
    std::string ip_address;
    uint16_t udp_port{0};
    // The topics the node's subscriptions cover (sorted hashes), from its
    // announcements on K32C_SUBS_TOPIC; subs_ms is when the last one came,
    // 0 for a node that announces none (it is sent every topic)
    std::vector<uint32_t> subs;
    uint64_t subs_ms{0};
    // The bytes a second the node takes (0: no limit), from its
    // announcement; a publisher paces messages to it accordingly
    uint32_t rx_rate{0};
    uint64_t next_send_us{0};
};

// A node heard over ESP-NOW this recently is sent to over ESP-NOW
constexpr uint64_t K32C_ESPNOW_FRESH_MS = 10000;
// A node's subscription announcement holds this long; after that it is
// sent every topic again (it may run a firmware that does not announce)
constexpr uint64_t K32C_SUBS_FRESH_MS = 20000;
// The bytes a second a node without PSRAM asks publishers for, and the
// longest a publish waits for a peer's pace
constexpr uint32_t K32C_RX_RATE_NO_PSRAM = 48 * 1024;
constexpr uint32_t K32C_MAX_PACE_WAIT_MS = 500;

constexpr const char* K32C_HELLO_TOPIC = "k32.hello";
// A node name: that node's kubelet lists its pods at once (the control
// plane bound a pod to it or is deleting one)
constexpr const char* K32C_KUBELET_SYNC_TOPIC = "k32.kubelet.sync";
// A node's subscriptions: its node id, the bytes a second it takes (0: no
// limit) and the hash of every topic it subscribes to (u32 each,
// little-endian), sent with each HELLO and when they change. Publishers send
// a topic only to the nodes that subscribe, no faster than they take.
constexpr const char* K32C_SUBS_TOPIC = "k32.subs";

class ChannelManager {
public:
    static ChannelManager& instance();

    bool init(uint32_t node_id = 0, uint16_t base_udp_port = 7400);
    void shutdown();
    bool is_initialized() const;

    // ---- Topic publish/subscribe (host-side consumers) --------------------
    using TopicCallback = std::function<void(std::string_view topic, uint32_t sender_hash, const uint8_t* data, size_t len)>;
    // The same, with the message itself: the subscriber may keep it
    using MessageCallback = std::function<void(std::string_view topic, const MessagePtr& msg)>;
    // Delivers a message of up to K32C_MAX_MESSAGE_BYTES to the local ring
    // for the topic (if a pod opened it), to this node's subscribers, and to
    // every peer node that subscribes to the topic (every peer that does not
    // announce its subscriptions). Returns len, or an error when a peer's
    // radio did not acknowledge it or its sending failed (IPC_ERR_HOSTUNREACH,
    // IPC_ERR_TIMEDOUT, IPC_ERR_IO), when a local subscriber's queue was full
    // (IPC_ERR_AGAIN) or no memory could hold it (IPC_ERR_NOMEM). The
    // message still reaches every other destination.
    int32_t publish(const std::string& pod_id, const std::string& topic, const uint8_t* data, size_t len);
    uint32_t subscribe(const std::string& topic, TopicCallback cb);
    uint32_t subscribe_messages(const std::string& topic, MessageCallback cb);
    void unsubscribe(uint32_t id);
    // This node's subscriptions as sorted topic hashes (subscribers, and
    // channels pods opened by name)
    std::vector<uint32_t> local_topic_hashes() const;
    // Whether a publish on `topic_hash` goes to the peer `node_id`
    bool peer_wants(uint32_t node_id, uint32_t topic_hash) const;

    // ---- Transport tuning ------------------------------------------------------
    // Pause between two frames of one message to one peer (0: none)
    void set_fragment_gap_us(uint32_t us) { fragment_gap_us_.store(us); }
    uint32_t fragment_gap_us() const { return fragment_gap_us_.load(); }
    // Frames received over ESP-NOW waiting for the dispatcher (default 16, or
    // 32 with PSRAM)
    void set_rx_queue_frames(size_t n);
    size_t rx_queue_frames() const;
    // 0: ESP-NOW to a node heard over ESP-NOW recently, else UDP; 1: UDP
    // whenever the node's address is known; 2: ESP-NOW whenever its MAC is
    enum class Route : uint8_t { Auto = 0, Udp = 1, EspNow = 2 };
    void set_route(Route r) { route_.store(static_cast<uint8_t>(r)); }
    Route route() const { return static_cast<Route>(route_.load()); }
    // Bytes of messages held in reassembly and waiting for subscribers, and
    // the most the node holds (beyond it a message is dropped and counted)
    size_t rx_held_bytes() const;
    void set_rx_budget_bytes(size_t bytes) { rx_budget_bytes_.store(bytes); }
    // The bytes a second this node announces it takes (0: no limit; 48 KiB
    // without PSRAM)
    void set_rx_rate_bytes(uint32_t bytes) { rx_rate_bytes_.store(bytes); subs_changed_.store(true); }
    uint32_t rx_rate_bytes() const { return rx_rate_bytes_.load(); }
    size_t rx_budget_bytes() const { return rx_budget_bytes_.load(); }

    // ---- Peers -------------------------------------------------------------
    void set_local_identity(uint32_t node_id, const std::string& node_name);
    void add_peer(const PeerInfo& peer);
    std::vector<PeerInfo> peers() const;
    bool find_peer_by_name(const std::string& node_name, PeerInfo& out) const;
    uint32_t local_node_id() const { return local_node_id_; }
    // Starts the periodic HELLO announcement and the dispatcher thread.
    bool start_background(uint32_t hello_interval_ms = 3000, size_t dispatcher_stack_bytes = 8192, bool stack_in_psram = false);
    void send_hello();
    // A control plane announces where its API server answers: HELLO carries
    // the name, a NUL and "api=<address>". "" stops the announcement.
    void set_announced_api(const std::string& address);
    // The address the most recently heard control plane announces, or ""
    std::string announced_api() const;
    // Uptime in ms when a HELLO last arrived over IP (UDP) from `ip`, 0 if
    // none has: proof that the access point carries this node's traffic
    uint64_t last_ip_hello_ms(const std::string& ip) const;
    // Peers reachable over IP that cannot be found by ESP-NOW broadcast (nodes
    // on another access point of the same network): HELLO is sent to them
    // unicast until they answer and become regular peers.
    void add_seed_peer(const std::string& ip, uint16_t port);
    // ESP-NOW receive callback entry: copies the frame for the dispatcher thread.
    void enqueue_raw_frame(const uint8_t* src_mac, const uint8_t* frame, size_t len);

    // Workload API primitives. Handles are numbered per node; a handle names
    // a channel only together with the pod that opened it.
    int32_t open(const std::string& pod_id, const std::string& channel_uri, uint32_t flags);
    // Opens `channel_uri` for the pod under the handle number `handle` (a
    // restored pod gets back the handles it had). Fails with IPC_ERR_INVAL
    // when the pod already holds that handle.
    int32_t open_as(const std::string& pod_id, const std::string& channel_uri, uint32_t flags, int32_t handle);
    // The pod's open handles and the channel each names, by handle number
    std::vector<std::pair<int32_t, std::string>> handles_of(const std::string& pod_id) const;
    // Closes every handle the pod holds (its container stopped)
    void close_all(const std::string& pod_id);
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
        // Messages for this node's subscriptions that found the dispatcher's
        // queue full
        std::atomic<uint64_t> dispatch_dropped{0};
        // Messages published, and those a peer's send failed for
        std::atomic<uint64_t> msgs_published{0};
        std::atomic<uint64_t> publish_errors{0};
        std::atomic<uint64_t> frames_retried{0};
        // Sends a peer was left out of: it does not subscribe to the topic
        std::atomic<uint64_t> fanout_skipped{0};
        // Received messages delivered whole to this node's subscribers
        std::atomic<uint64_t> rx_messages{0};
        // Received messages dropped: a fragment never came (or the
        // reassembly slots were all taken), no memory or budget held it, the
        // ESP-NOW receive queue was full, or the frames did not fit together
        std::atomic<uint64_t> rx_incomplete{0};
        std::atomic<uint64_t> rx_nomem{0};
        std::atomic<uint64_t> rx_queue_full{0};
        std::atomic<uint64_t> rx_malformed{0};
        std::atomic<uint64_t> subs_rx{0};
        // Time publishers waited for a peer's announced rate
        std::atomic<uint64_t> paced_us{0};
    };
    ChannelStats& stats() { return stats_; }

private:
    ChannelManager();
    ~ChannelManager();

    mutable osal::Mutex mutex_;
    int32_t next_handle_{1};
    std::map<std::pair<std::string, int32_t>, ChannelHandle> handles_;   // by (pod, handle)
    int32_t open_locked(const std::string& pod_id, const std::string& channel_uri, uint32_t flags, int32_t hid);
    std::unique_ptr<IChannelTransport> transport_;
    std::atomic<uint16_t> seq_generator_{1};
    ChannelStats stats_;
    bool initialized_{false};

    // A message being reassembled: its fragments land in place in a buffer
    // sized for frag_count fragments
    struct ReassemblyEntry {
        uint32_t channel_hash{0};
        uint32_t sender_hash{0};
        uint16_t seq_num{0};
        uint8_t  total_frags{0};
        uint8_t  received_frags{0};
        uint32_t received_mask{0};
        uint32_t chunk{0};
        uint32_t last_len{0};          // the last fragment's bytes (when it came)
        uint64_t last_activity_ms{0};
        MessagePtr msg;
    };
    osal::Mutex reassembly_mutex_;
    std::vector<ReassemblyEntry> reassembly_table_;
    // Reassemblies at once: 2 without PSRAM, 8 with it
    size_t max_reassembly_{8};

    // Peers and identity
    mutable osal::Mutex peer_mutex_;
    std::map<uint32_t, PeerInfo> peers_;
    std::vector<NodeEndpoint> seed_peers_;   // guarded by peer_mutex_
    uint32_t local_node_id_{0};
    std::string local_node_name_;
    std::string announced_api_;   // guarded by peer_mutex_: what this node announces
    std::map<std::string, uint64_t> ip_hello_ms_;   // guarded by peer_mutex_: last HELLO over UDP per address

    // Subscribers and dispatcher
    struct Subscription {
        uint32_t id;
        uint32_t topic_hash;
        std::string topic;
        TopicCallback cb;
        MessageCallback msg_cb;
    };
    mutable osal::Mutex sub_mutex_;
    std::vector<Subscription> subs_;
    uint32_t next_sub_id_{1};
    std::atomic<bool> subs_changed_{false};   // announce the subscriptions now

    // Frames received over ESP-NOW (copied out of the radio's callback) and
    // messages for this node's subscribers, both waiting for the dispatcher
    struct RawFrame {
        uint8_t* data{nullptr};   // malloc'd: 6 bytes of source MAC, then the frame
        uint16_t len{0};
    };
    osal::Mutex disp_mutex_;
    osal::ConditionVariable disp_cv_;
    std::vector<RawFrame> rx_frames_;          // a ring of rx_cap_ entries
    size_t rx_head_{0}, rx_count_{0}, rx_cap_{16};
    std::deque<MessagePtr> msg_queue_;         // at most msg_cap_
    size_t msg_cap_{8};
    bool disp_ready_{false};
    std::unique_ptr<osal::Thread> dispatch_thread_;
    std::atomic<bool> background_running_{false};
    uint32_t hello_interval_ms_{3000};
    uint64_t last_hello_ms_{0};

    // Sending: one frame buffer, one sender at a time
    osal::Mutex send_mutex_;
    std::unique_ptr<uint8_t[]> tx_frame_;
    std::atomic<uint32_t> fragment_gap_us_{0};
    std::atomic<uint8_t> route_{0};
    std::atomic<size_t> rx_budget_bytes_{K32C_MAX_MESSAGE_BYTES * 8};
    bool has_psram_{false};
    std::atomic<uint32_t> rx_rate_bytes_{0};
    void pace_to(uint32_t node_id, size_t len, uint32_t rate);

    // Queues a message for this node's subscribers; false when it was
    // dropped (queue full)
    bool deliver_local(const MessagePtr& msg);
    bool deliver_local(uint32_t channel_hash, uint32_t sender_hash, const uint8_t* data, size_t len, bool* nomem = nullptr);
    int32_t send_frames_to(const NodeEndpoint& ep, uint32_t channel_hash, uint32_t sender_hash, uint32_t target_hash, const uint8_t* data, size_t len, uint8_t extra_flags);
    NodeEndpoint route_to(const PeerInfo& p, uint64_t now_ms) const;
    void dispatch_loop();
    void dispatch_message(const MessagePtr& msg);
    void prune_reassembly(uint64_t now_ms);
    void handle_hello(const K32CHeader* hdr, const uint8_t* payload, size_t len, const NodeEndpoint* source);
    void handle_subs(const uint8_t* payload, size_t len);
    void send_subs();
    void on_fragment(const K32CHeader* hdr, const uint8_t* payload);
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
