// Object store persistence on raw flash: the log-structured journal
// (storage/flash_log.hpp), its recovery after power cuts, reclamation, the
// store's write coalescing and key classes (storage/persist_policy.hpp), and
// commit(): API writes are in the journal before the API answers.

#include "storage/store.hpp"
#include "storage/flash_log.hpp"
#include "storage/persist_policy.hpp"
#include "api/route_handler.hpp"
#include "mem/monotonic_arena.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unistd.h>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace kube32::storage;
namespace api = kube32::api;

static std::string pod_json(const std::string& name, const std::string& payload) {
    return "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},\"spec\":{\"x\":\"" + payload + "\"}}";
}

static std::string node_json(const std::string& name, const std::string& ready, int free_ki, const std::string& label = "a") {
    return "{\"kind\":\"Node\",\"metadata\":{\"name\":\"" + name + "\",\"labels\":{\"tier\":\"" + label + "\"}},"
           "\"spec\":{},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"" + ready + "\"}],"
           "\"allocatable\":{\"memory\":\"" + std::to_string(free_ki) + "Ki\"}}}";
}

// The payload pod_json() put in spec.x
static std::string spec_x(const std::string& json) {
    size_t at = json.find("\"x\":\"");
    if (at == std::string::npos) return std::string();
    at += 5;
    return json.substr(at, json.find('"', at) - at);
}

static api::HttpResponse call(api::RouteHandler& h, api::HttpMethod method, const std::string& uri, const std::string& body = "") {
    api::HttpRequest req;
    req.method = method;
    req.raw_uri = uri;
    api::normalize_uri(uri, req.path, req.query_string);
    req.body = body;
    req.content_type = "application/json";
    kube32::mem::MonotonicArena arena;
    return h.handle_request(req, arena);
}

static std::string config_map(const std::string& name, const std::string& value) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"ConfigMap\",\"metadata\":{\"name\":\"" + name + "\"},\"data\":{\"v\":\"" + value + "\"}}";
}

static StoreOptions log_options(FlashRegion& region) {
    StoreOptions o;
    o.enable_flash_persistence = true;
    o.flash_region = &region;
    o.flush_delay_ms = 0;
    o.deferred_flush_delay_ms = 3600 * 1000;
    return o;
}

static void test_store_survives_restart() {
    std::cout << "[RUN] test_store_survives_restart" << std::endl;
    RamFlashRegion flash(64 * 4096);
    uint64_t version_before = 0;
    {
        auto store = create_radix_store(log_options(flash));
        assert(store->recover().ok());
        assert(store->put("/registry/pods/default/web", pod_json("web", "v1"), false).ok());
        assert(store->put("/registry/deployments/default/api", "{\"kind\":\"Deployment\",\"metadata\":{\"name\":\"api\"}}", false).ok());
        assert(store->put("/registry/pods/default/gone", pod_json("gone", "x"), false).ok());
        // Leases and Events never reach flash, but they take revisions
        for (int i = 0; i < 50; ++i) {
            assert(store->put("/registry/leases/kube-node-lease/n1", "{\"metadata\":{\"name\":\"n1\"},\"spec\":{\"i\":" + std::to_string(i) + "}}", true).ok());
        }
        assert(store->put_extended("/registry/events/default/e1", "{\"metadata\":{\"name\":\"e1\"}}", true, false).ok());
        assert(store->flush(false).ok());
        assert(store->remove("/registry/pods/default/gone").ok());
        assert(store->put("/registry/pods/default/web", pod_json("web", "v2"), true).ok());
        assert(store->flush(false).ok());
        version_before = store->current_version();
        PersistenceStats ps;
        assert(store->persistence_stats(ps));
        assert(ps.live_keys == 2);
        assert(ps.pending_keys == 0);
    }
    auto store = create_radix_store(log_options(flash));
    assert(store->recover().ok());
    std::string json;
    assert(store->get("/registry/pods/default/web", json).ok());
    assert(json.find("\"v2\"") != std::string::npos);
    assert(store->get("/registry/deployments/default/api", json).ok());
    assert(store->get("/registry/pods/default/gone", json).code == StatusCode::NotFound);
    assert(store->get("/registry/leases/kube-node-lease/n1", json).code == StatusCode::NotFound);
    assert(store->get("/registry/events/default/e1", json).code == StatusCode::NotFound);
    // Revisions handed out before the restart (the lease's included) are never reused
    assert(store->current_version() > version_before);
    std::cout << "[PASS] test_store_survives_restart" << std::endl;
}

