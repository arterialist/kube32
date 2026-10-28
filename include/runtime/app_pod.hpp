#pragma once

// Host side of the Kube32 application ABI (see include/runtime/k32_app_abi.h).
//
// A pod whose module exports k32_handle is a request handler: the host feeds it
// HTTP requests (from the ingress proxy, the pod HTTP server, or the serial
// console) and streams whatever the guest writes back through an
// AppResponseSink. Pods that export k32_event receive cluster events; pods
// that export k32_tick get a 1 Hz callback.

#include "runtime/container_lifecycle.hpp"
#include "osal/mutex.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct M3Module* IM3Module;

namespace kube32::runtime {

struct AppRequest {
    std::string_view method;          // "GET"
    std::string_view path_and_query;  // "/api/issues?page=2"
    std::string_view cookie;          // raw Cookie header value, may be empty
    std::string_view content_type;
    std::string_view request_id;
    std::string_view forwarded_for;
    std::string_view body;
};

class AppResponseSink {
public:
    virtual ~AppResponseSink() = default;
    virtual void begin(int status, std::string_view content_type) = 0;
    virtual void header(std::string_view name, std::string_view value) = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual void end() = 0;
};

// Encodes an AppRequest into the text block defined by the ABI header.
std::string encode_app_request(const AppRequest& req);

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
};
void set_app_platform_hooks(AppPlatformHooks hooks);
void set_app_request_logging(bool verbose);
void set_app_gas_budget(uint64_t units);
const AppPlatformHooks& app_platform_hooks();

// Links the k32_* imports into a freshly parsed module. Called by the container
// lifecycle controller for every module so that missing imports never fail
// instantiation; permission is enforced at call time.
void link_app_host_functions(IM3Module module, const std::string& pod_id);

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

    // Registers a controller whose module exports at least one k32_* entry
    // point. Returns false when the module exports none of them.
    bool register_pod(const std::string& pod_id, ContainerLifecycleController* ctrl);
    void unregister_pod(const std::string& pod_id);
    bool has_pod(const std::string& pod_id) const;
    bool pod_handles_requests(const std::string& pod_id) const;
    std::vector<std::string> list_pods() const;
    // Returns the first registered request-handling pod id, or empty.
    std::string default_request_pod() const;

    // k32_init(); returns the guest result (0 = ready) or negative on trap/missing.
    int32_t init_pod(const std::string& pod_id);

    // Dispatches one request. Returns the HTTP status the guest produced, or a
    // negative error (-2 = no such pod, -1 = guest produced no response/trapped).
    int32_t handle(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink);
    int32_t tick(const std::string& pod_id, uint32_t uptime_ms);
    int32_t event(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len);
    // Delivers an event to every registered pod that exports k32_event.
    void broadcast_event(std::string_view topic, const uint8_t* data, size_t len);

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

    Entry* find_locked(const std::string& pod_id);
    int32_t call_with_block(Entry& e, const std::string& pod_id, const char* fn, std::string_view topic, const uint8_t* data, size_t len, AppResponseSink* sink);
};

} // namespace kube32::runtime
