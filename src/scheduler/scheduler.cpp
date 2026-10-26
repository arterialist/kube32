#include "scheduler/scheduler.hpp"
#include "models/kube_models.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <set>

namespace kube32::scheduler {

uint64_t parse_quantity(const std::string& val) {
    if (val.empty()) return 0;

    size_t idx = 0;
    while (idx < val.size() && (std::isdigit(static_cast<unsigned char>(val[idx])) || val[idx] == '.')) {
        idx++;
    }
    if (idx == 0) return 0;

    double num = 0.0;
    try {
        num = std::stod(val.substr(0, idx));
    } catch (...) {
        return 0;
    }

    std::string unit = val.substr(idx);
    if (unit == "Ki" || unit == "ki" || unit == "K" || unit == "k") {
        return static_cast<uint64_t>(num * 1024);
    } else if (unit == "Mi" || unit == "mi" || unit == "M" || unit == "m") {
        return static_cast<uint64_t>(num * 1024 * 1024);
    } else if (unit == "Gi" || unit == "gi" || unit == "G" || unit == "g") {
        return static_cast<uint64_t>(num * 1024 * 1024 * 1024);
    }
    return static_cast<uint64_t>(num);
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
    }

    spec.memory_bytes = (total_mem > 0) ? total_mem : (16 * 1024);
    spec.psram_bytes = total_psram;
    spec.gpio_pins = total_gpio;
    spec.i2c_buses = total_i2c;
    spec.adc_channels = total_adc;
    return spec;
}

ResourceSpec extract_node_allocatable(const std::string& node_json) {
    ResourceSpec spec;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(node_json, doc, err) || !doc.is_object()) {
        spec.memory_bytes = 240 * 1024;
        spec.gpio_pins = 14;
        spec.i2c_buses = 2;
        spec.adc_channels = 2;
        spec.psram_bytes = 3840 * 1024;
        return spec;
    }

    const models::JsonValue* status = doc.get("status");
    if (!status || !status->is_object()) {
        spec.memory_bytes = 240 * 1024;
        spec.gpio_pins = 14;
        spec.i2c_buses = 2;
        spec.adc_channels = 2;
        spec.psram_bytes = 3840 * 1024;
        return spec;
    }

    const models::JsonValue* alloc = status->get("allocatable");
    if (!alloc || !alloc->is_object()) {
        alloc = status->get("capacity");
    }
    if (!alloc || !alloc->is_object()) {
        spec.memory_bytes = 240 * 1024;
        spec.gpio_pins = 14;
        spec.i2c_buses = 2;
        spec.adc_channels = 2;
        spec.psram_bytes = 3840 * 1024;
        return spec;
    }

    std::string mem_str = alloc->get_string("memory", "240Ki");
    spec.memory_bytes = parse_quantity(mem_str);

    std::string gpio_str = alloc->get_string("esp.io/gpio", alloc->get_string("esp32.io/gpio", "14"));
    spec.gpio_pins = static_cast<uint32_t>(parse_quantity(gpio_str));

    std::string i2c_str = alloc->get_string("esp.io/i2c", alloc->get_string("esp32.io/i2c", "2"));
    spec.i2c_buses = static_cast<uint32_t>(parse_quantity(i2c_str));

    std::string adc_str = alloc->get_string("esp.io/adc", alloc->get_string("esp32.io/adc", "2"));
    spec.adc_channels = static_cast<uint32_t>(parse_quantity(adc_str));

    std::string psram_str = alloc->get_string("esp.io/psram", alloc->get_string("esp32.io/psram", "3840Ki"));
    spec.psram_bytes = parse_quantity(psram_str);

    return spec;
}