static void test_writes_coalesce_and_unchanged_values_skip() {
    std::cout << "[RUN] test_writes_coalesce_and_unchanged_values_skip" << std::endl;
    RamFlashRegion flash(64 * 4096);
    StoreOptions o = log_options(flash);
    o.flush_delay_ms = 60000;   // nothing is due before an explicit full flush
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    for (int i = 0; i < 100; ++i) {
        assert(store->put("/registry/pods/default/busy", pod_json("busy", std::to_string(i)), true).ok());
    }
    assert(store->flush(false).ok());
    PersistenceStats ps;
    store->persistence_stats(ps);
    assert(ps.pending_keys == 1);
    assert(ps.live_keys == 0);
    assert(store->flush(true).ok());
    store->persistence_stats(ps);
    const uint64_t after_first = ps.records_written;
    assert(ps.live_keys == 1);
    // Rewriting the same content (only resourceVersion differs) costs nothing
    assert(store->put("/registry/pods/default/busy", pod_json("busy", "99"), true).ok());
    assert(store->flush(true).ok());
    store->persistence_stats(ps);
    assert(ps.records_written == after_first);
    assert(ps.unchanged_skipped == 1);
    std::cout << "[PASS] test_writes_coalesce_and_unchanged_values_skip" << std::endl;
}

static void test_node_heartbeats_are_deferred() {
    std::cout << "[RUN] test_node_heartbeats_are_deferred" << std::endl;
    assert(persist_class("/registry/nodes/a") == PersistClass::Deferred);
    assert(persist_class("/registry/leases/kube-node-lease/a") == PersistClass::Ephemeral);
    assert(persist_class("/registry/events/default/x") == PersistClass::Ephemeral);
    assert(persist_class("/registry/pods/default/x") == PersistClass::Immediate);

    RamFlashRegion flash(64 * 4096);
    auto store = create_radix_store(log_options(flash));
    assert(store->recover().ok());
    const std::string key = "/registry/nodes/board-1";
    assert(store->put(key, node_json("board-1", "True", 100), false).ok());
    assert(store->flush(false).ok());
    PersistenceStats ps;
    store->persistence_stats(ps);
    uint64_t written = ps.records_written;
    assert(written >= 1);
    // Heartbeats: free memory moves, nothing durable changes
    for (int i = 0; i < 20; ++i) {
        assert(store->put(key, node_json("board-1", "True", 100 + i), true).ok());
        assert(store->flush(false).ok());
    }
    store->persistence_stats(ps);
    assert(ps.records_written == written);
    assert(ps.pending_keys == 1);
    // NotReady is written right away, and so is a label change
    assert(store->put(key, node_json("board-1", "Unknown", 120), true).ok());
    assert(store->flush(false).ok());
    store->persistence_stats(ps);
    assert(ps.records_written > written);
    written = ps.records_written;
    assert(store->put(key, node_json("board-1", "Unknown", 121, "b"), true).ok());
    assert(store->flush(false).ok());
    store->persistence_stats(ps);
    assert(ps.records_written > written);
    std::cout << "[PASS] test_node_heartbeats_are_deferred" << std::endl;
}

static void test_reclamation_rotates_sectors(bool nor) {
    std::cout << "[RUN] test_reclamation_rotates_sectors (" << (nor ? "NOR flash" : "file image") << ")" << std::endl;
    const size_t sectors = 48;
    RamFlashRegion flash(sectors * 4096, 4096, nor);
    std::map<std::string, std::string> expect;
    {
        StoreOptions o = log_options(flash);
        o.max_value_size_bytes = 8 * 1024;
        auto store = create_radix_store(o);
        assert(store->recover().ok());
        std::mt19937 rng(7);
        for (int round = 0; round < 3000; ++round) {
            std::string key = "/registry/configmaps/default/cm-" + std::to_string(rng() % 12);
            std::string payload(200 + rng() % 900, static_cast<char>('a' + round % 26));
            std::string json = pod_json(key.substr(key.rfind('/') + 1), payload + std::to_string(round));
            assert(store->put(key, json, true).ok());
            if (rng() % 9 == 0) { store->remove(key); expect.erase(key); }
            else expect[key] = payload + std::to_string(round);
            assert(store->flush(false).ok());
        }
        PersistenceStats ps;
        store->persistence_stats(ps);
        // The log went around the region many times: every sector was reused
        assert(!nor || ps.sectors_erased > sectors * 10);
        assert(ps.bytes_written > sectors * 4096 * 10);
        assert(ps.records_moved > 0);
        assert(ps.write_errors == 0);
        std::cout << "  " << ps.records_written << " records, " << ps.sectors_erased << " erases over " << sectors
                  << " sectors, " << ps.records_moved << " moved, live " << ps.live_bytes << " B" << std::endl;
    }
    auto store = create_radix_store(log_options(flash));
    assert(store->recover().ok());
    std::vector<std::string> all;
    store->list("/registry/configmaps/", all);
    assert(all.size() == expect.size());
    for (const auto& kv : expect) {
        std::string json;
        assert(store->get(kv.first, json).ok());
        assert(json.find(kv.second) != std::string::npos);
    }
    std::cout << "[PASS] test_reclamation_rotates_sectors" << std::endl;
}

