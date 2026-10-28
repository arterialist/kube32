#pragma once

// Resource usage measurement on the node that runs the pods. The kubelet turns
// two consecutive cumulative samples into rates and pushes them to the API
// server's MetricsRegistry with its heartbeat.
//
// Platform split:
//   * Host simulator: HostMetricsProvider measures the process (resident set
//     size, getrusage CPU time). Both simulated kubelets live in that process,
//     so they report the process's usage.
//   * ESP32: EspMetricsProvider reports memory from the heap allocator (total
//     internal heap minus free) and CPU only once the firmware installs a
//     CpuTimeSampler (FreeRTOS run-time stats). Until then CPU is "not
//     measured" and the API omits it; nothing is ever reported as zero.
//
// Per-pod CPU comes from the interpreter time the app host accounts for
// (AppPodStats: k32_handle / k32_tick / k32_event calls). Pods that run a
// free-running main loop are not covered by that counter; the firmware can
// install a PodCpuTimeSampler keyed by pod key to measure their task time.

#include "runtime/wasm_runtime.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace kube32::kubelet {

struct NodeResourceSample {
    bool has_memory{false};
    uint64_t memory_used_bytes{0};   // internal heap in use
    bool has_cpu{false};
    uint64_t cpu_busy_us{0};         // cumulative busy time summed over all cores
    uint64_t cpu_wall_us{0};         // cumulative wall-clock time the busy figure spans
    // usage in nanocores = delta(busy) * 1e9 / delta(wall); two fully busy cores read 2e9n = 2000m
};

struct PodResourceSample {
    // memory_bytes counts what the node's "memory" resource governs (internal
    // SRAM); psram_bytes is the pod's footprint in external RAM, the resource
    // esp32.io/psram. A pod's 1.3 MB data set on a PSRAM board is psram, not memory.
    bool has_memory{false};
    uint64_t memory_bytes{0};
    bool has_psram{false};
    uint64_t psram_bytes{0};
    bool has_cpu{false};
    uint64_t cpu_busy_us{0};         // cumulative interpreter time
};

class MetricsProvider {
public:
    virtual ~MetricsProvider() = default;
    virtual bool sample_node(NodeResourceSample& out) = 0;
    virtual bool sample_pod(const std::string& pod_key, runtime::Runtime& runtime, PodResourceSample& out) = 0;
};

// ESP32 hooks. Declared here, installed by the firmware (main_esp32.cpp).
// busy is the cumulative run time of every non-idle task summed over both
// cores, wall the cumulative wall-clock microseconds (esp_timer_get_time());
// return false while FreeRTOS run-time statistics are unavailable.
using CpuTimeSampler = bool (*)(uint64_t& out_busy_us, uint64_t& out_wall_us);
void set_platform_cpu_sampler(CpuTimeSampler sampler);
CpuTimeSampler platform_cpu_sampler();
// Optional per-pod task time (cumulative microseconds) for free-running pods.
using PodCpuTimeSampler = bool (*)(const std::string& pod_key, uint64_t& out_busy_us);
void set_platform_pod_cpu_sampler(PodCpuTimeSampler sampler);

// Host: process measurements. ESP32: heap + installed samplers.
std::unique_ptr<MetricsProvider> create_platform_metrics_provider();

// Shared pod accounting used by both providers: interpreter time from
// AppPodStats plus memory reported by the runtime. Returns false when the pod
// is unknown to the runtime.
bool sample_pod_from_runtime(const std::string& pod_key, runtime::Runtime& runtime, PodResourceSample& out);

} // namespace kube32::kubelet
