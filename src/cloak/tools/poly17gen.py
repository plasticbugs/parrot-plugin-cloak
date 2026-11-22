#!/usr/bin/env python3
"""poly17gen.py > cl_poly17.h -- the POKEY's 17-bit polynomial as cl.c's
bit table (MAME 0.288's pokey_device::poly_init_9_17, bit 0 of each of its
131,071 states, then the first 32 again), as a constant: on the cart it
stays in flash (an XIP plugin's SRAM is short), and cl.c checks it against
the polynomial itself in the tests' builds (no NDEBUG)."""
P17N = 131071
bits = bytearray((P17N + 32) // 8 + 2)
lfsr = 0x1ffff
seq = []
for i in range(P17N):
    in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1)
    inb = lfsr & 1
    lfsr >>= 1
    lfsr = (lfsr & 0xff7f) | (in8 << 7)
    lfsr = (inb << 16) | lfsr
    seq.append(lfsr & 1)
for i in range(P17N + 32):
    if seq[i % P17N]:
        bits[i >> 3] |= 1 << (i & 7)
print("// cl_poly17.h -- made by tools/poly17gen.py: the POKEY's 17-bit polynomial,")
print("// bit 0 of each state (cl.c's poly17_bit). Not game data: a function of the")
print("// polynomial alone.")
print("static const uint8_t poly17_bit[%d] = {" % len(bits))
for k in range(0, len(bits), 24):
    print("\t" + ",".join("0x%02x" % b for b in bits[k:k + 24]) + ",")
print("};")
