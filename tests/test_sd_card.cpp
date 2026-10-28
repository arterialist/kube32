// microSD card on the host: the HAL over a KUBE32_SD_ROOT directory, the
// pod storage ABI (k32_fs_*) through PodFs and through a real Wasm guest,
// images pulled from the card by the kubelet, and the node advertising the
// card. Every path is the same code the firmware runs over FATFS.
#include "hal/sd_card.hpp"
#include "hal/capability_firewall.hpp"
#include "runtime/pod_fs.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/app_pod.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "kubelet/kube_api_client.hpp"
#include "embedded/embedded_blobs.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace kube32;

static std::string g_root;

static std::string make_temp_root() {
    char tmpl[] = "/tmp/kube32-sd-XXXXXX";
    char* p = mkdtemp(tmpl);
    assert(p);
    return std::string(p);
}

static void mount_root() {
    hal::sd_unmount();
    setenv("KUBE32_SD_ROOT", g_root.c_str(), 1);
    assert(hal::sd_mount());
}

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void write_file(const std::string& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f.good());
}

static const runtime::EmbeddedImage* embedded_image(const char* name) {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    for (size_t i = 0; i < n; ++i) if (std::strcmp(imgs[i].name, name) == 0) return &imgs[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
void test_hal_without_env() {
    hal::sd_unmount();
    unsetenv("KUBE32_SD_ROOT");
    assert(!hal::sd_mount());
    assert(!hal::sd_is_mounted());
    assert(hal::sd_root_path().empty());
    hal::SdInfo info = hal::sd_info();
    assert(!info.mounted);
    assert(info.error.find("KUBE32_SD_ROOT") != std::string::npos);

    kubelet::NodeStatusReporter reporter("host-node", "127.0.0.1");
    kubelet::NodeTelemetry t = reporter.collect_telemetry();
    assert(!t.sd_present);
    assert(reporter.build_registration_json(t).find("hardware.kube32.io/sd") == std::string::npos);
    assert(reporter.build_heartbeat_patch_json(t, false).find("hardware.kube32.io/sd") == std::string::npos);
    std::cout << "[PASS] test_hal_without_env" << std::endl;
}

void test_hal_with_env() {
    setenv("KUBE32_SD_ROOT", "/definitely/not/a/directory", 1);
    hal::sd_unmount();
    assert(!hal::sd_mount());
    assert(hal::sd_info().error.find("not a directory") != std::string::npos);

    mount_root();
    assert(hal::sd_is_mounted());
    assert(hal::sd_root_path() == g_root);
    hal::SdInfo info = hal::sd_info();
    assert(info.mounted);
    assert(info.total_bytes > 0);
    assert(info.free_bytes > 0 && info.free_bytes <= info.total_bytes);
    assert(info.type.find("simulated") != std::string::npos);
    assert(info.error.empty());

    kubelet::NodeStatusReporter reporter("host-node", "127.0.0.1");
    kubelet::NodeTelemetry t = reporter.collect_telemetry();
    assert(t.sd_present && t.sd_free_bytes == info.free_bytes);
    std::string reg = reporter.build_registration_json(t);
    assert(reg.find("\"hardware.kube32.io/sd\":\"") != std::string::npos);
    std::string hb = reporter.build_heartbeat_patch_json(t, false);
    assert(hb.find("\"hardware.kube32.io/sd\":\"") != std::string::npos);
    // allocatable carries the measured free space as a quantity
    std::string key = "\"hardware.kube32.io/sd\":\"";
    size_t at = hb.find(key) + key.size();
    std::string q = hb.substr(at, hb.find('"', at) - at);
    uint64_t parsed = models::parse_quantity(q);
    assert(parsed == info.free_bytes);
    std::cout << "[PASS] test_hal_with_env" << std::endl;
}

// ---------------------------------------------------------------------------
static const char* k_sd_manifest = R"({
    "metadata": {"name": "notes", "namespace": "default"},
    "spec": {
        "volumes": [{"name": "data", "kube32.io/sd": {"claim": "tessera"}}],
        "containers": [{"name": "c", "image": "wasm://sd/sdnote",
                        "resources": {"requests": {"hardware.kube32.io/sd": "1"}}}]
    }
})";

