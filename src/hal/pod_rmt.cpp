#include "hal/pod_rmt.hpp"
#include "hal/hal_types.hpp"
#include "hal/mock_hal.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#endif

namespace kube32::hal {

namespace {

constexpr uint32_t kMaxHalfTicks = 32767;     // a symbol half holds a 15-bit duration
constexpr uint32_t kBlockSymbols = 48;        // one RMT memory block (TX ping-pong, RX copied out in parts)
#if defined(ESP_PLATFORM)
constexpr uint32_t kResolutionHz = 1000000;   // one tick per microsecond
constexpr uint32_t kRxGlitchNs = 1250;        // shorter pulses are filtered out as noise
#endif
constexpr uint8_t kDropFull = 1;
constexpr uint8_t kDropLong = 2;

inline uint16_t load_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline void store_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

struct TxSlot {
    std::string reserved;   // the pod the slot is held for (s_registry)
    std::mutex mutex;       // the driver; held for a whole send, so a release waits for the frame
    RmtState state;         // owner and installed: the pod the driver is up for
#if defined(ESP_PLATFORM)
    rmt_channel_handle_t chan{nullptr};
    rmt_encoder_handle_t encoder{nullptr};
    // Encoder cursor, advanced in rmt_transmit and then in the RMT interrupt
    uint32_t count{0};
    uint32_t index{0};
    uint32_t left{0};
#endif
};

// The queue holds committed frames in ring[head, head + used) (lengths in
// lens[first ...]) and, after them, the frame being received. The interrupt
// appends and commits, the pod's reads pop; both change the indices under
// the queue lock. The pod copies a committed frame without the lock: the
// interrupt writes only past the committed frames.
struct RxSlot {
    std::string reserved;   // the pod the slot is held for (s_registry)
    std::mutex mutex;       // the driver: install, release and the pod's reads
    RmtState state;
    uint16_t* ring{nullptr};
    uint16_t head{0};
    uint16_t used{0};
    uint16_t pending{0};   // durations of the frame being received
    uint16_t lens[kRmtRxQueueFrames]{};
    uint8_t first{0};
    uint8_t nframes{0};
    bool started{false};   // the frame being received has had its first mark
    bool ended{false};     // ... and its end (a zero duration)
    bool last_mark{false};
    uint8_t dropping{0};   // why the frame being received is not kept
    uint32_t drops_full{0};
    uint32_t drops_long{0};
    bool closing{true};    // set before the driver goes: the interrupt stops re-arming
#if defined(ESP_PLATFORM)
    bool armed{false};     // a receive is pending in the driver
    portMUX_TYPE lock;
    rmt_channel_handle_t chan{nullptr};
    rmt_symbol_word_t* block{nullptr};
    rmt_receive_config_t rx_cfg{};
    RxSlot() { portMUX_INITIALIZE(&lock); }
#else
    std::mutex lock;
#endif
};

// Slots are created when a pod first claims a channel and kept for the next
// one: a board pays for the channels its pods have used. s_registry covers
// creating them and `reserved`, so a lookup does not wait on the slot mutex
// a send holds for the length of a frame.
TxSlot* s_tx[kRmtChannels];
RxSlot* s_rx[kRmtChannels];
std::mutex s_registry;
std::atomic<uint32_t> s_tx_installs{0};
std::atomic<uint32_t> s_rx_installs{0};

void queue_lock(RxSlot& s) {
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL_SAFE(&s.lock);
#else
    s.lock.lock();
#endif
}

void queue_unlock(RxSlot& s) {
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL_SAFE(&s.lock);
#else
    s.lock.unlock();
#endif
}

void queue_reset(RxSlot& s) {
    s.head = s.used = s.pending = 0;
    s.first = s.nframes = 0;
    s.started = s.ended = s.last_mark = false;
    s.dropping = 0;
    s.drops_full = s.drops_long = 0;
}

// Appends captured symbols (the rmt_symbol_word_t layout: a 15-bit duration
// and a level bit, twice per word) to the frame being received. The channel
// inverts an active-low line, so a mark is level 1. Halves before the first
// mark are skipped, two halves of one level are one duration, and a zero
// duration ends the frame. Queue lock held.
void rx_append(RxSlot& s, const uint32_t* words, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        for (unsigned h = 0; h < 2; ++h) {
            if (s.ended) return;
            const uint32_t half = (words[i] >> (16 * h)) & 0xffff;
            const uint32_t ticks = half & 0x7fff;
            const bool mark = (half >> 15) != 0;
            if (ticks == 0) {
                s.ended = true;
                return;
            }
            if (!s.started) {
                if (!mark) continue;
                s.started = true;
            } else if (mark == s.last_mark) {
                if (!s.dropping && s.pending > 0) {
                    uint16_t& d = s.ring[(s.head + s.used + s.pending - 1) % kRmtRxQueueDurations];
                    d = static_cast<uint16_t>(d + ticks > 0xffff ? 0xffff : d + ticks);
                }
                continue;
            }
            s.last_mark = mark;
            if (s.dropping) continue;
            if (s.pending >= kRmtMaxDurations) {
                s.dropping = kDropLong;
            } else if (s.used + s.pending >= kRmtRxQueueDurations) {
                s.dropping = kDropFull;
            } else {
                s.ring[(s.head + s.used + s.pending) % kRmtRxQueueDurations] = static_cast<uint16_t>(ticks);
                s.pending++;
            }
        }
    }
}

// Ends the frame being received: queued, or counted as dropped. Queue lock held.
void rx_commit(RxSlot& s) {
    if (s.dropping == kDropLong) {
        s.drops_long++;
    } else if (s.dropping == kDropFull || (s.pending > 0 && s.nframes == kRmtRxQueueFrames)) {
        s.drops_full++;
    } else if (s.pending > 0) {
        s.lens[(s.first + s.nframes) % kRmtRxQueueFrames] = s.pending;
        s.nframes++;
        s.used = static_cast<uint16_t>(s.used + s.pending);
    }
    s.pending = 0;
    s.started = s.ended = s.last_mark = false;
    s.dropping = 0;
}

void* alloc_internal(size_t bytes) {
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return std::malloc(bytes);
#endif
}

// Free internal RAM, to report what a channel costs (0 on the simulator)
size_t internal_free() {
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#else
    return 0;
#endif
}

#if defined(ESP_PLATFORM)

// The next duration half of the frame: level 1 for a mark (carrier on);
// a duration longer than one half holds continues in the next half.
bool next_half(TxSlot& s, const uint8_t* d, uint32_t& level, uint32_t& ticks) {
    while (s.left == 0) {
        if (++s.index >= s.count) return false;
        s.left = load_le16(d + 2 * s.index);
    }
    ticks = s.left > kMaxHalfTicks ? kMaxHalfTicks : s.left;
    s.left -= ticks;
    level = (s.index & 1) == 0 ? 1 : 0;
    return true;
}

// Simple-encoder callback: fills the RMT memory block straight from the
// guest's durations, which stay in place while the caller waits for the
// frame. A frame ending in a mark gets a 1 us space to complete its symbol.
size_t encode_frame(const void* data, size_t, size_t symbols_written, size_t symbols_free, rmt_symbol_word_t* symbols, bool* done, void* arg) {
    TxSlot& s = *static_cast<TxSlot*>(arg);
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (symbols_written == 0) {
        s.index = 0;
        s.left = load_le16(d);
    }
    size_t n = 0;
    while (n < symbols_free) {
        uint32_t l0 = 0, t0 = 0, l1 = 0, t1 = 1;
        if (!next_half(s, d, l0, t0)) {
            *done = true;
            break;
        }
        const bool more = next_half(s, d, l1, t1);
        if (!more) {
            l1 = 0;
            t1 = 1;
        }
        symbols[n].level0 = l0;
        symbols[n].duration0 = t0;
        symbols[n].level1 = l1;
        symbols[n].duration1 = t1;
        n++;
        if (!more) {
            *done = true;
            break;
        }
    }
    return n;
}

// Starts the next receive unless the channel is going away. Queue lock held,
// so a release that sets `closing` never races a re-arm.
void rx_arm(RxSlot& s) {
    if (s.closing) return;
    s.armed = rmt_receive(s.chan, s.block, kBlockSymbols * sizeof(rmt_symbol_word_t), &s.rx_cfg) == ESP_OK;
}

// RMT interrupt: a part of a frame (the driver's block filled) or its end.
// The receiver is re-armed here, so it never waits for the pod.
bool on_rx_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t* ev, void* arg) {
    RxSlot& s = *static_cast<RxSlot*>(arg);
    portENTER_CRITICAL_ISR(&s.lock);
    if (s.ring) {
        rx_append(s, reinterpret_cast<const uint32_t*>(ev->received_symbols), ev->num_symbols);
        if (ev->flags.is_last) {
            rx_commit(s);
            rx_arm(s);
        }
    }
    portEXIT_CRITICAL_ISR(&s.lock);
    return false;
}
#endif

