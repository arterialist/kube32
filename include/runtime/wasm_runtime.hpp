#pragma once

#include "models/kube_models.hpp"
#include "runtime/image_registry.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <functional>

#include "mem/function_ref.hpp"

struct M3Module;

namespace kube32::runtime {

enum class ContainerPhase {
    Pending,
    Running,
    Paused,
    Terminated,
    Failed
};

struct ContainerStatus {
    ContainerPhase phase{ContainerPhase::Pending};
    int restart_count{0};
    uint32_t memory_used_bytes{0};   // Wasm linear memory currently mapped
    uint64_t bytecode_bytes{0};      // module bytes the controller keeps (an image not memory mapped)
    uint64_t stack_bytes{0};         // interpreter operand stack reserved for the module
    uint64_t runtime_bytes{0};       // backing bytes of the module's memories as wasm3 accounts them (c_m3Limit_MemoryBytes)
    // Everything the container holds: wasm3's structures, the compiled code,
    // the operand stack and linear memory, as allocated (headers included),
    // the module's bytes when the container keeps a copy, and its thread's
    // stack while it has one. 0 when not measured.
    uint64_t held_bytes{0};
    std::string exit_message;
    bool ready{false};
    uint64_t start_time_ms{0};
    int32_t exit_code{0};
    std::string termination_reason;
    // CPU time of the container's own thread (its entry point: _start,
    // app_main, main), calls into other pods made on it left out. False for
    // containers without one and where CPU time cannot be measured.
    bool has_thread_cpu{false};
    uint64_t thread_cpu_us{0};
};

// Full deployment spec used by the kubelet: memory limit, env, stack.
struct DeploySpec {
    uint32_t memory_limit_bytes{64 * 1024};
    uint32_t stack_size_bytes{16 * 1024};
    bool stack_size_set{false};                     // stack_size_bytes given by the pod (kube32.io/wasm-stack): used as is
    std::vector<std::pair<std::string, std::string>> env;
    uint64_t gas_budget_per_call{4000000000ULL};   // 0: no metering (see ContainerConfig)
    std::vector<std::string> event_topics;          // channel topics delivered to k32_event
    uint32_t tick_interval_ms{1000};                // k32_tick period (kube32.io/tick-interval)
    // Bundle files the pod may send with k32_resp_asset: an embedded bundle
    // (images/web/<name>/) or a directory on node storage
    std::string asset_bundle;
    std::string asset_directory;
    // Migration (docs/migration.md): the container's name and what in the
    // pod's spec keeps it from moving (static_migration_blockers)
    std::string container_name;
    std::vector<std::string> migration_blockers;
    // Start from a snapshot (kube32.io/restore-from) instead of k32_init:
    // called once the module is instantiated, it hands the snapshot's bytes
    // to `feed` as they arrive and returns true when the stream ended whole
    std::function<bool(mem::FunctionRef<bool(const uint8_t*, size_t)> feed, std::string& why)> restore_from;
};

// Abstract Runtime Interface conforming to PROJECT.md § 4.3
class Runtime {
public:
    virtual ~Runtime() = default;
    virtual bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) = 0;
    // Default forwards to deploy_workload; WasmRuntime overrides with env and app registration.
    virtual bool deploy_workload_ex(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, const DeploySpec& spec, std::string& out_error) {
        bool ok = deploy_workload(pod_id, wasm_bytes, len, spec.memory_limit_bytes);
        if (!ok) out_error = "runtime rejected module";
        return ok;
    }
    // Deploys a module read into pod memory (from the card or the API
    // server); the runtime keeps those bytes for the module's lifetime,
    // charged to the pod, instead of copying them.
    virtual bool deploy_workload_owned(const std::string& pod_id, ModuleBytes&& wasm_bytes, const DeploySpec& spec, std::string& out_error) {
        ModuleBytes bytes = std::move(wasm_bytes);
        return deploy_workload_ex(pod_id, bytes.data(), bytes.size(), spec, out_error);
    }
    virtual bool start(const std::string& pod_id) = 0;
    virtual bool pause(const std::string& pod_id) = 0;
    virtual bool terminate(const std::string& pod_id) = 0;
    virtual ContainerStatus get_status(const std::string& pod_id) = 0;
    // Frees containers whose stop had to wait for a host call to return, once
    // it has (called periodically by the kubelet).
    virtual void reap_stopped() {}
};

class WasmRuntime : public Runtime {
public:
    WasmRuntime();
    ~WasmRuntime() override;

