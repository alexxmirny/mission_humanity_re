#!/usr/bin/env python3
"""Generate savetest's LZW fixture header from REAL blocks of a real save (RI-SAVE / SV1 batch B).

    python tools/gen_lzw_fixtures.py [--sav tools/uiscripts/saves/11.sav] [--blocks 20,7,56,5,86,85]
    python tools/lint_repo.py --fix        # ALWAYS follow with this: the emitted header is inside
                                           # src/mh_dll, so clang-format owns its final layout and a
                                           # bare regen leaves the tree lint-red on whitespace alone.

Why a generator and not hand-written arrays: the fixtures have to be bytes the ORIGINAL game wrote,
and their expected plaintext has to come from something other than the code under test. So each
fixture carries

  * the block exactly as it sits in the file (8-byte header + payload), and
  * the length and FNV-1a-64 of its decoded plaintext, computed by src/formats/decompress.py --

which is an INDEPENDENT implementation of the same decoder, written 2026-07-05 from the resource
format, long before RI-SAVE existed. The reimpl-loop skill's anti-pattern list calls out a fixture
and the code under test sharing one author; this is the guard against it.

savetest then proves two different things with one fixture: our decoder reproduces the oracle's
plaintext (hash), and our encoder turns that plaintext back into the original's exact bytes.

Block selection is by CODEC FEATURE, not by size -- the default set is the cheapest cover of 9/10/11/
12-bit code widths, a mid-stream dictionary reset, a long-run stream and the expansion case. Rerun
with --report to see the per-block feature table before choosing a different set.

THE SOURCE SAVE IS THE COMMITTED ONE (fork F5C, 2026-09-14). The default used to be
machine.DEFAULT_SAV -- a path into a machine-local game install -- which made both the generator and
the provenance of its output unreproducible off this box: a clean clone could not re-derive the
header, and the header's bytes traced to a file nobody else had. It is now
tools/uiscripts/saves/11.sav, the save the tactical journals already run from, committed and ruled
publishable by the user 2026-09-12 (player-made save, not game-shipped data). That is what lets the
generated header publish with the rest of the tree. The two files were byte-identical when the
repoint was made (md5 3101b024f369d266a15c5deee0e12978), so the regenerated header is unchanged --
this moved the PROVENANCE, not the fixtures.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "src", "formats"))

import tqdm as _tqdm  # noqa: E402


class _SilentBar:
    def __init__(self, *a, **k):
        pass

    def update(self, *a):
        pass

    def close(self):
        pass


_tqdm.tqdm = _SilentBar

import decompress as oracle  # noqa: E402

MAGIC = b"LZW "
# Committed and publishable (see the docstring) -- NOT machine.DEFAULT_SAV. Keep it a repo-relative
# path so a clean clone regenerates this header with no machine configuration at all.
DEFAULT_SAV = os.path.join(REPO, "tools", "uiscripts", "saves", "11.sav")
DEFAULT_BLOCKS = "20,7,56,5,86,85"
OUT = os.path.join(REPO, "src", "mh_dll", "libmh_test", "sv1_lzw_fixtures.gen.h")
VERSION_HEADER_BYTES = 40


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class _Instrumented(oracle.Decoder):
    """The independent decoder, counting the features a fixture is chosen for."""

    def __init__(self):
        super().__init__()
        # Two resets are structural, not features of the data: full_reset() calls reset(), and every
        # stream Compress produces opens with a 0x100 that calls it again. Only what is left is a
        # MID-STREAM reset, i.e. the encoder having filled its 3839-entry dictionary.
        self.resets = -2
        self.max_bits = 9

    def reset(self):
        self.resets += 1
        super().reset()

    def fetch_word(self, inp):
        self.max_bits = max(self.max_bits, self.lzw_bit_count)
        return super().fetch_word(inp)


def walk(data: bytes):
    """Yield (index, offset, block_bytes, uncompressed_size) for every LZW block in a save."""
    off, n = VERSION_HEADER_BYTES, 0
    while off + 20 <= len(data):
        clen, usize = struct.unpack_from("<II", data, off)
        if not (12 <= clen and off + 8 + clen <= len(data) and data[off + 8 : off + 12] == MAGIC):
            off += 4  # the one plain u32 written between the game and per-planet sections
            continue
        yield n, off, data[off : off + 8 + clen], usize
        off += 8 + clen
        n += 1


def decode(block: bytes) -> tuple[bytes, int, int]:
    """Decode a block's payload with the independent oracle. Returns (plaintext, bits, resets)."""
    d = oracle.Decompressor()
    d.decoder = _Instrumented()
    plaintext = d.decompress(block[8:])
    return plaintext, d.decoder.max_bits, max(0, d.decoder.resets)


