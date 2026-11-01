#!/usr/bin/env python3
"""Writes tests/data/snapshot-probe.wasm, the request pod the snapshot tests
move between nodes. Its state covers what a snapshot carries: mutable globals
of every value type, a table changed with table.set and table.grow, memory
grown with memory.grow, a channel, events and a clock.

Memory: [0] requests, [4] events, [8] last event length, [12] channel handle,
[16] k32_restored calls, [20] event order hash, [24] descriptor of open_file.

k32_handle counts the request, runs setup(1) for a path starting with /s,
spins for a path starting with /w, answers the count as text and clears the
request block it was given. peek(addr) reads a word. check() returns bits for the state setup made:
0x7FF with memory grown, 0x67F without.

    python3 tests/data/snapshot_probe.py
"""
import os
import struct


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def sleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(s):
    return uleb(len(s)) + s.encode()


def section(sid, payload):
    return bytes([sid]) + uleb(len(payload)) + payload


I32, I64, F32, F64, FUNCREF = 0x7F, 0x7E, 0x7D, 0x7C, 0x70


def functype(params, results):
    return bytes([0x60]) + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])


def i32c(v):
    return b"\x41" + sleb(v)


def load(off=0):
    return b"\x28\x02" + uleb(off)


def store():
    return b"\x36\x02\x00"


def call(f):
    return b"\x10" + uleb(f)


def get(i):
    return b"\x20" + uleb(i)


def set_(i):
    return b"\x21" + uleb(i)


def tee(i):
    return b"\x22" + uleb(i)


def gget(i):
    return b"\x23" + uleb(i)


def gset(i):
    return b"\x24" + uleb(i)


END, DROP = b"\x0b", b"\x1a"
I32_EQ, I32_NE, I32_EQZ, I32_GEU = b"\x46", b"\x47", b"\x45", b"\x4f"
I32_ADD, I32_SUB, I32_MUL, I32_DIVU, I32_REMU, I32_OR, I32_SHL = b"\x6a", b"\x6b", b"\x6c", b"\x6e", b"\x70", b"\x72", b"\x74"


def incr(addr):
    return i32c(addr) + i32c(addr) + load() + i32c(1) + I32_ADD + store()


def body(locals_, code):
    groups = [uleb(n) + bytes([t]) for n, t in locals_]
    b = vec(groups) + code + END
    return uleb(len(b)) + b


def bit(n):
    return i32c(n) + I32_SHL + I32_OR


# Types
T_TIME, T_II_I, T_III_V, T_V_V, T_III_I, T_I_I, T_IIII_I, T_V_I = range(8)
types = [functype([], [I64]), functype([I32, I32], [I32]), functype([I32, I32, I32], []), functype([], []),
         functype([I32, I32, I32], [I32]), functype([I32], [I32]), functype([I32, I32, I32, I32], [I32]), functype([], [I32])]

imports = [("k32_time_ms", T_TIME), ("kube32_channel_open", T_II_I), ("k32_resp_begin", T_III_V), ("k32_resp_write", T_II_I),
           ("k32_resp_end", T_V_V), ("k32_fs_open", T_III_I)]
TIME_MS, CHANNEL_OPEN, RESP_BEGIN, RESP_WRITE, RESP_END, FS_OPEN = range(6)
ALLOC, HANDLE, EVENT, RESTORED, SETUP, CHECK, FA, FB, TIME, OPEN_FILE, PEEK = range(6, 17)

CHANNEL_NAME, CTYPE, NOTES, NUMBER_END, REQUEST_AT = 256, 300, 320, 524, 1024

# k32_alloc(size): the request block always goes to REQUEST_AT
f_alloc = (T_I_I, body([], i32c(REQUEST_AT)))

# k32_handle(ptr, len); locals: 2 n, 3 p
handle = incr(0)
handle += get(0) + b"\x2d\x00\x05" + i32c(ord("s")) + I32_EQ + b"\x04\x40" + i32c(1) + call(SETUP) + DROP + END
handle += get(0) + b"\x2d\x00\x05" + i32c(ord("w")) + I32_EQ + b"\x04\x40"
handle += i32c(100000000) + set_(2) + b"\x03\x40" + get(2) + i32c(1) + I32_SUB + tee(2) + b"\x0d\x00" + END + END
handle += i32c(0) + load() + set_(2) + i32c(NUMBER_END) + set_(3)
handle += b"\x03\x40" + get(3) + i32c(1) + I32_SUB + tee(3) + get(2) + i32c(10) + I32_REMU + i32c(48) + I32_ADD + b"\x3a\x00\x00"
handle += get(2) + i32c(10) + I32_DIVU + tee(2) + b"\x0d\x00" + END
handle += i32c(200) + i32c(CTYPE) + i32c(16) + call(RESP_BEGIN)
handle += get(3) + i32c(NUMBER_END) + get(3) + I32_SUB + call(RESP_WRITE) + DROP + call(RESP_END)
handle += get(0) + i32c(0) + get(1) + b"\xfc\x0b\x00"   # memory.fill: the request leaves no trace
handle += i32c(200)
f_handle = (T_II_I, body([(2, I32)], handle))

