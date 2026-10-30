#pragma once

#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/metrics_provider.hpp"
#include "runtime/wasm_runtime.hpp"
#include "net/network_policy.hpp"
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
    CreateContainerConfigError,   // env references could not be resolved; retried after next_restart_allowed_ms
    InsufficientMemory,           // the node cannot give the pod its memory now; retried after next_restart_allowed_ms
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
    std::string restart_policy{"Always"};    // spec.restartPolicy
    uint32_t restart_count{0};               // containers restarted so far (status restartCount)
    uint32_t crash_streak{0};                // restarts since the container last ran 10 minutes
    uint64_t started_at_ms{0};
    uint64_t next_restart_allowed_ms{0};     // end of the CrashLoopBackOff delay, or of the wait before a config retry
    bool restart_due{false};                 // a container exited and the next start is a restart
    // Last terminated container, reported as lastState after a restart
    int32_t last_exit_code{0};
    std::string last_reason;
    std::string last_finished_at;
    std::string last_error_message;
    std::string pod_ip;
    uint32_t memory_limit_bytes{64 * 1024};
    // What admission estimated the container needs, when it waits for memory:
    // a retry is only attempted once that much room exists
    uint32_t memory_need_bytes{0};
    uint64_t last_wait_log_ms{0};            // when a retry that still had no room was last logged
    // Module loads that failed for memory in a row, and whether smaller pods
    // were already stopped once to let this one load (repack_pool)
    uint8_t load_failures{0};
    bool repacked{false};
    uint64_t requested_gpio_mask{0};
    uint32_t listed_pass{0};                 // the last pod sync pass whose list held this pod
    // Log shipping and metrics rate state
    uint64_t shipped_log_seq{0};
    uint64_t log_instance_start_seq{0};      // runtime log lines up to this seq were written before the last (re)start
    bool has_cpu_sample{false};
    uint64_t last_cpu_busy_us{0};
    uint64_t last_cpu_wall_us{0};
};

struct KubeletConfig {
    std::string node_name;                 // DNS-1123 label, unique in the cluster
    std::string api_server_host{"127.0.0.1"};
    uint16_t api_server_port{6443};
    std::string api_token;                 // bearer token for the API server, empty when it has none
    size_t api_max_body{0};                // largest API response kept in memory (0: no limit)
    // Labels the node sets on its Node object besides kubernetes.io/hostname,
    // kubernetes.io/arch and the role label (for example kube32.io/board).
    std::map<std::string, std::string> node_labels;
    // Labels the node finds on itself (attached hardware), read before
    // registering and every node_label_refresh_ms after; changes are patched
    // onto the Node: new and changed labels set, vanished ones removed
    std::function<std::map<std::string, std::string>()> node_label_source;
    uint32_t node_label_refresh_ms{10000};
    // Key prefix of the labels node_label_source owns: when the Node outlived
    // this kubelet (a reboot), its labels with the prefix that are no longer
    // found are removed
    std::string node_label_prefix;
    std::string node_ip{"127.0.0.1"};
    // Port of this node's kubelet endpoint (the pod HTTP server: node storage
    // through the API server's node proxy), 0 when the node runs none
    uint16_t kubelet_port{0};
    std::function<std::string()> node_ip_provider;   // re-read before registering/heartbeating (DHCP may arrive late)
    bool control_plane{false};
    
