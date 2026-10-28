#include "kubelet/kubelet.hpp"
#include "hal/capability_firewall.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <functional>
#include <iostream>

namespace kube32::kubelet {

// Pods whose image resolves to nothing fail with ErrImagePull, as on Kubernetes.

Kubelet::Kubelet(const KubeletConfig& config,
                 runtime::Runtime& runtime,
                 std::unique_ptr<KubeApiClient> api_client)
    : config_(config), runtime_(runtime), api_client_(std::move(api_client)) {
    if (!api_client_) {
        auto client = std::make_unique<HttpKubeClient>(config_.api_server_host, config_.api_server_port);
        client->set_node_filter(config_.node_name);
        api_client_ = std::move(client);
    }
    status_reporter_ = std::make_unique<NodeStatusReporter>(config_.node_name, config_.node_ip, config_.control_plane);
    metrics_ = create_platform_metrics_provider();
    // Unique per process lifetime: the mirror resets its duplicate filter when it changes
    log_epoch_ = osal::Thread::get_time_us() ^ (static_cast<uint64_t>(std::hash<std::string>{}(config_.node_name)) << 20);
    if (log_epoch_ == 0) log_epoch_ = 1;
}

static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void Kubelet::record_pod_event(const LocalPodRecord& rec, const char* reason, const std::string& message, bool warning) {
    std::string ev = "{\"kind\":\"Event\",\"apiVersion\":\"v1\",\"metadata\":{\"namespace\":\"" + rec.namespace_name + "\"},"
                     "\"involvedObject\":{\"kind\":\"Pod\",\"namespace\":\"" + rec.namespace_name + "\",\"name\":\"" + rec.name + "\"" +
                     (rec.uid.empty() ? std::string() : ",\"uid\":\"" + rec.uid + "\"") + ",\"apiVersion\":\"v1\"},"
                     "\"reason\":\"" + std::string(reason) + "\",\"message\":\"" + json_escape(message) + "\","
                     "\"type\":\"" + std::string(warning ? "Warning" : "Normal") + "\","
                     "\"reportingComponent\":\"kubelet\",\"reportingInstance\":\"" + config_.node_name + "\","
                     "\"source\":{\"component\":\"kubelet\",\"host\":\"" + config_.node_name + "\"}}";
    api_client_->create_event(rec.namespace_name, ev);
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
    t_cfg.stack_in_psram = config_.task_stack_in_psram;

    worker_thread_ = std::make_unique<osal::Thread>();
    if (!worker_thread_->start(task_entry, this, t_cfg)) return false;

    // Lease renewal must keep flowing while the worker thread compiles and
    // initialises Wasm modules (seconds on a 240 MHz core).
    osal::Thread::Config h_cfg;
    h_cfg.name = "k32_kubelet_hb";
    h_cfg.stack_size = config_.task_stack_in_psram ? 12288 : 10240;  // JSON for lease + status patch (1.7 KB spare at 8 KB on the worker)
    h_cfg.priority = config_.task_priority;
    h_cfg.core_affinity = config_.core_affinity;
    h_cfg.stack_in_psram = config_.task_stack_in_psram;
    heartbeat_thread_ = std::make_unique<osal::Thread>();
    heartbeat_thread_->start(heartbeat_entry, this, h_cfg);
    return true;
}

void Kubelet::heartbeat_entry(void* arg) {
    auto* self = static_cast<Kubelet*>(arg);
    if (self) self->run_heartbeat_loop();
}

void Kubelet::run_heartbeat_loop() {
    while (running_.load()) {
        try {
            if (registered_.load()) {
                uint64_t now = osal::System::get_uptime_ms();
                if (now - last_heartbeat_ms_ >= config_.heartbeat_interval_ms) {
                    report_heartbeat();
                }
            }
        } catch (const std::exception& e) {
            std::cout << "[KUBELET] heartbeat failed: " << e.what() << std::endl;
        }
        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(250);
    }
}

void Kubelet::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (worker_thread_) {
        worker_thread_->join();
        worker_thread_.reset();
    }
    if (heartbeat_thread_) {
        heartbeat_thread_->join();
        heartbeat_thread_.reset();
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
        try {
            if (!registered_.load()) {
                reconcile_node_registration();
            } else {
                uint64_t now = osal::System::get_uptime_ms();

                if (now - last_sync_ms_ >= config_.sync_frequency_ms) {
                    sync_assigned_pods();
                }

                if (now - last_status_report_ms_ >= config_.pod_status_frequency_ms) {
                    monitor_and_report_pod_statuses();
                }
            }
        } catch (const std::exception& e) {
            std::cout << "[KUBELET] loop iteration failed: " << e.what() << " (free heap "
                      << osal::System::get_free_heap_bytes() << ")" << std::endl;
            osal::Thread::sleep_ms(500);
        }

        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(100);
    }
}

