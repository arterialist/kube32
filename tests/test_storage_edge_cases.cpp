#include "storage/radix_tree.hpp"
#include "storage/littlefs_journal.hpp"
#include "storage/store.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <random>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

using namespace kube32::storage;
using namespace kube32::osal;

// ============================================================================
// TEST 1: Complex Key Mutations & Recursive Compaction Stress
// ============================================================================
void test_key_mutations_and_compaction() {
    std::cout << "\n[TEST 1] Complex Key Mutations & Recursive Compaction Stress" << std::endl;
    RadixTree tree;
    std::map<std::string, std::string> oracle;

    // 1. Generate 160 hierarchical keys across diverse namespaces & resource kinds
    std::vector<std::string> keys;
    std::vector<std::string> namespaces = {"default", "kube-system", "monitoring", "kube-public", "edge-ns"};
    std::vector<std::string> kinds = {"pods", "nodes", "deployments", "services", "configmaps"};

    for (const auto& ns : namespaces) {
        for (const auto& kind : kinds) {
            for (int i = 0; i < 6; ++i) {
                std::string k = "/registry/" + kind + "/" + ns + "/res-" + std::to_string(i);
                keys.push_back(k);
            }
        }
    }
    // Add 10 deep nested keys
    for (int i = 0; i < 10; ++i) {
        std::string k = "/registry/pods/default/pod-deep-" + std::to_string(i) + "/status/conditions/ready";
        keys.push_back(k);
    }

    std::cout << "  -> Inserting " << keys.size() << " hierarchical keys..." << std::endl;
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (size_t i = 0; i < keys.size(); ++i) {
        std::string val = "{\"key\":\"" + keys[i] + "\",\"idx\":" + std::to_string(i) + "}";
        bool is_new = false;
        bool ok = tree.put(keys[i], val, i + 1, false, false, &is_new);
        assert(ok);
        assert(is_new);
        oracle[keys[i]] = val;
    }

    assert(tree.count_leaves() == oracle.size());
    size_t peak_nodes = tree.count_nodes();
    std::cout << "  -> Tree built: " << tree.count_leaves() << " leaves, " 
              << peak_nodes << " internal/leaf nodes." << std::endl;

    // Verify all keys in oracle match in tree
    for (const auto& kv : oracle) {
        std::string val;
        uint64_t ver = 0;
        assert(tree.get(kv.first, val, &ver));
        assert(val == kv.second);
    }

    // 2. Randomly delete half the keys in shuffled order and verify compaction
    std::vector<std::string> keys_to_delete = keys;
    std::shuffle(keys_to_delete.begin(), keys_to_delete.end(), rng);
    size_t delete_count = keys.size() / 2;
    keys_to_delete.resize(delete_count);

    std::cout << "  -> Randomly deleting " << delete_count << " keys to trigger recursive compactions..." << std::endl;
    for (size_t i = 0; i < keys_to_delete.size(); ++i) {
        const std::string& del_key = keys_to_delete[i];
        std::string deleted_val;
        uint64_t deleted_ver = 0;
        bool ok = tree.remove(del_key, &deleted_val, &deleted_ver);
        assert(ok);
        assert(!deleted_val.empty());
        assert(deleted_ver > 0);

        // Verify key is gone
        std::string tmp;
        assert(!tree.get(del_key, tmp));

        // Verify duplicate deletion fails cleanly
        assert(!tree.remove(del_key));

        oracle.erase(del_key);
        assert(tree.count_leaves() == oracle.size());
    }

    size_t nodes_after_half_delete = tree.count_nodes();
    std::cout << "  -> Nodes after deleting half: " << nodes_after_half_delete 
              << " (compacted from peak " << peak_nodes << ")" << std::endl;
    assert(nodes_after_half_delete < peak_nodes);

    // Verify that ALL remaining keys in oracle are still 100% intact!
    for (const auto& kv : oracle) {
        std::string val;
        assert(tree.get(kv.first, val));
        assert(val == kv.second);
    }

    // 3. Overwrite remaining keys with new values
    std::cout << "  -> Overwriting remaining keys..." << std::endl;
    for (auto& kv : oracle) {
        std::string new_val = "{\"updated\":true,\"key\":\"" + kv.first + "\"}";
        bool is_new = true;
        // Without overwrite should fail
        assert(!tree.put(kv.first, new_val, 9999, false, false, &is_new));
        // With overwrite should succeed
        assert(tree.put(kv.first, new_val, 9999, false, true, &is_new));
        assert(!is_new);
        kv.second = new_val;
    }
    for (const auto& kv : oracle) {
        std::string val;
        uint64_t ver = 0;
        assert(tree.get(kv.first, val, &ver));
        assert(val == kv.second);
        assert(ver == 9999);
    }

    // 4. Pathological Prefix Shadowing Edge Cases
    std::cout << "  -> Testing prefix-suffix shadowing & edge compactions..." << std::endl;
    RadixTree edge_tree;
    std::vector<std::string> edge_keys = {
        "/a",
        "/a/b",
        "/a/b/c",
        "/a/b/c/d",
        "/ab",
        "/abc",
        "/abcd",
        "/a/bc",
        "/a/b/cd"
    };
    for (const auto& k : edge_keys) {
        assert(edge_tree.put(k, "val:" + k, 1, false, false));
    }
    assert(edge_tree.count_leaves() == edge_keys.size());

    // Remove middle key /a/b
    std::string tmp_val;
    assert(edge_tree.remove("/a/b"));
    assert(!edge_tree.get("/a/b", tmp_val));
    // Verify /a and children /a/b/c, /a/b/c/d still exist!
    assert(edge_tree.get("/a", tmp_val));
    assert(tmp_val == "val:/a");
    assert(edge_tree.get("/a/b/c", tmp_val));
    assert(tmp_val == "val:/a/b/c");
    assert(edge_tree.get("/a/b/c/d", tmp_val));
    assert(tmp_val == "val:/a/b/c/d");

    // Remove leaf /a/b/c/d
    assert(edge_tree.remove("/a/b/c/d"));
    assert(!edge_tree.get("/a/b/c/d", tmp_val));
    assert(edge_tree.get("/a/b/c", tmp_val));

    // Remove root prefix /a
    assert(edge_tree.remove("/a"));
    assert(!edge_tree.get("/a", tmp_val));
    assert(edge_tree.get("/a/b/c", tmp_val));
    assert(edge_tree.get("/ab", tmp_val));
    assert(edge_tree.get("/abc", tmp_val));
    assert(edge_tree.get("/abcd", tmp_val));

    // 5. Delete all remaining keys from main tree until completely empty
    std::cout << "  -> Deleting all remaining keys until empty..." << std::endl;
    auto remaining_keys = oracle;
    for (const auto& kv : remaining_keys) {
        assert(tree.remove(kv.first));
    }
    assert(tree.count_leaves() == 0);
    assert(tree.count_nodes() == 1); // Only empty root remains
    std::vector<std::string> empty_list;
    tree.list("", empty_list);
    assert(empty_list.empty());

    std::cout << "  [PASS] Complex Key Mutations & Recursive Compaction Stress Passed." << std::endl;
}