// Slot mutex held by the caller. `free_before`: free internal RAM before the
// slot was reserved, so the install line reports the whole cost of the claim.
bool install_tx(TxSlot& s, const std::string& pod_id, const RmtAssignment& a, size_t free_before, std::string& err) {
#if defined(ESP_PLATFORM)
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(a.pin);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = kBlockSymbols;
    cfg.trans_queue_depth = 1;
    esp_err_t e = rmt_new_tx_channel(&cfg, &s.chan);
    if (e == ESP_OK && a.carrier_hz > 0) {
        rmt_carrier_config_t cc = {};
        cc.frequency_hz = a.carrier_hz;
        cc.duty_cycle = static_cast<float>(a.duty_pct) / 100.0f;
        e = rmt_apply_carrier(s.chan, &cc);
    }
    if (e == ESP_OK) {
        rmt_simple_encoder_config_t ec = {};
        ec.callback = encode_frame;
        ec.arg = &s;
        ec.min_chunk_size = 1;
        e = rmt_new_simple_encoder(&ec, &s.encoder);
    }
    if (e == ESP_OK) e = rmt_enable(s.chan);
    if (e != ESP_OK) {
        if (s.encoder) rmt_del_encoder(s.encoder);
        if (s.chan) rmt_del_channel(s.chan);
        s.encoder = nullptr;
        s.chan = nullptr;
        gpio_reset_pin(static_cast<gpio_num_t>(a.pin));
        err = std::string("cannot create the RMT TX channel: ") + esp_err_to_name(e);
        return false;
    }
    const size_t free_after = internal_free();
    printf("[HAL] RMT TX installed for %s: GPIO %u, carrier %u Hz at %u%%, %u bytes of internal RAM\n", pod_id.c_str(), a.pin,
           (unsigned)a.carrier_hz, a.duty_pct, (unsigned)(free_before > free_after ? free_before - free_after : 0));
#else
    (void)free_before;
    printf("[HAL] RMT TX installed for %s: GPIO %u, carrier %u Hz at %u%%\n", pod_id.c_str(), a.pin, (unsigned)a.carrier_hz, a.duty_pct);
#endif
    s.state.installed = true;
    s.state.owner = pod_id;
    s.state.config = a;
    s.state.frames = 0;
    s.state.installs = ++s_tx_installs;
    return true;
}

