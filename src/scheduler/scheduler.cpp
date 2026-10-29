#include "scheduler/scheduler.hpp"
#include "api/event_recorder.hpp"
#include "models/kube_models.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <cstdlib>

namespace kube32::scheduler {

uint64_t parse_quantity(const std::string& val) {
    return models::parse_quantity(val);
}

ResourceSpec extract_pod_resource_requests(const std::string& pod_json) {
    ResourceSpec spec;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(pod_json, doc, err) || !doc.is_object()) {
        spec.memory_bytes = 16 * 1024;
        return spec;
    }

    const models::JsonValue* spec_val = doc.get("spec");
    if (!spec_val || !spec_val->is_object()) {
        spec.memory_bytes = 16 * 1024;
        return spec;
    }

    const models::JsonValue* containers = spec_val->get("containers");
    if (!containers || !containers->is_array()) {
        spec.memory_bytes = 16 * 1024;
        return spec;
    }

    uint64_t total_mem = 0;
    uint64_t total_psram = 0;
    uint32_t total_gpio = 0;
    uint32_t total_i2c = 0;
    uint32_t total_uart = 0;
    uint32_t total_adc = 0;

    for (const auto& c : containers->arr_val) {
        if (!c.is_object()) continue;
        const models::JsonValue* res = c.get("resources");
        if (!res || !res->is_object()) continue;

        const models::JsonValue* req = res->get("requests");
        if (!req || !req->is_object()) {
            req = res->get("limits");
        }
        if (!req || !req->is_object()) continue;

        // Memory
        std::string mem_str = req->get_string("memory", "");
        if (!mem_str.empty()) {
            total_mem += parse_quantity(mem_str);
        }

        // PSRAM: esp.io/psram or esp32.io/psram
        std::string psram_str = req->get_string("esp.io/psram", req->get_string("esp32.io/psram", ""));
        if (!psram_str.empty()) {
            total_psram += parse_quantity(psram_str);
        }

        // GPIO: esp.io/gpio or esp32.io/gpio
        std::string gpio_str = req->get_string("esp.io/gpio", req->get_string("esp32.io/gpio", ""));
        if (!gpio_str.empty()) {
            total_gpio += static_cast<uint32_t>(parse_quantity(gpio_str));
        }

        // I2C: esp.io/i2c or esp32.io/i2c
        std::string i2c_str = req->get_string("esp.io/i2c", req->get_string("esp32.io/i2c", ""));
        if (!i2c_str.empty()) {
            total_i2c += static_cast<uint32_t>(parse_quantity(i2c_str));
        }

        // ADC: esp.io/adc or esp32.io/adc
        std::string adc_str = req->get_string("esp.io/adc", req->get_string("esp32.io/adc", ""));
        if (!adc_str.empty()) {
            total_adc += static_cast<uint32_t>(parse_quantity(adc_str));
        }

        // UART: esp.io/uart or esp32.io/uart; a named bus (esp32.io/i2c-bus-N,
        // esp32.io/uart-N) needs one of the node's buses as well
        std::string uart_str = req->get_string("esp.io/uart", req->get_string("esp32.io/uart", ""));
        if (!uart_str.empty()) {
            total_uart += static_cast<uint32_t>(parse_quantity(uart_str));
        }
        for (const auto& kv : req->obj_val) {
            const std::string& k = kv.first;
            if (k.rfind("esp32.io/i2c-bus-", 0) == 0 || k.rfind("esp.io/i2c-bus-", 0) == 0) total_i2c += 1;
            else if (k.rfind("esp32.io/uart-", 0) == 0 || k.rfind("esp.io/uart-", 0) == 0) total_uart += 1;
        }
    }

    spec.memory_bytes = (total_mem > 0) ? total_mem : (16 * 1024);
    spec.psram_bytes = total_psram;
    spec.gpio_pins = total_gpio;
    spec.i2c_buses = total_i2c;
    spec.uart_ports = total_uart;
    spec.adc_channels = total_adc;
    return spec;
}

