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
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <unistd.h>
#endif
#endif

namespace kube32::kubelet {

static CpuTimeSampler s_cpu_sampler = nullptr;
static PodCpuTimeSampler s_pod_cpu_sampler = nullptr;

void set_platform_cpu_sampler(CpuTimeSampler sampler) { s_cpu_sampler = sampler; }
CpuTimeSampler platform_cpu_sampler() { return s_cpu_sampler; }
void set_platform_pod_cpu_sampler(PodCpuTimeSampler sampler) { s_pod_cpu_sampler = sampler; }

bool sample_pod_from_runtime(const std::string& pod_key, runtime::Runtime& rt, PodResourceSample& out) {
    runtime::ContainerStatus st = rt.get_status(pod_key);
    if (st.phase == runtime::ContainerPhase::Terminated && st.exit_message == "Pod not found") return false;
    out = PodResourceSample{};
    if (st.memory_used_bytes > 0) {
        // Linear memory comes from the pod pool, which is PSRAM on boards that
        // have it; the interpreter's own allocations follow the same policy
        // (kube32_rt_calloc). Bytecode is parsed in place from flash.
        int pool_external = 0;
        kube32_pool_available(&pool_external);
        bool external = pool_external != 0;
        uint64_t linear = static_cast<uint64_t>(st.memory_used_bytes);
        uint64_t runtime_part = st.stack_bytes + st.runtime_bytes;
        out.has_memory = true;
        out.has_psram = external;
        if (external) {
            out.psram_bytes = linear + runtime_part;
            out.memory_bytes = 0;
        } else {
            out.memory_bytes = linear + runtime_part;
        }
    }
    uint64_t task_us = 0;
    if (s_pod_cpu_sampler && s_pod_cpu_sampler(pod_key, task_us)) {
        out.has_cpu = true;
        out.cpu_busy_us = task_us;
    } else if (runtime::AppPodHost::instance().has_pod(pod_key)) {
        out.has_cpu = true;
        out.cpu_busy_us = runtime::AppPodHost::instance().stats(pod_key).total_us;
    }
    return true;
}

#if defined(ESP_PLATFORM)

class EspMetricsProvider : public MetricsProvider {
public:
    bool sample_node(NodeResourceSample& out) override {
        out = NodeResourceSample{};
        size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (total > 0 && free_b <= total) {
            out.has_memory = true;
            out.memory_used_bytes = total - free_b;
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
#if defined(__APPLE__)
        mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
            out.has_memory = true;
            out.memory_used_bytes = static_cast<uint64_t>(info.resident_size);
        }
#else
        FILE* f = fopen("/proc/self/statm", "r");
        if (f) {
            unsigned long size = 0, resident = 0;
            if (fscanf(f, "%lu %lu", &size, &resident) == 2) {
                out.has_memory = true;
                out.memory_used_bytes = static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
            }
            fclose(f);
        }
#endif
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
