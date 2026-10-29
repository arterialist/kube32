#include "kubelet/kubelet.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/image_registry.hpp"
#include "hal/capability_firewall.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <sstream>

using namespace kube32::kubelet;
using namespace kube32::models;

// =============================================================================
// Mock Kube API Client
// =============================================================================
class RecordingKubeClient : public KubeApiClient {
public:
    bool registered{false};
    int register_count{0};
    std::string last_node_registration_json;
    bool node_patched{false};
    std::string last_node_patch;
    std::vector<std::string> all_node_patches;

    std::vector<std::pair<KubePod, std::string>> mock_pods;
    std::string last_pod_status_patch;
    std::vector<std::pair<std::string, std::string>> pod_status_history; // (pod, patch)

    bool register_node(const std::string& node_json, bool& out_conflict) override {
        register_count++;
        registered = true;
        last_node_registration_json = node_json;
        out_conflict = false;
        return true;
    }

    bool patch_node_status(const std::string& node_name, const std::string& patch_json) override {
        (void)node_name;
        node_patched = true;
        last_node_patch = patch_json;
        all_node_patches.push_back(patch_json);
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
        last_pod_status_patch = status_patch_json;
        pod_status_history.emplace_back(ns + "/" + name, status_patch_json);
        return true;
    }

    void reset_patches() {
        node_patched = false;
        last_node_patch.clear();
        last_pod_status_patch.clear();
    }
};

// =============================================================================
// Mock Controlled Runtime for Verifying Deployment and Execution
// =============================================================================
class DeepControlledRuntime : public kube32::runtime::Runtime {
public:
    kube32::runtime::ContainerStatus mock_status;
    std::vector<std::string> deployed_workloads;
    std::vector<std::string> started_workloads;
    std::vector<std::string> terminated_workloads;
    int deploy_call_count{0};
    int start_call_count{0};

    bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) override {
        (void)wasm_bytes; (void)len; (void)memory_limit_bytes;
        deploy_call_count++;
        deployed_workloads.push_back(pod_id);
        return true;
    }

    bool start(const std::string& pod_id) override {
        start_call_count++;
        started_workloads.push_back(pod_id);
        return true;
    }

    bool pause(const std::string& pod_id) override { (void)pod_id; return true; }

    bool terminate(const std::string& pod_id) override {
        terminated_workloads.push_back(pod_id);
        return true;
    }

    kube32::runtime::ContainerStatus get_status(const std::string& pod_id) override {
        (void)pod_id;
        return mock_status;
    }

    void reset() {
        deployed_workloads.clear();
        started_workloads.clear();
        terminated_workloads.clear();
        deploy_call_count = 0;
        start_call_count = 0;
    }
};