    // Cadence intervals in milliseconds
    uint32_t sync_frequency_ms{1000};
    uint32_t heartbeat_interval_ms{5000};
    uint32_t pod_status_frequency_ms{2000};
    uint32_t memory_pressure_threshold_bytes{24576}; // 24 KiB DRAM threshold
    // The node reports Ready only while the pod sync loop keeps completing:
    // NotReady once the last completed pass is older than this.
    uint32_t sync_stall_threshold_ms{60000};
    // Restart back-off (restartPolicy Always/OnFailure): initial delay,
    // doubled per consecutive restart up to the maximum, as in Kubernetes.
    uint32_t restart_backoff_initial_ms{10000};
    uint32_t restart_backoff_max_ms{300000};
    // A pod the node has no memory for yet waits (Pending, InsufficientMemory)
    // and is admitted again after this long
    uint32_t memory_retry_ms{5000};
    // Workers enforce NetworkPolicy egress on their pods' calls with this
    // view (null: none kept here). Each pod sync records the labels of the
    // node's pods and loads the policies of a namespace new to the node;
    // every network_policy_refresh_ms the policies of all its namespaces are
    // listed again.
    std::shared_ptr<net::NodeNetworkPolicies> network_policies;
    uint32_t network_policy_refresh_ms{5000};

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
    void refresh_node_labels();
    void drop_stale_node_labels();
    void report_heartbeat();
    void sync_assigned_pods();
    void monitor_and_report_pod_statuses();
    void handle_pod_completion(LocalPodRecord& rec, const runtime::ContainerStatus& cs);

    // Query local state
    const KubeletConfig& config() const { return config_; }
    std::vector<std::string> get_active_pod_keys() const;
    bool get_pod_record(const std::string& pod_key, LocalPodRecord& out_record) const;
    // Uptime of the last pod sync pass that listed pods from the API server, 0 before the first
    uint64_t last_sync_ok_ms() const { return last_sync_ok_ms_.load(); }

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

    // Fills the Ready condition from the health of the pod sync loop.
    void fill_ready_condition(NodeTelemetry& t) const;

    // Pod lifecycle actions. `previous` carries restart state when the pod
    // is being restarted after a back-off.
    bool deploy_and_start_pod(const models::KubePod& pod, const std::string& manifest_json, const LocalPodRecord* previous = nullptr);
    // deploy_and_start_pod, with running out of memory half way turned into
    // waiting for memory
    void start_pod_guarded(const models::KubePod& pod, const std::string& manifest_json, const LocalPodRecord* previous);
    // The container ended (exit, trap or failed start): restart it after a
    // back-off when restartPolicy asks for it, otherwise report the pod
    // Succeeded or Failed. Releases the container either way.
    void handle_container_exit(const std::string& pod_key, LocalPodRecord& rec, int32_t exit_code,
                               const std::string& reason, const std::string& message);
    bool terminate_pod(const std::string& pod_key);
    void handle_pod_failure(LocalPodRecord& rec, const runtime::ContainerStatus& cs);
    void handle_pod_failure(LocalPodRecord& rec, const std::string& exit_msg, int32_t exit_code = 137, const std::string& reason = "ContainerFailed");
    void record_pod_event(const LocalPodRecord& rec, const char* reason, const std::string& message, bool warning);
    // The pod cannot start for lack of memory now: it stays Pending with the
    // reason in its container status and is admitted again after
    // memory_retry_ms. Status and Event go out when it starts waiting.
    bool wait_for_memory(const std::string& pod_key, LocalPodRecord& rec, const std::string& why, const LocalPodRecord* previous);
    void repack_pool(uint64_t now_ms);
    void ship_pod_logs_locked(const std::string& pod_key, LocalPodRecord& rec);
    // Lists the NetworkPolicies of the namespaces new to config_.network_policies,
    // and of all of them once the refresh interval passed
    void refresh_network_policies(uint64_t now_ms);

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
    uint64_t last_label_refresh_ms_{0};
    std::map<std::string, std::string> found_labels_;   // from node_label_source, as on the Node
    uint32_t heartbeat_failures_{0};
    // The Node object is written here, allocated at start while the heap is
    // whole: re-registering must work when the heap is fragmented
    static constexpr size_t kRegistrationBufBytes = 2048;
    std::unique_ptr<char[]> registration_buf_;
    std::atomic<uint64_t> last_sync_ok_ms_{0};     // end of the last pod sync pass that listed pods
    std::atomic<bool> heartbeat_due_{false};       // send the next heartbeat without waiting

    // Local state map: key = namespace/name (looked up by view)
    std::map<std::string, LocalPodRecord, std::less<>> active_pods_;
    uint32_t sync_pass_{0};
    uint64_t last_policy_refresh_ms_{0};
};

} // namespace kube32::kubelet
