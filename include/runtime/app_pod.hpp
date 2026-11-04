#pragma once

// Host side of the Kube32 application ABI (see include/runtime/k32_app_abi.h).
//
// A pod whose module exports k32_handle is a request handler: the host feeds it
// HTTP requests (from the ingress proxy, the pod HTTP server, or the serial
// console) and streams whatever the guest writes back through an
// AppResponseSink. A pod that exports k32_event receives the messages
// published on the channel topics named in its kube32.io/event-topics
// annotation (from any node); pods that export k32_tick get a 1 Hz callback.

#include "runtime/container_lifecycle.hpp"
#include "runtime/pod_snapshot.hpp"
#include "osal/mutex.hpp"
#include "osal/condition_variable.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct M3Module* IM3Module;
typedef struct M3Runtime* IM3Runtime;

namespace kube32::runtime {

struct AppRequest {
    std::string_view method;          // "GET"
    std::string_view path_and_query;  // "/api/issues?page=2"
    std::string_view cookie;          // raw Cookie header value, may be empty
    std::string_view content_type;
    std::string_view request_id;
    std::string_view forwarded_for;
    std::string_view body;
    // Not passed to the guest: the client accepts gzip (k32_resp_asset), and
    // the pods already serving this request, "<ns>/<name>" separated by
    // commas (X-Kube32-Call-Chain), which a service call must not re-enter.
    bool accepts_gzip{false};
    std::string_view call_chain;
};

class AppResponseSink {
public:
    virtual ~AppResponseSink() = default;
    virtual void begin(int status, std::string_view content_type) = 0;
    virtual void header(std::string_view name, std::string_view value) = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual void end() = 0;
    // Length of the whole body, announced after begin() when the host knows
    // it (k32_resp_asset); a sink may frame the body with it.
    virtual void body_length(size_t len) { (void)len; }
};

// Collects a response into a buffer the caller owns (service calls): the
// status and at most `cap` body bytes; the rest is counted, not kept.
class BufferResponseSink : public AppResponseSink {
public:
    BufferResponseSink(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
    void begin(int status, std::string_view) override { status_ = status; }
    void header(std::string_view, std::string_view) override {}
    bool write(const uint8_t* data, size_t len) override;
    void end() override { ended_ = true; }
    int status() const { return status_; }
    size_t capacity() const { return cap_; }
    size_t copied() const { return len_; }
    bool overflow() const { return total_ > cap_; }
    bool ended() const { return ended_; }
    // Appends bytes received from elsewhere (a remote pod's body)
    void append(const uint8_t* data, size_t len) { write(data, len); }
    // Empty again, for another answer
    void reset() { len_ = total_ = 0; status_ = 0; ended_ = false; }
private:
    uint8_t* buf_;
    size_t cap_;
    size_t len_{0};
    size_t total_{0};
    int status_{0};
    bool ended_{false};
};

// Encodes an AppRequest into the text block defined by the ABI header.
std::string encode_app_request(const AppRequest& req);

// Where a pod's bundle files come from (k32_resp_asset): an embedded bundle
// (images/web/<bundle>/ compiled into the firmware) or a directory on the
// node's storage (kube32/images/<name>/ for wasm://sd/<name> images).
struct AppAssetSource {
    std::string bundle;
    std::string directory;
    bool empty() const { return bundle.empty() && directory.empty(); }
};

// Sends file `path` of a bundle as a complete response through `sink`: the
// .gz variant when `accepts_gzip` and one exists, Content-Type from the
// extension, the body streamed from flash or read from storage with one
// small buffer. `headers` are sent with it. Returns 0, or K32_ERR_NOENT when
// the bundle has no such file (nothing is sent).
int32_t send_bundle_file(const AppAssetSource& src, std::string_view path, int status, bool accepts_gzip,
                         const std::vector<std::pair<std::string, std::string>>& headers, AppResponseSink& sink);
// Content-Type for a file name, from its extension.
const char* content_type_for_path(std::string_view path);

// A k32_http_call as the app layer receives it (see k32_app_abi.h).
struct ServiceCallRequest {
    std::string_view caller_pod;    // "<namespace>/<name>"
    std::string_view method;
    std::string_view url;           // "http://<service>[.<namespace>][:<port>]<path>"
    std::string_view headers;       // "Name: value\r\n" lines
    std::string_view body;
    std::string_view call_chain;    // pods serving the caller's request, the caller last
    uint32_t timeout_ms{5000};
    const void* caller_runtime{nullptr};   // wasm3 runtime of the caller (stop checks)
    // A call made for another node (the egress gateway): true once that
    // node no longer waits for the answer
    std::function<bool()> abandoned;
};

// Platform services used by the hardware imports. The firmware installs these
// at boot; the host simulator installs printing stand-ins.
struct AppPlatformHooks {
    std::function<int32_t(int32_t x, int32_t y, const char* text, size_t len, uint32_t fg, uint32_t bg)> display_text;
    std::function<int32_t(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)> display_rect;
    std::function<int32_t(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg, uint32_t bg)> display_bar;
    // k32_display_blit: w x h RGB565 pixels, row after row (the pod's memory, any alignment)
    std::function<int32_t(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels)> display_blit;
    std::function<int32_t()> key_read;
    std::function<int32_t(uint32_t freq, uint32_t ms, uint32_t vol)> tone;
    // k32_audio_play: count samples (the pod's memory, any alignment) at rate
    std::function<int32_t(const int16_t* samples, uint32_t count, uint32_t rate, uint32_t vol)> audio_play;
    // k32_mic_read: up to max samples into out (any alignment)
    std::function<int32_t(int16_t* out, uint32_t max, uint32_t timeout_ms)> mic_read;
    std::function<uint32_t()> random;
    std::function<int32_t(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len)> event_emit;
    // k32_http_call: resolves the Service and performs the call. Returns the
    // HTTP status or a negative K32_ERR_* code; the body goes to `sink`.
    std::function<int32_t(const ServiceCallRequest& req, BufferResponseSink& sink)> service_call;
};
void set_app_platform_hooks(AppPlatformHooks hooks);
void set_app_request_logging(bool verbose);
const AppPlatformHooks& app_platform_hooks();

// Links the k32_* imports into a freshly parsed module. Called by the container
// lifecycle controller for every module so that missing imports never fail
// instantiation; permission is enforced at call time.
void link_app_host_functions(IM3Module module, const std::string& pod_id);
// Forgets the pod link_app_host_functions recorded for a runtime about to be freed.
void unlink_app_host_functions(IM3Runtime runtime);
// The pod a wasm3 runtime runs, for host calls defined outside app_pod.cpp
// ("default-pod" for a runtime no pod was linked to)
const std::string& app_pod_id(IM3Runtime runtime);
// What a restored pod's clock is ahead of the node's uptime (k32_time_ms,
// esp32_get_time_us, the k32_tick argument): 0 for a pod that started here.
int64_t pod_clock_offset_ms(const void* m3_runtime);
void set_pod_clock_offset(const void* m3_runtime, int64_t offset_ms);

// The bytes of a snapshot handed to `feed` in order (false from it stops the
// stream); returns true when the stream ended whole, else false with `why`.
using SnapshotStream = std::function<bool(mem::FunctionRef<bool(const uint8_t*, size_t)> feed, std::string& why)>;

// A frozen pod's snapshot, as AppPodHost::snapshot took it
struct SnapshotSummary {
    size_t bytes{0};
    uint8_t digest[32]{};       // the trailer
    uint8_t image_sha[32]{};
    uint64_t pod_time_ms{0};
};

// A pod worker: a thread that calls the k32_tick and k32_event of the pods
// placed on it (AppPodHost::start_workers)
struct PodWorkerOptions {
    int core{-1};                  // the core it runs on, kube32.io/core names it
    bool pinned{false};            // pinned to `core` (the boards)
    size_t stack_bytes{0};         // 0: the platform default
    bool stack_in_psram{false};
    void* stack_buffer{nullptr};   // the stack (stack_bytes), owned by the caller
    // The stack holds the interpreter: the worker's calls into pods run on
    // it, not on the shared interpreter stack (kube32_interp_stack_add)
    bool own_interpreter_stack{false};
    uint32_t priority{4};
};

struct AppPodStats {
    uint64_t requests{0};
    uint64_t errors{0};
    uint64_t cpu_us{0};      // CPU time of the pod's export calls (runtime/pod_cpu.hpp)
    uint32_t last_us{0};     // wall time of the last request, lock wait included
    uint32_t max_us{0};
};

class AppPodHost {
public:
    static AppPodHost& instance();

