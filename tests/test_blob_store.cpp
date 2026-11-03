// Blobs (storage/blob_store.hpp, runtime/pod_blobs.hpp, app/node_blobs_api.hpp):
// the flash store's directory, extents and crash safety, the passive app slot
// shared with the firmware, the card store, a pod's handles and grants, and
// the kubelet endpoint's /blobs.

#include "pod_test_util.hpp"
#include "app/pod_server.hpp"
#include "app/node_blobs_api.hpp"
#include "hal/sd_card.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_blobs.hpp"
#include "storage/blob_store.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace kube32;
using storage::BlobRegion;
using storage::FlashBlobStore;
using k32test::http;

// A region another region's bytes stand behind, so a test can open a store
// over the same flash again (a reboot) or cut its writes short (a power cut)
class SharedRegion : public BlobRegion {
public:
    SharedRegion(BlobRegion& base, bool mapped = true) : base_(base), mapped_(mapped) {}
    size_t size() const override { return base_.size(); }
    bool read(size_t o, void* d, size_t n) override { return base_.read(o, d, n); }
    bool write(size_t o, const void* s, size_t n) override {
        if (budget_ >= 0 && static_cast<long long>(n) > budget_) {
            base_.write(o, s, static_cast<size_t>(budget_));
            budget_ = 0;
            return false;
        }
        if (budget_ >= 0) budget_ -= static_cast<long long>(n);
        return base_.write(o, s, n);
    }
    bool erase(size_t o, size_t n) override { return base_.erase(o, n); }
    const uint8_t* data() const override { return mapped_ ? base_.data() : nullptr; }
    std::string name() const override { return "test"; }
    long long budget_{-1};   // bytes left before writes stop, -1 unlimited

private:
    BlobRegion& base_;
    bool mapped_;
};

static std::string pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 2654435761u + seed) >> 13);
    return s;
}

static int32_t put(storage::BlobStore& store, const std::string& name, const std::string& bytes, std::string* why_out = nullptr) {
    int32_t err = 0;
    std::string why;
    auto w = store.create(name, bytes.size(), err, why);
    if (!w) {
        if (why_out) *why_out = why;
        return err;
    }
    for (size_t off = 0; off < bytes.size(); off += 1000) {
        err = w->write(bytes.data() + off, std::min<size_t>(1000, bytes.size() - off), why);
        if (err) return err;
    }
    err = w->commit(why);
    if (why_out) *why_out = why;
    return err;
}

static std::string read_all(storage::BlobReader& r) {
    std::string out(static_cast<size_t>(r.size()), '\0');
    int32_t n = r.read(0, out.data(), static_cast<uint32_t>(out.size()));
    assert(n == static_cast<int32_t>(out.size()));
    return out;
}

