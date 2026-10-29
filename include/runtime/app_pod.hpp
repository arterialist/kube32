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
#include "osal/mutex.hpp"
#include "osal/condition_variable.hpp"

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
    size_t copied() const { return len_; }
    bool overflow() const { return total_ > cap_; }
    bool ended() const { return ended_; }
    // Appends bytes received from elsewhere (a remote pod's body)
    void append(const uint8_t* data, size_t len) { write(data, len); }
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
};

// Platform services used by the hardware imports. The firmware installs these
// at boot; the host simulator installs printing stand-ins.
struct AppPlatformHooks {
    std::function<int32_t(int32_t x, int32_t y, const char* text, size_t len, uint32_t fg, uint32_t bg)> display_text;
    std::function<int32_t(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)> display_rect;
    std::function<int32_t(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg, uint32_t bg)> display_bar;
    std::function<int32_t()> key_read;
    std::function<int32_t(uint32_t freq, uint32_t ms, uint32_t vol)> tone;
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

struct AppPodStats {
    uint64_t requests{0};
    uint64_t errors{0};
    uint64_t total_us{0};
    uint32_t last_us{0};
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
    int32_t event(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len);

    AppPodStats stats(const std::string& pod_id) const;
    // Diagnostics: the request currently executing (empty when idle) and its age
    std::string in_flight(uint32_t& out_age_ms) const;

private:
    AppPodHost() = default;
    struct Entry {
        ContainerLifecycleController* ctrl{nullptr};
        bool has_handle{false};
        bool has_event{false};
        bool has_tick{false};
        bool has_alloc{false};
        bool has_init{false};
        AppPodStats stats;
        std::unique_ptr<osal::Mutex> call_mutex; // serializes host-initiated calls per pod
    };
    mutable osal::Mutex mutex_;
    std::map<std::string, Entry> pods_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> env_;
    std::map<std::string, AppAssetSource> assets_;
#if defined(ESP_PLATFORM)
    size_t nested_stack_reserve_{16 * 1024};
#else
    size_t nested_stack_reserve_{128 * 1024};
#endif
    std::map<std::string, std::vector<std::string>> event_topics_;   // pod -> topics
    std::map<std::string, uint32_t> topic_subs_;                      // topic -> channel subscription

    struct PendingEvent {
        std::string pod_id;
        std::string topic;
        std::string data;
    };
    mutable osal::Mutex event_mutex_;
    osal::ConditionVariable event_cv_;
    std::deque<PendingEvent> event_queue_;
    bool event_thread_running_{false};
    bool event_thread_wanted_{false};     // some pod subscribes to a topic
    size_t event_stack_bytes_{0};          // 0: platform default
    bool event_stack_in_psram_{false};
    uint64_t events_dropped_{0};
    static constexpr size_t kMaxQueuedEvents = 16;
    void event_thread_main();
    void start_event_thread();

    Entry* find_locked(const std::string& pod_id);
    // Subscribes the topics wanted by registered k32_event pods, drops the rest.
    void sync_topic_subscriptions_locked();
    // Hands one channel message to every pod subscribed to its topic.
    void deliver_topic(std::string_view topic, const uint8_t* data, size_t len);
    int32_t call_with_block(Entry& e, const std::string& pod_id, const char* fn, std::string_view topic, const uint8_t* data, size_t len,
                            AppResponseSink* sink, const AppRequest* req = nullptr);
    // handle() and call_nested(): `wait_ms` 0 waits for a busy pod indefinitely
    int32_t dispatch(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink, uint32_t wait_ms, bool nested);
};

} // namespace kube32::runtime
