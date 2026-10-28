#pragma once

// metrics.k8s.io/v1beta1 backing store on the API server. Every kubelet
// measures its own node and pods (see kubelet/metrics_provider.hpp) and pushes
// the sample with its heartbeat; the registry keeps the latest sample per
// node and per pod, drops samples that stopped arriving, and renders
// NodeMetrics / PodMetrics. A field is emitted only when it was measured.

#include "osal/mutex.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kube32::api {

struct ContainerUsage {
    std::string name;
    bool has_cpu{false};
    uint64_t cpu_nanocores{0};
    bool has_memory{false};
    uint64_t memory_bytes{0};
    bool has_psram{false};           // external RAM footprint, reported as esp32.io/psram
    uint64_t psram_bytes{0};
};

struct NodeUsageSample {
    std::string node;
    bool has_cpu{false};
    uint64_t cpu_nanocores{0};
    bool has_memory{false};
    uint64_t memory_bytes{0};
    uint64_t window_ms{0};
    uint64_t sample_unix_ms{0};     // 0 when the kubelet had no wall clock
    uint64_t received_uptime_ms{0}; // API server uptime when the push arrived
};

struct PodUsageSample {
    std::string namespace_name;
    std::string name;
    std::string node;
    std::vector<ContainerUsage> containers;
    uint64_t window_ms{0};
    uint64_t sample_unix_ms{0};
    uint64_t received_uptime_ms{0};
};

class MetricsRegistry {
public:
    explicit MetricsRegistry(uint32_t stale_after_ms = 30000, size_t max_pods = 32);

    // Kubelet push body:
    // {"node":"n","window_ms":5000,"timestamp":"...","usage":{"cpu_nanocores":N,"memory_bytes":N},
    //  "pods":[{"namespace":"ns","name":"p","containers":[{"name":"c","cpu_nanocores":N,"memory_bytes":N}]}]}
    bool ingest(const std::string& json, std::string& err);

    bool node(const std::string& name, NodeUsageSample& out) const;
    std::vector<NodeUsageSample> nodes() const;
    bool pod(const std::string& ns, const std::string& name, PodUsageSample& out) const;
    std::vector<PodUsageSample> pods(const std::string& ns /* empty = all */) const;
    void forget_pod(const std::string& ns, const std::string& name);

    static std::string node_metrics_json(const NodeUsageSample& s);
    static std::string pod_metrics_json(const PodUsageSample& s);
    static std::string quantity_cpu(uint64_t nanocores);     // "1234567n"
    static std::string quantity_memory(uint64_t bytes);      // "65536Ki" or "1234"

private:
    bool fresh(uint64_t received_uptime_ms, uint64_t now) const;

    uint32_t stale_after_ms_;
    size_t max_pods_;
    mutable osal::Mutex mutex_;
    std::map<std::string, NodeUsageSample> nodes_;
    std::map<std::string, PodUsageSample> pods_; // "ns/name"
};

} // namespace kube32::api
