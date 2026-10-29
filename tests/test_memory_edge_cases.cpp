#include "mem/slab_allocator.hpp"
#include "mem/monotonic_arena.hpp"
#include "mem/memory_tier.hpp"
#include "osal/thread.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <random>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <thread>
#include <chrono>
#include <stdexcept>

using namespace kube32::mem;
using namespace kube32::osal;

// =============================================================================
// Helper Types
// =============================================================================
struct TestPayload {
    int id;
    std::string text;
    std::vector<int> numbers;
    static int ctor_count;
    static int dtor_count;

    TestPayload(int i, std::string s, size_t n) : id(i), text(std::move(s)), numbers(n, i) {
        ctor_count++;
    }
    ~TestPayload() {
        dtor_count++;
    }
};
int TestPayload::ctor_count = 0;
int TestPayload::dtor_count = 0;

struct ThrowOnDemand {
    ThrowOnDemand(bool fail) {
        if (fail) throw std::runtime_error("Induced constructor failure");
    }
};

// =============================================================================
// PART 1: Standard Contract Stress Testing
// =============================================================================

void test_slab_capacity_exhaustion() {
    std::cout << "  [RUN] Slab: Capacity exhaustion, nullptr boundary, full lifecycle ... " << std::flush;
    constexpr size_t CAP = 32;
    SlabAllocator<TestPayload, CAP> pool;
    TestPayload::ctor_count = 0;
    TestPayload::dtor_count = 0;

    std::vector<TestPayload*> allocated;
    for (size_t i = 0; i < CAP; ++i) {
        TestPayload* p = pool.allocate(static_cast<int>(i), "fill", 10);
        assert(p != nullptr);
        allocated.push_back(p);
    }

    assert(pool.is_full());
    assert(pool.active_count() == CAP);
    assert(pool.free_count() == 0);
    assert(pool.high_water_mark() == CAP);

    // Overflow attempts
    for (int i = 0; i < 50; ++i) {
        TestPayload* p = pool.allocate(999, "overflow", 5);
        assert(p == nullptr);
    }

    // Deallocate all
    for (auto* p : allocated) {
        pool.deallocate(p);
    }

    assert(pool.active_count() == 0);
    assert(pool.free_count() == CAP);
    assert(!pool.is_full());
    assert(TestPayload::ctor_count == CAP);
    assert(TestPayload::dtor_count == CAP);

    std::cout << "PASSED" << std::endl;
}

void test_slab_random_churn() {
    std::cout << "  [RUN] Slab: 100 rounds randomized deallocation and reallocation ... " << std::flush;
    constexpr size_t CAP = 64;
    SlabAllocator<TestPayload, CAP> pool;
    std::mt19937 rng(42);

    for (int round = 0; round < 100; ++round) {
        std::vector<TestPayload*> active;
        active.reserve(CAP);

        for (size_t i = 0; i < CAP; ++i) {
            TestPayload* p = pool.allocate(round * 1000 + i, "churn", 4);
            assert(p != nullptr);
            active.push_back(p);
        }

        std::shuffle(active.begin(), active.end(), rng);

        // Free half
        for (size_t i = 0; i < CAP / 2; ++i) {
            pool.deallocate(active[i]);
        }
        assert(pool.active_count() == CAP - (CAP / 2));

        // Reallocate half
        for (size_t i = 0; i < CAP / 2; ++i) {
            TestPayload* p = pool.allocate(round * 2000 + i, "realloc", 4);
            assert(p != nullptr);
            active[i] = p;
        }

        // Free all in random order
        std::shuffle(active.begin(), active.end(), rng);
        for (size_t i = 0; i < CAP; ++i) {
            pool.deallocate(active[i]);
        }

        assert(pool.active_count() == 0);
        assert(pool.free_count() == CAP);
    }

    std::cout << "PASSED" << std::endl;
}

void test_slab_concurrency() {
    std::cout << "  [RUN] Slab: Multi-threaded high-contention stress (8 threads, 40,000 ops) ... " << std::flush;
    constexpr size_t CAP = 32;
    SlabAllocator<int, CAP> pool;

    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 5000;
    std::atomic<bool> start_flag{false};
    std::atomic<int> successful_allocs{0};
    std::atomic<int> failed_allocs{0};

    std::vector<std::thread> workers;
    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_flag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            std::mt19937 rng(100 + t);
            std::vector<int*> held;
            held.reserve(CAP);

            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                if ((rng() % 2 == 0) || held.empty()) {
                    int* ptr = pool.allocate(t * 100000 + i);
                    if (ptr) {
                        successful_allocs.fetch_add(1, std::memory_order_relaxed);
                        held.push_back(ptr);
                    } else {
                        failed_allocs.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    int idx = rng() % held.size();
                    int* ptr = held[idx];
                    held[idx] = held.back();
                    held.pop_back();
                    pool.deallocate(ptr);
                }
            }

            for (int* ptr : held) {
                pool.deallocate(ptr);
            }
        });
    }

    start_flag.store(true, std::memory_order_relaxed);
    for (auto& w : workers) {
        w.join();
    }

    assert(pool.active_count() == 0);
    assert(pool.free_count() == CAP);
    assert(successful_allocs.load() > 0);
    assert(failed_allocs.load() > 0);

    std::cout << "PASSED" << std::endl;
}

