#include "kubelet/kubelet.hpp"
#include "runtime/image_registry.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/wasm_runtime.hpp"
#include "hal/capability_firewall.hpp"
#include "runtime/pod_log.hpp"
#include <iostream>
#include <cassert>
#include "osal/system.hpp"
#include <thread>

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

    std::string shipped_logs;
    bool push_pod_logs(const std::string&, const std::string&, const std::string& ndjson) override {
        shipped_logs += ndjson;
        return true;
    }
};

void test_node_status_reporter() {
    NodeStatusReporter reporter("esp32-node-test", "192.168.4.2");
    NodeTelemetry t = reporter.collect_telemetry();
    // Host figures: the machine's memory and cores, no PSRAM
    assert(t.total_heap_bytes > 0 && t.free_heap_bytes > 0);
    assert(t.cpu_cores == std::thread::hardware_concurrency());
    assert(t.total_psram_bytes == 0);
    {
        std::string host_reg = reporter.build_registration_json(t);
        assert(host_reg.find("psram") == std::string::npos);   // no PSRAM: not advertised
        assert(host_reg.find("\"cpu\":\"" + std::to_string(t.cpu_cores) + "\"") != std::string::npos);
        assert(host_reg.find("\"kubernetes.io/arch\":\"xtensa\"") == std::string::npos);
    }
    t.total_heap_bytes = 320 * 1024;

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

    // CapabilityFirewall live integration with collect_telemetry(): capacity
    // is what the board profile lets pods claim
    kube32::hal::CapabilityFirewall::instance().reset();
    NodeTelemetry t_fresh = reporter.collect_telemetry();
    const uint32_t safe_pins = static_cast<uint32_t>(__builtin_popcountll(kube32::hal::KUBE32_SAFE_GPIO_MASK));
    assert(safe_pins == 33);   // 45 ESP32-S3 pads minus 12 reserved on every board
    assert(t_fresh.total_gpio_count == safe_pins);
    assert(t_fresh.allocatable_gpio_count == safe_pins);
    assert(t_fresh.total_adc_count == 10 && t_fresh.total_i2c_count == 2);

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
    assert(t_allocated.allocatable_gpio_count == safe_pins - 3);

    std::string patch_allocated = reporter.build_heartbeat_patch_json(t_allocated, false);
    assert(patch_allocated.find("\"esp32.io/gpio\":\"" + std::to_string(safe_pins - 3) + "\"") != std::string::npos);

    kube32::hal::CapabilityFirewall::instance().unregister_pod("test-gpio-pod");
    NodeTelemetry t_cleared = reporter.collect_telemetry();
    assert(t_cleared.allocatable_gpio_count == safe_pins);

    // A Cardputer-style profile: on-board pins and buses without a driver
    // leave the node's capacity. GPIO 2-9 also take ADC1 channels 1-8.
    kube32::hal::BoardProfile prof;
    prof.name = "test-board";
    prof.reserved_gpio_mask = 0x3FCULL | (1ULL << 35);   // GPIO 2-9 and 35
    prof.i2c_bus_mask = 0;
    kube32::hal::CapabilityFirewall::instance().set_board_profile(prof);
    NodeTelemetry t_board = reporter.collect_telemetry();
    assert(t_board.total_gpio_count == safe_pins - 9);
    assert(t_board.total_adc_count == 2);       // channels 0 (GPIO 1) and 9 (GPIO 10)
    assert(t_board.total_i2c_count == 0);
    std::string reg_board = reporter.build_registration_json(t_board);
    assert(reg_board.find("esp32.io/i2c") == std::string::npos);   // no driver: not advertised
    assert(reg_board.find("\"esp32.io/adc\":\"2\"") != std::string::npos);
    kube32::hal::CapabilityFirewall::instance().reset();

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

    // Ready follows the pod sync loop: NotReady until a pass completes
    kubelet.report_heartbeat();
    assert(client_ref->node_patched);
    assert(client_ref->last_node_patch.find("{\"type\":\"Ready\",\"status\":\"False\",\"reason\":\"KubeletNotReady\"") != std::string::npos);
    kubelet.sync_assigned_pods();
    kubelet.report_heartbeat();
    assert(client_ref->last_node_patch.find("{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\"") != std::string::npos);

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
    int deploy_count{0};
    int terminate_count{0};

    bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) override {
        (void)wasm_bytes; (void)len; (void)memory_limit_bytes;
        deployed_pod_id = pod_id;
        deploy_count++;
        mock_status = kube32::runtime::ContainerStatus{};
        mock_status.phase = kube32::runtime::ContainerPhase::Running;
        return true;
    }
    bool start(const std::string& pod_id) override {
        (void)pod_id;
        was_started = true;
        return true;
    }
    bool pause(const std::string& pod_id) override { (void)pod_id; return true; }
    bool terminate(const std::string& pod_id) override { (void)pod_id; terminate_count++; return true; }
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
    mock_rt.mock_status.termination_reason = "Error";
    mock_rt.mock_status.exit_message = "wasm trap: integer divide by zero";

    kubelet.monitor_and_report_pod_statuses();
    assert(client_ref->last_pod_status_patch.find("\"exitCode\":136") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("\"reason\":\"Error\"") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("wasm trap: integer divide by zero") != std::string::npos);

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

    mock_rt.mock_status.phase = kube32::runtime::ContainerPhase::Failed;
    mock_rt.mock_status.exit_code = 134;
    mock_rt.mock_status.termination_reason = "Error";
    mock_rt.mock_status.exit_message = "wasm trap: unreachable";

    kubelet.monitor_and_report_pod_statuses();
    assert(client_ref->last_pod_status_patch.find("\"exitCode\":134") != std::string::npos);
    assert(client_ref->last_pod_status_patch.find("\"reason\":\"Error\"") != std::string::npos);

    std::cout << "[PASS] test_kubelet_dynamic_trap_status_reporting" << std::endl;
}

