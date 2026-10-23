#!/usr/bin/env python3
"""Reads and writes Frontier: Elite II save files (L73038_MakeSaveData / L730d0_ReadSaveData in fe2.s).

File = word $0011, encrypted body, long check value (the running key at the end).
Body (decrypted) = big_space image ($80ee bytes, zero-run coded), then the A6 globals
($20c bytes raw, then $3662 bytes zero-run coded). Zero-run coding: a zero byte is
followed by a count of further zeros.
"""
import struct, sys

KEY0 = 0x12350FD4
BIG_SIZE = 0x80EE
A6_RAW = 0x20C
A6_RLE = 0x3662
A6_SIZE = A6_RAW + A6_RLE
M32 = 0xFFFFFFFF


def rol1(v):
    return ((v << 1) | (v >> 31)) & M32


def crypt(body, decrypt):
    out = bytearray(body)
    d0 = KEY0
    for i in range(0, len(out) // 2 * 2, 2):
        w = (out[i] << 8) | out[i + 1]
        if decrypt:
            w ^= d0 & 0xFFFF
            plain = w
        else:
            plain = w
            w ^= d0 & 0xFFFF
        out[i], out[i + 1] = w >> 8, w & 0xFF
        sx = plain - 0x10000 if plain & 0x8000 else plain
        d0 = rol1((d0 + sx) & M32)
    return bytes(out), d0


def rle_decode(buf, pos, size):
    out = bytearray()
    while len(out) < size:
        b = buf[pos]; pos += 1
        out.append(b)
        if b == 0:
            n = buf[pos]; pos += 1
            out.extend(b"\0" * n)
    if len(out) != size:
        raise ValueError("zero run overruns the block")
    return bytes(out), pos


def rle_encode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] != 0:
            out.append(data[i]); i += 1
            continue
        n = 1
        while i + n < len(data) and data[i + n] == 0 and n < 256:
            n += 1
        out += bytes([0, n - 1]); i += n
    return bytes(out)


class Save:
    def __init__(self, big, a6):
        assert len(big) == BIG_SIZE and len(a6) == A6_SIZE
        self.big = bytearray(big)
        self.a6 = bytearray(a6)

    @classmethod
    def load(cls, path):
        raw = open(path, "rb").read()
        if struct.unpack(">H", raw[:2])[0] != 0x11:
            raise ValueError("not a save file")
        body, check = crypt(raw[2:-4], True)
        if struct.unpack(">I", raw[-4:])[0] != check:
            raise ValueError("check value mismatch")
        big, pos = rle_decode(body, 0, BIG_SIZE)
        a6 = body[pos:pos + A6_RAW]
        pos += A6_RAW
        rest, pos = rle_decode(body, pos + 0, A6_RLE)
        s = cls(big, a6 + rest)
        s.tail = body[pos:]
        return s

    def dump(self):
        body = rle_encode(bytes(self.big)) + bytes(self.a6[:A6_RAW]) + rle_encode(bytes(self.a6[A6_RAW:]))
        if len(body) & 1:
            body += b"\0"
        enc, check = crypt(body, False)
        return struct.pack(">H", 0x11) + enc + struct.pack(">I", check)

    def save(self, path):
        open(path, "wb").write(self.dump())


if __name__ == "__main__":
    for p in sys.argv[1:]:
        s = Save.load(p)
        orig = open(p, "rb").read()
        print(p, len(orig), "roundtrip identical:", s.dump() == orig, "tail", len(s.tail))
