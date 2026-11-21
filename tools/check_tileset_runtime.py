#!/usr/bin/env python3
"""Check the actual Planets[31] profile in the TILESET1 save UI walk.

Usage: check_tileset_runtime.py --stage create|missing|load PROCESS_OR_SESSION_DIR
The run directory fixes the lane and the process-owned logs; no newest-log lookup.
"""

import argparse
import contextlib
import io
import re
import struct
import sys
from pathlib import Path

from _rundir import process_and_sessions

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "src" / "formats"))
import decompress

BLOCK_SIZES = (57500, 211400, 11648, 560, 34016)
PLANET_STRIDE = 0x427
PLANET_SLOT = 31


def planet_profile(path):
    data = path.read_bytes()
    offset = 40  # container version header
    planet_blob = None
    for index, expected_size in enumerate(BLOCK_SIZES):
        if offset + 8 > len(data):
            raise ValueError(f"{path}: truncated block {index} header")
        compressed_size, size = struct.unpack_from("<II", data, offset)
        start = offset + 8
        end = start + compressed_size
        if size != expected_size or compressed_size < 12 or end > len(data):
            raise ValueError(
                f"{path}: invalid block {index}: compressed={compressed_size}, size={size}"
            )
        blob = data[start:end]
        magic, inner_size, inner_compressed = struct.unpack_from("<4sII", blob)
        if (magic, inner_size, inner_compressed) != (b"LZW ", size, compressed_size):
            raise ValueError(f"{path}: invalid inner header in block {index}")
        if index == len(BLOCK_SIZES) - 1:
            # The independent resource decoder emits a tqdm progress bar by default.
            with contextlib.redirect_stderr(io.StringIO()):
                planet_blob = decompress.Decompressor().decompress(blob)
        offset = end

    if planet_blob is None or len(planet_blob) != BLOCK_SIZES[-1]:
        raise ValueError(f"{path}: Planets block did not decode to its declared size")
    row = planet_blob[PLANET_SLOT * PLANET_STRIDE : (PLANET_SLOT + 1) * PLANET_STRIDE]
    token = row[0x20E : 0x20E + 13]
    if token[-1] != 0 or token[:-1].lower() != b"marsh_01.tlo":
        raise ValueError(f"{path}: wrong slot 31 terrain token {token!r}")
    index, soldier = row[0x425], row[0x426]
    banks = list(row[0x38D + 50 : 0x38D + 57])
    if index != 0 or soldier != 128 or banks != [0, 1, 0, 0, 0, 0, 0]:
        raise ValueError(
            f"{path}: wrong slot 31 profile index={index} soldier={soldier} banks50..56={banks}"
        )
    return token.lower(), index, soldier, banks


def lines(dirs, filename):
    result = []
    for directory in dirs:
        path = directory / filename
        if path.is_file():
            result.extend(path.read_text(encoding="utf-8", errors="replace").splitlines())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=("create", "missing", "load"), required=True)
    parser.add_argument("run", help="process or session directory belonging to this UI run")
    args = parser.parse_args()
    try:
        dirs = [Path(d) for d in process_and_sessions(args.run)]
        lane = dirs[0].parent.parent
        ui = lines(dirs, "mh_uidrive.log")
        net = lines(dirs, "mh_net.log")
        first = planet_profile(lane / "save" / "tilesetproof.sav")
        if args.stage == "missing":
            refusal = re.compile(r"\[map\] save refused [^\r\n]*tilesetproof\.sav:", re.IGNORECASE)
            if not any(refusal.search(line) for line in net):
                raise ValueError("this process did not refuse the tilesetproof save")
        else:
            saved = "tilesetproof" if args.stage == "create" else "tilesetreload"
            if not any(f"[script] savegame {saved} -> 1" in line for line in ui):
                raise ValueError(f"this process did not successfully save {saved}")
        if args.stage == "load":
            boundary = re.compile(
                r"\[desync\] BOUNDARY (?:noted: )?save_load \(tilesetproof\)", re.IGNORECASE
            )
            if not any(boundary.search(line) for line in net):
                raise ValueError("this process did not record a tilesetproof menu-load boundary")
            second = planet_profile(lane / "save" / "tilesetreload.sav")
            if first != second:
                raise ValueError("original save and resave profiles differ")
        print(f"PASS: {args.stage} {lane}: MARSH_01.TLO, index=0, soldier=128, bank=51")
        return 0
    except (OSError, ValueError, struct.error, AssertionError, IndexError, RecursionError) as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
