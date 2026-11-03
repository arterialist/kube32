#include "scheduler/scheduler.hpp"
#include "api/event_recorder.hpp"
#include "controller/reconcile_trigger.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
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

// A pod without a memory request or limit: the one 64 KiB page of linear
// memory the kubelet gives it. The runtime's own memory is its overhead.
static constexpr uint64_t kDefaultPodMemoryBytes = 64 * 1024;

static ResourceSpec pod_resource_requests(const models::JsonValue& doc);

ResourceSpec extract_pod_resource_requests(const std::string& pod_json) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(pod_json, doc, err) || !doc.is_object()) {
        ResourceSpec spec;
        spec.memory_bytes = kDefaultPodMemoryBytes;
        return spec;
    }
    return pod_resource_requests(doc);
}

static ResourceSpec pod_resource_requests(const models::JsonValue& doc) {
    ResourceSpec spec;
    const models::JsonValue* spec_val = doc.get("spec");
    if (!spec_val || !spec_val->is_object()) {
        spec.memory_bytes = kDefaultPodMemoryBytes;
        return spec;
    }

    const models::JsonValue* containers = spec_val->get("containers");
    if (!containers || !containers->is_array()) {
        spec.memory_bytes = kDefaultPodMemoryBytes;
        return spec;
    }

    uint64_t total_mem = 0;
    uint64_t total_psram = 0;
    uint32_t total_gpio = 0;
    uint32_t total_i2c = 0;
    uint32_t total_uart = 0;
    uint32_t total_adc = 0;
    uint32_t total_rmt_tx = 0;
    uint32_t total_rmt_rx = 0;

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

        // RMT (IR) channels: esp.io/rmt-tx or esp32.io/rmt-tx, and rmt-rx
        std::string rmt_tx_str = req->get_string("esp.io/rmt-tx", req->get_string("esp32.io/rmt-tx", ""));
        if (!rmt_tx_str.empty()) total_rmt_tx += static_cast<uint32_t>(parse_quantity(rmt_tx_str));
        std::string rmt_rx_str = req->get_string("esp.io/rmt-rx", req->get_string("esp32.io/rmt-rx", ""));
        if (!rmt_rx_str.empty()) total_rmt_rx += static_cast<uint32_t>(parse_quantity(rmt_rx_str));
    }

    spec.memory_bytes = (total_mem > 0) ? total_mem : kDefaultPodMemoryBytes;
    // Pod Overhead: what the runtime takes beyond the containers' requests
    // (set at admission), counted as Kubernetes' scheduler counts it
    if (const models::JsonValue* overhead = spec_val->get("overhead"); overhead && overhead->is_object()) {
        spec.memory_bytes += parse_quantity(overhead->get_string("memory", ""));
    }
    spec.psram_bytes = total_psram;
    spec.gpio_pins = total_gpio;
    spec.i2c_buses = total_i2c;
    spec.uart_ports = total_uart;
    spec.adc_channels = total_adc;
    spec.rmt_tx_channels = total_rmt_tx;
    spec.rmt_rx_channels = total_rmt_rx;
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
    spec.rmt_tx_channels = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/rmt-tx", alloc->get_string("esp32.io/rmt-tx", ""))));
    spec.rmt_rx_channels = static_cast<uint32_t>(parse_quantity(alloc->get_string("esp.io/rmt-rx", alloc->get_string("esp32.io/rmt-rx", ""))));
    spec.psram_bytes = parse_quantity(alloc->get_string("esp.io/psram", alloc->get_string("esp32.io/psram", "")));
    spec.pods = static_cast<uint32_t>(parse_quantity(alloc->get_string("pods", "")));

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