ResourceSpec extract_node_allocatable(const std::string& node_json) {
    ResourceSpec spec;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(node_json, doc, err) || !doc.is_object()) {
        // No reported status means no known capacity: nothing fits until the
        // kubelet reports what the board actually has.
        return spec;
    }

    const models::JsonValue* status = doc.get("status");
    if (!status || !status->is_object()) {
        // No reported status means no known capacity: nothing fits until the
        // kubelet reports what the board actually has.
        return spec;
    }

    const models::JsonValue* alloc = status->get("allocatable");
    if (!alloc || !alloc->is_object()) {
        alloc = status->get("capacity");
    }
    if (!alloc || !alloc->is_object()) {
        // No reported status means no known capacity: nothing fits until the
        // kubelet reports what the board actually has.
        return spec;
    }

    // A resource the node does not list is a resource it does not have.
    spec.memory_bytes = parse_quantity(alloc->get_string("memory", ""));
    spec.gpio_pins = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/gpio", alloc->get_string("esp32.io/gpio", ""))));
    spec.i2c_buses = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/i2c", alloc->get_string("esp32.io/i2c", ""))));
    spec.uart_ports = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/uart", alloc->get_string("esp32.io/uart", ""))));
    spec.adc_channels = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/adc", alloc->get_string("esp32.io/adc", ""))));
    spec.psram_bytes = parse_quantity(alloc->get_string("esp.io/psram", alloc->get_string("esp32.io/psram", "")));

    return spec;
}

static uint64_t resource_version_of(const models::JsonValue& doc) {
    const models::JsonValue* meta = doc.get("metadata");
    std::string rv = meta ? meta->get_string("resourceVersion", "") : "";
    return rv.empty() ? 0 : std::strtoull(rv.c_str(), nullptr, 10);
}

static bool container_running(const models::JsonValue* status) {
    const models::JsonValue* css = status ? status->get("containerStatuses") : nullptr;
    if (!css || !css->is_array() || css->arr_val.empty()) return false;
    for (const auto& cs : css->arr_val) {
        const models::JsonValue* state = cs.get("state");
        if (!state || !state->get("running")) return false;
    }
    return true;
}

// The node's allocatable figures are measured by its kubelet (free heap,
// free PSRAM, unclaimed pins), so pods it was already running when it last
// reported are inside them. What the scheduler still has to subtract are the
// requests of pods bound to the node that the report cannot include yet:
// pods its kubelet has not started (Pending), containers waiting to restart,
// and pods that started after the node's last status write (the store's
// resourceVersion is one global counter, so a pod written after the node
// is newer than its report).
ResourceSpec calculate_node_allocated_resources(storage::Store& store, const std::string& node_name, const std::string& exclude_pod_key) {
    ResourceSpec allocated;
    uint64_t node_rv = 0;
    {
        std::string node_json;
        models::JsonValue node_doc;
        std::string err;
        if (store.get("/registry/nodes/" + node_name, node_json).ok() && models::parse_json(node_json, node_doc, err)) {
            node_rv = resource_version_of(node_doc);
        }
    }
    std::vector<std::string> pod_jsons;
    std::vector<std::string> pod_keys;
    store.list_extended("/registry/pods/", pod_jsons, &pod_keys);

    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        if (!exclude_pod_key.empty() && pod_keys[i] == exclude_pod_key) {
            continue;
        }

        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(pod_jsons[i], doc, err) || !doc.is_object()) continue;

        const models::JsonValue* spec = doc.get("spec");
        if (!spec || !spec->is_object()) continue;

        std::string assigned_node = spec->get_string("nodeName", "");
        if (assigned_node != node_name) continue;

        const models::JsonValue* status = doc.get("status");
        std::string phase = status ? status->get_string("phase", "") : "";
        if (phase != "Running" && phase != "Pending") continue;
        const bool reflected = phase == "Running" && container_running(status) && node_rv > 0 && resource_version_of(doc) < node_rv;
        if (reflected) continue;

        ResourceSpec req = extract_pod_resource_requests(pod_jsons[i]);
        allocated.memory_bytes += req.memory_bytes;
        allocated.gpio_pins += req.gpio_pins;
        allocated.i2c_buses += req.i2c_buses;
        allocated.uart_ports += req.uart_ports;
        allocated.adc_channels += req.adc_channels;
        allocated.psram_bytes += req.psram_bytes;
    }

    return allocated;
}

