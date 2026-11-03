#!/usr/bin/env python3
"""Splits models/mdlm/mdlm.bin for the llm-diffusion pod.

  pack.py mdlm.bin tok512.bin blob.bin tables.h

blob.bin is mdlm.bin without its rotary table: the matrices, scales and norm
vectors the node reads from flash (13 of the flash store's 64 KiB blocks, one
less than the whole file takes). tables.h holds the rotary table and the
tokenizer for the module's data."""
import struct
import sys


def main():
    model = open(sys.argv[1], "rb").read()
    tok = open(sys.argv[2], "rb").read()
    h = struct.unpack("<16I", model[:64])
    dim, heads, vocab, window = h[2], h[4], h[6], h[7]
    hs = dim // heads
    rope_at = 64 + vocab * dim + 4 * vocab
    rope_len = 4 * window * hs
    rope = model[rope_at:rope_at + rope_len]
    with open(sys.argv[3], "wb") as f:
        f.write(model[:rope_at] + model[rope_at + rope_len:])
    with open(sys.argv[4], "w") as f:
        f.write("/* written by tools/pack.py from the model and the tokenizer */\n")
        f.write("static const float k_rope[%d] = {\n" % (rope_len // 4))
        vals = struct.unpack("<%df" % (rope_len // 4), rope)
        for i in range(0, len(vals), 4):
            f.write("    " + ", ".join(float(v).hex() + "f" if float(v) != 0 else "0.0f" for v in vals[i:i + 4]) + ",\n")
        f.write("};\n")
        f.write("static const unsigned char k_tok[%d] = {\n" % len(tok))
        for i in range(0, len(tok), 16):
            f.write("    " + ",".join("%d" % b for b in tok[i:i + 16]) + ",\n")
        f.write("};\n")


main()
