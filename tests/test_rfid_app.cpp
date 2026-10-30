// The rfid example against a simulated MFRC522 on the stub I2C bus: reader
// detection, ISO 14443-A wake-up, two-level anticollision and select for a
// 7-byte UID, halt, a tag leaving the field, a 4-byte UID, and the pod's
// kube32.io/tick-interval.
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
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
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace kube32;
using namespace kube32::hal;

namespace {

uint16_t crc_a(const uint8_t* p, size_t n) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = static_cast<uint8_t>(p[i] ^ static_cast<uint8_t>(crc));
        ch = static_cast<uint8_t>(ch ^ static_cast<uint8_t>(ch << 4));
        crc = static_cast<uint16_t>((crc >> 8) ^ (static_cast<uint16_t>(ch) << 8) ^ (static_cast<uint16_t>(ch) << 3) ^ (ch >> 4));
    }
    return crc;
}

// Registers, FIFO and the Transceive command of an MFRC522, with one tag
// that can be put into and taken out of the field.
class Mfrc522 : public MockI2cDevice {
public:
    std::mutex m;
    std::vector<uint8_t> uid;   // empty: no tag in the field
    bool halted{false};
    int selects{0};

    int32_t write(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (!data || len == 0) return 0;
        reg_ = data[0];
        for (size_t i = 1; i < len; ++i) store(reg_, data[i]);
        return static_cast<int32_t>(len);
    }
    int32_t read(uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        for (size_t i = 0; i < len; ++i) {
            if (reg_ == 0x09) {
                buf[i] = fifo_.empty() ? 0 : fifo_.front();
                if (!fifo_.empty()) fifo_.pop_front();
            } else if (reg_ == 0x0A) {
                buf[i] = static_cast<uint8_t>(fifo_.size());
            } else {
                buf[i] = regs_[reg_];
            }
        }
        return static_cast<int32_t>(len);
    }

private:
    uint8_t reg_{0};
    uint8_t regs_[64] = {};
    std::deque<uint8_t> fifo_;

    void store(uint8_t reg, uint8_t v) {
        switch (reg) {
            case 0x01:   // Command
                if (v == 0x0F) { for (auto& r : regs_) r = 0; fifo_.clear(); regs_[0x37] = 0x15; }
                regs_[0x01] = v;
                break;
            case 0x04: regs_[0x04] &= static_cast<uint8_t>(~v); break;   // ComIrq: writing 1s clears
            case 0x09: fifo_.push_back(v); break;
            case 0x0A: if (v & 0x80) fifo_.clear(); break;
            case 0x0D:
                regs_[0x0D] = v;
                if ((v & 0x80) && regs_[0x01] == 0x0C) transceive(v & 0x07);
                break;
            default: regs_[reg] = v; break;
        }
    }

    void answer(std::initializer_list<uint8_t> bytes) {
        fifo_.assign(bytes.begin(), bytes.end());
        regs_[0x04] |= 0x30;
    }
    void answer_crc(std::vector<uint8_t> bytes) {
        const uint16_t c = crc_a(bytes.data(), bytes.size());
        bytes.push_back(static_cast<uint8_t>(c));
        bytes.push_back(static_cast<uint8_t>(c >> 8));
        fifo_.assign(bytes.begin(), bytes.end());
        regs_[0x04] |= 0x30;
    }
    void silence() { fifo_.clear(); regs_[0x04] |= 0x01; }   // the timer runs out

    // UID bytes of a cascade level as sent in the anticollision answer
    std::vector<uint8_t> level_bytes(int level) const {
        std::vector<uint8_t> b;
        if (uid.size() == 4) b.assign(uid.begin(), uid.end());
        else if (level == 0) b = {0x88, uid[0], uid[1], uid[2]};
        else b.assign(uid.begin() + 3, uid.begin() + 7);
        b.push_back(static_cast<uint8_t>(b[0] ^ b[1] ^ b[2] ^ b[3]));
        return b;
    }

    void transceive(uint8_t last_bits) {
        std::vector<uint8_t> f(fifo_.begin(), fifo_.end());
        fifo_.clear();
        regs_[0x06] = 0;
        if (uid.empty() || f.empty()) return silence();
        if (last_bits == 7 && f.size() == 1 && (f[0] == 0x52 || (f[0] == 0x26 && !halted))) {
            halted = false;
            return answer({static_cast<uint8_t>(uid.size() == 4 ? 0x04 : 0x44), 0x00});
        }
        if (f.size() == 2 && (f[0] == 0x93 || f[0] == 0x95) && f[1] == 0x20) {
            auto b = level_bytes(f[0] == 0x93 ? 0 : 1);
            fifo_.assign(b.begin(), b.end());
            regs_[0x04] |= 0x30;
            return;
        }
        if (f.size() == 9 && (f[0] == 0x93 || f[0] == 0x95) && f[1] == 0x70) {
            if (crc_a(f.data(), 7) != static_cast<uint16_t>(f[7] | (f[8] << 8))) return silence();
            const int level = f[0] == 0x93 ? 0 : 1;
            if (std::vector<uint8_t>(f.begin() + 2, f.begin() + 7) != level_bytes(level)) return silence();
            selects++;
            const bool more = uid.size() == 7 && level == 0;
            return answer_crc({static_cast<uint8_t>(more ? 0x04 : (uid.size() == 4 ? 0x08 : 0x00))});
        }
        if (f.size() == 4 && f[0] == 0x50 && f[1] == 0x00) { halted = true; return silence(); }
        silence();
    }
};

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
    auto reader = std::make_shared<Mfrc522>();
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