void release_tx(TxSlot& s) {
    if (!s.state.installed) return;
#if defined(ESP_PLATFORM)
    rmt_disable(s.chan);
    rmt_del_encoder(s.encoder);
    rmt_del_channel(s.chan);
    s.encoder = nullptr;
    s.chan = nullptr;
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.config.pin));
#endif
    printf("[HAL] RMT TX on GPIO %u released by %s\n", s.state.config.pin, s.state.owner.c_str());
    s.state.installed = false;
    s.state.owner.clear();
}

bool install_rx(RxSlot& s, const std::string& pod_id, const RmtAssignment& a, size_t free_before, std::string& err) {
    uint16_t* ring = static_cast<uint16_t*>(alloc_internal(kRmtRxQueueDurations * sizeof(uint16_t)));
    if (!ring) {
        err = "no memory for the RMT receive queue";
        return false;
    }
#if defined(ESP_PLATFORM)
    s.block = static_cast<rmt_symbol_word_t*>(alloc_internal(kBlockSymbols * sizeof(rmt_symbol_word_t)));
    rmt_rx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(a.pin);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = kBlockSymbols;
    cfg.flags.invert_in = a.active_low;   // a mark reads as level 1 either way
    esp_err_t e = s.block ? rmt_new_rx_channel(&cfg, &s.chan) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        rmt_rx_event_callbacks_t cbs = {};
        cbs.on_recv_done = on_rx_done;
        e = rmt_rx_register_event_callbacks(s.chan, &cbs, &s);
    }
    bool enabled = false;
    if (e == ESP_OK) {
        e = rmt_enable(s.chan);
        enabled = e == ESP_OK;
    }
    if (e == ESP_OK) {
        s.rx_cfg = {};
        s.rx_cfg.signal_range_min_ns = kRxGlitchNs;
        s.rx_cfg.signal_range_max_ns = a.idle_us * 1000;
        s.rx_cfg.flags.en_partial_rx = true;   // frames longer than the block arrive in parts
        queue_lock(s);
        s.ring = ring;
        queue_reset(s);
        s.closing = false;
        rx_arm(s);
        if (!s.armed) {
            s.closing = true;
            s.ring = nullptr;
            e = ESP_FAIL;
        }
        queue_unlock(s);
    }
    if (e != ESP_OK) {
        if (enabled) rmt_disable(s.chan);
        if (s.chan) rmt_del_channel(s.chan);
        s.chan = nullptr;
        std::free(ring);
        heap_caps_free(s.block);
        s.block = nullptr;
        gpio_reset_pin(static_cast<gpio_num_t>(a.pin));
        err = std::string("cannot create the RMT RX channel: ") + esp_err_to_name(e);
        return false;
    }
    const size_t free_after = internal_free();
    printf("[HAL] RMT RX installed for %s: GPIO %u, active %s, idle %u us, %u bytes of internal RAM\n", pod_id.c_str(), a.pin,
           a.active_low ? "low" : "high", (unsigned)a.idle_us, (unsigned)(free_before > free_after ? free_before - free_after : 0));