static void test_large_values_span_sectors() {
    std::cout << "[RUN] test_large_values_span_sectors" << std::endl;
    RamFlashRegion flash(80 * 4096);
    std::string big(30000, 'z');
    {
        auto store = create_radix_store(log_options(flash));
        assert(store->recover().ok());
        for (int i = 0; i < 6; ++i) {
            assert(store->put("/registry/configmaps/default/big", pod_json("big", big + std::to_string(i)), true).ok());
            assert(store->put("/registry/configmaps/default/small", pod_json("small", std::to_string(i)), true).ok());
            assert(store->flush(false).ok());
        }
    }
    auto store = create_radix_store(log_options(flash));
    assert(store->recover().ok());
    std::string json;
    assert(store->get("/registry/configmaps/default/big", json).ok());
    assert(json.find(big + "5") != std::string::npos);
    assert(store->get("/registry/configmaps/default/small", json).ok());
    std::cout << "[PASS] test_large_values_span_sectors" << std::endl;
}

// A power cut at any byte of a write leaves every key at a value it had: the
// last one that was completely written, or the one being written.
static void test_power_cut_at_every_point(bool nor) {
    std::cout << "[RUN] test_power_cut_at_every_point (" << (nor ? "NOR flash" : "file image") << ")" << std::endl;
    std::mt19937 rng(11);
    int cuts = 0;
    for (long long budget = 0; budget < 90000; budget += 37 + rng() % 400) {
        RamFlashRegion flash(24 * 4096, 4096, nor);
        std::map<std::string, std::string> committed;
        std::map<std::string, std::string> attempted;
        {
            StoreOptions o = log_options(flash);
            o.max_value_size_bytes = 6 * 1024;
            auto store = create_radix_store(o);
            assert(store->recover().ok());
            for (int i = 0; i < 40; ++i) {
                std::string key = "/registry/services/default/s" + std::to_string(i % 5);
                std::string value = "v" + std::to_string(i) + std::string(150 + (i * 97) % 700, 'q');
                store->put(key, pod_json("s", value), true);
                committed[key] = value;
            }
            assert(store->flush(true).ok());
            flash.set_write_budget(budget);
            for (int i = 40; i < 140; ++i) {
                std::string key = "/registry/services/default/s" + std::to_string(i % 5);
                std::string value = "v" + std::to_string(i) + std::string(150 + (i * 97) % 700, 'q');
                store->put(key, pod_json("s", value), true);
                attempted[key] = value;
                store->flush(true);
            }
        }
        flash.set_write_budget(-1);
        StoreOptions o = log_options(flash);
        o.max_value_size_bytes = 6 * 1024;
        auto store = create_radix_store(o);
        assert(store->recover().ok());
        for (int k = 0; k < 5; ++k) {
            std::string key = "/registry/services/default/s" + std::to_string(k);
            std::string json;
            assert(store->get(key, json).ok());
            // The recovered value is one this key had at some point at or after the full flush
            size_t at = json.find("\"x\":\"v");
            assert(at != std::string::npos);
            int idx = std::atoi(json.c_str() + at + 6);
            assert(idx >= 35 && idx % 5 == k);
            std::string expect = "v" + std::to_string(idx) + std::string(150 + (idx * 97) % 700, 'q');
            assert(json.find(expect + "\"") != std::string::npos);
        }
        // The recovered log keeps working
        assert(store->put("/registry/services/default/after", pod_json("after", "ok"), false).ok());
        assert(store->flush(true).ok());
        auto again = create_radix_store(o);
        assert(again->recover().ok());
        std::string json;
        assert(again->get("/registry/services/default/after", json).ok());
        ++cuts;
    }
    std::cout << "  " << cuts << " power cuts recovered" << std::endl;
    std::cout << "[PASS] test_power_cut_at_every_point" << std::endl;
}

