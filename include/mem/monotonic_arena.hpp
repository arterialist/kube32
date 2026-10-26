#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kube32::mem {

class MonotonicArena {
public:
    static constexpr size_t DEFAULT_CAPACITY = 16 * 1024; // 16 KB

    // Default constructor: manages its own internal 16KB buffer
    MonotonicArena();

    // Constructor with caller-supplied backing memory
    explicit MonotonicArena(uint8_t* backing_memory, size_t capacity = DEFAULT_CAPACITY);

    ~MonotonicArena() = default;

    MonotonicArena(const MonotonicArena&) = delete;
    MonotonicArena& operator=(const MonotonicArena&) = delete;
    MonotonicArena(MonotonicArena&&) noexcept;
    MonotonicArena& operator=(MonotonicArena&&) noexcept;

    // Bump allocation: aligned to 4 or 8 bytes
    void* allocate(size_t bytes, size_t alignment = 4);

    // O(1) Instant Reset
    void reset();

    size_t get_offset() const { return offset_; }
    void set_offset(size_t offset) {
        if (offset <= capacity_) {
            offset_ = offset;
        }
    }
    size_t get_capacity() const { return capacity_; }
    size_t get_high_water_mark() const { return high_water_mark_; }
    size_t get_free_bytes() const {
        return (offset_ < capacity_) ? (capacity_ - offset_) : 0;
    }

private:
    uint8_t* buffer_{nullptr};
    size_t capacity_{0};
    size_t offset_{0};
    size_t high_water_mark_{0};
    bool owns_buffer_{false};
    std::vector<uint8_t> internal_storage_;
};

// RAII Scope Helper for nested execution
class ArenaScope {
public:
    explicit ArenaScope(MonotonicArena& arena)
        : arena_(arena), saved_offset_(arena.get_offset()) {}
    ~ArenaScope() {
        arena_.set_offset(saved_offset_);
    }

    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    MonotonicArena& arena_;
    size_t saved_offset_;
};

} // namespace kube32::mem
