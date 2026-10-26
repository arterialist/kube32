#include "storage/radix_tree.hpp"
#include "storage/littlefs_journal.hpp"
#include "storage/store.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <cstdlib>
#include <unistd.h>

using namespace kube32::storage;
using namespace kube32::osal;

void test_radix_tree_edge_splitting_and_compaction() {
    std::cout << "[RUN] test_radix_tree_edge_splitting_and_compaction" << std::endl;
    RadixTree tree;

    assert(tree.put("/registry/pods/default/nginx", "{\"name\":\"nginx\"}", 1, false, false));
    assert(tree.put("/registry/pods/default/redis", "{\"name\":\"redis\"}", 2, false, false));
    assert(tree.put("/registry/pods/kube-system/coredns", "{\"name\":\"coredns\"}", 3, false, false));
    assert(tree.put("/registry/nodes/esp32-node1", "{\"name\":\"esp32-node1\"}", 4, false, false));

    std::string val;
    uint64_t ver = 0;
    assert(tree.get("/registry/pods/default/nginx", val, &ver));
    assert(val == "{\"name\":\"nginx\"}");
    assert(ver == 1);

    assert(tree.get("/registry/pods/default/redis", val, &ver));
    assert(val == "{\"name\":\"redis\"}");
    assert(ver == 2);

    // Test prefix listing
    std::vector<std::string> pod_values;
    std::vector<std::string> pod_keys;
    tree.list("/registry/pods/", pod_values, &pod_keys);
    assert(pod_values.size() == 3);
    assert(pod_keys.size() == 3);

    std::vector<std::string> default_pods;
    tree.list("/registry/pods/default/", default_pods);
    assert(default_pods.size() == 2);

    // Test deletion and upward compaction
    size_t nodes_before = tree.count_nodes();
    assert(tree.remove("/registry/pods/default/redis"));
    assert(!tree.get("/registry/pods/default/redis", val));
    size_t nodes_after = tree.count_nodes();
    assert(nodes_after < nodes_before); // Confirms edge compaction!

    // Nginx should still be present
    assert(tree.get("/registry/pods/default/nginx", val));
    assert(val == "{\"name\":\"nginx\"}");

    std::cout << "[PASS] test_radix_tree_edge_splitting_and_compaction" << std::endl;
}

void test_littlefs_journal_atomic_and_recovery() {
    std::cout << "[RUN] test_littlefs_journal_atomic_and_recovery" << std::endl;
    std::string test_dir = "/tmp/kube32_test_storage_" + std::to_string(Thread::get_time_us());
    LittleFSJournal journal(test_dir);

    // Atomic write
    std::string payload = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"demo\",\"resourceVersion\":\"42\"}}";
    assert(journal.atomic_write("/registry/pods/default/demo", payload));

    // Inject simulated interrupted write (orphan .tmp file)
    std::string tmp_file = test_dir + "/registry/pods/default/crash.json.tmp";
    FILE* fp = fopen(tmp_file.c_str(), "w");
    assert(fp != nullptr);
    fputs("{\"corrupted_json\":", fp);
    fclose(fp);

    // Verify recovery sweeps orphan .tmp and recovers valid .json
    std::vector<std::pair<std::string, std::string>> recovered;
    journal.scan_and_recover(recovered);

    assert(recovered.size() == 1);
    assert(recovered[0].first == "/registry/pods/default/demo");
    assert(recovered[0].second == payload);

    // Verify crash.json.tmp is deleted
    assert(access(tmp_file.c_str(), F_OK) != 0);

    // Cleanup
    journal.remove_file("/registry/pods/default/demo");
    std::cout << "[PASS] test_littlefs_journal_atomic_and_recovery" << std::endl;
}

void test_store_crud_and_occ() {
    std::cout << "[RUN] test_store_crud_and_occ" << std::endl;
    std::string test_dir = "/tmp/kube32_store_occ_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    opts.enable_flash_persistence = true;
    auto store = create_radix_store(opts);

    // Put
    Status s1 = store->put("/registry/pods/default/testpod", "{\"metadata\":{\"name\":\"testpod\"}}", false);
    assert(s1.ok());

    // Put again without overwrite should return AlreadyExists
    Status s2 = store->put("/registry/pods/default/testpod", "{\"metadata\":{\"name\":\"testpod2\"}}", false);
    assert(s2.code == StatusCode::AlreadyExists);

    // Get extended
    std::string out_json;
    uint64_t ver1 = 0;
    Status s3 = store->get_extended("/registry/pods/default/testpod", out_json, &ver1);
    assert(s3.ok());
    assert(ver1 > 0);

    // Update with correct expected_version
    uint64_t ver2 = 0;
    Status s4 = store->put_extended("/registry/pods/default/testpod", 
                                    "{\"metadata\":{\"name\":\"testpod_v2\"}}", 
                                    true, true, ver1, &ver2);
    assert(s4.ok());
    assert(ver2 > ver1);

    // Update with outdated expected_version should return VersionConflict
    Status s5 = store->put_extended("/registry/pods/default/testpod", 
                                    "{\"metadata\":{\"name\":\"testpod_v3\"}}", 
                                    true, true, ver1, nullptr);
    assert(s5.code == StatusCode::VersionConflict);

    // Delete
    Status s6 = store->remove("/registry/pods/default/testpod");
    assert(s6.ok());
    Status s7 = store->get("/registry/pods/default/testpod", out_json);
    assert(s7.code == StatusCode::NotFound);

    std::cout << "[PASS] test_store_crud_and_occ" << std::endl;
}

