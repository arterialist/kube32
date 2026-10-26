#include "kubelet/kubelet.hpp"
#include "hal/capability_firewall.hpp"
#include "osal/system.hpp"

#include <iostream>

namespace kube32::kubelet {

// Minimal valid WebAssembly binary header (Magic: \0asm, Version: 1)
static const uint8_t s_default_wasm_binary[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
};

Kubelet::Kubelet(const KubeletConfig& config,
                 runtime::Runtime& runtime,
                 std::unique_ptr<KubeApiClient> api_client)
    : config_(config), runtime_(runtime), api_client_(std::move(api_client)) {
    if (!api_client_) {
        api_client_ = std::make_unique<HttpKubeClient>(config_.api_server_host, config_.api_server_port);
    }
    status_reporter_ = std::make_unique<NodeStatusReporter>(config_.node_name, config_.node_ip);
}

Kubelet::~Kubelet() {
    stop();
}

bool Kubelet::init() {
    return true;
}

bool Kubelet::start() {
    if (running_.load()) return true;

    running_.store(true);
    osal::Thread::Config t_cfg;
    t_cfg.name = "k32_kubelet";
    t_cfg.stack_size = config_.task_stack_size;
    t_cfg.priority = config_.task_priority;
    t_cfg.core_affinity = config_.core_affinity;

    worker_thread_ = std::make_unique<osal::Thread>();
    return worker_thread_->start(task_entry, this, t_cfg);
}

void Kubelet::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (worker_thread_) {
        worker_thread_->join();
        worker_thread_.reset();
    }
}

void Kubelet::task_entry(void* arg) {
    auto* self = static_cast<Kubelet*>(arg);
    if (self) {
        self->run_loop();
    }
}

void Kubelet::run_loop() {
    while (running_.load()) {
        if (!registered_.load()) {
            reconcile_node_registration();
        } else {
            uint64_t now = osal::System::get_uptime_ms();

            if (now - last_heartbeat_ms_ >= config_.heartbeat_interval_ms) {
                report_heartbeat();
            }

            if (now - last_sync_ms_ >= config_.sync_frequency_ms) {
                sync_assigned_pods();
            }

            if (now - last_status_report_ms_ >= config_.pod_status_frequency_ms) {
                monitor_and_report_pod_statuses();
            }
        }

        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(100);
    }
}

void Kubelet::reconcile_node_registration() {
    if (registered_.load()) return;

    NodeTelemetry t = status_reporter_->collect_telemetry();
    std::string reg_json = status_reporter_->build_registration_json(t);

    bool conflict = false;
    bool ok = api_client_->register_node(reg_json, conflict);
    if (ok) {
        registered_.store(true);
        report_heartbeat();
        last_heartbeat_ms_ = osal::System::get_uptime_ms();
    }
}

void Kubelet::report_heartbeat() {
    uint64_t now_ms = osal::System::get_uptime_ms();
    char time_buf[64];
    snprintf(time_buf, sizeof(time_buf), "2026-10-26T%02u:%02u:%02u.%03uZ",
             static_cast<unsigned>((now_ms / 3600000) % 24),
             static_cast<unsigned>((now_ms / 60000) % 60),
             static_cast<unsigned>((now_ms / 1000) % 60),
             static_cast<unsigned>(now_ms % 1000));

    api_client_->renew_node_lease(config_.node_name, 40, time_buf);

    NodeTelemetry t = status_reporter_->collect_telemetry();
    bool memory_pressure = (t.free_heap_bytes < config_.memory_pressure_threshold_bytes);
    std::string patch = status_reporter_->build_heartbeat_patch_json(t, memory_pressure);
    api_client_->patch_node_status(config_.node_name, patch);
    last_heartbeat_ms_ = now_ms;
}

void Kubelet::sync_assigned_pods() {
    last_sync_ms_ = osal::System::get_uptime_ms();

    std::vector<std::pair<models::KubePod, std::string>> all_pods;
    if (!api_client_->list_pods_raw(all_pods)) {
        return;
    }

    // Filter desired pods assigned to this node
    std::map<std::string, std::pair<models::KubePod, std::string>> desired;
    for (const auto& item : all_pods) {
        const auto& pod = item.first;
        if (pod.node_name == config_.node_name) {
            std::string key = pod.namespace_name + "/" + pod.name;
            desired[key] = item;
        }
    }

    osal::LockGuard guard(state_mutex_);

    // 1. Detect Additions
    for (const auto& kv : desired) {
        const std::string& key = kv.first;
        if (active_pods_.find(key) == active_pods_.end()) {
            deploy_and_start_pod(kv.second.first, kv.second.second);
        }
    }

    // 2. Detect Deletions
    std::vector<std::string> to_remove;
    for (const auto& kv : active_pods_) {
        const std::string& key = kv.first;
        if (desired.find(key) == desired.end()) {
            to_remove.push_back(key);
        }
    }

    for (const auto& key : to_remove) {
        terminate_pod(key);
    }
}