    // Environment must be known before the module links (k32_env_get).
    void set_env(const std::string& pod_id, const std::vector<std::pair<std::string, std::string>>& env);
    int32_t env_get(const std::string& pod_id, std::string_view key, std::string& out) const;
    // Channel topics delivered to the pod's k32_event while it is registered.
    // Messages are queued and k32_event runs on an event thread with a stack
    // sized for the interpreter (not on the channel dispatcher); the thread
    // exists while some pod subscribes.
    void set_event_topics(const std::string& pod_id, const std::vector<std::string>& topics);
    // Stack of the event thread (default: 32 KiB in PSRAM on boards with
    // PSRAM, 16 KiB of internal RAM without, 256 KiB on the host).
    void set_event_thread_stack(size_t bytes, bool in_psram);
    uint64_t events_dropped() const;

    // Registers a controller whose module exports at least one k32_* entry
    // point. Returns false when the module exports none of them.
    bool register_pod(const std::string& pod_id, ContainerLifecycleController* ctrl);
    void unregister_pod(const std::string& pod_id);
    bool has_pod(const std::string& pod_id) const;
    bool pod_handles_requests(const std::string& pod_id) const;
    std::vector<std::string> list_pods() const;
    // The same into `out`, reusing its strings: a caller that lists pods
    // over and over (the tick loop) keeps one vector and allocates nothing
    // once it has held every pod id.
    void list_pods(std::vector<std::string>& out) const;
    // Returns the first registered request-handling pod id, or empty.
    std::string default_request_pod() const;

