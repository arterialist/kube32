#include "kubelet/metrics_provider.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/app_pod.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace kube32::kubelet {

static CpuTimeSampler s_cpu_sampler = nullptr;

void set_platform_cpu_sampler(CpuTimeSampler sampler) { s_cpu_sampler = sampler; }
CpuTimeSampler platform_cpu_sampler() { return s_cpu_sampler; }

bool sample_pod_from_runtime(const std::string& pod_key, runtime::Runtime& rt, PodResourceSample& out) {
    runtime::ContainerStatus st = rt.get_status(pod_key);
    if (st.phase == runtime::ContainerPhase::Terminated && st.exit_message == "Pod not found") return false;
    out = PodResourceSample{};
    // Everything the pod holds, counted allocation by allocation as the
    // runtime makes them (ContainerStatus::held_bytes), wherever it lives:
    // internal RAM, the pool block or PSRAM
    if (st.held_bytes > 0) {
        out.has_memory = true;
        out.memory_bytes = st.held_bytes;
    }
    // CPU time: the pod's export calls on the node's threads plus its own
    // thread (an entry point), both measured as thread CPU time
    static const bool cpu_measured = [] {
        uint64_t probe = 0;
        return osal::Thread::current_cpu_time_us(probe);
    }();
    const bool app = runtime::AppPodHost::instance().has_pod(pod_key);
    if (cpu_measured && (app || st.has_thread_cpu)) {
        out.has_cpu = true;
        out.cpu_busy_us = (app ? runtime::AppPodHost::instance().stats(pod_key).cpu_us : 0) + (st.has_thread_cpu ? st.thread_cpu_us : 0);
    }
    return true;
}

#if defined(ESP_PLATFORM)

class EspMetricsProvider : public MetricsProvider {
public:
    bool sample_node(NodeResourceSample& out) override {
        out = NodeResourceSample{};
        // On the base of capacity.memory (internal heap and PSRAM): what is
        // allocated in both, less the pool block's free part, which the block
        // holds for pods but nothing uses
        const size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (total > 0 && free_b <= total && psram_free <= psram_total) {
            const uint64_t used = static_cast<uint64_t>(total - free_b) + (psram_total - psram_free);
            const uint64_t block_free = kube32_pool_available(nullptr);
            out.has_memory = true;
            out.memory_used_bytes = used > block_free ? used - block_free : 0;
        }
        uint64_t busy = 0, wall = 0;
        if (s_cpu_sampler && s_cpu_sampler(busy, wall) && wall > 0) {
            out.has_cpu = true;
            out.cpu_busy_us = busy;
            out.cpu_wall_us = wall;
        }
        return out.has_memory || out.has_cpu;
    }
    bool sample_pod(const std::string& pod_key, runtime::Runtime& rt, PodResourceSample& out) override {
        return sample_pod_from_runtime(pod_key, rt, out);
    }
};

std::unique_ptr<MetricsProvider> create_platform_metrics_provider() {
    return std::make_unique<EspMetricsProvider>();
}

#else

class HostMetricsProvider : public MetricsProvider {
public:
    bool sample_node(NodeResourceSample& out) override {
        out = NodeResourceSample{};
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            out.has_cpu = true;
            out.cpu_busy_us = static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000ULL + static_cast<uint64_t>(ru.ru_utime.tv_usec) +
                              static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000ULL + static_cast<uint64_t>(ru.ru_stime.tv_usec);
            out.cpu_wall_us = osal::Thread::get_time_us();
        }
        // On the base of capacity.memory: the heap a simulated board has
        // (--heap-budget) and its pool block, or the host's RAM
        if (kube32_pool_heap_budget()) {
            out.has_memory = true;
            out.memory_used_bytes = kube32_pool_general_bytes() + (kube32_pool_block_size() - kube32_pool_available(nullptr));
        } else {
            const size_t total = osal::System::get_total_heap_bytes();
            const size_t available = osal::System::get_free_heap_bytes();
            if (total > 0 && available <= total) {
                out.has_memory = true;
                out.memory_used_bytes = total - available;
            }
        }
        return out.has_memory || out.has_cpu;
    }
    bool sample_pod(const std::string& pod_key, runtime::Runtime& rt, PodResourceSample& out) override {
        return sample_pod_from_runtime(pod_key, rt, out);
    }
};

std::unique_ptr<MetricsProvider> create_platform_metrics_provider() {
    return std::make_unique<HostMetricsProvider>();
}

#endif

} // namespace kube32::kubelet
