#pragma once

// HTTP over the serial console with concurrent, streamed responses: the board
// side of tools/kube32_serial_gateway.py.
//
// The gateway opens one stream per HTTP request. The relay connects to the
// request's port on this node (a worker: on the control plane), sends the
// request and writes the response back in frames as it arrives, so an event
// stream or a watch keeps flowing while other requests complete beside it.
// One thread serves every stream with select().
//
//   host:   K32_RESET\n                                 ends every stream
//   board:  \x1eK32_READY <slots> <role>\n
//   host:   K32_OPEN <id> <port> <len> <crc32>\n<len bytes: the HTTP request>
//   board:  \x1eK32_DATA <id> <len> <crc32>\n<len bytes of the response>
//   board:  \x1eK32_END <id> <0|1>\n                      1: the response was cut short
//   host:   K32_CLOSE <id>\n                              the client went away
//   board:  \x1eK32_ERR crc <id>\n                        the request's CRC did not match
//
// Board frames start with 0x1e and are written whole, so log output can
// surround a frame but never split it. CRCs are CRC-32 (zlib), in hex.

#include "osal/condition_variable.hpp"
#include "osal/mutex.hpp"
#include "osal/thread.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace kube32::app {

struct ConsoleRelayOptions {
    std::string host{"127.0.0.1"};      // where requests go: this node, or the control plane
    std::string role{"control-plane"};  // reported in K32_READY
    size_t slots{2};                    // concurrent streams; an open beyond them answers 503
    size_t chunk{1024};                 // response bytes per DATA frame: a frame must fit the console's TX buffer
    uint32_t connect_timeout_ms{2000};
    uint32_t idle_timeout_ms{300000};   // a stream whose server sends nothing for this long is cut
    size_t stack_size{6144};
    bool stack_in_psram{false};
};

class ConsoleRelay {
public:
    // Writes one frame whole, or nothing when the console cannot take it in
    // time (false). Called from the console task and the relay thread.
    using FrameWriter = std::function<bool(const char* data, size_t len)>;

    ConsoleRelay(ConsoleRelayOptions opts, FrameWriter write);
    ~ConsoleRelay();
    ConsoleRelay(const ConsoleRelay&) = delete;
    ConsoleRelay& operator=(const ConsoleRelay&) = delete;

    // The thread starts with the first call.
    void reset();
    // Starts stream `id` when `request` matches `crc`; answers K32_ERR crc otherwise.
    void open(uint32_t id, uint16_t port, std::string request, uint32_t crc);
    void close(uint32_t id);
    // Answers stream `id` with `status` ("503 Service Unavailable") without
    // opening a connection: for a request the node could not take in.
    void refuse(uint32_t id, const char* status);

    static uint32_t crc32(const void* data, size_t len);

private:
    enum class Kind { Reset, Open, Close };
    struct Command {
        Kind kind;
        uint32_t id;
        uint16_t port;
        std::string request;
    };
    struct Stream {
        uint32_t id;
        int fd;
        uint64_t last_ms;
    };

    static void entry(void* self);
    void push(Command c);
    void run();
    void handle(Command& c);
    void start_stream(uint32_t id, uint16_t port, const std::string& request);
    void end_stream(size_t index, bool cut);
    void drop_stream(size_t index);
    int connect_to(uint16_t port);
    bool data_frame(uint32_t id, const char* data, size_t len);
    void end_frame(uint32_t id, bool cut);
    void respond(uint32_t id, const char* status);
    bool send_frame(const char* head, size_t head_len, const char* data, size_t len);

    ConsoleRelayOptions opts_;
    FrameWriter write_;
    osal::Mutex mu_;
    osal::ConditionVariable cv_;
    std::deque<Command> inbox_;   // guarded by mu_
    bool stop_{false};            // guarded by mu_
    bool started_{false};         // guarded by mu_
    osal::Thread thread_;
    std::vector<Stream> streams_;  // relay thread only
    std::string frame_;            // relay thread only
};

}  // namespace kube32::app
