#pragma once

#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/metrics_provider.hpp"
#include "runtime/wasm_runtime.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <functional>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <cstdint>

namespace kube32::kubelet {

enum class PodExecutionPhase {
    Pending,
    ContainerCreating,
    Running,
    CrashLoopBackOff,
    Terminating,
    Completed,
    Failed
};

struct LocalPodRecord {
    std::string uid;
    std::string name;
    std::string namespace_name;
    std::string container_name;
    std::string image;
    PodExecutionPhase execution_phase{PodExecutionPhase::Pending};
    uint32_t restart_count{0};
    uint64_t started_at_ms{0};
    uint64_t next_restart_allowed_ms{0};
    std::string last_error_message;
    std::string pod_ip;
    uint32_t memory_limit_bytes{64 * 1024};
    uint64_t requested_gpio_mask{0};
    // Log shipping and metrics rate state
    uint64_t shipped_log_seq{0};
    bool has_cpu_sample{false};
    uint64_t last_cpu_busy_us{0};
    uint64_t last_cpu_wall_us{0};
};

struct KubeletConfig {
    std::string node_name{"esp32-node-01"};
    std::string api_server_host{"127.0.0.1"};
    uint16_t api_server_port{6443};
    std::string node_ip{"127.0.0.1"};
    std::function<std::string()> node_ip_provider;   // re-read before registering/heartbeating (DHCP may arrive late)
    bool control_plane{false};
    
    // Cadence intervals in milliseconds
    uint32_t sync_frequency_ms{1000};
    uint32_t heartbeat_interval_ms{5000};
    uint32_t pod_status_frequency_ms{2000};
    uint32_t memory_pressure_threshold_bytes{24576}; // 24 KiB DRAM threshold

    // Task & concurrency configuration
    size_t task_stack_size{65536};
    bool task_stack_in_psram{false};
    uint32_t task_priority{5};
    int core_affinity{1}; // Core 1
};

class Kubelet {
public:
    Kubelet(const KubeletConfig& config,
            runtime::Runtime& runtime,
            std::unique_ptr<KubeApiClient> api_client = nullptr);
    ~Kubelet();

    // Lifecycle
    bool init();
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Core reconciler loops (invoked sequentially by the main task loop)
    void reconcile_node_registration();
    void refresh_node_ip();
    void report_heartbeat();
    void sync_assigned_pods();
    void monitor_and_report_pod_statuses();

    // Query local state
    const KubeletConfig& config() const { return config_; }
    std::vector<std::string> get_active_pod_keys() const;
    bool get_pod_record(const std::string& pod_key, LocalPodRecord& out_record) const;

    // Ships new log records of every local pod to the API server (called from
    // the status loop; public for tests).
    void ship_pod_logs();
    // Measures node and pod usage and pushes the sample (called with the heartbeat).
    void push_metrics_sample();
    void set_metrics_provider(std::unique_ptr<MetricsProvider> provider) { metrics_ = std::move(provider); }

private:
    static void task_entry(void* arg);
    static void heartbeat_entry(void* arg);
    void run_loop();
    void run_heartbeat_loop();

    // Pod lifecycle actions
    bool deploy_and_start_pod(const models::KubePod& pod, const std::string& manifest_json);
    bool terminate_pod(const std::string& pod_key);
    void handle_pod_failure(LocalPodRecord& rec, const runtime::ContainerStatus& cs);
    void handle_pod_failure(LocalPodRecord& rec, const std::string& exit_msg, int32_t exit_code = 137, const std::string& reason = "ContainerFailed");
    void record_pod_event(const LocalPodRecord& rec, const char* reason, const std::string& message, bool warning);
    void ship_pod_logs_locked(const std::string& pod_key, LocalPodRecord& rec);

    KubeletConfig config_;
    runtime::Runtime& runtime_;
    std::unique_ptr<KubeApiClient> api_client_;
    std::unique_ptr<NodeStatusReporter> status_reporter_;
    std::unique_ptr<MetricsProvider> metrics_;
    uint64_t log_epoch_{0};               // identifies this kubelet's process lifetime to the mirror
    bool has_node_sample_{false};
    NodeResourceSample last_node_sample_;

    std::unique_ptr<osal::Thread> worker_thread_;
    std::unique_ptr<osal::Thread> heartbeat_thread_;
    mutable osal::Mutex state_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> registered_{false};

    // Tracking timers
    uint64_t last_heartbeat_ms_{0};
    uint64_t last_sync_ms_{0};
    uint64_t last_status_report_ms_{0};
    uint32_t heartbeat_failures_{0};

    // Local state map: key = namespace/name
    std::map<std::string, LocalPodRecord> active_pods_;
};

} // namespace kube32::kubelet
