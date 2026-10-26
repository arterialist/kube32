#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <type_traits>
#include <bitset>
#include "osal/mutex.hpp"

namespace kube32::mem {

template <typename T, size_t Capacity>
class SlabAllocator {
    static_assert(Capacity > 0 && Capacity <= 32767, "Capacity must be within [1, 32767]");

public:
    union Slot {
        alignas(alignof(T)) uint8_t storage[sizeof(T)];
        int16_t next_free;
    };

    SlabAllocator() : free_head_(0), active_count_(0), high_water_mark_(0), occupied_{0} {
        for (size_t i = 0; i < Capacity - 1; ++i) {
            slots_[i].next_free = static_cast<int16_t>(i + 1);
        }
        slots_[Capacity - 1].next_free = -1;
    }

    ~SlabAllocator() = default;

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) {
        osal::LockGuard<osal::Mutex> lock(mutex_);
        if (free_head_ == -1) {
            return nullptr; // Pool exhausted
        }

        int16_t idx = free_head_;
        free_head_ = slots_[idx].next_free;
        active_count_++;

        T* obj_ptr = reinterpret_cast<T*>(slots_[idx].storage);
        try {
            T* instance = new (obj_ptr) T(std::forward<Args>(args)...);
            occupied_.set(static_cast<size_t>(idx));
            if (active_count_ > high_water_mark_) {
                high_water_mark_ = active_count_;
            }
            return instance;
        } catch (...) {
            slots_[idx].next_free = free_head_;
            free_head_ = idx;
            active_count_--;
            throw;
        }
    }

    void deallocate(T* ptr) {
        if (!ptr) return;

        uint8_t* byte_ptr = reinterpret_cast<uint8_t*>(ptr);
        uint8_t* base_ptr = reinterpret_cast<uint8_t*>(&slots_[0]);
        uint8_t* end_ptr = reinterpret_cast<uint8_t*>(&slots_[Capacity - 1]);

        if (byte_ptr < base_ptr || byte_ptr > end_ptr) {
            return; // Pointer does not belong to this slab pool
        }

        size_t offset = static_cast<size_t>(byte_ptr - base_ptr);
        if (offset % sizeof(Slot) != 0) {
            return; // Misaligned slot address
        }

        int16_t idx = static_cast<int16_t>(offset / sizeof(Slot));

        osal::LockGuard<osal::Mutex> lock(mutex_);
        if (!occupied_.test(static_cast<size_t>(idx))) {
            return; // Double-free or unallocated slot rejected!
        }

        occupied_.reset(static_cast<size_t>(idx));
        ptr->~T();

        slots_[idx].next_free = free_head_;
        free_head_ = idx;
        if (active_count_ > 0) {
            active_count_--;
        }
    }

    size_t capacity() const { return Capacity; }
    size_t active_count() const {
        osal::LockGuard<osal::Mutex> lock(mutex_);
        return active_count_;
    }
    size_t free_count() const {
        osal::LockGuard<osal::Mutex> lock(mutex_);
        return Capacity - active_count_;
    }
    size_t high_water_mark() const {
        osal::LockGuard<osal::Mutex> lock(mutex_);
        return high_water_mark_;
    }
    bool is_full() const {
        osal::LockGuard<osal::Mutex> lock(mutex_);
        return free_head_ == -1;
    }

private:
    Slot slots_[Capacity];
    int16_t free_head_;
    size_t active_count_;
    size_t high_water_mark_;
    std::bitset<Capacity> occupied_{0};
    mutable osal::Mutex mutex_;
};

} // namespace kube32::mem