#else
    queue_lock(s);
    s.ring = ring;
    queue_reset(s);
    s.closing = false;
    queue_unlock(s);
    (void)free_before;
    printf("[HAL] RMT RX installed for %s: GPIO %u, active %s, idle %u us\n", pod_id.c_str(), a.pin, a.active_low ? "low" : "high",
           (unsigned)a.idle_us);
#endif
    s.state.installed = true;
    s.state.owner = pod_id;
    s.state.config = a;
    s.state.frames = 0;
    s.state.installs = ++s_rx_installs;
    return true;
}

void release_rx(RxSlot& s) {
    if (!s.state.installed) return;
    queue_lock(s);
    s.closing = true;
    queue_unlock(s);
#if defined(ESP_PLATFORM)
    // After the channel is deleted its interrupt no longer runs: only then
    // do the queue and the driver's block go
    rmt_disable(s.chan);
    rmt_del_channel(s.chan);
    s.chan = nullptr;
    s.armed = false;
#endif
    queue_lock(s);
    uint16_t* ring = s.ring;
    s.ring = nullptr;
    queue_reset(s);
    queue_unlock(s);
    std::free(ring);
#if defined(ESP_PLATFORM)
    heap_caps_free(s.block);
    s.block = nullptr;
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.config.pin));
#endif
    printf("[HAL] RMT RX on GPIO %u released by %s\n", s.state.config.pin, s.state.owner.c_str());
    s.state.installed = false;
    s.state.owner.clear();
}

// Reserves a free slot for the pod, creating it on first use; null when all
// are taken (or no memory is left for one)
template <typename Slot>
Slot* reserve(Slot* (&slots)[kRmtChannels], const std::string& pod_id) {
    std::lock_guard<std::mutex> lock(s_registry);
    for (Slot*& slot : slots) {
        if (!slot) slot = new (std::nothrow) Slot();
        if (!slot) return nullptr;
        if (slot->reserved.empty()) {
            slot->reserved = pod_id;
            return slot;
        }
    }
    return nullptr;
}

template <typename Slot>
void unreserve(Slot* slot) {
    std::lock_guard<std::mutex> lock(s_registry);
    slot->reserved.clear();
}