static void test_full_journal_refuses_creates() {
    std::cout << "[RUN] test_full_journal_refuses_creates" << std::endl;
    RamFlashRegion flash(40 * 4096);
    StoreOptions o = log_options(flash);
    o.max_value_size_bytes = 4096;
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    std::string payload(3000, 'f');
    int created = 0;
    Status last = Status::Ok();
    for (int i = 0; i < 200; ++i) {
        last = store->put("/registry/configmaps/default/c" + std::to_string(i), pod_json("c", payload), false);
        if (!last.ok()) break;
        ++created;
        assert(store->flush(false).ok());
    }
    assert(last.code == StatusCode::StorageFull);
    assert(created > 5);
    // Updates and deletes still work when full
    assert(store->put("/registry/configmaps/default/c0", pod_json("c", "small"), true).ok());
    assert(store->remove("/registry/configmaps/default/c1").ok());
    assert(store->flush(false).ok());
    std::cout << "[PASS] test_full_journal_refuses_creates (" << created << " objects)" << std::endl;
}

static void test_file_region_survives_reopen() {
    std::cout << "[RUN] test_file_region_survives_reopen" << std::endl;
    char path[] = "/tmp/kube32_flash_image_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    std::remove(path);
    {
        auto region = create_file_flash_region(path, 32 * 4096);
        assert(region);
        auto store = create_radix_store(log_options(*region));
        assert(store->recover().ok());
        assert(store->put("/registry/namespaces/team", "{\"kind\":\"Namespace\",\"metadata\":{\"name\":\"team\"}}", false).ok());
        assert(store->flush(true).ok());
    }
    auto region = create_file_flash_region(path, 32 * 4096);
    auto store = create_radix_store(log_options(*region));
    assert(store->recover().ok());
    std::string json;
    assert(store->get("/registry/namespaces/team", json).ok());
    std::remove(path);
    std::cout << "[PASS] test_file_region_survives_reopen" << std::endl;
}

// The control plane crashes several times in a row, each time at any byte of
// a journal write, and recovers in between. Every change whose commit()
// returned before the cut comes back (or a later change to the same key), on
// NOR flash and on a file image alike, and revisions never go back.
static void test_repeated_crashes_keep_committed_writes(bool nor) {
    std::cout << "[RUN] test_repeated_crashes_keep_committed_writes (" << (nor ? "NOR flash" : "file image") << ")" << std::endl;
    struct Known {
        bool present{false};
        std::string value;
        std::vector<std::string> later;   // written after it, not committed
        bool later_delete{false};
    };
    int cuts = 0;
    for (int trial = 0; trial < 60; ++trial) {
        std::mt19937 rng(100 + trial);
        const size_t sectors = 12 + rng() % 12;
        RamFlashRegion flash(sectors * 4096, 4096, nor);
        std::map<std::string, Known> known;
        uint64_t committed_version = 0;
        const int boots = 6;
        for (int boot = 0; boot < boots; ++boot) {
            flash.set_write_budget(-1);
            StoreOptions o = log_options(flash);
            o.flush_delay_ms = 60000;   // only commit() writes
            o.max_value_size_bytes = 3000;
            auto store = create_radix_store(o);
            assert(store->recover().ok());
            assert(store->current_version() > committed_version);
            for (auto& kv : known) {
                std::string json;
                Known& k = kv.second;
                if (store->get(kv.first, json).ok()) {
                    const std::string v = spec_x(json);
                    assert((k.present && v == k.value) || std::find(k.later.begin(), k.later.end(), v) != k.later.end());
                    k.present = true;
                    k.value = v;
                } else {
                    assert(!k.present || k.later_delete);
                    k.present = false;
                }
                k.later.clear();
                k.later_delete = false;
            }
            if (boot + 1 < boots) flash.set_write_budget(static_cast<long long>(rng() % (sectors * 4096)));
            for (int i = 0; i < 60; ++i) {
                const std::string key = "/registry/configmaps/default/c" + std::to_string(rng() % 8);
                const bool del = rng() % 6 == 0;
                const std::string value = "b" + std::to_string(boot) + "-" + std::to_string(i) + std::string(rng() % 1800, 'k');
                Status s = del ? store->remove(key) : store->put_extended(key, pod_json("c", value), true, true);
                if (!s.ok()) continue;
                Known& k = known[key];
                if (del) k.later_delete = true;
                else k.later.push_back(value);
                // Committed when every byte of it reached flash before the cut
                if (store->commit().ok() && flash.write_budget() != 0) {
                    k.present = !del;
                    k.value = del ? std::string() : value;
                    k.later.clear();
                    k.later_delete = false;
                    committed_version = store->current_version();
                }
            }
            if (flash.write_budget() == 0) ++cuts;
        }
    }
    assert(cuts > 150);
    std::cout << "  " << cuts << " crashes recovered" << std::endl;
    std::cout << "[PASS] test_repeated_crashes_keep_committed_writes" << std::endl;
}

