#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace kube32::mem {

// Text written into storage the caller owns (an array on its stack), for
// documents built over and over that must not take memory from the heap.
// What does not fit is dropped and reported by overflowed().
class FixedText {
public:
    FixedText(char* data, size_t capacity) : data_(data), cap_(capacity) {}
    template <size_t N>
    explicit FixedText(char (&array)[N]) : FixedText(array, N) {}

    FixedText(const FixedText&) = delete;
    FixedText& operator=(const FixedText&) = delete;

    void append(const char* text, size_t n) {
        if (n > cap_ - len_) {
            overflowed_ = true;
            return;
        }
        std::memcpy(data_ + len_, text, n);
        len_ += n;
    }
    FixedText& operator+=(std::string_view text) {
        append(text.data(), text.size());
        return *this;
    }
    FixedText& operator+=(const char* text) { return *this += std::string_view(text); }
    FixedText& operator+=(char c) {
        append(&c, 1);
        return *this;
    }
    void clear() {
        len_ = 0;
        overflowed_ = false;
    }

    std::string_view view() const { return std::string_view(data_, len_); }
    size_t size() const { return len_; }
    bool overflowed() const { return overflowed_; }

private:
    char* data_;
    size_t cap_;
    size_t len_{0};
    bool overflowed_{false};
};

} // namespace kube32::mem
