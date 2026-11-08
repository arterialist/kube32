// The console's output ring: whole writes or none while a host reads, the
// newest whole lines kept for the next host while none does.

#include "hal/console_ring.hpp"

#include <cassert>
#include <iostream>
#include <string>

using kube32::hal::ConsoleRing;

static std::string drain(ConsoleRing& r) {
    std::string out;
    for (;;) {
        const char* p = nullptr;
        size_t n = r.peek(&p);
        if (n == 0) break;
        out.append(p, n);
        r.consume(n);
    }
    return out;
}

static std::string lost_line(ConsoleRing& r) {
    char buf[96];
    size_t n = r.lost_line(buf, sizeof buf);
    return std::string(buf, n);
}

// A host reading: output comes out in order, across the wrap
static void test_queue() {
    char mem[16];
    ConsoleRing r(mem, sizeof mem);
    assert(r.append("hello\n", 6));
    assert(r.size() == 6);
    assert(drain(r) == "hello\n");
    assert(r.size() == 0);
    assert(r.append("0123456789", 10));
    assert(drain(r) == "0123456789");
    assert(r.append("abcdefgh\n", 9));   // wraps
    assert(drain(r) == "abcdefgh\n");
}

// A write that does not fit is refused whole: a frame is never cut
static void test_frames_all_or_nothing() {
    char mem[8];
    ConsoleRing r(mem, sizeof mem);
    assert(r.append("aaaaaa", 6));
    assert(!r.append("bbb", 3));
    assert(r.size() == 6);
    assert(r.lost_bytes() == 0);
    assert(r.append("bb", 2));
    assert(drain(r) == "aaaaaabb");
}

// No host: the oldest whole lines make room, and the next host starts at a
// line, after a line that says what was lost
static void test_replay_starts_at_a_line() {
    char mem[32];
    ConsoleRing r(mem, sizeof mem);
    r.append_dropping_oldest("first line\n", 11);
    r.append_dropping_oldest("second line\n", 12);
    r.append_dropping_oldest("third line, longer\n", 19);   // 42 bytes in all: "first line\n" goes, whole
    assert(lost_line(r) == "[CONSOLE] 11 bytes of output lost while no host had the port open\n");
    assert(lost_line(r).empty());   // reported once
    assert(drain(r) == "second line\nthird line, longer\n");
}

// A partial line at the end stays and is finished by the next write
static void test_partial_tail_survives() {
    char mem[32];
    ConsoleRing r(mem, sizeof mem);
    r.append_dropping_oldest("line one\n", 9);
    r.append_dropping_oldest("half a ", 7);
    r.append_dropping_oldest("line\n", 5);
    assert(r.lost_bytes() == 0);
    assert(drain(r) == "line one\nhalf a line\n");
}

// A single line longer than the ring keeps its tail
static void test_line_longer_than_ring() {
    char mem[8];
    ConsoleRing r(mem, sizeof mem);
    std::string big(20, 'x');
    big += "END";
    r.append_dropping_oldest(big.data(), big.size());
    assert(r.size() == sizeof mem);
    assert(r.lost_bytes() == 15);
    assert(drain(r) == "xxxxxEND");
    // and a shorter one after it never reads the stale bytes
    r.append_dropping_oldest("ok\n", 3);
    assert(drain(r) == "ok\n");
    assert(lost_line(r) == "[CONSOLE] 15 bytes of output lost while no host had the port open\n");
}

// Oldest dropped when a whole-line trim would still be needed: the ring ends
// empty and later output comes out alone, also across the wrap
static void test_trim_across_wrap() {
    char mem[16];
    ConsoleRing r(mem, sizeof mem);
    assert(r.append("0123456789", 10));
    assert(drain(r) == "0123456789");   // head at 10
    r.append_dropping_oldest("aaa\nbbb\n", 8);    // wraps at 16
    r.append_dropping_oldest("cccccc\n", 7);      // 15 bytes: fits
    r.append_dropping_oldest("d\n", 2);           // the "aaa\n" line goes
    assert(lost_line(r) == "[CONSOLE] 4 bytes of output lost while no host had the port open\n");
    assert(drain(r) == "bbb\ncccccc\nd\n");
}

int main() {
    test_queue();
    test_frames_all_or_nothing();
    test_replay_starts_at_a_line();
    test_partial_tail_survives();
    test_line_longer_than_ring();
    test_trim_across_wrap();
    std::cout << "console ring ok" << std::endl;
    return 0;
}
