#include "kubelet/kubelet.hpp"
#include "kubelet/pod_env.hpp"
#include <cstdlib>
#include "hal/capability_firewall.hpp"
#include "hal/sd_card.hpp"
#include "hal/node_storage.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/pod_memory_pool.h"
#include "models/kube_models.hpp"
#include "models/kube_lease.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <new>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kube32::kubelet {

// Pods whose image resolves to nothing fail with ErrImagePull, as on Kubernetes.

static bool pod_phase_is_terminal(const models::KubePod& pod) {
    return pod.phase == "Succeeded" || pod.phase == "Failed";
}

static std::string kib(size_t bytes) {
    return std::to_string((bytes + 1023) / 1024) + " KiB";
}

// Largest module the kubelet pulls from the card into RAM: a board without
// PSRAM has about 90 KB free at steady state, so anything bigger than 256 KiB
// can never be placed and is refused up front with a clear reason.
static uint64_t sd_image_limit_bytes() {
#if defined(ESP_PLATFORM)
    return (osal::System::get_total_psram_bytes() > 0) ? (2u * 1024u * 1024u) : (256u * 1024u);
#else
    return 2u * 1024u * 1024u;
#endif
}

// Reads <sd root>/kube32/images/<name>[-<tag>].wasm into `out` (sized from
// the file, filled 4 KiB at a time). On failure `why` says what happened.
static bool pull_sd_image(const std::string& name, const std::string& tag, std::vector<uint8_t>& out, std::string& out_path, std::string& why) {
    out.clear();
    std::string root = hal::sd_root_path();
    if (root.empty()) {
        std::string reason = hal::sd_info().error;
        why = "no sd card mounted on this node" + (reason.empty() ? std::string() : " (" + reason + ")");
        return false;
    }
    out_path = root + "/" + runtime::sd_image_relative_path(name, tag);
    struct stat st;
    if (::stat(out_path.c_str(), &st) != 0) {
        why = out_path + " not found on the sd card";
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        why = out_path + " is not a regular file or is empty";
        return false;
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    const uint64_t limit = sd_image_limit_bytes();
    if (size > limit) {
        why = out_path + " is " + std::to_string(size) + " bytes, over the " + std::to_string(limit / 1024) + " KiB limit of this board" +
              (limit < (1024u * 1024u) ? " (no PSRAM)" : "");
        return false;
    }
    try {
        out.resize(static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
        why = "cannot allocate " + std::to_string(size) + " bytes for " + out_path + " (free heap " +
              std::to_string(osal::System::get_free_heap_bytes()) + ")";
        return false;
    }
    int fd = ::open(out_path.c_str(), O_RDONLY);
    if (fd < 0) {
        why = out_path + ": open failed (errno " + std::to_string(errno) + ")";
        out.clear(); out.shrink_to_fit();
        return false;
    }
    size_t got = 0;
    while (got < out.size()) {
        size_t chunk = out.size() - got;
        if (chunk > 4096) chunk = 4096;
        ssize_t n = ::read(fd, out.data() + got, chunk);
        if (n <= 0) {
            why = out_path + ": read failed at byte " + std::to_string(got) + " (errno " + std::to_string(errno) + ")";
            ::close(fd);
            out.clear(); out.shrink_to_fit();
            return false;
        }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

Kubelet::Kubelet(const KubeletConfig& config,
                 runtime::Runtime& runtime,
                 std::unique_ptr<KubeApiClient> api_client)
    : config_(config), runtime_(runtime), api_client_(std::move(api_client)) {
    if (!api_client_) {
        auto client = std::make_unique<HttpKubeClient>(config_.api_server_host, config_.api_server_port);
        client->set_node_filter(config_.node_name);
        client->set_bearer_token(config_.api_token);
        client->set_max_body(config_.api_max_body);
        api_client_ = std::move(client);
    }
    status_reporter_ = std::make_unique<NodeStatusReporter>(config_.node_name, config_.node_ip, config_.control_plane);
    status_reporter_->set_labels(config_.node_labels);
    status_reporter_->set_kubelet_port(config_.kubelet_port);
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
                if (heartbeat_due_.exchange(false) || now - last_heartbeat_ms_ >= config_.heartbeat_interval_ms) {
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
    fill_ready_condition(t);
    std::string reg_json = status_reporter_->build_registration_json(t);

    bool conflict = false;
    bool ok = api_client_->register_node(reg_json, conflict);
    if (ok) {
        // The Node object survived this kubelet (a worker reboot): bring its
        // labels, nodeInfo and capacity up to date with this boot.
        if (conflict) api_client_->patch_node_status(config_.node_name, reg_json);
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
    // The lease outlives three missed heartbeats: the control plane marks the
    // node NotReady once it expires (NodeLifecycleController)
    const uint32_t lease_s = std::max<uint32_t>(15, (3 * config_.heartbeat_interval_ms + 999) / 1000);
    api_client_->renew_node_lease(config_.node_name, lease_s, models::now_micro_time());

    NodeTelemetry t = status_reporter_->collect_telemetry();
    fill_ready_condition(t);
    bool memory_pressure = (t.free_heap_bytes < config_.memory_pressure_threshold_bytes);
    std::string patch = status_reporter_->build_heartbeat_patch_json(t, memory_pressure);
    if (!api_client_->patch_node_status(config_.node_name, patch)) {
        // Two rejected heartbeats in a row: the control plane restarted or
        // lost this node (a control plane without storage keeps its state in
        // RAM), so register again; a Node that survived answers 409 and is
        // updated. Local pods keep running; the next pod sync compares them
        // with the pods the API server has bound to this node.
        heartbeat_failures_++;
        if (heartbeat_failures_ >= 2) {
            std::cout << "[KUBELET] heartbeat rejected, re-registering node " << config_.node_name << std::endl;
            registered_.store(false);
            heartbeat_failures_ = 0;
        }
    } else {
        heartbeat_failures_ = 0;
        push_metrics_sample();
    }
    last_heartbeat_ms_ = now_ms;
}

void Kubelet::fill_ready_condition(NodeTelemetry& t) const {
    const uint64_t last = last_sync_ok_ms_.load();
    const uint64_t now = osal::System::get_uptime_ms();
    if (last == 0) {
        t.ready = false;
        t.ready_reason = "KubeletNotReady";
        t.ready_message = "kubelet has not completed a pod sync yet";
    } else if (now > last && now - last > config_.sync_stall_threshold_ms) {
        t.ready = false;
        t.ready_reason = "KubeletNotReady";
        t.ready_message = "pod sync loop last completed " + std::to_string((now - last) / 1000) + "s ago";
    } else {
        t.ready = true;
        t.ready_reason = "KubeletReady";
        t.ready_message = "kubelet is posting ready status";
    }
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
    for (auto& item : all_pods) {
        const auto& pod = item.first;
        if (pod.node_name == config_.node_name) {
            std::string key = pod.namespace_name + "/" + pod.name;
            if (!pod.deletion_timestamp.empty()) {
                terminating.emplace_back(pod.namespace_name, pod.name);
                continue;
            }
            // A pod that already Succeeded or Failed is never started again,
            // also not after this node or the control plane restarted
            if (active_pods_.find(key) == active_pods_.end() && pod_phase_is_terminal(pod)) continue;
            desired[key] = std::move(item);
        }
    }
    std::vector<std::pair<models::KubePod, std::string>>().swap(all_pods);

    osal::LockGuard guard(state_mutex_);

    // 0. Graceful deletions: stop the container, flush its log, then finalize
    for (const auto& tp : terminating) {
        std::string key = tp.first + "/" + tp.second;
        if (active_pods_.find(key) != active_pods_.end()) terminate_pod(key);
        api_client_->delete_pod(tp.first, tp.second);
    }

    // 1. Detect additions, and restart containers whose back-off has passed
    const uint64_t now_ms = osal::System::get_uptime_ms();
    for (const auto& kv : desired) {
        const std::string& key = kv.first;
        auto it = active_pods_.find(key);
        if (it != active_pods_.end() && !it->second.uid.empty() && !kv.second.first.uid.empty() && it->second.uid != kv.second.first.uid) {
            // Deleted and created again under the same name while this node
            // still had the old pod: the old one stops, the new one starts
            terminate_pod(key);
            it = active_pods_.end();
        }
        if (it == active_pods_.end()) {
            start_pod_guarded(kv.second.first, kv.second.second, nullptr);
            continue;
        }
        // Tried again once due: a crashed container after its back-off, a
        // container whose ConfigMap or Secret may exist by now, a pod the
        // node may have the memory for by now
        const PodExecutionPhase phase = it->second.execution_phase;
        if ((phase == PodExecutionPhase::CrashLoopBackOff || phase == PodExecutionPhase::CreateContainerConfigError ||
             phase == PodExecutionPhase::InsufficientMemory) && now_ms >= it->second.next_restart_allowed_ms) {
            LocalPodRecord previous = it->second;
            active_pods_.erase(it);
            if (phase == PodExecutionPhase::CrashLoopBackOff) {
                std::cout << "[KUBELET] " << key << ": restarting container (restart " << (previous.restart_count + 1) << ")" << std::endl;
            }
            start_pod_guarded(kv.second.first, kv.second.second, &previous);
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

    // A completed pass is what the node's Ready condition reports on; the
    // first one is announced right away instead of at the next heartbeat.
    const uint64_t done = osal::System::get_uptime_ms();
    if (last_sync_ok_ms_.exchange(done > 0 ? done : 1) == 0) heartbeat_due_.store(true);
}

void Kubelet::start_pod_guarded(const models::KubePod& pod, const std::string& manifest_json, const LocalPodRecord* previous) {
    const std::string key = pod.namespace_name + "/" + pod.name;
    try {
        deploy_and_start_pod(pod, manifest_json, previous);
    } catch (const std::bad_alloc&) {
        // Nothing of the attempt is kept; the pod waits for memory like one
        // that was not admitted
        runtime_.terminate(key);
        hal::CapabilityFirewall::instance().unregister_pod(key);
        // What the pod's last container left (restart count, last state) stays
        LocalPodRecord rec = previous ? *previous : LocalPodRecord{};
        rec.restart_due = previous && (previous->restart_due || previous->execution_phase == PodExecutionPhase::CrashLoopBackOff);
        rec.name = pod.name;
        rec.namespace_name = pod.namespace_name;
        rec.uid = pod.uid;
        rec.image = pod.image;
        rec.container_name = pod.container_name.empty() ? pod.name : pod.container_name;
        wait_for_memory(key, rec, "not enough memory on the node to start the container (free heap " +
                                  kib(osal::System::get_free_heap_bytes()) + ")", previous);
    }
}

bool Kubelet::wait_for_memory(const std::string& pod_key, LocalPodRecord& rec, const std::string& why, const LocalPodRecord* previous) {
    rec.execution_phase = PodExecutionPhase::InsufficientMemory;
    rec.last_error_message = why;
    rec.next_restart_allowed_ms = osal::System::get_uptime_ms() + config_.memory_retry_ms;
    active_pods_[pod_key] = rec;
    if (previous && previous->execution_phase == PodExecutionPhase::InsufficientMemory) return false;
    std::cout << "[KUBELET] " << pod_key << ": " << why << "; waiting" << std::endl;
    const std::string message = why + "; retrying every " + std::to_string(config_.memory_retry_ms / 1000) + " s";
    std::string status_patch = "{\"status\":{\"phase\":\"Pending\","
                               "\"conditions\":[{\"type\":\"PodScheduled\",\"status\":\"True\"},"
                               "{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"},"
                               "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"}],"
                               "\"containerStatuses\":[{\"name\":\"" + rec.container_name + "\",\"image\":\"" + rec.image +
                               "\",\"ready\":false,\"started\":false,\"restartCount\":" + std::to_string(rec.restart_count) + ","
                               "\"state\":{\"waiting\":{\"reason\":\"InsufficientMemory\",\"message\":\"" + json_escape(message) + "\"}}}]}}";
    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
    record_pod_event(rec, "InsufficientMemory", message, true);
    return false;
}

bool Kubelet::deploy_and_start_pod(const models::KubePod& pod_in, const std::string& manifest_json, const LocalPodRecord* previous) {
    // Callers may hand us a partially filled struct; the manifest is the truth
    models::KubePod pod = pod_in;
    if (pod.image.empty() && !manifest_json.empty()) {
        models::KubePod parsed = models::parse_pod_manifest(manifest_json);
        pod.image = parsed.image;
        pod.env = parsed.env;
        pod.annotations = parsed.annotations;
        pod.labels = parsed.labels;
        pod.restart_policy = parsed.restart_policy;
        if (pod.uid.empty()) pod.uid = parsed.uid;
        if (pod.container_name.empty()) pod.container_name = parsed.container_name;
        if (pod.memory_limit_bytes == 0) pod.memory_limit_bytes = parsed.memory_limit_bytes;
        if (parsed.memory_request_set) {
            pod.memory_request_bytes = parsed.memory_request_bytes;
            pod.memory_request_set = true;
        }
        if (pod.psram_request_bytes == 0) pod.psram_request_bytes = parsed.psram_request_bytes;
    }
    std::string pod_key = pod.namespace_name + "/" + pod.name;

    LocalPodRecord rec;
    rec.name = pod.name;
    rec.namespace_name = pod.namespace_name;
    rec.restart_policy = pod.restart_policy;
    // A start after a container exited is a restart; a retry of a container
    // that could not be created is not.
    const bool restarting = previous && (previous->execution_phase == PodExecutionPhase::CrashLoopBackOff || previous->restart_due);
    if (previous) {
        rec.restart_count = previous->restart_count + (restarting ? 1 : 0);
        rec.crash_streak = previous->crash_streak;
        rec.last_exit_code = previous->last_exit_code;
        rec.last_reason = previous->last_reason;
        rec.last_finished_at = previous->last_finished_at;
        rec.started_at_ms = previous->started_at_ms;
        rec.shipped_log_seq = previous->shipped_log_seq;
    }
    // Lines after this one belong to the container instance started now
    rec.log_instance_start_seq = runtime::PodLogStore::runtime().last_seq(pod_key);
    rec.uid = pod.uid;
    rec.image = pod.image;
    rec.container_name = pod.container_name.empty() ? pod.name : pod.container_name;
    rec.execution_phase = PodExecutionPhase::ContainerCreating;
    // Pods have no network namespace here: the pod answers on the node's
    // address (pod HTTP server), so that is its IP.
    rec.pod_ip = config_.node_ip;

    // Wasm linear memory budget: an explicit PSRAM request (esp32.io/psram) is
    // the sandbox size on PSRAM nodes; otherwise limits.memory, then
    // requests.memory, then one 64 KiB page. A budget below one page caps the
    // pod's first page at that size (ContainerConfig::memory_limit_bytes).
    uint64_t mem = pod.memory_limit_bytes > 0 ? pod.memory_limit_bytes
                 : (pod.memory_request_set ? pod.memory_request_bytes : 64 * 1024);
    if (pod.psram_request_bytes > mem) mem = pod.psram_request_bytes;
    rec.memory_limit_bytes = static_cast<uint32_t>(mem);
    // kube32.io/wasm-stack: the module's operand stack (default 4 KiB for
    // pods of at most one page, 16 KiB above), 512 bytes to 64 KiB
    uint32_t wasm_stack = mem <= 64 * 1024 ? 4 * 1024 : 16 * 1024;
    bool wasm_stack_set = false;
    {
        auto it = pod.annotations.find("kube32.io/wasm-stack");
        if (it != pod.annotations.end()) {
            uint64_t v = models::parse_quantity(it->second);
            if (v >= 512 && v <= 64 * 1024) {
                wasm_stack = static_cast<uint32_t>(v);
                wasm_stack_set = true;
            } else {
                std::cout << "[KUBELET] " << pod_key << ": ignoring kube32.io/wasm-stack \"" << it->second << "\" (want 512 to 64Ki)" << std::endl;
            }
        }
    }

    // Memory admission: what the pod needs to start (estimated from its
    // module) against what pods can still be given here with the system's
    // reserve kept. A pod larger than the node could ever give it is rejected,
    // as Kubernetes' kubelet rejects a pod that does not fit (OutOfmemory);
    // one that fits once other pods are gone waits for memory.
    {
        const uint8_t* module = nullptr;
        size_t module_len = 0;
        std::string sd_name, sd_tag;
        if (runtime::parse_sd_image_ref(pod.image, sd_name, sd_tag)) {
            struct stat st;
            const std::string root = hal::sd_root_path();
            if (!root.empty() && ::stat((root + "/" + runtime::sd_image_relative_path(sd_name, sd_tag)).c_str(), &st) == 0 && st.st_size > 0) {
                module_len = static_cast<size_t>(st.st_size);
            }
        } else if (const runtime::EmbeddedImage* img = runtime::ImageRegistry::instance().resolve(pod.image, nullptr)) {
            module = img->data;
            module_len = img->len;
        } else if (auto ann = pod.annotations.find("kube32.io/wasm-base64"); ann != pod.annotations.end()) {
            module_len = ann->second.size() / 4 * 3;   // empty in pod lists: then only the rest is estimated
        }
        const size_t need = runtime::estimate_pod_memory(module, module_len, rec.memory_limit_bytes, wasm_stack);
        size_t capacity = 0;
        const size_t room = kube32_pool_pod_room(rec.memory_limit_bytes, &capacity);
        const std::string reserve = kib(kube32_pool_system_reserve());
        if (need > capacity) {
            const std::string why = "Pod was rejected: Node didn't have enough resource: memory, needs about " + kib(need) +
                                    ", the node can give pods at most " + kib(capacity) + " (" + reserve + " kept for the system)";
            std::cout << "[KUBELET] " << pod_key << ": " << why << std::endl;
            rec.execution_phase = PodExecutionPhase::Failed;
            rec.last_error_message = why;
            active_pods_[pod_key] = rec;
            std::string status_patch = "{\"status\":{\"phase\":\"Failed\",\"reason\":\"OutOfmemory\",\"message\":\"" + json_escape(why) + "\"," +
                                       "\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"PodFailed\"}]}}";
            api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
            record_pod_event(rec, "OutOfmemory", why, true);
            return false;
        }
        if (need > room) {
            if (previous) {
                rec.restart_count = previous->restart_count;
                rec.restart_due = previous->restart_due || restarting;
            }
            return wait_for_memory(pod_key, rec, "not enough memory to start the container: it needs about " + kib(need) +
                                                 ", pods can be given " + kib(room) + " now (" + reserve + " kept for the system)", previous);
        }
    }

    // Register capabilities with firewall. A start that failed half way (out
    // of memory) may have left the pod's claims behind: they are its own.
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
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
        status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
        status_patch += "\"state\":{\"terminated\":{\"exitCode\":1,\"reason\":\"CapabilityAdmissionFailed\"}}";
        status_patch += "}]";
        status_patch += "}}";

        api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
        record_pod_event(rec, "Failed", "Error: capability admission rejected: " + firewall_err, true);
        return false;
    }

    // A pod with a volume needs node storage, and a kube32.io/sd volume the
    // card; say so instead of letting every k32_fs_* call fail later.
    const bool has_volume = !hal::CapabilityFirewall::instance().sd_claim(pod_key).empty();
    const bool needs_card = hal::CapabilityFirewall::instance().volume_needs_card(pod_key);
    if ((needs_card && !hal::sd_is_mounted()) || (has_volume && hal::node_storage_root().empty())) {
        std::string reason = needs_card ? hal::sd_info().error : hal::node_storage_info().error;
        std::string why = (needs_card ? "the pod's volume needs a microSD card (kube32.io/sd) and none is mounted on this node"
                                      : "the pod has a volume and this node has no storage mounted") +
                          (reason.empty() ? std::string() : " (" + reason + ")");
        hal::CapabilityFirewall::instance().unregister_pod(pod_key);
        rec.execution_phase = PodExecutionPhase::Failed;
        rec.last_error_message = why;
        active_pods_[pod_key] = rec;
        std::string status_patch = "{\"status\":{\"phase\":\"Failed\",\"reason\":\"CapabilityAdmissionFailed\",\"message\":\"" + json_escape(why) +
                                   "\",\"containerStatuses\":[{\"name\":\"" + pod.name + "\",\"ready\":false,\"restartCount\":" + std::to_string(rec.restart_count) + ","
                                   "\"state\":{\"terminated\":{\"exitCode\":1,\"reason\":\"CapabilityAdmissionFailed\"}}}]}}";
        api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
        record_pod_event(rec, "Failed", "Error: " + why, true);
        return false;
    }

    // Container environment: service links, envFrom and env with valueFrom.
    // A missing ConfigMap, Secret or key leaves the container uncreated
    // (CreateContainerConfigError) and is retried, as on Kubernetes.
    std::vector<std::pair<std::string, std::string>> container_env = pod.env;
    if (!manifest_json.empty()) {
        PodEnvContext ectx;
        ectx.api_get = [this](const std::string& path, std::string& body) { return api_client_->get(path, body); };
        ectx.api_list = [this](const std::string& path, const std::function<void(std::string&&)>& on_item) {
            return api_client_->list_items(path, on_item);
        };
        ectx.node_name = config_.node_name;
        ectx.node_ip = config_.node_ip;
        // The control plane's kubelet reaches the API on loopback; pods are
        // given the address the rest of the network uses.
        ectx.api_host = (config_.api_server_host == "127.0.0.1" || config_.api_server_host == "localhost") ? config_.node_ip : config_.api_server_host;
        ectx.api_port = config_.api_server_port;
        std::string env_err;
        if (!build_container_env(manifest_json, ectx, container_env, env_err)) {
            hal::CapabilityFirewall::instance().unregister_pod(pod_key);
            if (previous) rec.restart_count = previous->restart_count;
            rec.restart_due = restarting;
            rec.execution_phase = PodExecutionPhase::CreateContainerConfigError;
            rec.last_error_message = env_err;
            rec.next_restart_allowed_ms = osal::System::get_uptime_ms() + config_.restart_backoff_initial_ms;
            active_pods_[pod_key] = rec;
            std::cout << "[KUBELET] " << pod_key << ": CreateContainerConfigError: " << env_err << std::endl;
            std::string status_patch = "{\"status\":{\"phase\":\"Pending\","
                                       "\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"},"
                                       "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"}],"
                                       "\"containerStatuses\":[{\"name\":\"" + rec.container_name + "\",\"image\":\"" + pod.image +
                                       "\",\"ready\":false,\"started\":false,\"restartCount\":" + std::to_string(rec.restart_count) + ","
                                       "\"state\":{\"waiting\":{\"reason\":\"CreateContainerConfigError\",\"message\":\"" + json_escape(env_err) + "\"}}}]}}";
            api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
            record_pod_event(rec, "Failed", "Error: " + env_err, true);
            return false;
        }
    }

    // Resolve the container image: the card for wasm://sd/..., else the
    // embedded registry, else the inline annotation
    const uint8_t* image_bytes = nullptr;
    size_t image_len = 0;
    std::vector<uint8_t> inline_bytes;   // heap image the runtime takes over
    std::string image_key;
    std::string image_source = "embedded registry";
    std::string sd_name, sd_tag;
    std::string asset_bundle, asset_directory;   // k32_resp_asset: the image's bundle
    std::string image_error;                      // why an inline image could not be read
    if (runtime::parse_sd_image_ref(pod.image, sd_name, sd_tag)) {
        image_key = sd_name + (sd_tag.empty() ? std::string() : ":" + sd_tag);
        image_source = "sd card";
        record_pod_event(rec, "Pulling", "Pulling image \"" + pod.image + "\" from the sd card", false);
        std::string sd_path, why;
        uint64_t t_pull = osal::System::get_uptime_ms();
        if (!pull_sd_image(sd_name, sd_tag, inline_bytes, sd_path, why)) {
            std::cout << "[KUBELET] " << pod_key << ": image '" << pod.image << "': " << why << std::endl;
            record_pod_event(rec, "Failed", "Failed to pull image \"" + pod.image + "\": " + why, true);
            return [&]() {
                rec.execution_phase = PodExecutionPhase::Failed;
                rec.last_error_message = why;
                active_pods_[pod_key] = rec;
                hal::CapabilityFirewall::instance().unregister_pod(pod_key);
                std::string status_patch = "{\"status\":{\"phase\":\"Failed\",\"reason\":\"ErrImagePull\",\"message\":\"" + json_escape(why) +
                                           "\",\"containerStatuses\":[{\"name\":\"" + pod.name + "\",\"image\":\"" + pod.image + "\",\"ready\":false,\"restartCount\":" + std::to_string(rec.restart_count) + ","
                                           "\"state\":{\"waiting\":{\"reason\":\"ErrImagePull\",\"message\":\"" + json_escape(why) + "\"}}}]}}";
                api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
                return false;
            }();
        }
        image_bytes = inline_bytes.data();
        image_len = inline_bytes.size();
        asset_directory = hal::sd_root_path() + "/kube32/images/" + sd_name;
        std::cout << "[KUBELET] " << pod_key << ": pulled " << sd_path << " (" << image_len << " bytes, "
                  << (osal::System::get_uptime_ms() - t_pull) << " ms)" << std::endl;
        record_pod_event(rec, "Pulled", "Successfully pulled image \"" + pod.image + "\" in " +
                                        std::to_string(osal::System::get_uptime_ms() - t_pull) + "ms (" +
                                        std::to_string(image_len) + " bytes, sd card, " + sd_path + ")", false);
    } else {
        const runtime::EmbeddedImage* img = runtime::ImageRegistry::instance().resolve(pod.image, &image_key);
        if (img) {
            image_bytes = img->data;
            image_len = img->len;
            asset_bundle = img->name;
        } else {
            auto ann = pod.annotations.find("kube32.io/wasm-base64");
            if (ann != pod.annotations.end()) {
                // Pod lists carry the annotation empty: the module comes with
                // the pod itself, decoded as it streams in
                std::string why;
                bool ok = ann->second.empty() ? api_client_->get_pod_image(pod.namespace_name, pod.name, inline_bytes, why)
                                              : runtime::base64_decode(ann->second, inline_bytes);
                if (ok) {
                    image_bytes = inline_bytes.data();
                    image_len = inline_bytes.size();
                    image_source = "inline annotation";
                } else if (!why.empty()) {
                    std::cout << "[KUBELET] " << pod_key << ": inline image: " << why << std::endl;
                    image_error = why;
                }
            }
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
        status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
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
        const std::string why = image_error.empty() ? "not present in the node image registry" : image_error;
        record_pod_event(rec, "Failed", "Failed to pull image \"" + pod.image + "\": " + why, true);
        return fail_pod("ErrImagePull", "image " + pod.image + ": " + json_escape(why), 1);
    }
    if (image_source != "sd card") {
        record_pod_event(rec, "Pulled", "Container image \"" + pod.image + "\" already present on machine (" +
                                        std::to_string(image_len) + " bytes, " + image_source + ")", false);
    }

    std::string deploy_err;
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = rec.memory_limit_bytes;
    spec.stack_size_bytes = wasm_stack_set ? wasm_stack : 16 * 1024;
    spec.stack_size_set = wasm_stack_set;
    spec.env = std::move(container_env);
    spec.asset_bundle = asset_bundle;
    spec.asset_directory = asset_directory;
    // kube32.io/execution-budget: wasm3 gas units per call, or "off" for an
    // unmetered (faster, uninterruptible) pod. Default: metered.
    {
        auto it = pod.annotations.find("kube32.io/execution-budget");
        if (it != pod.annotations.end()) {
            if (it->second == "off" || it->second == "0") {
                spec.gas_budget_per_call = 0;
            } else {
                char* end = nullptr;
                unsigned long long v = std::strtoull(it->second.c_str(), &end, 10);
                if (end && *end == '\0' && v > 0) spec.gas_budget_per_call = v;
                else std::cout << "[KUBELET] " << pod_key << ": ignoring kube32.io/execution-budget \"" << it->second << "\" (want a number or \"off\")" << std::endl;
            }
        }
    }
    // kube32.io/event-topics: channel topics delivered to the pod's k32_event,
    // separated by commas or whitespace.
    {
        auto it = pod.annotations.find("kube32.io/event-topics");
        if (it != pod.annotations.end()) {
            std::string topic;
            for (char c : it->second + ",") {
                if (c == ',' || c == ' ' || c == '\t' || c == '\n') {
                    if (!topic.empty()) spec.event_topics.push_back(topic);
                    topic.clear();
                } else {
                    topic.push_back(c);
                }
            }
        }
    }
    // Heap images move into the runtime (no second copy); flash images are used in place
    bool deployed = inline_bytes.empty()
        ? runtime_.deploy_workload_ex(pod_key, image_bytes, image_len, spec, deploy_err)
        : runtime_.deploy_workload_owned(pod_key, std::move(inline_bytes), spec, deploy_err);
    if (!deployed && runtime::deploy_error_is_out_of_memory(deploy_err)) {
        hal::CapabilityFirewall::instance().unregister_pod(pod_key);
        if (previous) {
            rec.restart_count = previous->restart_count;
            rec.restart_due = previous->restart_due || restarting;
        }
        return wait_for_memory(pod_key, rec, "not enough memory to load the module: " + deploy_err, previous);
    }
    if (!deployed) {
        std::cout << "[KUBELET] " << pod_key << ": deploy failed: " << deploy_err << std::endl;
        record_pod_event(rec, "Failed", "Error: failed to create container " + rec.container_name + ": " + deploy_err, true);
        LocalPodRecord& stored = (active_pods_[pod_key] = rec);
        handle_container_exit(pod_key, stored, 128, "StartError", deploy_err);
        return false;
    }
    record_pod_event(rec, "Created", "Created container " + rec.container_name, false);
    if (!runtime_.start(pod_key)) {
        // Typically the pod thread's stack could not be allocated; say so
        // instead of reporting a container that does not exist as Running.
        std::string why = "failed to start container " + rec.container_name + ": cannot create pod thread (out of memory)";
        std::cout << "[KUBELET] " << pod_key << ": " << why << std::endl;
        record_pod_event(rec, "Failed", "Error: " + why, true);
        runtime_.terminate(pod_key);
        LocalPodRecord& stored = (active_pods_[pod_key] = rec);
        handle_container_exit(pod_key, stored, 128, "StartError", why);
        return false;
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
    // startTime: when the pod first started here, not at restarts
    if (!now.empty() && (!previous || previous->started_at_ms == 0)) status_patch += "\"startTime\":\"" + now + "\",";
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
    status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
    status_patch += "\"state\":{\"running\":{" + (now.empty() ? std::string() : "\"startedAt\":\"" + now + "\"") + "}}";
    if (rec.restart_count > 0) {
        status_patch += ",\"lastState\":{\"terminated\":{\"exitCode\":" + std::to_string(rec.last_exit_code) + ",\"reason\":\"" + rec.last_reason + "\"" +
                        (rec.last_finished_at.empty() ? std::string() : ",\"finishedAt\":\"" + rec.last_finished_at + "\"") + "}}";
    }
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

// The container ended and restartPolicy does not restart it: the pod is Failed.
void Kubelet::handle_pod_failure(LocalPodRecord& rec, const runtime::ContainerStatus& cs) {
    rec.execution_phase = PodExecutionPhase::Failed;
    rec.last_error_message = !cs.exit_message.empty() ? cs.exit_message : cs.termination_reason;

    int32_t exit_code = (cs.exit_code != 0) ? cs.exit_code : 137;
    std::string reason = !cs.termination_reason.empty() ? cs.termination_reason : "Error";
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

// The entry point returned 0 and restartPolicy (Never or OnFailure) does not
// restart it: the pod is Succeeded.
void Kubelet::handle_pod_completion(LocalPodRecord& rec, const runtime::ContainerStatus& cs) {
    rec.execution_phase = PodExecutionPhase::Completed;
    const std::string now = models::now_rfc3339();
    const std::string cname = rec.container_name.empty() ? rec.name : rec.container_name;
    std::string status_patch;
    status_patch.reserve(400);
    status_patch += "{\"status\":{\"phase\":\"Succeeded\",";
    status_patch += "\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"PodCompleted\"},"
                    "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"PodCompleted\"}],";
    status_patch += "\"containerStatuses\":[{\"name\":\"" + cname + "\",\"ready\":false,\"started\":false,";
    status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
    status_patch += "\"state\":{\"terminated\":{\"exitCode\":0,\"reason\":\"Completed\"" +
                    (now.empty() ? std::string() : ",\"finishedAt\":\"" + now + "\"") + "}}}]}}";
    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
    (void)cs;
    record_pod_event(rec, "Completed", "Container " + cname + " exited with code 0", false);
}

void Kubelet::handle_container_exit(const std::string& pod_key, LocalPodRecord& rec, int32_t exit_code,
                                    const std::string& reason, const std::string& message) {
    const bool failed = (exit_code != 0);
    const bool restart = rec.restart_policy == "Always" || (rec.restart_policy == "OnFailure" && failed);
    const uint64_t now = osal::System::get_uptime_ms();
    const std::string finished = models::now_rfc3339();
    const std::string cname = rec.container_name.empty() ? rec.name : rec.container_name;

    // The container is gone either way: free its memory, thread and claims.
    runtime_.terminate(pod_key);
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);

    if (!restart) {
        runtime::ContainerStatus cs;
        cs.exit_code = exit_code;
        cs.termination_reason = reason;
        cs.exit_message = message;
        if (failed) {
            cs.phase = runtime::ContainerPhase::Failed;
            handle_pod_failure(rec, cs);
        } else {
            cs.phase = runtime::ContainerPhase::Terminated;
            handle_pod_completion(rec, cs);
        }
        return;
    }

    // CrashLoopBackOff: 10 s, doubled per restart up to 5 minutes; a container
    // that ran for 10 minutes starts the sequence over.
    if (rec.started_at_ms > 0 && now - rec.started_at_ms >= 600000) rec.crash_streak = 0;
    uint64_t delay = config_.restart_backoff_initial_ms;
    for (uint32_t i = 0; i < rec.crash_streak && delay < config_.restart_backoff_max_ms; ++i) delay *= 2;
    if (delay > config_.restart_backoff_max_ms) delay = config_.restart_backoff_max_ms;
    rec.crash_streak++;
    rec.next_restart_allowed_ms = now + delay;
    rec.execution_phase = PodExecutionPhase::CrashLoopBackOff;
    rec.last_exit_code = exit_code;
    rec.last_reason = reason;
    rec.last_finished_at = finished;
    rec.last_error_message = message;

    const std::string backoff = "back-off " + std::to_string(delay / 1000) + "s restarting failed container=" + cname +
                                " pod=" + rec.name + "_" + rec.namespace_name + "(" + rec.uid + ")";
    std::string status_patch;
    status_patch.reserve(640);
    status_patch += "{\"status\":{\"phase\":\"Running\",";
    status_patch += "\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"},"
                    "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"}],";
    status_patch += "\"containerStatuses\":[{\"name\":\"" + cname + "\",\"image\":\"" + rec.image + "\",\"ready\":false,\"started\":false,";
    status_patch += "\"restartCount\":" + std::to_string(rec.restart_count) + ",";
    status_patch += "\"state\":{\"waiting\":{\"reason\":\"CrashLoopBackOff\",\"message\":\"" + json_escape(backoff) + "\"}},";
    status_patch += "\"lastState\":{\"terminated\":{\"exitCode\":" + std::to_string(exit_code) + ",\"reason\":\"" + reason + "\",\"message\":\"" +
                    json_escape(message) + "\"" + (finished.empty() ? std::string() : ",\"finishedAt\":\"" + finished + "\"") + "}}";
    status_patch += "}]}}";
    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
    std::cout << "[KUBELET] " << pod_key << ": container " << cname << " exited (" << reason << ", exit code " << exit_code
              << "), restartPolicy " << rec.restart_policy << ": restarting in " << delay / 1000 << " s" << std::endl;
    record_pod_event(rec, "BackOff", "Back-off restarting failed container " + cname + " in pod " + rec.name + "_" +
                                     rec.namespace_name + "(" + rec.uid + ")", true);
}

void Kubelet::monitor_and_report_pod_statuses() {
    last_status_report_ms_ = osal::System::get_uptime_ms();
    // Containers whose stop outlasted a host call give their memory back once it returns
    runtime_.reap_stopped();

    osal::LockGuard guard(state_mutex_);
    for (auto& kv : active_pods_) {
        const std::string& pod_key = kv.first;
        LocalPodRecord& rec = kv.second;

        // Only a running container can end; pods waiting out a back-off or in a
        // final phase no longer have one.
        if (rec.execution_phase == PodExecutionPhase::Running) {
            runtime::ContainerStatus cs = runtime_.get_status(pod_key);
            if (cs.phase == runtime::ContainerPhase::Failed) {
                // Trap or forced stop: 128 + signal and reason Error, or OOMKilled (runtime::describe_trap)
                handle_container_exit(pod_key, rec, cs.exit_code != 0 ? cs.exit_code : 137,
                                      cs.termination_reason.empty() ? "Error" : cs.termination_reason,
                                      cs.exit_message.empty() ? cs.termination_reason : cs.exit_message);
            } else if (cs.phase == runtime::ContainerPhase::Terminated && cs.exit_message != "Pod not found") {
                // The entry point returned; its return value is the exit code
                handle_container_exit(pod_key, rec, cs.exit_code,
                                      cs.exit_code == 0 ? "Completed" : (cs.termination_reason.empty() ? "Error" : cs.termination_reason),
                                      cs.exit_message);
            }
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
        // "r": the container instance (restart count) that wrote the line,
        // which is what the API server keeps a previous log by
        const uint32_t instance = (r.seq <= rec.log_instance_start_seq && rec.restart_count > 0) ? rec.restart_count - 1 : rec.restart_count;
        body += "{\"epoch\":" + std::to_string(log_epoch_) + ",\"seq\":" + std::to_string(r.seq) +
                ",\"t\":" + std::to_string(r.unix_ms) + ",\"u\":" + std::to_string(r.uptime_ms) +
                ",\"r\":" + std::to_string(instance) + ",\"m\":\"" + json_escape(r.text) + "\"}\n";
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