ResourceSpec calculate_node_allocated_resources(storage::Store& store, const std::string& node_name, const std::string& exclude_pod_key) {
    ResourceSpec allocated;
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

        ResourceSpec req = extract_pod_resource_requests(pod_jsons[i]);
        allocated.memory_bytes += req.memory_bytes;
        allocated.gpio_pins += req.gpio_pins;
        allocated.i2c_buses += req.i2c_buses;
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
        failure_reason = "Malformed node JSON";
        return false;
    }

    const models::JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object() && spec->get_bool("unschedulable", false)) {
        failure_reason = "Node is marked unschedulable";
        return false;
    }

    const models::JsonValue* status = doc.get("status");
    if (!status || !status->is_object()) {
        failure_reason = "Node has no status";
        return false;
    }

    const models::JsonValue* conds = status->get("conditions");
    if (!conds || !conds->is_array()) {
        failure_reason = "Node has no conditions";
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
        failure_reason = "Node is NotReady";
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
        failure_reason = "InsufficientMemory (available: " + std::to_string(available) + 
                         " bytes, requested: " + std::to_string(pod_req.memory_bytes) + " bytes)";
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
        failure_reason = "InsufficientPSRAM";
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
        failure_reason = "InsufficientGPIO (available: " + std::to_string(avail_gpio) + 
                         ", requested: " + std::to_string(pod_req.gpio_pins) + ")";
        return false;
    }

    uint32_t avail_i2c = (node_allocatable.i2c_buses >= node_allocated.i2c_buses)
        ? (node_allocatable.i2c_buses - node_allocated.i2c_buses) : 0;
    if (avail_i2c < pod_req.i2c_buses) {
        failure_reason = "InsufficientI2C";
        return false;
    }

    uint32_t avail_adc = (node_allocatable.adc_channels >= node_allocated.adc_channels)
        ? (node_allocatable.adc_channels - node_allocated.adc_channels) : 0;
    if (avail_adc < pod_req.adc_channels) {
        failure_reason = "InsufficientADC";
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
        failure_reason = "Node selector check failed (invalid node JSON)";
        return false;
    }

    const models::JsonValue* meta = node_doc.get("metadata");
    const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;

    for (const auto& kv : sel->obj_val) {
        std::string req_val = kv.second.is_string() ? kv.second.str_val : "";
        std::string node_val = labels ? labels->get_string(kv.first, "") : "";
        if (node_val != req_val) {
            failure_reason = "Node label mismatch: " + kv.first + "=" + req_val;
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
    if (!models::parse_json(node_json, node_doc, err) || !node_doc.is_object()) return false;

    const models::JsonValue* meta = node_doc.get("metadata");
    std::string nname = meta ? meta->get_string("name", "") : "";

    if (nname != node_name_pin) {
        failure_reason = "Node name mismatch (pinned to " + node_name_pin + ")";
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
        // Register default predicates
        add_predicate(std::make_shared<NodeSchedulablePredicate>());
        add_predicate(std::make_shared<NodeMemoryPredicate>());
        add_predicate(std::make_shared<NodePSRAMPredicate>());
        add_predicate(std::make_shared<NodePeripheralPredicate>());
        add_predicate(std::make_shared<NodeSelectorPredicate>());
        add_predicate(std::make_shared<NodeNamePredicate>());

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
        bool started = reconcile_thread_->start([](void* p) {
            auto* self = static_cast<SchedulerImpl*>(p);
            self->run_reconcile_loop();
        }, this);

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
            bool is_daemon = labels && labels->get_string("kube32.io/daemonset", "") == "true";
            if (is_daemon) continue;

            // Only consider a node "missing" if it was deleted from /registry/nodes/
            bool node_deleted = (!assigned_node.empty() && assigned_node != "<none>" && existing_nodes.count(assigned_node) == 0);

            if (node_deleted) {
                // Node was deleted from cluster: evict and re-schedule!
                schedule_pod(pod_name, pod_ns);
            } else if (phase == "Pending" || assigned_node.empty() || assigned_node == "<none>") {
                // Re-evaluate pending pod (e.g. evicted by NodeLifecycleController)
                schedule_pod(pod_name, pod_ns);
            }
        }
    }

private:
    void run_reconcile_loop() {
        while (running_.load()) {
            osal::Thread::sleep_ms(config_.reconcile_interval_ms);
            if (!running_.load()) break;
            reconcile_cluster();
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

        for (const auto& n_json : node_jsons) {
            models::JsonValue n_doc;
            if (!models::parse_json(n_json, n_doc, err) || !n_doc.is_object()) continue;
            const models::JsonValue* meta = n_doc.get("metadata");
            std::string nname = meta ? meta->get_string("name", "") : "";
            if (nname.empty()) continue;

            ResourceSpec node_alloc = extract_node_allocatable(n_json);
            ResourceSpec node_used = calculate_node_allocated_resources(store_, nname, exclude_pod_key);

            bool passes = true;
            std::string reason;
            for (const auto& pred : predicates_) {
                if (!pred->evaluate(in_out_pod_json, n_json, pod_req, node_alloc, node_used, reason)) {
                    passes = false;
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
            // Unschedulable
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
            cond_item.set("message", models::JsonValue{models::JsonType::String, false, 0, 
                "0/" + std::to_string(node_jsons.size()) + " nodes are available: insufficient heap memory or peripheral resources", {}, {}});

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
            return {false, "", "0/" + std::to_string(node_jsons.size()) + " nodes available"};
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

        // Mutate status
        models::JsonValue status_val;
        status_val.type = models::JsonType::Object;
        status_val.set("phase", models::JsonValue{models::JsonType::String, false, 0, "Running", {}, {}});
        status_val.set("hostIP", models::JsonValue{models::JsonType::String, false, 0, winner.ip, {}, {}});
        status_val.set("podIP", models::JsonValue{models::JsonType::String, false, 0, winner.ip, {}, {}});
        status_val.set("startTime", models::JsonValue{models::JsonType::String, false, 0, "2026-10-26T07:00:00Z", {}, {}});

        // Conditions
        models::JsonValue conds_arr;
        conds_arr.type = models::JsonType::Array;
        const char* cond_types[] = {"PodScheduled", "Initialized", "Ready", "ContainersReady"};
        for (const char* ct : cond_types) {
            models::JsonValue c;
            c.type = models::JsonType::Object;
            c.set("type", models::JsonValue{models::JsonType::String, false, 0, ct, {}, {}});
            c.set("status", models::JsonValue{models::JsonType::String, false, 0, "True", {}, {}});
            conds_arr.arr_val.push_back(std::move(c));
        }
        status_val.set("conditions", std::move(conds_arr));

        // Container statuses
        models::JsonValue cstatus_arr;
        cstatus_arr.type = models::JsonType::Array;

        std::string container_name = "app";
        std::string image_name = "wasm://app";
        if (spec) {
            const models::JsonValue* containers = spec->get("containers");
            if (containers && containers->is_array() && !containers->arr_val.empty()) {
                container_name = containers->arr_val[0].get_string("name", "app");
                image_name = containers->arr_val[0].get_string("image", "wasm://app");
            }
        }

        models::JsonValue cs0;
        cs0.type = models::JsonType::Object;
        cs0.set("name", models::JsonValue{models::JsonType::String, false, 0, container_name, {}, {}});
        cs0.set("ready", models::JsonValue{models::JsonType::Boolean, true, 0, "", {}, {}});
        cs0.set("restartCount", models::JsonValue{models::JsonType::Number, false, 0, "", {}, {}});
        cs0.set("image", models::JsonValue{models::JsonType::String, false, 0, image_name, {}, {}});

        models::JsonValue running_state;
        running_state.type = models::JsonType::Object;
        running_state.set("startedAt", models::JsonValue{models::JsonType::String, false, 0, "2026-10-26T07:00:00Z", {}, {}});

        models::JsonValue state_obj;
        state_obj.type = models::JsonType::Object;
        state_obj.set("running", std::move(running_state));
        cs0.set("state", std::move(state_obj));

        cstatus_arr.arr_val.push_back(std::move(cs0));
        status_val.set("containerStatuses", std::move(cstatus_arr));

        pod_doc.set("status", std::move(status_val));
        in_out_pod_json = pod_doc.to_string();

        return {true, winner.name, ""};
    }

    storage::Store& store_;
    SchedulerConfig config_;
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