def c_array(name: str, data: bytes) -> str:
    lines = [f"inline constexpr unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + "".join(f"0x{b:02x}, " for b in data[i : i + 16]).rstrip())
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sav", default=DEFAULT_SAV)
    ap.add_argument("--blocks", default=DEFAULT_BLOCKS)
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--report", action="store_true", help="print every block's features and exit")
    args = ap.parse_args()

    data = open(args.sav, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    blocks = {n: (off, blk, usize) for n, off, blk, usize in walk(data)}
    print(f"{args.sav}\n  {len(data)} bytes, md5 {md5}, {len(blocks)} LZW blocks")

    if args.report:
        print(f"  {'blk':>4} {'clen':>8} {'usize':>9} {'bits':>5} {'resets':>7}")
        for n, (off, blk, usize) in blocks.items():
            plain, bits, resets = decode(blk)
            ok = "" if len(plain) == usize else "  *** SIZE MISMATCH"
            print(f"  {n:4} {len(blk) - 8:8} {usize:9} {bits:5} {resets:7}{ok}")
        return 0

    wanted = [int(x) for x in args.blocks.split(",") if x.strip()]
    rows = []
    for n in wanted:
        if n not in blocks:
            print(f"  ERROR: block {n} not present (file has {len(blocks)})")
            return 1
        off, blk, usize = blocks[n]
        plain, bits, resets = decode(blk)
        if len(plain) != usize:
            print(f"  ERROR: block {n} decoded to {len(plain)}, header says {usize}")
            return 1
        rows.append(
            dict(n=n, off=off, blk=blk, usize=usize, bits=bits, resets=resets, fnv=fnv1a64(plain))
        )
        print(
            f"  block {n:3}: {len(blk) - 8:7} -> {usize:8} bytes, {bits} bit codes, "
            f"{resets} mid-stream reset(s), fnv1a64 {fnv1a64(plain):#018x}"
        )

    widths = sorted({r["bits"] for r in rows})
    print(
        f"  coverage: widths {widths}, resetting blocks "
        f"{[r['n'] for r in rows if r['resets']]}, "
        f"expanding {[r['n'] for r in rows if len(r['blk']) - 8 > r['usize']]}"
    )

    out = [
        "//",
        "// sv1_lzw_fixtures.gen.h -- GENERATED by tools/gen_lzw_fixtures.py. Do not hand-edit.",
        "//",
        "// Real LZW blocks lifted out of a real save, for savetest's byte-identity assertions",
        "// (RI-SAVE / SV1 batch B). Each block's expected plaintext length and hash come from",
        "// src/formats/decompress.py -- an INDEPENDENT decoder, so agreeing with it is evidence and",
        "// not a tautology. See the generator's docstring.",
        "//",
        f"// source: {os.path.basename(args.sav)}  md5 {md5}",
        f"// blocks: {', '.join(str(r['n']) for r in rows)}"
        f"   (code widths {widths}; mid-stream resets in "
        f"{[r['n'] for r in rows if r['resets']] or 'none'})",
        "//",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace sv1_fixtures {",
        "",
        "struct block_fixture {",
        "    const char          *name;",
        "    const unsigned char *block;             // the 8-byte header followed by the payload",
        "    uint32_t             block_bytes;",
        "    uint32_t             uncompressed_size; // what the block header claims, and the truth",
        "    uint64_t             plaintext_fnv1a64; // from the independent oracle",
        "    uint32_t             max_code_bits;     // widest code width the stream reaches",
        "    uint32_t             dict_resets;       // mid-stream dictionary resets",
        "};",
        "",
    ]
    for r in rows:
        out.append(f"// block {r['n']} @ file offset {r['off']:#08x}")
        out.append(c_array(f"BLOCK_{r['n']}", r["blk"]))
        out.append("")
    out.append("inline constexpr block_fixture ALL[] = {")
    for r in rows:
        out.append(
            f'    {{"block {r["n"]}", BLOCK_{r["n"]}, {len(r["blk"])}u, {r["usize"]}u, '
            f'{r["fnv"]:#018x}ull, {r["bits"]}u, {r["resets"]}u}},'
        )
    out.append("};")
    out.append("inline constexpr int COUNT = sizeof(ALL) / sizeof(ALL[0]);")
    out.append("")
    out.append("} // namespace sv1_fixtures")
    out.append("")

    with open(args.out, "w", newline="\n") as fh:
        fh.write("\n".join(out))
    print(f"  wrote {args.out} ({sum(len(r['blk']) for r in rows)} fixture bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