static void test_flash_store() {
    auto flash = storage::create_ram_blob_region(1024 * 1024);
    const std::string a = pattern(100000, 1), b = pattern(70000, 2);
    {
        FlashBlobStore store(std::make_unique<SharedRegion>(*flash), false);
        assert(store.available() && store.list().empty());
        assert(store.capacity_bytes() == 1024 * 1024 - storage::kBlobDataStart);
        assert(put(store, "weights.bin", a) == K32_OK);
        assert(put(store, "tok", b) == K32_OK);
        auto l = store.list();
        assert(l.size() == 2 && l[0].name == "weights.bin" && l[0].size == a.size() && l[0].offset == storage::kBlobDataStart);
        assert(l[1].offset == storage::kBlobDataStart + 2 * storage::kBlobPage);   // 100000 bytes take two pages
        int32_t err = 0;
        auto r = store.open("weights.bin", err);
        assert(r && err == K32_OK && r->size() == a.size() && r->view());
        assert(std::memcmp(r->view(), a.data(), a.size()) == 0);
        char buf[16];
        assert(r->read(a.size() - 4, buf, sizeof buf) == 4 && std::memcmp(buf, a.data() + a.size() - 4, 4) == 0);
        assert(r->read(a.size(), buf, sizeof buf) == 0);
        assert(!store.open("nope", err) && err == K32_ERR_NOENT);
        assert(!store.open("../x", err) && err == K32_ERR_INVAL);

        // Replaced while open: the reader keeps the old bytes, the new blob
        // lands elsewhere, and the old extent is free once the reader is gone
        const std::string a2 = pattern(100000, 3);
        assert(put(store, "weights.bin", a2) == K32_OK);
        assert(std::memcmp(r->view(), a.data(), a.size()) == 0);
        auto r2 = store.open("weights.bin", err);
        assert(r2 && std::memcmp(r2->view(), a2.data(), a2.size()) == 0 && r2->view() != r->view());
        assert(store.open_readers() == 2);
        r.reset();
        assert(store.open_readers() == 1);
        assert(put(store, "small", "x") == K32_OK);
        for (const auto& e : store.list()) {
            if (e.name == "small") assert(e.offset == storage::kBlobDataStart);   // the old extent, free again
        }
        std::string why;
        assert(store.remove("small", why) == K32_OK);
        assert(store.remove("small", why) == K32_ERR_NOENT);
    }
    // After a restart the directory is read back
    {
        FlashBlobStore store(std::make_unique<SharedRegion>(*flash), false);
        auto l = store.list();
        assert(l.size() == 2);
        int32_t err = 0;
        auto r = store.open("tok", err);
        assert(r && read_all(*r) == b);
        const uint32_t seq = store.directory_sequence();
        // A write cut short by a power cut leaves the directory as it was
        auto cut = std::make_unique<SharedRegion>(*flash);
        SharedRegion* cutp = cut.get();
        FlashBlobStore cutting(std::move(cut), false);
        cutp->budget_ = 5000;
        assert(put(cutting, "new", pattern(20000, 4)) != K32_OK);
        FlashBlobStore after(std::make_unique<SharedRegion>(*flash), false);
        assert(after.list().size() == 2 && after.directory_sequence() == seq);
    }
    // A torn directory copy: the other copy, one change older, is the directory
    {
        FlashBlobStore store(std::make_unique<SharedRegion>(*flash), false);
        const uint32_t seq = store.directory_sequence();
        assert(put(store, "c", "ccc") == K32_OK && store.directory_sequence() == seq + 1);
        // Clear the CRC of the copy just written (NOR flash only clears bits)
        uint8_t zero[4] = {0, 0, 0, 0};
        for (size_t base : {size_t(0), storage::kBlobDirSector}) {
            uint8_t head[12];
            flash->read(base, head, sizeof head);
            const uint32_t s = head[8] | (head[9] << 8) | (head[10] << 16) | (uint32_t(head[11]) << 24);
            if (s == seq + 1) flash->write(base + 16, zero, sizeof zero);
        }
        FlashBlobStore after(std::make_unique<SharedRegion>(*flash), false);
        assert(after.directory_sequence() == seq && after.list().size() == 2);
    }
    // Full: a blob larger than the free run is refused with the reason
    {
        FlashBlobStore store(std::make_unique<SharedRegion>(*flash), false);
        std::string why;
        assert(put(store, "huge", pattern(store.capacity_bytes() - 100000, 5), &why) == K32_ERR_NOSPC);
        assert(why.find("no free run") != std::string::npos);
        assert(put(store, "", "x") == K32_ERR_INVAL && put(store, "a.part", "x") == K32_ERR_INVAL);
    }
    std::cout << "[PASS] flash store: directory, replace while open, restart, power cut, torn copy, full" << std::endl;
}

