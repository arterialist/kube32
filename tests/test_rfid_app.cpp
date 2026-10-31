// The rfid example against a simulated MFRC522 on the stub I2C bus: reader
// detection, ISO 14443-A wake-up, two-level anticollision and select for a
// 7-byte UID, halt, a tag leaving the field, a 4-byte UID, and the pod's
// kube32.io/tick-interval.
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "mfrc522_sim.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/wasm_runtime.hpp"
#include "embedded/embedded_blobs.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace kube32;
using namespace kube32::hal;

namespace {

class Client : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    bool register_node(std::string_view, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string&) override { return true; }
    int get(const std::string&, std::string& body) override { body = "{\"items\":[]}"; return 200; }
};

bool log_has(const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail("default/rfid", 0, lines);
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

// Runs the pod's ticks for `ms`, counting them
int run_ticks(uint32_t ms, const std::function<bool()>& until = nullptr) {
    int rounds = 0;
    const uint64_t end = osal::System::get_uptime_ms() + ms;
    while (osal::System::get_uptime_ms() < end) {
        const uint32_t wait = runtime::AppPodHost::instance().tick_due(osal::System::get_uptime_ms());
        rounds++;
        if (until && until()) break;
        osal::Thread::sleep_ms(std::max<uint32_t>(wait, 5));
    }
    return rounds;
}

void test_rfid_reader() {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    MockHal::instance().reset();
    auto reader = std::make_shared<k32test::Mfrc522>();
    MockHal::instance().register_mock_i2c_device(0, 0x28, reader);
    runtime::PodLogStore::runtime().clear();

    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    auto client = std::make_unique<Client>();
    Client* c = client.get();
    models::KubePod p;
    p.name = "rfid";
    p.namespace_name = "default";
    p.node_name = "node-a";
    p.annotations["kube32.io/tick-interval"] = "200ms";
    c->pods.emplace_back(p, R"({"metadata":{"name":"rfid","namespace":"default","uid":"rfid-uid","annotations":{"kube32.io/tick-interval":"200ms"}},
        "spec":{"nodeName":"node-a","containers":[{"name":"rfid","image":"wasm://embedded/rfid",
        "env":[{"name":"TAG_04A1B2C3D4E580","value":"nec 0x00 0x45"}],
        "resources":{"requests":{"memory":"4Ki","esp32.io/i2c-bus-0":"1"},"limits":{"memory":"4Ki"}}}]}})");
    kubelet::Kubelet k(cfg, *rt, std::move(client));
    k.sync_assigned_pods();
    assert(runtime::AppPodHost::instance().has_pod("default/rfid"));
    assert(log_has("rfid: reader at 0x28 on bus 0, version 0x15"));

    // An empty field: about five polls a second
    const int rounds = run_ticks(1000);
    assert(rounds >= 4 && rounds <= 8);
    assert(!log_has("rfid: tag"));

    // A 7-byte UID: two cascade levels, then halt
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x80}; }
    run_ticks(1000, [] { return log_has("rfid: tag 04a1b2c3d4e580"); });
    assert(log_has("rfid: tag 04a1b2c3d4e580 sak 0x00"));
    assert(log_has("04A1B2C3D4E580's message on ir.send"));
    assert(reader->halted && reader->selects >= 2);

    // Still in the field: no second arrival; then it leaves
    run_ticks(600);
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail("default/rfid", 0, lines);
    int arrivals = 0;
    for (const auto& l : lines) if (l.text.find("rfid: tag 04a1b2c3d4e580 sak") != std::string::npos) arrivals++;
    assert(arrivals == 1);
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid.clear(); }
    run_ticks(1500, [] { return log_has("rfid: tag 04a1b2c3d4e580 left"); });
    assert(log_has("rfid: tag 04a1b2c3d4e580 left"));

    // A 4-byte UID (MIFARE Classic)
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0xde, 0xad, 0xbe, 0xef}; }
    run_ticks(1000, [] { return log_has("rfid: tag deadbeef sak 0x08"); });
    assert(log_has("rfid: tag deadbeef sak 0x08"));

    c->pods.clear();
    k.sync_assigned_pods();
    assert(!i2c_bus_state(0).installed);
    fw.reset();
    std::cout << "[PASS] test_rfid_reader" << std::endl;
}

} // namespace

int main() {
    test_rfid_reader();
    std::cout << "All rfid tests passed" << std::endl;
    return 0;
}