void Kubelet::reconcile_node_registration() {
    refresh_node_ip();
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

void Kubelet::refresh_node_ip() {
    if (!config_.node_ip_provider) return;
    std::string ip = config_.node_ip_provider();
    if (ip.empty() || ip == "0.0.0.0" || ip == config_.node_ip) return;
    config_.node_ip = ip;
    status_reporter_->set_node_ip(ip);
    std::cout << "[KUBELET] node address is now " << ip << std::endl;
}

void Kubelet::report_heartbeat() {
    refresh_node_ip();
    uint64_t now_ms = osal::System::get_uptime_ms();
    // renewTime carries the real wall clock, or nothing while it is unset
    api_client_->renew_node_lease(config_.node_name, 40, models::now_rfc3339());

    NodeTelemetry t = status_reporter_->collect_telemetry();
    bool memory_pressure = (t.free_heap_bytes < config_.memory_pressure_threshold_bytes);
    std::string patch = status_reporter_->build_heartbeat_patch_json(t, memory_pressure);
    if (!api_client_->patch_node_status(config_.node_name, patch)) {
        // The control plane lost us (reboot, RAM-only store): register again
        heartbeat_failures_++;
        if (heartbeat_failures_ >= 2) {
            std::cout << "[KUBELET] heartbeat rejected, re-registering node " << config_.node_name << std::endl;
            registered_.store(false);
            heartbeat_failures_ = 0;
            {
                osal::LockGuard guard(state_mutex_);
                // Pods will be re-sent by the control plane; local records stay so
                // running workloads survive the control plane's restart
            }
        }
    } else {
        heartbeat_failures_ = 0;
        push_metrics_sample();
    }
    last_heartbeat_ms_ = now_ms;
}

void Kubelet::sync_assigned_pods() {
    last_sync_ms_ = osal::System::get_uptime_ms();

    std::vector<std::pair<models::KubePod, std::string>> all_pods;
    if (!api_client_->list_pods_raw(all_pods)) {
        return;
    }

    // Filter desired pods assigned to this node; pods carrying a
    // deletionTimestamp are to be stopped and then finalized by us
    std::map<std::string, std::pair<models::KubePod, std::string>> desired;
    std::vector<std::pair<std::string, std::string>> terminating; // (ns, name)
    for (const auto& item : all_pods) {
        const auto& pod = item.first;
        if (pod.node_name == config_.node_name) {
            std::string key = pod.namespace_name + "/" + pod.name;
            if (!pod.deletion_timestamp.empty()) {
                terminating.emplace_back(pod.namespace_name, pod.name);
                continue;
            }
            desired[key] = item;
        }
    }

    osal::LockGuard guard(state_mutex_);

    // 0. Graceful deletions: stop the container, flush its log, then finalize
    for (const auto& tp : terminating) {
        std::string key = tp.first + "/" + tp.second;
        if (active_pods_.find(key) != active_pods_.end()) terminate_pod(key);
        api_client_->delete_pod(tp.first, tp.second);
    }

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

bool Kubelet::deploy_and_start_pod(const models::KubePod& pod_in, const std::string& manifest_json) {
    // Callers may hand us a partially filled struct; the manifest is the truth
    models::KubePod pod = pod_in;
    if (pod.image.empty() && !manifest_json.empty()) {
        models::KubePod parsed = models::parse_pod_manifest(manifest_json);
        pod.image = parsed.image;
        pod.env = parsed.env;
        pod.annotations = parsed.annotations;
        pod.labels = parsed.labels;
        if (pod.memory_limit_bytes == 0) pod.memory_limit_bytes = parsed.memory_limit_bytes;
        if (parsed.memory_request_bytes != 16 * 1024) pod.memory_request_bytes = parsed.memory_request_bytes;
        if (pod.psram_request_bytes == 0) pod.psram_request_bytes = parsed.psram_request_bytes;
    }
    std::string pod_key = pod.namespace_name + "/" + pod.name;

    LocalPodRecord rec;
    rec.name = pod.name;
    rec.namespace_name = pod.namespace_name;
    rec.uid = pod.uid;
    rec.image = pod.image;
    rec.container_name = pod.container_name.empty() ? pod.name : pod.container_name;
    rec.execution_phase = PodExecutionPhase::ContainerCreating;
    // Pods have no network namespace here: the pod answers on the node's
    // address (pod HTTP server), so that is its IP.
    rec.pod_ip = config_.node_ip;
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
        record_pod_event(rec, "Failed", "Error: capability admission rejected: " + firewall_err, true);
        return false;
    }

    // Resolve the container image: embedded registry first, inline annotation second
    const uint8_t* image_bytes = nullptr;
    size_t image_len = 0;
    std::vector<uint8_t> inline_bytes;
    std::string image_key;
    const runtime::EmbeddedImage* img = runtime::ImageRegistry::instance().resolve(pod.image, &image_key);
    if (img) {
        image_bytes = img->data;
        image_len = img->len;
    } else {
        auto ann = pod.annotations.find("kube32.io/wasm-base64");
        if (ann != pod.annotations.end() && runtime::base64_decode(ann->second, inline_bytes)) {
            image_bytes = inline_bytes.data();
            image_len = inline_bytes.size();
        }
    }

    auto fail_pod = [&](const std::string& reason, const std::string& message, int exit_code) {
        rec.execution_phase = PodExecutionPhase::Failed;
        rec.last_error_message = message;
        active_pods_[pod_key] = rec;
        hal::CapabilityFirewall::instance().unregister_pod(pod_key);
        std::string status_patch;
        status_patch.reserve(512);
        status_patch += "{\"status\":{";
        status_patch += "\"phase\":\"Failed\",";
        status_patch += "\"reason\":\"" + reason + "\",";
        status_patch += "\"message\":\"" + message + "\",";
        status_patch += "\"containerStatuses\":[{";
        status_patch += "\"name\":\"" + pod.name + "\",";
        status_patch += "\"image\":\"" + pod.image + "\",";
        status_patch += "\"ready\":false,";
        status_patch += "\"restartCount\":0,";
        status_patch += "\"state\":{\"waiting\":{\"reason\":\"" + reason + "\",\"message\":\"" + message + "\"}}";
        status_patch += "}]";
        status_patch += "}}";
        api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
        (void)exit_code;
        return false;
    };

    if (!image_bytes || image_len == 0) {
        std::cout << "[KUBELET] " << pod_key << ": image '" << pod.image << "' (key '" << image_key
                  << "') not found in local registry" << std::endl;
        record_pod_event(rec, "Failed", "Failed to pull image \"" + pod.image + "\": not present in the node image registry", true);
        return fail_pod("ErrImagePull", "image " + pod.image + " not present in the node image registry", 1);
    }
    record_pod_event(rec, "Pulled", "Container image \"" + pod.image + "\" already present on machine (" +
                                    std::to_string(image_len) + " bytes" + (inline_bytes.empty() ? ", embedded registry)" : ", inline annotation)"), false);

    // Wasm linear memory budget: an explicit PSRAM request (esp32.io/psram) is
    // the sandbox size on PSRAM nodes; otherwise limits.memory, then
    // requests.memory, then one 64 KiB page.
    uint64_t mem = pod.memory_limit_bytes > 0 ? pod.memory_limit_bytes : pod.memory_request_bytes;
    if (pod.psram_request_bytes > mem) mem = pod.psram_request_bytes;
    if (mem < 64 * 1024) mem = 64 * 1024;
    rec.memory_limit_bytes = static_cast<uint32_t>(mem);

    std::string deploy_err;
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = rec.memory_limit_bytes;
    spec.stack_size_bytes = 16 * 1024;
    spec.env = pod.env;
    bool deployed = runtime_.deploy_workload_ex(pod_key, image_bytes, image_len, spec, deploy_err);
    if (!deployed) {
        std::cout << "[KUBELET] " << pod_key << ": deploy failed: " << deploy_err << std::endl;
        record_pod_event(rec, "Failed", "Error: failed to create container " + rec.container_name + ": " + deploy_err, true);
        return fail_pod("CrashLoopBackOff", deploy_err, 1);
    }
    record_pod_event(rec, "Created", "Created container " + rec.container_name, false);
    if (!runtime_.start(pod_key)) {
        // Typically the pod thread's stack could not be allocated; say so
        // instead of reporting a container that does not exist as Running.
        std::string why = "failed to start container " + rec.container_name + ": cannot create pod thread (out of memory)";
        std::cout << "[KUBELET] " << pod_key << ": " << why << std::endl;
        record_pod_event(rec, "Failed", "Error: " + why, true);
        runtime_.terminate(pod_key);
        return fail_pod("CrashLoopBackOff", why, 1);
    }
    std::cout << "[KUBELET] " << pod_key << ": running image " << image_key << " (" << image_len
              << " bytes, limit " << rec.memory_limit_bytes / 1024 << " KiB)" << std::endl;
    record_pod_event(rec, "Started", "Started container " + rec.container_name, false);

    rec.execution_phase = PodExecutionPhase::Running;
    rec.started_at_ms = osal::System::get_uptime_ms();
    active_pods_[pod_key] = rec;

    // Report status to API server. Times come from the wall clock and are
    // omitted while it is unset.
    const std::string now = models::now_rfc3339();
    std::string status_patch;
    status_patch.reserve(512);
    status_patch += "{\"status\":{";
    status_patch += "\"phase\":\"Running\",";
    status_patch += "\"podIP\":\"" + rec.pod_ip + "\",";
    status_patch += "\"hostIP\":\"" + config_.node_ip + "\",";
    if (!now.empty()) status_patch += "\"startTime\":\"" + now + "\",";
    status_patch += "\"conditions\":[";
    status_patch += "{\"type\":\"PodScheduled\",\"status\":\"True\"},";
    status_patch += "{\"type\":\"Initialized\",\"status\":\"True\"},";
    status_patch += "{\"type\":\"ContainersReady\",\"status\":\"True\"},";
    status_patch += "{\"type\":\"Ready\",\"status\":\"True\"}],";
    status_patch += "\"containerStatuses\":[{";
    status_patch += "\"name\":\"" + rec.container_name + "\",";
    status_patch += "\"image\":\"" + pod.image + "\",";
    status_patch += "\"ready\":true,";
    status_patch += "\"started\":true,";
    status_patch += "\"restartCount\":0,";
    status_patch += "\"state\":{\"running\":{" + (now.empty() ? std::string() : "\"startedAt\":\"" + now + "\"") + "}}";
    status_patch += "}]";
    status_patch += "}}";

    api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
    return true;
}

bool Kubelet::terminate_pod(const std::string& pod_key) {
    std::cout << "[KUBELET] " << pod_key << ": terminating" << std::endl;
    auto it = active_pods_.find(pod_key);
    if (it != active_pods_.end()) {
        record_pod_event(it->second, "Killing", "Stopping container " + it->second.container_name, false);
    }
    runtime_.terminate(pod_key);
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
    if (it != active_pods_.end()) {
        // The "container stopped" line is the ring's last record; flush it
        ship_pod_logs_locked(pod_key, it->second);
        active_pods_.erase(it);
    }
    runtime::PodLogStore::runtime().remove(pod_key);
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
    const std::string now = models::now_rfc3339();

    std::string status_patch;
    status_patch.reserve(512);
    status_patch += "{\"status\":{";
    status_patch += "\"phase\":\"Failed\",";
    status_patch += "\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"},"
                    "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"}],";
    status_patch += "\"containerStatuses\":[{";
    status_patch += "\"name\":\"" + (rec.container_name.empty() ? rec.name : rec.container_name) + "\",";
    status_patch += "\"ready\":false,";
    status_patch += "\"started\":false,";
    status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
    status_patch += "\"state\":{\"terminated\":{\"exitCode\":" + std::to_string(exit_code) + ",\"reason\":\"" + reason + "\"," +
                    "\"message\":\"" + json_escape(rec.last_error_message) + "\"" +
                    (now.empty() ? std::string() : ",\"finishedAt\":\"" + now + "\"") + "}}";
    status_patch += "}]";
    status_patch += "}}";

    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
    record_pod_event(rec, "Failed", "Container " + (rec.container_name.empty() ? rec.name : rec.container_name) +
                                    " terminated: " + reason + " (exit code " + std::to_string(exit_code) + "): " + rec.last_error_message, true);
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
        ship_pod_logs_locked(pod_key, rec);
    }
}