// ============================================================================
// TEST 2: Subtree Prefix Queries Across Namespaces
// ============================================================================
void test_subtree_prefix_queries() {
    std::cout << "\n[TEST 2] Subtree Prefix Queries Across Namespaces" << std::endl;
    RadixTree tree;

    // Insert keys:
    // 30 pods in 'default'
    for (int i = 0; i < 30; ++i) {
        tree.put("/registry/pods/default/pod-" + std::to_string(i), "{\"pod\":" + std::to_string(i) + "}", 1, false, false);
    }
    // 25 pods in 'kube-system'
    for (int i = 0; i < 25; ++i) {
        tree.put("/registry/pods/kube-system/daemon-" + std::to_string(i), "{\"daemon\":" + std::to_string(i) + "}", 1, false, false);
    }
    // 20 pods in 'monitoring'
    for (int i = 0; i < 20; ++i) {
        tree.put("/registry/pods/monitoring/mon-" + std::to_string(i), "{\"mon\":" + std::to_string(i) + "}", 1, false, false);
    }
    // 10 pods in 'default-alt' (tests prefix collision against 'default')
    for (int i = 0; i < 10; ++i) {
        tree.put("/registry/pods/default-alt/pod-" + std::to_string(i), "{\"alt\":" + std::to_string(i) + "}", 1, false, false);
    }
    // Sibling resources that share prefix /registry/pod* but are NOT pods!
    for (int i = 0; i < 15; ++i) {
        tree.put("/registry/podcasts/episode-" + std::to_string(i), "{\"ep\":" + std::to_string(i) + "}", 1, false, false);
    }
    for (int i = 0; i < 5; ++i) {
        tree.put("/registry/poddisruptionbudgets/pdb-" + std::to_string(i), "{\"pdb\":" + std::to_string(i) + "}", 1, false, false);
    }
    // Nodes & Deployments
    for (int i = 0; i < 12; ++i) {
        tree.put("/registry/nodes/esp32-" + std::to_string(i), "{\"node\":" + std::to_string(i) + "}", 1, false, false);
    }
    for (int i = 0; i < 8; ++i) {
        tree.put("/registry/deployments/default/dep-" + std::to_string(i), "{\"dep\":" + std::to_string(i) + "}", 1, false, false);
    }

    // Exact count verification
    std::vector<std::string> values;
    std::vector<std::string> keys;

    // 1. All pods: /registry/pods/ -> must be 30 + 25 + 20 + 10 = 85
    tree.list("/registry/pods/", values, &keys);
    std::cout << "  -> /registry/pods/ returned " << values.size() << " items (expected 85)" << std::endl;
    assert(values.size() == 85);
    assert(keys.size() == 85);
    for (const auto& k : keys) {
        assert(k.rfind("/registry/pods/", 0) == 0);
        assert(k.find("/podcasts/") == std::string::npos);
        assert(k.find("/poddisruptionbudgets/") == std::string::npos);
    }

    // 2. Exact namespace: /registry/pods/default/ -> must be 30 (must NOT include default-alt!)
    values.clear(); keys.clear();
    tree.list("/registry/pods/default/", values, &keys);
    std::cout << "  -> /registry/pods/default/ returned " << values.size() << " items (expected 30)" << std::endl;
    assert(values.size() == 30);
    for (const auto& k : keys) {
        assert(k.rfind("/registry/pods/default/", 0) == 0);
        assert(k.find("default-alt") == std::string::npos);
    }

    // 3. Namespace default-alt: /registry/pods/default-alt/ -> must be 10
    values.clear(); keys.clear();
    tree.list("/registry/pods/default-alt/", values, &keys);
    std::cout << "  -> /registry/pods/default-alt/ returned " << values.size() << " items (expected 10)" << std::endl;
    assert(values.size() == 10);
    for (const auto& k : keys) {
        assert(k.rfind("/registry/pods/default-alt/", 0) == 0);
    }

    // 4. Namespace kube-system: /registry/pods/kube-system/ -> must be 25
    values.clear(); keys.clear();
    tree.list("/registry/pods/kube-system/", values, &keys);
    assert(values.size() == 25);

    // 5. Namespace monitoring: /registry/pods/monitoring/ -> must be 20
    values.clear(); keys.clear();
    tree.list("/registry/pods/monitoring/", values, &keys);
    assert(values.size() == 20);

    // 6. Sibling /registry/podcasts/ -> must be 15
    values.clear(); keys.clear();
    tree.list("/registry/podcasts/", values, &keys);
    assert(values.size() == 15);

    // 7. Sibling /registry/poddisruptionbudgets/ -> must be 5
    values.clear(); keys.clear();
    tree.list("/registry/poddisruptionbudgets/", values, &keys);
    assert(values.size() == 5);

    // 8. Shared prefix without trailing slash: /registry/pod
    // Should match pods (85) + podcasts (15) + poddisruptionbudgets (5) = 105
    values.clear(); keys.clear();
    tree.list("/registry/pod", values, &keys);
    std::cout << "  -> /registry/pod returned " << values.size() << " items (expected 105)" << std::endl;
    assert(values.size() == 105);

    // 9. Total items in tree: "" -> 85 + 15 + 5 + 12 + 8 = 125
    values.clear(); keys.clear();
    tree.list("", values, &keys);
    assert(values.size() == 125);
    assert(tree.count_leaves() == 125);

    // 10. Non-existent prefix -> 0
    values.clear(); keys.clear();
    tree.list("/registry/pods/nonexistent/", values, &keys);
    assert(values.empty());
    assert(keys.empty());

    // 11. Dynamic mutation test: delete 10 pods from default
    for (int i = 0; i < 10; ++i) {
        assert(tree.remove("/registry/pods/default/pod-" + std::to_string(i)));
    }
    values.clear();
    tree.list("/registry/pods/default/", values);
    assert(values.size() == 20); // Dropped from 30 to 20
    values.clear();
    tree.list("/registry/pods/", values);
    assert(values.size() == 75); // Dropped from 85 to 75

    std::cout << "  [PASS] Subtree Prefix Queries Across Namespaces Passed." << std::endl;
}

