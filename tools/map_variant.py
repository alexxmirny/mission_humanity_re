#!/usr/bin/env python3
"""map_variant.py -- mp:T5 (forced-snapshot lane) / mp:X2a (own-Maps lane shape).

Makes ONE map file's bytes differ from the shared install's copy while keeping the SAME name and
SAME length, so a lane built from it forces the host to re-send the map every run instead of
finding a peer that "already holds it" ~2/3 of the time (mp:T5's D30 evidence: only 3 of 10 250 ms
runs carried the bulk snapshot).

WHAT THE GAME ACTUALLY COMPARES (mp:X2, read from source -- not guessed):
    src/mh_net_proto/src/session_info.cpp  map_hash_from_sha256()
        memcpy(out, sha256(file_bytes), 8)   -- the first 8 bytes of a WHOLE-FILE SHA-256.
    src/mh_dll/mh/seams/map_transfer.cpp    file_hash() / resolve() / host_on_join()
        hashes the file, and a peer "holds the map" iff map_hash_equal(mine, host's) -- content
        only. The MATCH is by content; the NAME is only how the two sides agree on WHICH file to
        hash (case-insensitive basename), and the file's SIZE travels in the advert for display
        (`si.map_size`) but is never itself compared. So flipping a single content bit is both
        necessary and sufficient to make this file a "different map" by the check that matters,
        and touching only the last byte is sufficient to do that without changing the name or the
        length either.

WHERE THE FLIP LANDS, AND WHY. This project has no RE'd .mpm internal-format doc (only the
unrelated tactical .MAP format is documented), so "this byte is not structurally
checked" cannot be proven field-by-field here. Flipping the LAST byte is the smallest-risk choice
available without that RE, on two grounds: (1) every binary asset format from this era keeps its
magic/version/length fields at the FRONT, so the far end is the least likely place to be one of
them; (2) the flip does not change the file's LENGTH, so nothing that reads an offset relative to
EOF or to the declared size is disturbed -- only whatever that one trailing byte itself encodes.
This is NOT a claim that the mutated file would render correctly if the game actually opened it.
In the T5/X2a lane it never is: the client's own mismatched copy is redirected away the instant its
JOIN reports a hash the host does not recognise (map_transfer.cpp client_resolve_now, the X2
redirect), before anything tries to load it for real. The file's only job is to make
host_on_join's `map_hash_equal(p.had, g_host_hash)` read false on every run.

Importable (`variant_bytes`, `content_hash8`) by tools/make_lane.py's --map-variant option and by
this file's own --check self-test; run standalone as:

    python tools/map_variant.py --check "F:\\path\\to\\Maps\\blue monday.mpm"
"""

import argparse
import hashlib
import sys


def variant_bytes(orig: bytes) -> bytes:
    """The SAME length, ONE bit different: XOR the low bit of the last byte.

    Refuses an empty file (nothing to flip, and an empty "map" is not a map to begin with) rather
    than silently returning it unchanged, which would produce a same-name/same-content pair that
    defeats the whole point of this file.
    """
    if not orig:
        raise ValueError("empty map file -- nothing to flip")
    b = bytearray(orig)
    b[-1] ^= 0x01
    return bytes(b)


def content_hash8(data: bytes) -> str:
    """16 lowercase hex chars = the first 8 bytes of SHA-256(data), matching
    mh_net_proto::map_hash_from_sha256 byte for byte (see the module docstring)."""
    return hashlib.sha256(data).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("map_file", help="a real .mpm (or any) file to compute the variant of")
    ap.add_argument(
        "--check",
        action="store_true",
        help="OFFLINE ARM: read map_file, compute the variant, and assert the two content hashes "
        "(the same 8-byte truncated SHA-256 the game compares) differ while size and name are "
        "unchanged. Exits non-zero and prints a mismatch if they do not -- the mutation red for "
        "this tool is `variant_bytes` returning its input unchanged (see map_variant_selftest).",
    )
    ap.add_argument(
        "--write", metavar="DST", help="write the variant's bytes to DST (else read-only)"
    )
    args = ap.parse_args()

    with open(args.map_file, "rb") as f:
        orig = f.read()
    variant = variant_bytes(orig)
    h_orig, h_var = content_hash8(orig), content_hash8(variant)

    if args.check:
        ok = h_orig != h_var and len(orig) == len(variant)
        print(
            "%s: base hash=%s variant hash=%s, %d B -> %d B"
            % ("PASS" if ok else "FAIL", h_orig, h_var, len(orig), len(variant))
        )
        if not ok:
            sys.exit(1)
    else:
        print("base    hash=%s (%d B)" % (h_orig, len(orig)))
        print("variant hash=%s (%d B)" % (h_var, len(variant)))

    if args.write:
        with open(args.write, "wb") as f:
            f.write(variant)
        print("wrote variant -> %s" % args.write)
    return 0


if __name__ == "__main__":
    sys.exit(main())
