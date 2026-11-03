#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

static void* s_block = nullptr;
static size_t s_block_size = 0;
static std::atomic<bool> s_in_use{false};
static bool s_external = false;
static const size_t k_threshold = 48 * 1024;
static void* s_stack = nullptr;
static size_t s_stack_size = 0;
static std::atomic<bool> s_stack_in_use{false};

// Per-pod accounts: bytes each pod holds, charged to the thread's current
// account when allocated and to the allocation's owner when freed
namespace {
#if defined(ESP_PLATFORM)
constexpr int kAccounts = 64;
#else
constexpr int kAccounts = 512;   // the simulator runs several nodes' pods in one process
#endif
enum : uint8_t { kAccountFree = 0, kAccountOpen = 1, kAccountClosed = 2 };
std::atomic<size_t> s_account_bytes[kAccounts + 1];
std::atomic<uint8_t> s_account_state[kAccounts + 1];
thread_local int t_account = 0;

void charge(int account, size_t bytes) {
    if (account > 0 && account <= kAccounts) s_account_bytes[account].fetch_add(bytes);
}
void uncharge(int account, size_t bytes) {
    if (account > 0 && account <= kAccounts) s_account_bytes[account].fetch_sub(bytes);
}
// While alive, allocations on this thread go to `account`
struct ChargeTo {
    int previous;
    explicit ChargeTo(int account) : previous(t_account) { t_account = account; }
    ~ChargeTo() { t_account = previous; }
};
} // namespace

// The block's two uses can share it: one large linear memory (a pod whose
// memory wants most of the block) at its start, and pod allocations the
// general heap cannot place (it keeps a reserve for the system, and after
// Wi-Fi it has no large blocks) as a small heap of its own, placed from the
// end down so the start stays free for a large memory as long as possible.
// The small heap is gone again once its last allocation is freed.
namespace {
struct SubChunk {
    uint32_t size;   // whole chunk, header included, multiple of 8
    uint32_t used;   // 0 when free, else the owner's account + 1
};
constexpr size_t kSubAlign = 8;
constexpr size_t kMinChunk = 4 * sizeof(SubChunk);
std::mutex s_sub_mutex;
bool s_sub_active = false;
size_t s_sub_allocs = 0;
uint8_t* s_sub_base = nullptr;    // start of the small heap (above a large memory)
size_t s_whole_bytes = 0;         // the large memory's size while one is placed
int s_whole_owner = 0;            // ... and the account it is charged to

size_t align8(size_t n) { return (n + kSubAlign - 1) & ~(kSubAlign - 1); }
uint8_t* sub_end() { return static_cast<uint8_t*>(s_block) + (s_block_size & ~(kSubAlign - 1)); }
SubChunk* sub_first() { return reinterpret_cast<SubChunk*>(s_sub_base); }
SubChunk* sub_next(SubChunk* c) { return reinterpret_cast<SubChunk*>(reinterpret_cast<uint8_t*>(c) + c->size); }
bool sub_in_range(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(s_block);
    return s_block && p > b && p < b + s_block_size;
}

void sub_coalesce() {
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        while (!c->used) {
            SubChunk* n = sub_next(c);
            if (reinterpret_cast<uint8_t*>(n) >= sub_end() || n->used) break;
            c->size += n->size;
        }
    }
}

// Caller holds s_sub_mutex
void* sub_alloc_locked(size_t bytes) {
    if (!s_block || bytes == 0 || bytes > s_block_size) return nullptr;
    if (!s_sub_active) {
        s_sub_base = static_cast<uint8_t*>(s_block) + (s_in_use.load() ? align8(s_whole_bytes) : 0);
        if (sub_end() - s_sub_base < static_cast<ptrdiff_t>(kMinChunk)) return nullptr;
        SubChunk* c = sub_first();
        c->size = static_cast<uint32_t>(sub_end() - s_sub_base);
        c->used = 0;
        s_sub_active = true;
        s_sub_allocs = 0;
    }
    const size_t need = align8(bytes) + sizeof(SubChunk);
    // The smallest free chunk that fits (the highest of equal ones),
    // allocated from its top: large holes stay whole for large allocations,
    // so a pod that replaces another fits where that one was, and the bottom
    // of the block stays free for a large linear memory
    SubChunk* fit = nullptr;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        if (!c->used && c->size >= need && (!fit || c->size <= fit->size)) fit = c;
    }
    if (!fit) return nullptr;
    SubChunk* out = fit;
    if (fit->size - need >= kMinChunk) {
        fit->size = static_cast<uint32_t>(fit->size - need);
        out = sub_next(fit);
        out->size = static_cast<uint32_t>(need);
    }
    out->used = static_cast<uint32_t>(t_account) + 1;
    charge(t_account, out->size);
    s_sub_allocs++;
    return out + 1;
}