// ============================================================================
// TEST 3: OCC CAS Conflict Stress & Rejection
// ============================================================================
void test_occ_cas_conflicts() {
    std::cout << "\n[TEST 3] OCC CAS Conflict Stress & Version Rejection" << std::endl;
    std::string test_dir = "/tmp/kube32_adv_occ_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // 1. Basic OCC CAS Rejection Checks
    const std::string counter_key = "/registry/counters/shared";
    Status s_init = store->put(counter_key, "{\"val\":0}", false);
    assert(s_init.ok());

    std::string json;
    uint64_t v_initial = 0;
    assert(store->get_extended(counter_key, json, &v_initial).ok());
    assert(v_initial > 0);

    // CAS with wrong version (stale expected_version)
    Status s_stale = store->put_extended(counter_key, "{\"val\":1}", true, false, v_initial + 999, nullptr);
    assert(s_stale.code == StatusCode::VersionConflict);
    std::cout << "  -> Stale version correctly rejected: " << s_stale.message << std::endl;

    // Value must not have changed
    assert(store->get_extended(counter_key, json, nullptr).ok());
    assert(json == "{\"val\":0}");

    // CAS with valid version
    uint64_t v_next = 0;
    Status s_valid = store->put_extended(counter_key, "{\"val\":1}", true, false, v_initial, &v_next);
    assert(s_valid.ok());
    assert(v_next > v_initial);

    // Re-trying with v_initial must now fail (version has advanced)
    Status s_advance_fail = store->put_extended(counter_key, "{\"val\":2}", true, false, v_initial, nullptr);
    assert(s_advance_fail.code == StatusCode::VersionConflict);

    // CAS on non-existent key with expected_version > 0:
    Status s_ghost = store->put_extended("/registry/pods/default/ghost-key", "{}", true, false, 42, nullptr);
    if (s_ghost.ok()) {
        std::cout << "  [FINDING / DEFECT 1] CAS allowed on non-existent key with expected_version=42! Status: Ok" << std::endl;
    } else {
        std::cout << "  -> CAS on non-existent key rejected as expected: " << s_ghost.message << std::endl;
    }

    // Empty key handling:
    Status s_empty = store->put("", "{\"empty\":true}", true);
    std::string val_empty;
    Status s_get_empty = store->get("", val_empty);
    if (s_empty.ok() && !s_get_empty.ok()) {
        std::cout << "  [FINDING / DEFECT 2] store->put(\"\") returned Ok despite tree_.put rejecting empty key (phantom write / ignored return value)!" << std::endl;
    }

    // 2. High-Contention Multi-Threaded CAS Race
    constexpr int NUM_THREADS = 6;
    constexpr int INCREMENTS_PER_THREAD = 80;
    std::atomic<bool> start_flag{false};
    std::atomic<int> total_conflicts{0};
    std::atomic<int> total_successes{0};

    struct ThreadArg {
        Store* s;
        std::string key;
        std::atomic<bool>* start;
        std::atomic<int>* conflicts;
        std::atomic<int>* successes;
        int target_increments;
    };

    std::vector<Thread> threads(NUM_THREADS);
    std::vector<ThreadArg> args(NUM_THREADS);

    std::cout << "  -> Launching " << NUM_THREADS << " threads racing on CAS updates (" 
              << INCREMENTS_PER_THREAD << " increments each)..." << std::endl;

    for (int t = 0; t < NUM_THREADS; ++t) {
        args[t] = {store.get(), counter_key, &start_flag, &total_conflicts, &total_successes, INCREMENTS_PER_THREAD};
        threads[t].start([](void* p) {
            auto* arg = static_cast<ThreadArg*>(p);
            while (!arg->start->load()) {
                Thread::yield();
            }

            int completed = 0;
            while (completed < arg->target_increments) {
                std::string cur_val;
                uint64_t cur_ver = 0;
                Status g_stat = arg->s->get_extended(arg->key, cur_val, &cur_ver);
                if (!g_stat.ok()) {
                    Thread::yield();
                    continue;
                }

                // Extract current counter value
                int cur_num = 0;
                size_t ppos = cur_val.find("\"val\":");
                if (ppos != std::string::npos) {
                    cur_num = std::stoi(cur_val.substr(ppos + 6));
                }

                std::string new_val = "{\"val\":" + std::to_string(cur_num + 1) + "}";
                uint64_t next_ver = 0;
                Status p_stat = arg->s->put_extended(arg->key, new_val, true, false, cur_ver, &next_ver);

                if (p_stat.ok()) {
                    completed++;
                    arg->successes->fetch_add(1);
                } else if (p_stat.code == StatusCode::VersionConflict) {
                    arg->conflicts->fetch_add(1);
                    Thread::yield();
                } else {
                    std::cerr << "Unexpected status: " << p_stat.message << std::endl;
                    assert(false);
                }
            }
        }, &args[t]);
    }

    start_flag.store(true);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads[t].join();
    }

    int final_successes = total_successes.load();
    int final_conflicts = total_conflicts.load();
    std::cout << "  -> Contention Race Results: " << final_successes << " successful CAS commits, " 
              << final_conflicts << " version conflicts handled." << std::endl;

    assert(final_successes == NUM_THREADS * INCREMENTS_PER_THREAD);
    // How many writes collide depends on how the OS interleaves the threads
    // (none when they happen to run one after another), so the conflict count
    // is reported, not asserted. No lost update is what is checked below.
    (void)final_conflicts;

    // Verify counter value exactly matches total completed increments (+1 from initial valid test)
    std::string final_json;
    uint64_t final_ver = 0;
    assert(store->get_extended(counter_key, final_json, &final_ver).ok());
    int final_val = std::stoi(final_json.substr(final_json.find("\"val\":") + 6));
    assert(final_val == 1 + NUM_THREADS * INCREMENTS_PER_THREAD);
    std::cout << "  -> Verified final counter value: " << final_val 
              << " matches expected " << (1 + NUM_THREADS * INCREMENTS_PER_THREAD) << std::endl;

    std::cout << "  [PASS] OCC CAS Conflict Stress & Version Rejection Passed." << std::endl;
}