void test_store_selective_persistence_and_recovery() {
    std::cout << "[RUN] test_store_selective_persistence_and_recovery" << std::endl;
    std::string test_dir = "/tmp/kube32_store_rec_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    opts.enable_flash_persistence = true;

    {
        auto store = create_radix_store(opts);
        // Ephemeral lease: should NOT be written to flash
        Status s_lease = store->put("/registry/leases/kube-node-lease/node1", 
                                    "{\"holder\":\"node1\",\"resourceVersion\":\"5\"}", true);
        assert(s_lease.ok());

        // Persistent pod spec: should be written to flash
        Status s_pod = store->put("/registry/pods/default/web", 
                                  "{\"name\":\"web\",\"resourceVersion\":\"10\"}", true);
        assert(s_pod.ok());

        // Verify file existence on disk
        std::string lease_file = test_dir + "/registry/leases/kube-node-lease/node1.json";
        std::string pod_file = test_dir + "/registry/pods/default/web.json";
        assert(access(lease_file.c_str(), F_OK) != 0); // Must NOT exist
        assert(access(pod_file.c_str(), F_OK) == 0);   // Must exist
    }

    // Now instantiate new store to test reboot recovery
    {
        auto store_reboot = create_radix_store(opts);
        Status rec_status = store_reboot->recover();
        assert(rec_status.ok());

        std::string json;
        // Pod was recovered from flash
        assert(store_reboot->get("/registry/pods/default/web", json).ok());
        // Ephemeral lease was not persisted and is absent after reboot
        assert(store_reboot->get("/registry/leases/kube-node-lease/node1", json).code == StatusCode::NotFound);
        // Current version is greater than 10
        assert(store_reboot->current_version() > 10);
    }

    std::cout << "[PASS] test_store_selective_persistence_and_recovery" << std::endl;
}

void test_store_watch_multiplexer() {
    std::cout << "[RUN] test_store_watch_multiplexer" << std::endl;
    std::string test_dir = "/tmp/kube32_store_watch_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    auto store = create_radix_store(opts);

    std::atomic<int> added_count{0};
    std::atomic<int> mod_count{0};
    std::atomic<int> del_count{0};

    uint32_t wid = store->watch("/registry/pods/", [&](const WatchEvent& ev) {
        if (ev.type == WatchEventType::Added) added_count++;
        else if (ev.type == WatchEventType::Modified) mod_count++;
        else if (ev.type == WatchEventType::Deleted) del_count++;
    });

    store->put("/registry/pods/default/p1", "{}", false); // Added
    store->put("/registry/pods/default/p1", "{\"v\":2}", true); // Modified
    store->remove("/registry/pods/default/p1"); // Deleted

    assert(added_count.load() == 1);
    assert(mod_count.load() == 1);
    assert(del_count.load() == 1);

    store->unwatch(wid);
    store->put("/registry/pods/default/p2", "{}", false);
    assert(added_count.load() == 1); // No new trigger after unwatch

    std::cout << "[PASS] test_store_watch_multiplexer" << std::endl;
}

void test_store_concurrency() {
    std::cout << "[RUN] test_store_concurrency" << std::endl;
    std::string test_dir = "/tmp/kube32_store_conc_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    opts.enable_flash_persistence = false; // test in-memory concurrency
    auto store = create_radix_store(opts);

    std::atomic<bool> start_flag{false};
    constexpr int NUM_READERS = 4;
    constexpr int ITERATIONS = 500;
    std::vector<Thread> readers(NUM_READERS);

    struct ReaderContext {
        Store* s;
        std::atomic<bool>* flag;
    };
    std::vector<ReaderContext> ctxs(NUM_READERS);

    for (int r = 0; r < NUM_READERS; ++r) {
        ctxs[r] = {store.get(), &start_flag};
        readers[r].start([](void* arg) {
            auto* c = static_cast<ReaderContext*>(arg);
            while (!c->flag->load()) {
                Thread::yield();
            }
            for (int i = 0; i < ITERATIONS; ++i) {
                std::vector<std::string> jsons;
                c->s->list("/registry/pods/", jsons);
                Thread::yield();
            }
        }, &ctxs[r]);
    }

    start_flag.store(true);

    // Writer performs concurrent puts and removes
    for (int i = 0; i < ITERATIONS; ++i) {
        std::string key = "/registry/pods/default/p" + std::to_string(i % 10);
        store->put(key, "{\"i\":" + std::to_string(i) + "}", true);
        if (i % 5 == 0) {
            store->remove(key);
        }
    }

    for (int r = 0; r < NUM_READERS; ++r) {
        readers[r].join();
    }

    std::cout << "[PASS] test_store_concurrency" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 Storage Architecture Tests ===" << std::endl;
    test_radix_tree_edge_splitting_and_compaction();
    test_littlefs_journal_atomic_and_recovery();
    test_store_crud_and_occ();
    test_store_selective_persistence_and_recovery();
    test_store_watch_multiplexer();
    test_store_concurrency();
    std::cout << "=== All Storage Tests Passed Successfully ===" << std::endl;
    return 0;
}