// The slot at `i` when it is reserved for the pod (any pod when `pod_id` is null)
template <typename Slot>
Slot* slot_of(Slot* (&slots)[kRmtChannels], unsigned i, const std::string* pod_id) {
    std::lock_guard<std::mutex> lock(s_registry);
    Slot* slot = slots[i];
    return slot && !slot->reserved.empty() && (!pod_id || slot->reserved == *pod_id) ? slot : nullptr;
}

// The pod's slot; the caller checks the slot's state under its mutex
template <typename Slot>
Slot* find(Slot* (&slots)[kRmtChannels], const std::string& pod_id) {
    for (unsigned i = 0; i < kRmtChannels; ++i) {
        if (Slot* slot = slot_of(slots, i, &pod_id)) return slot;
    }
    return nullptr;
}

#if !defined(ESP_PLATFORM)
// Hands a frame to a receiver the way the RMT peripheral captures it: marks
// at level 1, durations above one symbol half split, a zero duration at the
// end, in parts of one memory block.
void deliver(RxSlot& s, const uint16_t* durations, size_t count) {
    uint32_t words[kBlockSymbols];
    size_t nwords = 0;
    unsigned halves = 0;
    uint32_t word = 0;
    auto put = [&](uint32_t level, uint32_t ticks) {
        word |= ((level << 15) | ticks) << (16 * (halves & 1));
        if (++halves % 2 == 0) {
            words[nwords++] = word;
            word = 0;
        }
        if (nwords == kBlockSymbols) {
            rx_append(s, words, nwords);
            nwords = 0;
        }
    };
    queue_lock(s);
    if (s.ring) {
        for (size_t i = 0; i < count; ++i) {
            uint32_t left = durations[i];
            while (left > 0) {
                const uint32_t ticks = left > kMaxHalfTicks ? kMaxHalfTicks : left;
                put((i & 1) == 0 ? 1 : 0, ticks);
                left -= ticks;
            }
        }
        put(0, 0);
        if (halves & 1) put(0, 0);
        rx_append(s, words, nwords);
        rx_commit(s);
    }
    queue_unlock(s);
}
#endif

} // namespace

bool rmt_install(const std::string& pod_id, const std::vector<RmtAssignment>& channels, std::string& out_error) {
    for (const auto& a : channels) {
        const size_t free_before = internal_free();
        bool ok = false;
        if (a.dir == RmtDir::Tx) {
            TxSlot* s = reserve(s_tx, pod_id);
            if (!s) {
                out_error = "all RMT TX channels are in use";
            } else {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    ok = install_tx(*s, pod_id, a, free_before, out_error);
                }
                if (!ok) unreserve(s);
            }
        } else {
            RxSlot* s = reserve(s_rx, pod_id);
            if (!s) {
                out_error = "all RMT RX channels are in use";
            } else {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    ok = install_rx(*s, pod_id, a, free_before, out_error);
                }
                if (!ok) unreserve(s);
            }
        }
        if (!ok) {
            rmt_release(pod_id);
            return false;
        }
    }
    return true;
}

// The pod's channels (every channel when `pod_id` is null)
static void release_slots(const std::string* pod_id) {
    for (unsigned i = 0; i < kRmtChannels; ++i) {
        if (TxSlot* s = slot_of(s_tx, i, pod_id)) {
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                release_tx(*s);
            }
            unreserve(s);
        }
        if (RxSlot* s = slot_of(s_rx, i, pod_id)) {
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                release_rx(*s);
            }
            unreserve(s);
        }
    }
}

void rmt_release(const std::string& pod_id) { release_slots(&pod_id); }

void rmt_release_all() { release_slots(nullptr); }

int32_t rmt_send(const std::string& pod_id, const uint8_t* durations, uint32_t count) {
    if (count == 0 || count > kRmtMaxDurations) return HAL_ERR_INVAL;
    uint32_t total_us = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t d = load_le16(durations + 2 * i);
        if (d == 0) return HAL_ERR_INVAL;
        total_us += d;
    }
    if (total_us > kRmtMaxFrameUs) return HAL_ERR_INVAL;
    TxSlot* slot = find(s_tx, pod_id);
    if (!slot) return HAL_ERR_NODEV;
    TxSlot& s = *slot;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed || s.state.owner != pod_id) return HAL_ERR_NODEV;