bool Kubelet::deploy_and_start_pod(const models::KubePod& pod, const std::string& manifest_json) {
    std::string pod_key = pod.namespace_name + "/" + pod.name;

    LocalPodRecord rec;
    rec.name = pod.name;
    rec.namespace_name = pod.namespace_name;
    rec.execution_phase = PodExecutionPhase::ContainerCreating;
    rec.pod_ip = "10.244.0." + std::to_string((active_pods_.size() + 2) & 0xFF);
    rec.memory_limit_bytes = (pod.memory_request_bytes > 0) ? pod.memory_request_bytes : (64 * 1024);

    // Register capabilities with firewall
    std::string firewall_err;
    if (!hal::CapabilityFirewall::instance().register_pod(pod_key, manifest_json, firewall_err)) {
        rec.execution_phase = PodExecutionPhase::Failed;
        rec.last_error_message = "Capability Admission Rejected: " + firewall_err;
        active_pods_[pod_key] = rec;

        std::string status_patch;
        status_patch.reserve(512);
        status_patch += "{\"status\":{";
        status_patch += "\"phase\":\"Failed\",";
        status_patch += "\"reason\":\"CapabilityAdmissionFailed\",";
        status_patch += "\"message\":\"Capability Admission Rejected: " + firewall_err + "\",";
        status_patch += "\"containerStatuses\":[{";
        status_patch += "\"name\":\"" + pod.name + "\",";
        status_patch += "\"ready\":false,";
        status_patch += "\"restartCount\":0,";
        status_patch += "\"state\":{\"terminated\":{\"exitCode\":1,\"reason\":\"CapabilityAdmissionFailed\"}}";
        status_patch += "}]";
        status_patch += "}}";

        api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
        return false;
    }

    // Deploy and start runtime
    runtime_.deploy_workload(pod_key, s_default_wasm_binary, sizeof(s_default_wasm_binary), rec.memory_limit_bytes);
    runtime_.start(pod_key);

    rec.execution_phase = PodExecutionPhase::Running;
    rec.started_at_ms = osal::System::get_uptime_ms();
    active_pods_[pod_key] = rec;

    // Report status to API server
    std::string status_patch;
    status_patch.reserve(512);
    status_patch += "{\"status\":{";
    status_patch += "\"phase\":\"Running\",";
    status_patch += "\"podIP\":\"" + rec.pod_ip + "\",";
    status_patch += "\"hostIP\":\"" + config_.node_ip + "\",";
    status_patch += "\"startTime\":\"2026-10-26T10:00:00Z\",";
    status_patch += "\"containerStatuses\":[{";
    status_patch += "\"name\":\"" + pod.name + "\",";
    status_patch += "\"ready\":true,";
    status_patch += "\"restartCount\":0,";
    status_patch += "\"state\":{\"running\":{\"startedAt\":\"2026-10-26T10:00:00Z\"}}";
    status_patch += "}]";
    status_patch += "}}";

    api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
    return true;
}

bool Kubelet::terminate_pod(const std::string& pod_key) {
    runtime_.terminate(pod_key);
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
    active_pods_.erase(pod_key);
    return true;
}

void Kubelet::handle_pod_failure(LocalPodRecord& rec, const runtime::ContainerStatus& cs) {
    rec.execution_phase = PodExecutionPhase::Failed;
    rec.last_error_message = !cs.exit_message.empty() ? cs.exit_message : cs.termination_reason;
    if (cs.restart_count > static_cast<int>(rec.restart_count)) {
        rec.restart_count = static_cast<uint32_t>(cs.restart_count);
    } else {
        rec.restart_count++;
    }

    int32_t exit_code = (cs.exit_code != 0) ? cs.exit_code : 137;
    std::string reason = !cs.termination_reason.empty() ? cs.termination_reason : "RuntimeTrap";

    std::string status_patch;
    status_patch.reserve(512);
    status_patch += "{\"status\":{";
    status_patch += "\"phase\":\"Failed\",";
    status_patch += "\"containerStatuses\":[{";
    status_patch += "\"name\":\"" + rec.name + "\",";
    status_patch += "\"ready\":false,";
    status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
    status_patch += "\"state\":{\"terminated\":{\"exitCode\":" + std::to_string(exit_code) + ",\"reason\":\"" + reason + "\"}}";
    status_patch += "}]";
    status_patch += "}}";

    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
}

void Kubelet::handle_pod_failure(LocalPodRecord& rec, const std::string& exit_msg, int32_t exit_code, const std::string& reason) {
    runtime::ContainerStatus cs;
    cs.phase = runtime::ContainerPhase::Failed;
    cs.exit_message = exit_msg;
    cs.exit_code = exit_code;
    cs.termination_reason = reason;
    handle_pod_failure(rec, cs);
}

void Kubelet::monitor_and_report_pod_statuses() {
    last_status_report_ms_ = osal::System::get_uptime_ms();

    osal::LockGuard guard(state_mutex_);
    for (auto& kv : active_pods_) {
        const std::string& pod_key = kv.first;
        LocalPodRecord& rec = kv.second;

        runtime::ContainerStatus cs = runtime_.get_status(pod_key);
        if (cs.phase == runtime::ContainerPhase::Failed && rec.execution_phase != PodExecutionPhase::Failed) {
            handle_pod_failure(rec, cs);
        }
    }
}

std::vector<std::string> Kubelet::get_active_pod_keys() const {
    osal::LockGuard guard(state_mutex_);
    std::vector<std::string> keys;
    keys.reserve(active_pods_.size());
    for (const auto& kv : active_pods_) {
        keys.push_back(kv.first);
    }
    return keys;
}

bool Kubelet::get_pod_record(const std::string& pod_key, LocalPodRecord& out_record) const {
    osal::LockGuard guard(state_mutex_);
    auto it = active_pods_.find(pod_key);
    if (it != active_pods_.end()) {
        out_record = it->second;
        return true;
    }
    return false;
}

} // namespace kube32::kubelet