static void test_firmware_slot() {
    auto flash = storage::create_ram_blob_region(512 * 1024);
    std::memset(const_cast<uint8_t*>(flash->data()), 0xE9, 4096);   // an app image's first sector
    FlashBlobStore store(std::make_unique<SharedRegion>(*flash), true);
    std::string firmware = "a firmware image is staged in ota_1";
    store.set_firmware_claim([&] { return firmware; });
    std::string why;
    assert(!store.available(&why) && why == firmware);
    assert(put(store, "w", "abc", &why) == K32_ERR_BUSY && why == firmware);
    firmware.clear();
    assert(store.available());
    assert(put(store, "w", "abc") == K32_OK);
    int32_t err = 0;
    auto r = store.open("w", err);
    assert(r);
    // An image may not go over a blob a pod reads
    assert(!store.yield_to_firmware(why) && why.find("blob reader") != std::string::npos);
    r.reset();
    assert(store.yield_to_firmware(why));
    assert(!store.available(&why) && store.list().empty() && !store.open("w", err) && err == K32_ERR_NOENT);
    // The firmware is done with the slot (an upload that failed): blobs take it again
    assert(put(store, "w2", "xyz") == K32_OK && store.list().size() == 1);
    std::cout << "[PASS] passive app slot: the firmware's claim, yield refused while read, blobs again after" << std::endl;
}

static void test_card_store_and_pod() {
    const std::string root = "/tmp/kube32_test_blobs_card";
    std::system(("rm -rf " + root + " && mkdir -p " + root).c_str());
    setenv("KUBE32_SD_ROOT", root.c_str(), 1);
    hal::sd_unmount();
    assert(hal::sd_mount());
    storage::CardBlobStore card;
    assert(card.available() && card.list().empty());
    const std::string t = pattern(5000, 6);
    assert(put(card, "tokenizer", t) == K32_OK);
    auto l = card.list();
    assert(l.size() == 1 && l[0].name == "tokenizer" && l[0].size == t.size());
    int32_t err = 0;
    auto r = card.open("tokenizer", err);
    assert(r && r->view() && read_all(*r) == t);   // mapped on the host
    std::string why;
    assert(put(card, "tokenizer", "new", &why) == K32_ERR_BUSY && why.find("open") != std::string::npos);
    assert(card.remove("tokenizer", why) == K32_ERR_BUSY);
    r.reset();

    // A pod's handles: the flash store first, then the card
    auto region = storage::create_ram_blob_region(512 * 1024);
    FlashBlobStore flash(std::make_unique<SharedRegion>(*region), false);
    const std::string w = pattern(70000, 7);
    assert(put(flash, "w", w) == K32_OK);
    storage::set_node_blob_stores(&flash, &card);
    auto& pb = runtime::PodBlobs::instance();
    const std::string pod = "default/reader";
    assert(pb.open(pod, "w") == K32_ERR_PERM);   // no kube32.io/blobs
    pb.set_allowed(pod, runtime::PodBlobs::parse_annotation(" w, tokenizer\tmissing "));
    assert(pb.open(pod, "other") == K32_ERR_PERM);
    assert(pb.open(pod, "missing") == K32_ERR_NOENT);
    const int32_t hw = pb.open(pod, "w"), ht = pb.open(pod, "tokenizer");
    assert(hw == 0 && ht == 1);
    assert(pb.size(pod, hw) == static_cast<int32_t>(w.size()) && pb.size(pod, ht) == static_cast<int32_t>(t.size()));
    assert(pb.mapped(pod, hw) == 1);
    char buf[64];
    assert(pb.read(pod, ht, 100, reinterpret_cast<uint8_t*>(buf), sizeof buf) == 64 && std::memcmp(buf, t.data() + 100, 64) == 0);
    assert(pb.read(pod, 3, 0, reinterpret_cast<uint8_t*>(buf), 1) == K32_ERR_BADF);
    int32_t verr = 0;
    assert(pb.view(pod, hw, 1000, 69000, verr) == flash.list()[0].offset + region->data() + 1000);
    assert(!pb.view(pod, hw, 1000, 69001, verr) && verr == K32_ERR_FAULT);
    assert(!pb.view(pod, 2, 0, 1, verr) && verr == K32_ERR_BADF);
    assert(pb.open(pod, "w") == 2 && pb.open(pod, "w") == 3 && pb.open(pod, "w") == K32_ERR_MFILE);
    assert(pb.open_blobs(pod).size() == 4);
    pb.release_pod(pod);   // a container restart: handles closed, the grant stays
    assert(pb.open_blobs(pod).empty() && flash.open_readers() == 0);
    assert(pb.open(pod, "w") == 0 && pb.close(pod, 0) == K32_OK && pb.close(pod, 0) == K32_ERR_BADF);
    pb.forget_pod(pod);
    assert(pb.open(pod, "w") == K32_ERR_PERM);
    storage::set_node_blob_stores(nullptr, nullptr);
    std::cout << "[PASS] card store and a pod's blob handles" << std::endl;
}