#if defined(ESP_PLATFORM)
    s.count = count;
    rmt_transmit_config_t tc = {};
    tc.flags.eot_level = 0;   // LED off after the frame
    esp_err_t e = rmt_transmit(s.chan, s.encoder, durations, count * sizeof(uint16_t), &tc);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(s.chan, static_cast<int>(total_us / 1000 + 100));
    if (e != ESP_OK) {
        // Abandons the frame: the encoder must not read the guest's memory
        // once this call has returned
        rmt_disable(s.chan);
        rmt_enable(s.chan);
        return e == ESP_ERR_TIMEOUT ? HAL_ERR_TIMEDOUT : HAL_ERR_IO;
    }
#else
    uint16_t frame[kRmtMaxDurations];
    for (uint32_t k = 0; k < count; ++k) frame[k] = load_le16(durations + 2 * k);
    MockHal::instance().rmt_transmit(s.state.config.pin, frame, count, s.state.config.carrier_hz);
    if (MockHal::instance().rmt_loopback()) {
        // The node's receivers see the LED
        for (unsigned r = 0; r < kRmtChannels; ++r) {
            if (RxSlot* rx = slot_of(s_rx, r, nullptr)) {
                std::lock_guard<std::mutex> rlock(rx->mutex);
                if (rx->state.installed) deliver(*rx, frame, count);
            }
        }
    }
#endif
    s.state.frames++;
    return static_cast<int32_t>(count);
}

int32_t rmt_receive(const std::string& pod_id, uint8_t* out, uint32_t cap, RmtDrops* out_drops) {
    RxSlot* slot = find(s_rx, pod_id);
    if (!slot) return HAL_ERR_NODEV;
    RxSlot& s = *slot;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed || s.state.owner != pod_id) return HAL_ERR_NODEV;
    queue_lock(s);
    if (out_drops) {
        out_drops->queue_full = s.drops_full;
        out_drops->too_long = s.drops_long;
    }
    s.drops_full = s.drops_long = 0;
#if defined(ESP_PLATFORM)
    if (!s.armed) rx_arm(s);   // a re-arm from the interrupt failed: try again
#endif
    if (s.nframes == 0) {
        queue_unlock(s);
        return HAL_ERR_AGAIN;
    }
    const uint16_t len = s.lens[s.first];
    const uint16_t head = s.head;
    queue_unlock(s);
    const uint32_t copy = len < cap ? len : cap;
    for (uint32_t k = 0; k < copy; ++k) store_le16(out + 2 * k, s.ring[(head + k) % kRmtRxQueueDurations]);
    queue_lock(s);
    s.head = static_cast<uint16_t>((head + len) % kRmtRxQueueDurations);
    s.used = static_cast<uint16_t>(s.used - len);
    s.first = static_cast<uint8_t>((s.first + 1) % kRmtRxQueueFrames);
    s.nframes--;
    queue_unlock(s);
    return len > cap ? HAL_ERR_TOOBIG : static_cast<int32_t>(len);
}

RmtState rmt_state(const std::string& pod_id, RmtDir dir) {
    if (dir == RmtDir::Tx) {
        TxSlot* s = find(s_tx, pod_id);
        if (!s) return RmtState{};
        std::lock_guard<std::mutex> lock(s->mutex);
        RmtState st = s->state;
        st.installs = s_tx_installs;
        return st.owner == pod_id ? st : RmtState{};
    }
    RxSlot* slot = find(s_rx, pod_id);
    if (!slot) return RmtState{};
    RxSlot& s = *slot;
    std::lock_guard<std::mutex> lock(s.mutex);
    RmtState st = s.state;
    st.installs = s_rx_installs;
    queue_lock(s);
    st.frames = s.nframes;
    queue_unlock(s);
    return st.owner == pod_id ? st : RmtState{};
}

#if !defined(ESP_PLATFORM)
bool rmt_inject(uint32_t pin, const uint16_t* durations, size_t count) {
    for (unsigned i = 0; i < kRmtChannels; ++i) {
        RxSlot* s = slot_of(s_rx, i, nullptr);
        if (!s) continue;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->state.installed && s->state.config.pin == pin) {
            deliver(*s, durations, count);
            return true;
        }
    }
    return false;
}
#endif

} // namespace kube32::hal