// What the pods bound to each node in `node_versions` (name -> the
// resourceVersion of its last status write) take of its allocatable
// figures; one list of the pods serves every node.
//   Memory and PSRAM: allocatable is fixed (what pods can be given with no
//   pod running), so every bound pod that has not terminated counts, with
//   its requests and its Pod Overhead, as Kubernetes' scheduler counts them.
//   Pins, buses and channels: the kubelet reports what is unclaimed, so pods
//   it was already running when it last reported are inside the figure.
//   What the scheduler still subtracts are pods its kubelet has not started
//   (Pending), containers waiting to restart, and pods that started after
//   the node's last status write (the store's resourceVersion is one global
//   counter, so a pod written after the node is newer than its report).
static std::map<std::string, ResourceSpec> allocated_by_node(storage::Store& store, const std::map<std::string, uint64_t>& node_versions,
                                                             const std::string& exclude_pod_key) {
    std::map<std::string, ResourceSpec> allocated;
    // Each pod is read in place and parsed once
    store.visit("/registry/pods/", [&](const std::string& key, const std::string& json) {
        if (!exclude_pod_key.empty() && key == exclude_pod_key) return true;

        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) return true;

        const models::JsonValue* spec = doc.get("spec");
        if (!spec || !spec->is_object()) return true;

        auto node = node_versions.find(spec->get_string("nodeName", ""));
        if (node == node_versions.end()) return true;
        const uint64_t node_rv = node->second;

        const models::JsonValue* status = doc.get("status");
        std::string phase = status ? status->get_string("phase", "") : "";
        if (phase == "Succeeded" || phase == "Failed") return true;
        allocated[node->first].pods++;   // a pod count is not in any report: every bound pod counts

        ResourceSpec req = pod_resource_requests(doc);
        ResourceSpec& used = allocated[node->first];
        used.memory_bytes += req.memory_bytes;
        used.psram_bytes += req.psram_bytes;

        if (phase != "Running" && phase != "Pending") return true;
        const bool reflected = phase == "Running" && container_running(status) && node_rv > 0 && resource_version_of(doc) < node_rv;
        if (reflected) return true;
        used.gpio_pins += req.gpio_pins;
        used.i2c_buses += req.i2c_buses;
        used.uart_ports += req.uart_ports;
        used.rmt_tx_channels += req.rmt_tx_channels;
        used.rmt_rx_channels += req.rmt_rx_channels;
        used.adc_channels += req.adc_channels;
        return true;
    });

    return allocated;
}

ResourceSpec calculate_node_allocated_resources(storage::Store& store, const std::string& node_name, const std::string& exclude_pod_key) {
    uint64_t node_rv = 0;
    {
        std::string node_json;
        models::JsonValue node_doc;
        std::string err;
        if (store.get("/registry/nodes/" + node_name, node_json).ok() && models::parse_json(node_json, node_doc, err)) {
            node_rv = resource_version_of(node_doc);
        }
    }
    return allocated_by_node(store, {{node_name, node_rv}}, exclude_pod_key)[node_name];
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

    uint32_t avail_rmt_tx = (node_allocatable.rmt_tx_channels >= node_allocated.rmt_tx_channels)
        ? (node_allocatable.rmt_tx_channels - node_allocated.rmt_tx_channels) : 0;
    if (avail_rmt_tx < pod_req.rmt_tx_channels) {
        failure_reason = "Insufficient esp32.io/rmt-tx";
        return false;
    }

    uint32_t avail_rmt_rx = (node_allocatable.rmt_rx_channels >= node_allocated.rmt_rx_channels)
        ? (node_allocatable.rmt_rx_channels - node_allocated.rmt_rx_channels) : 0;
    if (avail_rmt_rx < pod_req.rmt_rx_channels) {
        failure_reason = "Insufficient esp32.io/rmt-rx";
        return false;
    }

    return true;
}

bool NodePodCountPredicate::evaluate(const std::string&,
                                     const std::string&,
                                     const ResourceSpec&,
                                     const ResourceSpec& node_allocatable,
                                     const ResourceSpec& node_allocated,
                                     std::string& failure_reason) {
    if (node_allocatable.pods == 0 || node_allocated.pods < node_allocatable.pods) return true;
    failure_reason = "Too many pods";
    return false;
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

// What the filters read of a Node: its labels, spec (cordon, taints),
// Ready condition and allocatable resources
static uint64_t node_fingerprint(const models::JsonValue& node) {
    const models::JsonValue* meta = node.get("metadata");
    const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
    const models::JsonValue* spec = node.get("spec");
    const models::JsonValue* status = node.get("status");
    const models::JsonValue* allocatable = status ? status->get("allocatable") : nullptr;
    std::string ready;
    const models::JsonValue* conds = status ? status->get("conditions") : nullptr;
    if (conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == "Ready") ready = c.get_string("status", "");
        }
    }
    return controller::ReconcileTrigger::hash({labels ? labels->to_string() : std::string(), spec ? spec->to_string() : std::string(), ready,
                                               allocatable ? allocatable->to_string() : std::string()});
}