// A journal file left on the FAT volume moves onto the raw partition: the copy
// recovers to the same objects and revision, a copy cut short is made again,
// and a newer log on the partition is never replaced.
static void test_journal_moves_to_partition() {
    std::cout << "[RUN] test_journal_moves_to_partition" << std::endl;
    const size_t sectors = 40;
    RamFlashRegion file(sectors * 4096, 4096, false);
    RamFlashRegion partition(sectors * 4096);
    std::map<std::string, std::string> expect;
    uint64_t version = 0;
    {
        // A log that went around the file several times: reclaimed sectors
        // and stale ones from earlier passes are still in it
        auto store = create_radix_store(log_options(file));
        assert(store->recover().ok());
        std::mt19937 rng(5);
        for (int round = 0; round < 1500; ++round) {
            const std::string key = "/registry/configmaps/default/m" + std::to_string(rng() % 10);
            const std::string value = std::to_string(round) + std::string(100 + rng() % 600, 'm');
            assert(store->put(key, pod_json("m", value), true).ok());
            expect[key] = value;
            assert(store->flush(false).ok());
        }
        version = store->current_version();
    }
    auto check = [&](FlashRegion& region) {
        auto store = create_radix_store(log_options(region));
        assert(store->recover().ok());
        std::vector<std::string> all;
        store->list("/registry/configmaps/", all);
        assert(all.size() == expect.size());
        for (const auto& kv : expect) {
            std::string json;
            assert(store->get(kv.first, json).ok());
            assert(spec_x(json) == kv.second);
        }
        assert(store->current_version() >= version);
    };
    // A crash cuts the first copy short; the next boot copies again
    bool copied = false;
    partition.set_write_budget(30000);
    FlashLog::copy_log(file, partition, copied);
    partition.set_write_budget(-1);
    assert(FlashLog::copy_log(file, partition, copied) && copied);
    check(partition);
    // Copying again changes nothing
    const std::vector<uint8_t> image = partition.bytes();
    assert(FlashLog::copy_log(file, partition, copied) && copied);
    assert(partition.bytes() == image);
    // Once the partition has moved on, the file is older and stays out
    {
        auto store = create_radix_store(log_options(partition));
        assert(store->recover().ok());
        assert(store->put("/registry/configmaps/default/after", pod_json("after", "moved"), false).ok());
        assert(store->commit().ok());
        expect["/registry/configmaps/default/after"] = "moved";
    }
    assert(FlashLog::copy_log(file, partition, copied) && !copied);
    check(partition);
    // Nothing to move from an empty file; regions of another size are refused
    RamFlashRegion empty(sectors * 4096, 4096, false);
    assert(FlashLog::copy_log(empty, partition, copied) && !copied);
    RamFlashRegion smaller((sectors - 8) * 4096);
    assert(!FlashLog::copy_log(file, smaller, copied));
    check(partition);
    std::cout << "[PASS] test_journal_moves_to_partition" << std::endl;
}