static void test_http() {
    osal::Socket::init_networking();
    auto region = storage::create_ram_blob_region(1024 * 1024);
    FlashBlobStore flash(std::make_unique<SharedRegion>(*region), false);
    storage::CardBlobStore card;
    storage::set_node_blob_stores(&flash, &card);
    const std::string token = "node-token-0123456789";
    const std::string auth = "Authorization: Bearer " + token + "\r\n";
    app::PodServerOptions po;
    po.port = 18695;
    po.workers = 1;
    po.rx_buffer = 2048;
    po.worker_stack = 262144;
    po.token = token;
    app::PodServer node(po);
    assert(node.start());

    const std::string data = pattern(200 * 1024, 8);
    k32test::HttpReply r = http(po.port, "PUT", "/blobs/model", auth, data);
    std::cout << "[put] " << r.status << " " << r.body << std::endl;
    assert(r.status == 201 && r.body.find("\"medium\":\"flash\"") != std::string::npos);
    r = http(po.port, "PUT", "/blobs/model", auth, data);
    assert(r.status == 200);
    r = http(po.port, "PUT", "/blobs/vocab?medium=sd", auth, "abc");
    assert(r.status == 201 && r.body.find("\"medium\":\"sd\"") != std::string::npos);
    r = http(po.port, "GET", "/blobs/model", auth);
    assert(r.status == 200 && r.body == data);
    r = http(po.port, "GET", "/blobs", auth);
    std::cout << "[list] " << r.body << std::endl;
    assert(r.status == 200 && r.body.find("\"name\":\"model\",\"size\":204800,\"crc32\":\"0x") != std::string::npos &&
           r.body.find("\"name\":\"vocab\",\"size\":3}") != std::string::npos);
    r = http(po.port, "GET", "/blobs/vocab", auth);
    assert(r.status == 200 && r.body == "abc");
    r = http(po.port, "PUT", "/blobs/bad%20name", auth, "x");
    assert(r.status == 400);
    r = http(po.port, "PUT", "/blobs/x?medium=tape", auth, "x");
    assert(r.status == 400);
    r = http(po.port, "GET", "/blobs/model");
    assert(r.status == 401);
    r = http(po.port, "DELETE", "/blobs/model", auth);
    assert(r.status == 200);
    r = http(po.port, "DELETE", "/blobs/model", auth);
    assert(r.status == 404);
    r = http(po.port, "DELETE", "/blobs/vocab", auth);
    assert(r.status == 200);
    node.stop();
    storage::set_node_blob_stores(nullptr, nullptr);
    std::cout << "[PASS] /blobs on the kubelet endpoint" << std::endl;
}

static void test_file_region() {
    const std::string path = "/tmp/kube32_test_blobs.img";
    std::remove(path.c_str());
    {
        FlashBlobStore store(storage::create_file_blob_region(path, 512 * 1024), false);
        assert(put(store, "persisted", "hello") == K32_OK);
    }
    FlashBlobStore again(storage::create_file_blob_region(path, 512 * 1024), false);
    int32_t err = 0;
    auto r = again.open("persisted", err);
    assert(r && read_all(*r) == "hello");
    assert(!storage::create_file_blob_region(path, 100));   // smaller than a directory and one page
    std::cout << "[PASS] file region (kube32-sim's flash)" << std::endl;
}

int main() {
    std::cout << "=== blobs ===" << std::endl;
    test_flash_store();
    test_firmware_slot();
    test_card_store_and_pod();
    test_http();
    test_file_region();
    std::cout << "All blob tests passed" << std::endl;
    return 0;
}
