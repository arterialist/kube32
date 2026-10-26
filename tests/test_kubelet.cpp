#include "kubelet/kubelet.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/wasm_runtime.hpp"
#include "hal/capability_firewall.hpp"
#include <iostream>
#include <cassert>

using namespace kube32::kubelet;
using namespace kube32::models;

class MockKubeClient : public KubeApiClient {
public:
    bool registered{false};
    bool node_patched{false};
    std::string last_node_patch;
    std::vector<std::pair<KubePod, std::string>> mock_pods;
    std::string last_pod_status_patch;
    int register_count{0};

    bool register_node(const std::string& node_json, bool& out_conflict) override {
        (void)node_json;
        register_count++;
        registered = true;
        out_conflict = false;
        return true;
    }

    bool patch_node_status(const std::string& node_name, const std::string& patch_json) override {
        (void)node_name;
        node_patched = true;
        last_node_patch = patch_json;
        return true;
    }

    bool list_pods(std::vector<KubePod>& out_pods) override {
        for (const auto& p : mock_pods) {
            out_pods.push_back(p.first);
        }
        return true;
    }

    bool list_pods_raw(std::vector<std::pair<KubePod, std::string>>& out_pods_with_manifest) override {
        out_pods_with_manifest = mock_pods;
        return true;
    }

    bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) override {
        (void)ns;
        (void)name;
        last_pod_status_patch = status_patch_json;
        return true;
    }
};

void test_node_status_reporter() {
    NodeStatusReporter reporter("esp32-node-test", "192.168.4.2");
    NodeTelemetry t = reporter.collect_telemetry();

    std::string reg = reporter.build_registration_json(t);
    assert(reg.find("\"kind\":\"Node\"") != std::string::npos);
    assert(reg.find("esp32-node-test") != std::string::npos);
    assert(reg.find("192.168.4.2") != std::string::npos);
    assert(reg.find("\"Ready\"") != std::string::npos);
    assert(reg.find("320Ki") != std::string::npos);

    std::string patch_ok = reporter.build_heartbeat_patch_json(t, false);
    assert(patch_ok.find("KubeletHasSufficientMemory") != std::string::npos);

    std::string patch_pressure = reporter.build_heartbeat_patch_json(t, true);
    assert(patch_pressure.find("KubeletHasMemoryPressure") != std::string::npos);

    // Dynamic telemetry propagation assertions
    NodeTelemetry dynamic_t = t;
    dynamic_t.allocatable_gpio_count = 11;
    dynamic_t.free_heap_bytes = 180 * 1024;
    dynamic_t.allocatable_i2c_count = 1;

    std::string reg_dyn = reporter.build_registration_json(dynamic_t);
    assert(reg_dyn.find("\"esp32.io/gpio\":\"11\"") != std::string::npos);
    assert(reg_dyn.find("\"esp.io/gpio\":\"11\"") != std::string::npos);
    assert(reg_dyn.find("\"memory\":\"180Ki\"") != std::string::npos);
    assert(reg_dyn.find("\"esp32.io/i2c\":\"1\"") != std::string::npos);

    std::string patch_dyn = reporter.build_heartbeat_patch_json(dynamic_t, false);
    assert(patch_dyn.find("\"esp32.io/gpio\":\"11\"") != std::string::npos);
    assert(patch_dyn.find("\"esp.io/gpio\":\"11\"") != std::string::npos);
    assert(patch_dyn.find("\"memory\":\"180Ki\"") != std::string::npos);
    assert(patch_dyn.find("\"esp32.io/i2c\":\"1\"") != std::string::npos);

    // CapabilityFirewall live integration with collect_telemetry()
    kube32::hal::CapabilityFirewall::instance().reset();
    NodeTelemetry t_fresh = reporter.collect_telemetry();
    assert(t_fresh.allocatable_gpio_count == 14);

    // Register pod requesting 3 GPIOs
    std::string pod_manifest = R"({
        "spec": {
            "containers": [{
                "name": "c",
                "resources": { "requests": { "esp32.io/gpio": "3" } }
            }]
        }
    })";
    std::string fw_err;
    assert(kube32::hal::CapabilityFirewall::instance().register_pod("test-gpio-pod", pod_manifest, fw_err));

    NodeTelemetry t_allocated = reporter.collect_telemetry();
    assert(t_allocated.allocatable_gpio_count == 11);

    std::string patch_allocated = reporter.build_heartbeat_patch_json(t_allocated, false);
    assert(patch_allocated.find("\"esp32.io/gpio\":\"11\"") != std::string::npos);

    kube32::hal::CapabilityFirewall::instance().unregister_pod("test-gpio-pod");
    NodeTelemetry t_cleared = reporter.collect_telemetry();
    assert(t_cleared.allocatable_gpio_count == 14);

    std::cout << "[PASS] test_node_status_reporter" << std::endl;
}