// =============================================================================
// Standard Predicates Implementations
// =============================================================================

bool NodeSchedulablePredicate::evaluate(const std::string&,
                                       const std::string& node_json,
                                       const ResourceSpec&,
                                       const ResourceSpec&,
                                       const ResourceSpec&,
                                       std::string& failure_reason) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(node_json, doc, err) || !doc.is_object()) {
        failure_reason = "node(s) could not be read";
        return false;
    }

    const models::JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object() && spec->get_bool("unschedulable", false)) {
        failure_reason = "node(s) were unschedulable";
        return false;
    }

    const models::JsonValue* status = doc.get("status");
    if (!status || !status->is_object()) {
        failure_reason = "node(s) were not ready";
        return false;
    }

    const models::JsonValue* conds = status->get("conditions");
    if (!conds || !conds->is_array()) {
        failure_reason = "node(s) were not ready";
        return false;
    }

    bool is_ready = false;
    for (const auto& c : conds->arr_val) {
        if (c.is_object() && c.get_string("type") == "Ready") {
            if (c.get_string("status") == "True") {
                is_ready = true;
            }
            break;
        }
    }

    if (!is_ready) {
        failure_reason = "node(s) were not ready";
        return false;
    }
    return true;
}

bool NodeMemoryPredicate::evaluate(const std::string&,
                                   const std::string&,
                                   const ResourceSpec& pod_req,
                                   const ResourceSpec& node_allocatable,
                                   const ResourceSpec& node_allocated,
                                   std::string& failure_reason) {
    uint64_t available = (node_allocatable.memory_bytes >= node_allocated.memory_bytes) 
        ? (node_allocatable.memory_bytes - node_allocated.memory_bytes) : 0;
    if (available < pod_req.memory_bytes) {
        failure_reason = "Insufficient memory";
        return false;
    }
    return true;
}

bool NodePSRAMPredicate::evaluate(const std::string&,
                                  const std::string&,
                                  const ResourceSpec& pod_req,
                                  const ResourceSpec& node_allocatable,
                                  const ResourceSpec& node_allocated,
                                  std::string& failure_reason) {
    if (pod_req.psram_bytes == 0) return true;
    uint64_t available = (node_allocatable.psram_bytes >= node_allocated.psram_bytes)
        ? (node_allocatable.psram_bytes - node_allocated.psram_bytes) : 0;
    if (available < pod_req.psram_bytes) {
        failure_reason = "Insufficient esp32.io/psram";
        return false;
    }
    return true;
}

bool NodePeripheralPredicate::evaluate(const std::string&,
                                       const std::string&,
                                       const ResourceSpec& pod_req,
                                       const ResourceSpec& node_allocatable,
                                       const ResourceSpec& node_allocated,
                                       std::string& failure_reason) {
    uint32_t avail_gpio = (node_allocatable.gpio_pins >= node_allocated.gpio_pins)
        ? (node_allocatable.gpio_pins - node_allocated.gpio_pins) : 0;
    if (avail_gpio < pod_req.gpio_pins) {
        failure_reason = "Insufficient esp32.io/gpio";
        return false;
    }

    uint32_t avail_i2c = (node_allocatable.i2c_buses >= node_allocated.i2c_buses)
        ? (node_allocatable.i2c_buses - node_allocated.i2c_buses) : 0;
    if (avail_i2c < pod_req.i2c_buses) {
        failure_reason = "Insufficient esp32.io/i2c";
        return false;
    }

    uint32_t avail_uart = (node_allocatable.uart_ports >= node_allocated.uart_ports)
        ? (node_allocatable.uart_ports - node_allocated.uart_ports) : 0;
    if (avail_uart < pod_req.uart_ports) {
        failure_reason = "Insufficient esp32.io/uart";
        return false;
    }

    uint32_t avail_adc = (node_allocatable.adc_channels >= node_allocated.adc_channels)
        ? (node_allocatable.adc_channels - node_allocated.adc_channels) : 0;
    if (avail_adc < pod_req.adc_channels) {
        failure_reason = "Insufficient esp32.io/adc";
        return false;
    }

    return true;
}