void sub_free_locked(void* p) {
    SubChunk* c = static_cast<SubChunk*>(p) - 1;
    uncharge(static_cast<int>(c->used) - 1, c->size);
    c->used = 0;
    if (--s_sub_allocs == 0) s_sub_active = false;
    else sub_coalesce();
}

// Caller holds s_sub_mutex. The chunk keeps `bytes` and its start; the rest
// becomes a free chunk when it is at least a minimal one.
bool sub_shrink_locked(void* p, size_t bytes) {
    SubChunk* c = static_cast<SubChunk*>(p) - 1;
    const size_t need = align8(bytes) + sizeof(SubChunk);
    if (c->size < need + kMinChunk) return false;
    SubChunk* tail = reinterpret_cast<SubChunk*>(reinterpret_cast<uint8_t*>(c) + need);
    tail->size = static_cast<uint32_t>(c->size - need);
    tail->used = 0;
    uncharge(static_cast<int>(c->used) - 1, tail->size);
    c->size = static_cast<uint32_t>(need);
    sub_coalesce();
    return true;
}
} // namespace

extern "C" {

void* kube32_pool_sub_alloc(size_t bytes) {
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    void* p = sub_alloc_locked(bytes);
    if (p) std::memset(p, 0, bytes);
    return p;
}

int kube32_pool_sub_free(void* ptr) {
    if (!ptr || !sub_in_range(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active || static_cast<uint8_t*>(ptr) <= s_sub_base) return 0;
    sub_free_locked(ptr);
    return 1;
}

int kube32_rt_shrink(void* ptr, size_t bytes) {
    if (!ptr || !sub_in_range(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active || static_cast<uint8_t*>(ptr) <= s_sub_base) return 0;
    return sub_shrink_locked(ptr, bytes) ? 1 : 0;
}

size_t kube32_pool_whole_capacity(void) {
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    return s_sub_active ? static_cast<size_t>(s_sub_base - static_cast<uint8_t*>(s_block)) : s_block_size;
}

void kube32_pool_reserve(size_t bytes) {
    if (s_block || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_block = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_block = std::malloc(bytes);
#endif
    s_block_size = s_block ? bytes : 0;
}

void kube32_pool_reserve_external(size_t bytes) {
    if (s_block || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_block = std::malloc(bytes);
#endif
    s_block_size = s_block ? bytes : 0;
    s_external = (s_block != nullptr);
}

size_t kube32_pool_available(int* out_external) {
    if (out_external) *out_external = s_external ? 1 : 0;
    if (!s_block) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active) return s_in_use.load() ? s_block_size - align8(s_whole_bytes) : s_block_size;
    size_t free_bytes = 0;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        if (!c->used) free_bytes += c->size - sizeof(SubChunk);
    }
    return free_bytes;
}

size_t kube32_pool_threshold(void) { return k_threshold; }

int kube32_pool_wants_block(size_t memory_bytes) {
    return (s_block && memory_bytes >= k_threshold && memory_bytes >= s_block_size / 2) ? 1 : 0;
}

void kube32_pool_reserve_stack(size_t bytes) {
    if (s_stack || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_stack = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_stack = std::malloc(bytes);
#endif
    s_stack_size = s_stack ? bytes : 0;
}

void* kube32_pool_stack_alloc(size_t bytes) {
    if (!s_stack || bytes > s_stack_size) return nullptr;
    bool expected = false;
    if (!s_stack_in_use.compare_exchange_strong(expected, true)) return nullptr;
    return s_stack;
}

int kube32_pool_stack_free(void* ptr) {
    if (!ptr || ptr != s_stack) return 0;
    s_stack_in_use.store(false);
    return 1;
}

size_t kube32_pool_stack_size(void) { return s_stack_size; }
size_t kube32_pool_block_size(void) { return s_block_size; }

// The block is sized for the node's largest pod: a request under half of it
// is served by the general allocator so a small pod started first does not
// take the block from the pod it was reserved for.
static bool block_takes(size_t bytes) {
    return s_block && bytes >= k_threshold && bytes >= s_block_size / 2 && bytes <= s_block_size;
}

void* kube32_pool_alloc(size_t bytes) {
    if (!block_takes(bytes)) return nullptr;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (s_in_use.load()) return nullptr;
    if (s_sub_active) {
        // The small heap lives at the top: the memory fits below it when the
        // bottom chunk is free and large enough
        SubChunk* first = sub_first();
        const size_t take = align8(bytes);
        if (s_sub_base != s_block || first->used || first->size < take + kMinChunk) return nullptr;
        const size_t rest = first->size - take;
        s_sub_base = static_cast<uint8_t*>(s_block) + take;
        SubChunk* moved = sub_first();
        moved->size = static_cast<uint32_t>(rest);
        moved->used = 0;
    }
    s_in_use.store(true);
    s_whole_bytes = bytes;
    s_whole_owner = t_account;
    charge(s_whole_owner, align8(bytes));
    std::memset(s_block, 0, bytes);
    return s_block;
}

int kube32_pool_resize(void* ptr, size_t bytes) {
    if (!kube32_pool_owns(ptr) || !s_in_use.load()) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    const size_t capacity = s_sub_active ? static_cast<size_t>(s_sub_base - static_cast<uint8_t*>(s_block)) : s_block_size;
    if (bytes > capacity) return 0;
    uncharge(s_whole_owner, align8(s_whole_bytes));
    charge(s_whole_owner, align8(bytes));
    s_whole_bytes = bytes;
    return 1;
}

// Where pod allocations (runtime, compiled code, linear memory) go:
//   the pool's block first, as a small heap from its end down, then
//   with PSRAM: the PSRAM above the system's reserve, never internal RAM,
//     which stays for Wi-Fi, lwIP and the control plane
//   without PSRAM: the general heap above the system reserve
//   A pod whose linear memory wants the block whole asks for the rest
//   first while it loads (kube32_pool_prefer_general).
// Bytes pods hold in the general heap and in PSRAM are counted. The general
// heap keeps a reserve for the system that pods are never given: 16 KiB of
// internal RAM stay free for the kubelet, Wi-Fi and lwIP, 24 KiB on a board
// without PSRAM; the host simulator models a board's heap with a budget
// instead (kube32_pool_set_heap_budget).
static std::atomic<int> s_prefer_general{0};
static std::atomic<size_t> s_general_bytes{0};
static std::atomic<size_t> s_psram_bytes{0};
static std::atomic<size_t> s_general_allocs{0};
static size_t s_heap_budget = 0;                    // 0: not modelled
static const size_t k_system_reserve_default = 16 * 1024;
// PSRAM pods are never given: thread stacks made later, and on a control
// plane the API server, the object store, the console relay and firmware
// staging (kube32_pool_set_psram_reserve)
static std::atomic<size_t> s_psram_reserve{16 * 1024};
static std::atomic<bool> s_psram_reserve_set{false};

// A pod allocation in the general heap or PSRAM starts with the account it
// is charged to; the pointer handed out follows, 8-byte aligned as in the
// block's small heap
namespace {
struct alignas(8) GeneralHeader {
    int32_t account;
};
static_assert(sizeof(GeneralHeader) == 8, "the header keeps 8-byte alignment");
constexpr size_t kHeader = sizeof(GeneralHeader);
GeneralHeader* header_of(void* p) { return static_cast<GeneralHeader*>(p) - 1; }
} // namespace

static size_t general_size(const void* p) {
#if defined(ESP_PLATFORM)
    return heap_caps_get_allocated_size(const_cast<void*>(p));
#elif defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size(const_cast<void*>(p));
#endif
}

// Counts an allocation just made (`base`, header first) as the current
// account's and returns what the pod gets
static void* adopt(void* base, bool psram) {
    GeneralHeader* h = static_cast<GeneralHeader*>(base);
    h->account = t_account;
    const size_t size = general_size(base);
    (psram ? s_psram_bytes : s_general_bytes).fetch_add(size);
    s_general_allocs.fetch_add(1);
    charge(t_account, size);
    return h + 1;
}

// Internal RAM pods leave to the system. A board without PSRAM keeps more:
// its general heap also absorbs the network's bursts (lwIP send buffers and
// Wi-Fi frames, measured near 25 KB while a pod streams a large response)
static size_t system_reserve() {
#if !defined(ESP_PLATFORM)
    // The simulator models the system's use of its heap budget with the
    // reserve it is given (kube32-sim --system-reserved)
    if (s_psram_reserve_set.load()) return s_psram_reserve.load();
#endif
    if (s_block && !s_external) return 24 * 1024;
    return k_system_reserve_default;
}

// Whether `bytes` more of the general heap leave the system reserve free
static bool general_has_room(size_t bytes) {
    if (s_heap_budget) return s_general_bytes.load() + bytes + system_reserve() <= s_heap_budget;
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + system_reserve();
#else
    return true;
#endif
}

static void* general_calloc(size_t bytes) {
    if (!general_has_room(bytes + kHeader)) return nullptr;
#if defined(ESP_PLATFORM)
    void* base = heap_caps_calloc(bytes + kHeader, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* base = std::calloc(bytes + kHeader, 1);
#endif
    return base ? adopt(base, false) : nullptr;
}

static bool pool_first() { return s_block && !s_prefer_general.load(); }

// A board with PSRAM gives pods PSRAM only
static bool board_psram() {
#if defined(ESP_PLATFORM)
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false;
#endif
}

// PSRAM above the reserve, which pods may take besides the block
static size_t psram_above_reserve() {
#if defined(ESP_PLATFORM)
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return psram > s_psram_reserve.load() ? psram - s_psram_reserve.load() : 0;
#else
    return 0;
#endif
}

void kube32_pool_prefer_general(int on) { s_prefer_general.fetch_add(on ? 1 : -1); }
void kube32_pool_set_heap_budget(size_t bytes) { s_heap_budget = bytes; }
size_t kube32_pool_heap_budget(void) { return s_heap_budget; }
size_t kube32_pool_general_bytes(void) { return s_general_bytes.load(); }
size_t kube32_pool_general_allocations(void) { return s_general_allocs.load(); }
size_t kube32_pool_system_reserve(void) { return system_reserve(); }
void kube32_pool_set_psram_reserve(size_t bytes) {
    s_psram_reserve.store(bytes);
    s_psram_reserve_set.store(true);
}
size_t kube32_pool_psram_reserve(void) { return s_psram_reserve.load(); }
size_t kube32_pool_reserved_for_system(void) {
#if defined(ESP_PLATFORM)
    return board_psram() ? s_psram_reserve.load() : system_reserve();
#else
    return system_reserve();
#endif
}

int kube32_pod_account_open(void) {
    for (int i = 1; i <= kAccounts; ++i) {
        uint8_t expected = kAccountFree;
        if (s_account_state[i].compare_exchange_strong(expected, kAccountOpen)) return i;
    }
    // A closed account is free again once its last allocation is freed
    for (int i = 1; i <= kAccounts; ++i) {
        uint8_t expected = kAccountClosed;
        if (s_account_bytes[i].load() == 0 && s_account_state[i].compare_exchange_strong(expected, kAccountOpen)) return i;
    }
    return 0;
}

void kube32_pod_account_close(int account) {
    if (account <= 0 || account > kAccounts) return;
    s_account_state[account].store(s_account_bytes[account].load() == 0 ? kAccountFree : kAccountClosed);
}

size_t kube32_pod_account_bytes(int account) {
    return (account > 0 && account <= kAccounts) ? s_account_bytes[account].load() : 0;
}

int kube32_pod_account_enter(int account) {
    const int previous = t_account;
    t_account = (account > 0 && account <= kAccounts) ? account : 0;
    return previous;
}

void kube32_pod_account_leave(int previous) { t_account = previous; }

// What the system's own use leaves pods of PSRAM besides the block (or of
// the internal heap on a board with neither, or of the host's memory),
// measured the first time it is asked for with what pods hold then added
// back. The reserve covers the system's growth after that.
static std::mutex s_besides_mutex;
static bool s_besides_measured = false;
static size_t s_besides = 0;

static size_t besides_block() {
    std::lock_guard<std::mutex> lock(s_besides_mutex);
    if (!s_besides_measured) {
#if defined(ESP_PLATFORM)
        if (board_psram()) {
            s_besides = psram_above_reserve() + s_psram_bytes.load();
        } else {
            const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            s_besides = (internal > system_reserve() ? internal - system_reserve() : 0) + s_general_bytes.load();
        }
#else
        s_besides = kube32::osal::System::get_free_heap_bytes() + s_general_bytes.load();
#endif
        s_besides_measured = true;
    }
    return s_besides;
}

size_t kube32_pool_pod_room(size_t memory_bytes, size_t* out_capacity) {
    size_t room = 0;
    if (s_heap_budget) {
        // The simulator's --psram 0 also counts the modelled heap, unlike the
        // board: its 64-bit wasm3 structures are larger than the board's
        const size_t used = s_general_bytes.load() + system_reserve();
        room = s_heap_budget > used ? s_heap_budget - used : 0;
    } else {
#if defined(ESP_PLATFORM)
        // With PSRAM a pod gets PSRAM above the reserve. Without, it is
        // admitted on the pool block alone: general heap that looks free now
        // is what the network needs in a burst
        if (board_psram()) {
            room = psram_above_reserve();
        } else if (!s_block) {
            const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            room = internal > system_reserve() ? internal - system_reserve() : 0;
        }
#else
        room = kube32::osal::System::get_free_heap_bytes();   // the host's available memory
#endif
    }
    // The block is not part of the heap figures above; pods share it
    (void)memory_bytes;
    if (s_block) room += kube32_pool_available(nullptr);
    if (out_capacity) kube32_pool_node_room(out_capacity, nullptr);
    return room;
}

void kube32_pool_node_room(size_t* out_memory, size_t* out_psram) {
    size_t memory = 0, psram = 0;
#if defined(ESP_PLATFORM)
    if (board_psram()) {
        // The block and the PSRAM above the reserve; pods get nothing else
        memory = s_block_size + besides_block();
        psram = memory;
    } else if (s_block) {
        memory = s_block_size;   // admission counts the block alone
    } else {
        memory = besides_block();
    }
#else
    // The simulator's boards: its PSRAM is the block (--psram) and, with a
    // heap budget, the PSRAM besides it that pods' allocations come from;
    // without PSRAM the block and the budget, as kube32_pool_pod_room counts
    // them. Without a budget, the host's memory besides the block.
    if (s_heap_budget) {
        memory = (s_heap_budget > system_reserve() ? s_heap_budget - system_reserve() : 0) + s_block_size;
        if (s_external) psram = memory;
    } else {
        memory = besides_block() + s_block_size;
        if (s_external) psram = s_block_size;
    }
#endif
    if (out_memory) *out_memory = memory;
    if (out_psram) *out_psram = psram;
}

size_t kube32_pool_linear_max(void) {
    size_t memory = 0;
    kube32_pool_node_room(&memory, nullptr);
    const size_t besides = memory > s_block_size ? memory - s_block_size : 0;
    return s_block_size > besides ? s_block_size : besides;
}

static bool in_psram(const void* p) {
#if defined(ESP_PLATFORM)
    return esp_ptr_external_ram(p);
#else
    (void)p;
    return false;
#endif
}

// The size of the last allocation a pod was refused, for load errors
static std::atomic<size_t> s_last_refused{0};

size_t kube32_pool_last_refused(void) { return s_last_refused.load(); }

size_t kube32_pool_chunks(void (*each)(size_t offset, size_t size, int used, void* ctx), void* ctx) {
    if (!s_block) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active) return 0;
    size_t n = 0;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c), ++n) {
        each(static_cast<size_t>(reinterpret_cast<uint8_t*>(c) - static_cast<uint8_t*>(s_block)), c->size, c->used ? 1 : 0, ctx);
    }
    return n;
}

size_t kube32_pool_largest_free(void) {
    if (!s_block) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active) return s_in_use.load() ? s_block_size - align8(s_whole_bytes) : s_block_size;
    size_t largest = 0;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        if (!c->used && c->size - sizeof(SubChunk) > largest) largest = c->size - sizeof(SubChunk);
    }
    return largest;
}

