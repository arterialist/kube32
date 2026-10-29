#pragma once

#include "storage/store.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>

namespace kube32::scheduler {

struct ResourceSpec {
    uint64_t memory_bytes{0};
    uint64_t psram_bytes{0};
    uint32_t gpio_pins{0};
    uint32_t i2c_buses{0};
    uint32_t uart_ports{0};
    uint32_t adc_channels{0};
    uint32_t cpu_cores{1};
};

struct ScheduleResult {
    bool scheduled{false};
    std::string target_node;
    std::string failure_reason;
};

enum class ScoringStrategy {
    LeastAllocated,
    BestFit
};

struct SchedulerConfig {
    ScoringStrategy strategy{ScoringStrategy::LeastAllocated};
    uint32_t reconcile_interval_ms{1000};
    uint32_t node_heartbeat_timeout_ms{30000};
    float weight_memory{1.0f};
    float weight_gpio{0.5f};
    float weight_role{0.2f};
};

class Predicate {
public:
    virtual ~Predicate() = default;
    virtual const char* name() const = 0;
    virtual bool evaluate(const std::string& pod_json,
                          const std::string& node_json,
                          const ResourceSpec& pod_req,
                          const ResourceSpec& node_allocatable,
                          const ResourceSpec& node_allocated,
                          std::string& failure_reason) = 0;
};

class NodeSchedulablePredicate : public Predicate {
public:
    const char* name() const override { return "NodeSchedulable"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class NodeMemoryPredicate : public Predicate {
public:
    const char* name() const override { return "NodeMemory"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class NodePSRAMPredicate : public Predicate {
public:
    const char* name() const override { return "NodePSRAM"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class NodePeripheralPredicate : public Predicate {
public:
    const char* name() const override { return "NodePeripheral"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class NodeSelectorPredicate : public Predicate {
public:
    const char* name() const override { return "NodeSelector"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class NodeNamePredicate : public Predicate {
public:
    const char* name() const override { return "NodeName"; }
    bool evaluate(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated,
                  std::string& failure_reason) override;
};

class Scorer {
public:
    virtual ~Scorer() = default;
    virtual const char* name() const = 0;
    virtual int64_t score(const std::string& pod_json,
                          const std::string& node_json,
                          const ResourceSpec& pod_req,
                          const ResourceSpec& node_allocatable,
                          const ResourceSpec& node_allocated) = 0;
};

class LeastAllocatedScorer : public Scorer {
public:
    const char* name() const override { return "LeastAllocated"; }
    int64_t score(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated) override;
};

class BestFitScorer : public Scorer {
public:
    const char* name() const override { return "BestFit"; }
    int64_t score(const std::string& pod_json,
                  const std::string& node_json,
                  const ResourceSpec& pod_req,
                  const ResourceSpec& node_allocatable,
                  const ResourceSpec& node_allocated) override;
};

class Scheduler {
public:
    virtual ~Scheduler() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    // Evaluates a pod and updates store atomically
    virtual ScheduleResult schedule_pod(const std::string& pod_name, const std::string& pod_namespace) = 0;

    // Synchronously evaluates and mutates pod manifest in-place before initial store insertion
    virtual ScheduleResult schedule_pod_manifest(std::string& in_out_pod_json) = 0;

    // Background loop: re-evaluates Pending pods and evicts orphaned pods from dead nodes
    virtual void reconcile_cluster() = 0;

    virtual void add_predicate(std::shared_ptr<Predicate> predicate) = 0;
    virtual void add_scorer(std::shared_ptr<Scorer> scorer, float weight) = 0;
};

// Factory creation
std::unique_ptr<Scheduler> create_scheduler(storage::Store& store, SchedulerConfig config = {});

// Utility functions
uint64_t parse_quantity(const std::string& val);
ResourceSpec extract_pod_resource_requests(const std::string& pod_json);
ResourceSpec extract_node_allocatable(const std::string& node_json);
ResourceSpec calculate_node_allocated_resources(storage::Store& store, const std::string& node_name, const std::string& exclude_pod_key = "");

} // namespace kube32::scheduler