// =============================================================================
// Hardware Pin Admission Rejection Verification
// =============================================================================
void test_hardware_pin_admission_rejection() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Hardware Pin Admission Rejection                               " << std::endl;
    std::cout << "==================================================================" << std::endl;

    // The set of reserved pins that MUST be rejected:
    // Strapping / Boot: GPIO 0
    // Octal Flash/PSRAM: GPIO 26, 27, 28, 29, 30, 31, 32
    // UART0 Console: GPIO 43, 44
    // USB: GPIO 19, 20
    const std::vector<uint32_t> reserved_boot_pins = {0};
    const std::vector<uint32_t> reserved_flash_psram_pins = {26, 27, 28, 29, 30, 31, 32};
    const std::vector<uint32_t> reserved_uart0_pins = {43, 44};
    const std::vector<uint32_t> reserved_usb_pins = {19, 20};

    std::vector<uint32_t> all_forbidden_pins;
    all_forbidden_pins.insert(all_forbidden_pins.end(), reserved_boot_pins.begin(), reserved_boot_pins.end());
    all_forbidden_pins.insert(all_forbidden_pins.end(), reserved_flash_psram_pins.begin(), reserved_flash_psram_pins.end());
    all_forbidden_pins.insert(all_forbidden_pins.end(), reserved_uart0_pins.begin(), reserved_uart0_pins.end());
    all_forbidden_pins.insert(all_forbidden_pins.end(), reserved_usb_pins.begin(), reserved_usb_pins.end());

    KubeletConfig cfg;
    cfg.node_name = "esp32-node-c1";
    cfg.node_ip = "192.168.4.10";

    for (uint32_t pin : all_forbidden_pins) {
        kube32::hal::CapabilityFirewall::instance().reset();

        DeepControlledRuntime mock_rt;
        auto mock_client = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref = mock_client.get();

        Kubelet kubelet(cfg, mock_rt, std::move(mock_client));

        std::string pod_name = "test-bad-pin-" + std::to_string(pin);
        std::string pod_key = "default/" + pod_name;

        KubePod pod;
        pod.name = pod_name;
        pod.namespace_name = "default";
        pod.node_name = cfg.node_name;
        pod.status = "Pending";

        // Test with esp32.io/gpio-<pin> prefix
        std::string manifest = "{\n"
            "  \"metadata\": { \"name\": \"" + pod_name + "\", \"namespace\": \"default\" },\n"
            "  \"spec\": {\n"
            "    \"nodeName\": \"" + cfg.node_name + "\",\n"
            "    \"containers\": [{\n"
            "      \"name\": \"worker\",\n"
            "      \"image\": \"wasm://sensor\",\n"
            "      \"resources\": { \"requests\": { \"esp32.io/gpio-" + std::to_string(pin) + "\": \"1\" } }\n"
            "    }]\n"
            "  }\n"
            "}";

        client_ref->mock_pods.emplace_back(pod, manifest);

        // Execute sync
        kubelet.sync_assigned_pods();

        // 1. Assert local pod record state
        LocalPodRecord rec;
        bool found = kubelet.get_pod_record(pod_key, rec);
        assert(found);
        assert(rec.execution_phase == PodExecutionPhase::Failed);
        assert(rec.last_error_message.find("Capability Admission Rejected") != std::string::npos);
        assert(rec.last_error_message.find("reserved or unsafe") != std::string::npos);

        // 2. Assert status patch sent to API client
        assert(!client_ref->last_pod_status_patch.empty());
        assert(client_ref->last_pod_status_patch.find("\"phase\":\"Failed\"") != std::string::npos);
        assert(client_ref->last_pod_status_patch.find("\"reason\":\"CapabilityAdmissionFailed\"") != std::string::npos);
        assert(client_ref->last_pod_status_patch.find("\"exitCode\":1") != std::string::npos);

        // 3. Assert Wasm bytecode was NEVER deployed or started
        assert(mock_rt.deploy_call_count == 0);
        assert(mock_rt.start_call_count == 0);
        assert(mock_rt.deployed_workloads.empty());
        assert(mock_rt.started_workloads.empty());

        // Now test the exact same pin with "esp.io/gpio-<pin>" alias
        kube32::hal::CapabilityFirewall::instance().reset();
        DeepControlledRuntime mock_rt2;
        auto mock_client2 = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref2 = mock_client2.get();

        Kubelet kubelet2(cfg, mock_rt2, std::move(mock_client2));
        std::string pod_name_alias = "test-bad-pin-alias-" + std::to_string(pin);
        std::string pod_key_alias = "default/" + pod_name_alias;

        KubePod pod2;
        pod2.name = pod_name_alias;
        pod2.namespace_name = "default";
        pod2.node_name = cfg.node_name;
        pod2.status = "Pending";

        std::string manifest_alias = "{\n"
            "  \"metadata\": { \"name\": \"" + pod_name_alias + "\", \"namespace\": \"default\" },\n"
            "  \"spec\": {\n"
            "    \"nodeName\": \"" + cfg.node_name + "\",\n"
            "    \"containers\": [{\n"
            "      \"name\": \"worker\",\n"
            "      \"image\": \"wasm://sensor\",\n"
            "      \"resources\": { \"requests\": { \"esp.io/gpio-" + std::to_string(pin) + "\": \"1\" } }\n"
            "    }]\n"
            "  }\n"
            "}";

        client_ref2->mock_pods.emplace_back(pod2, manifest_alias);
        kubelet2.sync_assigned_pods();

        LocalPodRecord rec2;
        assert(kubelet2.get_pod_record(pod_key_alias, rec2));
        assert(rec2.execution_phase == PodExecutionPhase::Failed);
        assert(client_ref2->last_pod_status_patch.find("\"reason\":\"CapabilityAdmissionFailed\"") != std::string::npos);
        assert(mock_rt2.deploy_call_count == 0);
        assert(mock_rt2.start_call_count == 0);

        std::cout << "  [PASS] Pin GPIO " << pin << " strictly rejected (both esp32.io and esp.io). Bytecode never deployed." << std::endl;
    }

    // 4. Test direct UART0 peripheral request ("esp32.io/uart-0")
    {
        kube32::hal::CapabilityFirewall::instance().reset();
        DeepControlledRuntime mock_rt;
        auto mock_client = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref = mock_client.get();

        Kubelet kubelet(cfg, mock_rt, std::move(mock_client));
        std::string pod_name = "test-bad-uart0";
        KubePod pod;
        pod.name = pod_name;
        pod.namespace_name = "default";
        pod.node_name = cfg.node_name;
        pod.status = "Pending";

        std::string manifest = "{\n"
            "  \"metadata\": { \"name\": \"" + pod_name + "\", \"namespace\": \"default\" },\n"
            "  \"spec\": {\n"
            "    \"nodeName\": \"" + cfg.node_name + "\",\n"
            "    \"containers\": [{\n"
            "      \"name\": \"worker\",\n"
            "      \"image\": \"wasm://uart\",\n"
            "      \"resources\": { \"requests\": { \"esp32.io/uart-0\": \"1\" } }\n"
            "    }]\n"
            "  }\n"
            "}";

        client_ref->mock_pods.emplace_back(pod, manifest);
        kubelet.sync_assigned_pods();

        LocalPodRecord rec;
        assert(kubelet.get_pod_record("default/" + pod_name, rec));
        assert(rec.execution_phase == PodExecutionPhase::Failed);
        assert(client_ref->last_pod_status_patch.find("\"reason\":\"CapabilityAdmissionFailed\"") != std::string::npos);
        assert(mock_rt.deploy_call_count == 0);
        assert(mock_rt.start_call_count == 0);
        std::cout << "  [PASS] Direct UART0 (esp32.io/uart-0) strictly rejected. Bytecode never deployed." << std::endl;
    }

    // 5. Test out-of-range GPIO pin (e.g. GPIO 65)
    {
        kube32::hal::CapabilityFirewall::instance().reset();
        DeepControlledRuntime mock_rt;
        auto mock_client = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref = mock_client.get();

        Kubelet kubelet(cfg, mock_rt, std::move(mock_client));
        std::string pod_name = "test-bad-pin-out-of-range";
        KubePod pod;
        pod.name = pod_name;
        pod.namespace_name = "default";
        pod.node_name = cfg.node_name;
        pod.status = "Pending";

        std::string manifest = "{\n"
            "  \"metadata\": { \"name\": \"" + pod_name + "\", \"namespace\": \"default\" },\n"
            "  \"spec\": {\n"
            "    \"nodeName\": \"" + cfg.node_name + "\",\n"
            "    \"containers\": [{\n"
            "      \"name\": \"worker\",\n"
            "      \"image\": \"wasm://sensor\",\n"
            "      \"resources\": { \"requests\": { \"esp32.io/gpio-65\": \"1\" } }\n"
            "    }]\n"
            "  }\n"
            "}";

        client_ref->mock_pods.emplace_back(pod, manifest);
        kubelet.sync_assigned_pods();

        LocalPodRecord rec;
        assert(kubelet.get_pod_record("default/" + pod_name, rec));
        assert(rec.execution_phase == PodExecutionPhase::Failed);
        assert(client_ref->last_pod_status_patch.find("\"reason\":\"CapabilityAdmissionFailed\"") != std::string::npos);
        assert(mock_rt.deploy_call_count == 0);
        std::cout << "  [PASS] Out-of-range GPIO (GPIO 65) strictly rejected. Bytecode never deployed." << std::endl;
    }

    std::cout << "[PASS] (11 hardware pin scenarios passed)." << std::endl;
}