class SchedulerImpl : public Scheduler {
public:
    SchedulerImpl(storage::Store& store, SchedulerConfig config)
        : store_(store), config_(config),
          trigger_(store, controller::ReconcileTriggerConfig{config.reconcile_interval_ms, config.resync_interval_ms}) {
        trigger_.watch("/registry/pods/");
        trigger_.watch_fields("/registry/nodes/", node_fingerprint);
        // Default filters, in kube-scheduler's order: node state, placement
        // (name, selector), then resources. The first one a node fails is
        // its reason in the FailedScheduling message.
        add_predicate(std::make_shared<NodeSchedulablePredicate>());
        add_predicate(std::make_shared<NodeNamePredicate>());
        add_predicate(std::make_shared<NodeSelectorPredicate>());
        add_predicate(std::make_shared<NodePodCountPredicate>());
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
        trigger_.interrupt();
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

    ScheduleResult pick_node(const std::string& pod_json, const std::string& exclude_node) override {
        osal::LockGuard guard(mutex_);
        std::string copy = pod_json;
        return evaluate_and_bind_manifest(copy, "", exclude_node, true);
    }

    ScheduleResult schedule_pod(const std::string& pod_name, const std::string& pod_namespace) override {
        return schedule_key("/registry/pods/" + pod_namespace + "/" + pod_name, nullptr);
    }

    // Binds the stored pod, or writes why it does not fit when that changed.
    // `read_version` (optional): the pod's version the attempt started from.
    ScheduleResult schedule_key(const std::string& pod_key, uint64_t* read_version) {
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
                if (read_version) *read_version = version;
                const std::string stored = pod_json;

                result = evaluate_and_bind_manifest(pod_json, pod_key);
                // Still unschedulable for the same reasons: nothing to write
                if (!result.scheduled && pod_json == stored) return result;

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
        pass(osal::System::get_uptime_ms(), true);
    }

private:
    // A pod that did not fit when last tried
    struct Attempt {
        uint64_t version{0};   // its resourceVersion then
        uint64_t changes{0};   // changes_ then
        uint64_t next_ms{0};   // when its back-off ends
        uint32_t delay_ms{0};
    };

    // Whether the pod at `key` (resourceVersion `version`) is tried in this
    // pass: never tried, changed since, or past its back-off after a change
    // in the cluster (or at a resync)
    bool attempt_due(const std::string& key, uint64_t version, uint64_t now, bool resync) {
        auto it = attempts_.find(key);
        if (it == attempts_.end() || it->second.version != version) return true;
        const Attempt& a = it->second;
        if (now < a.next_ms) {
            if (changes_ > a.changes) trigger_.wake_at(a.next_ms);
            return false;
        }
        return changes_ > a.changes || resync;
    }

    void pass(uint64_t now, bool resync) {
        // 1. Nodes in the cluster registry
        static const size_t kNodePrefix = std::string("/registry/nodes/").size();
        std::set<std::string> existing_nodes;
        store_.visit("/registry/nodes/", [&existing_nodes](const std::string& key, const std::string&) {
            existing_nodes.insert(key.substr(kNodePrefix));
            return true;
        });

        // 2. The pods that need a node, found in place: their keys and versions
        std::vector<std::pair<std::string, uint64_t>> unplaced;
        store_.visit("/registry/pods/", [&](const std::string& key, const std::string& json) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(json, doc, err) || !doc.is_object()) return true;

            const models::JsonValue* spec = doc.get("spec");
            std::string assigned_node = spec ? spec->get_string("nodeName", "") : "";
            const models::JsonValue* meta = doc.get("metadata");

            const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
            // DaemonSet pods belong to their node; the DaemonSet controller
            // removes them when the node goes away
            bool is_daemon = models::has_owner(doc, "DaemonSet") || (labels && labels->get_string("kube32.io/daemonset", "") == "true");
            if (is_daemon && !assigned_node.empty() && assigned_node != "<none>") return true;
            if (meta && !meta->get_string("deletionTimestamp", "").empty()) return true; // being torn down

            // Unbound (new, or evicted by NodeLifecycleController), or bound to a
            // node deleted from /registry/nodes/: evict and re-schedule. A bound
            // pod stays Pending until its kubelet starts it; it is not re-evaluated.
            const bool unbound = assigned_node.empty() || assigned_node == "<none>";
            if (!unbound && existing_nodes.count(assigned_node)) return true;
            unplaced.emplace_back(key, resource_version_of(doc));
            return true;
        });

