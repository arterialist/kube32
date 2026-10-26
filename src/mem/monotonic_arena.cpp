#include "mem/monotonic_arena.hpp"
#include <algorithm>
#include <utility>

namespace kube32::mem {

MonotonicArena::MonotonicArena()
    : capacity_(DEFAULT_CAPACITY),
      offset_(0),
      high_water_mark_(0),
      owns_buffer_(true),
      internal_storage_(DEFAULT_CAPACITY, 0) {
    buffer_ = internal_storage_.data();
}

MonotonicArena::MonotonicArena(uint8_t* backing_memory, size_t capacity)
    : buffer_(backing_memory),
      capacity_(capacity),
      offset_(0),
      high_water_mark_(0),
      owns_buffer_(false) {}

MonotonicArena::MonotonicArena(MonotonicArena&& other) noexcept
    : buffer_(other.buffer_),
      capacity_(other.capacity_),
      offset_(other.offset_),
      high_water_mark_(other.high_water_mark_),
      owns_buffer_(other.owns_buffer_),
      internal_storage_(std::move(other.internal_storage_)) {
    if (owns_buffer_) {
        buffer_ = internal_storage_.data();
    }
    other.buffer_ = nullptr;
    other.capacity_ = 0;
    other.offset_ = 0;
    other.high_water_mark_ = 0;
    other.owns_buffer_ = false;
}

MonotonicArena& MonotonicArena::operator=(MonotonicArena&& other) noexcept {
    if (this != &other) {
        internal_storage_ = std::move(other.internal_storage_);
        owns_buffer_ = other.owns_buffer_;
        capacity_ = other.capacity_;
        offset_ = other.offset_;
        high_water_mark_ = other.high_water_mark_;
        if (owns_buffer_) {
            buffer_ = internal_storage_.data();
        } else {
            buffer_ = other.buffer_;
        }
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.offset_ = 0;
        other.high_water_mark_ = 0;
        other.owns_buffer_ = false;
    }
    return *this;
}

void* MonotonicArena::allocate(size_t bytes, size_t alignment) {
    if (bytes == 0 || !buffer_) return nullptr;

    if (alignment == 0) alignment = 4;
    // Align offset
    size_t aligned_offset = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned_offset + bytes > capacity_) {
        return nullptr; // Arena capacity exceeded
    }

    void* ptr = buffer_ + aligned_offset;
    offset_ = aligned_offset + bytes;
    if (offset_ > high_water_mark_) {
        high_water_mark_ = offset_;
    }
    return ptr;
}

void MonotonicArena::reset() {
    offset_ = 0;
}

} // namespace kube32::mem
