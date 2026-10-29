#!/usr/bin/env python3
"""Lists the imports, exports and memory of a .wasm file and checks its
linear-memory layout without external tools.

  wasm_inspect.py module.wasm
  wasm_inspect.py module.wasm --map build/x.map --cap 16384 --stack 2048 --heap-min 4608

With --map (the wasm-ld -Map output) it reports the layout the linker chose:
the stack region [0, stack) (stack-first), data and bss, and __heap_base.
It also computes the deepest shadow-stack use from the code: every frame a
function reserves from __stack_pointer, summed along the deepest call chain
(indirect calls count as calls to every function in the table). The check
fails (exit 1) when that depth exceeds --stack, when __heap_base is past
--cap, or when less than --heap-min bytes remain between them.
"""
import argparse
import re
import sys


def leb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if not c & 0x80:
            return r, i


def sleb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if not c & 0x80:
            if c & 0x40:
                r -= 1 << s
            return r, i


def name(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode(), i + n


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'\0asm', 'not a wasm file'
    m = {'size': len(b), 'imports': [], 'exports': [], 'mem': None, 'nimport_funcs': 0,
         'bodies': [], 'elem': set(), 'names': {}, 'globals': []}
    i = 8
    while i < len(b):
        sid = b[i]; i += 1
        size, i = leb(b, i); end = i + size; p = i
        if sid == 0:
            nm, q = name(b, p)
            if nm == 'name':
                while q < end:
                    sub = b[q]; q += 1
                    ssz, q = leb(b, q); send = q + ssz
                    if sub == 1:
                        n, q = leb(b, q)
                        for _ in range(n):
                            idx, q = leb(b, q); fn, q = name(b, q)
                            m['names'][idx] = fn
                    q = send
        elif sid == 2:
            n, p = leb(b, p)
            for _ in range(n):
                mod, p = name(b, p); nm, p = name(b, p); kind = b[p]; p += 1
                if kind == 0:
                    _, p = leb(b, p); m['nimport_funcs'] += 1
                elif kind == 1:
                    p += 1; fl, p = leb(b, p); _, p = leb(b, p)
                    if fl & 1: _, p = leb(b, p)
                elif kind == 2:
                    fl = b[p]; p += 1; _, p = leb(b, p)
                    if fl & 1: _, p = leb(b, p)
                elif kind == 3:
                    p += 2
                m['imports'].append((mod, nm, kind))
        elif sid == 5:
            n, p = leb(b, p)
            if n:
                fl = b[p]; p += 1; mn, p = leb(b, p); mx = None
                if fl & 1: mx, p = leb(b, p)
                m['mem'] = (mn, mx)
        elif sid == 6:
            n, p = leb(b, p)
            for _ in range(n):
                p += 2
                op = b[p]; p += 1
                v = None
                if op == 0x41: v, p = sleb(b, p)
                elif op == 0x42: v, p = sleb(b, p)
                p += 1  # end
                m['globals'].append(v)
        elif sid == 7:
            n, p = leb(b, p)
            for _ in range(n):
                nm, p = name(b, p); kind = b[p]; p += 1; _, p = leb(b, p)
                m['exports'].append((nm, kind))
        elif sid == 9:
            n, p = leb(b, p)
            for _ in range(n):
                fl, p = leb(b, p)
                if fl == 0:
                    _, p = sleb(b, p + 1); p += 1   # i32.const offset end
                    k, p = leb(b, p)
                    for _ in range(k):
                        f, p = leb(b, p); m['elem'].add(f)
                else:
                    break
        elif sid == 10:
            n, p = leb(b, p)
            for _ in range(n):
                sz, p = leb(b, p)
                m['bodies'].append(b[p:p + sz])
                p += sz
        i = end
    return m


BLOCKTYPES = {0x40, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x70, 0x6f}


def scan_body(body):
    """Returns (frame bytes taken from __stack_pointer, direct callees, has indirect call)."""
    i = 0
    n, i = leb(body, i)
    for _ in range(n):
        _, i = leb(body, i); i += 1
    calls, indirect, frame = set(), False, 0
    window = []   # last three decoded instructions for the prologue pattern
    while i < len(body):
        op = body[i]; i += 1
        imm = None
        if op in (0x02, 0x03, 0x04):
            if body[i] in BLOCKTYPES: i += 1
            else: _, i = sleb(body, i)
        elif op in (0x0c, 0x0d): _, i = leb(body, i)
        elif op == 0x0e:
            k, i = leb(body, i)
            for _ in range(k + 1): _, i = leb(body, i)
        elif op == 0x10:
            imm, i = leb(body, i); calls.add(imm)
        elif op == 0x11:
            _, i = leb(body, i); _, i = leb(body, i); indirect = True
        elif op == 0x1c:
            k, i = leb(body, i); i += k
        elif 0x20 <= op <= 0x26: imm, i = leb(body, i)
        elif 0x28 <= op <= 0x3e: _, i = leb(body, i); _, i = leb(body, i)
        elif op in (0x3f, 0x40): i += 1
        elif op == 0x41: imm, i = sleb(body, i)
        elif op == 0x42: imm, i = sleb(body, i)
        elif op == 0x43: i += 4
        elif op == 0x44: i += 8
        elif op == 0xd0: i += 1
        elif op == 0xd2: _, i = leb(body, i)
        elif op == 0xfc:
            sub, i = leb(body, i)
            if sub == 8: _, i = leb(body, i); i += 1
            elif sub == 9: _, i = leb(body, i)
            elif sub == 10: i += 2
            elif sub == 11: i += 1
            elif sub in (12, 14): _, i = leb(body, i); _, i = leb(body, i)
            elif sub in (13, 15, 16, 17): _, i = leb(body, i)
        window = (window + [(op, imm)])[-3:]
        if not frame and len(window) == 3 and window[0] == (0x23, 0) and window[1][0] == 0x41 and window[2][0] == 0x6b:
            frame = window[1][1]
    return frame, calls, indirect


def stack_depth(m):
    nimp = m['nimport_funcs']
    info = {}
    for k, body in enumerate(m['bodies']):
        info[nimp + k] = scan_body(body)
    memo, active = {}, set()

    def depth(f):
        if f < nimp: return 0, [], False
        if f in memo: return memo[f]
        if f in active: return 0, [f], True
        active.add(f)
        frame, calls, indirect = info[f]
        targets = set(calls) | (m['elem'] if indirect else set())
        best, path, rec = 0, [], False
        for t in targets:
            d, pth, r = depth(t)
            rec = rec or r
            if d > best: best, path = d, pth
        active.discard(f)
        memo[f] = (frame + best, [f] + path, rec)
        return memo[f]

    roots = [nimp + k for k in range(len(m['bodies']))]
    worst = (0, [], False)
    recursive = False
    for r in roots:
        d = depth(r)
        recursive = recursive or d[2]
        if d[0] > worst[0]: worst = d
    names = [m['names'].get(f, 'func%d' % f) for f in worst[1]]
    return worst[0], names, recursive


def map_layout(path):
    """(stack bytes, end of data+bss) from a wasm-ld map file."""
    stack = None; end = 0
    for line in open(path):
        mm = re.match(r'\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\.\S+)\s*$', line)
        if mm:
            addr, size = int(mm.group(1), 16), int(mm.group(3), 16)
            if stack is None or addr < stack: stack = addr
            end = max(end, addr + size)
    return stack or 0, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wasm', nargs='?', default='build/tessera-api.wasm')
    ap.add_argument('--map')
    ap.add_argument('--cap', type=int)
    ap.add_argument('--stack', type=int)
    ap.add_argument('--heap-min', type=int, default=0)
    a = ap.parse_args()
    m = parse(a.wasm)
    kinds = {0: 'func', 1: 'table', 2: 'memory', 3: 'global'}
    print(f'{a.wasm}: {m["size"]} bytes')
    print(f'imports ({len(m["imports"])}):')
    for mod, nm, k in m['imports']: print(f'  {mod}.{nm} [{kinds[k]}]')
    mem = m['mem']
    if mem: print(f'memory: initial {mem[0]} pages ({mem[0]*65536} bytes), max {mem[1]} pages' + (f' ({mem[1]*65536} bytes)' if mem[1] else ''))
    print(f'exports ({len(m["exports"])}):')
    for nm, k in m['exports']: print(f'  {nm} [{kinds[k]}]')
    bad = [mod for mod, _, _ in m['imports'] if mod != 'env']
    if bad: print('ERROR: imports from non-env modules:', bad); sys.exit(1)
    depth, path, recursive = stack_depth(m)
    print(f'shadow stack: deepest chain uses {depth} bytes ({" > ".join(path)})' + (' [recursion: bound not proven]' if recursive else ''))
    if not a.map: return
    stack, end = map_layout(a.map)
    heap_base = (end + 15) & ~15
    print(f'layout: stack [0, {stack}), data+bss [{stack}, {end}), __heap_base {heap_base}')
    ok = True
    if a.stack is not None and (depth > a.stack or recursive):
        print(f'ERROR: shadow stack needs {depth} bytes, the stack is {a.stack}'); ok = False
    if a.cap is not None:
        room = a.cap - heap_base
        print(f'cap {a.cap}: {room} bytes from __heap_base to the cap (need {a.heap_min})')
        if room < a.heap_min:
            print('ERROR: the layout does not fit under the cap'); ok = False
    if not ok: sys.exit(1)


if __name__ == '__main__':
    main()
