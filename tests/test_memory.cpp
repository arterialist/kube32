#include "mem/memory_tier.hpp"
#include "mem/slab_allocator.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <atomic>

using namespace kube32::mem;
using namespace kube32::osal;

void test_memory_manager_tiers() {
    std::cout << "[RUN] test_memory_manager_tiers" << std::endl;
    MemoryManager::init();

    void* sram_buf = MemoryManager::allocate(1024, MemoryTier::InternalSRAM, 8);
    assert(sram_buf != nullptr);
    assert(reinterpret_cast<uintptr_t>(sram_buf) % 8 == 0);

    void* psram_buf = MemoryManager::allocate(4096, MemoryTier::ExternalPSRAM, 16);
    assert(psram_buf != nullptr);
    assert(reinterpret_cast<uintptr_t>(psram_buf) % 16 == 0);

    MemoryStats stats = MemoryManager::get_stats();
    assert(stats.sram_free < stats.sram_total);
    assert(stats.psram_free < stats.psram_total);

    MemoryManager::deallocate(sram_buf, MemoryTier::InternalSRAM);
    MemoryManager::deallocate(psram_buf, MemoryTier::ExternalPSRAM);

    MemoryStats stats_after = MemoryManager::get_stats();
    assert(stats_after.sram_free == stats.sram_total);
    assert(stats_after.psram_free == stats.psram_total);

    assert(MemoryManager::check_integrity());
    std::cout << "[PASS] test_memory_manager_tiers" << std::endl;
}

void test_memory_manager_exhaustion() {
    std::cout << "[RUN] test_memory_manager_exhaustion" << std::endl;
    MemoryManager::init();

    // Attempt to allocate 300KB from 240KB SRAM pool
    void* oversized = MemoryManager::allocate(300 * 1024, MemoryTier::InternalSRAM);
    assert(oversized == nullptr);

    // Attempt to allocate 3MB from 2MB PSRAM pool
    void* oversized_psram = MemoryManager::allocate(3 * 1024 * 1024, MemoryTier::ExternalPSRAM);
    assert(oversized_psram == nullptr);

    std::cout << "[PASS] test_memory_manager_exhaustion" << std::endl;
}

struct TestItem {
    int id;
    std::string payload;

    TestItem(int i, std::string p) : id(i), payload(std::move(p)) {}
};

void test_slab_allocator_basic() {
    std::cout << "[RUN] test_slab_allocator_basic" << std::endl;
    constexpr size_t CAPACITY = 4;
    SlabAllocator<TestItem, CAPACITY> pool;

    assert(pool.capacity() == CAPACITY);
    assert(pool.active_count() == 0);
    assert(pool.free_count() == CAPACITY);
    assert(!pool.is_full());

    TestItem* item1 = pool.allocate(1, "item1");
    assert(item1 != nullptr);
    assert(item1->id == 1);
    assert(item1->payload == "item1");
    assert(pool.active_count() == 1);

    TestItem* item2 = pool.allocate(2, "item2");
    TestItem* item3 = pool.allocate(3, "item3");
    TestItem* item4 = pool.allocate(4, "item4");
    assert(item4 != nullptr);
    assert(pool.is_full());
    assert(pool.high_water_mark() == 4);

    // 5th allocation should fail
    TestItem* item5 = pool.allocate(5, "item5");
    assert(item5 == nullptr);

    // Deallocate item2 and reallocate
    pool.deallocate(item2);
    assert(!pool.is_full());
    assert(pool.active_count() == 3);

    TestItem* item2_new = pool.allocate(22, "item2_new");
    assert(item2_new != nullptr);
    assert(item2_new->id == 22);

    pool.deallocate(item1);
    pool.deallocate(item2_new);
    pool.deallocate(item3);
    pool.deallocate(item4);
    assert(pool.active_count() == 0);

    std::cout << "[PASS] test_slab_allocator_basic" << std::endl;
}

void test_slab_allocator_concurrency() {
    std::cout << "[RUN] test_slab_allocator_concurrency" << std::endl;
    constexpr size_t CAPACITY = 64;
    SlabAllocator<TestItem, CAPACITY> pool;

    std::atomic<bool> start_flag{false};
    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 1000;
    std::vector<Thread> threads(NUM_THREADS);

    struct ThreadContext {
        SlabAllocator<TestItem, CAPACITY>* p;
        std::atomic<bool>* flag;
        int thread_id;
    };

    std::vector<ThreadContext> contexts(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        contexts[t] = {&pool, &start_flag, t};
        threads[t].start([](void* arg) {
            auto* ctx = static_cast<ThreadContext*>(arg);
            while (!ctx->flag->load()) {
                Thread::yield();
            }
            for (int i = 0; i < ITERATIONS; ++i) {
                TestItem* item = ctx->p->allocate(ctx->thread_id * 1000 + i, "concurrent_test");
                if (item) {
                    assert(item->payload == "concurrent_test");
                    ctx->p->deallocate(item);
                }
            }
        }, &contexts[t]);
    }

    start_flag.store(true);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads[t].join();
    }

    assert(pool.active_count() == 0);
    std::cout << "[PASS] test_slab_allocator_concurrency" << std::endl;
}

void test_monotonic_arena_basic() {
    std::cout << "[RUN] test_monotonic_arena_basic" << std::endl;
    MonotonicArena arena;
    assert(arena.get_capacity() == 16 * 1024);
    assert(arena.get_offset() == 0);

    // Allocate 10 bytes with 4-byte alignment
    void* p1 = arena.allocate(10, 4);
    assert(p1 != nullptr);
    assert(reinterpret_cast<uintptr_t>(p1) % 4 == 0);
    assert(arena.get_offset() >= 10);

    // Allocate 15 bytes with 8-byte alignment
    void* p2 = arena.allocate(15, 8);
    assert(p2 != nullptr);
    assert(reinterpret_cast<uintptr_t>(p2) % 8 == 0);

    size_t offset_before_scope = arena.get_offset();
    {
        ArenaScope scope(arena);
        void* p3 = arena.allocate(256);
        assert(p3 != nullptr);
        assert(arena.get_offset() >= offset_before_scope + 256);
    }
    // ArenaScope should unwind to saved offset
    assert(arena.get_offset() == offset_before_scope);

    // Reset arena to 0
    arena.reset();
    assert(arena.get_offset() == 0);

    // Test capacity boundary trap
    void* too_big = arena.allocate(17 * 1024);
    assert(too_big == nullptr);

    std::cout << "[PASS] test_monotonic_arena_basic" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 Memory Architecture Tests ===" << std::endl;
    test_memory_manager_tiers();
    test_memory_manager_exhaustion();
    test_slab_allocator_basic();
    test_slab_allocator_concurrency();
    test_monotonic_arena_basic();
    std::cout << "=== All Memory Tests Passed Successfully ===" << std::endl;
    return 0;
}