# k32_event(topic, topic_len, data, len)
event = incr(4) + i32c(8) + get(3) + store()
event += i32c(20) + i32c(20) + load() + i32c(31) + I32_MUL + get(2) + b"\x2d\x00\x00" + I32_ADD + store() + i32c(0)
f_event = (T_IIII_I, body([], event))

f_restored = (T_V_I, body([], incr(16) + i32c(0)))

# setup(grow)
setup = i32c(7) + gset(0)
setup += b"\x42" + sleb(0x1122334455667788) + gset(1)
setup += b"\x43" + struct.pack("<f", 1.5) + gset(2)
setup += b"\x44" + struct.pack("<d", 2.25) + gset(3)
setup += b"\xd2" + uleb(FB) + gset(4)
setup += i32c(2) + b"\xd2" + uleb(FB) + b"\x26\x00"                 # table.set 0
setup += b"\xd2" + uleb(FA) + i32c(1) + b"\xfc\x0f\x00" + DROP       # table.grow 0
setup += i32c(12) + i32c(CHANNEL_NAME) + i32c(0) + call(CHANNEL_OPEN) + store()
setup += get(0) + b"\x04\x40" + i32c(1) + b"\x40\x00" + i32c(-1) + I32_NE
setup += b"\x04\x40" + i32c(65636) + i32c(0xABCDEF) + store() + END + END
setup += i32c(0)
f_setup = (T_I_I, body([], setup))

# check(): one bit per piece of state setup made
check = gget(0) + i32c(7) + I32_EQ
check += gget(1) + b"\x42" + sleb(0x1122334455667788) + b"\x51" + bit(1)
check += gget(2) + b"\x43" + struct.pack("<f", 1.5) + b"\x5b" + bit(2)
check += gget(3) + b"\x44" + struct.pack("<d", 2.25) + b"\x61" + bit(3)
check += i32c(2) + b"\x11" + uleb(T_V_I) + b"\x00" + i32c(43) + I32_EQ + bit(4)
check += i32c(4) + b"\x11" + uleb(T_V_I) + b"\x00" + i32c(42) + I32_EQ + bit(5)
check += b"\xfc\x10\x00" + i32c(5) + I32_EQ + bit(6)
check += b"\x3f\x00" + i32c(2) + I32_EQ + bit(7)
check += b"\x3f\x00" + i32c(2) + I32_GEU + b"\x04\x7f" + i32c(65636) + load() + i32c(0xABCDEF) + I32_EQ + b"\x05" + i32c(0) + END + bit(8)
check += gget(4) + b"\xd1" + I32_EQZ + bit(9)
check += i32c(0) + b"\x11" + uleb(T_V_I) + b"\x00" + i32c(42) + I32_EQ + bit(10)
f_check = (T_V_I, body([], check))

f_fa = (T_V_I, body([], i32c(42)))
f_fb = (T_V_I, body([], i32c(43)))
f_time = (T_V_I, body([], call(TIME_MS) + b"\xa7"))
f_open = (T_V_I, body([], i32c(24) + i32c(NOTES) + i32c(9) + i32c(0x42) + call(FS_OPEN) + store() + i32c(24) + load()))

f_peek = (T_I_I, body([], get(0) + load()))

defined = [f_alloc, f_handle, f_event, f_restored, f_setup, f_check, f_fa, f_fb, f_time, f_open, f_peek]
exports = [("k32_alloc", ALLOC), ("k32_handle", HANDLE), ("k32_event", EVENT), ("k32_restored", RESTORED), ("setup", SETUP),
           ("check", CHECK), ("time", TIME), ("open_file", OPEN_FILE), ("peek", PEEK)]

globals_ = [
    bytes([I32, 1]) + i32c(0) + END,
    bytes([I64, 1]) + b"\x42\x00" + END,
    bytes([F32, 1]) + b"\x43" + struct.pack("<f", 0.0) + END,
    bytes([F64, 1]) + b"\x44" + struct.pack("<d", 0.0) + END,
    bytes([FUNCREF, 1]) + b"\xd0\x70" + END,
    bytes([I32, 0]) + i32c(99) + END,
]
data = [(CHANNEL_NAME, b"pod://snapshot-probe/in\x00"), (CTYPE, b"application/json"), (NOTES, b"notes.txt")]

module = b"\x00asm\x01\x00\x00\x00"
module += section(1, vec(types))
module += section(2, vec([name("env") + name(n) + b"\x00" + uleb(t) for n, t in imports]))
module += section(3, vec([uleb(t) for t, _ in defined]))
module += section(4, vec([bytes([FUNCREF, 0x01]) + uleb(4) + uleb(8)]))
module += section(5, vec([b"\x01" + uleb(1) + uleb(4)]))
module += section(6, vec(globals_))
module += section(7, vec([name(n) + b"\x00" + uleb(i) for n, i in exports]))
module += section(9, vec([b"\x00" + i32c(0) + END + vec([uleb(FA), uleb(FB)])]))
module += section(10, vec([b for _, b in defined]))
module += section(11, vec([b"\x00" + i32c(at) + END + uleb(len(d)) + d for at, d in data]))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "snapshot-probe.wasm")
with open(out, "wb") as f:
    f.write(module)
print(out, len(module), "bytes")