bool NodeSelectorPredicate::evaluate(const std::string& pod_json,
                                     const std::string& node_json,
                                     const ResourceSpec&,
                                     const ResourceSpec&,
                                     const ResourceSpec&,
                                     std::string& failure_reason) {
    models::JsonValue pod_doc;
    std::string err;
    if (!models::parse_json(pod_json, pod_doc, err) || !pod_doc.is_object()) return true;

    const models::JsonValue* spec = pod_doc.get("spec");
    if (!spec || !spec->is_object()) return true;

    const models::JsonValue* sel = spec->get("nodeSelector");
    if (!sel || !sel->is_object() || sel->obj_val.empty()) return true;

    models::JsonValue node_doc;
    if (!models::parse_json(node_json, node_doc, err) || !node_doc.is_object()) {
        failure_reason = "node(s) could not be read";
        return false;
    }

    const models::JsonValue* meta = node_doc.get("metadata");
    const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;

    for (const auto& kv : sel->obj_val) {
        std::string req_val = kv.second.is_string() ? kv.second.str_val : "";
        // The label must exist: "node-role.kubernetes.io/control-plane: ''"
        // selects nodes that carry it with an empty value, not every node
        const bool present = labels && labels->get(kv.first);
        std::string node_val = present ? labels->get_string(kv.first, "") : "";
        if (!present || node_val != req_val) {
            failure_reason = "node(s) didn't match Pod's node affinity/selector";
            return false;
        }
    }
    return true;
}

bool NodeNamePredicate::evaluate(const std::string& pod_json,
                                 const std::string& node_json,
                                 const ResourceSpec&,
                                 const ResourceSpec&,
                                 const ResourceSpec&,
                                 std::string& failure_reason) {
    models::JsonValue pod_doc;
    std::string err;
    if (!models::parse_json(pod_json, pod_doc, err) || !pod_doc.is_object()) return true;

    const models::JsonValue* spec = pod_doc.get("spec");
    if (!spec || !spec->is_object()) return true;

    std::string node_name_pin = spec->get_string("nodeName", "");
    if (node_name_pin.empty() || node_name_pin == "<none>") return true;

    models::JsonValue node_doc;
    if (!models::parse_json(node_json, node_doc, err) || !node_doc.is_object()) {
        failure_reason = "node(s) could not be read";
        return false;
    }

    const models::JsonValue* meta = node_doc.get("metadata");
    std::string nname = meta ? meta->get_string("name", "") : "";

    if (nname != node_name_pin) {
        failure_reason = "node(s) didn't match the requested node name";
        return false;
    }
    return true;
}

// =============================================================================
// Standard Scorers Implementations
// =============================================================================

int64_t LeastAllocatedScorer::score(const std::string&,
                                   const std::string&,
                                   const ResourceSpec& pod_req,
                                   const ResourceSpec& node_allocatable,
                                   const ResourceSpec& node_allocated) {
    int64_t mem_score = 0;
    if (node_allocatable.memory_bytes > 0) {
        uint64_t remaining = (node_allocatable.memory_bytes >= node_allocated.memory_bytes + pod_req.memory_bytes)
            ? (node_allocatable.memory_bytes - (node_allocated.memory_bytes + pod_req.memory_bytes)) : 0;
        mem_score = static_cast<int64_t>((remaining * 100) / node_allocatable.memory_bytes);
    }

    int64_t gpio_score = 100;
    if (node_allocatable.gpio_pins > 0) {
        uint32_t remaining = (node_allocatable.gpio_pins >= node_allocated.gpio_pins + pod_req.gpio_pins)
            ? (node_allocatable.gpio_pins - (node_allocated.gpio_pins + pod_req.gpio_pins)) : 0;
        gpio_score = static_cast<int64_t>((remaining * 100) / node_allocatable.gpio_pins);
    }

    return (mem_score + gpio_score) / 2;
}

