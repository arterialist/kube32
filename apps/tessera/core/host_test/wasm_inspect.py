#!/usr/bin/env python3
"""Lists imports, exports and memory of a .wasm file without external tools."""
import sys, struct

def leb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return r, i

def name(b, i):
    n, i = leb(b, i); return b[i:i+n].decode(), i + n

def main(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'\0asm', 'not a wasm file'
    i = 8; imports, exports, mem = [], [], None
    while i < len(b):
        sid = b[i]; i += 1
        size, i = leb(b, i); end = i + size; p = i
        if sid == 2:
            n, p = leb(b, p)
            for _ in range(n):
                mod, p = name(b, p); nm, p = name(b, p); kind = b[p]; p += 1
                if kind == 0: _, p = leb(b, p)
                elif kind == 1: p += 1; _, p = leb(b, p)
                elif kind == 2:
                    fl = b[p]; p += 1; _, p = leb(b, p)
                    if fl & 1: _, p = leb(b, p)
                elif kind == 3: p += 2
                imports.append((mod, nm, kind))
        elif sid == 5:
            n, p = leb(b, p)
            if n:
                fl = b[p]; p += 1; mn, p = leb(b, p); mx = None
                if fl & 1: mx, p = leb(b, p)
                mem = (mn, mx)
        elif sid == 7:
            n, p = leb(b, p)
            for _ in range(n):
                nm, p = name(b, p); kind = b[p]; p += 1; _, p = leb(b, p)
                exports.append((nm, kind))
        i = end
    kinds = {0: 'func', 1: 'table', 2: 'memory', 3: 'global'}
    print(f'{path}: {len(b)} bytes')
    print(f'imports ({len(imports)}):')
    for mod, nm, k in imports: print(f'  {mod}.{nm} [{kinds[k]}]')
    if mem: print(f'memory: initial {mem[0]} pages ({mem[0]*65536} bytes), max {mem[1]} pages' + (f' ({mem[1]*65536} bytes)' if mem[1] else ''))
    print(f'exports ({len(exports)}):')
    for nm, k in exports: print(f'  {nm} [{kinds[k]}]')
    bad = [m for m, _, _ in imports if m != 'env']
    if bad: print('ERROR: imports from non-env modules:', bad); sys.exit(1)

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build/tessera-core.wasm')
