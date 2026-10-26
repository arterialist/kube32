#include "kubelet/kubelet.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/wasm_runtime.hpp"
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

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running ESP32 Kubelet Node Agent Tests " << std::endl;
    std::cout << "========================================" << std::endl;

    test_node_status_reporter();
    test_kubelet_mock_registration_and_heartbeat();
    test_kubelet_differential_pod_sync();
    test_kubelet_thread_lifecycle();

    std::cout << "All Kubelet tests passed successfully!" << std::endl;
    return 0;
}
