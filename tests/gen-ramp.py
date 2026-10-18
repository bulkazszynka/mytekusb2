#!/usr/bin/env python3
"""Write a stereo ramp test signal, n = 1, 2, ...
PCM (S32_LE/S24_LE): left = n, right = ~n (24 bit).
DSD (DSD_U8, DSD_U16_LE/BE, DSD_U32_LE/BE): the DSD byte stream of each
channel (in time order) is a sequence of 16 bit big endian words, left = n,
right = ~n; it is cut into samples of 1/2/4 bytes, the _LE formats store
each sample's bytes reversed. The driver packs 16 DSD bits per channel into
each DoP frame (earlier byte in the middle byte), so the emulator sees the
same ramp in every DSD format.
usage: gen-ramp.py FORMAT rate seconds outfile"""
import struct, sys
fmt, rate, secs, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
n_frames = int(rate * secs)
buf = bytearray()
if fmt.startswith("DSD"):
    size = {"DSD_U8": 1, "DSD_U16_LE": 2, "DSD_U16_BE": 2,
            "DSD_U32_LE": 4, "DSD_U32_BE": 4}[fmt]
    n_bytes = n_frames * size
    words = range(1, n_bytes // 2 + 2)
    left = b"".join(struct.pack(">H", n & 0xffff) for n in words)[:n_bytes]
    right = bytes(~b & 0xff for b in left)
    for i in range(0, n_bytes, size):
        for ch in (left, right):
            s = ch[i:i + size]
            buf += s[::-1] if fmt.endswith("_LE") else s
else:
    shift = 8 if fmt == "S32_LE" else 0
    for n in range(1, n_frames + 1):
        l = n & 0xffffff
        r = ~n & 0xffffff
        buf += struct.pack("<II", (l << shift) & 0xffffffff, (r << shift) & 0xffffffff)
open(out, "wb").write(buf)