int64_t BestFitScorer::score(const std::string&,
                             const std::string&,
                             const ResourceSpec& pod_req,
                             const ResourceSpec& node_allocatable,
                             const ResourceSpec& node_allocated) {
    if (node_allocatable.memory_bytes == 0) return 0;
    uint64_t remaining = (node_allocatable.memory_bytes >= node_allocated.memory_bytes + pod_req.memory_bytes)
        ? (node_allocatable.memory_bytes - (node_allocated.memory_bytes + pod_req.memory_bytes)) : 0;
    int64_t mem_score = static_cast<int64_t>((remaining * 100) / node_allocatable.memory_bytes);
    return 100 - mem_score;
}

// =============================================================================
// Scheduler Implementation
// =============================================================================

class SchedulerImpl : public Scheduler {
public:
    SchedulerImpl(storage::Store& store, SchedulerConfig config)
        : store_(store), config_(config) {
        // Default filters, in kube-scheduler's order: node state, placement
        // (name, selector), then resources. The first one a node fails is
        // its reason in the FailedScheduling message.
        add_predicate(std::make_shared<NodeSchedulablePredicate>());
        add_predicate(std::make_shared<NodeNamePredicate>());
        add_predicate(std::make_shared<NodeSelectorPredicate>());
        add_predicate(std::make_shared<NodeMemoryPredicate>());
        add_predicate(std::make_shared<NodePSRAMPredicate>());
        add_predicate(std::make_shared<NodePeripheralPredicate>());

        // Register default scorer
        if (config_.strategy == ScoringStrategy::BestFit) {
            add_scorer(std::make_shared<BestFitScorer>(), 1.0f);
        } else {
            add_scorer(std::make_shared<LeastAllocatedScorer>(), 1.0f);
        }
    }

    ~SchedulerImpl() override {
        stop();
    }

    bool start() override {
        if (running_.load()) return true;
        running_.store(true);

        reconcile_thread_ = std::make_unique<osal::Thread>();
        osal::Thread::Config cfg;
        cfg.name = "k32_sched";
        cfg.stack_size = 16384;   // manifest DOM parsing + scoring
        bool started = reconcile_thread_->start([](void* p) {
            auto* self = static_cast<SchedulerImpl*>(p);
            self->run_reconcile_loop();
        }, this, cfg);

        if (!started) {
            running_.store(false);
            return false;
        }
        return true;
    }

    void stop() override {
        if (!running_.load()) return;
        running_.store(false);
        if (reconcile_thread_) {
            reconcile_thread_->join();
            reconcile_thread_.reset();
        }
    }

    bool is_running() const override {
        return running_.load();
    }

    void add_predicate(std::shared_ptr<Predicate> predicate) override {
        osal::LockGuard guard(mutex_);
        predicates_.push_back(std::move(predicate));
    }

    void add_scorer(std::shared_ptr<Scorer> scorer, float weight) override {
        osal::LockGuard guard(mutex_);
        scorers_.emplace_back(std::move(scorer), weight);
    }

    ScheduleResult schedule_pod_manifest(std::string& in_out_pod_json) override {
        osal::LockGuard guard(mutex_);
        return evaluate_and_bind_manifest(in_out_pod_json);
    }

    ScheduleResult schedule_pod(const std::string& pod_name, const std::string& pod_namespace) override {
        std::string pod_key = "/registry/pods/" + pod_namespace + "/" + pod_name;
        std::string pod_json;
        uint64_t version = 0;

        for (int retry = 0; retry < 3; ++retry) {
            ScheduleResult result;
            storage::Status put_s;
            {
                osal::LockGuard guard(mutex_);
                storage::Status s = store_.get_extended(pod_key, pod_json, &version);
                if (!s.ok()) {
                    return {false, "", "Pod not found in store: " + pod_key};
                }

                result = evaluate_and_bind_manifest(pod_json, pod_key);

                uint64_t next_ver = 0;
                put_s = store_.put_extended(pod_key, pod_json, true, true, version, &next_ver);
            }

            if (put_s.ok()) {
                return result;
            }
            if (put_s.code != storage::StatusCode::VersionConflict) {
                return {false, "", "Store error: " + put_s.message};
            }
            osal::Thread::yield();
        }

        return {false, "", "Schedule conflict retries exceeded"};
    }