    // k32_init(); returns the guest result (0 = ready) or negative on trap/missing.
    int32_t init_pod(const std::string& pod_id);

    // Dispatches one request. Returns the HTTP status the guest produced, or a
    // negative error (-2 = no such pod, -1 = guest produced no response/trapped).
    // A pod named in req.call_chain is not entered again: 508 Loop Detected.
    int32_t handle(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink);
    // Calls a pod of this node from inside another pod's host call (service
    // calls), on the caller's thread and stack. Returns the guest's status or
    // K32_ERR_NOENT (no such pod), K32_ERR_LOOP (the pod is in
    // req.call_chain), K32_ERR_NOMEM (too little stack left for a nested
    // interpreter, see set_nested_stack_reserve), K32_ERR_TIMEOUT (the pod
    // stayed busy with another request for `timeout_ms`).
    int32_t call_nested(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink, uint32_t timeout_ms);
    // Native stack a nested call needs left on the calling thread (default:
    // 16 KiB on the board, 128 KiB on the host).
    void set_nested_stack_reserve(size_t bytes) { nested_stack_reserve_ = bytes; }

    // Bundle files of a pod (k32_resp_asset); set before the module links.
    void set_assets(const std::string& pod_id, AppAssetSource src);
    AppAssetSource assets(const std::string& pod_id) const;
    int32_t tick(const std::string& pod_id, uint32_t uptime_ms);
    // k32_tick period of a pod (default 1 s); set before it registers.
    void set_tick_interval(const std::string& pod_id, uint32_t ms);
    // Ticks every pod whose period has passed, and 10 ms after a tick that
    // returned >0 (background work left). Returns the milliseconds until the
    // next pod is due, at most `max_wait_ms`.
    uint32_t tick_due(uint64_t now_ms, uint32_t max_wait_ms = 1000);
    int32_t event(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len);

    // Pod workers ------------------------------------------------------------
    // Each worker ticks the pods placed on it and delivers their events, so
    // pods on two workers (two cores of the ESP32-S3) run at the same time;
    // calls into one pod stay serialized by its call lock wherever they come
    // from. A pod goes to the worker of the core its kube32.io/core names,
    // else to the worker with the least CPU of its pods' ticks and events
    // over the last window (then the fewest pods). Pods that call each other
    // in-process (k32_http_call to a Service on the node) share a worker. Every
    // kBalanceWindowMs a worker whose pods used a quarter of the window more
    // CPU than another's hands one group of placed-by-load pods over when
    // that lowers the busier figure; a group stays put for kMoveHoldMs after
    // a move. Without workers, tick_due ticks every pod and the event thread
    // delivers every event.
    static constexpr uint32_t kBalanceWindowMs = 2000;
    static constexpr uint32_t kMoveHoldMs = 10000;
    // Starts the workers once; returns how many run (a worker whose thread
    // cannot start is left out).
    size_t start_workers(const std::vector<PodWorkerOptions>& workers);
    // Stops and joins the workers; pods stay registered (tests)
    void stop_workers();
    size_t worker_count() const { return worker_n_.load(); }
    // kube32.io/core: the core whose worker runs the pod, -1 for placement
    // by load; set before the pod registers
    void set_core(const std::string& pod_id, int core);
    // The core of the pod's worker; -1 for no such pod or no workers
    int pod_core(const std::string& pod_id) const;

    AppPodStats stats(const std::string& pod_id) const;
    // Diagnostics: the request currently executing (empty when idle) and its age
    std::string in_flight(uint32_t& out_age_ms) const;

