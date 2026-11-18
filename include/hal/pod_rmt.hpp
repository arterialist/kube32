#pragma once

// RMT (infrared pulse train) channels handed to pods.
//
// The capability firewall admits a pod that claims esp32.io/rmt-tx or
// esp32.io/rmt-rx together with the pin the channel runs on; this module then
// creates the ESP-IDF RMT channel on that pin and deletes it when the pod's
// claims are released, leaving the pin reset. Durations are microseconds and
// a frame starts with a mark: on the wire a TX mark is the carrier switched
// on, an RX mark is the receiver's active level.
//
// A receiver stays armed from admission until release. Every frame it
// captures (ended by a space of idle_us) goes into a queue of its own, filled
// from the RMT interrupt and drained by rmt_receive without blocking. A frame
// that finds the queue full, or that is longer than kRmtMaxDurations, is
// dropped and counted.
//
// On the host simulator the channels are a stub with the same lifecycle and
// queue: MockHal records the frames sent, rmt_inject hands a receiver a frame,
// and with MockHal's IR loopback on every frame sent also reaches the
// node's receivers. A host with its own infrared devices (a Linux board's
// LIRC transmitter and receiver, set_host_ir) runs the channels on them
// instead: frames go out of its LED and come in from its receiver, into the
// same queue.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#if !defined(ESP_PLATFORM)
#include <functional>
#endif

namespace kube32::hal {

enum class RmtDir : uint8_t { Tx, Rx };

// The bit of direction `dir` in a mask of directions (BoardProfile::rmt_fixed_mask)
constexpr uint8_t rmt_dir_bit(RmtDir dir) { return dir == RmtDir::Tx ? 0x1 : 0x2; }

struct RmtAssignment {
    RmtDir dir{RmtDir::Tx};
    uint8_t pin{0};
    uint32_t carrier_hz{0};   // TX: carrier frequency; 0 sends the marks unmodulated
    uint8_t duty_pct{0};      // TX: carrier duty cycle
    bool active_low{true};    // RX: the line is low during a mark (IR demodulators)
    uint32_t idle_us{0};      // RX: a space this long ends the frame
};

constexpr uint32_t kRmtDefaultCarrierHz = 38000;
constexpr uint32_t kRmtMinCarrierHz = 10000;     // the carrier's high and low times count 80 MHz ticks in 16 bits
constexpr uint32_t kRmtMaxCarrierHz = 500000;
constexpr uint8_t kRmtDefaultDutyPct = 33;
constexpr uint32_t kRmtDefaultIdleUs = 12000;
constexpr uint32_t kRmtMinIdleUs = 1000;
constexpr uint32_t kRmtMaxIdleUs = 32767;       // the longest level one RMT symbol half holds at 1 MHz
constexpr uint32_t kRmtChannels = 4;            // per direction at most; a chip with fewer fails the claim at install
constexpr uint32_t kRmtMaxDurations = 512;      // per frame, sent or received (K32_RMT_MAX_DURATIONS)
constexpr uint32_t kRmtMaxFrameUs = 500000;     // total length of a frame sent (K32_RMT_MAX_FRAME_US)
constexpr uint32_t kRmtRxQueueFrames = 8;       // frames a receiver keeps for its pod
constexpr uint32_t kRmtRxQueueDurations = 512;  // durations a receiver keeps for its pod, all frames together

// Creates every channel of `channels` for the pod. On failure nothing it
// created stays, the pins are reset and `out_error` says why.
bool rmt_install(const std::string& pod_id, const std::vector<RmtAssignment>& channels, std::string& out_error);
// Deletes the pod's channels (no-op when it holds none) and resets their pins.
void rmt_release(const std::string& pod_id);
// Deletes every channel (tests).
void rmt_release_all();

// Sends one frame on the pod's TX channel and returns once it is out.
// `durations` holds `count` little-endian uint16 values (any alignment):
// mark, space, mark, ... Returns count, HAL_ERR_INVAL (count 0 or above
// kRmtMaxDurations, a zero duration, a frame longer than kRmtMaxFrameUs),
// HAL_ERR_NODEV (the pod has no TX channel) or HAL_ERR_TIMEDOUT.
int32_t rmt_send(const std::string& pod_id, const uint8_t* durations, uint32_t count);

// Frames a receiver dropped since its pod's previous rmt_receive
struct RmtDrops {
    uint32_t queue_full{0};   // the queue held kRmtRxQueueFrames frames or no room for the durations
    uint32_t too_long{0};     // more than kRmtMaxDurations durations
};

// Takes the oldest frame of the pod's receiver queue without waiting:
// little-endian uint16 durations into `out` (room for `cap`, any alignment),
// mark first. Returns the number of durations, HAL_ERR_AGAIN when the queue
// is empty, HAL_ERR_TOOBIG when the frame has more than `cap` durations (the
// first `cap` are copied and the frame leaves the queue), HAL_ERR_NODEV (the
// pod has no RX channel).
int32_t rmt_receive(const std::string& pod_id, uint8_t* out, uint32_t cap, RmtDrops* out_drops);

struct RmtState {
    bool installed{false};
    std::string owner;
    RmtAssignment config;
    uint32_t frames{0};     // TX: frames sent since install; RX: frames waiting in the queue
    uint32_t installs{0};   // channels of this direction created since boot
};
// The pod's channel of direction `dir` (installed false when it holds none)
RmtState rmt_state(const std::string& pod_id, RmtDir dir);

#if !defined(ESP_PLATFORM)
// Simulator: a frame arriving at the receiver on `pin`, mark first, as the
// RMT peripheral captures it. False when no receiver runs on the pin.
bool rmt_inject(uint32_t pin, const uint16_t* durations, size_t count);

// A host's own infrared transmitter (a Linux board's LIRC device). One pod
// holds it at a time.
class HostIrTransmitter {
public:
    virtual ~HostIrTransmitter() = default;
    virtual const std::string& device() const = 0;   // for the log
    // Takes the carrier and duty cycle of `a` for the frames that follow;
    // false with `err` set when the device refuses them
    virtual bool configure(const RmtAssignment& a, std::string& err) = 0;
    // Sends one frame (mark, space, mark, ...) and returns once it is out,
    // a last space included: `count`, or a negative HAL_ERR_*
    virtual int32_t send(const uint16_t* durations, size_t count) = 0;
};

// A host's own infrared receiver. One pod holds it at a time.
class HostIrReceiver {
public:
    // A captured frame, mark first, as a pod receives it. Called on the
    // receiver's thread.
    using Sink = std::function<void(const uint16_t* durations, size_t count)>;
    virtual ~HostIrReceiver() = default;
    virtual const std::string& device() const = 0;
    // Hands every frame that starts from now on to `sink`, frames ended by a
    // space of a.idle_us; false with `err` set when the device cannot start
    virtual bool start(const RmtAssignment& a, Sink sink, std::string& err) = 0;
    // Returns once `sink` is no longer called
    virtual void stop() = 0;
};

// Channels created from now on run on `tx` and `rx` (nullptr: the stub).
// Set before pods run.
void set_host_ir(HostIrTransmitter* tx, HostIrReceiver* rx);
#endif

} // namespace kube32::hal