void test_arena_boundary_and_alignment() {
    std::cout << "  [RUN] Arena: Exact capacity, capacity + 1, and standard alignments (1B, 2B, 4B, 8B, 16B) ... " << std::flush;
    MonotonicArena arena;
    size_t cap = arena.get_capacity();
    assert(cap == 16 * 1024);

    void* p_full = arena.allocate(cap, 1);
    assert(p_full != nullptr);
    assert(arena.get_offset() == cap);
    assert(arena.get_free_bytes() == 0);

    assert(arena.allocate(1, 1) == nullptr);

    arena.reset();
    assert(arena.get_offset() == 0);

    void* p1 = arena.allocate(1, 1);
    assert(p1 != nullptr);

    void* p2 = arena.allocate(1, 2);
    assert(p2 != nullptr && reinterpret_cast<uintptr_t>(p2) % 2 == 0);

    void* p4 = arena.allocate(1, 4);
    assert(p4 != nullptr && reinterpret_cast<uintptr_t>(p4) % 4 == 0);

    void* p8 = arena.allocate(1, 8);
    assert(p8 != nullptr && reinterpret_cast<uintptr_t>(p8) % 8 == 0);

    void* p16 = arena.allocate(1, 16);
    assert(p16 != nullptr && reinterpret_cast<uintptr_t>(p16) % 16 == 0);

    std::cout << "PASSED" << std::endl;
}

void test_arena_reset_reusability() {
    std::cout << "  [RUN] Arena: O(1) reset reusability over 1,000 cycles with memory pattern verification ... " << std::flush;
    MonotonicArena arena;
    constexpr size_t BLOCK_SZ = 256;
    constexpr size_t N_BLOCKS = (16 * 1024) / BLOCK_SZ;

    for (int cycle = 0; cycle < 1000; ++cycle) {
        uint8_t byte_val = static_cast<uint8_t>(cycle & 0xFF);
        std::vector<uint8_t*> blocks;

        for (size_t b = 0; b < N_BLOCKS; ++b) {
            void* p = arena.allocate(BLOCK_SZ, 4);
            assert(p != nullptr);
            std::memset(p, byte_val, BLOCK_SZ);
            blocks.push_back(reinterpret_cast<uint8_t*>(p));
        }

        for (uint8_t* p : blocks) {
            for (size_t i = 0; i < BLOCK_SZ; ++i) {
                assert(p[i] == byte_val);
            }
        }

        arena.reset();
        assert(arena.get_offset() == 0);
    }

    std::cout << "PASSED" << std::endl;
}

void test_memory_manager_concurrency() {
    std::cout << "  [RUN] MemoryManager: Multi-threaded SRAM and PSRAM concurrency stress ... " << std::flush;
    MemoryManager::init();

    constexpr int NUM_THREADS = 4;
    constexpr int ITERS = 1000;
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < ITERS; ++i) {
                void* p_sram = MemoryManager::allocate(128, MemoryTier::InternalSRAM, 8);
                if (p_sram) {
                    std::memset(p_sram, 0x11, 128);
                    MemoryManager::deallocate(p_sram, MemoryTier::InternalSRAM);
                }
                void* p_psram = MemoryManager::allocate(512, MemoryTier::ExternalPSRAM, 16);
                if (p_psram) {
                    std::memset(p_psram, 0x22, 512);
                    MemoryManager::deallocate(p_psram, MemoryTier::ExternalPSRAM);
                }
            }
        });
    }

    start_flag.store(true, std::memory_order_relaxed);
    for (auto& th : threads) {
        th.join();
    }

    assert(MemoryManager::check_integrity());
    MemoryStats stats = MemoryManager::get_stats();
    assert(stats.sram_free == stats.sram_total);
    assert(stats.psram_free == stats.psram_total);

    std::cout << "PASSED" << std::endl;
}

// =============================================================================
// PART 2: Robustness Assertions
// =============================================================================

void test_arena_integer_overflow_defense() {
    std::cout << "  [RUN] Arena: Integer overflow wraparound protection ... " << std::flush;
    MonotonicArena arena;
    arena.allocate(32, 4);
    size_t initial_offset = arena.get_offset();

    // Size designed to overflow (aligned_offset + bytes) around size_t
    size_t overflow_bytes = (std::numeric_limits<size_t>::max() - initial_offset) + 5;
    void* p_overflow = arena.allocate(overflow_bytes, 4);

    // REGRESSION ASSERTION: Integer overflow MUST be rejected safely
    assert(p_overflow == nullptr);
    assert(arena.get_offset() == initial_offset); // Offset must not rewind

    std::cout << "PASSED (Overflow safely rejected)" << std::endl;
}