void test_kubelet_mock_registration_and_heartbeat() {
    KubeletConfig cfg;
    cfg.node_name = "test-node-01";
    cfg.node_ip = "127.0.0.1";

    auto wasm_rt = kube32::runtime::create_wasm_runtime();
    auto mock_client = std::make_unique<MockKubeClient>();
    MockKubeClient* client_ref = mock_client.get();

    Kubelet kubelet(cfg, *wasm_rt, std::move(mock_client));

    kubelet.reconcile_node_registration();
    assert(client_ref->registered);
    assert(client_ref->register_count == 1);

    kubelet.report_heartbeat();
    assert(client_ref->node_patched);
    assert(client_ref->last_node_patch.find("KubeletReady") != std::string::npos);

    std::cout << "[PASS] test_kubelet_mock_registration_and_heartbeat" << std::endl;
}

void test_kubelet_differential_pod_sync() {
    KubeletConfig cfg;
    cfg.node_name = "test-node-01";

    auto wasm_rt = kube32::runtime::create_wasm_runtime();
    auto mock_client = std::make_unique<MockKubeClient>();
    MockKubeClient* client_ref = mock_client.get();

    // Prepare simulated pod assigned to this node
    KubePod pod;
    pod.name = "sensor-app";
    pod.namespace_name = "default";
    pod.node_name = "test-node-01";
    pod.status = "Pending";

    std::string manifest = R"({
        "metadata": { "name": "sensor-app", "namespace": "default" },
        "spec": {
            "nodeName": "test-node-01",
            "containers": [{ "name": "reader", "image": "wasm://sensor" }]
        }
    })";

    client_ref->mock_pods.emplace_back(pod, manifest);

    Kubelet kubelet(cfg, *wasm_rt, std::move(mock_client));

    // First sync tick: pod addition
    kubelet.sync_assigned_pods();

    auto active = kubelet.get_active_pod_keys();
    assert(active.size() == 1);
    assert(active[0] == "default/sensor-app");

    LocalPodRecord rec;
    assert(kubelet.get_pod_record("default/sensor-app", rec));
    assert(rec.execution_phase == PodExecutionPhase::Running);
    assert(client_ref->last_pod_status_patch.find("\"phase\":\"Running\"") != std::string::npos);

    // Second sync tick: pod deleted from cluster
    client_ref->mock_pods.clear();
    kubelet.sync_assigned_pods();

    active = kubelet.get_active_pod_keys();
    assert(active.empty());

    std::cout << "[PASS] test_kubelet_differential_pod_sync" << std::endl;
}

void test_kubelet_thread_lifecycle() {
    KubeletConfig cfg;
    cfg.node_name = "thread-node-01";

    auto wasm_rt = kube32::runtime::create_wasm_runtime();
    auto mock_client = std::make_unique<MockKubeClient>();

    Kubelet kubelet(cfg, *wasm_rt, std::move(mock_client));
    assert(kubelet.start());
    assert(kubelet.is_running());

    kube32::osal::Thread::sleep_ms(150);

    kubelet.stop();
    assert(!kubelet.is_running());

    std::cout << "[PASS] test_kubelet_thread_lifecycle" << std::endl;
}

// Mock Runtime with controllable ContainerStatus for deterministic trap verification
class MockControlledRuntime : public kube32::runtime::Runtime {
public:
    kube32::runtime::ContainerStatus mock_status;
    std::string deployed_pod_id;
    bool was_started{false};

    bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) override {
        (void)wasm_bytes; (void)len; (void)memory_limit_bytes;
        deployed_pod_id = pod_id;
        return true;
    }
    bool start(const std::string& pod_id) override {
        (void)pod_id;
        was_started = true;
        return true;
    }
    bool pause(const std::string& pod_id) override { (void)pod_id; return true; }
    bool terminate(const std::string& pod_id) override { (void)pod_id; return true; }
    kube32::runtime::ContainerStatus get_status(const std::string& pod_id) override {
        (void)pod_id;
        return mock_status;
    }
};

