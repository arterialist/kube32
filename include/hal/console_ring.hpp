#pragma once

// Console output waiting for a host, in a caller-provided buffer.
//
// While a host has the port open, output queues here and goes out in order;
// whole writes are kept or refused, so a frame is never cut. While no host
// has the port open, the oldest complete lines make room for new output, and
// the next host starts at the beginning of a line, after a line that says
// how much was lost. Not thread safe: the caller holds its lock.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace kube32::hal {

class ConsoleRing {
public:
    ConsoleRing(char* storage, size_t capacity) : data_(storage), capacity_(capacity) {}

    size_t capacity() const { return capacity_; }
    size_t size() const { return size_; }

    // Output lost to make room since the last lost_line()
    uint32_t lost_bytes() const { return lost_; }

    // Adds all `len` bytes, or none and returns false: nothing is cut.
    bool append(const char* data, size_t len) {
        if (len > capacity_ - size_) return false;
        put(data, len);
        return true;
    }

    // Adds the newest `len` bytes, dropping the oldest output to make room.
    // After a drop the ring starts at the first byte after a newline, so a
    // host never starts in the middle of a line. A line longer than the whole
    // ring loses its head instead.
    void append_dropping_oldest(const char* data, size_t len) {
        if (len >= capacity_) {
            // Only the tail of this write fits: everything older goes too
            lost_ += static_cast<uint32_t>(size_ + len - capacity_);
            head_ = 0;
            size_ = 0;
            put(data + (len - capacity_), capacity_);
            align_to_line();
            return;
        }
        if (len > capacity_ - size_) {
            drop(len - (capacity_ - size_));
            align_to_line();
        }
        put(data, len);
    }

    // The next contiguous bytes to send; 0 when empty.
    size_t peek(const char** out) const {
        if (size_ == 0) {
            *out = nullptr;
            return 0;
        }
        *out = data_ + head_;
        size_t span = capacity_ - head_;
        return span < size_ ? span : size_;
    }

    void consume(size_t n) {
        if (n > size_) n = size_;
        head_ = (head_ + n) % capacity_;
        size_ -= n;
    }

    // Writes the line that announces lost output into `out` and clears the
    // count; 0 when nothing was lost since the last call.
    size_t lost_line(char* out, size_t cap) {
        if (lost_ == 0) return 0;
        int n = std::snprintf(out, cap, "[CONSOLE] %u bytes of output lost while no host had the port open\n", static_cast<unsigned>(lost_));
        lost_ = 0;
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

private:
    void put(const char* data, size_t len) {
        size_t at = (head_ + size_) % capacity_;
        size_t first = capacity_ - at < len ? capacity_ - at : len;
        std::memcpy(data_ + at, data, first);
        std::memcpy(data_, data + first, len - first);
        size_ += len;
    }

    void drop(size_t n) {
        head_ = (head_ + n) % capacity_;
        size_ -= n;
        lost_ += static_cast<uint32_t>(n);
    }

    // Moves the head past the next newline, losing the partial line it cut
    void align_to_line() {
        for (size_t i = 0; i < size_; ++i) {
            if (data_[(head_ + i) % capacity_] == '\n') {
                drop(i + 1);
                return;
            }
        }
        // No newline at all: one line longer than the ring, keep its tail
    }

    char* data_;
    size_t capacity_;
    size_t head_{0};
    size_t size_{0};
    uint32_t lost_{0};
};

}  // namespace kube32::hal
