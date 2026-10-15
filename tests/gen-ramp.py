#!/usr/bin/env python3
"""Write a stereo ramp test signal: left = n, right = ~n (24 bit), n = 1, 2, ...
usage: gen-ramp.py S32_LE|S24_LE rate seconds outfile"""
import struct, sys
fmt, rate, secs, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
shift = 8 if fmt == "S32_LE" else 0
n_frames = int(rate * secs)
buf = bytearray()
for n in range(1, n_frames + 1):
    l = n & 0xffffff
    r = ~n & 0xffffff
    buf += struct.pack("<II", (l << shift) & 0xffffffff, (r << shift) & 0xffffffff)
open(out, "wb").write(buf)