void Kubelet::ship_pod_logs() {
    osal::LockGuard guard(state_mutex_);
    for (auto& kv : active_pods_) ship_pod_logs_locked(kv.first, kv.second);
}

void Kubelet::ship_pod_logs_locked(const std::string& pod_key, LocalPodRecord& rec) {
    std::vector<runtime::PodLogRecord> recs;
    if (!runtime::PodLogStore::runtime().read_after(pod_key, rec.shipped_log_seq, recs, 64) || recs.empty()) return;
    std::string body;
    body.reserve(recs.size() * 64);
    for (const auto& r : recs) {
        body += "{\"epoch\":" + std::to_string(log_epoch_) + ",\"seq\":" + std::to_string(r.seq) +
                ",\"t\":" + std::to_string(r.unix_ms) + ",\"u\":" + std::to_string(r.uptime_ms) +
                ",\"m\":\"" + json_escape(r.text) + "\"}\n";
    }
    if (api_client_->push_pod_logs(rec.namespace_name, rec.name, body)) {
        rec.shipped_log_seq = recs.back().seq;
    }
}

void Kubelet::push_metrics_sample() {
    if (!metrics_) return;
    NodeResourceSample node;
    bool have_node = metrics_->sample_node(node);
    const uint64_t wall_now_us = osal::Thread::get_time_us();

    std::string body = "{\"node\":\"" + config_.node_name + "\"";
    uint64_t window_ms = 0;
    std::string usage;
    if (have_node) {
        if (node.has_cpu && has_node_sample_ && last_node_sample_.has_cpu && node.cpu_wall_us > last_node_sample_.cpu_wall_us) {
            uint64_t dbusy = node.cpu_busy_us - last_node_sample_.cpu_busy_us;
            uint64_t dwall = node.cpu_wall_us - last_node_sample_.cpu_wall_us;
            uint64_t nanocores = (dbusy * 1000000000ULL) / dwall;
            usage += "\"cpu_nanocores\":" + std::to_string(nanocores);
            window_ms = dwall / 1000ULL;
        }
        if (node.has_memory) {
            if (!usage.empty()) usage += ",";
            usage += "\"memory_bytes\":" + std::to_string(node.memory_used_bytes);
        }
        last_node_sample_ = node;
        has_node_sample_ = true;
    }
    if (window_ms == 0) window_ms = (last_heartbeat_ms_ > 0) ? (osal::System::get_uptime_ms() - last_heartbeat_ms_) : 0;
    std::string ts = models::now_rfc3339();
    if (!ts.empty()) body += ",\"timestamp\":\"" + ts + "\"";
    body += ",\"window_ms\":" + std::to_string(window_ms);
    body += ",\"usage\":{" + usage + "}";

    body += ",\"pods\":[";
    bool first = true;
    {
        osal::LockGuard guard(state_mutex_);
        for (auto& kv : active_pods_) {
            LocalPodRecord& rec = kv.second;
            PodResourceSample ps;
            if (!metrics_->sample_pod(kv.first, runtime_, ps)) continue;
            std::string cu;
            if (ps.has_cpu && rec.has_cpu_sample && wall_now_us > rec.last_cpu_wall_us) {
                uint64_t dbusy = ps.cpu_busy_us >= rec.last_cpu_busy_us ? ps.cpu_busy_us - rec.last_cpu_busy_us : 0;
                uint64_t dwall = wall_now_us - rec.last_cpu_wall_us;
                cu += "\"cpu_nanocores\":" + std::to_string((dbusy * 1000000000ULL) / dwall);
            }
            if (ps.has_cpu) {
                rec.has_cpu_sample = true;
                rec.last_cpu_busy_us = ps.cpu_busy_us;
                rec.last_cpu_wall_us = wall_now_us;
            }
            if (ps.has_memory) {
                if (!cu.empty()) cu += ",";
                cu += "\"memory_bytes\":" + std::to_string(ps.memory_bytes);
                if (ps.has_psram) cu += ",\"psram_bytes\":" + std::to_string(ps.psram_bytes);
            }
            if (!first) body += ",";
            first = false;
            body += "{\"namespace\":\"" + rec.namespace_name + "\",\"name\":\"" + rec.name + "\",\"containers\":[{\"name\":\"" +
                    (rec.container_name.empty() ? rec.name : rec.container_name) + "\"" + (cu.empty() ? std::string() : "," + cu) + "}]}";
        }
    }
    body += "]}";
    api_client_->push_metrics(config_.node_name, body);
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