// restartPolicy: Never reports the exit code and stops; OnFailure restarts
// failures after a doubling back-off and completes on exit 0; Always
// restarts whatever the exit code. Containers are released when they end.
static std::string pod_manifest_with_policy(const std::string& name, const std::string& policy) {
    return "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"uid\":\"u-" + name + "\"},"
           "\"spec\":{\"nodeName\":\"test-node-01\",\"restartPolicy\":\"" + policy + "\","
           "\"containers\":[{\"name\":\"main\",\"image\":\"wasm://app\"}]}}";
}

void test_kubelet_restart_policy() {
    std::cout << "[RUN] test_kubelet_restart_policy" << std::endl;
    using kube32::runtime::ContainerPhase;
    auto exited = [](MockControlledRuntime& rt, int32_t code) {
        rt.mock_status.phase = ContainerPhase::Terminated;
        rt.mock_status.exit_code = code;
        rt.mock_status.termination_reason = code == 0 ? "Completed" : "Error";
        rt.mock_status.exit_message = "entry point returned " + std::to_string(code);
    };

    // Never: app_main returned 3 -> Failed with exit code 3, no restart
    {
        KubeletConfig cfg;
        cfg.node_name = "test-node-01";
        MockControlledRuntime rt;
        auto client = std::make_unique<MockKubeClient>();
        MockKubeClient* c = client.get();
        KubePod pod; pod.name = "job-never"; pod.namespace_name = "default"; pod.node_name = "test-node-01";
        c->mock_pods.emplace_back(pod, pod_manifest_with_policy("job-never", "Never"));
        Kubelet k(cfg, rt, std::move(client));
        k.sync_assigned_pods();
        exited(rt, 3);
        k.monitor_and_report_pod_statuses();
        const std::string& p = c->last_pod_status_patch;
        assert(p.find("\"phase\":\"Failed\"") != std::string::npos);
        assert(p.find("\"exitCode\":3") != std::string::npos);
        assert(p.find("\"reason\":\"Error\"") != std::string::npos);
        assert(p.find("\"restartCount\":0") != std::string::npos);
        assert(rt.terminate_count == 1);   // container released
        k.sync_assigned_pods();
        assert(rt.deploy_count == 1);      // never restarted
    }

    // OnFailure: exit 0 completes the pod
    {
        KubeletConfig cfg;
        cfg.node_name = "test-node-01";
        MockControlledRuntime rt;
        auto client = std::make_unique<MockKubeClient>();
        MockKubeClient* c = client.get();
        KubePod pod; pod.name = "job-ok"; pod.namespace_name = "default"; pod.node_name = "test-node-01";
        c->mock_pods.emplace_back(pod, pod_manifest_with_policy("job-ok", "OnFailure"));
        Kubelet k(cfg, rt, std::move(client));
        k.sync_assigned_pods();
        exited(rt, 0);
        k.monitor_and_report_pod_statuses();
        assert(c->last_pod_status_patch.find("\"phase\":\"Succeeded\"") != std::string::npos);
        assert(c->last_pod_status_patch.find("\"exitCode\":0") != std::string::npos);
    }

    // OnFailure: a trap is restarted after the back-off, which doubles
    {
        KubeletConfig cfg;
        cfg.node_name = "test-node-01";
        cfg.restart_backoff_initial_ms = 300;
        cfg.restart_backoff_max_ms = 1000;
        MockControlledRuntime rt;
        auto client = std::make_unique<MockKubeClient>();
        MockKubeClient* c = client.get();
        KubePod pod; pod.name = "crasher"; pod.namespace_name = "default"; pod.node_name = "test-node-01";
        c->mock_pods.emplace_back(pod, pod_manifest_with_policy("crasher", "OnFailure"));
        Kubelet k(cfg, rt, std::move(client));
        k.sync_assigned_pods();
        rt.mock_status.phase = ContainerPhase::Failed;
        rt.mock_status.exit_code = 136;
        rt.mock_status.termination_reason = "Error";
        rt.mock_status.exit_message = "wasm trap: integer divide by zero";
        k.monitor_and_report_pod_statuses();
        std::string p = c->last_pod_status_patch;
        assert(p.find("\"phase\":\"Running\"") != std::string::npos);
        assert(p.find("\"waiting\":{\"reason\":\"CrashLoopBackOff\"") != std::string::npos);
        assert(p.find("\"lastState\":{\"terminated\":{\"exitCode\":136,\"reason\":\"Error\"") != std::string::npos);
        assert(p.find("\"restartCount\":0") != std::string::npos);
        LocalPodRecord rec;
        assert(k.get_pod_record("default/crasher", rec));
        assert(rec.execution_phase == PodExecutionPhase::CrashLoopBackOff);
        uint64_t now = kube32::osal::System::get_uptime_ms();
        assert(rec.next_restart_allowed_ms > now && rec.next_restart_allowed_ms - now <= 300);
        k.sync_assigned_pods();
        assert(rt.deploy_count == 1);       // still backing off
        // A line of the stopped container not yet shipped when the restart comes
        kube32::runtime::PodLogStore::runtime().append("default/crasher", "old container's last line");
        kube32::osal::Thread::sleep_ms(350);
        k.sync_assigned_pods();
        assert(rt.deploy_count == 2);       // restarted
        // Shipped lines name the container instance that wrote them
        kube32::runtime::PodLogStore::runtime().append("default/crasher", "new container");
        k.ship_pod_logs();
        assert(c->shipped_logs.find("\"r\":0,\"m\":\"old container's last line\"") != std::string::npos);
        assert(c->shipped_logs.find("\"r\":1,\"m\":\"new container\"") != std::string::npos);
        p = c->last_pod_status_patch;
        assert(p.find("\"phase\":\"Running\"") != std::string::npos);
        assert(p.find("\"restartCount\":1") != std::string::npos);
        assert(p.find("\"lastState\":{\"terminated\":{\"exitCode\":136") != std::string::npos);
        rt.mock_status.phase = ContainerPhase::Failed;
        rt.mock_status.exit_code = 137;
        rt.mock_status.termination_reason = "OOMKilled";
        k.monitor_and_report_pod_statuses();
        assert(k.get_pod_record("default/crasher", rec));
        now = kube32::osal::System::get_uptime_ms();
        assert(rec.next_restart_allowed_ms - now > 300 && rec.next_restart_allowed_ms - now <= 600);   // doubled
        assert(rec.restart_count == 1);
    }

    // Always: even exit 0 is restarted
    {
        KubeletConfig cfg;
        cfg.node_name = "test-node-01";
        MockControlledRuntime rt;
        auto client = std::make_unique<MockKubeClient>();
        MockKubeClient* c = client.get();
        KubePod pod; pod.name = "server"; pod.namespace_name = "default"; pod.node_name = "test-node-01";
        c->mock_pods.emplace_back(pod, pod_manifest_with_policy("server", "Always"));
        Kubelet k(cfg, rt, std::move(client));
        k.sync_assigned_pods();
        exited(rt, 0);
        k.monitor_and_report_pod_statuses();
        assert(c->last_pod_status_patch.find("CrashLoopBackOff") != std::string::npos);
        assert(c->last_pod_status_patch.find("back-off 10s restarting failed container=main pod=server_default(u-server)") != std::string::npos);
    }
    std::cout << "[PASS] test_kubelet_restart_policy" << std::endl;
}