    void reconcile_cluster() override {
        // 1. Check for ready nodes in cluster registry
        std::vector<std::string> node_jsons;
        store_.list("/registry/nodes/", node_jsons);

        std::set<std::string> ready_nodes;
        std::set<std::string> existing_nodes;
        for (const auto& n_json : node_jsons) {
            osal::Thread::yield();
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(n_json, doc, err) || !doc.is_object()) continue;

            const models::JsonValue* meta = doc.get("metadata");
            std::string nname = meta ? meta->get_string("name", "") : "";
            if (nname.empty()) continue;

            existing_nodes.insert(nname);

            const models::JsonValue* status = doc.get("status");
            const models::JsonValue* conds = status ? status->get("conditions") : nullptr;
            if (conds && conds->is_array()) {
                for (const auto& c : conds->arr_val) {
                    if (c.is_object() && c.get_string("type") == "Ready" && c.get_string("status") == "True") {
                        ready_nodes.insert(nname);
                        break;
                    }
                }
            }
        }

        // 2. Scan all pods in store
        std::vector<std::string> pod_jsons;
        std::vector<std::string> pod_keys;
        store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);

        for (size_t i = 0; i < pod_jsons.size(); ++i) {
            osal::Thread::yield();
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(pod_jsons[i], doc, err) || !doc.is_object()) continue;

            const models::JsonValue* spec = doc.get("spec");
            std::string assigned_node = spec ? spec->get_string("nodeName", "") : "";
            const models::JsonValue* status = doc.get("status");
            std::string phase = status ? status->get_string("phase", "") : "";

            const models::JsonValue* meta = doc.get("metadata");
            std::string pod_name = meta ? meta->get_string("name", "") : "";
            std::string pod_ns = meta ? meta->get_string("namespace", "default") : "default";

            const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
            // DaemonSet pods belong to their node; the DaemonSet controller
            // removes them when the node goes away
            bool is_daemon = models::has_owner(doc, "DaemonSet") || (labels && labels->get_string("kube32.io/daemonset", "") == "true");
            if (is_daemon && !assigned_node.empty() && assigned_node != "<none>") continue;
            if (meta && !meta->get_string("deletionTimestamp", "").empty()) continue; // being torn down

            // Only consider a node "missing" if it was deleted from /registry/nodes/
            bool node_deleted = (!assigned_node.empty() && assigned_node != "<none>" && existing_nodes.count(assigned_node) == 0);

            if (node_deleted) {
                // Node was deleted from cluster: evict and re-schedule!
                schedule_pod(pod_name, pod_ns);
            } else if (assigned_node.empty() || assigned_node == "<none>") {
                // Unbound pod (new, or evicted by NodeLifecycleController). A bound
                // pod stays Pending until its kubelet starts it; it is not re-evaluated.
                (void)phase;
                schedule_pod(pod_name, pod_ns);
            }
        }
    }