    // Migration (docs/migration.md) ---------------------------------------
    // The pod's container name and what in its spec keeps it from moving
    // (static_migration_blockers); set before the module loads, kept until
    // the pod is unregistered. add_migration_blocker adds one found later
    // (the module's entry point).
    void set_migration_info(const std::string& pod_id, const DeploySpec& spec);
    // Notes the module's entry point (_start, app_main, main) as a blocker
    void note_entry_point(const std::string& pod_id, ContainerLifecycleController& ctrl);
    // False when no pod of that id was deployed here
    bool container_name(const std::string& pod_id, std::string& out) const;
    // Every reason the pod cannot move now: the static ones and its open files
    std::vector<std::string> migration_blockers(const std::string& pod_id) const;
    // Stops handing the pod requests, ticks and events once the call it is in
    // returns: requests get 503 with Retry-After: 1, events are held (at most
    // kMaxHeldEvents, the rest dropped and counted). Returns 0, -2 for no
    // such request pod, -1 when a call was still running after `timeout_ms`
    // (`why` names it; the pod is not frozen). A frozen pod freezes again at once.
    int freeze(const std::string& pod_id, uint32_t timeout_ms, std::string& why);
    // Resumes a frozen pod: the held events are delivered first. False when
    // the pod is not frozen.
    bool thaw(const std::string& pod_id);
    bool is_frozen(const std::string& pod_id) const;
    // Writes the K32S snapshot of a frozen pod through `out` (see
    // write_snapshot); the same frozen pod gives the same bytes every time.
    bool snapshot(const std::string& pod_id, SnapshotWrite out, SnapshotSummary& summary, std::string& why);
    // Writes the snapshot `stream` delivers into a pod registered moments
    // ago, instead of k32_init: memory, globals and tables, then its clock,
    // channels and held events, then the export k32_restored once. Returns
    // 0; -1 when the snapshot could not be restored (`why` starts with
    // kRestoreErrorPrefix); -2 when k32_restored returned non-zero (`why`).
    int restore_pod(const std::string& pod_id, const SnapshotStream& stream, std::string& why);

private:
    AppPodHost() = default;
    struct Entry {
        ContainerLifecycleController* ctrl{nullptr};
        bool has_handle{false};
        bool has_event{false};
        bool has_tick{false};
        bool has_alloc{false};
        bool has_init{false};
        bool ticking{false};      // k32_init has returned 0 (or there is none): ticks may run
        uint32_t tick_interval_ms{1000};
        uint64_t next_tick_ms{0};
        AppPodStats stats;
        std::unique_ptr<osal::Mutex> call_mutex; // serializes host-initiated calls per pod
        const char* busy_fn{nullptr};            // the export running now
        uint64_t busy_since_ms{0};
        bool frozen{false};
        uint64_t frozen_pod_time_ms{0};          // the pod's clock when it froze
        uint64_t frozen_unix_ms{0};              // the wall clock then (0: unset)
        int64_t clock_offset_ms{0};
        std::vector<HeldEvent> held;             // events that arrived while frozen
        uint32_t held_dropped{0};
        // Pod workers
        int core_request{-1};                    // kube32.io/core, -1: placed by load
        size_t worker{0};
        uint32_t group{0};                       // pods linked by in-process calls share one
        bool in_worker{false};                   // a worker ticks it now
        uint64_t worker_cpu_us{0};               // CPU of its calls on worker threads
        uint64_t balance_mark_us{0};             // worker_cpu_us at the last balance
        uint64_t moved_ms{0};
    };
    struct MigrationInfo {
        std::string container;
        std::vector<std::string> blockers;
    };
    std::map<std::string, MigrationInfo> migration_;
    void set_busy(const std::string& pod_id, const char* fn);
    // Charges a call's CPU time to the pod (AppPodStats::cpu_us)
    void add_cpu(const std::string& pod_id, uint64_t us);
    // Puts events back at the front of the event queue, in order
    void requeue_events(const std::string& pod_id, std::vector<HeldEvent>&& events);
    // An export called with a fresh budget, named while it runs (freeze timeouts)
    int32_t call_export(const std::string& pod_id, ContainerLifecycleController* ctrl, const char* fn);
    mutable osal::Mutex mutex_;
    std::map<std::string, Entry> pods_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> env_;
    std::map<std::string, AppAssetSource> assets_;
    std::map<std::string, uint32_t> tick_intervals_;
    std::vector<std::string> tick_scratch_;   // tick_due's list of due pods
#if defined(ESP_PLATFORM)
    size_t nested_stack_reserve_{16 * 1024};
#else
    size_t nested_stack_reserve_{128 * 1024};
#endif
    std::map<std::string, std::vector<std::string>> event_topics_;   // pod -> topics
    std::map<std::string, uint32_t> topic_subs_;                      // topic -> channel subscription

