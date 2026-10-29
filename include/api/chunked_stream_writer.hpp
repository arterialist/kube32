#pragma once

#include "api/http_types.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string_view>

namespace kube32::api {

/**
 * @brief Output abstraction interface allowing mock unit testing of streaming writer.
 */
class ISink {
public:
    virtual ~ISink() = default;
    virtual bool write(const void* data, size_t len) = 0;
};

/**
 * @brief Production socket sink forwarding directly to osal::Socket::send_all.
 */
class SocketSink : public ISink {
public:
    explicit SocketSink(osal::Socket* socket = nullptr) : socket_(socket) {}
    bool write(const void* data, size_t len) override {
        return socket_ ? socket_->send_all(data, len) : false;
    }
private:
    osal::Socket* socket_{nullptr};
};

/**
 * @brief High-performance, zero-allocation HTTP chunked stream serializer.
 * 
 * Maintained entirely on a 512-byte stack buffer. Direct-to-socket streaming ensures
 * O(1) memory footprint regardless of response payload size.
 */
class ChunkedStreamWriter {
public:
    static constexpr size_t CHUNK_CAPACITY = 512;

    explicit ChunkedStreamWriter(osal::Socket& socket)
        : socket_sink_(&socket), sink_(&socket_sink_), pos_(0), finished_(false), has_error_(false) {}

    explicit ChunkedStreamWriter(ISink& sink)
        : socket_sink_(nullptr), sink_(&sink), pos_(0), finished_(false), has_error_(false) {}

    ~ChunkedStreamWriter() {
        if (!finished_ && !has_error_) {
            finish();
        }
    }

    // Non-copyable, non-movable
    ChunkedStreamWriter(const ChunkedStreamWriter&) = delete;
    ChunkedStreamWriter& operator=(const ChunkedStreamWriter&) = delete;
    ChunkedStreamWriter(ChunkedStreamWriter&&) = delete;
    ChunkedStreamWriter& operator=(ChunkedStreamWriter&&) = delete;

    /**
     * @brief Send standard HTTP 200 OK headers for chunked streaming.
     */
    bool send_headers(const char* content_type = "application/json", int status_code = 200) {
        if (has_error_) return false;
        char header_buf[256];
        const char* status_text = http_reason_phrase(status_code);
        int len = snprintf(header_buf, sizeof(header_buf),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "Connection: close\r\n\r\n",
                           status_code, status_text, content_type);
        if (len <= 0) {
            has_error_ = true;
            return false;
        }
        if (!sink_->write(header_buf, static_cast<size_t>(len))) {
            has_error_ = true;
            return false;
        }
        return true;
    }

    /**
     * @brief Stream raw bytes into the chunked buffer, flushing chunks as capacity is reached.
     */
    bool write(const char* data, size_t len) {
        if (has_error_ || finished_) return false;

        while (len > 0) {
            size_t space = CHUNK_CAPACITY - pos_;
            if (len < space) {
                std::memcpy(buffer_ + pos_, data, len);
                pos_ += len;
                return true;
            } else {
                std::memcpy(buffer_ + pos_, data, space);
                pos_ += space;
                data += space;
                len -= space;
                if (!flush_chunk()) {
                    return false;
                }
            }
        }
        return true;
    }

    bool write(const char* str) {
        if (!str) return true;
        return write(str, std::strlen(str));
    }

    bool write(std::string_view sv) {
        return write(sv.data(), sv.size());
    }

    bool write_char(char c) {
        return write(&c, 1);
    }

    bool write_int(int64_t val) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(val));
        if (len <= 0) return false;
        return write(buf, static_cast<size_t>(len));
    }

    bool write_uint(uint64_t val) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(val));
        if (len <= 0) return false;
        return write(buf, static_cast<size_t>(len));
    }

    /**
     * @brief Write a string formatted and JSON-escaped (handling quotes, backslashes, controls).
     */
    bool write_escaped_json(const char* str) {
        if (!str) return true;
        while (*str) {
            switch (*str) {
                case '"':  if (!write("\\\"", 2)) return false; break;
                case '\\': if (!write("\\\\", 2)) return false; break;
                case '\b': if (!write("\\b", 2)) return false; break;
                case '\f': if (!write("\\f", 2)) return false; break;
                case '\n': if (!write("\\n", 2)) return false; break;
                case '\r': if (!write("\\r", 2)) return false; break;
                case '\t': if (!write("\\t", 2)) return false; break;
                default: {
                    unsigned char uc = static_cast<unsigned char>(*str);
                    if (uc < 0x20) {
                        char ubuf[8];
                        snprintf(ubuf, sizeof(ubuf), "\\u%04x", uc);
                        if (!write(ubuf, 6)) return false;
                    } else {
                        if (!write_char(*str)) return false;
                    }
                    break;
                }
            }
            str++;
        }
        return true;
    }

    /**
     * @brief Flushes the accumulated buffer as an HTTP chunk and cooperatively yields.
     */
    bool flush_chunk() {
        if (has_error_) return false;
        if (pos_ == 0) return true;

        char hex_hdr[16];
        int hdr_len = snprintf(hex_hdr, sizeof(hex_hdr), "%X\r\n", static_cast<unsigned int>(pos_));
        if (hdr_len <= 0) {
            has_error_ = true;
            return false;
        }

        // 1. Send chunk length in hex + CRLF
        if (!sink_->write(hex_hdr, static_cast<size_t>(hdr_len))) {
            has_error_ = true;
            return false;
        }

        // 2. Send chunk data
        if (!sink_->write(buffer_, pos_)) {
            has_error_ = true;
            return false;
        }

        // 3. Send trailing CRLF
        if (!sink_->write("\r\n", 2)) {
            has_error_ = true;
            return false;
        }

        pos_ = 0;

        // Cooperative yield to reset FreeRTOS TWDT
        osal::Thread::yield();

        return true;
    }

    /**
     * @brief Concludes the chunked stream by flushing remaining data and sending 0\r\n\r\n.
     */
    bool finish() {
        if (finished_) return !has_error_;
        finished_ = true;

        if (pos_ > 0) {
            if (!flush_chunk()) return false;
        }

        // Send HTTP terminating chunk
        const char* terminal = "0\r\n\r\n";
        if (!sink_->write(terminal, 5)) {
            has_error_ = true;
            return false;
        }

        // Cooperative yield
        osal::Thread::yield();
        return true;
    }

    bool has_error() const { return has_error_; }
    size_t buffered_bytes() const { return pos_; }

private:
    SocketSink socket_sink_;
    ISink* sink_{nullptr};
    size_t pos_{0};
    bool finished_{false};
    bool has_error_{false};
    char buffer_[CHUNK_CAPACITY];
};

} // namespace kube32::api