        std::set<std::string> waiting;   // pods tried and still unbound
        for (const auto& [key, pod_version] : unplaced) {
            osal::Thread::yield();
            if (!attempt_due(key, pod_version, now, resync)) {
                waiting.insert(key);
                continue;
            }
            uint64_t version = 0;
            const ScheduleResult r = schedule_key(key, &version);
            if (r.scheduled || version == 0) continue;
            Attempt& a = attempts_[key];
            a.delay_ms = a.delay_ms ? std::min(a.delay_ms * 2, config_.backoff_max_ms) : config_.backoff_initial_ms;
            a.version = version;
            a.changes = changes_;
            a.next_ms = now + a.delay_ms;
            waiting.insert(key);
        }
        // Pods bound or gone since
        for (auto it = attempts_.begin(); it != attempts_.end();) {
            it = waiting.count(it->first) ? std::next(it) : attempts_.erase(it);
        }
    }

    void run_reconcile_loop() {
        while (running_.load()) {
            // An allocation failure while parsing objects must not take the
            // node down; the next pass retries once memory is back.
            try {
                const uint32_t why = trigger_.due(osal::System::get_uptime_ms());
                if (why & controller::ReconcileTrigger::kChange) ++changes_;
                if (why) pass(osal::System::get_uptime_ms(), (why & controller::ReconcileTrigger::kResync) != 0);
            } catch (const std::exception& e) {
                std::cout << "[SCHED] reconcile skipped: " << e.what() << std::endl;
            }
            trigger_.wait(osal::System::get_uptime_ms(), config_.resync_interval_ms);
        }
    }

    // `dry`: the choice only, the manifest is left as it is and no event recorded
    ScheduleResult evaluate_and_bind_manifest(std::string& in_out_pod_json, const std::string& exclude_pod_key = "",
                                              const std::string& exclude_node = "", bool dry = false) {
        models::JsonValue pod_doc;
        std::string err;
        if (!models::parse_json(in_out_pod_json, pod_doc, err) || !pod_doc.is_object()) {
            return {false, "", "Invalid pod manifest"};
        }

        ResourceSpec pod_req = pod_resource_requests(pod_doc);

        // Fetch all nodes
        std::vector<std::string> node_jsons;
        store_.list("/registry/nodes/", node_jsons);

        struct Candidate {
            std::string name;
            std::string ip{"127.0.0.1"};
            int64_t total_score{0};
        };

        std::vector<Candidate> qualified;
        std::map<std::string, size_t> reasons;   // why nodes were filtered out, with counts

        // What the pods bound to each node take of its allocatable figures
        // (allocated_by_node): one list of the pods for all nodes. Each node
        // is parsed on its own; its name and first address are kept.
        std::vector<Candidate> n_info(node_jsons.size());
        std::map<std::string, uint64_t> node_versions;
        for (size_t i = 0; i < node_jsons.size(); ++i) {
            models::JsonValue n_doc;
            if (!models::parse_json(node_jsons[i], n_doc, err) || !n_doc.is_object()) continue;
            const models::JsonValue* meta = n_doc.get("metadata");
            n_info[i].name = meta ? meta->get_string("name", "") : "";
            if (n_info[i].name.empty()) continue;
            node_versions[n_info[i].name] = resource_version_of(n_doc);
            const models::JsonValue* status = n_doc.get("status");
            const models::JsonValue* addrs = status && status->is_object() ? status->get("addresses") : nullptr;
            if (addrs && addrs->is_array() && !addrs->arr_val.empty()) {
                n_info[i].ip = addrs->arr_val[0].get_string("address", "127.0.0.1");
            }
        }
        std::map<std::string, ResourceSpec> used_by_node = allocated_by_node(store_, node_versions, exclude_pod_key);

        for (size_t i = 0; i < node_jsons.size(); ++i) {
            const std::string& n_json = node_jsons[i];
            const std::string& nname = n_info[i].name;
            if (nname.empty()) { reasons["node(s) could not be read"]++; continue; }
            if (nname == exclude_node) { reasons["node(s) were the node the pod leaves"]++; continue; }

            ResourceSpec node_alloc = extract_node_allocatable(n_json);
            const ResourceSpec& node_used = used_by_node[nname];

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
                Candidate cand = n_info[i];

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
            if (dry) return {false, "", message};
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
        if (dry) return {true, winner.name, ""};

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
    controller::ReconcileTrigger trigger_;
    std::map<std::string, Attempt> attempts_;   // unbound pods that did not fit (loop thread)
    uint64_t changes_{0};                       // passes a change made due (loop thread)
};

std::unique_ptr<Scheduler> create_scheduler(storage::Store& store, SchedulerConfig config) {
    return std::make_unique<SchedulerImpl>(store, config);
}

} // namespace kube32::scheduler