void test_kubelet_capability_admission_rejection() {
    kube32::hal::CapabilityFirewall::instance().reset();

    KubeletConfig cfg;
    cfg.node_name = "test-node-01";

    MockControlledRuntime mock_rt;
    auto mock_client = std::make_unique<MockKubeClient>();
    MockKubeClient* client_ref = mock_client.get();

    // Pod requesting reserved Octal Flash/PSRAM pin GPIO 28
    KubePod pod;
    pod.name = "bad-hardware-pod";
    pod.namespace_name = "default";
    pod.node_name = "test-node-01";
    pod.status = "Pending";

    std::string bad_manifest = R"({
        "metadata": { "name": "bad-hardware-pod", "namespace": "default" },
        "spec": {
            "nodeName": "test-node-01",
            "containers": [{
                "name": "attacker",
                "image": "wasm://gpio",
                "resources": { "requests": { "esp32.io/gpio-28": "1" } }
            }]
        }
    })";

    client_ref->mock_pods.emplace_back(pod, bad_manifest);

    Kubelet kubelet(cfg, mock_rt, std::move(mock_client));
    kubelet.sync_assigned_pods();

    LocalPodRecord rec;
    assert(kubelet.get_pod_record("default/bad-hardware-pod", rec));
    assert(rec.execution_phase == PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("Capability Admission Rejected") != std::string::npos);
    assert(rec.last_error_message.find("reserved or unsafe") != std::string::npos);

    // Verify status patch sent to API server
    assert(client_ref->last_pod_status_patch.find("\"phase\":\"Failed\"") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("\"reason\":\"CapabilityAdmissionFailed\"") != std::string::npos);

    // Verify runtime workload was NOT deployed or started
    assert(!mock_rt.was_started);
    assert(mock_rt.deployed_pod_id.empty());

    kube32::hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_kubelet_capability_admission_rejection" << std::endl;
}

void test_kubelet_dynamic_trap_status_reporting() {
    KubeletConfig cfg;
    cfg.node_name = "test-node-01";

    MockControlledRuntime mock_rt;
    auto mock_client = std::make_unique<MockKubeClient>();
    MockKubeClient* client_ref = mock_client.get();

    KubePod pod;
    pod.name = "trap-app";
    pod.namespace_name = "default";
    pod.node_name = "test-node-01";
    pod.status = "Pending";

    std::string manifest = R"({
        "metadata": { "name": "trap-app", "namespace": "default" },
        "spec": {
            "nodeName": "test-node-01",
            "containers": [{ "name": "worker", "image": "wasm://trap" }]
        }
    })";

    client_ref->mock_pods.emplace_back(pod, manifest);

    Kubelet kubelet(cfg, mock_rt, std::move(mock_client));
    kubelet.sync_assigned_pods();

    // 1. Division by Zero (exitCode 136)
    mock_rt.mock_status.phase = kube32::runtime::ContainerPhase::Failed;
    mock_rt.mock_status.exit_code = 136;
    mock_rt.mock_status.termination_reason = "IntegerDivisionByZero";
    mock_rt.mock_status.exit_message = "[trap] integer divide by zero";

    kubelet.monitor_and_report_pod_statuses();
    assert(client_ref->last_pod_status_patch.find("\"exitCode\":136") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("\"reason\":\"IntegerDivisionByZero\"") != std::string::npos);

    // 2. Unreachable Executed (exitCode 134)
    KubePod pod2;
    pod2.name = "trap-app-2";
    pod2.namespace_name = "default";
    pod2.node_name = "test-node-01";
    pod2.status = "Pending";

    std::string manifest2 = R"({
        "metadata": { "name": "trap-app-2", "namespace": "default" },
        "spec": {
            "nodeName": "test-node-01",
            "containers": [{ "name": "worker", "image": "wasm://trap2" }]
        }
    })";

    client_ref->mock_pods.clear();
    client_ref->mock_pods.emplace_back(pod2, manifest2);
    kubelet.sync_assigned_pods();

    mock_rt.mock_status.exit_code = 134;
    mock_rt.mock_status.termination_reason = "UnreachableExecuted";
    mock_rt.mock_status.exit_message = "[trap] unreachable executed";

    kubelet.monitor_and_report_pod_statuses();
    assert(client_ref->last_pod_status_patch.find("\"exitCode\":134") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("\"reason\":\"UnreachableExecuted\"") != std::string::npos);

    std::cout << "[PASS] test_kubelet_dynamic_trap_status_reporting" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running ESP32 Kubelet Node Agent Tests " << std::endl;
    std::cout << "========================================" << std::endl;

    test_node_status_reporter();
    test_kubelet_mock_registration_and_heartbeat();
    test_kubelet_differential_pod_sync();
    test_kubelet_thread_lifecycle();
    test_kubelet_capability_admission_rejection();
    test_kubelet_dynamic_trap_status_reporting();

    std::cout << "All Kubelet tests passed successfully!" << std::endl;
    return 0;
}