    // One allocation per message: the pod's id, the topic and the data in
    // one buffer; a channel message is not copied: the event holds it (every
    // pod subscribed to the topic shares it). `worker` is the pod's worker
    // (kept current by moves)
    struct PendingEvent {
        std::string buf;
        std::shared_ptr<const void> msg;
        std::string_view msg_data;
        uint16_t id_len{0}, topic_len{0};
        uint16_t worker{0};
        PendingEvent() = default;
        PendingEvent(std::string_view pod_id, std::string_view topic, std::string_view data, size_t w = 0)
            : id_len(static_cast<uint16_t>(pod_id.size())), topic_len(static_cast<uint16_t>(topic.size())), worker(static_cast<uint16_t>(w)) {
            buf.reserve(pod_id.size() + topic.size() + data.size());
            buf.append(pod_id).append(topic).append(data);
        }
        PendingEvent(std::string_view pod_id, std::string_view topic, std::shared_ptr<const void> owner, std::string_view data, size_t w = 0)
            : msg(std::move(owner)), msg_data(data), id_len(static_cast<uint16_t>(pod_id.size())), topic_len(static_cast<uint16_t>(topic.size())),
              worker(static_cast<uint16_t>(w)) {
            buf.reserve(pod_id.size() + topic.size());
            buf.append(pod_id).append(topic);
        }
        std::string_view pod_id() const { return std::string_view(buf).substr(0, id_len); }
        std::string_view topic() const { return std::string_view(buf).substr(id_len, topic_len); }
        std::string_view data() const { return msg ? msg_data : std::string_view(buf).substr(static_cast<size_t>(id_len) + topic_len); }
    };
    mutable osal::Mutex event_mutex_;
    osal::ConditionVariable event_cv_;
    std::deque<PendingEvent> event_queue_;
    bool event_thread_running_{false};
    bool event_thread_wanted_{false};     // some pod subscribes to a topic
    size_t event_stack_bytes_{0};          // 0: platform default
    bool event_stack_in_psram_{false};
    uint64_t events_dropped_{0};
    // Messages waiting for a busy pod: 16, or 8 where internal RAM is all
    // there is (a frame of messages takes ~250 bytes)
    static constexpr size_t kMaxQueuedEvents = 16;
    static constexpr size_t kMaxQueuedEventsSmall = 8;
    void event_thread_main();
    void start_event_thread();

    struct Worker {
        AppPodHost* host{nullptr};
        size_t index{0};
        PodWorkerOptions opts;
        osal::Thread thread;
        std::vector<std::string> due;   // its pass's due pods, buffers kept
        std::string event_pod;          // the pod it delivers an event to now (event_mutex_)
        bool kick{false};               // a pod became due on it (event_mutex_)
        uint64_t load_us{0};            // CPU of its pods' worker calls in the last window (mutex_)
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<size_t> worker_n_{0};
    bool workers_stop_{false};          // event_mutex_
    std::map<std::string, int> core_requests_;
    uint32_t group_seq_{0};
    uint64_t balance_at_ms_{0};
    void worker_main(Worker& w);
    bool deliver_worker_event(Worker& w);
    uint32_t tick_worker(Worker& w, uint64_t now_ms, uint32_t max_wait_ms);
    void balance(uint64_t now_ms);
    size_t place_locked(int core_request) const;
    // Puts every pod of `group` on worker `to`, its queued events with it
    void move_group_locked(uint32_t group, size_t to, uint64_t now_ms, const char* why);
    // An in-process call from `caller` to `callee`: their groups join
    void link_locked(const std::string& caller, const std::string& callee, uint64_t now_ms);
    size_t worker_hint(const std::string& pod_id) const;
    void wake_workers();

    Entry* find_locked(const std::string& pod_id);
    // Subscribes the topics wanted by registered k32_event pods, drops the rest.
    void sync_topic_subscriptions_locked();
    // Hands one channel message to every pod subscribed to its topic: each
    // queued event holds the message (`owner`), data lies in it
    void deliver_topic(std::string_view topic, std::shared_ptr<const void> owner, const uint8_t* data, size_t len);
    int32_t call_with_block(Entry& e, const std::string& pod_id, const char* fn, std::string_view topic, const uint8_t* data, size_t len,
                            AppResponseSink* sink, const AppRequest* req = nullptr);
    // handle() and call_nested(): `wait_ms` 0 waits for a busy pod indefinitely
    int32_t dispatch(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink, uint32_t wait_ms, bool nested);
};

} // namespace kube32::runtime