void test_firewall_sd_grant() {
    hal::CapabilityFirewall& fw = hal::CapabilityFirewall::instance();
    fw.reset();
    std::string err;
    assert(fw.register_pod("default/notes", k_sd_manifest, err));
    assert(fw.check_permission("default/notes", hal::PeripheralType::SdCard, 0, hal::AccessMode::ReadWrite));
    assert(fw.sd_claim("default/notes") == "tessera");

    // the standard persistentVolumeClaim shape names the claim too
    std::string pvc = R"({"spec": {"volumes": [{"name": "data", "persistentVolumeClaim": {"claimName": "pvc-notes"}}],
        "containers": [{"name": "c", "resources": {"requests": {"hardware.kube32.io/sd": "1"}}}]}})";
    assert(fw.register_pod("default/pvc", pvc, err));
    assert(fw.sd_claim("default/pvc") == "pvc-notes");

    // request without a claim is rejected at admission
    std::string no_claim = R"({"spec": {"containers": [{"name": "c", "resources": {"requests": {"hardware.kube32.io/sd": "1"}}}]}})";
    assert(!fw.register_pod("default/noclaim", no_claim, err));
    assert(err.find("claim") != std::string::npos);

    // a claim without the resource request grants nothing
    std::string no_request = R"({"spec": {"volumes": [{"name": "data", "kube32.io/sd": {"claim": "x"}}],
        "containers": [{"name": "c"}]}})";
    assert(fw.register_pod("default/norequest", no_request, err));
    assert(!fw.check_permission("default/norequest", hal::PeripheralType::SdCard, 0, hal::AccessMode::Read));
    assert(fw.sd_claim("default/norequest").empty());

    // claim names are directory names
    std::string bad = R"({"spec": {"volumes": [{"name": "data", "kube32.io/sd": {"claim": "../escape"}}],
        "containers": [{"name": "c", "resources": {"requests": {"hardware.kube32.io/sd": "1"}}}]}})";
    assert(!fw.register_pod("default/bad", bad, err));
    assert(!hal::CapabilityFirewall::valid_sd_claim("Upper"));
    assert(!hal::CapabilityFirewall::valid_sd_claim("-lead"));
    assert(!hal::CapabilityFirewall::valid_sd_claim(""));
    assert(hal::CapabilityFirewall::valid_sd_claim("tessera-2"));

    fw.unregister_pod("default/notes");
    assert(fw.sd_claim("default/notes").empty());
    fw.reset();
    std::cout << "[PASS] test_firewall_sd_grant" << std::endl;
}