// A journal that cannot be read at boot is not taken for an empty one: the
// store reports it, writes nothing over it and the API refuses changes it
// cannot persist; once the flash reads again every object is back.
static void test_unreadable_journal_is_left_alone() {
    std::cout << "[RUN] test_unreadable_journal_is_left_alone" << std::endl;
    RamFlashRegion flash(32 * 4096);
    {
        auto store = create_radix_store(log_options(flash));
        assert(store->recover().ok());
        for (int i = 0; i < 3; ++i) {
            assert(store->put("/registry/configmaps/default/k" + std::to_string(i), pod_json("k", std::to_string(i)), false).ok());
        }
        assert(store->flush(true).ok());
    }
    const std::vector<uint8_t> image = flash.bytes();
    flash.set_read_error(true);
    {
        StoreOptions o = log_options(flash);
        o.flush_delay_ms = 60000;
        auto store = create_radix_store(o);
        assert(!store->recover().ok());
        assert(store->put("/registry/configmaps/default/new", pod_json("new", "x"), false).ok());
        assert(!store->commit().ok());
        assert(!store->flush(true).ok());
        api::RouteHandler h(*store);
        assert(call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/configmaps", config_map("refused", "1")).status_code == 500);
    }
    assert(flash.bytes() == image);
    flash.set_read_error(false);
    auto store = create_radix_store(log_options(flash));
    assert(store->recover().ok());
    std::vector<std::string> all;
    store->list("/registry/configmaps/", all);
    assert(all.size() == 3);
    std::cout << "[PASS] test_unreadable_journal_is_left_alone" << std::endl;
}

// The API server answers a create, an update or a delete only once it is in
// the journal: a crash right after the answer (no flush ever ran) keeps it.
static void test_api_writes_are_durable_before_the_answer() {
    std::cout << "[RUN] test_api_writes_are_durable_before_the_answer" << std::endl;
    RamFlashRegion flash(64 * 4096);
    StoreOptions o = log_options(flash);
    o.flush_delay_ms = 60000;   // the periodic flush never comes
    const std::string cms = "/api/v1/namespaces/default/configmaps";
    {
        auto store = create_radix_store(o);
        assert(store->recover().ok());
        api::RouteHandler h(*store);
        assert(call(h, api::HttpMethod::Post, cms, config_map("settings", "one")).status_code == 201);
        assert(call(h, api::HttpMethod::Post, cms, config_map("gone", "x")).status_code == 201);
        assert(call(h, api::HttpMethod::Put, cms + "/settings", config_map("settings", "two")).status_code == 200);
        assert(call(h, api::HttpMethod::Delete, cms + "/gone").status_code == 200);
        assert(call(h, api::HttpMethod::Post, cms + "?dryRun=All", config_map("dry", "x")).status_code == 201);
        PersistenceStats ps;
        store->persistence_stats(ps);
        assert(ps.pending_keys == 0);
        // The process dies here
    }
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    std::string json;
    assert(store->get("/registry/configmaps/default/settings", json).ok());
    assert(json.find("\"two\"") != std::string::npos);
    assert(store->get("/registry/configmaps/default/gone", json).code == StatusCode::NotFound);
    assert(store->get("/registry/configmaps/default/dry", json).code == StatusCode::NotFound);
    assert(store->get("/registry/namespaces/default", json).ok());
    std::cout << "[PASS] test_api_writes_are_durable_before_the_answer" << std::endl;
}

// Writers that commit at the same time share a journal write: the batch that
// runs while they arrive takes all their changes with one sync.
static void test_commit_groups_concurrent_writers() {
    std::cout << "[RUN] test_commit_groups_concurrent_writers" << std::endl;
    RamFlashRegion flash(64 * 4096);
    StoreOptions o = log_options(flash);
    o.flush_delay_ms = 60000;
    const int writers = 8;
    std::atomic<int> changed{0};
    std::atomic<int> batches{0};
    o.io_call = [&](const std::function<void()>& fn) {
        // Slow flash: the batch starts once every writer has made its change
        ++batches;
        for (int i = 0; i < 5000 && changed.load() < writers; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        fn();
    };
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    const uint64_t syncs = flash.sync_count();
    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < writers; ++i) {
        threads.emplace_back([&, i]() {
            const std::string key = "/registry/configmaps/default/w" + std::to_string(i);
            const bool put = store->put(key, pod_json("w", std::to_string(i)), false).ok();
            ++changed;
            if (put && store->commit().ok()) ++committed;
        });
    }
    for (auto& t : threads) t.join();
    assert(committed == writers);
    assert(batches == 1);
    assert(flash.sync_count() - syncs == 1);
    auto again = create_radix_store(log_options(flash));
    assert(again->recover().ok());
    std::vector<std::string> all;
    again->list("/registry/configmaps/default/", all);
    assert(all.size() == static_cast<size_t>(writers));
    std::cout << "[PASS] test_commit_groups_concurrent_writers" << std::endl;
}

// commit() leaves Node heartbeat churn for the deferred interval: a status
// update that only moves heartbeat fields costs no journal write.
static void test_commit_leaves_heartbeats_deferred() {
    std::cout << "[RUN] test_commit_leaves_heartbeats_deferred" << std::endl;
    RamFlashRegion flash(64 * 4096);
    StoreOptions o = log_options(flash);
    o.flush_delay_ms = 60000;
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    const std::string key = "/registry/nodes/board-1";
    assert(store->put(key, node_json("board-1", "True", 100), false).ok());
    assert(store->commit().ok());
    PersistenceStats ps;
    store->persistence_stats(ps);
    const uint64_t written = ps.records_written;
    const uint64_t syncs = flash.sync_count();
    for (int i = 1; i <= 20; ++i) {
        assert(store->put(key, node_json("board-1", "True", 100 + i), true).ok());
        assert(store->commit().ok());
    }
    store->persistence_stats(ps);
    assert(ps.records_written == written);
    assert(flash.sync_count() == syncs);
    assert(ps.pending_keys == 1);
    assert(store->put(key, node_json("board-1", "Unknown", 130), true).ok());
    assert(store->commit().ok());
    store->persistence_stats(ps);
    assert(ps.records_written > written);
    assert(ps.pending_keys == 0);
    std::cout << "[PASS] test_commit_leaves_heartbeats_deferred" << std::endl;
}

// A write that asks not to be persisted (a controller's status update) leaves
// its key in the journal: the flush pending for the key writes the latest
// value, not a delete, and a key not yet flushed still reaches the journal.
static void test_unpersisted_write_keeps_key_in_journal() {
    std::cout << "[RUN] test_unpersisted_write_keeps_key_in_journal" << std::endl;
    RamFlashRegion flash(64 * 4096);
    StoreOptions o = log_options(flash);
    o.flush_delay_ms = 60000;
    const std::string fresh = "/registry/ingresses/default/fresh";
    const std::string known = "/registry/ingresses/default/known";
    {
        auto store = create_radix_store(o);
        assert(store->recover().ok());
        // Already in the journal, then changed: a flush is pending
        assert(store->put(known, pod_json("known", "v1"), false).ok());
        assert(store->flush(true).ok());
        uint64_t v = 0;
        assert(store->put_extended(known, pod_json("known", "v2"), true, true, 0, &v).ok());
        assert(store->put_extended(known, pod_json("known", "v2-status"), true, false, v).ok());
        // Created, then its status written before any flush
        assert(store->put_extended(fresh, pod_json("fresh", "v1"), false, true, 0, &v).ok());
        assert(store->put_extended(fresh, pod_json("fresh", "v1-status"), true, false, v).ok());
        assert(store->flush(true).ok());
    }
    auto store = create_radix_store(o);
    assert(store->recover().ok());
    std::string json;
    assert(store->get(known, json).ok());
    assert(spec_x(json) == "v2-status");
    assert(store->get(fresh, json).ok());
    assert(spec_x(json) == "v1-status");
    std::cout << "[PASS] test_unpersisted_write_keeps_key_in_journal" << std::endl;
}

int main() {
    test_store_survives_restart();
    test_writes_coalesce_and_unchanged_values_skip();
    test_node_heartbeats_are_deferred();
    test_reclamation_rotates_sectors(true);
    test_reclamation_rotates_sectors(false);
    test_large_values_span_sectors();
    test_power_cut_at_every_point(true);
    test_power_cut_at_every_point(false);
    test_full_journal_refuses_creates();
    test_file_region_survives_reopen();
    test_repeated_crashes_keep_committed_writes(true);
    test_repeated_crashes_keep_committed_writes(false);
    test_journal_moves_to_partition();
    test_unreadable_journal_is_left_alone();
    test_api_writes_are_durable_before_the_answer();
    test_commit_groups_concurrent_writers();
    test_commit_leaves_heartbeats_deferred();
    test_unpersisted_write_keeps_key_in_journal();
    std::cout << "All flash journal tests passed" << std::endl;
    return 0;
}
