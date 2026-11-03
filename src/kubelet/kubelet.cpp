#include "kubelet/kubelet.hpp"
#include "kubelet/pod_env.hpp"
#include <cstdlib>
#include <new>
#include "hal/capability_firewall.hpp"
#include "hal/sd_card.hpp"
#include "hal/node_storage.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_snapshot.hpp"
#include "app/pod_checkpoint_api.hpp"
#include "runtime/pod_memory_pool.h"
#include "models/kube_models.hpp"
#include "models/kube_lease.hpp"
#include "osal/system.hpp"
#include "mem/fixed_text.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <iostream>
#include <new>
#include <tuple>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kube32::kubelet {

// Pods whose image resolves to nothing fail with ErrImagePull, as on Kubernetes.

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

// Reads <sd root>/kube32/images/<name>[-<tag>].wasm into `out` (pod memory,
// sized from the file, filled 4 KiB at a time). On failure `why` says what
// happened.
static bool pull_sd_image(const std::string& name, const std::string& tag, runtime::ModuleBytes& out, std::string& out_path, std::string& why) {
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
    if (!out.resize(static_cast<size_t>(size))) {
        why = runtime::module_no_memory(static_cast<size_t>(size)) + " (" + out_path + ")";
        return false;
    }
    int fd = ::open(out_path.c_str(), O_RDONLY);
    if (fd < 0) {
        why = out_path + ": open failed (errno " + std::to_string(errno) + ")";
        out.clear();
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
            out.clear();
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
    status_reporter_->set_max_pods(config_.max_pods);
    metrics_ = create_platform_metrics_provider();
    if (config_.firmware_slots) {
        FirmwareAgentConfig fw = config_.firmware;
        fw.node_name = config_.node_name;
        firmware_ = std::make_unique<FirmwareAgent>(*config_.firmware_slots, *api_client_, fw);
        status_reporter_->set_firmware_version(firmware_->running().version);
    }
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
    if (!registration_buf_) registration_buf_.reset(new (std::nothrow) char[kRegistrationBufBytes]);
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
    h_cfg.stack_size = config_.task_stack_in_psram ? 12288 : 8192;   // the status patch (1.5 KB) and the metrics sample (1.5 KB) are written here
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
                const bool firmware_due = firmware_ && firmware_->heartbeat_wanted();
                if (heartbeat_due_.exchange(false) || firmware_due || now - last_heartbeat_ms_ >= config_.heartbeat_interval_ms) {
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
        const char* step = "registration";   // named in the log when it throws
        try {
            if (!registered_.load()) {
                reconcile_node_registration();
            } else {
                uint64_t now = osal::System::get_uptime_ms();

                if (sync_due_.exchange(false) || now - last_sync_ms_ >= config_.sync_frequency_ms) {
                    step = "pod sync";
                    sync_assigned_pods();
                }

                if (now - last_status_report_ms_ >= config_.pod_status_frequency_ms) {
                    step = "pod status";
                    monitor_and_report_pod_statuses();
                }

                if (config_.node_label_source && now - last_label_refresh_ms_ >= config_.node_label_refresh_ms) {
                    step = "node labels";
                    refresh_node_labels();
                } else if (!config_.node_label_source && now - last_label_refresh_ms_ >= config_.node_label_refresh_ms) {
                    last_label_refresh_ms_ = now;
                    step = "node annotations";
                    patch_node_annotations();   // what changed (the card's free space), or a patch that failed
                }
            }
            // Runs unregistered too: an image that cannot reach the API
            // server must still reject itself in time
            step = "firmware";
            if (firmware_) firmware_->tick(osal::System::get_uptime_ms(), registered_.load());
        } catch (const std::exception& e) {
            std::cout << "[KUBELET] " << step << " failed: " << e.what() << " (free heap "
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
    if (config_.node_label_source && last_label_refresh_ms_ == 0) refresh_node_labels();

    NodeTelemetry t = status_reporter_->collect_telemetry();
    fill_ready_condition(t);
    fill_firmware_condition(t);
    time_conditions(t, nullptr);
    if (firmware_) status_reporter_->set_annotations(firmware_->annotations());
    std::string reg_heap;
    std::string_view reg_json;
    mem::FixedText reg(registration_buf_.get(), registration_buf_ ? kRegistrationBufBytes : 0);
    if (registration_buf_ && status_reporter_->write_registration_json(reg, t)) {
        reg_json = reg.view();
    } else {
        reg_heap = status_reporter_->build_registration_json(t);
        reg_json = reg_heap;
    }

    bool conflict = false;
    bool ok = api_client_->register_node(reg_json, conflict);
    if (ok) {
        status_reporter_->set_node_resources(NodeStatusReporter::resource_bits(t));
        // The Node object survived this kubelet (a worker reboot): bring its
        // labels, nodeInfo and capacity up to date with this boot. The
        // resources it had then and this boot has not are taken off.
        if (conflict) {
            if (registration_buf_ && status_reporter_->write_registration_json(reg, t, true)) {
                reg_json = reg.view();
            } else {
                reg_heap = status_reporter_->build_registration_json(t, true);
                reg_json = reg_heap;
            }
            // Until the patch lands the Node may hold any of them: the
            // heartbeats take them off then
            if (!api_client_->patch_node_status(config_.node_name, reg_json)) status_reporter_->set_node_resources(~0u);
            drop_stale_node_labels();
        }
        registered_.store(true);
        // The firmware annotations again: a Node that outlived this kubelet
        // may carry ones this boot no longer has
        if (firmware_) firmware_->mark_annotations_dirty();
        annotations_sent_ = false;
        patch_node_annotations();
        report_heartbeat();
        last_heartbeat_ms_ = osal::System::get_uptime_ms();
    }
}

// A byte count as a Kubernetes quantity: "512Mi", "1024Ki" when it divides, else bytes
static std::string bytes_quantity(uint64_t bytes) {
    if (bytes > 0 && bytes % (1024ULL * 1024ULL) == 0) return std::to_string(bytes / (1024ULL * 1024ULL)) + "Mi";
    if (bytes > 0 && bytes % 1024ULL == 0) return std::to_string(bytes / 1024ULL) + "Ki";
    return std::to_string(bytes);
}

void Kubelet::patch_node_annotations() {
    std::map<std::string, std::string> found;
    if (config_.node_annotation_source) found = config_.node_annotation_source();
    found.insert(config_.node_annotations.begin(), config_.node_annotations.end());
    // The card's size and free space, measured; free space in whole MiB
    // (rounded down), so that writes of a few KB do not rewrite the Node
    const hal::SdInfo sd = hal::sd_info();
    if (sd.mounted) {
        found[kAnnotationSdCapacity] = bytes_quantity(sd.total_bytes);
        found[kAnnotationSdFree] = std::to_string(sd.free_bytes >> 20) + "Mi";
    }
    if (found == found_annotations_ && annotations_sent_) return;
    if (registered_.load()) {
        std::string patch = "{\"metadata\":{\"annotations\":{";
        bool first = true;
        for (const auto& kv : found) {
            patch += (first ? "\"" : ",\"") + kv.first + "\":\"" + json_escape(kv.second) + "\"";
            first = false;
        }
        for (const auto& kv : found_annotations_) {
            if (found.count(kv.first)) continue;
            patch += (first ? "\"" : ",\"") + kv.first + "\":null";
            first = false;
        }
        // Card annotations a Node from an earlier boot may still carry
        for (const char* key : {kAnnotationSdCapacity, kAnnotationSdFree}) {
            if (found.count(key) || found_annotations_.count(key) || annotations_sent_) continue;
            patch += (first ? "\"" : ",\"") + std::string(key) + "\":null";
            first = false;
        }
        patch += "}}}";
        annotations_sent_ = api_client_->patch_node_status(config_.node_name, patch);   // tried again next round
    }
    found_annotations_ = std::move(found);
}

void Kubelet::refresh_node_labels() {
    last_label_refresh_ms_ = osal::System::get_uptime_ms();
    std::map<std::string, std::string> found = config_.node_label_source();
    patch_node_annotations();
    if (found == found_labels_) return;
    if (registered_.load()) {
        // JSON merge patch: null removes a label
        std::string patch = "{\"metadata\":{\"labels\":{";
        bool first = true;
        auto add = [&](const std::string& key, const std::string* value) {
            if (!first) patch += ",";
            first = false;
            patch += "\"" + key + "\":";
            patch += value ? "\"" + *value + "\"" : std::string("null");
        };
        for (const auto& kv : found) {
            auto it = found_labels_.find(kv.first);
            if (it == found_labels_.end() || it->second != kv.second) add(kv.first, &kv.second);
        }
        for (const auto& kv : found_labels_) {
            if (!found.count(kv.first)) add(kv.first, nullptr);
        }
        patch += "}}}";
        if (!api_client_->patch_node_status(config_.node_name, patch)) return;   // tried again next round
    }
    for (const auto& kv : found) {
        auto it = found_labels_.find(kv.first);
        if (it == found_labels_.end() || it->second != kv.second) std::cout << "[KUBELET] node label " << kv.first << "=" << kv.second << std::endl;
    }
    for (const auto& kv : found_labels_) {
        if (!found.count(kv.first)) std::cout << "[KUBELET] node label " << kv.first << " removed" << std::endl;
    }
    found_labels_ = std::move(found);
    std::map<std::string, std::string> labels = config_.node_labels;
    labels.insert(found_labels_.begin(), found_labels_.end());   // configured labels win
    status_reporter_->set_labels(labels);
}

void Kubelet::drop_stale_node_labels() {
    if (config_.node_label_prefix.empty()) return;
    std::string body;
    if (api_client_->get("/api/v1/nodes/" + config_.node_name, body) != 200) return;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(body, doc, err) || !doc.is_object()) return;
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
    if (!labels || !labels->is_object()) return;
    std::string patch;
    for (const auto& kv : labels->obj_val) {
        if (kv.first.rfind(config_.node_label_prefix, 0) != 0 || found_labels_.count(kv.first)) continue;
        patch += patch.empty() ? "{\"metadata\":{\"labels\":{" : ",";
        patch += "\"" + kv.first + "\":null";
        std::cout << "[KUBELET] node label " << kv.first << " removed" << std::endl;
    }
    if (!patch.empty()) api_client_->patch_node_status(config_.node_name, patch + "}}}");
}

bool Kubelet::set_api_server(const std::string& host, uint16_t port) {
    if (!api_client_->set_server(host, port)) return false;
    osal::LockGuard guard(api_server_mutex_);
    config_.api_server_host = host;
    config_.api_server_port = port;
    return true;
}

void Kubelet::refresh_node_ip() {
    if (config_.node_ip_provider) {
        std::string ip = config_.node_ip_provider();
        if (!ip.empty() && ip != "0.0.0.0" && ip != config_.node_ip) {
            config_.node_ip = ip;
            status_reporter_->set_node_ip(ip);
            node_address_reported_ = false;
            std::cout << "[KUBELET] node address is now " << ip << std::endl;
        }
    }
    // Heartbeats carry no addresses, and registering over an existing Node
    // can keep the old ones: write the address once after a start and after
    // each change, so that clients that dial the node reach this board
    if (node_address_reported_ || config_.node_ip.empty() || config_.node_ip == "0.0.0.0") return;
    std::string patch = "{\"status\":{\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"";
    patch += config_.node_ip;
    patch += "\"}]}}";
    node_address_reported_ = api_client_->patch_node_status(config_.node_name, patch);
}

void Kubelet::report_heartbeat() {
    refresh_node_ip();
    uint64_t now_ms = osal::System::get_uptime_ms();
    // renewTime carries the real wall clock, or nothing while it is unset
    // The lease outlives three missed heartbeats: the control plane marks the
    // node NotReady once it expires (NodeLifecycleController)
    const uint32_t lease_s = std::max<uint32_t>(15, (3 * config_.heartbeat_interval_ms + 999) / 1000);
    char renew_time[32];
    const size_t renew_len = models::now_micro_time(renew_time, sizeof(renew_time));
    api_client_->renew_node_lease(config_.node_name, lease_s, std::string_view(renew_time, renew_len));

    NodeTelemetry t = status_reporter_->collect_telemetry();
    fill_ready_condition(t);
    fill_firmware_condition(t);
    bool memory_pressure = (t.free_heap_bytes < config_.memory_pressure_threshold_bytes);
    time_conditions(t, &memory_pressure);
    // Written on this thread's stack; a patch that does not fit (a board
    // with an unusual set of resources, or one whose resources changed) is
    // built on the heap instead. A node with every device grant, PSRAM and
    // the FirmwareUpdate condition, all with their times, needs about 1.2 KB.
    char patch_buf[1536];
    mem::FixedText patch(patch_buf);
    const bool patched = status_reporter_->write_heartbeat_patch_json(patch, t, memory_pressure)
        ? api_client_->patch_node_status(config_.node_name, patch.view())
        : api_client_->patch_node_status(config_.node_name, status_reporter_->build_heartbeat_patch_json(t, memory_pressure));
    if (!patched) {
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
        status_reporter_->set_node_resources(NodeStatusReporter::resource_bits(t));
        push_metrics_sample();
    }
    if (firmware_) firmware_->on_heartbeat(patched, t.ready, now_ms);
    last_heartbeat_ms_ = now_ms;
}

// A change is noted on the uptime clock, so its time is known in wall-clock
// time once the clock is set, also when it happened before
void Kubelet::time_conditions(NodeTelemetry& t, const bool* memory_pressure) {
    const uint64_t up = osal::System::get_uptime_ms();
    const uint64_t unix_now = osal::System::get_unix_time_ms();
    auto since = [&](ConditionClock& c, bool status) -> uint64_t {
        if (c.status != static_cast<int8_t>(status)) {
            c.status = static_cast<int8_t>(status);
            c.since_ms = up;
        }
        const uint64_t ago = up - c.since_ms;
        return unix_now > ago ? unix_now - ago : 0;
    };
    osal::LockGuard guard(condition_mutex_);
    t.heartbeat_unix_ms = unix_now;
    t.ready_transition_unix_ms = since(ready_clock_, t.ready);
    if (memory_pressure) t.memory_pressure_transition_unix_ms = since(memory_pressure_clock_, *memory_pressure);
    if (t.firmware_reported) t.firmware_transition_unix_ms = since(firmware_clock_, t.firmware_updating);
}

void Kubelet::fill_firmware_condition(NodeTelemetry& t) const {
    if (!firmware_) return;
    const FirmwareConditionView c = firmware_->condition();
    t.firmware_reported = true;
    t.firmware_updating = c.updating;
    std::memcpy(t.firmware_reason, c.reason, sizeof(t.firmware_reason));
    std::memcpy(t.firmware_message, c.message, sizeof(t.firmware_message));
}

void Kubelet::fill_ready_condition(NodeTelemetry& t) const {
    const uint64_t last = last_sync_ok_ms_.load();
    const uint64_t now = osal::System::get_uptime_ms();
    if (last == 0) {
        t.ready = false;
        t.ready_reason = "KubeletNotReady";
        snprintf(t.ready_message, sizeof(t.ready_message), "kubelet has not completed a pod sync yet");
    } else if (now > last && now - last > config_.sync_stall_threshold_ms) {
        t.ready = false;
        t.ready_reason = "KubeletNotReady";
        snprintf(t.ready_message, sizeof(t.ready_message), "pod sync loop last completed %llus ago",
                 static_cast<unsigned long long>((now - last) / 1000));
    } else {
        t.ready = true;
        t.ready_reason = "KubeletReady";
        snprintf(t.ready_message, sizeof(t.ready_message), "kubelet is posting ready status");
    }
}

// ,"lastState":{...} of the container's last termination, "" when none is known
static std::string last_state_json(const LocalPodRecord& rec) {
    if (!rec.has_last_state) return std::string();
    return ",\"lastState\":{\"terminated\":{\"exitCode\":" + std::to_string(rec.last_exit_code) + ",\"reason\":\"" + rec.last_reason + "\"" +
           (rec.last_message.empty() ? std::string() : ",\"message\":\"" + json_escape(rec.last_message) + "\"") +
           (rec.last_started_at.empty() ? std::string() : ",\"startedAt\":\"" + rec.last_started_at + "\"") +
           (rec.last_finished_at.empty() ? std::string() : ",\"finishedAt\":\"" + rec.last_finished_at + "\"") + "}}";
}

// A pod this kubelet has no record of, after its node restarted: the
// container history the pod's status records stands in for one. The restart
// count goes on from there, and a container that ran before is restarted
// (counted, as Kubernetes counts a container a node reboot ended). One that
// was running when the node went down has no exit code to tell: it is
// recorded as Kubernetes records a container its kubelet lost.
static bool history_from_status(const models::KubePod& pod, LocalPodRecord& out) {
    if (!pod.container_ran && pod.start_time.empty()) return false;
    out.restart_count = pod.first_restart_count;
    out.restart_due = pod.container_ran;
    out.start_time_set = !pod.start_time.empty();
    if (pod.container_running) {
        out.has_last_state = true;
        out.last_exit_code = 137;
        out.last_reason = "ContainerStatusUnknown";
        out.last_message = "The container was running when its node restarted";
        out.last_started_at = pod.container_started_at;
    } else if (pod.has_last_state) {
        out.has_last_state = true;
        out.last_exit_code = pod.last_exit_code;
        out.last_reason = pod.last_reason;
        out.last_message = pod.last_message;
        out.last_started_at = pod.last_started_at;
        out.last_finished_at = pod.last_finished_at;
    }
    return true;
}

// The pod key "namespace/name" in `buf`
static std::string_view pod_key_view(std::string_view ns, std::string_view name, char* buf, size_t cap) {
    if (ns.size() + 1 + name.size() > cap) return std::string_view();
    std::memcpy(buf, ns.data(), ns.size());
    buf[ns.size()] = '/';
    std::memcpy(buf + ns.size() + 1, name.data(), name.size());
    return std::string_view(buf, ns.size() + 1 + name.size());
}

// A pod's labels as listed, hashed (FNV-1a): their JSON text when the client
// read the fields in place, the parsed map otherwise
static uint32_t labels_hash(const PodListEntry& e) {
    uint32_t h = 2166136261u;
    auto add = [&h](std::string_view s) {
        for (unsigned char c : s) { h ^= c; h *= 16777619u; }
        h ^= 0xFF;
        h *= 16777619u;
    };
    if (e.pod) {
        for (const auto& kv : e.pod->labels) { add(kv.first); add(kv.second); }
    } else {
        add(e.labels);
    }
    return h;
}

static net::LabelMap labels_from(const PodListEntry& e) {
    net::LabelMap out;
    if (e.pod) {
        for (const auto& kv : e.pod->labels) out.emplace_back(kv.first, kv.second);
        return out;
    }
    models::JsonValue doc;
    std::string err;
    if (e.labels.empty() || !models::parse_json(e.labels, doc, err) || !doc.is_object()) return out;
    for (const auto& kv : doc.obj_val) {
        if (kv.second.is_string()) out.emplace_back(kv.first, kv.second.str_val);
    }
    return out;
}

void Kubelet::refresh_network_policies(uint64_t now_ms) {
    net::NodeNetworkPolicies& policies = *config_.network_policies;
    const bool all = now_ms - last_policy_refresh_ms_ >= config_.network_policy_refresh_ms;
    if (all) last_policy_refresh_ms_ = now_ms;
    for (const std::string& ns : policies.namespaces(!all)) {
        net::NamespacePolicies compiled;
        const int status = api_client_->list_items("/apis/networking.k8s.io/v1/namespaces/" + ns + "/networkpolicies", [&](std::string&& item) {
            models::JsonValue doc;
            std::string err;
            if (models::parse_json(item, doc, err) && doc.is_object()) compiled.push_back(net::compile_network_policy(doc));
        });
        // An API server without the kind has no policy to enforce
        if (status == 200 || status == 404) {
            policies.set_policies(ns, std::move(compiled));
        } else if (all) {
            std::cout << "[KUBELET] cannot list the network policies of namespace " << ns << " (http " << status << ")" << std::endl;
        }
    }
}

void Kubelet::sync_assigned_pods() {
    last_sync_ms_ = osal::System::get_uptime_ms();
    const uint32_t pass = ++sync_pass_;
    const uint64_t now_ms = last_sync_ms_;

    // The list is read through first and acted on once it arrived whole. Only
    // the pods to stop or to start are copied out of it (a start parses the
    // pod); a pass over pods that keep running takes nothing from the heap.
    struct PodStart {
        models::KubePod pod;
        std::string manifest;
    };
    std::vector<PodStart> starts;
    bool deferred_start = false;
    std::vector<std::pair<std::string, std::string>> terminating;   // (ns, name), deletionTimestamp set
    char key_buf[320];
    net::NodeNetworkPolicies* policies = config_.network_policies.get();
    const bool listed = api_client_->for_each_node_pod([&](const PodListEntry& e) {
        if (e.node_name != config_.node_name) return;
        const std::string_view key = pod_key_view(e.namespace_name, e.name, key_buf, sizeof(key_buf));
        // The labels NetworkPolicy selects this pod by, parsed only when
        // they changed
        if (policies) {
            const uint32_t hash = labels_hash(e);
            if (!policies->pod_current(key, hash, pass)) policies->set_pod(key, hash, labels_from(e), pass);
        }
        // Pods carrying a deletionTimestamp are to be stopped and then
        // finalized by us
        if (e.deleting) {
            terminating.emplace_back(std::string(e.namespace_name), std::string(e.name));
            return;
        }
        bool start = false;
        {
            osal::LockGuard guard(state_mutex_);
            auto it = active_pods_.find(key);
            if (it == active_pods_.end()) {
                // A pod that already Succeeded or Failed is never started
                // again, also not after this node or the control plane restarted
                start = !(e.phase == "Succeeded" || e.phase == "Failed");
            } else {
                LocalPodRecord& rec = it->second;
                rec.listed_pass = pass;
                const PodExecutionPhase phase = rec.execution_phase;
                if (!rec.uid.empty() && !e.uid.empty() && rec.uid != e.uid) {
                    start = true;   // deleted and created again under the same name
                } else if ((phase == PodExecutionPhase::CrashLoopBackOff || phase == PodExecutionPhase::CreateContainerConfigError ||
                            phase == PodExecutionPhase::InsufficientMemory || phase == PodExecutionPhase::BusInUse) &&
                           now_ms >= rec.next_restart_allowed_ms) {
                    // A pod waiting for memory is tried again once the room
                    // admission asked for exists; until then the wait goes on
                    // without parsing the pod
                    if (phase == PodExecutionPhase::InsufficientMemory && rec.memory_need_bytes > 0 &&
                        kube32_pool_pod_room(rec.memory_limit_bytes, nullptr) < rec.memory_need_bytes) {
                        rec.next_restart_allowed_ms = now_ms + config_.memory_retry_ms;
                    } else {
                        start = true;
                    }
                }
            }
        }
        if (start && (config_.max_starts_per_sync == 0 || starts.size() < config_.max_starts_per_sync)) {
            starts.push_back(PodStart{e.pod ? *e.pod : models::parse_pod_manifest(std::string(e.manifest)), std::string(e.manifest)});
        } else if (start) {
            deferred_start = true;
        }
    });
    // Pods left for the next pass start on the next loop turn, not after
    // sync_frequency_ms: one at a time, without the wait in between
    if (deferred_start) sync_due_.store(true);
    if (!listed) {
        return;
    }
    // Before any pod starts: a pod's calls are checked against the policies
    // of its namespace from its first instruction
    if (policies) {
        policies->retain(pass);
        refresh_network_policies(now_ms);
    }

    // Started in key order, as the API server lists them
    std::sort(starts.begin(), starts.end(), [](const PodStart& a, const PodStart& b) {
        return std::tie(a.pod.namespace_name, a.pod.name) < std::tie(b.pod.namespace_name, b.pod.name);
    });

    osal::LockGuard guard(state_mutex_);

    repack_pool(now_ms);

    // 0. Graceful deletions: stop the container, flush its log, then finalize
    for (const auto& tp : terminating) {
        std::string key = tp.first + "/" + tp.second;
        if (active_pods_.find(key) != active_pods_.end()) terminate_pod(key);
        api_client_->delete_pod(tp.first, tp.second);
    }

    // 1. Additions, and restarts of containers whose back-off has passed
    for (const auto& st : starts) {
        const std::string key = st.pod.namespace_name + "/" + st.pod.name;
        auto it = active_pods_.find(key);
        if (it != active_pods_.end() && !it->second.uid.empty() && !st.pod.uid.empty() && it->second.uid != st.pod.uid) {
            // Deleted and created again under the same name while this node
            // still had the old pod: the old one stops, the new one starts
            terminate_pod(key);
            it = active_pods_.end();
        }
        if (it == active_pods_.end()) {
            LocalPodRecord history;
            if (admitted_pods() >= config_.max_pods) reject_out_of_pods(st.pod);
            else start_pod_guarded(st.pod, st.manifest, history_from_status(st.pod, history) ? &history : nullptr);
        } else {
            // Tried again once due: a crashed container after its back-off, a
            // container whose ConfigMap or Secret may exist by now, a pod the
            // node may have the memory for by now
            LocalPodRecord previous = it->second;
            active_pods_.erase(it);
            if (previous.execution_phase == PodExecutionPhase::CrashLoopBackOff) {
                std::cout << "[KUBELET] " << key << ": restarting container (restart " << (previous.restart_count + 1) << ")" << std::endl;
            }
            start_pod_guarded(st.pod, st.manifest, &previous);
        }
        if (auto started = active_pods_.find(key); started != active_pods_.end()) started->second.listed_pass = pass;
    }

    // 2. Deletions: local pods the list no longer holds
    std::vector<std::string> to_remove;
    for (const auto& kv : active_pods_) {
        if (kv.second.listed_pass != pass) to_remove.push_back(kv.first);
    }
    for (const auto& key : to_remove) {
        terminate_pod(key);
    }

    // A completed pass is what the node's Ready condition reports on; the
    // first one is announced right away instead of at the next heartbeat.
    const uint64_t done = osal::System::get_uptime_ms();
    if (last_sync_ok_ms_.exchange(done > 0 ? done : 1) == 0) heartbeat_due_.store(true);
}

uint32_t Kubelet::admitted_pods() const {
    uint32_t n = 0;
    for (const auto& kv : active_pods_) {
        const PodExecutionPhase phase = kv.second.execution_phase;
        if (phase != PodExecutionPhase::Completed && phase != PodExecutionPhase::Failed) ++n;
    }
    return n;
}

void Kubelet::reject_out_of_pods(const models::KubePod& pod) {
    LocalPodRecord rec;
    rec.name = pod.name;
    rec.namespace_name = pod.namespace_name;
    rec.uid = pod.uid;
    rec.image = pod.image;
    rec.container_name = pod.container_name.empty() ? pod.name : pod.container_name;
    rec.execution_phase = PodExecutionPhase::Failed;
    rec.last_error_message = "Pod was rejected: Node didn't have enough resource: pods, requested: 1, used: " +
                             std::to_string(admitted_pods()) + ", capacity: " + std::to_string(config_.max_pods);
    active_pods_[pod.namespace_name + "/" + pod.name] = rec;
    std::cout << "[KUBELET] " << rec.namespace_name << "/" << rec.name << ": " << rec.last_error_message << std::endl;
    api_client_->patch_pod_status(pod.namespace_name, pod.name,
                                  "{\"status\":{\"phase\":\"Failed\",\"reason\":\"OutOfpods\",\"message\":\"" + json_escape(rec.last_error_message) +
                                  "\",\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"PodFailed\"}]}}");
    record_pod_event(rec, "OutOfpods", rec.last_error_message, true);
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

// A pod whose module failed to load twice for memory, on a node without
// PSRAM, where pods share one pool block: the pods already loaded hold
// pieces of it, and a module only fits whole into a block they leave in one
// piece. Pods with a smaller memory limit than the waiting one are stopped,
// once for that pod, so it loads first; they start again after it. A bigger
// pod is never stopped for a smaller one.
void Kubelet::repack_pool(uint64_t now_ms) {
    if (osal::System::get_total_psram_bytes() > 0) return;
    for (auto& kv : active_pods_) {
        LocalPodRecord& waiting = kv.second;
        if (waiting.execution_phase != PodExecutionPhase::InsufficientMemory || waiting.load_failures < 2 || waiting.repacked) continue;
        waiting.repacked = true;
        std::vector<std::string> smaller;
        for (const auto& other : active_pods_) {
            if (other.first != kv.first && other.second.execution_phase == PodExecutionPhase::Running &&
                other.second.memory_limit_bytes < waiting.memory_limit_bytes) {
                smaller.push_back(other.first);
            }
        }
        if (smaller.empty()) continue;
        std::cout << "[KUBELET] " << kv.first << ": its module does not fit beside the pods loaded before it; stopping "
                  << smaller.size() << " smaller pod(s) so it loads first" << std::endl;
        for (const auto& key : smaller) {
            LocalPodRecord& rec = active_pods_[key];
            runtime_.terminate(key);
            hal::CapabilityFirewall::instance().unregister_pod(key);
            rec.restart_due = true;
            wait_for_memory(key, rec, "stopped so that " + kv.first + " loads first", nullptr);
            // Back once the waiting pod had its turn
            rec.next_restart_allowed_ms = now_ms + config_.memory_retry_ms * 4;
        }
        waiting.next_restart_allowed_ms = now_ms;
        return;   // one pod per pass
    }
}

bool Kubelet::wait_for_memory(const std::string& pod_key, LocalPodRecord& rec, const std::string& why, const LocalPodRecord* previous) {
    return wait_for(pod_key, rec, PodExecutionPhase::InsufficientMemory, "InsufficientMemory", why, previous);
}

// A module whose bytes found no room in pod memory: the pod waits as one
// whose load ran out of memory does
bool Kubelet::wait_for_module_memory(const std::string& pod_key, LocalPodRecord& rec, const std::string& why, const LocalPodRecord* previous,
                                     bool restarting) {
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
    if (previous) {
        rec.restart_count = previous->restart_count;
        rec.restart_due = previous->restart_due || restarting;
        rec.repacked = previous->repacked;
    }
    const uint8_t failures = previous ? previous->load_failures : 0;
    rec.load_failures = failures < 255 ? failures + 1 : failures;
    return wait_for_memory(pod_key, rec, why, previous);
}

bool Kubelet::wait_for(const std::string& pod_key, LocalPodRecord& rec, PodExecutionPhase phase, const char* reason, const std::string& why,
                       const LocalPodRecord* previous) {
    rec.execution_phase = phase;
    rec.last_error_message = why;
    rec.next_restart_allowed_ms = osal::System::get_uptime_ms() + config_.memory_retry_ms;
    if (previous && previous->execution_phase == phase) {
        // Still no room: the status already says so; the log repeats why once a minute
        const uint64_t now = osal::System::get_uptime_ms();
        rec.last_wait_log_ms = previous->last_wait_log_ms;
        if (now - rec.last_wait_log_ms >= 60000) {
            rec.last_wait_log_ms = now;
            std::cout << "[KUBELET] " << pod_key << ": still waiting: " << why << std::endl;
        }
        active_pods_[pod_key] = rec;
        return false;
    }
    rec.last_wait_log_ms = osal::System::get_uptime_ms();
    active_pods_[pod_key] = rec;
    std::cout << "[KUBELET] " << pod_key << ": " << why << "; waiting" << std::endl;
    const std::string message = why + "; retrying every " + std::to_string(config_.memory_retry_ms / 1000) + " s";
    std::string status_patch = "{\"status\":{\"phase\":\"Pending\","
                               "\"conditions\":[{\"type\":\"PodScheduled\",\"status\":\"True\"},"
                               "{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"},"
                               "{\"type\":\"ContainersReady\",\"status\":\"False\",\"reason\":\"ContainersNotReady\"}],"
                               "\"containerStatuses\":[{\"name\":\"" + rec.container_name + "\",\"image\":\"" + rec.image +
                               "\",\"ready\":false,\"started\":false,\"restartCount\":" + std::to_string(rec.restart_count) + ","
                               "\"state\":{\"waiting\":{\"reason\":\"" + reason + "\",\"message\":\"" + json_escape(message) + "\"}}" +
                               last_state_json(rec) + "}]}}";
    api_client_->patch_pod_status(rec.namespace_name, rec.name, status_patch);
    record_pod_event(rec, reason, message, true);
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
        rec.has_last_state = previous->has_last_state;
        rec.last_exit_code = previous->last_exit_code;
        rec.last_reason = previous->last_reason;
        rec.last_message = previous->last_message;
        rec.last_started_at = previous->last_started_at;
        rec.last_finished_at = previous->last_finished_at;
        rec.started_at_ms = previous->started_at_ms;
        rec.start_time_set = previous->start_time_set;
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
        bool module_held = true;   // read into pod memory, unless embedded (mapped from flash)
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
            module_held = false;
        } else if (auto ann = pod.annotations.find("kube32.io/wasm-base64"); ann != pod.annotations.end()) {
            module_len = ann->second.size() / 4 * 3;   // empty in pod lists: then only the rest is estimated
        } else if (auto size = pod.annotations.find("kube32.io/wasm-size");
                   size != pod.annotations.end() && pod.annotations.count("kube32.io/wasm-sha256")) {
            module_len = static_cast<size_t>(models::parse_quantity(size->second));   // in the control plane's image store
        }
        size_t linear = 0;
        const size_t need = runtime::estimate_pod_memory(module, module_len, rec.memory_limit_bytes, wasm_stack, !wasm_stack_set,
                                                         runtime::EstimateSizes::Board, &linear, module_held);
        size_t capacity = 0;
        const size_t room = kube32_pool_pod_room(rec.memory_limit_bytes, &capacity);
        const size_t linear_max = kube32_pool_linear_max();
        const std::string reserve = osal::System::get_total_psram_bytes() > 0
            ? kib(kube32_pool_system_reserve()) + " of internal RAM and " + kib(kube32_pool_psram_reserve()) + " of PSRAM"
            : kib(kube32_pool_system_reserve());
        if (need > capacity || linear > linear_max) {
            const std::string why = need > capacity
                ? "Pod was rejected: Node didn't have enough resource: memory, needs about " + kib(need) +
                      ", the node can give pods at most " + kib(capacity) + " (" + reserve + " kept for the system)"
                : "Pod was rejected: Node didn't have enough resource: memory, its linear memory of " + kib(linear) +
                      " needs one piece, the largest the node can give is " + kib(linear_max);
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
            rec.memory_need_bytes = static_cast<uint32_t>(need);
            return wait_for_memory(pod_key, rec, "not enough memory to start the container: it needs about " + kib(need) +
                                                 ", pods can be given " + kib(room) + " now (" + reserve + " kept for the system)", previous);
        }
    }

    // Register capabilities with firewall. A start that failed half way (out
    // of memory) may have left the pod's claims behind: they are its own.
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
    std::string firewall_err;
    bool busy = false;
    if (!hal::CapabilityFirewall::instance().register_pod(pod_key, manifest_json, firewall_err, &busy)) {
        // An I2C bus claimed whole while the pod shares it (or the other way
        // round) is free once the other pods go: the pod waits, Pending
        if (busy) {
            if (previous) {
                rec.restart_count = previous->restart_count;
                rec.restart_due = previous->restart_due || restarting;
            }
            return wait_for(pod_key, rec, PodExecutionPhase::BusInUse, "BusInUse", firewall_err, previous);
        }
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
        ectx.node_port = config_.kubelet_port;
        // The control plane's kubelet reaches the API on loopback; pods are
        // given the address the rest of the network uses.
        {
            osal::LockGuard guard(api_server_mutex_);
            ectx.api_host = (config_.api_server_host == "127.0.0.1" || config_.api_server_host == "localhost") ? config_.node_ip : config_.api_server_host;
            ectx.api_port = config_.api_server_port;
        }
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
                                       "\"state\":{\"waiting\":{\"reason\":\"CreateContainerConfigError\",\"message\":\"" + json_escape(env_err) + "\"}}" +
                                       last_state_json(rec) + "}]}}";
            api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
            record_pod_event(rec, "Failed", "Error: " + env_err, true);
            return false;
        }
    }

    // Resolve the container image: the card for wasm://sd/..., else the
    // embedded registry, else the control plane's copy of an inline image
    // (kube32.io/wasm-sha256), else the inline annotation itself
    const uint8_t* image_bytes = nullptr;
    size_t image_len = 0;
    runtime::ModuleBytes inline_bytes(rec.memory_limit_bytes);   // pod memory the runtime takes over
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
            if (runtime::module_error_is_out_of_memory(why)) return wait_for_module_memory(pod_key, rec, why, previous, restarting);
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
        } else if (auto digest = pod.annotations.find("kube32.io/wasm-sha256");
                   digest != pod.annotations.end() && !pod.annotations.count("kube32.io/wasm-base64")) {
            // An inline image the API server keeps apart from the pod
            // (api/inline_images.hpp): read by its digest
            std::string why;
            const uint64_t t_pull = osal::System::get_uptime_ms();
            if (api_client_->get_image(digest->second, inline_bytes, why)) {
                image_bytes = inline_bytes.data();
                image_len = inline_bytes.size();
                image_source = "control plane";
                record_pod_event(rec, "Pulled", "Successfully pulled image \"" + pod.image + "\" in " +
                                                std::to_string(osal::System::get_uptime_ms() - t_pull) + "ms (" + std::to_string(image_len) +
                                                " bytes, control plane, sha256:" + digest->second.substr(0, 12) + ")", false);
                std::cout << "[KUBELET] " << pod_key << ": image sha256:" << digest->second.substr(0, 12) << ", " << image_len << " bytes" << std::endl;
            } else {
                std::cout << "[KUBELET] " << pod_key << ": image sha256:" << digest->second.substr(0, 12) << ": " << why << std::endl;
                if (runtime::module_error_is_out_of_memory(why)) return wait_for_module_memory(pod_key, rec, why, previous, restarting);
                image_error = why;
            }
        } else {
            auto ann = pod.annotations.find("kube32.io/wasm-base64");
            if (ann != pod.annotations.end()) {
                // Pod lists carry the annotation empty: the module comes with
                // the pod itself, decoded as it streams in
                std::string why;
                bool ok = ann->second.empty() ? api_client_->get_pod_image(pod.namespace_name, pod.name, inline_bytes, why)
                                              : runtime::base64_decode(ann->second, inline_bytes, why);
                if (ok) {
                    image_bytes = inline_bytes.data();
                    image_len = inline_bytes.size();
                    image_source = "inline annotation";
                    uint32_t crc = 0xFFFFFFFFu;
                    for (uint8_t b : inline_bytes) {
                        crc ^= b;
                        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                    }
                    char crc_hex[12];
                    snprintf(crc_hex, sizeof(crc_hex), "%08x", static_cast<unsigned>(~crc));
                    std::cout << "[KUBELET] " << pod_key << ": inline image, " << image_len << " bytes, crc32 " << crc_hex << std::endl;
                } else if (!why.empty()) {
                    std::cout << "[KUBELET] " << pod_key << ": inline image: " << why << std::endl;
                    if (runtime::module_error_is_out_of_memory(why)) return wait_for_module_memory(pod_key, rec, why, previous, restarting);
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
    if (image_source != "sd card" && image_source != "control plane") {
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
    spec.container_name = rec.container_name;
    spec.migration_blockers = runtime::static_migration_blockers(manifest_json);
    // kube32.io/restore-from: the container starts from that Checkpoint's
    // snapshot (docs/migration.md), streamed from the control plane straight
    // into its memory. A container that restarts later starts fresh.
    if (auto it = pod.annotations.find("kube32.io/restore-from"); it != pod.annotations.end() && !it->second.empty() && !restarting) {
        rec.restored_from = it->second;
        spec.restore_from = app::checkpoint_data_stream(*api_client_, pod.namespace_name, it->second);
    }
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
    // kube32.io/tick-interval: k32_tick period, "250ms", "2s" or milliseconds,
    // 50 ms to 1 h (default 1 s)
    {
        auto it = pod.annotations.find("kube32.io/tick-interval");
        if (it != pod.annotations.end()) {
            const std::string& v = it->second;
            char* end = nullptr;
            unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            const std::string unit = end ? std::string(end) : std::string();
            if (unit == "s") n *= 1000;
            if (end != v.c_str() && (unit.empty() || unit == "ms" || unit == "s") && n >= 50 && n <= 3600000) {
                spec.tick_interval_ms = static_cast<uint32_t>(n);
            } else {
                std::cout << "[KUBELET] " << pod_key << ": ignoring kube32.io/tick-interval \"" << v << "\" (want 50ms to 1h, e.g. \"250ms\" or \"2s\")" << std::endl;
            }
        }
    }
    // Modules read into pod memory move into the runtime (no second copy);
    // embedded images are used in place
    bool deployed = inline_bytes.empty()
        ? runtime_.deploy_workload_ex(pod_key, image_bytes, image_len, spec, deploy_err)
        : runtime_.deploy_workload_owned(pod_key, std::move(inline_bytes), spec, deploy_err);
    if (!deployed && runtime::deploy_error_is_restore(deploy_err)) {
        // The control plane falls back (restores elsewhere, or starts the pod
        // fresh): a container that cannot restore is not restarted here
        std::cout << "[KUBELET] " << pod_key << ": " << deploy_err << std::endl;
        hal::CapabilityFirewall::instance().unregister_pod(pod_key);
        LocalPodRecord& stored = (active_pods_[pod_key] = rec);
        handle_pod_failure(stored, deploy_err, 128, "RestoreError");
        return false;
    }
    if (!deployed && runtime::deploy_error_is_out_of_memory(deploy_err)) {
        hal::CapabilityFirewall::instance().unregister_pod(pod_key);
        if (previous) {
            rec.restart_count = previous->restart_count;
            rec.restart_due = previous->restart_due || restarting;
            rec.repacked = previous->repacked;
        }
        const uint8_t failures = previous ? previous->load_failures : 0;
        rec.load_failures = failures < 255 ? failures + 1 : failures;
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
    record_pod_event(rec, "Started", "Started container " + rec.container_name +
                                     (rec.restored_from.empty() ? std::string() : " from checkpoint " + rec.restored_from), false);

    // Report status to API server. Times come from the wall clock and are
    // omitted while it is unset.
    const std::string now = models::now_rfc3339();
    // startTime: when the pod first started, kept over restarts and node restarts
    const bool send_start_time = !rec.start_time_set && !now.empty();
    rec.execution_phase = PodExecutionPhase::Running;
    rec.started_at_ms = osal::System::get_uptime_ms();
    rec.started_at = now;
    rec.start_time_set = rec.start_time_set || send_start_time;
    active_pods_[pod_key] = rec;

    std::string status_patch;
    status_patch.reserve(512);
    status_patch += "{\"status\":{";
    status_patch += "\"phase\":\"Running\",";
    status_patch += "\"podIP\":\"" + rec.pod_ip + "\",";
    status_patch += "\"hostIP\":\"" + config_.node_ip + "\",";
    if (send_start_time) status_patch += "\"startTime\":\"" + now + "\",";
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
    status_patch += last_state_json(rec);
    status_patch += "}]";
    status_patch += "}}";

    api_client_->patch_pod_status(pod.namespace_name, pod.name, status_patch);
    return true;
}

bool Kubelet::terminate_pod(const std::string& pod_key) {
    std::cout << "[KUBELET] " << pod_key << ": terminating" << std::endl;
    // The container's memory is given back before anything is reported: on
    // a node out of memory the report may fail, the stop must not
    runtime_.terminate(pod_key);
    hal::CapabilityFirewall::instance().unregister_pod(pod_key);
    auto it = active_pods_.find(pod_key);
    if (it != active_pods_.end()) {
        try {
            record_pod_event(it->second, "Killing", "Stopping container " + it->second.container_name, false);
            // The "container stopped" line is the ring's last record; flush it
            ship_pod_logs_locked(pod_key, it->second);
        } catch (const std::bad_alloc&) {
            std::cout << "[KUBELET] " << pod_key << ": stopped, not reported (out of memory)" << std::endl;
        }
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
    rec.has_last_state = true;
    rec.last_exit_code = exit_code;
    rec.last_reason = reason;
    rec.last_message = message;
    rec.last_started_at = rec.started_at;
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
    status_patch += "\"state\":{\"waiting\":{\"reason\":\"CrashLoopBackOff\",\"message\":\"" + json_escape(backoff) + "\"}}";
    status_patch += last_state_json(rec);
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
            } else if (const bool frozen = runtime::AppPodHost::instance().is_frozen(pod_key); frozen != rec.frozen) {
                // Frozen for a checkpoint (or a move): not Ready, so Services
                // stop sending it requests; Ready again once thawed
                const char* st = frozen ? "False\",\"reason\":\"ContainersNotReady\",\"message\":\"container frozen for a checkpoint" : "True";
                char patch[400];
                snprintf(patch, sizeof(patch), "{\"status\":{\"conditions\":[{\"type\":\"PodScheduled\",\"status\":\"True\"},"
                         "{\"type\":\"Initialized\",\"status\":\"True\"},{\"type\":\"ContainersReady\",\"status\":\"%s\"},"
                         "{\"type\":\"Ready\",\"status\":\"%s\"}]}}", st, st);
                if (api_client_->patch_pod_status(rec.namespace_name, rec.name, patch)) rec.frozen = frozen;
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

static void append_uint(mem::FixedText& out, uint64_t v) {
    char buf[24];
    out.append(buf, static_cast<size_t>(std::to_chars(buf, buf + sizeof(buf), v).ptr - buf));
}

// The sample is written on this thread's stack: nothing is taken from the
// heap. Room for about ten pods; a larger sample is not sent.
void Kubelet::push_metrics_sample() {
    if (!metrics_) return;
    NodeResourceSample node;
    bool have_node = metrics_->sample_node(node);
    const uint64_t wall_now_us = osal::Thread::get_time_us();

    char body_buf[1536];
    mem::FixedText body(body_buf);
    body += "{\"node\":\"";
    body += config_.node_name;
    body += "\"";
    char ts[32];
    const uint64_t unix_ms = osal::System::get_unix_time_ms();
    const size_t ts_len = unix_ms ? models::format_rfc3339(unix_ms, ts, sizeof(ts)) : 0;
    if (ts_len) {
        body += ",\"timestamp\":\"";
        body.append(ts, ts_len);
        body += "\"";
    }
    // usage and window_ms follow; the window is known once the CPU sample is
    uint64_t window_ms = 0;
    bool have_cpu = false;
    uint64_t nanocores = 0;
    if (have_node) {
        if (node.has_cpu && has_node_sample_ && last_node_sample_.has_cpu && node.cpu_wall_us > last_node_sample_.cpu_wall_us) {
            uint64_t dbusy = node.cpu_busy_us - last_node_sample_.cpu_busy_us;
            uint64_t dwall = node.cpu_wall_us - last_node_sample_.cpu_wall_us;
            nanocores = (dbusy * 1000000000ULL) / dwall;
            have_cpu = true;
            window_ms = dwall / 1000ULL;
        }
        last_node_sample_ = node;
        has_node_sample_ = true;
    }
    if (window_ms == 0) window_ms = (last_heartbeat_ms_ > 0) ? (osal::System::get_uptime_ms() - last_heartbeat_ms_) : 0;
    body += ",\"window_ms\":";
    append_uint(body, window_ms);
    body += ",\"usage\":{";
    if (have_cpu) {
        body += "\"cpu_nanocores\":";
        append_uint(body, nanocores);
    }
    if (have_node && node.has_memory) {
        if (have_cpu) body += ",";
        body += "\"memory_bytes\":";
        append_uint(body, node.memory_used_bytes);
    }
    body += "}";

    body += ",\"pods\":[";
    bool first = true;
    {
        osal::LockGuard guard(state_mutex_);
        for (auto& kv : active_pods_) {
            LocalPodRecord& rec = kv.second;
            PodResourceSample ps;
            if (!metrics_->sample_pod(kv.first, runtime_, ps)) continue;
            if (!first) body += ",";
            first = false;
            body += "{\"namespace\":\"";
            body += rec.namespace_name;
            body += "\",\"name\":\"";
            body += rec.name;
            body += "\",\"containers\":[{\"name\":\"";
            body += rec.container_name.empty() ? rec.name : rec.container_name;
            body += "\"";
            if (ps.has_cpu && rec.has_cpu_sample && wall_now_us > rec.last_cpu_wall_us) {
                uint64_t dbusy = ps.cpu_busy_us >= rec.last_cpu_busy_us ? ps.cpu_busy_us - rec.last_cpu_busy_us : 0;
                uint64_t dwall = wall_now_us - rec.last_cpu_wall_us;
                body += ",\"cpu_nanocores\":";
                append_uint(body, (dbusy * 1000000000ULL) / dwall);
            }
            if (ps.has_cpu) {
                rec.has_cpu_sample = true;
                rec.last_cpu_busy_us = ps.cpu_busy_us;
                rec.last_cpu_wall_us = wall_now_us;
            }
            if (ps.has_memory) {
                body += ",\"memory_bytes\":";
                append_uint(body, ps.memory_bytes);
            }
            body += "}]}";
        }
    }
    body += "]}";
    if (body.overflowed()) {
        static bool logged = false;
        if (!logged) std::cout << "[KUBELET] metrics sample over " << sizeof(body_buf) << " bytes, not sent" << std::endl;
        logged = true;
        return;
    }
    api_client_->push_metrics(config_.node_name, body.view());
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