// ============================================================================
// TEST 4: Power-Failure Crash Simulation & LittleFS Journal Recovery
// ============================================================================
void test_power_failure_crash_simulation() {
    std::cout << "\n[TEST 4] Power-Failure Crash Simulation & LittleFS Recovery" << std::endl;
    std::string crash_dir = "/tmp/kube32_adv_crash_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = crash_dir;
    opts.enable_flash_persistence = true;

    // 1. Initial State Commit
    {
        auto store = create_radix_store(opts);

        // Write 25 persistent pods
        for (int i = 0; i < 25; ++i) {
            std::string key = "/registry/pods/default/pod-" + std::to_string(i);
            std::string payload = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"pod-" + std::to_string(i) + 
                                  "\",\"resourceVersion\":\"" + std::to_string(100 + i) + "\"}}";
            Status s = store->put(key, payload, false);
            assert(s.ok());
        }

        // Write 10 persistent deployments
        for (int i = 0; i < 10; ++i) {
            std::string key = "/registry/deployments/prod/dep-" + std::to_string(i);
            std::string payload = "{\"kind\":\"Deployment\",\"metadata\":{\"name\":\"dep-" + std::to_string(i) + 
                                  "\",\"resourceVersion\":\"" + std::to_string(200 + i) + "\"}}";
            Status s = store->put(key, payload, false);
            assert(s.ok());
        }

        // Write 10 ephemeral leases (should NOT be written to disk)
        for (int i = 0; i < 10; ++i) {
            std::string key = "/registry/leases/kube-node-lease/node-" + std::to_string(i);
            std::string payload = "{\"holder\":\"node-" + std::to_string(i) + "\"}";
            Status s = store->put(key, payload, true);
            assert(s.ok());
        }
    } // Store is destructed

    // Verify baseline disk state
    std::string test_pod_file = crash_dir + "/registry/pods/default/pod-0.json";
    std::string test_lease_file = crash_dir + "/registry/leases/kube-node-lease/node-0.json";
    assert(access(test_pod_file.c_str(), F_OK) == 0);
    assert(access(test_lease_file.c_str(), F_OK) != 0); // Ephemeral lease bypassed flash

    // 2. Inject Simulated Power-Failure Crash Artifacts (Orphaned .tmp files)
    std::cout << "  -> Injecting simulated power-cut orphaned .tmp files across directories..." << std::endl;
    std::vector<std::string> injected_tmp_files = {
        // Crash scenario 1: Interrupted update on existing pod-5
        crash_dir + "/registry/pods/default/pod-5.json.tmp",
        // Crash scenario 2: Interrupted creation of uncommitted pod-99
        crash_dir + "/registry/pods/default/pod-99.json.tmp",
        // Crash scenario 3: Deep nested uncommitted namespace
        crash_dir + "/registry/pods/crashed-ns/orphan.json.tmp",
        // Crash scenario 4: Truncated 0-byte tmp files
        crash_dir + "/registry/deployments/prod/dep-3.json.tmp",
        crash_dir + "/registry/deployments/prod/orphan_zero.tmp",
        // Crash scenario 5: Root-level crash artifact
        crash_dir + "/registry/powercut_state.tmp"
    };

    // Ensure directory exists for crashed-ns
    mkdir((crash_dir + "/registry/pods/crashed-ns").c_str(), 0755);

    for (size_t i = 0; i < injected_tmp_files.size(); ++i) {
        FILE* fp = fopen(injected_tmp_files[i].c_str(), "w");
        assert(fp != nullptr);
        if (i % 2 == 0) {
            fputs("{\"corrupted_json_fragment\":true,", fp);
        }
        fclose(fp);
        assert(access(injected_tmp_files[i].c_str(), F_OK) == 0);
    }

    // 3. Instantiate Store and Execute Crash Recovery
    std::cout << "  -> Invoking store->recover()..." << std::endl;
    auto recovered_store = create_radix_store(opts);
    Status rec_status = recovered_store->recover();
    assert(rec_status.ok());

    // 4. Verification: All orphaned .tmp files MUST BE SWEPT AND REMOVED
    std::cout << "  -> Verifying orphan .tmp sweep..." << std::endl;
    for (const auto& tmp_path : injected_tmp_files) {
        int res = access(tmp_path.c_str(), F_OK);
        assert(res != 0); // File must have been deleted by recover()
    }

    // 5. Verification: Committed state restored accurately
    std::cout << "  -> Verifying committed state integrity..." << std::endl;
    for (int i = 0; i < 25; ++i) {
        std::string key = "/registry/pods/default/pod-" + std::to_string(i);
        std::string json;
        Status s = recovered_store->get(key, json);
        assert(s.ok());
        // pod-5 must have its committed content, NOT the corrupted tmp content
        assert(json.find("\"name\":\"pod-" + std::to_string(i) + "\"") != std::string::npos);
        assert(json.find("corrupted_json_fragment") == std::string::npos);
    }

    for (int i = 0; i < 10; ++i) {
        std::string key = "/registry/deployments/prod/dep-" + std::to_string(i);
        std::string json;
        assert(recovered_store->get(key, json).ok());
        assert(json.find("\"name\":\"dep-" + std::to_string(i) + "\"") != std::string::npos);
    }

    // Uncommitted pod-99 must NOT exist
    std::string dummy;
    assert(recovered_store->get("/registry/pods/default/pod-99", dummy).code == StatusCode::NotFound);
    assert(recovered_store->get("/registry/pods/crashed-ns/orphan", dummy).code == StatusCode::NotFound);

    // Ephemeral leases must NOT exist
    assert(recovered_store->get("/registry/leases/kube-node-lease/node-0", dummy).code == StatusCode::NotFound);

    // The store is the resourceVersion authority: every recovered object carries
    // the version the store assigned at write time, and the recovered
    // current_version must be above the largest of them (35 persistent writes).
    {
        std::vector<std::string> all_jsons;
        recovered_store->list("/registry/", all_jsons);
        uint64_t max_rv = 0;
        for (const auto& j : all_jsons) {
            size_t p = j.find("\"resourceVersion\":\"");
            assert(p != std::string::npos);
            uint64_t rv = std::strtoull(j.c_str() + p + 19, nullptr, 10);
            if (rv > max_rv) max_rv = rv;
        }
        assert(max_rv >= 35);
        assert(recovered_store->current_version() > max_rv);
        std::cout << "  -> Recovered current_version: " << recovered_store->current_version()
                  << " (> max stored resourceVersion " << max_rv << ")" << std::endl;
    }

    // 6. Post-recovery operability: verify puts and lists succeed
    std::vector<std::string> pod_list;
    recovered_store->list("/registry/pods/", pod_list);
    assert(pod_list.size() == 25);

    std::vector<std::string> dep_list;
    recovered_store->list("/registry/deployments/", dep_list);
    assert(dep_list.size() == 10);

    // Verify writing a new pod after recovery works seamlessly
    Status s_new = recovered_store->put("/registry/pods/default/post-reboot-pod", "{\"status\":\"healthy\"}", false);
    assert(s_new.ok());
    assert(recovered_store->get("/registry/pods/default/post-reboot-pod", dummy).ok());

    std::cout << "  [PASS] Power-Failure Crash Simulation & LittleFS Recovery Passed." << std::endl;
}