size_t kube32_rt_fit(size_t bytes, size_t minimum) {
    // Where the pool block comes first but has no piece of `bytes` left: its
    // largest piece, when that holds `minimum`, so a code page stays in the
    // block in a smaller size instead of going to the general heap
    if (!pool_first() || bytes <= minimum) return bytes;
    const size_t largest = kube32_pool_largest_free() & ~(kSubAlign - 1);
    return (largest < bytes && largest >= minimum) ? largest : bytes;
}

void* kube32_rt_calloc(size_t bytes) {
    if (pool_first()) {
        void* shared = kube32_pool_sub_alloc(bytes);
        if (shared) return shared;
    }
#if defined(ESP_PLATFORM)
    if (board_psram()) {
        void* p = nullptr;
        if (psram_above_reserve() > bytes + kHeader) {
            void* base = heap_caps_calloc(bytes + kHeader, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (base) p = adopt(base, true);
        }
        if (!p) p = kube32_pool_sub_alloc(bytes);
        if (!p) s_last_refused.store(bytes);
        return p;
    }
#endif
    void* p = general_calloc(bytes);
    if (!p) p = kube32_pool_sub_alloc(bytes);
    if (!p) s_last_refused.store(bytes);
    return p;
}

static bool in_sub_heap(const void* ptr) {
    return sub_in_range(ptr) && s_sub_active && static_cast<const uint8_t*>(ptr) > s_sub_base;
}

int kube32_rt_owner(const void* ptr) {
    if (!ptr) return 0;
    if (kube32_pool_owns(ptr)) return s_whole_owner;
    if (sub_in_range(ptr)) {
        std::lock_guard<std::mutex> lock(s_sub_mutex);
        if (in_sub_heap(ptr)) return static_cast<int>((static_cast<const SubChunk*>(ptr) - 1)->used) - 1;
    }
    return (static_cast<const GeneralHeader*>(ptr) - 1)->account;
}

void kube32_rt_set_owner(void* ptr, int account) {
    if (!ptr) return;
    if (account < 0 || account > kAccounts) account = 0;
    if (kube32_pool_owns(ptr)) {
        std::lock_guard<std::mutex> lock(s_sub_mutex);
        uncharge(s_whole_owner, align8(s_whole_bytes));
        s_whole_owner = account;
        charge(account, align8(s_whole_bytes));
        return;
    }
    if (sub_in_range(ptr)) {
        std::lock_guard<std::mutex> lock(s_sub_mutex);
        if (in_sub_heap(ptr)) {
            SubChunk* c = static_cast<SubChunk*>(ptr) - 1;
            uncharge(static_cast<int>(c->used) - 1, c->size);
            c->used = static_cast<uint32_t>(account) + 1;
            charge(account, c->size);
            return;
        }
    }
    GeneralHeader* h = header_of(ptr);
    const size_t size = general_size(h);
    uncharge(h->account, size);
    h->account = account;
    charge(account, size);
}

void* kube32_rt_realloc(void* ptr, size_t bytes) {
    if (!ptr) return kube32_rt_calloc(bytes);
    // What replaces an allocation is its owner's
    ChargeTo owner(kube32_rt_owner(ptr));
    if (in_sub_heap(ptr)) {
        // Within the block's own heap: in place when the chunk is large
        // enough, else moved
        SubChunk* c = static_cast<SubChunk*>(ptr) - 1;
        const size_t have = c->size - sizeof(SubChunk);
        if (bytes <= have) return ptr;
        void* moved = kube32_rt_calloc(bytes);
        if (!moved) return nullptr;
        std::memcpy(moved, ptr, have);
        kube32_pool_sub_free(ptr);
        return moved;
    }
    GeneralHeader* h = header_of(ptr);
    const int account = h->account;
    const size_t old = general_size(h);
    const size_t have = old - kHeader;
    if (in_psram(h)) {
        // Grown in place in PSRAM, or moved to wherever kube32_rt_calloc finds room
        void* base = nullptr;
#if defined(ESP_PLATFORM)
        if (bytes + kHeader <= old || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > (bytes + kHeader - old) + s_psram_reserve.load()) {
            base = heap_caps_realloc(h, bytes + kHeader, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
#endif
        if (base) {
            const size_t now = general_size(base);
            s_psram_bytes.fetch_sub(old);
            s_psram_bytes.fetch_add(now);
            uncharge(account, old);
            charge(account, now);
            return static_cast<GeneralHeader*>(base) + 1;
        }
        void* moved = kube32_rt_calloc(bytes);
        if (!moved) return nullptr;
        std::memcpy(moved, ptr, have < bytes ? have : bytes);
        kube32_rt_free(ptr);
        return moved;
    }
    if (bytes + kHeader > old && !general_has_room(bytes + kHeader - old)) return nullptr;
#if defined(ESP_PLATFORM)
    void* base = heap_caps_realloc(h, bytes + kHeader, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* base = std::realloc(h, bytes + kHeader);
#endif
    if (!base) return nullptr;
    const size_t now = general_size(base);
    s_general_bytes.fetch_sub(old);
    s_general_bytes.fetch_add(now);
    uncharge(account, old);
    charge(account, now);
    return static_cast<GeneralHeader*>(base) + 1;
}

void kube32_rt_free(void* ptr) {
    if (!ptr || kube32_pool_sub_free(ptr)) return;
    GeneralHeader* h = header_of(ptr);
    const size_t size = general_size(h);
    (in_psram(h) ? s_psram_bytes : s_general_bytes).fetch_sub(size);
    s_general_allocs.fetch_sub(1);
    uncharge(h->account, size);
#if defined(ESP_PLATFORM)
    heap_caps_free(h);
#else
    std::free(h);
#endif
}

static uint8_t* s_interp_stack = nullptr;
static size_t s_interp_size = 0;

void kube32_interp_stack_set(void* stack, size_t bytes) {
    s_interp_stack = static_cast<uint8_t*>(stack);
    s_interp_size = stack ? bytes : 0;
}

int kube32_interp_stack_active(void) { return s_interp_stack ? 1 : 0; }

void* kube32_interp_stack_get(size_t* out_bytes) {
    if (out_bytes) *out_bytes = s_interp_size;
    return s_interp_stack;
}

void* kube32_interp_stack_base_for(const void* sp) {
    const uint8_t* p = static_cast<const uint8_t*>(sp);
    return (s_interp_stack && p >= s_interp_stack && p < s_interp_stack + s_interp_size) ? s_interp_stack : nullptr;
}

int kube32_pool_owns(const void* ptr) {
    return (s_block && ptr == s_block) ? 1 : 0;
}

int kube32_pool_free(void* ptr) {
    if (!kube32_pool_owns(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (s_sub_active && s_sub_base != s_block) {
        // The space the memory held joins the small heap's bottom chunk
        SubChunk* first = sub_first();
        const size_t gained = static_cast<size_t>(s_sub_base - static_cast<uint8_t*>(s_block));
        SubChunk* bottom = static_cast<SubChunk*>(s_block);
        bottom->size = static_cast<uint32_t>(gained + (first->used ? 0 : first->size));
        bottom->used = 0;
        if (first->used) bottom->size = static_cast<uint32_t>(gained);
        s_sub_base = static_cast<uint8_t*>(s_block);
    }
    uncharge(s_whole_owner, align8(s_whole_bytes));
    s_whole_bytes = 0;
    s_whole_owner = 0;
    s_in_use.store(false);
    return 1;
}

}