void test_http_chunked_response_decoding() {
    std::cout << "[RUN] test_http_chunked_response_decoding" << std::endl;

    // 1. Header Detection: Various casings, commas, parameters
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("transfer-encoding: chunked\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("TRANSFER-ENCODING: CHUNKED\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: gzip, chunked\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked; charset=utf-8\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Content-Length: 42\r\nContent-Type: application/json\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: gzip\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding(""));

    // 2. Single Chunk (exact wire format emitted by TableSerializer)
    std::string single_chunk = "3D\r\n{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}\r\n0\r\n\r\n";
    std::string decoded = HttpKubeClient::decode_chunked_body(single_chunk);
    assert(decoded == "{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}");

    // Verify models::parse_json parses cleanly without error
    kube32::models::JsonValue doc;
    std::string err;
    assert(kube32::models::parse_json(decoded, doc, err));
    assert(doc.is_object());
    assert(doc.get("kind") && doc.get("kind")->str_val == "PodList");

    // 3. Multi-Chunk Streaming
    std::string multi_chunk = "12\r\n{\"kind\":\"PodList\",\r\n2B\r\n\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}\r\n0\r\n\r\n";
    std::string multi_decoded = HttpKubeClient::decode_chunked_body(multi_chunk);
    assert(multi_decoded == "{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}");
    assert(kube32::models::parse_json(multi_decoded, doc, err));

    // 4. Chunk Extensions and Whitespace
    std::string ext_chunk = "  3D ;ext=param;foo=bar\r\n{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}\r\n0;terminal-ext\r\n\r\n";
    std::string ext_decoded = HttpKubeClient::decode_chunked_body(ext_chunk);
    assert(ext_decoded == "{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[]}");

    // 5. Trailers Discarded After Terminal 0 Chunk
    std::string trailer_chunk = "13\r\n{\"status\":\"Active\"}\r\n0\r\nTrailer-Header: checksum\r\n\r\n";
    std::string trailer_decoded = HttpKubeClient::decode_chunked_body(trailer_chunk);
    assert(trailer_decoded == "{\"status\":\"Active\"}");

    // 6. Non-Chunked Payload Handling
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Content-Length: 32\r\n"));

    std::cout << "[PASS] test_http_chunked_response_decoding" << std::endl;
}

int main() {
    // Pods in these tests reference images that must exist in the local registry
    static const uint8_t k_min_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    for (const char* key : {"sensor", "gpio", "trap", "trap2", "app", "test"}) {
        kube32::runtime::ImageRegistry::instance().add({key, k_min_wasm, sizeof(k_min_wasm)});
    }

    std::cout << "========================================" << std::endl;
    std::cout << " Running ESP32 Kubelet Node Agent Tests " << std::endl;
    std::cout << "========================================" << std::endl;

    test_node_status_reporter();
    test_kubelet_mock_registration_and_heartbeat();
    test_kubelet_differential_pod_sync();
    test_kubelet_thread_lifecycle();
    test_kubelet_capability_admission_rejection();
    test_kubelet_dynamic_trap_status_reporting();
    test_kubelet_restart_policy();
    test_http_chunked_response_decoding();

    std::cout << "All Kubelet tests passed successfully!" << std::endl;
    return 0;
}