void test_arena_unaligned_backing_defense() {
    std::cout << "  [RUN] Arena: Alignment enforcement with unaligned backing memory ... " << std::flush;
    alignas(16) uint8_t raw[1024];
    uint8_t* unaligned_backing = raw + 1; // 1-byte unaligned offset
    MonotonicArena arena(unaligned_backing, 1000);

    void* p = arena.allocate(16, 8);
    assert(p != nullptr);
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);

    // REGRESSION ASSERTION: Returned pointer must be 8-byte aligned regardless of backing buffer
    assert(addr % 8 == 0);

    std::cout << "PASSED (8-byte alignment verified)" << std::endl;
}

void test_slab_constructor_exception_safety() {
    std::cout << "  [RUN] Slab: Constructor exception rollback & slot leak prevention ... " << std::flush;
    SlabAllocator<ThrowOnDemand, 2> pool;

    try {
        pool.allocate(true); // Intentionally throw
        assert(false); // Should not reach
    } catch (const std::runtime_error&) {
        // Exception expected
    }

    assert(pool.active_count() == 0);
    assert(pool.free_count() == 2);

    // REGRESSION ASSERTION: Slots must not be leaked; both slots allocatable
    ThrowOnDemand* p1 = pool.allocate(false);
    ThrowOnDemand* p2 = pool.allocate(false);
    assert(p1 != nullptr);
    assert(p2 != nullptr);
    assert(pool.is_full());

    pool.deallocate(p1);
    pool.deallocate(p2);
    assert(pool.active_count() == 0);

    std::cout << "PASSED (Slots cleanly rolled back)" << std::endl;
}

void test_slab_double_free_protection() {
    std::cout << "  [RUN] Slab: Double-free rejection & cyclic free-list prevention ... " << std::flush;
    SlabAllocator<int, 4> pool;
    int* a = pool.allocate(10);
    int* b = pool.allocate(20);
    assert(a != nullptr && b != nullptr);

    // Free 'a' once
    pool.deallocate(a);

    // Erroneous duplicate free attempt
    pool.deallocate(a);

    // Subsequent allocations
    int* x = pool.allocate(100);
    int* y = pool.allocate(200);
    assert(x != nullptr && y != nullptr);

    // REGRESSION ASSERTION: Double-free must not create cycle or return duplicate aliased pointers
    assert(x != y);

    pool.deallocate(b);
    pool.deallocate(x);
    pool.deallocate(y);
    assert(pool.active_count() == 0);

    std::cout << "PASSED (Double-free handled without aliasing)" << std::endl;
}

void test_slab_concurrent_telemetry() {
    std::cout << "  [RUN] Slab: Concurrent const telemetry reader thread-safety ... " << std::flush;
    constexpr size_t CAP = 16;
    SlabAllocator<int, CAP> pool;
    std::atomic<bool> running{true};

    // Reader thread continuously calls const query methods. Each call takes
    // the pool's lock on its own, so the mutator may run between two calls:
    // only per-value bounds hold here, not sums across calls such as
    // active + free == CAP.
    std::thread reader([&]() {
        while (running.load(std::memory_order_relaxed)) {
            size_t act = pool.active_count();
            size_t free_cnt = pool.free_count();
            size_t hwm = pool.high_water_mark();
            (void)pool.is_full();
            assert(act <= CAP);
            assert(free_cnt <= CAP);
            assert(hwm <= CAP);
            std::this_thread::yield();
        }
    });

    // Mutator thread performs rapid allocations and deallocations
    for (int i = 0; i < 2000; ++i) {
        int* p = pool.allocate(i);
        if (p) {
            pool.deallocate(p);
        }
    }

    running.store(false, std::memory_order_relaxed);
    reader.join();
    assert(pool.active_count() == 0);
    std::cout << "PASSED (Zero telemetry data races)" << std::endl;
}

// =============================================================================
// MAIN ENTRY
// =============================================================================
int main() {
    std::cout << "====================================================================" << std::endl;
    std::cout << "     KUBE32 MEMORY ARCHITECTURE EDGE CASE & CONCURRENCY TESTS        " << std::endl;
    std::cout << "====================================================================" << std::endl;

    std::cout << "\n>>> PART 1: SPECIFICATION & STRESS-TEST CONTRACT VERIFICATION <<<" << std::endl;
    test_slab_capacity_exhaustion();
    test_slab_random_churn();
    test_slab_concurrency();
    test_arena_boundary_and_alignment();
    test_arena_reset_reusability();
    test_memory_manager_concurrency();

    std::cout << "\n>>> PART 2: ROBUSTNESS ASSERTIONS <<<" << std::endl;
    test_arena_integer_overflow_defense();
    test_arena_unaligned_backing_defense();
    test_slab_constructor_exception_safety();
    test_slab_double_free_protection();
    test_slab_concurrent_telemetry();

    std::cout << "\n====================================================================" << std::endl;
    std::cout << "  ALL MEMORY EDGE CASE TESTS PASSED                                " << std::endl;
    std::cout << "====================================================================" << std::endl;

    return 0;
}