// =============================================================================
// Dynamic Trap Exit Codes Verification
// =============================================================================
void test_dynamic_trap_exit_codes() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Dynamic Trap Exit Codes                                        " << std::endl;
    std::cout << "==================================================================" << std::endl;

    KubeletConfig cfg;
    cfg.node_name = "esp32-node-c2";
    cfg.node_ip = "192.168.4.11";

    struct TrapTestCase {
        std::string name;
        int32_t exit_code;
        std::string reason;
        std::string exit_message;
    };

    std::vector<TrapTestCase> cases = {
        {"div-zero-pod", 136, "Error", "wasm trap: integer divide by zero"},
        {"unreachable-pod", 134, "Error", "wasm trap: unreachable"},
        {"segv-pod", 139, "Error", "wasm trap: out of bounds memory access"},
        {"oom-pod", 137, "OOMKilled", "memory.grow refused at the pod memory limit of 64 KiB, then wasm trap: unreachable"},
        {"stopped-pod", 137, "Error", "stopped by the kubelet (wasm trap: out of gas)"},
        {"budget-pod", 152, "Error", "wasm trap: out of gas: a call used its whole execution budget of 4000000000 gas units (kube32.io/execution-budget)"},
        {"stack-pod", 139, "Error", "wasm trap: stack overflow"}
    };

    for (const auto& tc : cases) {
        kube32::hal::CapabilityFirewall::instance().reset();

        DeepControlledRuntime mock_rt;
        auto mock_client = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref = mock_client.get();

        KubePod pod;
        pod.name = tc.name;
        pod.namespace_name = "default";
        pod.node_name = cfg.node_name;
        pod.status = "Pending";

        std::string manifest = "{\n"
            "  \"metadata\": { \"name\": \"" + tc.name + "\", \"namespace\": \"default\" },\n"
            "  \"spec\": {\n"
            "    \"nodeName\": \"" + cfg.node_name + "\",\n"
            "    \"restartPolicy\": \"Never\",\n"
            "    \"containers\": [{ \"name\": \"worker\", \"image\": \"wasm://" + tc.name + "\" }]\n"
            "  }\n"
            "}";

        client_ref->mock_pods.emplace_back(pod, manifest);

        Kubelet kubelet(cfg, mock_rt, std::move(mock_client));
        // 1. Initial sync starts the pod
        kubelet.sync_assigned_pods();
        assert(mock_rt.deploy_call_count == 1);
        assert(mock_rt.start_call_count == 1);
        assert(client_ref->last_pod_status_patch.find("\"phase\":\"Running\"") != std::string::npos);

        // 2. Inject dynamic trap in runtime
        mock_rt.mock_status.phase = kube32::runtime::ContainerPhase::Failed;
        mock_rt.mock_status.exit_code = tc.exit_code;
        mock_rt.mock_status.termination_reason = tc.reason;
        mock_rt.mock_status.exit_message = tc.exit_message;

        // 3. Trigger Kubelet status monitoring loop
        client_ref->reset_patches();
        kubelet.monitor_and_report_pod_statuses();

        // 4. Assert exact exit code and reason serialization
        const std::string& patch = client_ref->last_pod_status_patch;
        assert(!patch.empty());
        assert(patch.find("\"phase\":\"Failed\"") != std::string::npos);

        std::string expected_exit_code = "\"exitCode\":" + std::to_string(tc.exit_code);
        std::string expected_reason = "\"reason\":\"" + tc.reason + "\"";

        assert(patch.find(expected_exit_code) != std::string::npos);
        assert(patch.find(expected_reason) != std::string::npos);

        // Assert local record matches
        LocalPodRecord rec;
        assert(kubelet.get_pod_record("default/" + tc.name, rec));
        assert(rec.execution_phase == PodExecutionPhase::Failed);
        assert(rec.restart_count == 0);  // restartPolicy Never

        std::cout << "  [PASS] Trap: " << tc.name << " -> ExitCode=" << tc.exit_code
                  << " (" << tc.reason << ") correctly serialized into status patch." << std::endl;
    }

    std::cout << "[PASS] (All dynamic trap codes & reasons match specification)." << std::endl;
}

