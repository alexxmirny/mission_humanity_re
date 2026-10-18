#!/usr/bin/env python3
"""make_cfgmod_sav.py -- build tools/uiscripts/saves/cfgmod.sav, the mods:G17 fixture.

    python tools/make_cfgmod_sav.py            # write the fixture from 2-almost.sav
    python tools/make_cfgmod_sav.py --check    # fail if the committed fixture differs from a rebuild

A savegame embeds the seven baked cfg tables (Unit, Building, Weapon, System, Planets, Projects,
Upgrades). The fixture is 2-almost.sav with ONE byte changed in each of them (a field of the
last record that no code reads in a quiet menu-to-game walk), so "the live table equals the
save's, not the boot parse" is observable per table. It is exactly what a save made under a modded
INIT.CFG holds.

The blocks are re-encoded with a LITERAL-ONLY LZW stream (every byte its own code, a CLEAR every
3000 codes): the game's decoder accepts any stream with its code-width schedule (9 bits, +1 at
dict sizes 0xfe / 0x2fe / 0x6fe), and a real compressor is not needed for a one-off. The result is
verified by decoding with src/formats/decompress.py. Everything else in the file is copied verbatim.
"""

import argparse
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "src", "formats"))

SRC = os.path.join(REPO, "tools", "uiscripts", "saves", "2-almost.sav")
OUT = os.path.join(REPO, "tools", "uiscripts", "saves", "cfgmod.sav")

# uncompressed size -> (name, byte offset to flip): the last byte of the last record.
TABLES = {
    57500: ("Unit", 99 * 0x23F + 0x23E),
    211400: ("Building", 99 * 0x842 + 0x841),
    11648: ("Weapon", 31 * 0x16C + 0x16B),
    560: ("System", 3 * 0x8C + 0x8B),
    34016: ("Planets", 31 * 0x427 - 1),
    20800: ("Projects", 99 * 0xD0 + 0xCF),
    10400: ("Upgrades", 99 * 0x68 + 0x48),  # energy of the last record
}
FLIP = 0x5A
CLEAR, EOF_CODE = 0x100, 0x101


def lzw_literal_encode(raw: bytes) -> bytes:
    bits = 0
    nbits = 0
    out = bytearray()

    def put(code, width):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += width
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    width = 9
    dict_size = 0
    n_since_clear = 0
    put(CLEAR, 9)
    for b in raw:
        if n_since_clear == 3000:
            put(CLEAR, width)
            width, dict_size, n_since_clear = 9, 0, 0
        put(b, width)
        if n_since_clear > 0:  # the decoder updates its dictionary on every code after the first
            dict_size += 1
            if dict_size in (0xFE, 0x2FE, 0x6FE):
                width += 1
        n_since_clear += 1
    put(EOF_CODE, width)
    if nbits:
        out.append(bits & 0xFF)
    return bytes(out) + b"\0\0"  # the reader peeks 3 bytes


def lzw_block(raw: bytes) -> bytes:
    body = lzw_literal_encode(raw)
    blob_len = 12 + len(body)
    blob = b"LZW " + struct.pack("<II", len(raw), blob_len) + body
    return struct.pack("<II", len(blob), len(raw)) + blob


def build() -> bytes:
    sys.setrecursionlimit(10000)
    import decompress  # noqa: E402

    data = open(SRC, "rb").read()
    out = bytearray(data[:0x28])
    pos = 0x28
    done = set()
    while pos < len(data) and len(done) < len(TABLES):
        blen, usize = struct.unpack_from("<II", data, pos)
        blob = data[pos + 8 : pos + 8 + blen]
        if usize in TABLES and usize not in done and blob[:4] == b"LZW ":
            name, off = TABLES[usize]
            raw = bytearray(decompress.Decompressor().decompress(bytes(blob)))
            assert len(raw) == usize
            raw[off] ^= FLIP
            new = lzw_block(bytes(raw))
            back = decompress.Decompressor().decompress(new[8:])
            assert back == bytes(raw), name
            out += new
            done.add(usize)
        else:
            out += data[pos : pos + 8 + blen]
        pos += 8 + blen
    assert len(done) == len(TABLES), "missing tables: %s" % sorted(set(TABLES) - done)
    out += data[pos:]
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    new = build()
    if a.check:
        ok = os.path.exists(OUT) and open(OUT, "rb").read() == new
        print("make_cfgmod_sav: %s" % ("ok" if ok else "STALE"))
        return 0 if ok else 1
    open(OUT, "wb").write(new)
    print("wrote %s (%d bytes)" % (OUT, len(new)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