// ============================================================================
// TEST 5: Boundary Conditions
// ============================================================================
void test_vulnerabilities_and_edge_cases() {
    std::cout << "\n[TEST 5] Boundary Conditions" << std::endl;
    std::string test_dir = "/tmp/kube32_adv_vuln_" + std::to_string(Thread::get_time_us());
    StoreOptions opts;
    opts.base_path = test_dir;
    opts.enable_flash_persistence = true;
    auto store = create_radix_store(opts);

    // ------------------------------------------------------------------------
    // ADV-1: OCC CAS Phantom Write on Missing/Deleted Key
    // ------------------------------------------------------------------------
    std::cout << "  -> Probing ADV-1: CAS on non-existent key with expected_version > 0..." << std::endl;
    Status s_ghost = store->put_extended("/registry/pods/default/phantom-key", "{\"kind\":\"Pod\"}", true, false, 999, nullptr);
    // REGRESSION ASSERTION: CAS on non-existent key must be rejected with VersionConflict or NotFound
    assert(!s_ghost.ok());
    assert(s_ghost.code == StatusCode::VersionConflict || s_ghost.code == StatusCode::NotFound);
    std::cout << "     [PASS] CAS correctly rejected: " << s_ghost.message << std::endl;

    // ------------------------------------------------------------------------
    // ADV-2: Silent False-Positive on Empty Key
    // ------------------------------------------------------------------------
    std::cout << "  -> Probing ADV-2: Empty key insertion in store->put(\"\")..." << std::endl;
    Status s_empty = store->put("", "{\"empty\":true}", true);
    // REGRESSION ASSERTION: Empty key must be rejected with InvalidArgument
    assert(!s_empty.ok());
    assert(s_empty.code == StatusCode::InvalidArgument);
    std::cout << "     [PASS] store->put(\"\") rejected as expected: " << s_empty.message << std::endl;

    // ------------------------------------------------------------------------
    // ADV-3: Path Traversal Vulnerability in LittleFS Journal
    // ------------------------------------------------------------------------
    std::cout << "  -> Probing ADV-3: Directory traversal in LittleFSJournal..." << std::endl;
    LittleFSJournal journal(test_dir);
    std::string traversal_key = "/../../../tmp/kube32_traversal_test_" + std::to_string(Thread::get_time_us());
    std::string generated_path = journal.key_to_filepath(traversal_key);
    // REGRESSION ASSERTION: Traversal must be rejected (empty path) or sanitized without escaping base_path
    assert(generated_path.empty() || generated_path.find("..") == std::string::npos);
    std::cout << "     [PASS] Path traversal safely rejected/sanitized." << std::endl;

    // ------------------------------------------------------------------------
    // ADV-4: Synchronous Re-Entrancy Hazard in Watcher Callbacks
    // ------------------------------------------------------------------------
    std::cout << "  -> Probing ADV-4: Synchronous re-entrant read deadlock in watcher..." << std::endl;
    std::atomic<bool> inside_watcher{false};
    std::atomic<bool> reentrant_read_success{false};

    uint32_t wid = store->watch("/registry/pods/reentrant-test", [&](const WatchEvent& ev) {
        inside_watcher.store(true);
        // Attempting to read from the store inside watcher callback:
        std::string out;
        Status s = store->get(ev.key, out);
        if (s.ok()) {
            reentrant_read_success.store(true);
        }
    });

    Status s_reent = store->put("/registry/pods/reentrant-test/pod1", "{\"state\":\"running\"}", false);
    assert(s_reent.ok());
    assert(inside_watcher.load());
    assert(reentrant_read_success.load()); // Confirms watcher executed outside rw_mutex_!
    store->unwatch(wid);
    std::cout << "     [PASS] Watcher re-entrancy executed without deadlock." << std::endl;

    std::cout << "  [COMPLETED] Boundary Conditions Completed." << std::endl;
}

// ============================================================================
// MAIN RUNNER
// ============================================================================
int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "  KUBE32 STORAGE EDGE CASE TESTS (Radix Store & LittleFS)     " << std::endl;
    std::cout << "==================================================================" << std::endl;

    test_key_mutations_and_compaction();
    test_subtree_prefix_queries();
    test_occ_cas_conflicts();
    test_power_failure_crash_simulation();
    test_vulnerabilities_and_edge_cases();

    std::cout << "\n==================================================================" << std::endl;
    std::cout << "  >>> ALL STORAGE EDGE CASE TESTS COMPLETED <<<                   " << std::endl;
    std::cout << "==================================================================" << std::endl;
    return 0;
}
