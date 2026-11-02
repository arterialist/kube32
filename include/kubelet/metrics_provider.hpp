#pragma once

// Resource usage measurement on the node that runs the pods. The kubelet turns
// two consecutive cumulative samples into rates and pushes them to the API
// server's MetricsRegistry with its heartbeat.
//
// Platform split:
//   * Host simulator: HostMetricsProvider measures the host's RAM in use (or
//     what pods hold of a modelled board's heap and pool block, with
//     --heap-budget) and the process's getrusage CPU time. Both simulated
//     kubelets live in that process, so they report the process's usage.
//   * ESP32: EspMetricsProvider reports memory from the heap allocator (internal
//     heap and PSRAM in use, less the pool block's free part, on the base of
//     capacity.memory) and CPU only once the firmware installs a
//     CpuTimeSampler (FreeRTOS run-time stats). Until then CPU is "not
//     measured" and the API omits it; nothing is ever reported as zero.
//
// Per-pod memory is everything the pod's runtime holds (ContainerStatus::
// held_bytes), counted allocation by allocation, on every board.
// Per-pod CPU is thread CPU time (runtime/pod_cpu.hpp): the pod's export
// calls (k32_handle, k32_tick, k32_event, k32_init) on the node's threads,
// and the pod's own thread when it has an entry point (_start, app_main,
// main). Lock waits and blocking host calls are not CPU time; a call into
// another pod of the node is charged to that pod.

#include "runtime/wasm_runtime.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace kube32::kubelet {

struct NodeResourceSample {
    bool has_memory{false};
    uint64_t memory_used_bytes{0};   // internal heap and PSRAM in use
    bool has_cpu{false};
    uint64_t cpu_busy_us{0};         // cumulative busy time summed over all cores
    uint64_t cpu_wall_us{0};         // cumulative wall-clock time the busy figure spans
    // usage in nanocores = delta(busy) * 1e9 / delta(wall); two fully busy cores read 2e9n = 2000m
};

struct PodResourceSample {
    // Everything the pod holds, wherever it lives (internal RAM, the pool
    // block, PSRAM)
    bool has_memory{false};
    uint64_t memory_bytes{0};
    bool has_cpu{false};
    uint64_t cpu_busy_us{0};         // cumulative CPU time of the container
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

// Host: process measurements. ESP32: heap + installed samplers.
std::unique_ptr<MetricsProvider> create_platform_metrics_provider();

// Shared pod accounting used by both providers: CPU time of the pod's calls
// and thread plus memory reported by the runtime. Returns false when the pod
// is unknown to the runtime.
bool sample_pod_from_runtime(const std::string& pod_key, runtime::Runtime& runtime, PodResourceSample& out);

} // namespace kube32::kubelet