private:
    void run_reconcile_loop() {
        while (running_.load()) {
            osal::Thread::sleep_ms(config_.reconcile_interval_ms);
            if (!running_.load()) break;
            // An allocation failure while parsing objects must not take the
            // node down; the next pass retries once memory is back.
            try {
                reconcile_cluster();
            } catch (const std::exception& e) {
                std::cout << "[SCHED] reconcile skipped: " << e.what() << std::endl;
            }
        }
    }

    ScheduleResult evaluate_and_bind_manifest(std::string& in_out_pod_json, const std::string& exclude_pod_key = "") {
        models::JsonValue pod_doc;
        std::string err;
        if (!models::parse_json(in_out_pod_json, pod_doc, err) || !pod_doc.is_object()) {
            return {false, "", "Invalid pod manifest"};
        }

        ResourceSpec pod_req = extract_pod_resource_requests(in_out_pod_json);

        // Fetch all nodes
        std::vector<std::string> node_jsons;
        store_.list("/registry/nodes/", node_jsons);

        struct Candidate {
            std::string name;
            std::string node_json;
            std::string ip{"127.0.0.1"};
            int64_t total_score{0};
        };

        std::vector<Candidate> qualified;
        std::map<std::string, size_t> reasons;   // why nodes were filtered out, with counts

        for (const auto& n_json : node_jsons) {
            models::JsonValue n_doc;
            if (!models::parse_json(n_json, n_doc, err) || !n_doc.is_object()) { reasons["node(s) could not be read"]++; continue; }
            const models::JsonValue* meta = n_doc.get("metadata");
            std::string nname = meta ? meta->get_string("name", "") : "";
            if (nname.empty()) { reasons["node(s) could not be read"]++; continue; }

            ResourceSpec node_alloc = extract_node_allocatable(n_json);
            ResourceSpec node_used = calculate_node_allocated_resources(store_, nname, exclude_pod_key);

            bool passes = true;
            std::string reason;
            for (const auto& pred : predicates_) {
                if (!pred->evaluate(in_out_pod_json, n_json, pod_req, node_alloc, node_used, reason)) {
                    passes = false;
                    reasons[reason.empty() ? std::string(pred->name()) : reason]++;
                    break;
                }
            }

            if (passes) {
                Candidate cand;
                cand.name = nname;
                cand.node_json = n_json;

                const models::JsonValue* status = n_doc.get("status");
                if (status && status->is_object()) {
                    const models::JsonValue* addrs = status->get("addresses");
                    if (addrs && addrs->is_array() && !addrs->arr_val.empty()) {
                        cand.ip = addrs->arr_val[0].get_string("address", "127.0.0.1");
                    }
                }

                // Calculate score
                int64_t score_sum = 0;
                for (const auto& s_entry : scorers_) {
                    score_sum += static_cast<int64_t>(s_entry.first->score(in_out_pod_json, n_json, pod_req, node_alloc, node_used) * s_entry.second);
                }
                cand.total_score = score_sum;
                qualified.push_back(cand);
            }
        }

        if (qualified.empty()) {
            // Unschedulable. The message has kube-scheduler's shape: one count
            // per reason, sorted ("0/2 nodes are available: 1 Insufficient
            // memory, 1 node(s) didn't match Pod's node affinity/selector.").
            std::string message;
            if (node_jsons.empty()) {
                message = "no nodes available to schedule pods";
            } else {
                std::vector<std::string> parts;
                for (const auto& r : reasons) parts.push_back(std::to_string(r.second) + " " + r.first);
                std::sort(parts.begin(), parts.end());
                message = "0/" + std::to_string(node_jsons.size()) + " nodes are available: ";
                for (size_t k = 0; k < parts.size(); ++k) message += (k ? ", " : "") + parts[k];
                message += ".";
            }
            models::JsonValue* spec = pod_doc.get_mut("spec");
            if (spec) spec->set("nodeName", models::JsonValue{models::JsonType::String, false, 0, "", {}, {}});

            models::JsonValue* status_val = pod_doc.get_mut("status");
            if (!status_val || !status_val->is_object()) {
                models::JsonValue new_status;
                new_status.type = models::JsonType::Object;
                pod_doc.set("status", std::move(new_status));
                status_val = pod_doc.get_mut("status");
            }

            status_val->set("phase", models::JsonValue{models::JsonType::String, false, 0, "Pending", {}, {}});

            models::JsonValue* conds_arr = status_val->get_mut("conditions");
            if (!conds_arr || !conds_arr->is_array()) {
                models::JsonValue new_conds;
                new_conds.type = models::JsonType::Array;
                status_val->set("conditions", std::move(new_conds));
                conds_arr = status_val->get_mut("conditions");
            }

            models::JsonValue cond_item;
            cond_item.type = models::JsonType::Object;
            cond_item.set("type", models::JsonValue{models::JsonType::String, false, 0, "PodScheduled", {}, {}});
            cond_item.set("status", models::JsonValue{models::JsonType::String, false, 0, "False", {}, {}});
            cond_item.set("reason", models::JsonValue{models::JsonType::String, false, 0, "Unschedulable", {}, {}});
            cond_item.set("message", models::JsonValue{models::JsonType::String, false, 0, message, {}, {}});

            bool found_sched = false;
            for (auto& c : conds_arr->arr_val) {
                if (c.is_object() && c.get_string("type", "") == "PodScheduled") {
                    c = std::move(cond_item);
                    found_sched = true;
                    break;
                }
            }
            if (!found_sched) {
                conds_arr->arr_val.push_back(std::move(cond_item));
            }

            in_out_pod_json = pod_doc.to_string();
            {
                api::EventSpec ev;
                const models::JsonValue* pmeta = pod_doc.get("metadata");
                ev.involved.kind = "Pod";
                ev.involved.name = pmeta ? pmeta->get_string("name", "") : "";
                ev.involved.namespace_name = pmeta ? pmeta->get_string("namespace", "default") : "default";
                ev.involved.uid = pmeta ? pmeta->get_string("uid", "") : "";
                ev.reason = "FailedScheduling";
                ev.message = message;
                ev.warning = true;
                ev.component = "default-scheduler";
                events_.record(ev);
            }
            return {false, "", message};
        }

        // Sort candidates: highest score first, lexicographical tie-break
        std::sort(qualified.begin(), qualified.end(), [](const Candidate& a, const Candidate& b) {
            if (a.total_score != b.total_score) return a.total_score > b.total_score;
            return a.name < b.name;
        });

        const Candidate& winner = qualified.front();

        // Mutate spec.nodeName
        models::JsonValue* spec = pod_doc.get_mut("spec");
        if (spec) {
            spec->set("nodeName", models::JsonValue{models::JsonType::String, false, 0, winner.name, {}, {}});
        }

        // Status after binding: Pending with PodScheduled=True. Phase, IPs,
        // start time and container states are reported by the kubelet once
        // the workload actually runs; nothing is claimed before then.
        models::JsonValue status_val;
        const models::JsonValue* prev_status = pod_doc.get("status");
        if (prev_status && prev_status->is_object()) status_val = *prev_status;
        else status_val.type = models::JsonType::Object;
        status_val.set("phase", models::JsonValue{models::JsonType::String, false, 0, "Pending", {}, {}});
        status_val.remove("reason");
        status_val.remove("message");

        models::JsonValue conds_arr;
        conds_arr.type = models::JsonType::Array;
        {
            models::JsonValue c;
            c.type = models::JsonType::Object;
            c.set("type", models::JsonValue{models::JsonType::String, false, 0, "PodScheduled", {}, {}});
            c.set("status", models::JsonValue{models::JsonType::String, false, 0, "True", {}, {}});
            std::string now = models::now_rfc3339();
            if (!now.empty()) c.set("lastTransitionTime", models::JsonValue{models::JsonType::String, false, 0, now, {}, {}});
            conds_arr.arr_val.push_back(std::move(c));
        }
        status_val.set("conditions", std::move(conds_arr));
        status_val.remove("containerStatuses");

        {
            api::EventSpec ev;
            const models::JsonValue* pmeta = pod_doc.get("metadata");
            std::string pname = pmeta ? pmeta->get_string("name", "") : "";
            std::string pns = pmeta ? pmeta->get_string("namespace", "default") : "default";
            ev.involved.kind = "Pod";
            ev.involved.name = pname;
            ev.involved.namespace_name = pns;
            ev.involved.uid = pmeta ? pmeta->get_string("uid", "") : "";
            ev.reason = "Scheduled";
            ev.message = "Successfully assigned " + pns + "/" + pname + " to " + winner.name;
            ev.component = "default-scheduler";
            events_.record(ev);
        }

        pod_doc.set("status", std::move(status_val));
        in_out_pod_json = pod_doc.to_string();

        return {true, winner.name, ""};
    }

    storage::Store& store_;
    SchedulerConfig config_;
    api::EventRecorder events_{store_, 64};
    osal::Mutex mutex_;
    std::vector<std::shared_ptr<Predicate>> predicates_;
    std::vector<std::pair<std::shared_ptr<Scorer>, float>> scorers_;
    std::unique_ptr<osal::Thread> reconcile_thread_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<Scheduler> create_scheduler(storage::Store& store, SchedulerConfig config) {
    return std::make_unique<SchedulerImpl>(store, config);
}

} // namespace kube32::scheduler