    // Runtime interface implementation
    bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) override;
    bool start(const std::string& pod_id) override;
    bool pause(const std::string& pod_id) override;
    bool terminate(const std::string& pod_id) override;
    ContainerStatus get_status(const std::string& pod_id) override;
    void reap_stopped() override;
    // Stopped containers whose thread has not ended yet
    size_t lingering_count() const;

    // Extended runtime management operations
    bool resume(const std::string& pod_id);
    bool restart(const std::string& pod_id);
    bool is_deployed(const std::string& pod_id) const;
    std::vector<std::string> list_containers() const;
    size_t container_count() const;

    // Direct execution helper for unit testing and synchronous execution
    int32_t execute_function(const std::string& pod_id, const std::string& fn_name, const std::vector<int32_t>& args = {});

    // Full deployment used by the kubelet: memory limit, env, and registration
    // of application pods (modules exporting k32_* entry points). On success
    // `out_error` is empty; otherwise it carries a human readable reason.
    bool deploy_workload_ex(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, const DeploySpec& spec, std::string& out_error) override;
    bool deploy_workload_owned(const std::string& pod_id, ModuleBytes&& wasm_bytes, const DeploySpec& spec, std::string& out_error) override;

private:
    struct Impl;
    bool deploy_impl(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, ModuleBytes* owned, const DeploySpec& spec, std::string& out_error);
    std::unique_ptr<Impl> impl_;
};

// Factory function
std::unique_ptr<WasmRuntime> create_wasm_runtime();

// Which sizes of wasm3's structures an estimate counts: the ESP32-S3
// board's (admission's, also in the simulator, so that both admit the same
// pods) or this build's
// BoardHeld: board sizes, and only what the pod holds while it runs (no
// compiler scratch, no host-side state outside the pod's account): Pod
// Overhead, which must not depend on the node that computes it
enum class EstimateSizes { Board, ThisBuild, BoardHeld };

// Memory a pod needs to start: its runtime and module structures, operand
// stack, linear memory (memory_limit_bytes below one page, else the module's
// initial pages), compiled code, its host-side state where pods share the
// node's heap, and the compiler's scratch where it is not lent. Estimated
// from the module's sections; the stack is `stack_bytes`, or with
// `stack_from_module` what the module's call chains need, `stack_bytes` at
// most. `wasm` may be null when only the size is known.
// `out_linear`, when given, receives the linear memory part of the estimate.
// `module_held`: the module is not memory mapped (an image from the card or
// the API server, or decoded from an annotation), so the pod holds its
// bytes in its memory, and they count too.
size_t estimate_pod_memory(const uint8_t* wasm, size_t len, uint32_t memory_limit_bytes, uint32_t stack_bytes,
                           bool stack_from_module = false, EstimateSizes sizes = EstimateSizes::Board,
                           size_t* out_linear = nullptr, bool module_held = false);

// Pod Overhead admission: sets spec.overhead.memory of a pod (`pod` is the
// whole object, or a pod template with metadata and spec) to what the
// runtime needs beyond its containers' linear memory: wasm3's structures,
// the compiled code and the operand stack, estimated from each container's
// image (an embedded image, or kube32.io/wasm-size for one in the image
// store) with board sizes, and the module's bytes for an image in the image
// store, which the pod holds in its memory. The scheduler adds it to the
// pod's requests, as Kubernetes does with a RuntimeClass's overhead.
// Returns the overhead.
size_t set_pod_overhead(models::JsonValue& pod);

// The operand stack a compiled module can need at most: the deepest chain of
// calls, each frame as large as wasm3 compiled it, an indirect call reaching
// any function of its type. 0 when there is no bound (recursion) or the
// module uses instructions the call graph does not read.
uint32_t module_stack_need_bytes(const M3Module* module);

// Whether a deploy error is the node running out of memory while the module
// loaded, rather than a fault of the module.
bool deploy_error_is_out_of_memory(const std::string& error);

// A deploy from a snapshot (DeploySpec::restore_from) that failed in the
// restore itself starts its error with this
constexpr const char* kRestoreErrorPrefix = "cannot restore the snapshot: ";
bool deploy_error_is_restore(const std::string& error);

// The entry point a module exports ("_start", "app_main", "main"), read from
// its export section; null for a request pod
const char* module_entry_point(const uint8_t* wasm, size_t len);

// The interpreter's speed on this node: millions of wasm3 operations per
// second on a small fixed integer kernel (kInterpKernelOps operations per
// run), measured on the stack and memory pods use. Takes about 20 ms on a
// board.
double measure_interp_mops();

} // namespace kube32::runtime