// ---------------------------------------------------------------------------
void test_pod_fs_abi() {
    mount_root();
    hal::CapabilityFirewall& fw = hal::CapabilityFirewall::instance();
    fw.reset();
    std::string err;
    assert(fw.register_pod("default/notes", k_sd_manifest, err));
    runtime::PodFs& fs = runtime::PodFs::instance();
    const std::string pod = "default/notes";

    // create, write, close, reopen, size, read to end
    int32_t fd = fs.open(pod, "boot.log", K32_FS_O_WRONLY | K32_FS_O_CREAT | K32_FS_O_TRUNC);
    assert(fd == 0);
    assert(fs.write(pod, fd, reinterpret_cast<const uint8_t*>("hello\n"), 6) == 6);
    assert(fs.read(pod, fd, nullptr, 0) == K32_ERR_BADF);   // write-only descriptor
    assert(fs.close(pod, fd) == 0);
    assert(file_exists(g_root + "/kube32/volumes/tessera/boot.log"));
    assert(read_file(g_root + "/kube32/volumes/tessera/boot.log") == "hello\n");

    fd = fs.open(pod, "boot.log", K32_FS_O_WRONLY | K32_FS_O_APPEND);
    assert(fd == 0);
    assert(fs.write(pod, fd, reinterpret_cast<const uint8_t*>("again\n"), 6) == 6);
    assert(fs.close(pod, fd) == 0);

    fd = fs.open(pod, "boot.log", K32_FS_O_RDONLY);
    assert(fd == 0);
    assert(fs.size(pod, fd) == 12);
    uint8_t buf[64];
    assert(fs.read(pod, fd, buf, sizeof(buf)) == 12);
    assert(std::memcmp(buf, "hello\nagain\n", 12) == 0);
    assert(fs.read(pod, fd, buf, sizeof(buf)) == 0);    // end of file
    assert(fs.write(pod, fd, buf, 1) == K32_ERR_BADF);  // read-only descriptor
    assert(fs.close(pod, fd) == 0);
    assert(fs.close(pod, fd) == K32_ERR_BADF);          // already closed
    assert(fs.close(pod, -1) == K32_ERR_BADF);
    assert(fs.close(pod, 9) == K32_ERR_BADF);
    assert(fs.size(pod, 2) == K32_ERR_BADF);

    // list: files, and directories with a trailing slash
    assert(mkdir((g_root + "/kube32/volumes/tessera/sub").c_str(), 0777) == 0);
    write_file(g_root + "/kube32/volumes/tessera/sub/inner.txt", "x");
    char names[256];
    int32_t n = fs.list(pod, "", names, sizeof(names));
    assert(n > 0);
    std::string listing(names, static_cast<size_t>(n));
    assert(listing.find("boot.log\n") != std::string::npos);
    assert(listing.find("sub/\n") != std::string::npos);
    n = fs.list(pod, "sub", names, sizeof(names));
    assert(std::string(names, static_cast<size_t>(n)) == "inner.txt\n");
    assert(fs.list(pod, "nope", names, sizeof(names)) == K32_ERR_NOENT);
    // a buffer too small for the first entry yields an empty listing, never a cut name
    assert(fs.list(pod, "sub", names, 4) == 0);

    // exclusive create, open of a missing file, remove
    assert(fs.open(pod, "boot.log", K32_FS_O_WRONLY | K32_FS_O_CREAT | K32_FS_O_EXCL) == K32_ERR_EXIST);
    assert(fs.open(pod, "missing.txt", K32_FS_O_RDONLY) == K32_ERR_NOENT);
    assert(fs.remove(pod, "sub") == K32_ERR_INVAL);      // not empty
    assert(fs.remove(pod, "sub/inner.txt") == 0);
    assert(fs.remove(pod, "sub") == 0);                  // empty directory
    assert(fs.remove(pod, "boot.log") == 0);
    assert(fs.remove(pod, "boot.log") == K32_ERR_NOENT);
    assert(!file_exists(g_root + "/kube32/volumes/tessera/boot.log"));

    // path rules: relative only, no "..", no empty components, bounded length
    assert(fs.open(pod, "../escape.txt", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "a/../escape.txt", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "/etc/passwd", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "a//b", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "./x", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.open(pod, "sp ace", K32_FS_O_RDONLY) == K32_ERR_INVAL);
    assert(fs.list(pod, "..", names, sizeof(names)) == K32_ERR_INVAL);
    assert(fs.remove(pod, "../../kube32") == K32_ERR_INVAL);
    std::string long_name(K32_FS_MAX_PATH + 1, 'a');
    assert(fs.open(pod, long_name, K32_FS_O_RDONLY) == K32_ERR_NAMETOOLONG);
    std::string max_name(K32_FS_MAX_PATH, 'b');
    int32_t mfd = fs.open(pod, max_name, K32_FS_O_WRONLY | K32_FS_O_CREAT);
    assert(mfd >= 0);
    assert(fs.close(pod, mfd) == 0);
    assert(fs.remove(pod, max_name) == 0);
    assert(fs.open(pod, "x", 3) == K32_ERR_INVAL);       // RDONLY|WRONLY together
    assert(fs.open(pod, "x", 0x10000) == K32_ERR_INVAL); // unknown flag
    assert(!file_exists(g_root + "/escape.txt"));

    // at most 4 descriptors per pod; release closes them all
    int32_t fds[4];
    for (int i = 0; i < 4; ++i) {
        fds[i] = fs.open(pod, "f" + std::to_string(i) + ".txt", K32_FS_O_WRONLY | K32_FS_O_CREAT);
        assert(fds[i] == i);
    }
    assert(fs.open(pod, "f4.txt", K32_FS_O_WRONLY | K32_FS_O_CREAT) == K32_ERR_MFILE);
    assert(fs.open_count(pod) == 4);
    assert(fs.close(pod, fds[1]) == 0);
    assert(fs.open(pod, "f4.txt", K32_FS_O_WRONLY | K32_FS_O_CREAT) == 1);   // slot reused
    fs.release_pod(pod);
    assert(fs.open_count(pod) == 0);
    assert(fs.read(pod, 0, buf, 1) == K32_ERR_BADF);
    for (int i = 0; i < 5; ++i) fs.remove(pod, "f" + std::to_string(i) + ".txt");

    // no grant: every call is a permission error and a logged violation
    assert(fs.open("default/stranger", "boot.log", K32_FS_O_RDONLY) == K32_ERR_PERM);
    assert(fs.list("default/stranger", "", names, sizeof(names)) == K32_ERR_PERM);
    assert(fs.remove("default/stranger", "boot.log") == K32_ERR_PERM);
    assert(fs.read("default/stranger", 0, buf, 1) == K32_ERR_PERM);
    assert(fs.write("default/stranger", 0, buf, 1) == K32_ERR_PERM);
    assert(fs.close("default/stranger", 0) == K32_ERR_PERM);
    assert(fs.size("default/stranger", 0) == K32_ERR_PERM);
    std::string no_request = R"({"spec": {"volumes": [{"name": "data", "kube32.io/sd": {"claim": "x"}}], "containers": [{"name": "c"}]}})";
    assert(fw.register_pod("default/norequest", no_request, err));
    assert(fs.open("default/norequest", "boot.log", K32_FS_O_RDONLY) == K32_ERR_PERM);
    const hal::PodSecurityRecord* rec = fw.get_pod_security_record("default/norequest");
    assert(rec && rec->violation_count == 1);

    // no card: the grant is there but there is nothing to open
    hal::sd_unmount();
    assert(fs.open(pod, "boot.log", K32_FS_O_RDONLY) == K32_ERR_NODEV);
    assert(fs.list(pod, "", names, sizeof(names)) == K32_ERR_NODEV);
    assert(fs.volume_root(pod).empty());
    mount_root();
    assert(fs.volume_root(pod) == g_root + "/kube32/volumes/tessera");

    fw.reset();
    std::cout << "[PASS] test_pod_fs_abi" << std::endl;
}

// ---------------------------------------------------------------------------
void test_sd_image_refs() {
    std::string name, tag;
    assert(runtime::parse_sd_image_ref("wasm://sd/echo", name, tag) && name == "echo" && tag.empty());
    assert(runtime::parse_sd_image_ref("wasm://sd/echo:v2", name, tag) && name == "echo" && tag == "v2");
    assert(runtime::parse_sd_image_ref("sd/tessera-core:1.0", name, tag) && name == "tessera-core" && tag == "1.0");
    assert(!runtime::parse_sd_image_ref("wasm://embedded/echo", name, tag));
    assert(!runtime::parse_sd_image_ref("wasm://sd/../echo", name, tag));
    assert(!runtime::parse_sd_image_ref("wasm://sd/a/b", name, tag));
    assert(!runtime::parse_sd_image_ref("wasm://sd/", name, tag));
    assert(!runtime::parse_sd_image_ref("wasm://sd/echo:", name, tag));
    assert(!runtime::parse_sd_image_ref("wasm://sd/.hidden", name, tag));
    assert(runtime::sd_image_relative_path("echo", "") == "kube32/images/echo.wasm");
    assert(runtime::sd_image_relative_path("echo", "v2") == "kube32/images/echo-v2.wasm");
    std::cout << "[PASS] test_sd_image_refs" << std::endl;
}

// ---------------------------------------------------------------------------
class RecordingClient : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::vector<std::string> events;
    std::string last_status;
    bool register_node(const std::string&, bool& conflict) override { conflict = false; return true; }
    bool patch_node_status(const std::string&, const std::string&) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& patch) override { last_status = patch; return true; }
    bool create_event(const std::string&, const std::string& ev) override { events.push_back(ev); return true; }
    bool has_event(const char* reason, const char* text) const {
        for (const auto& e : events) {
            if (e.find(std::string("\"reason\":\"") + reason + "\"") != std::string::npos && e.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

static models::KubePod make_pod(const std::string& name) {
    models::KubePod pod;
    pod.name = name;
    pod.namespace_name = "default";
    pod.node_name = "test-node-01";
    pod.status = "Pending";
    return pod;
}

static std::string plain_manifest(const std::string& name, const std::string& image) {
    return "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"nodeName\":\"test-node-01\",\"containers\":[{\"name\":\"c\",\"image\":\"" + image + "\"}]}}";
}

void test_kubelet_pulls_image_from_sd() {
    mount_root();
    hal::CapabilityFirewall::instance().reset();
    const runtime::EmbeddedImage* echo = embedded_image("echo");
    assert(echo && "images/echo.wasm must be embedded in the host build");
    mkdir((g_root + "/kube32").c_str(), 0777);
    mkdir((g_root + "/kube32/images").c_str(), 0777);
    write_file(g_root + "/kube32/images/echo.wasm", std::string(reinterpret_cast<const char*>(echo->data), echo->len));
    write_file(g_root + "/kube32/images/echo-v2.wasm", std::string(reinterpret_cast<const char*>(echo->data), echo->len));

    kubelet::KubeletConfig cfg;
    cfg.node_name = "test-node-01";
    auto rt = runtime::create_wasm_runtime();
    auto client = std::make_unique<RecordingClient>();
    RecordingClient* c = client.get();
    kubelet::Kubelet kl(cfg, *rt, std::move(client));

    // 1. wasm://sd/echo comes from the card even though "echo" is also embedded
    c->pods.emplace_back(make_pod("from-sd"), plain_manifest("from-sd", "wasm://sd/echo"));
    kl.sync_assigned_pods();
    kubelet::LocalPodRecord rec;
    assert(kl.get_pod_record("default/from-sd", rec));
    assert(rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(c->has_event("Pulling", "sd card"));
    assert(c->has_event("Pulled", "sd card"));
    assert(c->has_event("Pulled", (std::to_string(echo->len) + " bytes").c_str()));
    assert(c->has_event("Pulled", "/kube32/images/echo.wasm"));
    assert(c->last_status.find("\"phase\":\"Running\"") != std::string::npos);
    assert(runtime::AppPodHost::instance().pod_handles_requests("default/from-sd"));

    // 2. a tag selects <name>-<tag>.wasm
    c->pods.emplace_back(make_pod("tagged"), plain_manifest("tagged", "wasm://sd/echo:v2"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/tagged", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(c->has_event("Pulled", "echo-v2.wasm"));

    // 3. a missing file is ErrImagePull with the path in the message
    c->events.clear();
    c->pods.emplace_back(make_pod("absent"), plain_manifest("absent", "wasm://sd/absent"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/absent", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("absent.wasm not found on the sd card") != std::string::npos);
    assert(c->last_status.find("ErrImagePull") != std::string::npos);
    assert(c->has_event("Failed", "not found on the sd card"));

    // 4. a module over the board's limit is refused before any allocation
    {
        std::ofstream big(g_root + "/kube32/images/huge.wasm", std::ios::binary);
        big.seekp(2 * 1024 * 1024);
        big.put(0);
    }
    c->events.clear();
    c->pods.emplace_back(make_pod("huge"), plain_manifest("huge", "wasm://sd/huge"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/huge", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("over the") != std::string::npos);
    assert(rec.last_error_message.find("KiB limit") != std::string::npos);
    assert(c->has_event("Failed", "over the"));

    // 5. without a card the pull fails with that reason
    hal::sd_unmount();
    c->events.clear();
    c->pods.emplace_back(make_pod("nocard"), plain_manifest("nocard", "wasm://sd/echo"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/nocard", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("no sd card mounted") != std::string::npos);
    mount_root();

    // 6. the embedded and inline paths still work
    c->pods.emplace_back(make_pod("embedded"), plain_manifest("embedded", "wasm://embedded/echo"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/embedded", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(c->has_event("Pulled", "embedded registry"));

    c->pods.clear();
    kl.sync_assigned_pods();
    assert(kl.get_active_pod_keys().empty());
    hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_kubelet_pulls_image_from_sd" << std::endl;
}

// ---------------------------------------------------------------------------
class CollectingSink : public runtime::AppResponseSink {
public:
    int status{0};
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool ended{false};
    void begin(int s, std::string_view ct) override { status = s; content_type = std::string(ct); }
    void header(std::string_view n, std::string_view v) override { headers.emplace_back(std::string(n), std::string(v)); }
    bool write(const uint8_t* d, size_t l) override { body.append(reinterpret_cast<const char*>(d), l); return true; }
    void end() override { ended = true; }
};

static int32_t call_pod(const std::string& pod, const char* method, const char* path, CollectingSink& sink) {
    runtime::AppRequest req;
    req.method = method;
    req.path_and_query = path;
    return runtime::AppPodHost::instance().handle(pod, req, sink);
}

static std::string sd_manifest(const std::string& name, bool with_request) {
    return "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"nodeName\":\"test-node-01\","
           "\"volumes\":[{\"name\":\"data\",\"persistentVolumeClaim\":{\"claimName\":\"notes\"}}],"
           "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://sd/sdnote\"" +
           std::string(with_request ? ",\"resources\":{\"requests\":{\"memory\":\"64Ki\",\"hardware.kube32.io/sd\":\"1\"}}" : "") + "}]}}";
}

void test_sdnote_guest_end_to_end() {
    mount_root();
    hal::CapabilityFirewall::instance().reset();
    const std::string src = std::string(KUBE32_SOURCE_DIR) + "/images/sd/sdnote.wasm";
    std::string module = read_file(src);
    assert(!module.empty() && "build the guest first: make -C apps/sdnote");
    mkdir((g_root + "/kube32").c_str(), 0777);
    mkdir((g_root + "/kube32/images").c_str(), 0777);
    write_file(g_root + "/kube32/images/sdnote.wasm", module);

    kubelet::KubeletConfig cfg;
    cfg.node_name = "test-node-01";
    auto rt = runtime::create_wasm_runtime();
    auto client = std::make_unique<RecordingClient>();
    RecordingClient* c = client.get();
    kubelet::Kubelet kl(cfg, *rt, std::move(client));
    const std::string pod = "default/sdnote";
    const std::string log_path = g_root + "/kube32/volumes/notes/boot.log";

    // k32_init appended one line to boot.log in the pod's volume
    c->pods.emplace_back(make_pod("sdnote"), sd_manifest("sdnote", true));
    kl.sync_assigned_pods();
    kubelet::LocalPodRecord rec;
    assert(kl.get_pod_record(pod, rec));
    assert(rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(file_exists(log_path));
    std::string log1 = read_file(log_path);
    assert(log1.rfind("boot at uptime_ms=", 0) == 0);
    assert(log1.back() == '\n');
    assert(runtime::PodFs::instance().open_count(pod) == 0);   // the guest closed its descriptor

    // GET /log streams the file back through the response ABI
    CollectingSink sink;
    assert(call_pod(pod, "GET", "/log", sink) == 200);
    assert(sink.content_type == "text/plain");
    assert(sink.body == log1);
    bool size_hdr = false;
    for (auto& h : sink.headers) if (h.first == "X-File-Size" && h.second == std::to_string(log1.size())) size_hdr = true;
    assert(size_hdr);

    CollectingSink ls;
    assert(call_pod(pod, "GET", "/ls", ls) == 200);
    assert(ls.body == "boot.log\n");

    CollectingSink other;
    assert(call_pod(pod, "GET", "/nope", other) == 404);

    // a restart (same module, fresh instance) appends a second line
    c->pods.clear();
    kl.sync_assigned_pods();
    assert(!runtime::AppPodHost::instance().has_pod(pod));
    c->pods.emplace_back(make_pod("sdnote"), sd_manifest("sdnote", true));
    kl.sync_assigned_pods();
    std::string log2 = read_file(log_path);
    assert(log2.size() > log1.size() && log2.rfind(log1, 0) == 0);

    CollectingSink del;
    assert(call_pod(pod, "DELETE", "/log", del) == 200);
    assert(!file_exists(log_path));
    CollectingSink gone;
    assert(call_pod(pod, "GET", "/log", gone) == 404);
    assert(gone.body.find("rc=-2") != std::string::npos);   // K32_ERR_NOENT reaches the guest
    c->pods.clear();
    kl.sync_assigned_pods();

    // without hardware.kube32.io/sd the same guest is denied inside k32_init
    c->events.clear();
    c->pods.emplace_back(make_pod("denied"), sd_manifest("denied", false));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/denied", rec));
    assert(rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("k32_init returned " + std::to_string(K32_ERR_PERM)) != std::string::npos);
    assert(!file_exists(log_path));
    c->pods.clear();
    kl.sync_assigned_pods();

    // with the grant but no card the pod is refused at admission with the reason
    hal::sd_unmount();
    c->events.clear();
    c->pods.emplace_back(make_pod("nocard"), sd_manifest("nocard", true));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/nocard", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("no sd card is mounted") != std::string::npos);
    assert(c->last_status.find("CapabilityAdmissionFailed") != std::string::npos);
    mount_root();
    c->pods.clear();
    kl.sync_assigned_pods();
    hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_sdnote_guest_end_to_end" << std::endl;
}

int main() {
    g_root = make_temp_root();
    // The embedded registry as the firmware and simulator populate it at boot
    {
        size_t n = 0;
        const auto* imgs = embedded::images(n);
        runtime::ImageRegistry::instance().add_all(imgs, n);
    }
    std::cout << "=== microSD card tests (KUBE32_SD_ROOT=" << g_root << ") ===" << std::endl;
    test_hal_without_env();
    test_hal_with_env();
    test_firewall_sd_grant();
    test_pod_fs_abi();
    test_sd_image_refs();
    test_kubelet_pulls_image_from_sd();
    test_sdnote_guest_end_to_end();
    std::cout << "All microSD card tests passed" << std::endl;
    return 0;
}
