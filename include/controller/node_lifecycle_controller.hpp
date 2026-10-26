#pragma once

#include "storage/store.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <map>
#include <atomic>
#include <memory>
#include <cstdint>

namespace kube32::controller {

struct NodeLifecycleConfig {
    uint32_t check_interval_ms{1000};       // 1-second check
    uint32_t not_ready_threshold_ms{15000}; // 15-second NotReady
    uint32_t eviction_threshold_ms{30000};  // 30-second Eviction
};

class NodeLifecycleController {
public:
    explicit NodeLifecycleController(storage::Store& store, NodeLifecycleConfig config = {});
    ~NodeLifecycleController();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Core periodic check
    void reconcile_nodes(uint64_t mock_now = 0);

    // Callback invoked when a lease update is accepted by the API server
    void record_node_lease(const std::string& node_name, uint64_t uptime_ms = 0);

    // Query last recorded lease time
    uint64_t get_last_lease_time(const std::string& node_name) const;

private:
    static void task_entry(void* arg);
    void run_loop();

    void mark_node_not_ready(const std::string& node_name, const std::string& current_node_json);
    void mark_node_unreachable_and_evict(const std::string& node_name, const std::string& current_node_json);
    void evict_pods_from_node(const std::string& node_name);

    storage::Store& store_;
    NodeLifecycleConfig config_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};

    mutable osal::Mutex lease_mutex_;
    std::map<std::string, uint64_t> last_lease_update_ms_;
};

} // namespace kube32::controller