// =============================================================================
// Dynamic Node Telemetry Verification
// =============================================================================
void test_dynamic_node_telemetry() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Dynamic Node Telemetry                                          " << std::endl;
    std::cout << "==================================================================" << std::endl;

    kube32::hal::CapabilityFirewall::instance().reset();

    NodeStatusReporter reporter("esp32-node-telemetry", "192.168.4.100");

    // Phase 1: Baseline telemetry
    NodeTelemetry t_base = reporter.collect_telemetry();
    std::cout << "  [INFO] Initial Allocatable GPIO: " << t_base.allocatable_gpio_count
              << ", Total GPIO: " << t_base.total_gpio_count << std::endl;
    const auto base = static_cast<uint32_t>(t_base.allocatable_gpio_count);
    const auto claimed3 = base - 3;
    assert(base > 3);
    assert(t_base.total_gpio_count >= t_base.allocatable_gpio_count);
    const std::string base_s = std::to_string(base);
    const std::string claimed3_s = std::to_string(claimed3);

    std::string patch_base = reporter.build_heartbeat_patch_json(t_base, false);
    assert(patch_base.find("\"esp32.io/gpio\":\"" + base_s + "\"") != std::string::npos);
    assert(patch_base.find("\"esp.io/gpio\":\"" + base_s + "\"") != std::string::npos);

    // Phase 2: Claim 3 generic GPIO pins via pod manifest
    std::string pod_manifest_3gpio = R"({
        "spec": {
            "containers": [{
                "name": "sensor-reader",
                "resources": { "requests": { "esp32.io/gpio": "3" } }
            }]
        }
    })";

    std::string fw_err;
    bool reg_ok = kube32::hal::CapabilityFirewall::instance().register_pod("default/sensor-reader", pod_manifest_3gpio, fw_err);
    assert(reg_ok);
    assert(fw_err.empty());

    NodeTelemetry t_claimed = reporter.collect_telemetry();
    std::cout << "  [INFO] After claiming 3 GPIOs -> Allocatable GPIO: " << t_claimed.allocatable_gpio_count << std::endl;
    assert(t_claimed.allocatable_gpio_count == claimed3);

    std::string patch_claimed = reporter.build_heartbeat_patch_json(t_claimed, false);
    assert(patch_claimed.find("\"esp32.io/gpio\":\"" + claimed3_s + "\"") != std::string::npos);
    assert(patch_claimed.find("\"esp.io/gpio\":\"" + claimed3_s + "\"") != std::string::npos);

    // Phase 3: Unregister pod and verify the allocatable count is restored
    kube32::hal::CapabilityFirewall::instance().unregister_pod("default/sensor-reader");

    NodeTelemetry t_restored = reporter.collect_telemetry();
    std::cout << "  [INFO] After unregistering pod -> Allocatable GPIO: " << t_restored.allocatable_gpio_count << std::endl;
    assert(t_restored.allocatable_gpio_count == base);

    std::string patch_restored = reporter.build_heartbeat_patch_json(t_restored, false);
    assert(patch_restored.find("\"esp32.io/gpio\":\"" + base_s + "\"") != std::string::npos);
    assert(patch_restored.find("\"esp.io/gpio\":\"" + base_s + "\"") != std::string::npos);

    // Phase 4: Full Kubelet instance heartbeat reporting cycle with client inspection
    {
        KubeletConfig cfg;
        cfg.node_name = "esp32-node-telemetry";
        cfg.node_ip = "192.168.4.100";

        DeepControlledRuntime mock_rt;
        auto mock_client = std::make_unique<RecordingKubeClient>();
        RecordingKubeClient* client_ref = mock_client.get();

        Kubelet kubelet(cfg, mock_rt, std::move(mock_client));

        // Reconcile registration
        kubelet.reconcile_node_registration();
        assert(client_ref->registered);
        assert(client_ref->last_node_registration_json.find("\"esp32.io/gpio\":\"" + base_s + "\"") != std::string::npos);

        // Heartbeat with 0 claimed
        kubelet.report_heartbeat();
        assert(client_ref->last_node_patch.find("\"esp32.io/gpio\":\"" + base_s + "\"") != std::string::npos);

        // Claim 3 pins via firewall
        reg_ok = kube32::hal::CapabilityFirewall::instance().register_pod("default/sensor-reader", pod_manifest_3gpio, fw_err);
        assert(reg_ok);

        // Heartbeat with 3 claimed
        client_ref->reset_patches();
        kubelet.report_heartbeat();
        assert(client_ref->last_node_patch.find("\"esp32.io/gpio\":\"" + claimed3_s + "\"") != std::string::npos);

        // Unregister pod
        kube32::hal::CapabilityFirewall::instance().unregister_pod("default/sensor-reader");

        // Heartbeat after unregister
        client_ref->reset_patches();
        kubelet.report_heartbeat();
        assert(client_ref->last_node_patch.find("\"esp32.io/gpio\":\"" + base_s + "\"") != std::string::npos);
        std::cout << "  [PASS] Kubelet heartbeat cycle tracks allocatable GPIO as pins are claimed and released." << std::endl;
    }

    // Phase 5: Claim every allocatable pin
    {
        std::string pod_manifest_14gpio = "{\"spec\": {\"containers\": [{\"name\": \"greedy-gpio\", "
            "\"resources\": { \"requests\": { \"esp32.io/gpio\": \"" + base_s + "\" } }}]}}";

        reg_ok = kube32::hal::CapabilityFirewall::instance().register_pod("default/greedy-gpio", pod_manifest_14gpio, fw_err);
        assert(reg_ok);

        NodeTelemetry t_max = reporter.collect_telemetry();
        assert(t_max.allocatable_gpio_count == 0);
        std::string patch_zero = reporter.build_heartbeat_patch_json(t_max, false);
        assert(patch_zero.find("\"esp32.io/gpio\":\"0\"") != std::string::npos);

        // Claim remaining physical safe pins until firewall exhaustion
        // Count the safe pins in KUBE32_SAFE_GPIO_MASK
        uint32_t remaining_safe = 0;
        for (uint32_t p = 1; p < 64; ++p) {
            if ((kube32::hal::KUBE32_SAFE_GPIO_MASK & (1ULL << p)) != 0) remaining_safe++;
        }
        std::cout << "  [INFO] Total physical safe GPIO pins in firewall: " << remaining_safe << std::endl;

        // Claim all remaining safe pins
        uint32_t to_claim = remaining_safe - base;
        std::string pod_manifest_rest = "{\n"
            "  \"spec\": {\n"
            "    \"containers\": [{\n"
            "      \"name\": \"rest-gpio\",\n"
            "      \"resources\": { \"requests\": { \"esp32.io/gpio\": \"" + std::to_string(to_claim) + "\" } }\n"
            "    }]\n"
            "  }\n"
            "}";
        reg_ok = kube32::hal::CapabilityFirewall::instance().register_pod("default/rest-gpio", pod_manifest_rest, fw_err);
        assert(reg_ok);

        // Now firewall is 100% physically exhausted: next pin request MUST fail
        std::string pod_manifest_1more = R"({
            "spec": {
                "containers": [{
                    "name": "one-more",
                    "resources": { "requests": { "esp32.io/gpio": "1" } }
                }]
            }
        })";
        bool fail_ok = kube32::hal::CapabilityFirewall::instance().register_pod("default/one-more", pod_manifest_1more, fw_err);
        assert(!fail_ok);
        assert(fw_err.find("Insufficient allocatable GPIO") != std::string::npos);

        // Teardown all
        kube32::hal::CapabilityFirewall::instance().unregister_pod("default/greedy-gpio");
        kube32::hal::CapabilityFirewall::instance().unregister_pod("default/rest-gpio");
        NodeTelemetry t_restored_all = reporter.collect_telemetry();
        assert(t_restored_all.allocatable_gpio_count == base);
        std::cout << "  [PASS] Boundary condition (0 allocatable clamp, full physical exhaustion rejected, restoration) passed." << std::endl;
    }

    std::cout << "[PASS] (Dynamic telemetry transitions and boundaries verified)." << std::endl;
}

// =============================================================================
// MAIN ENTRY POINT
// =============================================================================
int main() {
    // Pods in these tests reference images that must exist in the local registry
    static const uint8_t k_min_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    for (const char* key : {"sensor", "uart", "div-zero-pod", "unreachable-pod", "segv-pod", "oom-pod", "stopped-pod", "budget-pod", "stack-pod"}) {
        kube32::runtime::ImageRegistry::instance().add({key, k_min_wasm, sizeof(k_min_wasm)});
    }

    std::cout << "==================================================================" << std::endl;
    std::cout << " Kubelet Node Status Tests                                       " << std::endl;
    std::cout << " Testing: Hardware Admission, Dynamic Traps, Node Telemetry       " << std::endl;
    std::cout << "==================================================================" << std::endl;

    test_hardware_pin_admission_rejection();
    test_dynamic_trap_exit_codes();
    test_dynamic_node_telemetry();

    std::cout << "\n==================================================================" << std::endl;
    std::cout << " ALL KUBELET NODE STATUS TESTS PASSED                          " << std::endl;
    std::cout << "==================================================================" << std::endl;
    return 0;
}
